#ifndef CKBSETTINGS_H
#define CKBSETTINGS_H

#include <QSettings>
#include <QStringList>
#include <QMap>
#include <QVariant>
#include <QtCore/QFile>
#include <QtCore/QDir>

class CkbSettingsBase
{
public:
    virtual void        beginGroup(const QString& prefix) = 0;
    virtual void        endGroup() = 0;
    virtual QStringList childGroups() const = 0;
    virtual QStringList childKeys() const = 0;
    virtual bool        contains(const QString& key) const = 0;
    virtual QVariant    value(const QString& key, const QVariant& defaultValue = QVariant()) const = 0;
    virtual void        setValue(const QString& key, const QVariant& value) = 0;
    virtual void        remove(const QString& key) = 0;
    bool                containsGroup(const QString& group);
};

// QSettings wrapper to use common functions for export/import
class CkbExternalSettings : public CkbSettingsBase, public QSettings
{
public:
    CkbExternalSettings(const QString& s, const QSettings::Format fmt, const quint16& ver): QSettings(s, fmt), _currentProfileVer(ver) {}
    void        beginGroup(const QString& prefix) { QSettings::beginGroup(prefix); }
    void        endGroup() {QSettings::endGroup(); }
    QStringList childGroups() const {return QSettings::childGroups(); }
    QStringList childKeys() const { return QSettings::childKeys(); }
    bool        contains(const QString& key) const { return QSettings::contains(key); }
    QVariant    value(const QString& key, const QVariant& defaultValue = QVariant()) const { return QSettings::value(key, defaultValue); }
    void        setValue(const QString& key, const QVariant& value) { QSettings::setValue(key, value); }
    void        remove(const QString& key) { QSettings::remove(key); }

    // Class-specific
    quint16 profileVer() const { return _currentProfileVer; }
private:
    quint16 _currentProfileVer;
};

// QSettings replacement with convenience functions
class CkbSettings : public CkbSettingsBase
{
public:
    // Basic settings object. No default group.
    CkbSettings();
    // Settings object with a default group. Optionally erases any existing group with the same name.
    CkbSettings(const QString& basePath, bool eraseExisting = false);

    ~CkbSettings();

    // On version 0.2.9 the organisation and application names were changed.
    // This function migrates existing settings to the new location.
    static void migrateSettings(bool macFormat);

    // The hardware profile of a K95 RGB Platinum (hwslot1) turns into exactly the three on-board slots and drops the modes it had
    // beyond them: before any of that, the whole settings are copied once to a backup of their own (without raising the
    // settings version, which would make an older GUI warn about a downgrade). Returns the name of the backup, "none" when there
    // were no device settings to keep, or "" when it had already been done (the name is kept in Program/K95PHwBackup).
    static QString k95pHwBackupOnce(QSettings& settings);
    // The same on the settings of the program, taken by the migration itself (Kb::migrateK95HwProfile()), so that nothing is
    // backed up for a device or a daemon that never migrates
    static QString k95pHwBackupOnce();

    // One-shot get/set
    static QVariant get(const QString& key, const QVariant& defaultValue = QVariant());
    static void     set(const QString& key, const QVariant& value);

    // Whether or not CkbSettings is busy writing data. If busy, the constructors will block until it is not.
    // The global set() will also block if busy, but global get() will not, unless the value has never been read before.
    static bool isBusy();

    // Finalize all writes, clean up and release resources
    static void cleanUp();

    // QSettings functions
    void        beginGroup(const QString& prefix);
    void        endGroup();
    QStringList childGroups() const;
    QStringList childKeys() const;
    bool        contains(const QString& key) const;
    QVariant    value(const QString& key, const QVariant& defaultValue = QVariant()) const;
    void        setValue(const QString& key, const QVariant& value);
    void        remove(const QString& key);

    // Use Native format on Linux. It is ini but the file extension is .conf
    // Everything else is going to be ini
    static const QSettings::Format Format =
#ifdef Q_OS_LINUX
            QSettings::NativeFormat;
#else
            QSettings::IniFormat;
#endif

protected:
    // CkbSettings from QSettings
    CkbSettings(QSettings& settings);

private:
    QSettings* backing;
    QStringList groups;
    QStringList removeCache;
    QMap<QString, QVariant> writeCache;

    inline QString pwd() const { return groups.join("/"); }
    inline QString pwd(const QString& key) const { return pwd() + (groups.isEmpty() ? "" : "/") + key; }
};

class CkbDemoSettings : public CkbSettings {

public:
    CkbDemoSettings(QSettings& settings) : CkbSettings(settings) {}
};

// Settings group wrapper. Useful for easily pushing/popping a group without worrying about everything breaking if you forgot endGroup.
class SGroup
{
public:
    inline SGroup(CkbSettingsBase& settings, const QString& prefix) : _settings(settings) { settings.beginGroup(prefix); }
    inline ~SGroup() { _settings.endGroup(); }

private:
    CkbSettingsBase& _settings;
};

#endif // CKBSETTINGS_H
