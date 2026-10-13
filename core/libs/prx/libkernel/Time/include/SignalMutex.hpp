#pragma once

#include <mutex>

namespace TimedWait {
void EnterSignalCriticalSection();
void LeaveSignalCriticalSection();

class SignalMutex {
public:
    void lock() {
        EnterSignalCriticalSection();
        try { mutex.lock(); }
        catch (...) { LeaveSignalCriticalSection(); throw; }
    }
    bool try_lock() {
        EnterSignalCriticalSection();
        try { if (mutex.try_lock()) return true; }
        catch (...) { LeaveSignalCriticalSection(); throw; }
        LeaveSignalCriticalSection();
        return false;
    }
    void unlock() {
        mutex.unlock();
        LeaveSignalCriticalSection();
    }
private:
    std::mutex mutex;
};
}  // namespace TimedWait
