#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_FORKMUTEXOWNER_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_FORKMUTEXOWNER_HPP

#include "prx/libc/include/GuestProcess.hpp"
#include <cerrno>
#include <stdexcept>

template<class Mutex>
class ForkMutexOwner {
public:
    ForkMutexOwner(Mutex& mutex, unsigned priority) : mutex(mutex) {
        const GuestForkHooks hooks{priority, Prepare, Parent, Child, this};
        if (GuestRegisterForkHooks_nid_no_patch(&hooks) != 0)
            throw std::runtime_error("libc: registering mutex fork owner failed");
    }
    ForkMutexOwner(const ForkMutexOwner&) = delete;
    ForkMutexOwner& operator=(const ForkMutexOwner&) = delete;

private:
    static bool Prepare(void* context) {
        auto& self = *static_cast<ForkMutexOwner*>(context);
        self.prepared = self.mutex.try_lock();
        if (!self.prepared) errno = 35;
        return self.prepared;
    }
    static bool Release(void* context) noexcept {
        auto& self = *static_cast<ForkMutexOwner*>(context);
        if (self.prepared) {
            self.prepared = false;
            self.mutex.unlock();
        }
        return true;
    }
    static bool Parent(void* context, bool) noexcept { return Release(context); }
    static bool Child(void* context) noexcept {
        auto& self = *static_cast<ForkMutexOwner*>(context);
        if constexpr (requires { self.mutex.RebindForForkChild(); }) {
            if (self.prepared && !self.mutex.RebindForForkChild()) return false;
        }
        return Release(context);
    }
    Mutex& mutex;
    bool prepared = false;
};

#endif
