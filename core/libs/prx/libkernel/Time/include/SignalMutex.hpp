#pragma once

#include <mutex>

namespace TimedWait {
extern "C" void EnterSignalCriticalSection_nid_no_patch();
extern "C" void LeaveSignalCriticalSection_nid_no_patch();

class SignalMutex {
public:
    void lock() {
        EnterSignalCriticalSection_nid_no_patch();
        try { mutex.lock(); }
        catch (...) { LeaveSignalCriticalSection_nid_no_patch(); throw; }
    }
    bool try_lock() {
        EnterSignalCriticalSection_nid_no_patch();
        try { if (mutex.try_lock()) return true; }
        catch (...) { LeaveSignalCriticalSection_nid_no_patch(); throw; }
        LeaveSignalCriticalSection_nid_no_patch();
        return false;
    }
    void unlock() {
        mutex.unlock();
        LeaveSignalCriticalSection_nid_no_patch();
    }
private:
    std::mutex mutex;
};
}  // namespace TimedWait
