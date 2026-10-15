#ifndef HWSAVECONTROLLER_H
#define HWSAVECONTROLLER_H

#include <QObject>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <array>
#include <functional>
#include "hwsaveflow.h"
#include "hwsavedevice.h"

class QWidget;
class QProgressDialog;

// GUI-thread adapter. It owns a K95P notification transaction from the first
// preparation line through the final cache marker, including user dialogs.
class HwSaveController : public QObject {
    Q_OBJECT
public:
    explicit HwSaveController(HwSaveDevice* device, QWidget* parent);
    // rgbRequired: the slots whose read-back after a save must answer their lighting (:hwrgb)
    bool start(const std::vector<HwSaveFlow::Slot>& saveSlots, bool all,
               const std::array<bool, 3>& rgbRequired);
    // The erase of slot mode (1..3) with its line (Kb::k95EraseLine), after the user confirmed it
    bool startErase(unsigned mode, const std::string& line);
    bool busy() const { return active_; }

signals:
    void busyChanged(bool busy);

private:
    QPointer<HwSaveDevice> device_;
    QWidget* parent_;
    QProgressDialog* progress_ = nullptr;
    QTimer timer_;
    QElapsedTimer clock_;
    HwSaveFlow flow_;
    std::array<bool, 3> rgbRequired_{{false, false, false}};
    std::array<QString, 3> names_;                       // the names the slots are saved with, for the warning
    bool active_ = false;
    bool uncertain_ = false;
    bool refreshPendingSend_ = false;
    std::string refreshLine_;
    qint64 refreshSendDeadline_ = 0;
    bool dispatchingSend_ = false;
    std::vector<std::function<void()>> deferredSignals_;

    void pump();
    void drainSignals();
    void sendRefresh(const std::string& line);
    bool begin(const QString& title, const QString& label);
    void showDialog(const HwSaveFlow::Event& event);
    void finish(const QString& reason);
    static QString meaning(const HwSaveFlow::Outcome& outcome);
public:
    // The warning before a save in words: the flow's "mode N:<word>,...;<word>;" made sentences, a block for each slot, named
    static QString warningText(const std::string& words, const std::array<QString, 3>& names);
};

#endif
