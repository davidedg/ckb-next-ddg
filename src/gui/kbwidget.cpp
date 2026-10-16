#include <cmath>
#include <QBrush>
#include <QFileDialog>
#include <QMenu>
#include <QMessageBox>
#include <QUrl>
#include <QTimer>
#include "ckbsettings.h"
#include "fwupgradedialog.h"
#include "kbfirmware.h"
#include "kbwidget.h"
#include "kblightwidget.h"
#include "kbprofiledialog.h"
#include "ui_kbwidget.h"
#include "ui_kblightwidget.h"
#include "kbmodeeventmgr.h"
#include "mainwindow.h"
#include <QItemSelectionModel>
#include "modelisttablemodel.h"
#include "hwsavecontroller.h"
#include "kbhwsavedevice.h"
#include "hwbindwidget.h"
#include "hwperfwidget.h"
#include "hwlightwidget.h"
#include "k95pkeytable.h"
#include "modeselectdialog.h"
#include "macroreader.h"
#include <QPushButton>
#include <QLineEdit>
#include <QStyledItemDelegate>
#include "hwbinding.h"
#include "modelistwidget.h"

namespace {
// The name of a hardware slot has to fit PROFILE.I: its editor takes no more UTF-16 units than a save can write
class SlotNameDelegate : public ModeNameDelegate {
public:
    SlotNameDelegate(Kb* kb, ModeListWidget* view) : ModeNameDelegate(view), device(kb) {}
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QWidget* editor = ModeNameDelegate::createEditor(parent, option, index);
        if(auto* line = qobject_cast<QLineEdit*>(editor))
            if(device->isHwSlotProfile(device->currentProfile()))
                line->setMaxLength(int(HwBinding::NAME_UNITS));
        return editor;
    }
private:
    Kb* device;
};
}

KbWidget::KbWidget(QWidget *parent, Kb *_device, XWindowDetector* windowDetector) :
    QWidget(parent),
    device(_device), hasShownNewFW(false),
    ui(new Ui::KbWidget), currentMode(nullptr),
    prevmode(nullptr)
{
    ui->setupUi(this);
    hwLightWidget = new HwLightWidget(this);
    ui->gridLayout_2->addWidget(hwLightWidget, 1, 0);
    hwLightWidget->hide();
    {   // the note under the list of slots: muted, as a hint
        QPalette pal = ui->hwSlotNote->palette();
        pal.setColor(QPalette::WindowText, pal.color(QPalette::Disabled, QPalette::WindowText));
        ui->hwSlotNote->setPalette(pal);
    }
    // The widgets of the slots of a K95P's hardware profile (hwslot): shown by showHwBindings() and showHwSlotExtras()
    ui->hwBindWidget->hide();
    ui->k95LightBar->hide();
    ui->k95PerfPanel->hide();   // what the tab says of the slot (not read, cannot be saved as it is...)
    ui->k95PerfWidget->hide();
    ui->hwEraseButton->setVisible(device->k95HwSlots());
    ui->hwReloadButton->setVisible(device->k95HwSlots());
    if(device->k95Onboard()){
        k95SaveController = new HwSaveController(new KbHwSaveDevice(device, this), this);
        ui->hwSaveButton->setText(tr("Save selected slot to hardware"));
        connect(k95SaveController, &HwSaveController::busyChanged, this, [this](bool busy){
            const bool enabled = !busy && externalControlsEnabled && k95ReloadPending < 0;
            ui->tabWidget->setEnabled(enabled);
            ui->profileBox->setEnabled(enabled);
            ui->modesList->setEnabled(enabled);
            ui->bindWidget->setControlsEnabled(enabled);
            if(device->k95HwSlots()) ui->hwBindWidget->setControlsEnabled(enabled);
            updateK95SaveButtons();
        });
        connect(device, &Kb::k95CacheFinished, this, [this](bool){ updateK95SaveButtons(); });
        connect(device, &Kb::profileChanged, this, [this](){ updateK95SaveButtons(); });
        connect(device, &Kb::modeChanged, this, [this](){ updateK95SaveButtons(); });
        if(device->k95HwSlots()){
            connect(device, &Kb::k95MigrationFailed, this, [this](){
                auto* dialog = new QMessageBox(QMessageBox::Warning, tr("Hardware profile migration"),
                    tr("The settings backup could not be verified. Your profiles have been kept unchanged and hardware "
                       "saving is unavailable. Check that the settings directory is writable and has free space, then retry."),
                    QMessageBox::Retry | QMessageBox::Close, this);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                connect(dialog, &QMessageBox::finished, this, [this](int result){
                    if(result == QMessageBox::Retry) device->retryK95Migration();
                });
                dialog->open();
            });
            // hwslot: the modes of the hardware profile have their own Binding tab (the bindings of the slot's draft)
            connect(ui->hwBindWidget, &HwBindWidget::draftChanged, this, [this](){
                device->k95DraftChanged(device->currentProfile()->indexOf(device->currentMode()));
            });
            connect(hwLightWidget, &HwLightWidget::draftChanged, this, [this](){
                device->k95DraftChanged(device->currentProfile()->indexOf(device->currentMode()));
                updateK95SaveButtons();
            });
            connect(ui->hwBindWidget, &HwBindWidget::copyRequested, this, &KbWidget::copyHwBindings);
            connect(ui->hwEraseButton, &QPushButton::clicked, this, &KbWidget::eraseK95);
            connect(ui->hwReloadButton, &QPushButton::clicked, this, &KbWidget::reloadK95);
            connect(ui->hwBindWidget, &HwBindWidget::recordRequested, this, &KbWidget::recordHw);
            // Above the lighting: a slot's effects are kept, unless replaced with static colours
            // The performance settings of a slot, instead of the software ones (the Performance HW tab), and what it says of the slot
            auto currentSlot = [this](){
                KbProfile* profile = device->currentProfile();
                return profile && device->isHwSlotProfile(profile) ? profile->indexOf(device->currentMode()) : -1;
            };
            connect(ui->k95PerfWidget, &HwPerfWidget::perfChanged, this, [this, currentSlot](const HwSlotDraft::Perf& perf){
                device->k95SetPerf(currentSlot(), perf);
                showHwSlotExtras();
            });
            connect(ui->k95PerfWidget, &HwPerfWidget::restoreRequested, this, [this, currentSlot](){
                device->k95RestorePerfDefaults(currentSlot());
                showHwSlotExtras();
            });
            connect(ui->k95PerfWidget, &HwPerfWidget::copyRequested, this, &KbWidget::copyHwPerf);
            connect(device, &Kb::k95Note, this, [this](const QString& text){
                QMessageBox::information(this, tr("Copy performance settings"), text);
            });
            connect(device, &Kb::k95RecordingChanged, this, [this](bool recording){
                // Nothing else changes while the recorder runs: the profile, the mode, a save
                const bool enabled = !recording && externalControlsEnabled && !device->hwFlowBusy() && k95ReloadPending < 0;
                ui->tabWidget->tabBar()->setEnabled(enabled);
                ui->profileBox->setEnabled(enabled);
                ui->modesList->setEnabled(enabled);
                updateK95SaveButtons();
                if(!recording && k95Recorder){
                    delete k95Recorder;
                    k95Recorder = nullptr;
                    ui->hwBindWidget->setRecording(false);
                }
            });
            connect(device, &Kb::k95CacheFinished, this, [this](bool){ showHwBindings(); });
            // The read of a slot on demand: the editor of that slot when it is the one shown (another slot's progress leaves it alone)
            connect(device, &Kb::k95SlotReadChanged, this, [this](int slot){
                const bool reloading = slot == k95ReloadPending;
                const Kb::K95Read state = device->k95ReadState(slot);
                // Derive every editor's enabled state after releasing a completed reload.
                if(reloading && (state == Kb::K95_READ || state == Kb::K95_FAILED)) k95ReloadPending = -1;
                KbProfile* profile = device->currentProfile();
                if(profile && device->isHwSlotProfile(profile) && profile->indexOf(device->currentMode()) == slot)
                    showHwBindings();
                else if(profile && device->isHwSlotProfile(profile))
                    showHwSlotExtras();   // (a copy of the performance settings may wait for that slot: the tab says it)
                updateK95SaveButtons();
                if(reloading){
                    // A successful reload replaces the whole draft: prevent edits while it is being read.
                    const bool enabled = externalControlsEnabled && k95ReloadPending < 0 && !device->hwFlowBusy() &&
                        !device->k95Recording() && (!k95SaveController || !k95SaveController->busy());
                    ui->tabWidget->setEnabled(enabled);
                    ui->profileBox->setEnabled(enabled);
                    ui->modesList->setEnabled(enabled);
                    if(state == Kb::K95_FAILED){
                        const QString why = device->k95ReadError(slot);
                        QTimer::singleShot(0, this, [this, slot, why](){
                            QMessageBox::warning(this, tr("Reload Slot"),
                                tr("M%1 could not be read from the keyboard (%2).\n\nYour local draft is kept.")
                                .arg(slot + 1).arg(why));
                        });
                    }
                }
            });
            connect(device, &Kb::k95DraftConflict, this, &KbWidget::askK95Conflict);
        }
    } else {
        ui->hwSaveAllButton->hide();
    }
    defaultProfileBoxPalette = ui->profileBox->palette();
    Q_ASSERT(ui->pollRateBox->count() == Kb::POLLRATE_COUNT);
    ui->modesList->setDevice(device);
    ui->modesList->setItemDelegateForColumn(ModeListTableModel::COL_MODE_NAME, new SlotNameDelegate(device, ui->modesList));
    connect(device, &Kb::profileRenamed, this, &KbWidget::updateProfileList);
    connect(device, &Kb::profileAdded, this, &KbWidget::updateProfileList);
    connect(device, &Kb::modeChanged, this, &KbWidget::modeChanged);
    connect(device, &Kb::modeCountExceeded, this, &KbWidget::showModeCountWarning);
    connect(device, &Kb::modeCountStatusChanged, this, &KbWidget::updateProfileList);
    // Also needed for the profileBox's own "current selection" color (see
    // updateProfileList()): switching profiles doesn't otherwise touch it.
    connect(device, &Kb::profileChanged, this, &KbWidget::updateProfileList);
    connect(device, &Kb::infoUpdated, this, &KbWidget::devUpdate);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    connect(ui->batteryTrayBox, &QCheckBox::checkStateChanged, this, &KbWidget::batteryTrayBox_checkStateChanged);
#else // QT_VERSION < 6.7.0
    connect(ui->batteryTrayBox, &QCheckBox::stateChanged, this, &KbWidget::batteryTrayBox_stateChanged);
#endif
    connect(MainWindow::mainWindow, &MainWindow::switchToProfileCLI, this, &KbWidget::switchToProfile);
    connect(MainWindow::mainWindow, &MainWindow::switchToProfileAtCLI, this, &KbWidget::switchToProfileAt);
    connect(MainWindow::mainWindow, &MainWindow::switchToModeCLI, this, &KbWidget::switchToMode);
    connect(MainWindow::mainWindow, &MainWindow::switchToModeAtCLI, this, &KbWidget::switchToModeAt);
    connect(ui->modesList->selectionModel(), &QItemSelectionModel::currentRowChanged, this, &KbWidget::currentSelectionChanged);

#ifdef USE_XCB_EWMH
    if(windowDetector)
        connect(windowDetector, &XWindowDetector::activeWindowChanged, this, &KbWidget::switchToModeByFocus);
#endif

    // Remove the Legacy Lighting tab on anything other than the K95L
    if(device->model() != KeyMap::K95L)
        ui->tabWidget->removeTab(ui->tabWidget->indexOf(ui->legacyLightTab));

    // Remove the Lighting and Performance tabs from non-RGB keyboards
    if(!device->features.contains("rgb")){
        if(device->model() != KeyMap::M95){
            ui->tabWidget->removeTab(ui->tabWidget->indexOf(ui->mPerfTab));
            ui->tabWidget->removeTab(ui->tabWidget->indexOf(ui->lightTab));
        }
        ui->tabWidget->removeTab(ui->tabWidget->indexOf(ui->kPerfTab));
    } else {
        // Remove mouse Performance tab from non-mice
        if(!device->isMouse())
            ui->tabWidget->removeTab(ui->tabWidget->indexOf(ui->mPerfTab));
        // Remove keyboard Performance tab from non-keyboards
        if(!device->isKeyboard())
            ui->tabWidget->removeTab(ui->tabWidget->indexOf(ui->kPerfTab));
    }

    // If we have an M95, set the performance and lighting tabs as such
    if(device->model() == KeyMap::M95){
        ui->mPerfWidget->setLegacyM95();
        ui->lightWidget->setLegacyM95();
    }

    // If we have a DARK CORE or Dark Core SE, then change the performance tab accordingly
    if(device->model() == KeyMap::DARKCORE)
        ui->mPerfWidget->setDarkCore();

    // If the device is supports it, show the battery
    if(device->features.contains("battery")){
        connect(device, &Kb::batteryChanged, this, &KbWidget::updateBattery);
    } else {
        ui->batteryLabel->hide();
        ui->batteryStatusLabel->hide();
        ui->batteryTrayBox->hide();
    }

    // Hide poll rate and FW update as appropriate
    if(device->pollrate == Kb::POLLRATE_UNKNOWN){
        ui->pollRateBox->hide();
        ui->pollLabel2->hide();
        ui->horizontalLayout_2->removeItem(ui->horizontalSpacer_4);
        delete ui->horizontalSpacer_4;
        ui->horizontalSpacer_4 = nullptr;
    }
    if(!device->features.contains("fwupdate")){
        ui->fwUpdButton->hide();
        ui->fwUpdLabel->hide();
        delete ui->fwUpdLayout->takeAt(1);
    }
    // Remove unsupported pollrates
    // Block signals so that the pollrate doesn't change
    bool block = ui->pollRateBox->blockSignals(true);
    if(device->maxpollrate != Kb::POLLRATE_UNKNOWN){
        for(int i = 0; i < (Kb::POLLRATE_COUNT - 1) - device->maxpollrate; i++)
            ui->pollRateBox->removeItem(0);
    }
    ui->pollRateBox->blockSignals(block);

    // Remove binding tab if the device doesn't support it
    if(!device->features.contains("bind")){
        ui->tabWidget->removeTab(ui->tabWidget->indexOf(ui->bindTab));
    }
    // Set monochrome mode according to hardware
    if(device->monochrome)
        ui->lightWidget->setMonochrome();
    // Disable Save to hardware button for unsupported devices
    if(!device->hwSaveAllowed()){
        ui->hwSaveButton->setDisabled(true);
        ui->hwSaveButton->setToolTip(device->model() == KeyMap::K95P && !device->k95Onboard() && device->k95FirmwareTested() ?
                                     tr("For this device, saving to hardware requires ckb-next-daemon started with --enable-experimental.") :
                                     device->hwload ? tr("Saving to hardware is not supported yet on this device.")
                                                    : tr("Saving to hardware is not supported on this device."));
    }
    updateK95SaveButtons();
    // Read device layout
    if(device->features.contains("bind")){
        // Clear the "Default" value
        ui->layoutBox->clear();

        // Load the current device's layout from the settings
        QString layoutSettingsPath("Devices/%1");
        CkbSettings settings(layoutSettingsPath.arg(device->usbSerial));

        QList<QPair<int, QString>> layoutnames = KeyMap::layoutNames(device->hwlayout);

        // Enable the ComboBox only if there is more than one supported layout
        if(layoutnames.count() > 1)
            ui->layoutBox->setEnabled(true);

        for(int i = 0; i < layoutnames.count(); i++)
            ui->layoutBox->addItem(layoutnames[i].second, layoutnames[i].first);

        KeyMap::Layout layout = KeyMap::getLayout(settings.value("hwLayout").toString());
        if(layout == KeyMap::NO_LAYOUT){
            // If the layout hasn't been set yet, first check if one was set globally from a previous version
            // If not, try to pick an appropriate one that's supported by the hardware
            KeyMap::Layout oldLayout = KeyMap::getLayout(CkbSettings::get("Program/KbdLayout").toString());
            if(oldLayout == KeyMap::NO_LAYOUT){
                layout = KeyMap::locale(&layoutnames);
            } else {
                CkbSettings::set("Program/KbdLayout", "");
                layout = oldLayout;
            }
        }
        // Find the position of the layout in the QComboBox and set it
        int layoutpos = -1;
        if(layout != KeyMap::NO_LAYOUT){
            for(int i = 0; i < layoutnames.count(); i++){
                if(layoutnames.at(i).first == (int)layout){
                    layoutpos = i;
                    break;
                }
            }
        }
        // If no layout was found, pick the first one from the list
        if(layoutpos == -1){
            layout = (KeyMap::Layout)layoutnames.at(0).first;
            layoutpos = 0;
        }

        ui->layoutBox->setCurrentIndex(layoutpos);

        // Set the layout and save it
        device->layout(layout, false);
    }
    else
        device->layout(KeyMap::GB, false);

    // Set max DPI for mice
    if(device->isMouse())
        ui->mPerfWidget->setMaxDpi(device->getMaxDpi());

    if(!device->adjrate){
        ui->pollRateBox->setEnabled(false);
        ui->pollRateBox->setToolTip(tr("This device does not support setting the poll rate through software."));
    }
}

KbWidget::~KbWidget(){
    delete ui;
}

void KbWidget::showDeviceTab(){
    ui->tabWidget->setCurrentIndex(ui->tabWidget->indexOf(ui->devTab));
}

void KbWidget::updateProfileList(){
    // Clear profile list and rebuild
    KbProfile* hwProfile = device->hwProfile(), *currentProfile = device->currentProfile();
    ui->profileBox->clear();
    int i = 0;
    foreach(KbProfile* profile, device->profiles()){
        ui->profileBox->addItem((profile == hwProfile) ? QIcon(":/img/icon_profile_hardware.png") : QIcon(":/img/icon_profile.png"),
                                profile->name());
        if(profile->modeCount() > device->daemonModeCount){
            // Flag profiles the daemon can't fully represent (too many modes for
            // its configured --modecount), independently of whether the one-time
            // warning popup already fired for this profile.
            ui->profileBox->setItemData(i, QBrush(Qt::red), Qt::ForegroundRole);
            ui->profileBox->setItemData(i, tr("This profile has %1 modes, but the daemon only supports %2.")
                                        .arg(profile->modeCount()).arg(device->daemonModeCount), Qt::ToolTipRole);
        }
        if(profile == currentProfile)
            ui->profileBox->setCurrentIndex(i);
        i++;
    }
    ui->profileBox->addItem(QIcon(":/img/icon_blank.png"), tr("Manage profiles..."));
    QFont font = ui->profileBox->font();
    font.setItalic(true);
    ui->profileBox->setItemData(ui->profileBox->count() - 1, font, Qt::FontRole);

    // The ForegroundRole set above only colors entries in the dropdown list, not
    // the closed box's displayed text - that reads from the widget's own palette.
    // Only touch ButtonText here: QPalette::Text is inherited by the dropdown's
    // item view for any entry without its own ForegroundRole override, so setting
    // it here would turn every profile red instead of just the closed-box text.
    if(currentProfile && currentProfile->modeCount() > device->daemonModeCount){
        QPalette pal = defaultProfileBoxPalette;
        pal.setColor(QPalette::ButtonText, Qt::red);
        ui->profileBox->setPalette(pal);
    } else {
        ui->profileBox->setPalette(defaultProfileBoxPalette);
    }
    // The slots of a K95P are edited here: the keyboard's active profile does not follow the selection
    ui->hwSlotNote->setVisible(currentProfile && device->isHwSlotProfile(currentProfile));
}

void KbWidget::on_profileBox_activated(int index){
    if(device->hwFlowBusy() || device->k95Recording()) return;
    if(index < 0)
        return;
    if(index >= device->profiles().count()){
        // "Manage profiles" option
        KbProfileDialog dialog(this);
        dialog.exec();
        updateProfileList();
        return;
    }
    device->setCurrentProfile(device->profiles()[index]);
    // Focus the mode list to highlight the whole row properly
    ui->modesList->setFocus();
    // Device will emit profileChanged() and modeChanged() signals to update UI
}

void KbWidget::modeChanged(){
    int index = device->currentProfile()->indexOf(device->currentMode());
    if(index < 0)
        return;
    // Update tabs
    ui->lightWidget->setLight(device->currentLight());
    ui->bindWidget->setBind(device->currentBind(), device->currentProfile());
    ui->kPerfWidget->setPerf(device->currentPerf(), device->currentProfile());
    ui->mPerfWidget->setPerf(device->currentPerf(), device->currentProfile());
    if(device->k95HwSlots()){
        // hwslot: the lighting of a slot is narrowed and the performance settings of the slots (Win Lock options, indicator colours)
        // are only shown by showHwSlotExtras()
        ui->lightWidget->setEnabled(true);
        ui->kPerfWidget->setEnabled(!device->isHwSlotProfile(device->currentProfile()));
        // In hardware mode the daemon ignores the poll rate
        ui->pollRateBox->setEnabled(!device->isHwSlotProfile(device->currentProfile()));
        showHwBindings();
    }
    // Update selection
    ui->modesList->setCurrentIndex(ui->modesList->model()->index(index, 0));
    currentMode = device->currentMode();
}

void KbWidget::currentSelectionChanged(const QModelIndex& current, const QModelIndex& previous){
    if(device->hwFlowBusy() || device->k95Recording()) return;
    if(current.row() > device->currentProfile()->modeCount() - 1){
        const int row = dynamic_cast<ModeListTableModel*>(ui->modesList->model())->addNewMode();
        if(row >= 0)
            ui->modesList->edit(ui->modesList->model()->index(row, ModeListTableModel::COL_MODE_NAME));
        return;
    }
    KbMode* mode = device->currentProfile()->at(current.row());
    device->setCurrentMode(mode);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
#define CHECK_VALUE_UNCHECKED Qt::CheckState::Unchecked
void KbWidget::batteryTrayBox_checkStateChanged(Qt::CheckState state){
#else // QT_VERSION < 6.7.0
#define CHECK_VALUE_UNCHECKED 0
void KbWidget::batteryTrayBox_stateChanged(int state){
#endif
    if(!device->features.contains("battery"))
        return;
    device->showBatteryIndicator = state != CHECK_VALUE_UNCHECKED;
    device->needsSave();
    if(device->showBatteryIndicator){
        device->batteryIcon->show();
    } else {
        device->batteryIcon->hide();
    }
}

void KbWidget::on_modesList_customContextMenuRequested(const QPoint& pos){
    if(device->hwFlowBusy() || device->k95Recording()) return;
    QModelIndex idx = ui->modesList->indexAt(pos);
    KbProfile* currentProfile = device->currentProfile();
    if(!idx.isValid() || !currentMode || idx.row() > currentProfile->modeCount() - 1)
        return;

    const int row = idx.row();
    if(currentProfile->modes().at(row) != currentMode){
        return;
    }

    QMenu menu(this);
    QAction* rename = new QAction(tr("Rename..."), this);
    QAction* duplicate = new QAction(tr("Duplicate"), this);
    QAction* del = new QAction(tr("Delete"), this);
    const bool canDelete = device->canDeleteMode(currentProfile);
    if(!canDelete)
        // Can't delete modes if they're required by hardware
        del->setEnabled(false);
    // hwslot: the hardware profile is the three slots
    const bool canAdd = device->canAddMode(currentProfile), canMove = device->canMoveModes(currentProfile);
    duplicate->setEnabled(canAdd);
    QAction* moveup = new QAction(tr("Move Up"), this);
#ifdef USE_XCB_EWMH
    QAction* focusevts = new QAction(tr("Manage Events"), this);
    focusevts->setEnabled(!device->isHwSlotProfile(currentProfile));
#endif
    if(row == 0 || !canMove)
        moveup->setEnabled(false);
    QAction* movedown = new QAction(tr("Move Down"), this);
    if(row >= currentProfile->modeCount() - 1 || !canMove)
        movedown->setEnabled(false);
    menu.addAction(rename);
    // The hardware slots are fixed: only their names can change.
    if(!device->isHwSlotProfile(currentProfile)){
        menu.addAction(duplicate);
        menu.addAction(del);
#ifdef USE_XCB_EWMH
        menu.addSeparator();
        menu.addAction(focusevts);
        menu.addSeparator();
#endif
        menu.addAction(moveup);
        menu.addAction(movedown);
    }
    ui->modesList->setIgnoreFocusLoss(true);
    QAction* result = menu.exec(QCursor::pos());
    ui->modesList->setIgnoreFocusLoss(false);
    if(!result || !result->isEnabled())
        return;
    if(result == rename){
        ui->modesList->edit(ui->modesList->model()->index(idx.row(), ModeListTableModel::COL_MODE_NAME, idx.parent()));
    } else if(result == duplicate){
        KbMode* newMode = device->newMode(currentMode);
        newMode->newId();
        profileAboutToChange();
        currentProfile->insert(row + 1, newMode);
        // Update UI
        profileChanged();
        device->setCurrentMode(newMode);
    } else if(result == del){
        if(!canDelete)
            return;
        if(QMessageBox::question(this, tr("Delete mode"), tr("Are you sure you want to delete this mode?")) != QMessageBox::Yes)
            return;
        profileAboutToChange();
        currentProfile->removeAll(currentMode);
        currentMode->deleteLater();
        currentMode = nullptr;
        // Select next mode
        profileChanged();
        if(row < currentProfile->modeCount())
            device->setCurrentMode(currentProfile->modes()[row]);
        else
            device->setCurrentMode(currentProfile->modes().last());
    } else if(result == moveup){
        profileAboutToChange();
        currentProfile->removeAll(currentMode);
        currentProfile->insert(row - 1, currentMode);
        // Update UI
        profileChanged();
        modeChanged();
    } else if(result == movedown){
        profileAboutToChange();
        currentProfile->removeAll(currentMode);
        currentProfile->insert(row + 1, currentMode);
        // Update UI
        profileChanged();
        modeChanged();
    }
#ifdef USE_XCB_EWMH
     else if(result == focusevts) {
        openEventMgr(currentProfile->currentMode());
    }
#endif
}

void KbWidget::devUpdate(){
    // Update device tab
    ui->devLabel->setText(device->usbModel);
    ui->serialLabel->setText(device->usbSerial);
    ui->fwLabel->setText(device->firmware.app.toString());
    ui->bldValLabel->setText(device->firmware.bld.toString());
    // Not all WL devices have a radio BLD so these must be kept separate
    if(device->firmware.radioapp.isNull()){
        ui->wlLabel->setVisible(false);
        ui->wlValLabel->setVisible(false);
    } else {
        ui->wlValLabel->setText(device->firmware.radioapp.toString());
    }
    if(device->firmware.radiobld.isNull()){
        ui->wlBldLabel->setVisible(false);
        ui->wlBldValLabel->setVisible(false);
    } else {
        ui->wlBldValLabel->setText(device->firmware.radiobld.toString());
    }
    // This is needed so that the currentIndexChanged event doesn't fire
    // If it does, we'll end up with an always greyed out box when pollrate != 1
    bool block = ui->pollRateBox->blockSignals(true);
    ui->pollRateBox->setCurrentIndex(getPollRateBoxIdx(device->pollrate));
    ui->pollRateBox->blockSignals(block);
    ui->batteryTrayBox->setChecked(device->showBatteryIndicator);
}

void KbWidget::updateBattery(uint battery, BatteryStatus charging){
    QString label = QString("%1 (%2%), %3")
                    .arg(BatteryStatusTrayIcon::BATTERY_VALUES[BatteryStatusTrayIcon::getBatteryString(battery)],
                    QString::number(battery),
                    BatteryStatusTrayIcon::BATTERY_STATUS_VALUES[charging]);
    ui->batteryStatusLabel->setText(label);
}

void KbWidget::on_hwSaveButton_clicked(){
    if(device->k95Onboard()){ saveK95(false); return; }
    profileAboutToChange();
    device->save();
    device->hwSave();
    updateProfileList();
    profileChanged();
}

void KbWidget::on_hwSaveAllButton_clicked(){
    saveK95(true);
}

void KbWidget::updateK95SaveButtons(){
    if(!device->k95Onboard()) return;
    const bool eligible = externalControlsEnabled && !device->k95Recording() && device->hwSaveAllowed() && device->k95CacheIsReady() &&
        device->currentProfile() && device->currentProfile() == device->hwProfile() &&
        device->currentProfile()->modeCount() >= 3 && !device->hwFlowBusy() && !device->k95Reading() &&
        (!k95SaveController || !k95SaveController->busy())
        && !device->k95CopyPending();
    const int current = device->currentProfile() && device->currentMode() ? device->currentProfile()->indexOf(device->currentMode()) : -1;
    ui->hwReloadButton->setEnabled(externalControlsEnabled && device->k95HwSlots() && device->k95CacheIsReady() &&
        device->isHwSlotProfile(device->currentProfile()) && current >= 0 && current < 3 && !device->hwFlowBusy() &&
        !device->k95Recording() && !device->k95Reading() && !device->k95CopyPending() &&
        (!k95SaveController || !k95SaveController->busy()));
    // Save slot needs the slot read from the keyboard (its mode was opened); Save ALL leaves out the slots not read and not changed
    const bool currentRead = current >= 0 && current < 3 && device->k95ReadState(current) == Kb::K95_READ;
    // A slot that cannot be saved as it is: Save slot off, and why (Save ALL checks it, and says that it is refused)
    const HwBinding::Record* base = currentRead ? device->k95SlotRecord(current) : nullptr;
    QString unsavable;
    if(base && base->readonly)
        unsavable = k95ReadonlyText(*base);
    ui->hwSaveAllButton->setEnabled(eligible);
    ui->hwSaveButton->setEnabled(eligible && currentRead && unsavable.isEmpty());
    // Erase: any slot, read or not, empty or one that cannot be rewritten
    std::string eraseLine;
    QString eraseWhy;
    ui->hwEraseButton->setEnabled(eligible && current >= 0 && current < 3 && device->k95EraseLine(current, eraseLine, eraseWhy));
    int reading = -1;
    for(int i = 0; i < 3; ++i)
        if(device->k95ReadState(i) == Kb::K95_READING) reading = i;
    const QString unavailable = device->k95CopyPending() ?
        tr("A copy of the performance settings is waiting for a slot to be read from the keyboard.") :
        device->k95Reading() && reading >= 0 ?
        tr("M%1 is being read from the keyboard: wait for it to finish.").arg(reading + 1) :
        !device->k95FirmwareTested() ?
        tr("Hardware saving is available only for K95 Platinum firmware 3.29 / bootloader 3.03.") :
        !device->k95HwSlots() ?
        tr("Hardware saving needs a ckb-next daemon that supports the hardware slots of this keyboard (hwslot).") :
        !device->k95CacheIsReady() ?
        tr("Hardware slots are still loading or their state could not be verified.") :
        tr("Select M1, M2 or M3 in the hardware profile to save.");
    ui->hwSaveButton->setToolTip(eligible && currentRead ? (unsavable.isEmpty() ? tr("Save the selected hardware slot.") : unsavable) :
                                 eligible ? tr("The slot is not read from the keyboard yet.") : unavailable);
    ui->hwSaveAllButton->setToolTip(eligible ? tr("Check all three hardware slots, then save eligible changes.") : unavailable);
}

void KbWidget::reloadK95(){
    updateK95SaveButtons();
    if(!ui->hwReloadButton->isEnabled()) return;
    KbProfile* profile = device->currentProfile();
    KbMode* mode = device->currentMode();
    const int slot = profile->indexOf(mode);
    QMessageBox box(QMessageBox::Question, tr("Reload Slot"),
        tr("Reload M%1 from the keyboard?\n\nThe slot name, bindings, lighting and Performance settings in your local draft "
           "will be replaced, including changes kept after restarting the GUI.\n\nThe keyboard will not be changed. "
           "If reading fails, your local draft is kept.").arg(slot + 1), QMessageBox::Cancel, this);
    QPushButton* reload = box.addButton(tr("Reload Slot"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.setEscapeButton(QMessageBox::Cancel);
    box.exec();
    if(box.clickedButton() != reload) return;
    updateK95SaveButtons();
    // A modal dialog runs the event loop: the slot and controls may have changed meanwhile.
    if(!ui->hwReloadButton->isEnabled() || device->currentProfile() != profile || device->currentMode() != mode) return;
    k95ReloadPending = slot;
    if(!device->k95ReloadSlot(slot)) k95ReloadPending = -1;
    updateK95SaveButtons();
}

void KbWidget::saveK95(bool all){
    updateK95SaveButtons();
    if(!externalControlsEnabled || !k95SaveController || !device->hwSaveAllowed() || !device->k95CacheIsReady() ||
       device->currentProfile() != device->hwProfile() || device->hwFlowBusy()) return;
    KbProfile* profile = device->currentProfile();
    if(!profile || profile->modeCount() < 3) return;
    const int selected = profile->indexOf(device->currentMode());
    if(!all && (selected < 0 || selected >= 3)) return;
    // hwslot: the lines of each slot's transaction (the hwsave route is closed)
    std::vector<HwSaveFlow::Slot> saveSlots;
    std::array<bool, 3> rgbRequired{{false, false, false}};
    QString why;
    if(!device->k95SaveSlots(all, selected, saveSlots, rgbRequired, why)){
        QMessageBox::warning(this, tr("Hardware save unavailable"), why);
        return;
    }
    if(!k95SaveController->start(saveSlots, all, rgbRequired)){
        QMessageBox::warning(this, tr("Hardware save unavailable"),
                             tr("The device is busy or its hardware cache is not ready."));
        return;
    }
    updateK95SaveButtons();
}

int KbWidget::k95EraseStepMs = 1000;

void KbWidget::eraseK95(){
    updateK95SaveButtons();
    KbProfile* profile = device->currentProfile();
    const int slot = profile ? profile->indexOf(device->currentMode()) : -1;
    std::string line;
    QString why;
    if(!ui->hwEraseButton->isEnabled() || !k95SaveController || slot < 0 || slot > 2 || !device->k95EraseLine(slot, line, why))
        return;
    // The confirmation: Yes only after 3 s, counting down in its text; Cancel the default, and the one of Esc
    QMessageBox box(QMessageBox::Warning, tr("Erase hardware slot"),
                    tr("Erase M%1 \"%2\" from the keyboard?\n\nIts remaps, macros, lighting and performance settings are deleted, for "
                       "iCUE too. This cannot be undone.").arg(slot + 1).arg(device->currentMode()->name()),
                    QMessageBox::Yes | QMessageBox::Cancel, this);
    box.setDefaultButton(QMessageBox::Cancel);
    box.setEscapeButton(QMessageBox::Cancel);
    QAbstractButton* yes = box.button(QMessageBox::Yes);
    int left = 3;
    yes->setEnabled(false);
    yes->setText(tr("Yes (%1)").arg(left));
    QTimer countdown;
    countdown.setInterval(k95EraseStepMs);
    connect(&countdown, &QTimer::timeout, &box, [&](){
        if(--left > 0)
            yes->setText(tr("Yes (%1)").arg(left));
        else {
            countdown.stop();
            yes->setText(tr("Yes"));
            yes->setEnabled(true);
        }
    });
    countdown.start();
    if(box.exec() != QMessageBox::Yes)
        return;
    if(!k95SaveController->startErase(unsigned(slot + 1), line))
        QMessageBox::warning(this, tr("Erase hardware slot"), tr("The device is busy or its hardware cache is not ready."));
    updateK95SaveButtons();
}

QString KbWidget::k95ReadonlyText(const HwBinding::Record& base) const {
    switch(base.zip){
    case HwBinding::Record::ZIP_NONE:
        return tr("This slot is read-only: it has no PROFILE.ZIP, iCUE's copy of the profile (older versions of ckb-next did not write "
                  "one). Erase it to use it again.");
    case HwBinding::Record::ZIP_UNKNOWN:
        return tr("This slot is read-only: it was written by an unsupported version of iCUE, with settings this version of ckb-next "
                  "does not know. Erase it to use it again, or edit it in iCUE. Please also consider opening an issue on GitHub.");
    case HwBinding::Record::ZIP_CORRUPT:
        return tr("This slot is read-only: its PROFILE.ZIP, iCUE's copy of the profile, cannot be read. Erase it to use it again.");
    case HwBinding::Record::ZIP_UNCOMPILABLE:
        return tr("This slot is read-only: it was written by an unsupported version of iCUE, with settings this version of ckb-next "
                  "cannot write to the keyboard yet. Edit it in iCUE, or erase it to use it again. Please also consider opening an "
                  "issue on GitHub.");
    default:
        // (its PROFILE.I: the reason says it)
        return tr("This slot cannot be rewritten: %1").arg(QString::fromStdString(base.readonlyWhy));
    }
}

QString KbWidget::k95ReadonlyTip(const HwBinding::Record& base) const {
    if(base.zip == HwBinding::Record::ZIP_OK || base.zip == HwBinding::Record::ZIP_STALE)
        return QString();
    return tr("Details: %1").arg(QString::fromStdString(base.readonlyWhy));
}

void KbWidget::on_tabWidget_currentChanged(int index){
    if(!device)
        return;
    if(index == ui->tabWidget->count() - 1){
        // Device tab
        updateFwButton();
    }
}

void KbWidget::updateFwButton(){
    if(!MainWindow::mainWindow->kbfw->hasDownloaded())
        ui->fwUpdButton->setText(tr("Check for updates"));
    else {
        CkbVersionNumber newVersion = MainWindow::mainWindow->kbfw->versionForBoard(device->productID);
        const CkbVersionNumber& oldVersion = device->firmware.app;
        if(newVersion.isNull() || newVersion <= oldVersion)
            ui->fwUpdButton->setText(tr("Up to date"));
        else
            ui->fwUpdButton->setText(tr("Upgrade to v%1").arg(newVersion.toString()));
    }
}

void KbWidget::setTabBarEnabled(const bool e){
    externalControlsEnabled = e;
    const bool enabled = e && !device->hwFlowBusy() && k95ReloadPending < 0;
    ui->tabWidget->tabBar()->setEnabled(enabled);
    ui->profileBox->setEnabled(enabled);
    ui->modesList->setEnabled(enabled);
    ui->hwSaveButton->setEnabled(enabled);
    if(device->k95Onboard()) updateK95SaveButtons();
    ui->bindWidget->setControlsEnabled(enabled);
    if(device->k95HwSlots()) ui->hwBindWidget->setControlsEnabled(enabled);
}

void KbWidget::recordHw(bool start){
    if(!device->k95HwSlots())
        return;
    if(!start){
        device->k95StopRecording();   // k95RecordingChanged(false) closes the reader
        return;
    }
    if(k95Recorder || !device->k95StartRecording())
        return;
    k95Recorder = new MacroReader(QStringList{device->getMacroPath()});
    connect(k95Recorder, &MacroReader::macroLineRead, ui->hwBindWidget, &HwBindWidget::recordedEvent);
    ui->hwBindWidget->setRecording(true);
}

QString KbWidget::k95ReadText(int slot, const QString& otherwise) const {
    // What the read of the slot on demand is doing, in the labels that say why the slot is not shown
    switch(device->k95ReadState(slot)){
    case Kb::K95_QUEUED:
        return tr("M%1 will be read from the keyboard after the slot being read now.").arg(slot + 1);
    case Kb::K95_READING:
        return tr("Reading M%1 from the keyboard\u2026 %2 %").arg(slot + 1).arg(device->k95ReadDone(slot) / 10);
    case Kb::K95_FAILED:
        return tr("M%1 could not be read from the keyboard (%2). Select another mode and then this one again to try again.")
            .arg(slot + 1).arg(device->k95ReadError(slot));
    default:
        return otherwise;
    }
}

void KbWidget::showHwBindings(){
    if(!device->k95HwSlots() || device->k95Recording())
        return;
    KbProfile* profile = device->currentProfile();
    const int slot = profile ? profile->indexOf(device->currentMode()) : -1;
    const bool hw = device->isHwSlotProfile(profile) && slot >= 0 && slot < 3;
    ui->bindWidget->setVisible(!hw);
    ui->hwBindWidget->setVisible(hw);
    if(!hw){
        // Lighting and Performance go back to the software ones too (they stayed "hardware slot" after leaving the HW profile)
        showHwSlotExtras();
        return;
    }
    const HwBinding::Record* base = device->k95SlotRecord(slot);
    HwSlotDraft::Draft* draft = device->k95Draft(slot);
    QString why;
    bool editable = false;
    if(!device->k95CacheIsReady() || !base)
        why = k95ReadText(slot, tr("The bindings of this slot are still loading, or could not be read from the keyboard."));
    else if(draft && draft->conflict)
        why = tr("This slot changed on the keyboard after you edited it: reload it or keep your changes first.");
    else if(base->readonly)
        why = k95ReadonlyText(*base);
    else {
        editable = true;
        if(device->k95SlotEmpty(slot))
            why = tr("This slot is empty: saving it makes a new hardware profile in it.");
    }
    ui->hwBindWidget->setSlot(base, draft, profile->keyMap(), KeyMap::getLayout(device->getCurrentLayout()), editable, why);
    ui->hwBindWidget->setControlsEnabled(externalControlsEnabled && !device->hwFlowBusy());
    showHwSlotExtras();
}

void KbWidget::showHwSlotExtras(){
    if(!device->k95HwSlots())
        return;
    KbProfile* profile = device->currentProfile();
    const int slot = profile ? profile->indexOf(device->currentMode()) : -1;
    const bool hw = device->isHwSlotProfile(profile) && slot >= 0 && slot < 3;
    const HwBinding::Record* base = hw ? device->k95SlotRecord(slot) : nullptr;
    HwSlotDraft::Draft* draft = hw ? device->k95Draft(slot) : nullptr;
    // Hardware lighting belongs to the complete layer model.
    const bool readonly = base && base->readonly;
    const bool stale = base && !readonly && base->zip == HwBinding::Record::ZIP_STALE;
    // The bar says what the slot is when it is not plain: not read yet (or being read, or failed: the GUI opens on this tab), one that
    // cannot be rewritten (the daemon's reason in its tooltip), one with effects (what can be done, the rest in its tooltip), one
    // whose files are not its PROFILE.ZIP's
    ui->k95LightBar->setVisible(hw && (!base || readonly || stale));
    ui->k95LightButton->hide();
    QString tip;
    if(hw && !base)
        ui->k95LightLabel->setText(k95ReadText(slot, tr("The lighting of this slot is still loading, or could not be read from the keyboard.")));
    else if(readonly){
        ui->k95LightLabel->setText(k95ReadonlyText(*base));
        tip = k95ReadonlyTip(*base);
    } else if(stale)
        ui->k95LightLabel->setText(tr("The files of this slot on the keyboard do not match its PROFILE.ZIP (a save that stopped?): "
                                      "saving the slot writes them again."));
    ui->k95LightLabel->setToolTip(tip);
    ui->lightWidget->setVisible(!hw);
    ui->label_3->setVisible(!hw);
    hwLightWidget->setVisible(hw);
    // The indicator buttons in Lighting: the colours of the performance settings, not paintable
    QMap<QString, QColor> fixed;
    if(hw){
        const HwSlotDraft::Perf perf = device->k95Perf(slot);
        const QColor profileColour(perf.indicators[0], perf.indicators[1], perf.indicators[2]);
        const QColor brightness(perf.indicators[3], perf.indicators[4], perf.indicators[5]);
        const QColor lockOff(perf.indicators[9], perf.indicators[10], perf.indicators[11]);   // the Win Lock button at rest
        const KeyMap map = profile->keyMap();
        for(const char* name : {"profswitch", "logo"})
            if(map.contains(name) && map.key(name).hasLed) fixed[name] = profileColour;
        if(map.contains("light")) fixed["light"] = brightness;
        if(map.contains("lock")) fixed["lock"] = lockOff;
    }
    if(hw){
        const bool editable = externalControlsEnabled && !device->hwFlowBusy() && !device->k95Recording() &&
            k95ReloadPending < 0 && base && !base->readonly && draft && !draft->conflict;
        hwLightWidget->setSlot(draft, profile->keyMap(), device->hwlayout, fixed, editable);
    } else hwLightWidget->setSlot(nullptr, KeyMap(), QString(), {}, false);
    // Performance: the Performance HW tab
    ui->kPerfWidget->setVisible(!hw);
    if(!hw){
        ui->k95PerfPanel->hide();
        ui->k95PerfWidget->hide();
        return;
    }
    if(!base){
        ui->k95PerfPanel->setText(k95ReadText(slot, tr("The performance settings of this slot are still loading, or could not be read.")));
        ui->k95PerfPanel->show();
        ui->k95PerfWidget->hide();
        return;
    }
    QStringList notes;
    bool editable = externalControlsEnabled && !device->hwFlowBusy() && !device->k95Recording() && draft;
    if(base->readonly){
        notes << k95ReadonlyText(*base);
        editable = false;
    } else if(draft && draft->conflict){
        notes << tr("This slot changed on the keyboard after you edited it: reload it or keep your changes first.");
        editable = false;
    }
    for(int i = 0; i < 3; ++i)
        if(device->k95CopyPendingFor(i))
            notes << (device->k95ReadState(i) == Kb::K95_READING ?
                      tr("Copying to M%1: reading it from the keyboard\u2026 %2 %").arg(i + 1).arg(device->k95ReadDone(i) / 10) :
                      tr("Copying to M%1: it will be read from the keyboard first.").arg(i + 1));
    ui->k95PerfPanel->setText(notes.join("\n"));
    ui->k95PerfPanel->setVisible(!notes.isEmpty());
    ui->k95PerfWidget->setPerf(device->k95Perf(slot));
    ui->k95PerfWidget->setEditable(editable);
    ui->k95PerfWidget->setCopyEnabled(externalControlsEnabled && !device->hwFlowBusy() && !device->k95Recording());
    ui->k95PerfWidget->show();
}

void KbWidget::copyHwPerf(){
    KbProfile* profile = device->currentProfile();
    const int from = profile ? profile->indexOf(device->currentMode()) : -1;
    if(!device->isHwSlotProfile(profile) || from < 0 || from > 2)
        return;
    // To the other hardware slots, whatever they are: one not read is read first, one that cannot be rewritten is said
    QList<KbMode*> targets;
    for(int i = 0; i < 3; ++i)
        if(i != from)
            targets.append(profile->at(i));
    ModeSelectDialog dialog(this, device->currentMode(), targets, tr("Copy performance settings to:"));
    if(dialog.exec() != QDialog::Accepted)
        return;
    std::vector<int> to;
    for(KbMode* mode : dialog.selection())
        to.push_back(profile->indexOf(mode));
    device->k95CopyPerf(from, to);
    showHwSlotExtras();
    updateK95SaveButtons();
}

void KbWidget::copyHwBindings(const QStringList& keys){
    KbProfile* profile = device->currentProfile();
    const int from = profile ? profile->indexOf(device->currentMode()) : -1;
    HwSlotDraft::Draft* source = device->k95Draft(from);
    if(!source || !source->hasBindings || !device->isHwSlotProfile(profile))
        return;
    // Only to the other slots that can be edited
    QList<KbMode*> targets;
    for(int i = 0; i < 3; ++i){
        HwSlotDraft::Draft* d = device->k95Draft(i);
        if(i != from && d && d->hasBindings && !d->conflict && device->k95SlotRecord(i))
            targets.append(profile->at(i));
    }
    if(targets.isEmpty())
        return;
    const QString what = keys.isEmpty() ? tr("all keys") : tr("%n key(s)", nullptr, keys.count());
    ModeSelectDialog dialog(this, device->currentMode(), targets, tr("Copy the hardware bindings of %1 to:").arg(what));
    if(dialog.exec() != QDialog::Accepted)
        return;
    for(KbMode* mode : dialog.selection()){
        const int to = profile->indexOf(mode);
        HwSlotDraft::Draft* target = device->k95Draft(to);
        if(!target) continue;
        const HwBinding::Keys before = target->keys;
        for(unsigned k = 0; k < HwBinding::KEYS; ++k){
            const std::string label = K95PKeyTable::label(k);
            if(keys.isEmpty() || keys.contains(QString::fromStdString(label)))
                target->keys[k] = source->keys[k];
        }
        const HwBinding::Record* base = device->k95SlotRecord(to);
        std::string why;
        if(!HwBindEdit::check(target->keys, base ? &base->keys : nullptr, why)){
            target->keys = before;
            QMessageBox::warning(this, tr("Copy to slot"), tr("M%1: %2").arg(to + 1).arg(QString::fromStdString(why)));
            continue;
        }
        device->k95DraftChanged(to);
    }
}

void KbWidget::askK95Conflict(int slot){
    // The buttons say what they do: Yes and No for a question with two actions in it would be guessed at
    QMessageBox box(QMessageBox::Question, tr("Hardware slot changed"),
        tr("M%1 changed on the keyboard after you edited it.\n\nReload it from the keyboard (your changes to it are lost), "
           "or keep your changes on top of what the keyboard has now?").arg(slot + 1), QMessageBox::NoButton, this);
    QPushButton* reload = box.addButton(tr("Reload from the keyboard"), QMessageBox::DestructiveRole);
    QPushButton* keep = box.addButton(tr("Keep my changes"), QMessageBox::RejectRole);
    box.setDefaultButton(keep);
    box.setEscapeButton(keep);
    box.exec();
    device->k95ResolveConflict(slot, box.clickedButton() == reload);
    showHwBindings();
}

void KbWidget::on_fwUpdButton_clicked(){
    if(device->k95InHardwareMode() || (device->k95HwSlots() && !device->k95ModeDecided())){
        QMessageBox::information(this, tr("Firmware update"),
                                 tr("<center>The keyboard is in hardware mode.<br />Select a software profile to update its firmware.</center>"));
        return;
    }
    // If alt is pressed, ignore upgrades and go straight to the manual prompt
    if(!(qApp->keyboardModifiers() & Qt::AltModifier)){
        // Check version numbers
        if(!MainWindow::mainWindow->kbfw->hasDownloaded()){
            ui->fwUpdButton->setText(tr("Checking..."));
            ui->fwUpdButton->setEnabled(false);
        }
        const CkbVersionNumber newVersion = MainWindow::mainWindow->kbfw->versionForBoard(device->productID, true);
        const CkbVersionNumber& oldVersion = device->firmware.app;
        ui->fwUpdButton->setEnabled(true);
        updateFwButton();
        if(newVersion.isNull()){
            if(QMessageBox::question(this, tr("Firmware update"), tr("<center>There was a problem getting the status for this device.<br />Would you like to select a file manually?</center>")) != QMessageBox::Yes)
                return;
            // "Yes" -> fall through to browse file
        } else if(newVersion.CkbTooOld()){
            QMessageBox::information(this, tr("Firmware update"), tr("<center>There is a new firmware available for this device (v%1).<br />However, it requires a newer version of ckb-next.<br />Please upgrade ckb-next and try again.</center>").arg(newVersion.toString()));
            return;
        } else if(newVersion <= oldVersion){
            if(QMessageBox::question(this, tr("Firmware update"), tr("<center>Your firmware is already up to date.<br />Would you like to select a file manually?</center>")) != QMessageBox::Yes)
                return;
            // "Yes" -> fall through to browse file
        } else {
            // Automatic upgrade. Fetch file from web.
            // FwUpgradeDialog can't be parented to KbWidget because KbWidget may be deleted before the dialog exits
            FwUpgradeDialog dialog(parentWidget(), newVersion, QByteArray(), device);
            dialog.exec();
            return;
        }
    }
    // Browse for file
    QString path = QFileDialog::getOpenFileName(this, tr("Select firmware file"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), tr("Firmware blobs (*.bin)"));
    if(path.isEmpty())
        return;
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)){
        QMessageBox::warning(parentWidget(), tr("Error"), tr("<center>File could not be read.</center>"));
        return;
    }
    QByteArray blob = file.readAll();
    FwUpgradeDialog dialog(parentWidget(), CkbVersionNumber(), blob, device);
    dialog.exec();
}

void KbWidget::on_layoutBox_activated(int index){
    // Can't use currentIndexChanged as it fires when the GUI is first drawn
    // before the layout has been initialised
    int idxLayout = ui->layoutBox->itemData(index).toInt();
    KeyMap::Layout layout = (KeyMap::Layout)idxLayout;
    // Only set the layout if it was changed
    if(layout == device->getCurrentLayout())
        return;
    QString layoutSettingsPath("Devices/%1/hwLayout");
    CkbSettings::set(layoutSettingsPath.arg(device->usbSerial), KeyMap::getLayout(layout));
    device->layout(layout, true);
    // The HW Binding editor takes the new key map and converts a new Text with the new layout (it kept the old ones until the mode
    // changed)
    if(device->k95HwSlots())
        showHwBindings();
}

void KbWidget::switchToProfile(const QString& profile, const QString& serial){
    if(device->hwFlowBusy() || device->k95Recording()) return;
    if(!serial.isEmpty() && device->usbSerial.compare(serial, Qt::CaseInsensitive) != 0)
        return;

    int len = device->profiles().length();
    for(int i = 0; i < len; i++){
        KbProfile* loopProfile = device->profiles().at(i);
        if(loopProfile->name() != profile)
            continue;

        qDebug() << "Switching" << this->name() << "to" << profile;
        device->setCurrentProfile(loopProfile);

        // Also update the dropdown
        ui->profileBox->setCurrentIndex(i);
        return;
    }
}

// Resolves a --profile-select/--mode-select CLI selector ("next"/"prev"/"first"/"last",
// already lowercased and grammar-checked by main.cpp before it reached the wire, or a
// positive 1-based integer index) against the current 0-based index and item count.
// Returns the target 0-based index, or -1 for a no-op (out-of-range absolute index; count==0
// guards only against %0 UB, see note below).
static int resolveSelectorIndex(const QString& selector, int currentIndex, int count){
    if(count <= 0)
        return -1;

    if(selector == QLatin1String("first"))
        return 0;
    if(selector == QLatin1String("last"))
        return count - 1;
    if(selector == QLatin1String("next"))
        return (currentIndex + 1) % count;
    if(selector == QLatin1String("prev"))
        return (currentIndex - 1 + count) % count;

    bool ok;
    int idx = selector.toInt(&ok); // 1-based
    if(!ok || idx < 1 || idx > count)
        return -1;
    return idx - 1;
}

void KbWidget::switchToProfileAt(const QString& selector, const QString& serial){
    if(device->hwFlowBusy() || device->k95Recording()) return;
    if(!serial.isEmpty() && device->usbSerial.compare(serial, Qt::CaseInsensitive) != 0)
        return;

    int count = device->profiles().length();
    int current = device->indexOf(device->currentProfile());
    int target = resolveSelectorIndex(selector, current, count);
    if(target < 0)
        return;

    KbProfile* targetProfile = device->profiles().at(target);
    qDebug() << "Switching" << this->name() << "to" << targetProfile->name();
    device->setCurrentProfile(targetProfile);

    // Also update the dropdown
    ui->profileBox->setCurrentIndex(target);
}

void KbWidget::showModeCountWarning(int loadedModes, int daemonModes){
    QMessageBox::warning(this, tr("Too many modes"),
        tr("This profile has %1 modes, but the running ckb-next-daemon only supports %2 mode slots.\n\n"
           "The extra modes will not be synced to the daemon correctly. Restart ckb-next-daemon "
           "with a higher --modecount value to support more modes; see the ckb-next documentation "
           "for details.").arg(loadedModes).arg(daemonModes));
}

void KbWidget::switchToMode(const QString& mode, const QString& serial){
    if(device->hwFlowBusy() || device->k95Recording()) return;
    if(!serial.isEmpty() && device->usbSerial.compare(serial, Qt::CaseInsensitive) != 0)
        return;

    KbProfile* currentProfile = device->currentProfile();
    int len = currentProfile->modes().length();

    for(int i = 0; i < len; i++){
        KbMode* loopMode = currentProfile->modes().at(i);
        if(loopMode->name() != mode)
            continue;

        qDebug() << "Switching" << this->name() << "to mode" << mode << "in" << currentProfile->name();
        device->setCurrentMode(loopMode);

        return;
    }
}

void KbWidget::switchToModeAt(const QString& selector, const QString& serial){
    if(device->hwFlowBusy() || device->k95Recording()) return;
    if(!serial.isEmpty() && device->usbSerial.compare(serial, Qt::CaseInsensitive) != 0)
        return;

    KbProfile* currentProfile = device->currentProfile();
    int count = currentProfile->modes().length();
    int current = currentProfile->indexOf(currentProfile->currentMode());
    int target = resolveSelectorIndex(selector, current, count);
    if(target < 0)
        return;

    KbMode* targetMode = currentProfile->modes().at(target);
    qDebug() << "Switching" << this->name() << "to mode" << targetMode->name() << "in" << currentProfile->name();
    device->setCurrentMode(targetMode);
}

void KbWidget::on_pollRateBox_currentIndexChanged(int arg1) {
    if(arg1 == -1)
        return;
    const QString str = ui->pollRateBox->itemText(arg1);
    ui->pollRateBox->setEnabled(false);
    device->setPollRate(str.left(str.indexOf(QLatin1String(" ms"))));
}

// Returns true if a match is found
static inline bool checkForWinInfoMatch(KbWindowInfo* kbinfo, XWindowInfo* wininfo) {
    if(kbinfo->isEmpty() || !kbinfo->isEnabled())
        return false;

    QVector<KbWindowInfo::MatchPair>& rules = kbinfo->items;
    bool result = false;
    for(int i = 0; i < rules.length(); i++){
        const KbWindowInfo::MatchPair& mp = rules.at(i);

        Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
        if(mp.flags.testFlag(KbWindowInfo::MATCH_CASE_INSENSITIVE))
            sensitivity = Qt::CaseInsensitive;

        const QString* target = nullptr;

        switch(mp.type){
        case KbWindowInfo::MATCH_TYPE_WINDOW_TITLE:
            target = &wininfo->windowTitle;
            break;
        case KbWindowInfo::MATCH_TYPE_PROGRAM_PATH:
            target = &wininfo->program;
            break;
        case KbWindowInfo::MATCH_TYPE_WM_CLASS_NAME:
            target = &wininfo->wm_class_name;
            break;
        case KbWindowInfo::MATCH_TYPE_WM_INSTANCE_NAME:
            target = &wininfo->wm_instance_name;
            break;
        default:
            qDebug() << "Invalid match type" << mp.type;
            return false;
        }

        // Do the comparison
        if(mp.flags.testFlag(KbWindowInfo::MATCH_SUBSTRING))
            result = target->contains(mp.item, sensitivity);
        else if(mp.flags.testFlag(KbWindowInfo::MATCH_STARTS_WITH))
            result = target->startsWith(mp.item, sensitivity);
        else if(mp.flags.testFlag(KbWindowInfo::MATCH_ENDS_WITH))
            result = target->endsWith(mp.item, sensitivity);
        else
            result = !target->compare(mp.item, sensitivity);

        // If it's an OR and we found a match, return immediately
        // If it's an AND and we haven't found a match, also return immediately
        if(mp.op == KbWindowInfo::MATCH_OP_OR && result)
            break;
        else if(mp.op == KbWindowInfo::MATCH_OP_AND && !result)
            break;
    }
    return result;
}

void KbWidget::switchToModeByFocus(XWindowInfo win) {
    if(device->hwFlowBusy() || device->k95Recording()) return;
    if(win.isEmpty())
        return;

    KbProfile* currentProfile = device->currentProfile();
    // The keyboard, not the GUI, picks the slot of the hardware profile of the slots
    if(device->isHwSlotProfile(currentProfile))
        return;
    int len = currentProfile->modes().length();
    for(int i = 0; i < len; i++)
    {
        KbMode* loopMode = currentProfile->modes().at(i);
        if(!checkForWinInfoMatch(loopMode->winInfo(), &win))
            continue;

        if(!prevmode)
            prevmode = currentMode;

        // Set the new mode
        device->setCurrentMode(loopMode);
        return;
    }
    // If we got here, we found no match
    const int mode = currentProfile->indexOf(prevmode);
    if(prevmode && mode != -1)
        device->setCurrentMode(prevmode);

    prevmode = nullptr;
}

void KbWidget::openEventMgr(KbMode* mode) {
    KbModeEventMgr* mgr = new KbModeEventMgr(this, mode);
    // We set this attribute so that we don't have to free it
    mgr->setAttribute(Qt::WA_DeleteOnClose);
    mgr->show();
}

void KbWidget::on_modesList_doubleClicked(const QModelIndex& index) {
    if(device->hwFlowBusy() || device->isHwSlotProfile(device->currentProfile())) return;
    if(index.column() != ModeListTableModel::COL_EVENT_ICON)
        return;
    // If the current state is "enabled", the previous one was "disabled", which means the user
    // most likely just wanted to edit, and not disable it, so re-enable it.
    if(!currentMode->winInfo()->isEnabled())
        currentMode->winInfo()->setEnabled(true);
    openEventMgr(currentMode);
}

void KbWidget::on_modesList_clicked(const QModelIndex& index) {
    if(device->hwFlowBusy() || device->isHwSlotProfile(device->currentProfile())) return;
    if(index.column() != ModeListTableModel::COL_EVENT_ICON || index.row() > device->currentProfile()->modeCount() - 1)
        return;
    currentMode->winInfo()->setEnabled(!currentMode->winInfo()->isEnabled());
    if(currentMode->winInfo()->isEmpty())
        openEventMgr(currentMode);
}
