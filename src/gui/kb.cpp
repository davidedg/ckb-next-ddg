#include <fcntl.h>
#include <QSet>
#include <QUrl>
#include <QMutex>
#include <QDebug>
#include "kb.h"
#include "kbmanager.h"
#include "hwsavepipe.h"
#include "k95pledmap.h"
#include "ckbsettings.h"
#include "k95pstage.h"
#include "k95psnapshot.h"
#include "compat/qrand.h"
#include <set>

// The daemon checks command words before it decodes arguments. For the K95P,
// leaving even ASCII letters unescaped in a name can execute e.g. "hwsave".
static bool encodedModeName(KeyMap::Model model, const QString& name, QByteArray& bytes){
    if(model != KeyMap::K95P){
        bytes = QUrl::toPercentEncoding(name);
        return true;
    }
    QByteArray utf8 = name.toUtf8();
    if(QString::fromUtf8(utf8) != name)
        return false;
    std::string encoded;
    if(!HwSavePipe::encode_name(std::string(utf8.constData(), size_t(utf8.size())), encoded))
        return false;
    bytes = QByteArray(encoded.data(), int(encoded.size()));
    return true;
}

// All active devices
static QSet<Kb*> activeDevices;
// Active notification node paths
static QSet<QString> notifyPaths;
static QMutex notifyPathMutex;

int Kb::_frameRate = 30, Kb::_scrollSpeed = 0;
int Kb::k95DecisionTimeoutMs = 10000;
int Kb::k95ReadGuardMs = 30000;
bool Kb::_dither = false, Kb::_mouseAccel = true;

static inline Kb::pollrate_t stringToPollrate(const QString& str){
    if(str == QLatin1String("8 ms"))
        return Kb::POLLRATE_8MS;
    else if(str == QLatin1String("4 ms"))
        return Kb::POLLRATE_4MS;
    else if(str == QLatin1String("2 ms"))
        return Kb::POLLRATE_2MS;
    else if(str == QLatin1String("1 ms"))
        return Kb::POLLRATE_1MS;
    else if(str == QLatin1String("0.5 ms"))
        return Kb::POLLRATE_05MS;
    else if(str == QLatin1String("0.25 ms"))
        return Kb::POLLRATE_025MS;
    else if(str == QLatin1String("0.1 ms"))
        return Kb::POLLRATE_01MS;
    else
        return Kb::POLLRATE_UNKNOWN;
}

Kb::Kb(QObject *parent, const QString& path) :
    QThread(parent), features(QStringList()), pollrate(POLLRATE_UNKNOWN), maxpollrate(POLLRATE_UNKNOWN), monochrome(false), productID(0), hwload(false), adjrate(false), firmware(),
    batteryTimer(nullptr), batteryIcon(nullptr), showBatteryIndicator(false), devpath(path), cmdpath(path + "/cmd"), notifyPath(path + "/notify1"), macroPath(path + "/notify2"),
    _currentProfile(nullptr), _currentMode(nullptr), _model(KeyMap::NO_MODEL), batteryLevel(0), batteryStatus(BatteryStatus::BATT_STATUS_UNKNOWN),
    _hwProfile(nullptr), prevProfile(nullptr), prevMode(nullptr),
    cmd(cmdpath), notifyNumber(1), macroNumber(2), _needsSave(false), _layout(KeyMap::NO_LAYOUT), _maxDpi(0),
    deviceIdleTimer()
{
    memset(iState, 0, sizeof(iState));
    memset(hwLoading, 0, sizeof(hwLoading));

    // Get the features, model, serial number, FW version (if available), poll rate (if available), and layout from /dev nodes
    QFile ftpath(path + "/features"), mpath(path + "/model"), spath(path + "/serial"), fwpath(path + "/fwversion"), ppath(path + "/pollrate"), prodpath(path + "/productid"), hwlayoutPath(path + "/layout"), dpiPath(path + "/dpi");
    if (ftpath.open(QIODevice::ReadOnly)){
        QString featurestr = ftpath.read(1000).trimmed();
        ftpath.close();
        // Read model from features (first word: vendor, second word: product)
        features = featurestr.split(" ");
        if(features.length() < 2)
            return;
        _model = KeyMap::getModel(features[1]);
        if(_model == KeyMap::NO_MODEL) {
            qDebug() << "could not find valid model information:" << features[1] << "produced" << _model;
            return;
        }
    } else {
        // Bail if features aren't readable
        qDebug() << "Could not open" << ftpath.fileName();
        return;
    }
    if (features.contains("monochrome"))
        monochrome = true;
    if (features.contains("hwload"))
        hwload = true;
    if (mpath.open(QIODevice::ReadOnly)){
        usbModel = mpath.read(100);
#ifdef WITH_ENV_VARS
        usbProductRaw = usbModel.trimmed();
#endif
        usbModel = usbModel.remove("Corsair", Qt::CaseInsensitive).remove("Gaming").remove("Keyboard").remove("Mouse").remove("Bootloader").remove("Mechanical").replace("LOW PROFILE", "LP").trimmed();
        mpath.close();
    }
    if (usbModel == "")
        usbModel = "Keyboard";
    if (spath.open(QIODevice::ReadOnly)){
        usbSerial = spath.read(100);
        usbSerial = usbSerial.trimmed().toUpper();
        spath.close();
    }
    if (usbSerial == "")
        usbSerial = "Unknown-" + usbModel;
    if (fwpath.open(QIODevice::ReadOnly)) {
        firmware.parse(fwpath.read(100));
        fwpath.close();
        if (prodpath.open(QIODevice::ReadOnly)) {
            productID = prodpath.read(4).toUShort(nullptr, 16);
            // qInfo() << "ProductID of device is" << productID;
        } else {
            qCritical() << "could not open" << prodpath.fileName();
        }
    }
    if (ppath.open(QIODevice::ReadOnly)){
        QStringList sl = QString(ppath.read(100)).trimmed().split(QLatin1Char('\n'));
        ppath.close();
        if(sl.length() > 0)
            pollrate = stringToPollrate(sl.at(0));
        if(sl.length() > 1)
            maxpollrate = stringToPollrate(sl.at(1));

        if(pollrate > maxpollrate)
                maxpollrate = POLLRATE_UNKNOWN;

        if(features.contains("adjrate"))
            adjrate = true;
    }

    if(hwlayoutPath.open(QIODevice::ReadOnly)){
        hwlayout = hwlayoutPath.read(10);
        hwlayout = hwlayout.trimmed();
        hwlayoutPath.close();
    }

    if(dpiPath.open(QIODevice::ReadOnly)){
        _maxDpi = dpiPath.read(6).trimmed().toUShort();
        dpiPath.close();
    }
    if(!_maxDpi)
        _maxDpi = 12000;

    prefsPath = "Devices/" + usbSerial;
    _k95HwSlots = _model == KeyMap::K95P && features.contains("hwslot1") && k95FirmwareTested();

    hwModeCount = (_model == KeyMap::K95 || k95Onboard()) ? 3 : 1;
    // Assumed until the daemon answers `get :modecount` on connect (see run());
    // matches ckb-next-daemon's compiled-in default.
    daemonModeCount = DAEMON_MODE_COUNT_DEFAULT;
    // Open cmd in non-blocking mode so that it doesn't lock up if nothing is reading
    // (e.g. if the daemon crashed and didn't clean up the node)
    int fd = open(cmdpath.toLatin1().constData(), O_WRONLY | O_NONBLOCK);
    if(!cmd.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle))
        return;

    // Find an available notification node (if none is found, take notify1)
    {
        QMutexLocker locker(&notifyPathMutex);
        for(int i = 1; i < 10; i++){
            QString notify = QString(path + "/notify%1").arg(i);
            if(!QFile::exists(notify) && !notifyPaths.contains(notify)){
                notifyNumber = i;
                notifyPath = notify;
                break;
            }
        }
        notifyPaths.insert(notifyPath);
    }
    cmd.write(QString("notifyon %1\n").arg(notifyNumber).toLatin1());
    cmd.flush();

    if(features.contains("battery")){
        batteryIcon = new BatteryStatusTrayIcon(usbModel, this);
        updateBattery();
        batteryTimer = new QTimer(this);
        connect(batteryTimer, &QTimer::timeout, this, &Kb::updateBattery);
        connect(this, &Kb::batteryChanged, batteryIcon, &BatteryStatusTrayIcon::setBattery);
        if (this->currentPerf())
            connect(this, &Kb::batteryChangedLed, this->currentPerf(), &KbPerf::setBattery);
        if(showBatteryIndicator)
            batteryIcon->show();
        batteryTimer->setInterval(10000);
        batteryTimer->start();
    }

    // Again, find an available notification node for macro definition
    // (if none is found, take notify2)
    {
        QMutexLocker locker(&notifyPathMutex);
        for(int i = 1; i < 10; i++){
            QString notify = QString(path + "/notify%1").arg(i);
            if(!QFile::exists(notify) && !notifyPaths.contains(notify)){
                macroNumber = i;
                macroPath = notify;
                break;
            }
        }
        notifyPaths.insert(notifyPath); ///< \todo Is adding notify2 to the notifypaths neccessary?
    }
    // Activate device, apply settings, and ask for hardware profile
    cmd.write(QString("fps %1\n").arg(_frameRate).toLatin1());
    cmd.write(QString("dither %1\n").arg(static_cast<int>(_dither)).toLatin1());
#ifdef Q_OS_MACOS
    // Write ANSI/ISO flag to daemon (OSX only)
    cmd.write("layout ");
    cmd.write(KeyMap::isISO(_layout) ? "iso" : "ansi");
    // Also OSX only: scroll speed and mouse acceleration
    cmd.write(QString("accel %1\n").arg(QString(_mouseAccel ? "on" : "off")).toLatin1());
    cmd.write(QString("scrollspeed %1\n").arg(_scrollSpeed).toLatin1());
#endif
    if(_k95HwSlots){
        // hwslot1: hardware or software mode is decided when the first cache load ends (decideK95Mode())
        cmd.write("\n");
        k95Decided = false;
        k95DecisionTimer = new QTimer(this);
        k95DecisionTimer->setSingleShot(true);
        connect(k95DecisionTimer, &QTimer::timeout, this, &Kb::decideK95Mode);
        k95DecisionTimer->start(k95DecisionTimeoutMs);
        k95ReadTimer = new QTimer(this);
        k95ReadTimer->setSingleShot(true);
        connect(k95ReadTimer, &QTimer::timeout, this, [this](){
            if(k95ReadSlot >= 0) k95ReadEnd(k95ReadSlot, false, tr("no answer from the daemon"));
        });
    } else
        cmd.write("\nactive\n");
    if(k95Onboard()){
        // K95P: an ordered marker closes each batch. No callback may publish a
        // partially loaded hardware profile or start a follow-up query itself.
        k95CacheBatch.reset(new HwCacheBatch(7, 0, false));
        if(_k95HwSlots)
            k95CacheBatch->requestBindings();
        cmd.write(QString("@%1 get :i :keys\n").arg(notifyNumber).toLatin1());
        const std::string request = k95CacheBatch->identityRequest(unsigned(notifyNumber));
        cmd.write(QByteArray(request.data(), int(request.size())));
    } else {
        cmd.write(QString("@%1 get :hwprofileid").arg(notifyNumber).toLatin1());
        hwLoading[0] = true;
        for(int i = 0; i < hwModeCount; i++){
            cmd.write(QString(" mode %1 get :hwid").arg(i + 1).toLatin1());
            hwLoading[i + 1] = true;
        }
        // Ask for current indicator and key state, and the daemon's configured mode count
        cmd.write(" get :i :keys :modecount\n");
    }
    cmd.flush();

    emit infoUpdated();
    activeDevices.insert(this);

    // Start a separate thread to read from the notification node
    start();
}

Kb::~Kb(){
    // Save settings first
    save();

    delete batteryTimer;
    delete batteryIcon;

    // remove the notify channel from the list of notifyPaths.
    ///< \todo I don't think, that notifypaths is used somewhere. So why do we have it?
    /// If we do not need it, searching for an ununsed notify channel can easy be refactored to a private member function.
    notifyPaths.remove(macroPath);

    // Kill notification thread and remove node
    activeDevices.remove(this);
    // FIXME: https://github.com/ckb-next/ckb-next/pull/1011
    if(k95Rec)
        k95StopRecording();
    if(!_hwFlowBusy && QFile::exists(cmdpath) && cmd.isOpen() && notifyNumber > 0){
        cmd.write(QString("idle\nnotifyoff %1\n").arg(notifyNumber).toLatin1());
        // Manually flush so that the daemon closes the notify pipe and the thread can gracefully stop
        cmd.flush();
    }
    if(!wait(1000)){
        terminate();
        qDebug() << "Second wait returned" << wait(1000);
    }
    // This does nothing if cmd isn't open.
    cmd.close();
}

void Kb::frameRate(int newFrameRate){
    KbManager::fps(newFrameRate);
    // If the rate has changed, send to all devices
    if(newFrameRate == _frameRate)
        return;
    _frameRate = newFrameRate;
    foreach(Kb* kb, activeDevices){
        if(kb->_hwFlowBusy) continue;
        kb->cmd.write(QString("fps %1\n").arg(newFrameRate).toLatin1());
        kb->cmd.flush();
    }
}

void Kb::layout(KeyMap::Layout newLayout, bool stop){
    if(_hwFlowBusy) return;
    if(newLayout == KeyMap::NO_LAYOUT || newLayout == _layout)
        return;
    _layout = newLayout;
    // Update the current device
    this->updateLayout(stop);
}

void Kb::updateLayout(bool stop){
#ifdef Q_OS_MACOS
    // Write ANSI/ISO flag to daemon (OSX only)
    if(!_hwFlowBusy){
        cmd.write("layout ");
        cmd.write(KeyMap::isISO(_layout) ? "iso" : "ansi");
        cmd.write("\n");
        cmd.flush();
    }
#endif
    foreach(KbProfile* profile, _profiles)
        profile->keyMap(getKeyMap());
    if(_hwProfile && !_profiles.contains(_hwProfile))
        _hwProfile->keyMap(getKeyMap());
    // Stop all animations as they'll need to be restarted, only if requested
    if(stop){
        foreach(KbMode* mode, _currentProfile->modes())
            mode->light()->close();
    }
    emit infoUpdated();
}

void Kb::updateBattery(){
    if(_hwFlowBusy) return;
    cmd.write(QString("@%1 get :battery\n").arg(notifyNumber).toLatin1());
    cmd.flush();
}

void Kb::dither(bool newDither){
    if(newDither == _dither)
        return;
    _dither = newDither;
    // Update all devices
    foreach(Kb* kb, activeDevices){
        if(kb->_hwFlowBusy) continue;
        kb->cmd.write(QString("dither %1\n").arg(static_cast<int>(newDither)).toLatin1());
        kb->cmd.flush();
    }
}

void Kb::mouseAccel(bool newAccel){
    if(newAccel == _mouseAccel)
        return;
    _mouseAccel = newAccel;
#ifdef Q_OS_MACOS
    // Update all devices
    foreach(Kb* kb, activeDevices){
        if(kb->_hwFlowBusy) continue;
        kb->cmd.write(QString("accel %1\n").arg(QString(newAccel ? "on" : "off")).toLatin1());
        kb->cmd.flush();
    }
#endif
}

void Kb::scrollSpeed(int newSpeed){
    if(newSpeed == _scrollSpeed)
        return;
    _scrollSpeed = newSpeed;
#ifdef Q_OS_MACOS
    // Update all devices
    foreach(Kb* kb, activeDevices){
        if(kb->_hwFlowBusy) continue;
        kb->cmd.write(QString("scrollspeed %1\n").arg(newSpeed).toLatin1());
        kb->cmd.flush();
    }
#endif
}

void Kb::load(){
    if(prefsPath.isEmpty())
        return;
    _needsSave = false;
    CkbSettings settings(prefsPath);
    // Read profiles
    KbProfile* newCurrentProfile = nullptr;
    QString current = settings.value("CurrentProfile").toString().trimmed().toUpper();
    foreach(QString guid, settings.value("Profiles").toString().split(" ")){
        guid = guid.trimmed().toUpper();
        if(guid != ""){
            KbProfile* profile = new KbProfile(this, getKeyMap(), settings, guid);
            _profiles.append(profile);
            if(guid == current || !newCurrentProfile)
                newCurrentProfile = profile;
        }
    }
    showBatteryIndicator = settings.value("batteryIndicator", true).toBool();
    // Kept whatever the daemon offers: save() rewrites the whole group of the device
    k95Migrated = settings.value("K95PHwMigrated", false).toBool();
    k95HwGuid = settings.value("K95PHwProfile").toString().trimmed().toUpper();
    if(_model == KeyMap::K95P){
        for(int i = 0; i < 3; ++i){
            const QString key = QString("K95PSlot%1").arg(i + 1);
            if(settings.contains(key))
                HwSlotDraft::deserialize(settings.value(key).toString().toStdString(), k95Drafts[size_t(i)]);
        }
    }
    if(_k95HwSlots && !k95HwGuid.isEmpty()){
        if(KbProfile* hw = find(QUuid::fromString(k95HwGuid)))
            hwProfile(hw);
    }
    // The hardware profile of the slots, with a daemon that does not read them: hidden (k95HiddenHwProfile), and the current
    // profile is then the first one shown, or the demo profile if there is none (as for a device without profiles). That one
    // is also saved as the current profile: with a daemon that reads the slots again the keyboard starts in software mode.
    if(_model == KeyMap::K95P && !k95Onboard() && k95Migrated && !k95HwGuid.isEmpty()){
        const QUuid hwGuid = QUuid::fromString(k95HwGuid);
        for(int i = 0; i < _profiles.count(); ++i){
            if(_profiles.at(i)->id().guid != hwGuid)
                continue;
            k95HiddenHwProfile = _profiles.takeAt(i);
            k95HiddenIndex = i;
            if(newCurrentProfile == k95HiddenHwProfile)
                newCurrentProfile = _profiles.isEmpty() ? nullptr : _profiles.first();
            qInfo() << "K95P hardware profile" << k95HiddenHwProfile->name() << "hidden: the daemon does not read the slots"
                    << "(it needs --enable-experimental)";
            break;
        }
    }
    if(newCurrentProfile)
        setCurrentProfile(newCurrentProfile);
    else {
        KeyMap map = getKeyMap();
        // If nothing was loaded, load the appropriate demo profile for each device
        QString demoProfile(":/txt/demoprofile.conf");
        if(map.model() == KeyMap::M95)
            demoProfile = ":/txt/demoprofile_m95.ini";
        else if(map.model() == KeyMap::K55)
            demoProfile = ":/txt/demoprofile_k55.ini";
        else if(map.model() == KeyMap::POLARIS)
            demoProfile = ":/txt/demoprofile_polaris.ini";
        else if(map.model() == KeyMap::ST100)
            demoProfile = ":/txt/demoprofile_st100.ini";
        else if(map.model() == KeyMap::NIGHTSWORD)
            demoProfile = ":/txt/demoprofile_nightsword.ini";
        else if(map.model() == KeyMap::K55PRO)
            demoProfile = ":/txt/demoprofile_k55pro.ini";
        else if(map.model() == KeyMap::MM700)
            demoProfile = ":/txt/demoprofile_mm700.ini";
        QSettings demoSettings(demoProfile, QSettings::IniFormat, this);
        CkbDemoSettings cSettings(demoSettings);
        KbProfile* demo = new KbProfile(this, map, cSettings, "{BA7FC152-2D51-4C26-A7A6-A036CC93D924}");
        _profiles.append(demo);
        setCurrentProfile(demo);
    }

    emit infoUpdated();
    emit profileAdded();
}

void Kb::save(){
    // CkbSettings::save changes UsbId::modified. Freeze the in-flight K95P
    // picture and identity while a hardware transaction is active.
    if(_hwFlowBusy) return;
    if(prefsPath.isEmpty())
        return;
    _needsSave = false;
    CkbSettings settings(prefsPath, true);
    QStringList guids;
    QString currentGuid;
    foreach(KbProfile* profile, _profiles){
        guids.append(profile->id().guidString());
        if(profile == _currentProfile)
            currentGuid = profile->id().guidString();
        profile->save(settings);
    }
    // The hidden hardware profile of the slots, where it was: the group of the device was erased, nothing of it may be lost
    if(k95HiddenHwProfile){
        guids.insert(qBound(0, k95HiddenIndex, guids.count()), k95HiddenHwProfile->id().guidString());
        k95HiddenHwProfile->save(settings);
    }
    settings.setValue("CurrentProfile", currentGuid);
    settings.setValue("Profiles", guids.join(" "));
    settings.setValue("hwLayout", KeyMap::getLayout(_layout));
    settings.setValue("batteryIndicator", showBatteryIndicator);
    if(_k95HwSlots && _hwProfile && _profiles.contains(_hwProfile))
        k95HwGuid = _hwProfile->id().guidString();
    if(k95Migrated)
        settings.setValue("K95PHwMigrated", true);
    if(!k95HwGuid.isEmpty())
        settings.setValue("K95PHwProfile", k95HwGuid);
    // The drafts of the slots (hwslot1), kept apart from the profiles so that an export never carries them; rewritten
    // also by a GUI whose daemon does not offer hwslot1, which leaves them as they were
    if(_model == KeyMap::K95P){
        for(int i = 0; i < 3; ++i){
            HwSlotDraft::Draft& d = k95Drafts[size_t(i)];
            if(_k95HwSlots)
                d.modified = k95DraftModified(i);
            if(!d.base.empty() || d.modified)
                settings.setValue(QString("K95PSlot%1").arg(i + 1), QString::fromStdString(HwSlotDraft::serialize(d)));
        }
    }
}

bool Kb::canDeleteMode(const KbProfile* profile) const {
    if(!profile || isHwSlotProfile(profile))
        return false;
    return profile->modeCount() > minimumModes(profile);
}

void Kb::migrateK95HwProfile(KbProfile* profile){
    // Once per device, on the first cache of a daemon with hwslot1. By position: hwsave saved the mode in position i to the
    // slot i, and its GUIDs were rewritten by position on every load. The first three modes become the slots (their name, id
    // and colours are kept until the cache replaces them; animations, bindings, events and performance are software things
    // and go), every mode after them is dropped. The backup of the settings, taken first (once), still has them.
    CkbSettings::k95pHwBackupOnce();
    foreach(KbProfile* other, _profiles){
        if(other != profile && other->id().guid == profile->id().guid){
            other->newId();
            qInfo() << "K95P hardware profile: the profile" << other->name() << "had its GUID and got a new one";
        }
    }
    const bool current = profile == _currentProfile;
    if(current)
        emit profileAboutToChange();
    if(prevMode && profile->indexOf(prevMode) >= 0){
        prevMode->light()->close();
        deletePrevious();
    }
    if(prevProfile == profile)
        prevProfile = nullptr;
    const int oldCurrent = profile->indexOf(profile->currentMode());
    KbProfile::ModeList slotModes;
    for(int i = 0; i < profile->modeCount(); ++i){
        KbMode* old = profile->at(i);
        if(i < hwModeCount){
            KbMode* slot = new KbMode(this, getKeyMap(), old->id().guidString(), old->id().modifiedString());
            slot->id().hwModified = old->id().hwModified;
            slot->name(old->name());
            const QColorMap& colours = old->light()->colorMap();
            for(QColorMap::const_iterator c = colours.constBegin(); c != colours.constEnd(); ++c)
                slot->light()->color(c.key(), QColor::fromRgb(c.value()));
            slotModes.append(slot);
        } else {
            qInfo() << "K95P hardware profile: the mode" << old->name() << "is not a slot and was dropped";
        }
        old->light()->close();
        old->deleteLater();
    }
    profile->modes(slotModes);
    profile->currentMode(slotModes.isEmpty() ? nullptr : slotModes.at(oldCurrent >= 0 && oldCurrent < slotModes.count() ? oldCurrent : 0));
    if(current)
        _currentMode = nullptr;
    k95Migrated = true;
    _needsSave = true;
    qInfo() << "K95P hardware profile" << profile->name() << "is now the three slotModes; backup of the settings:"
            << CkbSettings::get("Program/K95PHwBackup").toString();
    if(current){
        emit profileChanged();
        if(profile->currentMode())
            setCurrentMode(profile->currentMode());
    }
}

void Kb::autoSave(){
    if(needsSave() && !CkbSettings::isBusy())
        save();
}

void Kb::hwSave(){
    // The legacy path rewrites every onboard mode without K95P's check
    // and transaction result. K95P can only use HwSaveController.
    if(_hwFlowBusy || _model == KeyMap::K95P) return;
    if(!_currentProfile)
        return;
    // Close active lighting (if any)
    if(prevMode){
        prevMode->light()->close();
        deletePrevious();
    }
    hwProfile(_currentProfile);
    _hwProfile->id().hwModified = _hwProfile->id().modified;
    _hwProfile->setNeedsSave();
    // Re-send the current profile from scratch to ensure consistency
    if(!writeProfileHeader())
        return;
    // Make sure there are enough modes
    while(_currentProfile->modeCount() < hwModeCount)
        _currentProfile->append(new KbMode(this, getKeyMap()));
    // Write only the base colors of each mode, no animations
    for(int i = 0; i < hwModeCount; i++){
        KbMode* mode = _currentProfile->modes()[i];
        cmd.write(QString("\nmode %1").arg(i + 1).toLatin1());
        KbLight* light = mode->light();
        KbPerf* perf = mode->perf();
        if(mode == _currentMode)
            cmd.write(" switch");
        // Write the mode name and ID
        QByteArray encoded;
        if(!encodedModeName(_model, mode->name(), encoded))
            return;
        cmd.write(" name ");
        cmd.write(encoded);
        cmd.write(" id ");
        cmd.write(mode->id().guidString().toLatin1());
        cmd.write(" ");
        cmd.write(mode->id().modifiedString().toLatin1());
        cmd.write(" ");
        // Write lighting and performance
        light->base(cmd, true, monochrome);
        cmd.write(" ");
        perf->update(cmd, notifyNumber, true, false);
        // Update mode ID
        mode->id().hwModified = mode->id().modified;
        mode->setNeedsSave();
    }
    cmd.write("\n");

    // Save the profile to memory
    cmd.write("hwsave\n");
    cmd.flush();
}

bool Kb::needsSave() const {
    if(_needsSave)
        return true;
    foreach(const KbProfile* profile, _profiles){
        if(profile->needsSave())
            return true;
    }
    return false;
}

bool Kb::writeProfileHeader(){
    QByteArray encoded;
    if(!encodedModeName(_model, _currentProfile->name(), encoded))
        return false;
    cmd.write("eraseprofile");
    // Write the profile name and ID
    cmd.write(" profilename ");
    cmd.write(encoded);
    cmd.write(" profileid ");
    cmd.write(_currentProfile->id().guidString().toLatin1());
    cmd.write(" ");
    cmd.write(_currentProfile->id().modifiedString().toLatin1());
    return true;
}

void Kb::pushModeNames(){
    if(_hwFlowBusy || k95Quiet()) return;
    // Sets each mode's name in the daemon's in-memory profile, without touching
    // lighting/binding/erasing anything - safe to call any time, not just on
    // profile load. Capped at daemonModeCount: the daemon has no slot for modes
    // beyond that (see checkModeCountWarning()).
    const KbProfile::ModeList& profileModes = _currentProfile->modes();
    int nameableModes = qMin(profileModes.count(), daemonModeCount);
    for(int i = 0; i < nameableModes; i++){
        QByteArray encoded;
        if(!encodedModeName(_model, profileModes.at(i)->name(), encoded))
            continue;
        cmd.write(QString("mode %1 name ").arg(i + 1).toLatin1());
        cmd.write(encoded);
        cmd.write(" ");
    }
}

void Kb::checkModeCountWarning(){
    if(!_currentProfile)
        return;
    if(_currentProfile->modeCount() > daemonModeCount){
        // Warn at most once per profile instance while it stays over the limit -
        // naturally re-arms if the profile changes (different pointer) or if this
        // same profile later drops under the limit and goes over it again.
        if(modeCountWarnedProfile != _currentProfile){
            modeCountWarnedProfile = _currentProfile;
            emit modeCountExceeded(_currentProfile->modeCount(), daemonModeCount);
            emit modeCountStatusChanged();
        }
    } else if(modeCountWarnedProfile == _currentProfile){
        modeCountWarnedProfile = nullptr;
        emit modeCountStatusChanged();
    }
}

void Kb::fwUpdate(const QString& path){
    // A firmware update needs the software mode: not from the hardware profile of the slots
    if(_hwFlowBusy || k95Quiet()) return;
    fwUpdPath = path;
    // Write the active command to ensure it's not ignored
    cmd.write("active");
    cmd.write(QString(" @%1 ").arg(notifyNumber).toLatin1());
    cmd.write("fwupdate ");
    cmd.write(path.toLatin1());
    cmd.write("\n");
}

void Kb::frameUpdate(){
    if(_hwFlowBusy) return;
    // Advance animation frame
    if(!_currentMode)
        return;
    if(k95Quiet()){
        // The keyboard shows its own slot: only the GUI's preview of the colours being edited
        if(k95InHw)
            _currentMode->light()->previewBase();
        return;
    }
    KbLight* light = _currentMode->light();
    KbBind* bind = _currentMode->bind();
    KbPerf* perf = _currentMode->perf();
    if(!light->isStarted()){
        // Don't do anything until the animations are started
        light->open();
        return;
    }

    // Stop animations on the previously active mode (if any)
    bool changed = false;
    if(prevMode != _currentMode){
        if(prevMode){
            prevMode->light()->close();
            disconnect(prevMode, SIGNAL(destroyed()), this, SLOT(deletePrevious()));
        }
        prevMode = _currentMode;
        connect(prevMode, SIGNAL(destroyed()), this, SLOT(deletePrevious()));
        changed = true;
    }

    // If the profile has changed, update it
    if(prevProfile != _currentProfile){
        if(!writeProfileHeader())
            return;
        cmd.write(" ");
        // Push every mode's name into the daemon's in-memory profile too (not just
        // the active mode, handled below), so `mode <n> get :name` works for any
        // mode.
        pushModeNames();
        prevProfile = _currentProfile;
    }
    // Checked every frame (not just on profile change) so adding a mode to the
    // profile that's already selected is caught immediately, not only after
    // switching away and back.
    checkModeCountWarning();

    // Update current mode
    int rawIndex = _currentProfile->indexOf(_currentMode);
    int index = rawIndex;
    // The daemon only keeps daemonModeCount software mode slots per device: the
    // first hwModeCount of those double as actual onboard hardware slots, the
    // rest form a rotating window shared by every mode beyond that.
    // e.g. (hwModeCount=3, daemonModeCount=6): 1,2,3,4,5,6,4,5,6,4,5,6 ...
    if(index >= daemonModeCount){
        int extraSlots = daemonModeCount - hwModeCount;
        index = (extraSlots > 0) ? hwModeCount + (index - hwModeCount) % extraSlots : hwModeCount - 1;
    }

    // Send lighting/binding to driver
    bool modeSwitched = (prevMode != _currentMode || changed);
    if(modeSwitched)
        cmd.write(QString("mode %1 switch ").arg(index + 1).toLatin1());
    // Keep the daemon's in-memory mode name in sync so `get :name` reflects it
    // even for software profiles (which are otherwise never pushed to the daemon
    // outside of hwSave()). Re-sent whenever the mode becomes active, or when it
    // has unsaved changes (e.g. was just renamed while already active).
    // Skipped for modes beyond daemonModeCount: `index` above was remapped onto a
    // slot shared with (and correctly named for) an in-range mode - sending this
    // mode's name would clobber that slot's real name with the wrong one.
    if(rawIndex < daemonModeCount && (modeSwitched || _currentMode->needsSave())){
        QByteArray encoded;
        if(encodedModeName(_model, _currentMode->name(), encoded)){
            cmd.write("name ");
            cmd.write(encoded);
            cmd.write(" ");
        }
    }
    perf->applyIndicators(index, iState);
    light->frameUpdate(cmd, monochrome);
    bind->update(cmd, notifyNumber, changed);
    perf->update(cmd, notifyNumber, changed, true);
    cmd.flush();
}

void Kb::deletePrevious(){
    disconnect(prevMode, SIGNAL(destroyed()), this, SLOT(deletePrevious()));
    prevMode = nullptr;
}

void Kb::hwProfile(KbProfile* newHwProfile){
    if(_hwProfile == newHwProfile)
        return;
    if(_hwProfile)
        disconnect(_hwProfile, SIGNAL(destroyed()), this, SLOT(deleteHw()));
    _hwProfile = newHwProfile;
    if(_hwProfile)
        connect(_hwProfile, SIGNAL(destroyed()), this, SLOT(deleteHw()));
}

void Kb::deleteHw(){
    disconnect(_hwProfile, SIGNAL(destroyed()), this, SLOT(deleteHw()));
    _hwProfile = nullptr;
}

void Kb::run(){
    QFile notify(notifyPath);
    // Wait a small amount of time for the node to open (100ms)
    QThread::usleep(100000);
    if(!notify.open(QIODevice::ReadOnly)){
        // If it's still not open, try again before giving up (1s at a time, 10s total)
        QThread::usleep(900000);
        for(int i = 1; i < 10; i++){
            if(notify.open(QIODevice::ReadOnly))
                break;
            QThread::sleep(1);
        }
        if(!notify.isOpen())
            return;
    }
    // Read data from notification node
    QByteArray line;
    while(notify.isOpen() && (line = notify.readLine()).length() > 0){
        QString text = QString::fromUtf8(line);
        metaObject()->invokeMethod(this, "readNotify", Qt::QueuedConnection, Q_ARG(QString, text));
    }
    QMutexLocker locker(&notifyPathMutex);
    notifyPaths.remove(notifyPath);
    qDebug() << "Notify thread returning. Read" << line.length() << "isOpen()" << notify.isOpen();
}

void Kb::applyK95CacheBatch(){
    if(!k95CacheBatch || k95CacheBatch->state() != HwCacheBatch::Complete) return;
    const HwCacheBatch::Result& data = k95CacheBatch->result();
    // The load of the record of one slot read on demand, or the attach or the read-back of a save
    const int readSlot = k95ReadBatchSlot;
    k95ReadBatchSlot = -1;
    const QString profileGuid = QString::fromStdString(data.profileGuid);
    KbProfile* profile = find(QUuid::fromString(profileGuid));
    if(!profile && _k95HwSlots && _hwProfile && _profiles.contains(_hwProfile)){
        // One hardware profile: it follows the identity the daemon reports
        profile = _hwProfile;
        profile->id().guidString(profileGuid);
    }
    if(!profile){
        profile = new KbProfile(this, getKeyMap(), profileGuid,
                                QString::fromStdString(data.profileRevision));
        _profiles.append(profile);
        _needsSave = true;
    }
    profile->id().modifiedString(QString::fromStdString(data.profileRevision));
    profile->id().hwModifiedString(QString::fromStdString(data.profileRevision));
    profile->name(QUrl::fromPercentEncoding(QByteArray::fromStdString(data.profileName)));
    if(_k95HwSlots && !k95Migrated)
        migrateK95HwProfile(profile);
    // The slots by position: a refresh of slot 3 alone must not append it as the first missing mode
    while(profile->modeCount() < 3)
        profile->append(new KbMode(this, getKeyMap()));
    unsigned conflicts = 0, unsynced = 0;
    for(unsigned i = 0; i < 3; ++i){
        if(!(k95CacheBatch->mask() & (1u << i))) continue;
        const auto& mode = data.modes[i];
        KbMode* target = profile->at(int(i));
        target->id().guidString(QString::fromStdString(mode.guid));
        target->id().modifiedString(QString::fromStdString(mode.revision));
        target->id().hwModifiedString(QString::fromStdString(mode.revision));
        if(!_k95HwSlots){
            fillK95Mode(target, mode, i);
            continue;
        }
        // hwslot1: the record is the base; it replaces the draft (the mode's name and colours, and the bindings) only when
        // the draft has nothing of its own, or when it is the read-back of our own save and holds what was saved
        HwSlotDraft::Draft& draft = k95Drafts[i];
        if(k95CacheReady)
            draft.modified = k95DraftModified(int(i));  // against the base it was made on, before that is replaced
        const bool committing = k95CommitMask & (1u << i);
        // The read-back of our own save holds what the slot was to become, or the GUI and the keyboard disagree: then the draft
        // stays, modified on the base it was made on, and the next load asks to reload or keep it (nothing is lost in silence)
        const HwBinding::Record* oldBase = k95CacheKnown.modes[i].hasRecord ? &k95CacheKnown.modes[i].record : nullptr;
        if(committing && (!mode.hasRecord || profile != _hwProfile || k95ContentDiffers(int(i), mode, oldBase))){
            unsynced |= 1u << i;
            draft.modified = true;
            continue;
        }
        if(readSlot < 0)
            k95Read[i] = mode.hasRecord ? K95_READ : K95_UNREAD;   // (a slot in use has no record until it is read: nocache)
        if(!mode.hasRecord){
            // The daemon has no cache of the slot: nothing to compare with, and nothing to edit
            if(!draft.modified || committing){
                HwSlotDraft::replace(draft, mode.record, false);
                fillK95Mode(target, mode, i);
            }
            continue;
        }
        switch(HwSlotDraft::decide(draft, mode.record.id, committing)){
        case HwSlotDraft::REPLACE:
            HwSlotDraft::replace(draft, mode.record, true);
            fillK95Mode(target, mode, i);
            break;
        case HwSlotDraft::KEEP:
            break;
        case HwSlotDraft::CONFLICT:
            draft.conflict = true;
            conflicts |= 1u << i;
            break;
        }
    }
    k95CommitMask &= ~k95CacheBatch->mask();
    if(!profile->currentMode() && profile->modeCount() > 0)
        profile->currentMode(profile->at(0));
    hwProfile(profile);
    daemonModeCount = int(data.modeCount);
    k95CacheKnown = data;
    if(readSlot < 0)
        k95CacheReady = !unsynced;
    if(unsynced)
        qWarning() << "K95P hardware cache: the slots read back after a save do not hold what was saved, mask" << unsynced
                   << "- no further save until the keyboard is read again";
    if(!_currentProfile) setCurrentProfile(profile);
    emit profileAdded();
    emit profileRenamed();
    emit modeCountStatusChanged();
    if(readSlot >= 0){
        // (not k95CacheFinished: that is the end of the attach or of a save's read-back)
        const bool ok = data.modes[size_t(readSlot)].hasRecord;
        k95ReadEnd(readSlot, ok, ok ? QString() : tr("the daemon has no copy of it"));
    } else
        emit k95CacheFinished(!unsynced);
    decideK95Mode();
    // A load that ends after the decision (it timed out on the first start, before the hardware profile was known) may
    // recognize the hardware profile only now: it is still the keyboard's hardware mode
    if(_k95HwSlots && k95Decided && !k95InHw && !k95Rec && !_hwFlowBusy && isHwSlotProfile(_currentProfile))
        enterK95Hw();
    // Asked after the decision, and from the event loop: the answer (a dialog) must not run inside the reading of the
    // notifications
    for(int i = 0; i < 3; ++i)
        if(conflicts & (1u << i))
            QTimer::singleShot(0, this, [this, i](){ emit k95DraftConflict(i); });
    // The mode shown may be a slot to read now (the first load, or one that was waiting for this one)
    k95WantSlot();
}

static const std::set<int>& k95WritableLeds(){
    // The LEDs of the 135 cells a lighting file can hold (the indicators are in PROFILE.I, not in these files)
    static std::set<int> writable;
    if(writable.empty()){
        for(size_t c = 0; c < K95PLedMap::canonicalCount; ++c){
            const int cell = K95PLedMap::canonicalCells[c];
            if(cell >= 144) writable.insert(cell);
            else if(cell < 96) writable.insert(12 * (cell / 8) + cell % 8);
            else {
                const int col = cell / 8 - 12, within = cell % 8;
                writable.insert(12 * (col + 6 * (within / 4)) + 8 + within % 4);
            }
        }
    }
    return writable;
}

// The colours of the writable LEDs that a :hwrgb answer gives, by LED: black for one that no name of the answer gives, else the
// colour of the names it gives for it (the daemon prints every name of a LED with its colour)
static std::map<int, QRgb> k95BaseLeds(const std::string& rgb){
    std::map<int, QRgb> out;
    const std::set<int>& writable = k95WritableLeds();
    for(int led : writable) out[led] = 0;
    for(const QString& group : QString::fromStdString(rgb).split(' ')){
        if(group.isEmpty()) continue;
        const int colon = group.indexOf(':');
        bool ok = false;
        const uint colour = (colon < 0 ? group : group.mid(colon + 1)).toUInt(&ok, 16) & 0xffffff;
        if(!ok) continue;
        if(colon < 0){
            for(auto& led : out) led.second = colour;
            continue;
        }
        for(const QString& key : group.left(colon).split(','))
            for(size_t n = 0; n < K95PLedMap::count; ++n)
                if(key == QLatin1String(K95PLedMap::entries[n].name) && out.count(K95PLedMap::entries[n].led))
                    out[K95PLedMap::entries[n].led] = colour;
    }
    return out;
}

static QString k95NameOf(const HwCacheBatch::Mode& mode, unsigned i){
    // The whole name of the record (hwslot1); iCUE's name of a new profile for an empty slot; else the one of :hwname
    if(mode.hasRecord && mode.record.hasName)
        return QString::fromStdString(mode.record.name);
    if(mode.hasRecord && mode.record.id == "0")
        return QString("HW Profile %1").arg(i + 1);
    return QUrl::fromPercentEncoding(QByteArray::fromStdString(mode.name));
}

bool Kb::k95SlotEmpty(int i) const {
    const HwBinding::Record* r = k95SlotRecord(i);
    return r && r->id == "0";
}

QString Kb::k95BaseName(int i) const {
    if(i < 0 || i > 2)
        return QString();
    return k95NameOf(k95CacheKnown.modes[size_t(i)], unsigned(i));
}

void Kb::k95Recreate(int i, bool on){
    HwSlotDraft::Draft* d = k95Draft(i);
    const HwBinding::Record* r = k95SlotRecord(i);
    if(!d || !r || r->readonly || (r->state != HwBinding::Record::RAW && r->state != HwBinding::Record::BROKEN) || d->recreate == on)
        return;
    d->recreate = on;
    d->hasBindings = on;
    d->keys = HwBinding::Keys();   // (the performance settings are not bindings: they stay as they are)
    k95DraftChanged(i);
}

void Kb::k95ReplaceLights(int i, bool on){
    HwSlotDraft::Draft* d = k95Draft(i);
    const HwBinding::Record* r = k95SlotRecord(i);
    if(!d || !r || r->readonly || (r->light != HwBinding::Record::EFFECTS && r->light != HwBinding::Record::UNKNOWN) ||
       d->replaceLights == on)
        return;
    d->replaceLights = on;
    k95DraftChanged(i);
}

HwSlotDraft::Perf Kb::k95Perf(int i) const {
    if(!_k95HwSlots || i < 0 || i > 2)
        return HwSlotDraft::Perf();
    return HwSlotDraft::effectivePerf(k95Drafts[size_t(i)], k95SlotRecord(i));
}

void Kb::k95SetPerf(int i, const HwSlotDraft::Perf& perf){
    HwSlotDraft::Draft* d = k95Draft(i);
    if(!d)
        return;
    d->hasWinlock = d->hasIndicators = true;
    d->winlock = perf.winlock;
    d->indicators = perf.indicators;
    k95DraftChanged(i);
}

void Kb::k95RestorePerfDefaults(int i){
    k95SetPerf(i, HwSlotDraft::Perf());   // iCUE's: the Windows key only, and its colours; bits 4..7 cleared
}

void Kb::k95NoteLater(const QString& text){
    QTimer::singleShot(0, this, [this, text](){ emit k95Note(text); });
}

void Kb::k95ApplyPerf(int i, const HwSlotDraft::Perf& from){
    const HwCacheBatch::Mode& m = k95CacheKnown.modes[size_t(i)];
    if(!m.hasRecord){
        k95NoteLater(tr("M%1 was not read from the keyboard: the performance settings were not copied to it.").arg(i + 1));
        return;
    }
    if(m.record.readonly){
        k95NoteLater(tr("M%1 cannot be rewritten (%2): the performance settings were not copied to it.")
                     .arg(i + 1).arg(QString::fromStdString(m.record.readonlyWhy)));
        return;
    }
    HwSlotDraft::Draft& d = k95Drafts[size_t(i)];
    const HwSlotDraft::Perf to = HwSlotDraft::effectivePerf(d, &m.record);
    HwSlotDraft::Perf p = from;
    p.winlock = uint8_t((from.winlock & 0x0f) | (to.winlock & 0xf0));   // the bits iCUE never writes stay the target's
    k95SetPerf(i, p);
}

void Kb::k95CopyPerf(int from, const std::vector<int>& to){
    if(!_k95HwSlots || from < 0 || from > 2)
        return;
    const HwSlotDraft::Perf src = k95Perf(from);
    for(int i : to){
        if(i < 0 || i > 2 || i == from)
            continue;
        if(k95Read[size_t(i)] == K95_READ && k95CacheKnown.modes[size_t(i)].hasRecord){
            k95ApplyPerf(i, src);
            continue;
        }
        // Read it first: the copy waits for its record (a read already queued or running is not asked again)
        k95PendingCopy[size_t(i)] = true;
        k95PendingPerf[size_t(i)] = src;
        k95QueueRead(i, true);
        emit k95SlotReadChanged(i);
    }
    k95NextRead();
}

void Kb::fillK95Mode(KbMode* target, const HwCacheBatch::Mode& mode, unsigned i){
    target->name(k95NameOf(mode, i));
    const std::set<int>& writable = k95WritableLeds();
    if(mode.hasRgb){
        // Clear only writable cells. Indicators are not in CAPE lighting
        // files and retain their local settings unless explicitly reported.
        for(size_t n = 0; n < K95PLedMap::count; ++n)
            if(writable.count(K95PLedMap::entries[n].led))
                target->light()->color(QLatin1String(K95PLedMap::entries[n].name), QColor(0, 0, 0));
        const QStringList groups = QString::fromStdString(mode.rgb).split(' ');
        for(const QString& group : groups){
            if(group.isEmpty()) continue;
            const int colon = group.indexOf(':');
            const QString hex = colon < 0 ? group : group.mid(colon + 1);
            bool ok = false;
            const uint rgb = hex.toUInt(&ok, 16);
            if(!ok) continue; // already validated by HwCacheBatch
            const QColor colour = QColor::fromRgb(rgb);
            if(colon < 0){
                for(size_t n = 0; n < K95PLedMap::count; ++n)
                    if(writable.count(K95PLedMap::entries[n].led))
                        target->light()->color(QLatin1String(K95PLedMap::entries[n].name), colour);
            } else {
                for(const QString& key : group.left(colon).split(','))
                    target->light()->color(key, colour);
            }
        }
    } else if((_hwFlowBusy && (k95CacheBlackMask & (1u << i))) ||
              (mode.hasRecord && mode.record.light == HwBinding::Record::NONE)){
        // A saved all-black picture has zero layers and therefore no RGB
        // response (the record of hwslot1 says it: lt:empty). Only canonical
        // writable cells may be inferred black.
        for(size_t n = 0; n < K95PLedMap::count; ++n)
            if(writable.count(K95PLedMap::entries[n].led))
                target->light()->color(QLatin1String(K95PLedMap::entries[n].name), QColor(0, 0, 0));
    }
}

const HwBinding::Record* Kb::k95SlotRecord(int i) const {
    if(!_k95HwSlots || !k95CacheReady || i < 0 || i > 2 || !k95CacheKnown.modes[size_t(i)].hasRecord)
        return nullptr;
    return &k95CacheKnown.modes[size_t(i)].record;
}

HwSlotDraft::Draft* Kb::k95Draft(int i){
    if(!_k95HwSlots || i < 0 || i > 2)
        return nullptr;
    return &k95Drafts[size_t(i)];
}

bool Kb::k95DraftModified(int i){
    if(!_k95HwSlots || i < 0 || i > 2)
        return false;
    HwSlotDraft::Draft& d = k95Drafts[size_t(i)];
    const HwBinding::Record* base = k95SlotRecord(i);
    if(!base || !_hwProfile || _hwProfile->modeCount() <= i || d.base != base->id)
        return d.modified;  // not comparable now: what was last known
    return d.recreate || d.replaceLights || k95ContentDiffers(i, k95CacheKnown.modes[size_t(i)]);
}

bool Kb::k95NameDiffers(int i, const HwCacheBatch::Mode& cached) const {
    // As a mode would hold the record's name (KbMode::name(): trimmed, "Unnamed" for nothing): a name with spaces at its ends
    // is not a change
    const QString name = k95NameOf(cached, unsigned(i)).trimmed();
    return _hwProfile->at(i)->name() != (name.isEmpty() ? QString("Unnamed") : name);
}

bool Kb::k95ContentDiffers(int i, const HwCacheBatch::Mode& cached, const HwBinding::Record* fallback) const {
    const HwBinding::Record& base = cached.record;
    const HwSlotDraft::Draft& d = k95Drafts[size_t(i)];
    if(k95NameDiffers(i, cached))
        return true;
    if(d.hasBindings && (base.state == HwBinding::Record::RAW || base.state == HwBinding::Record::BROKEN ||
                         !HwBinding::sameBindings(d.keys, base.keys)))
        return true;
    // The performance settings, whatever the bindings are; what the draft does not know is the one of the base it was made on
    if(HwSlotDraft::effectivePerf(d, fallback ? fallback : &base) != HwSlotDraft::basePerf(&base))
        return true;
    return k95ColoursDiffer(i, cached);
}

bool Kb::k95ColoursDiffer(int i, const HwCacheBatch::Mode& cached) const {
    // The colours count only where they are an image the GUI can show (static layers, or none)
    if(cached.record.light != HwBinding::Record::STATIC && cached.record.light != HwBinding::Record::NONE)
        return false;
    // By LED, not by name: the daemon's :hwrgb gives every name of a LED (the LED of 1 is also zone14, the one of esc zone1), the
    // keymap of the mode has one of them, and a save takes the colour of that one (makeK95PSnapshot)
    const std::map<int, QRgb> want = k95BaseLeds(cached.hasRgb ? cached.rgb : std::string());
    K95PSnapshot have;
    std::string error;
    if(!makeK95PSnapshot(_hwProfile->keyMap(), _hwProfile->at(i)->light()->colorMap(), QString("x"), unsigned(i + 1), have, error))
        return true;
    for(const auto& led : want)
        if(size_t(led.first) >= have.ledRgb.size() || (have.ledRgb[size_t(led.first)] & 0xffffff) != led.second)
            return true;
    return false;
}

bool Kb::k95LightsChanged(int i) const {
    if(!k95SlotRecord(i) || !_hwProfile || _hwProfile->modeCount() <= i)
        return false;
    return k95ColoursDiffer(i, k95CacheKnown.modes[size_t(i)]);
}

bool Kb::k95SaveSlots(bool all, int selected, std::vector<HwSaveFlow::Slot>& saveSlots, std::array<bool, 3>& rgbRequired,
                      QString& why){
    saveSlots.clear();
    rgbRequired = {{false, false, false}};
    if(!_k95HwSlots || !k95CacheReady || !_hwProfile || _hwProfile->modeCount() < 3 || k95Rec || notifyNumber < 1 ||
       notifyNumber > 9 || (!all && (selected < 0 || selected > 2))){
        why = tr("The hardware slots are not ready.");
        return false;
    }
    if(k95CopyPending()){
        why = tr("A copy of the performance settings is waiting for a slot to be read from the keyboard.");
        return false;
    }
    // A transaction of its own for each slot (a begin ends every open one with the same number): never 0, never the same
    const uint32_t txn = uint32_t(Q_RAND()) & 0xfffffff0u;
    for(int i = 0; i < 3; ++i){
        if(!all && i != selected)
            continue;
        const HwBinding::Record* record = k95SlotRecord(i);
        if(!record && all && k95DraftModified(i)){
            // Changes kept from an earlier session on a slot not read yet
            why = tr("M%1 has changes but has not been read from the keyboard yet: select it to read it, then save.").arg(i + 1);
            return false;
        }
        if(!record && all)
            continue;   // not read and not changed: nothing to save
        if(!record){
            why = tr("M%1 was not read from the keyboard.").arg(i + 1);
            return false;
        }
        KbMode* mode = _hwProfile->at(i);
        K95PStage stage;
        std::string error;
        if(!makeK95PStage(_hwProfile->keyMap(), mode->light()->colorMap(), mode->name(), k95BaseName(i), k95LightsChanged(i),
                          *record, k95Drafts[size_t(i)], unsigned(i + 1), unsigned(notifyNumber), txn | unsigned(i + 1), stage, error)){
            why = tr("M%1 cannot be prepared: %2").arg(i + 1).arg(QString::fromStdString(error));
            return false;
        }
        HwSaveFlow::Slot slot;
        slot.mode = unsigned(i + 1);
        slot.prepare = stage.prepare;
        slot.check_line = stage.check;
        slot.save_line = stage.save;
        slot.abort_line = stage.abort;
        slot.invisible_keys = stage.pic && stage.invisibleKeys;
        slot.many_colours = stage.pic && stage.colours > 5;
        rgbRequired[size_t(i)] = stage.rgbRequired;
        saveSlots.push_back(slot);
    }
    if(saveSlots.empty()){
        why = tr("No slot was read from the keyboard yet: select a mode to read it.");
        return false;
    }
    return true;
}

void Kb::k95DraftChanged(int i){
    if(!_k95HwSlots || i < 0 || i > 2)
        return;
    _needsSave = true;
    emit k95DraftUpdated(i);
}

void Kb::k95ResolveConflict(int i, bool reload){
    if(!_k95HwSlots || i < 0 || i > 2 || !k95Drafts[size_t(i)].conflict || !_hwProfile || _hwProfile->modeCount() <= i)
        return;
    const HwCacheBatch::Mode& cached = k95CacheKnown.modes[size_t(i)];
    if(reload){
        HwSlotDraft::replace(k95Drafts[size_t(i)], cached.record, cached.hasRecord);
        fillK95Mode(_hwProfile->at(i), cached, unsigned(i));
    } else
        HwSlotDraft::keep(k95Drafts[size_t(i)], cached.hasRecord ? cached.record.id : std::string());
    k95DraftChanged(i);
}

void Kb::decideK95Mode(){
    // Once, when the first cache load ends, fails, or takes too long. The hardware profile may be known from the settings
    // even when the load failed: then the keyboard still goes to hardware mode, with the editor and the save off until a load
    // succeeds. Software mode only for a software profile, or when the hardware profile is not known at all.
    if(k95Decided)
        return;
    k95Decided = true;
    if(k95DecisionTimer)
        k95DecisionTimer->stop();
    if(isHwSlotProfile(_currentProfile)){
        enterK95Hw();
        return;
    }
    cmd.write("active\n");
    cmd.flush();
    if(_currentMode)
        _currentMode->light()->forceFrameUpdate();
}

void Kb::enterK95Hw(){
    // The software state goes: the next software profile is sent whole (header, switch, bindings, macros, performance)
    if(prevMode){
        prevMode->light()->close();
        deletePrevious();
    }
    prevProfile = nullptr;
    k95InHw = true;
    // One line: the daemon's profile is cleared while it is still active, then the keyboard goes to hardware mode
    cmd.write("eraseprofile idle\n");
    cmd.flush();
}

void Kb::readNotify(const QString& line){
    QStringList components = line.trimmed().split(" ");
    // The read of a slot on demand: before the cache batch, which would take any other "mode" line for a line out of order
    if(_model == KeyMap::K95P && _k95HwSlots && components[0] == "mode" && components.count() >= 3 && components[2] == "hwread"){
        k95ReadLine(components, line);
        return;
    }
    if(k95Onboard() && components[0] == "mode" && components.count() >= 3 &&
       (components[2] == "hwsave" || components[2] == "hwsavecheck")){
        emit hwSaveLine(line);
        return;
    }
    if(_model == KeyMap::K95P && k95CacheBatch && !k95ReplayingCache){
        if(k95CacheBatch->accept(line.trimmed().toStdString())){
            if(k95BatchDiscard)
                return;   // (a late line of the record of a read that ran out of time: nothing follows from it)
            // A line of a load renews the guard that waits for it: the read of a slot on demand, or a save's read-back
            if(k95ReadBatchSlot >= 0 && k95ReadBatchSlot == k95ReadSlot && k95ReadTimer)
                k95ReadTimer->start(k95ReadGuardMs);
            if(_hwFlowBusy)
                emit k95CacheLineAccepted();
            // The follow-up of the phase or page that just began, once (the details, then each page of the bindings)
            const std::string request = k95CacheBatch->takeRequest(unsigned(notifyNumber));
            if(!request.empty()){
                if(_hwFlowBusy) emit k95CacheRequest(QString::fromStdString(request));
                else {
                    cmd.write(QByteArray(request.data(), int(request.size())));
                    cmd.flush();
                }
            }
            if(k95CacheBatch->state() == HwCacheBatch::Complete){
                applyK95CacheBatch();
            } else if(k95CacheBatch->state() == HwCacheBatch::Failed && k95ReadBatchSlot >= 0){
                // The record of a slot read on demand: that slot failed, the rest of the cache is as it was
                if(k95CacheBatch->failureDrained() && !k95CacheFailureNotified){
                    k95CacheFailureNotified = true;
                    const int slot = k95ReadBatchSlot;
                    k95ReadBatchSlot = -1;
                    qWarning() << "K95P hardware cache: the record of slot" << slot + 1 << "failed:" <<
                                  QString::fromStdString(k95CacheBatch->error());
                    k95ReadEnd(slot, false, QString::fromStdString(k95CacheBatch->error()));
                }
            } else if(k95CacheBatch->state() == HwCacheBatch::Failed){
                k95CacheReady = false;
                if(k95CacheBatch->failureDrained() && !k95CacheFailureNotified){
                    k95CacheFailureNotified = true;
                    qWarning() << "K95P hardware cache load failed:" <<
                                  QString::fromStdString(k95CacheBatch->error());
                    emit k95CacheFinished(false);
                    decideK95Mode();
                }
            }
            return;
        }
        // A late or out-of-stage cache response must not enter the legacy
        // incremental loader and mutate a profile behind the batch's back.
        if(components[0] == "hwprofileid" || components[0] == "hwprofilename" ||
           (components[0] == "mode" && components.count() >= 3 &&
            (components[2] == "hwid" || components[2] == "hwname" || components[2] == "hwrgb" ||
             components[2] == "hwbind")))
            return;
    }
    if(components.count() < 2)
        return;
    if(components[0] == "key"){
        // Key event
        QString key = components[1];
        if(key.length() < 2)
            return;
        QString keyName = key.mid(1);
        bool keyPressed = (key[0] == '+');
        KbMode* mode = _currentMode;
        if(mode){
            mode->light()->animKeypress(keyName, keyPressed);
            mode->bind()->keyEvent(keyName, keyPressed);
        }
        deviceIdleTimer.start();
    } else if (components[0] == "battery"){
        QStringList bComponents = components[1].split(':');
        if(bComponents.length() != 2)
            return;
        // Convert battery values into human readable text
        bool ok, ok2;
        uint newBatteryLevel = bComponents[0].toUInt(&ok), newBatteryStatus = bComponents[1].toUInt(&ok2);
        if(!ok || !ok2 || newBatteryStatus >= BatteryStatus::BATT_STATUS_INVALID || (batteryLevel == newBatteryLevel && batteryStatus == newBatteryStatus))
            return;
        batteryLevel = newBatteryLevel;
        batteryStatus = static_cast<BatteryStatus>(newBatteryStatus);
        emit batteryChanged(batteryLevel, batteryStatus);
        emit batteryChangedLed(batteryLevel, batteryStatus);
    } else if(components[0] == "i"){
        // Indicator event
        QString i = components[1];
        if(i.length() < 2)
            return;
        QString iName = i.mid(1);
        bool on = (i[0] == '+');
        if(iName == "num")
            iState[0] = on;
        else if(iName == "caps")
            iState[1] = on;
        else if(iName == "scroll")
            iState[2] = on;
    } else if(components[0] == "hwprofileid"){
        // Hardware profile ID
        if(components.count() < 3)
            return;
        // Find the hardware profile in the list of profiles
        QString guid = components[1];
        QString modified = components[2];
        KbProfile* newProfile = nullptr;
        foreach(KbProfile* profile, _profiles){
            if(profile->id().guid == QUuid::fromString(guid)){
                newProfile = profile;
                break;
            }
        }
        // If it wasn't found, create it
        if(!newProfile){
            newProfile = new KbProfile(this, getKeyMap(), guid, modified);
            hwLoading[0] = true;
            if(!k95ReplayingCache){
                cmd.write(QString("@%1 get :hwprofilename\n").arg(notifyNumber).toLatin1());
                cmd.flush();
            }
        } else {
            // If it's been updated, fetch its name
            if(newProfile->id().hwModifiedString() != modified){
                newProfile->id().modifiedString(modified);
                newProfile->id().hwModifiedString(modified);
                newProfile->setNeedsSave();
                if(hwLoading[0] && !k95ReplayingCache){
                    cmd.write(QString("@%1 get :hwprofilename\n").arg(notifyNumber).toLatin1());
                    cmd.flush();
                }
            } else {
                hwLoading[0] = false;
            }
        }
        hwProfile(newProfile);
        if(!k95ReplayingCache) emit profileAdded();
    } else if(components[0] == "hwprofilename"){
        // Hardware profile name
        QString name = QUrl::fromPercentEncoding(components[1].toUtf8());
        if(!_hwProfile || !hwLoading[0])
            return;
        QString oldName = _hwProfile->name();
        if(!(oldName.length() >= name.length() && oldName.left(name.length()) == name)){
            // Don't change the name if it's a truncated version of what we already have
            _hwProfile->name(name);
            if(!k95ReplayingCache) emit profileRenamed();
        }
    } else if(components[0] == "modecount"){
        // Number of software mode slots the daemon was configured with (see
        // --modecount on ckb-next-daemon). Comes back asynchronously in response
        // to the `get :modecount` sent on connect (see run()), so anything that
        // assumed DAEMON_MODE_COUNT_DEFAULT in the meantime needs to be patched
        // up once the real value is known.
        bool ok;
        int newCount = components[1].toInt(&ok);
        if(ok && newCount > 0 && newCount != daemonModeCount){
            daemonModeCount = newCount;
            if(_currentProfile) pushModeNames();
            cmd.flush();
            checkModeCountWarning();
            emit modeCountStatusChanged();
        }
    } else if(components[0] == "mode"){
        // Mode-specific data
        if(components.count() < 4)
            return;
        int mode = components[1].toInt() - 1;
        if(components[2] == "hwid"){
            if(components.count() < 5 || mode >= HWMODE_MAX || !_hwProfile)
                return;
            // Hardware mode ID
            QString guid = components[3];
            QString modified = components[4];
            // Look for this mode in the hardware profile
            KbMode* hwMode = nullptr;
            bool isUpdated = false;
            foreach(KbMode* kbMode, _hwProfile->modes()){
                if(kbMode->id().guid == QUuid::fromString(guid)){
                    hwMode = kbMode;
                    if(kbMode->id().hwModifiedString() != modified){
                        // Update modification time
                        hwMode->id().modifiedString(modified);
                        hwMode->id().hwModifiedString(modified);
                        hwMode->setNeedsSave();
                        isUpdated = true;
                    } else {
                        hwLoading[mode + 1] = false;
                    }
                    break;
                }
            }
            // If it wasn't found, add it
            if(!hwMode){
                isUpdated = true;
                hwMode = new KbMode(this, getKeyMap(), guid, modified);
                _hwProfile->append(hwMode);
                // If the hardware profile now contains enough modes to be added to the list, do so
                if(!_profiles.contains(_hwProfile) && _hwProfile->modeCount() >= hwModeCount){
                    _profiles.append(_hwProfile);
                    _needsSave = true;
                    if(!k95ReplayingCache) emit profileAdded();
                    if(!_currentProfile && !k95ReplayingCache)
                        setCurrentProfile(_hwProfile);
                }
            }
            if(hwLoading[mode + 1] && isUpdated){
                // If the mode isn't in the right place, move it
                int index = _hwProfile->indexOf(hwMode);
                if(mode < _hwProfile->modeCount() && index != mode)
                    _hwProfile->move(index, mode);
                // Fetch the updated data
                if(!k95ReplayingCache){
                    cmd.write(QString("@%1 mode %2 get :hwname :hwrgb").arg(notifyNumber).arg(mode + 1).toLatin1());
                    if(isMouse())
                        cmd.write(" :hwdpi :hwdpisel :hwlift :hwsnap");
                    cmd.write("\n");
                    cmd.flush();
                }
            }
        } else if(components[2] == "hwname"){
            // Mode name - update list
            if(!_hwProfile || _hwProfile->modeCount() <= mode || mode >= HWMODE_MAX || !hwLoading[mode + 1])
                return;
            KbMode* hwMode = _hwProfile->modes()[mode];
            QString name = QUrl::fromPercentEncoding(components[3].toUtf8());
            QString oldName = hwMode->name();
            if(!(oldName.length() >= name.length() && oldName.left(name.length()) == name)){
                // Don't change the name if it's a truncated version of what we already have
                hwMode->name(name);
            }
        } else if(components[2] == "hwrgb"){
            // RGB - set mode lighting
            if(!_hwProfile || _hwProfile->modeCount() <= mode || mode >= HWMODE_MAX || !hwLoading[mode + 1])
                return;
            KbMode* kbmode = _hwProfile->modes()[mode];
            KbLight* light = kbmode->light();
            // Scan the input for colors
            QColor lightColor = QColor();
            for(int i = 3; i < components.count(); i++){
                QString comp = components[i];
                if(comp.indexOf(":") < 0){
                    // No ":" - single hex constant
                    bool ok;
                    int rgb = comp.toInt(&ok, 16);
                    if(ok)
                        light->color(QColor::fromRgb((QRgb)rgb));
                } else {
                    // List of keys ("a,b:xxxxxx"). Parse color first
                    QStringList set = comp.split(":");
                    bool ok;
                    int rgb = set[1].toInt(&ok, 16);
                    if(ok){
                        QColor color = QColor::fromRgb((QRgb)rgb);
                        // Parse keys
                        QStringList keys = set[0].split(",");
                        foreach(QString key, keys){
                            if(key == "light" && _model != KeyMap::K95P)
                                // Extrapolate the Light key to the M-keys and Lock key, since those will be set to black on hwsave
                                lightColor = color;
                            if(key.startsWith("dpi") && key.length() > 3){
                                // DPI levels go to the KbPerf object instead of KbLight
                                int index = key.mid(3).toInt(&ok);
                                if(ok)
                                    kbmode->perf()->dpiColor(index, color);
                                continue;
                            }
                            light->color(key, color);
                        }
                    }
                }
            }
            if(lightColor.isValid()){
                light->color("mr", lightColor);
                light->color("m1", lightColor);
                light->color("m2", lightColor);
                if(!(this->model() == KeyMap::K70MK2 || this->model() == KeyMap::STRAFE_MK2))
                    light->color("m3", lightColor);
                light->color("lock", lightColor);
            }
        } else if(components[2] == "hwdpi"){
            // DPI settings
            if(!_hwProfile || _hwProfile->modeCount() <= mode || mode >= HWMODE_MAX || !hwLoading[mode + 1])
                return;
            KbPerf* perf = _hwProfile->modes()[mode]->perf();
            // Read the rest of the line as stage:x,y
            foreach(QString comp, components.mid(3)){
                QStringList dpi = comp.split(':');
                if(dpi.length() != 2)
                    continue;
                QStringList xy = dpi[1].split(',');
                int x = 0, y = 0;
                bool off = false;
                if(xy.length() < 2){
                    // If the right side only has one parameter, set both X and Y
                    if(xy[0] == "off")
                        off = true;
                    else
                        x = y = xy[0].toInt();
                } else {
                    x = xy[0].toInt();
                    y = xy[1].toInt();
                }
                // Set DPI for this stage
                int index = dpi[0].toInt();
                if(off){
                    perf->dpiEnabled(index, false);
                    // If all DPIs have been disabled, turn them back on
                    bool allOff = true;
                    for(int i = 1; i < KbPerf::DPI_COUNT; i++){
                        if(perf->dpiEnabled(i)){
                            allOff = false;
                            break;
                        }
                    }
                    if(allOff){
                        for(int i = 1; i < KbPerf::DPI_COUNT; i++)
                            perf->dpiEnabled(i, true);
                    }
                } else {
                    perf->dpiEnabled(index, true);
                    perf->dpi(index, QPoint(x, y));
                }
            }
        } else if(components[2] == "hwdpisel"){
            // Hardware DPI selection (0...5)
            if(!_hwProfile || _hwProfile->modeCount() <= mode || mode >= HWMODE_MAX || !hwLoading[mode + 1])
                return;
            KbPerf* perf = _hwProfile->modes()[mode]->perf();
            int idx = components[3].toInt();
            if(idx < 1)
                idx = 1;
            if(idx >= KbPerf::DPI_COUNT)
                idx = KbPerf::DPI_COUNT - 1;
            perf->baseDpiIdx(idx);
        } else if(components[2] == "hwlift"){
            // Mouse lift height (1...5)
            if(!_hwProfile || _hwProfile->modeCount() <= mode || mode >= HWMODE_MAX || !hwLoading[mode + 1])
                return;
            KbPerf* perf = _hwProfile->modes()[mode]->perf();
            perf->liftHeight((KbPerf::height)components[3].toInt());
        } else if(components[3] == "hwsnap"){
            // Mouse angle snapping ("on" or "off")
            if(!_hwProfile || _hwProfile->modeCount() <= mode || mode >= HWMODE_MAX || !hwLoading[mode + 1])
                return;
            KbPerf* perf = _hwProfile->modes()[mode]->perf();
            perf->angleSnap(components[3] == "on");
        }
    } else if(components[0] == "fwupdate"){
        // Firmware update progress
        if(components.count() < 3)
            return;
        // Make sure path is the same
        if(components[1] != fwUpdPath)
            return;
        QString res = components[2];
        if(res == "invalid" || res == "fail")
            emit fwUpdateFinished(false);
        else if(res == "ok")
            emit fwUpdateFinished(true);
        else {
            // "xx/yy" indicates progress
            if(!res.contains("/"))
                return;
            QStringList numbers = res.split("/");
            emit fwUpdateProgress(numbers[0].toInt(), numbers[1].toInt());
        }
    }
}

KeyMap Kb::getKeyMap(){
    return KeyMap(_model, _layout);
}

void Kb::setCurrentProfile(KbProfile* profile){
    if(_hwFlowBusy || k95Rec) return;
    while(profile->modeCount() < minimumModes(profile))
        profile->append(new KbMode(this, getKeyMap()));

    emit profileAboutToChange();
    _currentProfile = profile;
    if(_k95HwSlots && k95Decided){
        // hwslot1: the hardware profile is the keyboard's hardware mode
        if(isHwSlotProfile(profile) && !k95InHw)
            enterK95Hw();
        else if(!isHwSlotProfile(profile) && k95InHw){
            k95InHw = false;
            cmd.write("active\n");
            cmd.flush();
        }
    }
    if(!isHwSlotProfile(profile))
        k95DropQueuedReads();   // (the one being read goes on)
    emit profileChanged();
    // Hack to prevent crash when switching to HW mode on first start with no config file.
    // It happens when called by KbWidget::on_profileBox_activated().
    // The KbWidget event will re-call this after the currentMode has properly been set.
    if(profile->currentMode())
        setCurrentMode(profile->currentMode());
}

void Kb::setCurrentMode(KbMode* mode){
    if(_hwFlowBusy || k95Rec) return;
    _currentProfile->currentMode(_currentMode = mode);
    _needsSave = true;

    if(features.contains("battery") && this->currentPerf())
        connect(this, &Kb::batteryChangedLed, this->currentPerf(), &KbPerf::setBattery);
    emit modeChanged();
    mode->light()->forceFrameUpdate();
    k95WantSlot(true);   // the user opened it: a failed read is tried again
}

KbProfile* Kb::newProfileWithBlankMode(){
    KbProfile* p = new KbProfile(this, getKeyMap());
    KbMode* m = newMode();
    p->append(m);
    p->currentMode(m);
    return p;
}

KeyMap::Layout Kb::getCurrentLayout(){
    return _layout;
}

void Kb::setPollRate(const QString& poll){
    if(_hwFlowBusy){ deferredPollRate = poll; return; }
    // The daemon ignores it in hardware mode
    if(k95Quiet()) return;
    cmd.write(QString("\npollrate %1\n").arg(poll).toLatin1());
}

bool Kb::k95StartRecording(){
    if(!_k95HwSlots || k95Rec || _hwFlowBusy || !k95CacheReady || !k95InHw || !isHwSlotProfile(_currentProfile) ||
       !cmd.isOpen() || macroNumber <= 0 || k95ReadSlot >= 0)
        return false;
    k95Rec = true;
    // One line: software mode, then the daemon's profile cleared (it may still hold a software profile of an earlier session); the
    // keys light grey, not off, so that they can be seen in a dark room (not white, not to dazzle)
    cmd.write(QString("active eraseprofile\nrgb 999999\nnotifyon %1\n@%1 notify all:on\n").arg(macroNumber).toLatin1());
    cmd.flush();
    emit k95RecordingChanged(true);
    return true;
}

void Kb::k95StopRecording(){
    if(!k95Rec)
        return;
    k95Rec = false;
    cmd.write(QString("@%1 notify all:off\nnotifyoff %1\neraseprofile idle\n").arg(macroNumber).toLatin1());
    cmd.flush();
    emit k95RecordingChanged(false);
    k95NextRead();   // (a read queued meanwhile)
}

void Kb::k95WantSlot(bool retry){
    if(!_k95HwSlots || !k95CacheReady || !isHwSlotProfile(_currentProfile) || !_currentMode)
        return;
    k95QueueRead(_currentProfile->indexOf(_currentMode), retry);
    k95NextRead();
}

void Kb::k95QueueRead(int i, bool retry){
    // A failed read is tried again only when the user opens the mode again (never on its own: a read that fails the same way would
    // go round without end), or copies to it
    if(i >= 0 && i < 3 && (k95Read[size_t(i)] == K95_UNREAD || (retry && k95Read[size_t(i)] == K95_FAILED))){
        k95Read[size_t(i)] = K95_QUEUED;
        k95ReadProgress[size_t(i)] = 0;
        k95ReadWhy[size_t(i)].clear();
        k95ReadQueue.push_back(i);
        emit k95SlotReadChanged(i);
    }
}

void Kb::k95NextRead(){
    if(k95ReadSlot >= 0 || _hwFlowBusy || k95Rec || k95ReadQueue.empty() || !cmd.isOpen() || notifyNumber < 1)
        return;
    // One load of the cache at a time: the attach, a save's read-back or another slot's record goes first (a failed one until
    // the end of its lines). The record of a read whose guard ran out does not: it stopped, and the next read replaces it
    if(k95CacheBatch && !k95BatchDiscard && k95CacheBatch->state() != HwCacheBatch::Complete && !k95CacheBatch->failureDrained())
        return;
    if(!isHwSlotProfile(_currentProfile)){
        k95DropQueuedReads();
        return;
    }
    const int slot = k95ReadQueue.front();
    k95ReadQueue.erase(k95ReadQueue.begin());
    k95ReadSlot = slot;
    k95Read[size_t(slot)] = K95_READING;
    k95ReadProgress[size_t(slot)] = 0;
    cmd.write(QString("@%1 hwslot read:%2\n").arg(notifyNumber).arg(slot + 1).toLatin1());
    cmd.flush();
    if(k95ReadTimer)
        k95ReadTimer->start(k95ReadGuardMs);
    emit k95SlotReadChanged(slot);
}

void Kb::k95ReadLine(const QStringList& components, const QString& line){
    // mode <m> hwread progress <done> 1000 read | ok gen=<g> packets=<n> | fail err=<text>
    bool ok = false;
    const int slot = components[1].toInt(&ok) - 1;
    if(!ok || slot != k95ReadSlot || k95ReadBatchSlot == slot || components.count() < 4)
        return;   // late, of a read that ended, or out of order
    const QString& what = components[3];
    if(what == "progress"){
        bool a = false, b = false;
        const unsigned done = components.count() == 7 ? components[4].toUInt(&a) : 0;
        if(!a || components[5].toUInt(&b) != 1000 || !b || components[6] != "read" || done > 1000 ||
           done < k95ReadProgress[size_t(slot)])
            return;
        k95ReadProgress[size_t(slot)] = done;
        if(k95ReadTimer)
            k95ReadTimer->start(k95ReadGuardMs);
        emit k95SlotReadChanged(slot);
    } else if(what == "ok"){
        // The daemon has the slot: its record, by the pages of a load of that slot alone
        k95CacheFailureNotified = false;
        k95BatchDiscard = false;
        k95ReadBatchSlot = slot;
        k95CacheBatch.reset(new HwCacheBatch(1u << unsigned(slot), 0, false, k95CacheKnown));
        k95CacheBatch->requestBindings();
        const std::string request = k95CacheBatch->identityRequest(unsigned(notifyNumber));
        cmd.write(QByteArray(request.data(), int(request.size())));
        cmd.flush();
        k95ReadProgress[size_t(slot)] = 1000;
        if(k95ReadTimer)
            k95ReadTimer->start(k95ReadGuardMs);
        emit k95SlotReadChanged(slot);
    } else if(what == "fail"){
        const int at = line.indexOf(" err=");
        k95ReadEnd(slot, false, at < 0 ? tr("the daemon could not read it") : line.mid(at + 5).trimmed());
    }
}

void Kb::k95ReadEnd(int slot, bool ok, const QString& why){
    if(slot < 0 || slot > 2)
        return;
    k95Read[size_t(slot)] = ok ? K95_READ : K95_FAILED;
    k95ReadWhy[size_t(slot)] = why;
    if(k95PendingCopy[size_t(slot)]){
        // The copy that waited for this read: onto the draft the record just made
        k95PendingCopy[size_t(slot)] = false;
        if(ok)
            k95ApplyPerf(slot, k95PendingPerf[size_t(slot)]);
        else
            k95NoteLater(tr("M%1 could not be read from the keyboard (%2): the performance settings were not copied to it.")
                         .arg(slot + 1).arg(why));
    }
    if(slot == k95ReadSlot){
        k95ReadSlot = -1;
        if(k95ReadTimer)
            k95ReadTimer->stop();
    }
    if(!ok && k95ReadBatchSlot == slot){
        // Its record stopped half way (the guard ran out): what comes of it later is dropped, and the next read loads it anew
        k95ReadBatchSlot = -1;
        k95BatchDiscard = true;
    }
    if(!ok)
        qWarning() << "K95P: slot" << slot + 1 << "could not be read:" << why;
    emit k95SlotReadChanged(slot);
    k95NextRead();
}

void Kb::k95DropQueuedReads(){
    const std::vector<int> queued = k95ReadQueue;
    k95ReadQueue.clear();
    for(int i : queued){
        k95Read[size_t(i)] = K95_UNREAD;
        if(k95PendingCopy[size_t(i)]){
            k95PendingCopy[size_t(i)] = false;
            k95NoteLater(tr("M%1 was not read from the keyboard (the hardware profile was left first): the performance settings were "
                            "not copied to it.").arg(i + 1));
        }
        emit k95SlotReadChanged(i);
    }
}

bool Kb::beginHwFlow(){
    if(k95Rec || k95ReadSlot >= 0) return false;
    if(!k95FirmwareTested() || !k95CacheReady || _hwFlowBusy || !cmd.isOpen() ||
       !cmd.flush() || cmd.bytesToWrite() != 0)
        return false;
    _hwFlowBusy = true;
    return true;
}

bool Kb::startK95CacheRefresh(unsigned mask, unsigned rgbRequired, std::string& request){
    request.clear();
    // (a record of a read on demand abandoned by its guard is replaced too: it stopped)
    if(!_hwFlowBusy || !k95CacheReady || !k95CacheBatch ||
       (k95CacheBatch->state() != HwCacheBatch::Complete && !k95BatchDiscard) || !mask || (mask & ~7u) ||
       (rgbRequired & ~mask)) return false;
    // hwsave infers a black picture from a missing :hwrgb; hwslot1 has it in the record (lt:empty): a slot whose effects were
    // kept has no :hwrgb either, and is not black
    k95CacheBlackMask = _k95HwSlots ? 0 : mask & ~rgbRequired;
    k95CacheFailureNotified = false;
    k95ReadBatchSlot = -1;
    k95BatchDiscard = false;
    k95CacheBatch.reset(new HwCacheBatch(mask, rgbRequired, true, k95CacheKnown));
    if(_k95HwSlots)
        k95CacheBatch->requestBindings();
    request = k95CacheBatch->identityRequest(unsigned(notifyNumber));
    return !request.empty();
}

HwSavePipe::Result Kb::sendHwFlowLine(const std::string& line){
    if(!_hwFlowBusy || !cmd.isOpen()) return {HwSavePipe::Invalid, 0};
    return HwSavePipe::send_line(cmd.handle(), HwSavePipe::with_notification(unsigned(notifyNumber), line));
}

void Kb::endHwFlow(){
    if(!_hwFlowBusy) return;
    _hwFlowBusy = false;
    cmd.write(QString("fps %1\ndither %2\n").arg(_frameRate).arg(static_cast<int>(_dither)).toLatin1());
#ifdef Q_OS_MACOS
    cmd.write(QString("layout %1\naccel %2\nscrollspeed %3\n")
                  .arg(KeyMap::isISO(_layout) ? "iso" : "ansi")
                  .arg(_mouseAccel ? "on" : "off").arg(_scrollSpeed).toLatin1());
#endif
    if(!deferredPollRate.isEmpty()){
        cmd.write(QString("pollrate %1\n").arg(deferredPollRate).toLatin1());
        deferredPollRate.clear();
    }
    cmd.flush();
    if(_currentMode) _currentMode->light()->forceFrameUpdate();
    k95WantSlot();   // the reads that waited for the save, and the mode shown if it is not read
}
