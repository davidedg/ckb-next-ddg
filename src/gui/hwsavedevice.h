#ifndef HWSAVEDEVICE_H
#define HWSAVEDEVICE_H

#include <QObject>
#include <string>
#include "hwsavepipe.h"

// Thin boundary between the GUI-thread controller and a command/notification
// channel. Tests supply a fake channel; production wraps one Kb object.
class HwSaveDevice : public QObject {
    Q_OBJECT
public:
    explicit HwSaveDevice(QObject* parent = nullptr) : QObject(parent) {}
    virtual bool begin() = 0;
    virtual void end() = 0;
    virtual HwSavePipe::Result send(const std::string& line) = 0;
    virtual bool refresh(unsigned mask, unsigned rgbRequired, std::string& request) = 0;
    // The read-back after the erase of slot mode (1..3): erased (the daemon said ok), the slot is now empty and replaces its draft;
    // else (it failed) it is read again as the daemon has it (nothing, until it is read on demand)
    virtual bool refreshAfterErase(unsigned mode, bool erased, std::string& request) = 0;
    virtual void invalidateCache() = 0;
signals:
    void lineReceived(const QString& line);
    void cacheRequest(const QString& line);
    void cacheFinished(bool complete);
    void cacheAlive();   // a line of the read-back was accepted: its guard starts again
    void deviceGone();
};

#endif
