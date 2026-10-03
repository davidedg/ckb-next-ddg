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
    void showDialog(const HwSaveFlow::Event& event);
    void finish(const QString& reason);
    static QString meaning(const HwSaveFlow::Outcome& outcome);
};

#endif
