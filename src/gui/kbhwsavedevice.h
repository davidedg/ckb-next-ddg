#ifndef KBHWSAVEDEVICE_H
#define KBHWSAVEDEVICE_H

#include <QPointer>
#include "hwsavedevice.h"

class Kb;

class KbHwSaveDevice : public HwSaveDevice {
    Q_OBJECT
public:
    explicit KbHwSaveDevice(Kb* kb, QObject* parent = nullptr);
    bool begin() override;
    void end() override;
    HwSavePipe::Result send(const std::string& line) override;
    bool refresh(unsigned mask, unsigned rgbRequired, std::string& request) override;
    void invalidateCache() override;
private:
    QPointer<Kb> kb_;
};

#endif
