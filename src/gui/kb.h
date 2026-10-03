#ifndef KB_H
#define KB_H

#include <QtGlobal>
#include <QObject>
#include <QFile>
#include <QThread>
#include <QTimer>

#include "kbprofile.h"
#include <QElapsedTimer>
#include <limits>
#include "batterysystemtrayicon.h"
#include "ckbversionnumber.h"
#include "hwsavepipe.h"
#include "hwcachebatch.h"
#include "hwslotdraft.h"
#include "hwsaveflow.h"
#include <array>
#include <ckbnextconfig.h>
#include <memory>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringView>
#endif

struct firmware_t {
    CkbVersionNumber app;
    CkbVersionNumber bld;
    CkbVersionNumber radioapp;
    CkbVersionNumber radiobld;
    void parse(const QString& str){
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QStringView view(str);
        view.truncate(view.size() - 1);
        // KeepEmptyParts is the default
        QList<QStringView> split = view.split(QChar('\n'));
#else
        // KeepEmptyParts is the default
        QVector<QStringRef> split = str.leftRef(str.size() - 1).split(QChar('\n'));
#endif
        // Old < 0.5.0 format (NXP and legacy)
        if(split.size() == 1){
            app = CkbVersionNumber(split.at(0).toString());
        } else if (split.size() >= 4) {
            app = CkbVersionNumber(split.at(0).toString());
            bld = CkbVersionNumber(split.at(1).toString());
            radioapp = CkbVersionNumber(split.at(2).toString());
            radiobld = CkbVersionNumber(split.at(3).toString());
        }
    }
};

// Class for managing devices
class Kb : public QThread
{
    Q_OBJECT
public:
    // USB model and serial number
    QString usbModel, usbSerial;
#ifdef WITH_ENV_VARS
    // Unstripped USB iProduct string, as reported by the device (for CKBNEXT_IPRODUCT)
    QString usbProductRaw;
    // Public accessor to the (otherwise private) devnode path, for CKBNEXT_DEVPATH
    inline QString devicePath() const { return devpath; }
#endif
    // Device information
    QStringList features;

    enum pollrate_t {
        POLLRATE_UNKNOWN = -1,
        POLLRATE_8MS,
        POLLRATE_4MS,
        POLLRATE_2MS,
        POLLRATE_1MS,
        POLLRATE_05MS,
        POLLRATE_025MS,
        POLLRATE_01MS,
        POLLRATE_COUNT,
    };
    pollrate_t pollrate, maxpollrate;

    bool monochrome;
    ushort productID;
    bool hwload;
    bool adjrate;
    firmware_t firmware;

    // Keyboard model
    inline KeyMap::Model    model() const                       { return _model; }
    // Hardware profiles: the daemon can load them ("hwload") and the GUI may offer to save to them. The K95 RGB Platinum saves
    // only through hwslot1 (a tested firmware, the cache loaded, no flow running): with a daemon without it the GUI
    // saves nothing (the hwsave route is closed for it)
    bool                    hwSaveAllowed() const               { return hwload && (_model != KeyMap::K95P ||
                                                              (_k95HwSlots && k95FirmwareTested() && k95CacheReady && !_hwFlowBusy &&
                                                               k95ReadSlot < 0)); }
    bool                    isKeyboard() const                  { return KeyMap::isKeyboard(_model); }
    bool                    isMouse() const                     { return KeyMap::isMouse(_model); }
    bool                    isMousepad() const                  { return KeyMap::isMousepad(_model); }
    bool                    isHeadsetStand() const              { return KeyMap::isHeadsetStand(_model); }

    // Frame rate (all devices). Also updates the event timer in KbManager.
    static inline int               frameRate()                         { return _frameRate; }
    static void                     frameRate(int newFrameRate);
    // Layout
    inline KeyMap::Layout           layout()                            { return _layout; }
    void                            layout(KeyMap::Layout newLayout, bool stop);
    // Layout string as reported by the daemon
    QString hwlayout;
    // Whether dithering is used (all devices)
    static inline bool              dither()                            { return _dither; }
    static void                     dither(bool newDither);
    // OSX: mouse acceleration toggle (all devices)
    static inline bool              mouseAccel()                        { return _mouseAccel; }
    static void                     mouseAccel(bool newAccel);
    // OSX: scroll speed (-1 = use acceleration)
    static inline int               scrollSpeed()                       { return _scrollSpeed; }
    static void                     scrollSpeed(int newSpeed);

    // Profile saved to hardware
    inline KbProfile*   hwProfile() { return _hwProfile; }
    void                hwProfile(KbProfile* newHwProfile);
    // Required hardware modes
    int hwModeCount;
    const static int HWMODE_MAX = 3;

    // Number of software mode slots the connected daemon was configured with
    // (--modecount, learned via `get :modecount` on connect). Assumed to be
    // DAEMON_MODE_COUNT_DEFAULT until the real value is known.
    int daemonModeCount;
    const static int DAEMON_MODE_COUNT_DEFAULT = 6;

    // Perform a firmware update
    void fwUpdate(const QString& path);

    // Currently-selected profile
    inline KbProfile* currentProfile() { return _currentProfile; }
    // Profile list
    inline const QList<KbProfile*>& profiles() const                                { return _profiles; }
    void                            profiles(const QList<KbProfile*>& newProfiles)  { _needsSave = true; _profiles = newProfiles; }
    void                            appendProfile(KbProfile* newProfile)            { _needsSave = true; _profiles.append(newProfile); }
    inline int                      indexOf(KbProfile* profile)                     { return _profiles.indexOf(profile); }
    inline KbProfile*               find(const QUuid& id)                           { foreach(KbProfile* profile, _profiles) { if(profile->id().guid == id) return profile; } return nullptr; }

    // Currently-selected mode
    inline KbMode*  currentMode()   { return _currentMode; }
    inline KbLight* currentLight()  { return _currentMode ? _currentMode->light() : nullptr; }
    inline KbBind*  currentBind()   { return _currentMode ? _currentMode->bind() : nullptr; }
    inline KbPerf*  currentPerf()   { return _currentMode ? _currentMode->perf() : nullptr; }

    // Update selection
    void        setCurrentProfile(KbProfile* profile);
    void        setCurrentMode(KbMode* mode);

    // Create a new profile/mode. The newly-created object will NOT be inserted into the current profile/mode list.
    KbProfile*   newProfileWithBlankMode();
    inline KbProfile*   newProfile(KbProfile* other)                  { return new KbProfile(this, getKeyMap(), *other); }
    inline KbProfile*   newProfile(CkbExternalSettings* settings, QString guid) { return new KbProfile(this, getKeyMap(), *settings, guid); }
    inline KbMode*      newMode()                                     { return new KbMode(this, getKeyMap()); }
    inline KbMode*      newMode(KbMode* other)                        { return new KbMode(this, getKeyMap(), *other); }

    // Load/save stored settings
    void load();
    void save();
    bool needsSave() const;

    void hwSave();
    bool beginHwFlow();
    void endHwFlow(); // only after a terminal notification (and refresh marker, if requested)
    bool hwFlowBusy() const { return _hwFlowBusy; }
    HwSavePipe::Result sendHwFlowLine(const std::string& line);
    bool startK95CacheRefresh(unsigned mask, unsigned rgbRequired, std::string& request);
    bool k95CacheIsReady() const { return k95CacheReady; }
    // A K95 RGB Platinum whose daemon reads its on-board profiles ("hwload", or "hwslot1"): it has its own way to load and save
    // them, below. Without either (the daemon was started without --enable-experimental, or the firmware is not a tested one)
    // it is handled as every other keyboard, and a hardware profile made of its slots before is kept hidden (load(), save()).
    bool k95Onboard() const { return _model == KeyMap::K95P && (hwload || _k95HwSlots); }
    // The GUID is the one of the hardware profile of the slots, shown or hidden: not to be imported from a file
    bool isK95HwGuid(const QUuid& guid) const {
        return (_k95HwSlots && _hwProfile && _hwProfile->id().guid == guid) || (k95HiddenHwProfile && k95HiddenHwProfile->id().guid == guid);
    }
    bool k95FirmwareTested() const {
        return _model == KeyMap::K95P && productID == 0x1b2d &&
               firmware.app == CkbVersionNumber(QStringLiteral("3.29")) &&
               firmware.bld == CkbVersionNumber(QStringLiteral("3.03"));
    }
    void invalidateK95Cache() { k95CacheReady = false; }

    // hwslot1: a K95 RGB Platinum whose daemon offers "hwslot1" (a tested firmware and a daemon that reads, checks and saves the
    // bindings of the slots). Its hardware profile is then exactly the three on-board slots, in the order of the firmware, kept
    // apart from the software profiles: no mode is added to it, deleted or moved, and the profile is not renamed, duplicated or
    // deleted. Without the token all of these give what they give for any other keyboard.
    bool k95HwSlots() const { return _k95HwSlots; }
    bool isHwSlotProfile(const KbProfile* profile) const { return _k95HwSlots && profile && profile == _hwProfile; }
    bool canAddMode(const KbProfile* profile) const { return profile && !isHwSlotProfile(profile); }
    bool canMoveModes(const KbProfile* profile) const { return profile && !isHwSlotProfile(profile); }
    bool canDeleteMode(const KbProfile* profile) const;
    bool canManageProfile(const KbProfile* profile) const { return profile && !isHwSlotProfile(profile); }
    // The modes a software profile always has: hwModeCount without hwslot1, one with it
    int minimumModes(const KbProfile* profile) const { return !_k95HwSlots || isHwSlotProfile(profile) ? hwModeCount : 1; }
    bool k95HwMigrated() const { return k95Migrated; }
    // hwslot1: with the hardware profile selected the keyboard is in hardware mode (the daemon was sent "idle"): the GUI sends
    // it no frames, bindings or settings. Which mode it starts in is decided when the first cache load ends (or fails, or
    // after k95DecisionTimeoutMs); until then the GUI sends neither "active" nor "idle".
    bool k95InHardwareMode() const { return k95InHw; }
    bool k95ModeDecided() const { return k95Decided; }
    static int k95DecisionTimeoutMs;
    // hwslot1: the record of slot i (0..2) the daemon last read (the base), nullptr when there is none; the draft of the
    // slot (its bindings; its name and colours are those of the mode of the hardware profile), nullptr without hwslot1
    const HwBinding::Record* k95SlotRecord(int i) const;
    HwSlotDraft::Draft* k95Draft(int i);
    // Whether the draft differs from its base (against the base itself when it is known, else as last known)
    bool k95DraftModified(int i);
    // The editor changed the draft of slot i
    void k95DraftChanged(int i);
    // After k95DraftConflict: reload the slot (the changes go) or keep the changes on the new base
    void k95ResolveConflict(int i, bool reload);
    // The next record of these slots is the read-back of our own save: it replaces the drafts, and it must hold what they
    // held (else the cache is not ready: no further save until the keyboard is read again)
    void k95CommitOnRefresh(unsigned mask) { k95CommitMask |= mask & 7u; }
    // Whether the mode's colours of slot i differ from its base's (they count only where they are an image: static layers or
    // none)
    bool k95LightsChanged(int i) const;
    // The slots of a save of the hardware profile (hwslot1): all three, or the one of index selected, each the lines of its
    // transaction; rgbRequired says which of them have lighting to read back after a save. false, with why, when one cannot
    // be prepared.
    bool k95SaveSlots(bool all, int selected, std::vector<HwSaveFlow::Slot>& saveSlots, std::array<bool, 3>& rgbRequired,
                      QString& why);
    // The macro recorder of the hardware editor: the keyboard goes to software mode with the daemon's profile cleared (so the
    // physical keys are recorded, not the slot's remaps) and sends its key events to the macro node, until
    // k95StopRecording() (also when the GUI closes or the device goes) takes it back to hardware mode. Meanwhile the profile, the
    // mode and a save cannot change. Only from the hardware profile, with the cache loaded and no save running.
    // Special slots: an empty one (a save makes a new profile in it), bindings recreated from scratch on a slot whose bindings
    // are not a model, static colours instead of a slot's lighting effects; the name of a new profile is iCUE's ("HW Profile N")
    bool k95SlotEmpty(int i) const;
    QString k95BaseName(int i) const;
    void k95Recreate(int i, bool on);
    void k95ReplaceLights(int i, bool on);
    // The performance settings of slot i: the draft's, else the slot's, else iCUE's defaults; set by the Performance HW tab
    HwSlotDraft::Perf k95Perf(int i) const;
    void k95SetPerf(int i, const HwSlotDraft::Perf& perf);
    void k95RestorePerfDefaults(int i);
    // Copies the performance settings of slot from to the slots to (hardware slots): at once to one that was read, after its read to
    // one that was not (it is queued); the Win Lock bits iCUE never writes (4..7) stay the target's. Nothing is saved while one waits
    void k95CopyPerf(int from, const std::vector<int>& to);
    bool k95CopyPending() const { return k95PendingCopy[0] || k95PendingCopy[1] || k95PendingCopy[2]; }
    bool k95CopyPendingFor(int i) const { return i >= 0 && i < 3 && k95PendingCopy[size_t(i)]; }
    bool k95StartRecording();
    void k95StopRecording();
    bool k95Recording() const { return k95Rec; }
    // The read of a slot on demand: the attach gives the names only; the bindings and the lighting of a slot are read
    // when its mode is opened in the hardware profile (hwslot read:), one slot at a time, in the order the modes were opened. A slot
    // is Reading until its record has come (the pages after the daemon's "ok"); while one is, Save and the recorder are off.
    enum K95Read { K95_UNREAD, K95_QUEUED, K95_READING, K95_READ, K95_FAILED };
    K95Read k95ReadState(int i) const { return i >= 0 && i < 3 ? k95Read[size_t(i)] : K95_UNREAD; }
    unsigned k95ReadDone(int i) const { return i >= 0 && i < 3 ? k95ReadProgress[size_t(i)] : 0; }   // 0..1000
    QString k95ReadError(int i) const { return i >= 0 && i < 3 ? k95ReadWhy[size_t(i)] : QString(); }
    bool k95Reading() const { return k95ReadSlot >= 0; }
    static int k95ReadGuardMs;   // no line of a read for this long: it failed (30 s; the tests make it shorter)

    KeyMap::Layout getCurrentLayout();

    // Battery polling timer
    QTimer* batteryTimer;

    // Battery status icon
    BatteryStatusTrayIcon* batteryIcon;

    bool showBatteryIndicator;

    //////////
    /// For usage with macro definions, these two params must only be readable.
    /// So there are no setters.
    /// \brief getMacroNumber returns the macroNumber, which we have saved in the constructor.
    /// For usage with macro definions, this param must only be readable.
    /// So there is no setter.
    /// \return The Number is returned as int.
    ///
    inline int getMacroNumber () { return macroNumber; }

    ///
    /// \brief getMacroPath returns the macroPath (e.g. /dev/input/ckb1/notify),
    /// which we have saved in the constructor.
    /// For usage with macro definions, this param must only be readable.
    /// So there is no setter.
    /// \return The absolute path as String
    ///
    inline QString getMacroPath () { return macroPath; }

    inline ushort getMaxDpi () {return _maxDpi; }
    void setPollRate(const QString& poll);

    // The valid check is done because we don't start the timer on purpose in the constructor.
    // This is done so that when a new device is plugged in while the lights are off, it doesn't suddenly return a really low value and wake everything up.
    inline qint64 getDeviceIdleTime() const { return (deviceIdleTimer.isValid() ? deviceIdleTimer.elapsed() : std::numeric_limits<qint64>::max()); }

    ~Kb();

signals:
    // Layout/model updated
    void infoUpdated();

    // Profile/mode updates
    void profileAdded();
    void hwSaveLine(const QString& line);
    void k95CacheRequest(const QString& line);
    void k95CacheFinished(bool complete);
    // hwslot1: the slot changed on the keyboard under a draft with changes of its own (k95ResolveConflict)
    void k95DraftConflict(int slot);
    void k95DraftUpdated(int slot);
    void k95RecordingChanged(bool recording);
    // The read of slot on demand changed state or progress (k95ReadState)
    void k95SlotReadChanged(int slot);
    // Something the user should know that is not an answer to a click (a copy that could not be made): from the event loop, never
    // inside the reading of the notifications
    void k95Note(const QString& text);
    // A line of a cache load of a save's read-back was accepted (HwSaveFlow::refresh_alive)
    void k95CacheLineAccepted();
    void profileRenamed();
    void batteryChanged(uint batteryLevel, BatteryStatus batteryStatus);
    void batteryChangedLed(uint batteryLevel, BatteryStatus batteryStatus);

    void profileChanged();
    void profileAboutToChange();
    void modeChanged();

    // Emitted when the current profile has more modes than the daemon has slots
    // for (see daemonModeCount) - the extra modes can't be synced/animated correctly.
    void modeCountExceeded(int loadedModes, int daemonModes);
    // Emitted whenever something that affects whether a profile is over the
    // daemon's mode limit changes: daemonModeCount itself, or the current
    // profile's over/under status. Profile pickers should recheck their coloring.
    void modeCountStatusChanged();

    // FW update status
    void fwUpdateProgress(int current, int total);
    void fwUpdateFinished(bool succeeded);

public slots:
    // Send lighting and settings to the driver
    void frameUpdate();

    // Auto-save every 15s (if settings have changed, and no other writes are in progress)
    void autoSave();

private slots:
    // Processes lines read from the notification node
    void readNotify(const QString& line);

    void deleteHw();
    void deletePrevious();
    void updateBattery();

private:
    // Following methods should only be used by KbManager
    friend class KbManager;
    friend struct K95KbFixture; // offline GUI/device simulation only

    // Creates a keyboard object with the given device path
    Kb(QObject *parent, const QString& path);

    inline bool isOpen() const { return cmd.isOpen(); }

    //////////
    /// \brief pathVars
    /// devpath is the device root path (e.g. /dev/device/ckb1),
    /// cmdpath leads to the daemon input pipe for daemon commands,
    /// notifyPath is the standard input monitor for general purpose,
    /// macroPath added for a second thread to read macro input.
    /// layoutPath is used to get the physical layout from the daemon
    QString devpath, cmdpath, notifyPath, macroPath;
    // Is this the keyboard at the given serial/path?
    inline bool matches(const QString& path, const QString& serial) { return path.trimmed() == devpath.trimmed() && usbSerial == serial.trimmed().toUpper(); }

private:
    // Following properties shouldn't be used by any other classes
    void updateLayout(bool stop);

    static int _frameRate, _scrollSpeed;
    static bool _dither, _mouseAccel;

    KbProfile*          _currentProfile;
    QList<KbProfile*>   _profiles;
    KbMode*             _currentMode;

    KeyMap::Model   _model;

    uint batteryLevel;
    BatteryStatus batteryStatus;

    // Indicator light state
    bool iState[KbPerf::HW_I_COUNT];

    // CkbSettings path
    QString prefsPath;

    // Current firmware update file
    QString fwUpdPath;

    KbProfile*  _hwProfile;
    // Previously-selected profile and mode
    KbProfile*  prevProfile;
    KbMode*     prevMode;
    // Used to write the profile info when switching
    bool writeProfileHeader();
    // Sends each mode's name to the daemon's in-memory profile
    void pushModeNames();
    void applyK95CacheBatch();
    // Emits modeCountExceeded() if the current profile has more modes than the
    // daemon supports, at most once per profile instance while it stays over
    KbProfile* modeCountWarnedProfile = nullptr;
    void checkModeCountWarning();

    // cmd and notify file handles
    QFile cmd;

    /// \brief notifyNumber is the trailing number in the device path.
    int notifyNumber;
    // Macro Numer to notify macro definition events
    int macroNumber;

    // Needs to be saved?
    bool _needsSave;

    KeyMap::Layout _layout;

    ushort _maxDpi;

    // Whether or not the hardware profile is being loaded
    // (0 = profile, 1...3 = modes)
    bool hwLoading[HWMODE_MAX + 1];
    bool _hwFlowBusy = false;
    bool k95CacheReady = false; // Set only after the validated final cache marker.
    std::unique_ptr<HwCacheBatch> k95CacheBatch;
    HwCacheBatch::Result k95CacheKnown;
    unsigned k95CacheBlackMask = 0;
    bool k95CacheFailureNotified = false;
    bool k95ReplayingCache = false;
    QString deferredPollRate;
    bool _k95HwSlots = false;
    // The hardware profile was turned into the three slots (once per device, kept in Devices/<serial>/K95PHwMigrated), and
    // its GUID (Devices/<serial>/K95PHwProfile), so that it is known before the daemon's cache arrives
    bool k95Migrated = false;
    QString k95HwGuid;
    void migrateK95HwProfile(KbProfile* profile);
    // That profile, when the daemon does not read the slots (!k95Onboard()): out of _profiles, so that no list, menu or command
    // line reaches it, and saved back where it was in the order of the profiles (never as the current profile).
    KbProfile* k95HiddenHwProfile = nullptr;
    int k95HiddenIndex = -1;
    std::array<HwSlotDraft::Draft, 3> k95Drafts;
    unsigned k95CommitMask = 0;
    void fillK95Mode(KbMode* target, const HwCacheBatch::Mode& mode, unsigned i);
    bool k95Rec = false;
    bool k95Decided = true;
    bool k95InHw = false;
    QTimer* k95DecisionTimer = nullptr;
    void decideK95Mode();
    void enterK95Hw();
    // Nothing goes to the daemon for the current mode: the start is undecided, or the keyboard is in hardware mode
    // The content of slot i in the GUI (the mode's name and colours, the draft's bindings) against a record of the slot: the
    // flags of the draft (recreate, replaceLights) are not content
    // (fallback: the base whose performance settings the draft's unknown ones are; NULL: cached's record)
    bool k95ContentDiffers(int i, const HwCacheBatch::Mode& cached, const HwBinding::Record* fallback = nullptr) const;
    bool k95NameDiffers(int i, const HwCacheBatch::Mode& cached) const;
    bool k95ColoursDiffer(int i, const HwCacheBatch::Mode& cached) const;
    bool k95Quiet() const { return _k95HwSlots && (!k95Decided || k95InHw); }
    std::array<K95Read, 3> k95Read{{K95_UNREAD, K95_UNREAD, K95_UNREAD}};
    std::array<unsigned, 3> k95ReadProgress{{0, 0, 0}};
    std::array<QString, 3> k95ReadWhy;
    std::vector<int> k95ReadQueue;
    int k95ReadSlot = -1;          // the slot being read: its "hwslot read:" was sent, and its record has not come yet
    int k95ReadBatchSlot = -1;     // k95CacheBatch is the load of the record of this slot, read on demand
    bool k95BatchDiscard = false;  // k95CacheBatch is such a load whose guard ran out: its late lines are taken and dropped
    QTimer* k95ReadTimer = nullptr;
    // The current mode of the hardware profile: read it if it never was, or, when the user opens it again (retry), if its read failed
    void k95WantSlot(bool retry = false);
    void k95NextRead();            // start the next read of the queue, when nothing is in the way
    void k95ReadLine(const QStringList& components, const QString& line);
    void k95ReadEnd(int slot, bool ok, const QString& why);
    void k95DropQueuedReads();
    // Queues the read of slot i as k95WantSlot does for the current mode (for a copy to it)
    void k95QueueRead(int i, bool retry);
    // Copies of the performance settings waiting for the read of their slot
    std::array<bool, 3> k95PendingCopy{{false, false, false}};
    std::array<HwSlotDraft::Perf, 3> k95PendingPerf;
    void k95ApplyPerf(int i, const HwSlotDraft::Perf& from);
    void k95NoteLater(const QString& text);

    // Key map for this keyboard
    KeyMap getKeyMap();

    // Notification reader, launches as a separate thread and reads from file.
    // (QFile doesn't have readyRead() so there's no other way to do this asynchronously)
    void run();

    QElapsedTimer deviceIdleTimer;
};

#endif // KB_H
