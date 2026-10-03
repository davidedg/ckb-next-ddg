#include "kbhwsavedevice.h"
#include "kb.h"

KbHwSaveDevice::KbHwSaveDevice(Kb* kb, QObject* parent)
    : HwSaveDevice(parent), kb_(kb){
    connect(kb, &Kb::hwSaveLine, this, &HwSaveDevice::lineReceived);
    connect(kb, &Kb::k95CacheRequest, this, &HwSaveDevice::cacheRequest);
    connect(kb, &Kb::k95CacheFinished, this, &HwSaveDevice::cacheFinished);
    connect(kb, &Kb::k95CacheLineAccepted, this, &HwSaveDevice::cacheAlive);
    connect(kb, &QObject::destroyed, this, [this](){ kb_ = nullptr; emit deviceGone(); });
}

bool KbHwSaveDevice::begin(){ return kb_ && kb_->beginHwFlow(); }
void KbHwSaveDevice::end(){ if(kb_) kb_->endHwFlow(); }
HwSavePipe::Result KbHwSaveDevice::send(const std::string& line){
    return kb_ ? kb_->sendHwFlowLine(line) : HwSavePipe::Result{HwSavePipe::Invalid, 0};
}
bool KbHwSaveDevice::refresh(unsigned mask, unsigned rgbRequired, std::string& request){
    if(!kb_)
        return false;
    // The written slots: their read-back replaces the drafts, and is checked against them (hwslot1)
    kb_->k95CommitOnRefresh(mask);
    return kb_->startK95CacheRefresh(mask, rgbRequired, request);
}
void KbHwSaveDevice::invalidateCache(){ if(kb_) kb_->invalidateK95Cache(); }
