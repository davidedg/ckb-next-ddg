#include "ckbsettings.h"
#include "ckbsettingswriter.h"
#include <QThread>
#include <QDebug>
#include <QDateTime>
#include <ckbnextconfig.h>
#include <QCoreApplication>
#include <QFileInfo>
#include <QUuid>

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
#include <QRecursiveMutex>
#else
#include <QMutex>
#endif

// Shared global QSettings object
static QSettings* _globalSettings = nullptr;
static QThread* globalThread = nullptr;
QAtomicInt cacheWritesInProgress(0);

// Global one-shot settings cache, to avoid reading/writing QSettings constantly
static QMap<QString, QVariant> globalCache;

// Mutexes for accessing settings
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
QRecursiveMutex settingsMutex, settingsCacheMutex;
#else
QMutex settingsMutex(QMutex::Recursive), settingsCacheMutex(QMutex::Recursive);
#endif

#define lockMutex           QMutexLocker locker(backing == _globalSettings ? &settingsMutex : nullptr)
#define lockMutexStatic     QMutexLocker locker(&settingsMutex)
#define lockMutexStatic2    QMutexLocker locker2(&settingsMutex)
#define lockMutexCache      QMutexLocker locker(&settingsCacheMutex)

#if QT_VERSION < QT_VERSION_CHECK(5, 5, 0)
#define qInfo       qDebug
#define qWarning    qDebug
#define qFatal      qDebug
#define qCritical   qDebug
#endif

static bool readableBackup(const QString& name){
    QSettings backup(CkbSettings::Format, QSettings::UserScope, QCoreApplication::organizationName(), name);
    backup.setFallbacksEnabled(false);
    const QFileInfo file(backup.fileName());
    if(!file.isFile() || !file.isReadable() || file.size() == 0)
        return false;
    backup.sync();
    return backup.status() == QSettings::NoError && backup.childGroups().contains("Devices");
}

static bool backupMatches(QSettings& from, const QString& name){
    if(!readableBackup(name)) return false;
    QSettings backup(CkbSettings::Format, QSettings::UserScope, QCoreApplication::organizationName(), name);
    backup.setFallbacksEnabled(false);
    const QStringList keys = from.allKeys();
    if(backup.allKeys() != keys){
        qWarning() << "K95P backup verification: the key lists differ";
        return false;
    }
    for(const QString& key : keys){
        const QVariant expected = from.value(key), actual = backup.value(key);
        // QSettings reads an empty/single-item QStringList as an invalid
        // QVariant/a QString. Compare the list as callers read it back.
        if(expected.userType() == QMetaType::QStringList &&
           (!actual.isValid() || actual.userType() == QMetaType::QString || actual.userType() == QMetaType::QStringList) &&
           actual.toStringList() == expected.toStringList())
            continue;
        if(actual != expected){
            qWarning() << "K95P backup verification: different stored value at" << key << expected.typeName() << actual.typeName();
            return false;
        }
    }
    return true;
}

static bool backupAll(QSettings& from, const QString& backupName){
    QSettings backupSettings(CkbSettings::Format, QSettings::UserScope, QCoreApplication::organizationName(), backupName);
    backupSettings.setFallbacksEnabled(false);
    qInfo() << "Backing up settings to" << backupSettings.fileName();
    const QStringList oldKeys = from.allKeys();
    for(const QString& key : oldKeys)
        backupSettings.setValue(key, from.value(key));
    backupSettings.sync();
    if(backupSettings.status() != QSettings::NoError){
        qWarning() << "Settings backup failed:" << backupSettings.fileName() << backupSettings.status();
        return false;
    }
    qInfo() << oldKeys.count() << "keys backed up.";
    return true;
}

QString CkbSettings::k95pHwBackupOnce(QSettings& settings, HwBackupStatus* status){
    static const QString key("Program/K95PHwBackup");
    // A verified copy survives a marker write error and can be reused on retry.
    // Restarting without its marker may make another unique copy, never overwrite it.
    static QMap<QString, QString> pending;
    if(status) *status = BackupFailed;
    settings.sync();
    QSettings persisted(settings.fileName(), settings.format());
    persisted.setFallbacksEnabled(false);
    persisted.sync();
    if(persisted.status() != QSettings::NoError){
        qWarning() << "K95P migration: settings could not be synchronized:" << settings.fileName();
        return QString();
    }
    const bool devices = settings.childGroups().contains("Devices");
    const QString previous = settings.value(key).toString();
    if((previous == "none" && !devices) || (!previous.isEmpty() && previous != "none" && readableBackup(previous))){
        if(status) *status = devices ? BackupValid : BackupNotNeeded;
        return QString();
    }
    QString name("none");
    if(devices){
        name = pending.value(settings.fileName());
        if(name.isEmpty() || !backupMatches(settings, name)){
            // UUID plus milliseconds also separates multiple devices/retries in one second.
            do {
                name = QString("ckb-next_backup_k95p_hw_%1_%2").arg(QDateTime::currentMSecsSinceEpoch())
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
                QSettings candidate(Format, QSettings::UserScope, QCoreApplication::organizationName(), name);
                if(!QFileInfo::exists(candidate.fileName())) break;
            } while(true);
            if(!backupAll(settings, name) || !backupMatches(settings, name)){
                qWarning() << "K95P migration: the settings backup could not be verified";
                return QString();
            }
            pending[settings.fileName()] = name;
        }
    }
    const bool hadMarker = settings.contains(key);
    const QVariant oldMarker = settings.value(key);
    settings.setValue(key, name);
    settings.sync();
    QSettings check(settings.fileName(), settings.format());
    check.setFallbacksEnabled(false);
    check.sync();
    if(check.status() != QSettings::NoError || check.value(key).toString() != name){
        // Do not let this QSettings object's pending values masquerade as success.
        if(hadMarker) settings.setValue(key, oldMarker);
        else settings.remove(key);
        settings.sync();
        qWarning() << "K95P migration: the backup marker could not be persisted:" << settings.fileName();
        return QString();
    }
    pending.remove(settings.fileName());
    if(status) *status = devices ? BackupCreated : BackupNotNeeded;
    return name;
}

static QSettings* globalSettings(){
    if(!_globalSettings){
        lockMutexStatic;
        if(!(volatile QSettings*)_globalSettings){   // Check again after locking mutex in case another thread created the object
            _globalSettings = new QSettings;
            qInfo() << "Path to settings:" << _globalSettings->fileName();
            // Check if a config migration attempt needs to be made.
            // On mac, first check if we need to migrate from plist to ini
            // If we copy nothing from this, we can go and try to migrate straight from the old ckb namespace
            if(!_globalSettings->value("Program/CkbNextIniMigrationChecked", false).toBool()) {
#ifdef Q_OS_MAC
                CkbSettings::migrateSettings(true);
#endif
                // on other platforms, just mark it as true so that if someone migrates settings from linux to mac, it will not try to migrate
                _globalSettings->setValue("Program/CkbNextIniMigrationChecked", true);
            }
            if(!_globalSettings->value("Program/CkbMigrationChecked", false).toBool()){
                CkbSettings::migrateSettings(false);
                // Mark settings as migrated
                _globalSettings->setValue("Program/CkbMigrationChecked", true);
            }
            // If the current settings are older than the expected settings version, take a backup first
            const quint16 currentSettingsVersion = _globalSettings->value("Program/SettingsVersion", 0).toInt();
            if(currentSettingsVersion < CKB_NEXT_SETTINGS_VER){
                backupAll(*_globalSettings, QString("ckb-next_backup_%1").arg(QDateTime::currentMSecsSinceEpoch() / 1000));
                // Bump the profile version in the current config
                _globalSettings->setValue("Program/SettingsVersion", CKB_NEXT_SETTINGS_VER);
            }
            // Put the settings object in a separate thread so that it won't lock up the GUI when it syncs
            globalThread = new QThread;
            globalThread->start();
            _globalSettings->moveToThread(globalThread);
        }
    }
    return _globalSettings;
}

QString CkbSettings::k95pHwBackupOnce(HwBackupStatus* status){
    // What was saved last goes into the backup: the writes already queued first
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    while(cacheWritesInProgress.loadRelaxed() > 0)
#else
    while(cacheWritesInProgress.load() > 0)
#endif
        QThread::yieldCurrentThread();
    lockMutexStatic;
    return k95pHwBackupOnce(*globalSettings(), status);
}

bool CkbSettings::isBusy(){
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    return cacheWritesInProgress.loadRelaxed() > 0;
#else
    return cacheWritesInProgress.load() > 0;
#endif
}

void CkbSettings::migrateSettings(bool macFormat){
    QSettings* oldSettings;
    if(macFormat)
        oldSettings = new QSettings(QSettings::NativeFormat, QSettings::UserScope, QCoreApplication::organizationName(), QCoreApplication::applicationName());
    else
        oldSettings = new QSettings("ckb", "ckb");

    // On macOS and iOS, allKeys() will return some extra keys for global settings that apply to all applications.
    // These keys can be read using value() but cannot be changed, only shadowed. Calling setFallbacksEnabled(false) will hide these global settings.
    // https://doc.qt.io/qt-5/qsettings.html
    oldSettings->setFallbacksEnabled(false);
    qInfo() << "Looking for old settings in:" << oldSettings->fileName();
    // Check if a basic key exists
    if(oldSettings->contains("Program/KbdLayout")){
        qInfo() << "Found, proceeding to migrate.";
        QStringList oldKeys = oldSettings->allKeys();
        foreach (const QString& key, oldKeys){
            QVariant value = oldSettings->value(key);
            _globalSettings->setValue(key, value);
        }
        qInfo() << oldKeys.count() << "keys migrated.";
    }
    delete oldSettings;
}

void CkbSettings::cleanUp(){
    if(!_globalSettings)
        return;
    // Wait for all writers to finish
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    while(cacheWritesInProgress.loadRelaxed() > 0)
#else
    while(cacheWritesInProgress.load() > 0)
#endif
        QThread::yieldCurrentThread();
    // Stop thread and delete objects
    globalThread->quit();
    globalThread->wait();
    delete globalThread;
    delete _globalSettings;
    globalThread = nullptr;
    _globalSettings = nullptr;
}

CkbSettings::CkbSettings() :
    backing(globalSettings()) {
}

CkbSettings::CkbSettings(const QString& basePath, bool eraseExisting) :
    backing(globalSettings()) {
    if(basePath.isEmpty()){
        if(eraseExisting)
            qDebug() << "CkbSettings created with basePath = \"\" and eraseExisting = true. This is a mistake.";
        return;
    }
    if(eraseExisting)
        remove(basePath);
    beginGroup(basePath);
}

CkbSettings::CkbSettings(QSettings& settings) :
    backing(&settings) {
}

void CkbSettings::beginGroup(const QString& prefix){
    groups.append(prefix);
}

void CkbSettings::endGroup(){
    groups.removeLast();
}

QStringList CkbSettings::childGroups() const {
    QString current = pwd();
    lockMutex;
    if(!current.isEmpty())
        backing->beginGroup(current);
    QStringList res = backing->childGroups();
    if(!current.isEmpty())
        backing->endGroup();
    return res;
}

QStringList CkbSettings::childKeys() const {
    QString current = pwd();
    lockMutex;
    if(!current.isEmpty())
        backing->beginGroup(current);
    QStringList res = backing->childKeys();
    if(!current.isEmpty())
        backing->endGroup();
    return res;
}

bool CkbSettings::contains(const QString& key) const {
    lockMutex;
    return backing->contains(pwd(key));
}

bool CkbSettingsBase::containsGroup(const QString& group){
    QStringList components = group.split("/");
    if(components.length() > 1){
        // Find sub-group
        SGroup g(*this, components[0]);
        return containsGroup(QStringList(components.mid(1)).join('/'));
    }
    return childGroups().contains(group);
}

QVariant CkbSettings::value(const QString& key, const QVariant& defaultValue) const {
    lockMutex;
    return backing->value(pwd(key), defaultValue);
}

void CkbSettings::setValue(const QString& key, const QVariant& value){
    //qDebug() << "Setting " << key << "to value" << value;
    // Cache the write values, save them when the object is destroyed
    QString realKey = pwd(key);
    writeCache[realKey] = value;
    // Update global cache if needed
    lockMutexCache;
    if(globalCache.contains(realKey))
        globalCache[realKey] = value;
}

void CkbSettings::remove(const QString& key){
    removeCache.append(pwd(key));
}

CkbSettings::~CkbSettings(){
    if(removeCache.isEmpty() && writeCache.isEmpty())
        return;
    // Save the settings from the settings thread.
    // They have to be saved from that thread specifically to avoid performance issues
    CkbSettingsWriter* writer = new CkbSettingsWriter(backing, removeCache, writeCache);
    writer->moveToThread(globalThread);
    QObject::staticMetaObject.invokeMethod(writer, "run", Qt::QueuedConnection);
}

QVariant CkbSettings::get(const QString& key, const QVariant& defaultValue){
    // Store these settings in a memory cache
    lockMutexCache;
    if(globalCache.contains(key))
        return globalCache.value(key);
    // If it wasn't found in the memory cache, look for it in QSettings
    lockMutexStatic2;
    QSettings* settings = globalSettings();
    return globalCache[key] = settings->value(key, defaultValue);
}

void CkbSettings::set(const QString& key, const QVariant& value){
    {
        lockMutexCache;
        if(globalCache.value(key) == value)
            return;
        globalCache[key] = value;
    }
    lockMutexStatic;
    globalSettings()->setValue(key, value);
}
