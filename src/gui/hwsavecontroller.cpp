#include "hwsavecontroller.h"

#include <QMessageBox>
#include <QProgressDialog>
#include <QWidget>
#include <cerrno>
#include "hwsavedevice.h"

HwSaveController::HwSaveController(HwSaveDevice* device, QWidget* parent)
    : QObject(parent), device_(device), parent_(parent){
    timer_.setInterval(100);
    connect(&timer_, &QTimer::timeout, this, [this](){
        if(!active_ || uncertain_) return;
        const qint64 now = clock_.elapsed();
        if(refreshPendingSend_){
            if(now < refreshSendDeadline_) sendRefresh(refreshLine_);
            else {
                refreshPendingSend_ = false;
                if(device_) device_->invalidateCache();
                flow_.refresh_result(false); // proven zero-byte delivery
            }
        }
        if(!refreshPendingSend_) flow_.retry_send(now);
        flow_.tick(now);
        pump();
    });
    connect(device, &HwSaveDevice::lineReceived, this, [this](const QString& line){
        if(!active_ || uncertain_) return;
        if(dispatchingSend_){ deferredSignals_.push_back([this, line](){
            if(active_ && !uncertain_){ flow_.line(line.toStdString(), clock_.elapsed()); pump(); }
        }); return; }
        flow_.line(line.toStdString(), clock_.elapsed());
        pump();
    });
    connect(device, &HwSaveDevice::cacheRequest, this, [this](const QString& line){
        if(dispatchingSend_){ deferredSignals_.push_back([this, line](){
            if(active_ && !uncertain_) sendRefresh(line.toStdString());
        }); return; }
        if(active_ && !uncertain_) sendRefresh(line.toStdString());
    });
    connect(device, &HwSaveDevice::cacheFinished, this, [this](bool complete){
        if(!active_ || uncertain_) return;
        if(dispatchingSend_){ deferredSignals_.push_back([this, complete](){
            if(active_ && !uncertain_){ flow_.refresh_result(complete); pump(); }
        }); return; }
        flow_.refresh_result(complete);
        pump();
    });
    connect(device, &HwSaveDevice::cacheAlive, this, [this](){
        if(active_ && !uncertain_) flow_.refresh_alive(clock_.elapsed());
    });
    connect(device, &HwSaveDevice::deviceGone, this, [this](){
        if(dispatchingSend_){ deferredSignals_.push_back([this](){
            device_ = nullptr; flow_.device_gone(); pump();
        }); return; }
        device_ = nullptr;
        if(!active_) return;
        flow_.device_gone();
        pump();
    });
}

bool HwSaveController::start(const std::vector<HwSaveFlow::Slot>& saveSlots, bool all,
                             const std::array<bool, 3>& rgbRequired){
    if(active_ || !device_ || !device_->begin()) return false;
    active_ = true; uncertain_ = false; rgbRequired_ = rgbRequired;
    clock_.start();
    if(!flow_.start(saveSlots, all, clock_.elapsed())){
        device_->end(); active_ = false; return false;
    }
    progress_ = new QProgressDialog(parent_);
    progress_->setWindowTitle(tr("Hardware save"));
    progress_->setLabelText(tr("Checking hardware slots…"));
    progress_->setCancelButton(nullptr);
    progress_->setRange(0, 1000);
    progress_->setMinimumDuration(400);
    progress_->setAutoClose(false);
    progress_->setAutoReset(false);
    timer_.start();
    emit busyChanged(true);
    pump();
    return true;
}

void HwSaveController::sendRefresh(const std::string& line){
    if(!device_ || uncertain_) return;
    if(!refreshPendingSend_ || refreshLine_ != line){
        refreshLine_ = line;
        refreshSendDeadline_ = clock_.elapsed() + 30000;
    }
    dispatchingSend_ = true;
    const HwSavePipe::Result result = device_->send(line);
    dispatchingSend_ = false;
    if(result.status == HwSavePipe::Sent){
        refreshPendingSend_ = false;
        refreshLine_.clear();
    } else if(result.status == HwSavePipe::NotSent &&
              (result.error == EAGAIN || result.error == EWOULDBLOCK || result.error == EINTR) &&
              clock_.elapsed() < refreshSendDeadline_){
        refreshPendingSend_ = true;
    } else if(result.status == HwSavePipe::NotSent){
        refreshPendingSend_ = false;
        device_->invalidateCache();
        flow_.refresh_result(false); // no daemon request is pending
    } else {
        uncertain_ = true;
        device_->invalidateCache();
        timer_.stop();
        if(progress_) progress_->setLabelText(tr("Command channel uncertain. Disconnect the device before retrying."));
        QMessageBox::critical(parent_, tr("Hardware save"),
                              tr("The command channel may contain a partial request. No further command will be sent to this device until it is disconnected."));
    }
    drainSignals();
}

void HwSaveController::drainSignals(){
    std::vector<std::function<void()>> pending;
    pending.swap(deferredSignals_);
    for(const auto& callback : pending) callback();
}

void HwSaveController::showDialog(const HwSaveFlow::Event& event){
    // The progress dialog is itself a top-level window. Leaving it visible
    // while opening a modal message box can put it *above* the confirmation
    // on some window managers, making Yes/Cancel appear inaccessible.
    const bool resumeProgress = progress_ && progress_->isVisible();
    if(resumeProgress) progress_->hide();
    const QString detail = QString::fromStdString(event.text).replace(';', "\n");
    if(event.dialog == HwSaveFlow::Confirm){
        const auto answer = QMessageBox::warning(parent_, tr("Save to hardware"),
            tr("Review the affected slot(s):\n%1\nContinue with the hardware write?").arg(detail),
            QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
        flow_.choose(event.id, answer == QMessageBox::Yes, clock_.elapsed());
    } else if(event.dialog == HwSaveFlow::Failed){
        const auto answer = QMessageBox::warning(parent_, tr("Hardware write failed"),
            tr("Slot M%1 failed: %2\nRetry this slot once, or abort the remaining writes?")
                .arg(event.mode).arg(detail),
            QMessageBox::Retry | QMessageBox::Abort, QMessageBox::Abort);
        flow_.choose(event.id, answer == QMessageBox::Retry, clock_.elapsed());
    } else {
        QMessageBox::warning(parent_, tr("Hardware result unknown"),
            tr("%1\nNo further writes will be sent. The application must keep this device busy until the pending reply arrives or it disconnects.").arg(detail));
        flow_.choose(event.id, false, clock_.elapsed());
        if(progress_) progress_->setLabelText(tr("Waiting for the pending daemon reply…"));
    }
    if(resumeProgress && progress_ && flow_.busy()) progress_->show();
}

QString HwSaveController::meaning(const HwSaveFlow::Outcome& outcome){
    // What each outcome means, under its line ("checked(same)" alone says little)
    const std::string word = HwSaveFlow::outcome_word(outcome);
    if(outcome.verdict == "checked"){
        if(word == "same") return tr("No change found: the keyboard already holds what the editor shows.");
        if(word == "new") return tr("The slot is empty on the keyboard: saving would create it.");
        if(word == "refused") return tr("This slot cannot be saved as it is.");
        if(word == "changed"){
            QStringList parts;
            for(const std::string& p : HwSaveFlow::changed_parts(outcome)) parts << QString::fromStdString(p);
            return parts.isEmpty() ? tr("Changes found: saving would rewrite the slot.")
                                   : tr("Changes found (%1): saving would rewrite the slot.").arg(parts.join(", "));
        }
        return QString();
    }
    if(word == "written") return tr("Saved and read back: the keyboard holds what the editor shows.");
    if(word == "unchanged") return tr("No change found: not written again.");
    if(word == "empty") return tr("Empty on the keyboard: Save ALL does not create a slot, Save slot does.");
    if(word == "recovery") return tr("An earlier save of this slot did not finish: Save slot writes it again.");
    if(word == "refused") return tr("Not saved: this slot cannot be saved as it is.");
    if(word == "failed" && outcome.no_space)
        return tr("Not saved: not enough free space in the keyboard's memory for this slot (it needs %1 sectors of 4 KiB, %2 are free; "
                  "the sectors the slot takes now are counted as used). Nothing was written.").arg(outcome.need_sectors).arg(outcome.free_sectors);
    if(word == "failed") return tr("Not saved: the save failed.");
    if(word == "not-tried") return tr("Not tried.");
    return QString();
}

void HwSaveController::finish(const QString& reason){
    timer_.stop();
    if(progress_){ progress_->close(); progress_->deleteLater(); progress_ = nullptr; }
    if(device_ && !uncertain_) device_->end();
    active_ = false;
    emit busyChanged(false);
    QStringList lines;
    for(const auto& outcome : flow_.outcomes()){
        lines << tr("M%1: %2").arg(outcome.mode).arg(QString::fromStdString(HwSaveFlow::outcome_text(outcome)));
        const QString what = meaning(outcome);
        if(!what.isEmpty()) lines << "    " + what;
    }
    if(flow_.check_only())
        lines << tr("Saving to hardware is disabled in this build: the slots were only checked.");
    for(const auto& outcome : flow_.outcomes())
        if(outcome.hwmode_failed){
            lines << tr("The keyboard did not go back to hardware mode after the save: disconnect it and connect it again.");
            break;
        }
    // (the slots could not be read back, or do not hold what was saved: the edits stay, and the next reading asks about them)
    if(flow_.refresh_failed())
        lines << tr("The saved slots could not be read back as saved; further saves are disabled. Your edits are kept: "
                    "disconnect the keyboard and connect it again (or restart ckb-next) to read it again, and you will be "
                    "asked whether to reload a slot that differs or keep your edits.");
    if(reason == "device-gone") lines << tr("Device disconnected.");
    if(!lines.isEmpty()) QMessageBox::information(parent_, tr("Save result"), lines.join('\n'));
}

void HwSaveController::pump(){
    while(true){
        const auto events = flow_.take_events();
        if(events.empty()) return;
        for(const auto& event : events){
            if(event.kind == HwSaveFlow::Send){
                if(!device_){ flow_.device_gone(); continue; }
                dispatchingSend_ = true;
                const auto result = device_->send(event.text);
                dispatchingSend_ = false;
                const auto verdict = result.status == HwSavePipe::Sent ? HwSaveFlow::Sent :
                    result.status == HwSavePipe::NotSent ? HwSaveFlow::NotSent : HwSaveFlow::Uncertain;
                flow_.send_result(event.id, verdict, clock_.elapsed());
                drainSignals();
            } else if(event.kind == HwSaveFlow::Dialog){
                showDialog(event);
            } else if(event.kind == HwSaveFlow::Progress){
                if(progress_){
                    progress_->setValue(int(event.done));
                    // The checks read the slots first (their own bar), then a save starts it again with its phases
                    progress_->setLabelText(event.text == "check" ? tr("M%1: checking (reading the slot)\u2026").arg(event.mode) :
                                            tr("M%1: %2").arg(event.mode).arg(QString::fromStdString(event.text)));
                }
            } else if(event.kind == HwSaveFlow::Refresh){
                unsigned mask = 0, rgbRequired = 0;
                for(const auto& outcome : flow_.outcomes()) if(outcome.verdict == "written"){
                    const unsigned bit = 1u << (outcome.mode - 1);
                    mask |= bit;
                    if(rgbRequired_[outcome.mode - 1]) rgbRequired |= bit;
                }
                std::string request;
                if(!device_ || !device_->refresh(mask, rgbRequired, request)){
                    flow_.refresh_result(false);
                } else {
                    if(progress_) progress_->setLabelText(tr("Refreshing saved slots…"));
                    sendRefresh(request);
                }
            } else if(event.kind == HwSaveFlow::ChannelUncertain){
                uncertain_ = true;
                if(device_) device_->invalidateCache();
                timer_.stop();
                if(progress_) progress_->setLabelText(tr("Command channel uncertain; disconnect device."));
                QMessageBox::critical(parent_, tr("Hardware save"),
                    tr("A command may have been only partly delivered. The device remains locked until disconnect."));
            } else if(event.kind == HwSaveFlow::Summary){
                finish(QString::fromStdString(event.text));
            }
        }
    }
}
