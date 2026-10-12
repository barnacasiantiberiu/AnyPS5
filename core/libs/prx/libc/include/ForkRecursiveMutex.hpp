#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_FORKRECURSIVEMUTEX_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_FORKRECURSIVEMUTEX_HPP

#include <cerrno>
#include <cstddef>
#include <memory>
#include <mutex>
#ifdef _WIN32
#include <atomic>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

class ForkRecursiveMutex {
public:
    void lock() {
#ifdef _WIN32
        const DWORD current = GetCurrentThreadId();
        if (owner.load(std::memory_order_relaxed) != current) {
            AcquireSRWLockExclusive(&mutex);
            owner.store(current, std::memory_order_relaxed);
        }
#else
        mutex.lock();
#endif
        ++depth;
    }
    bool try_lock() {
#ifdef _WIN32
        const DWORD current = GetCurrentThreadId();
        if (owner.load(std::memory_order_relaxed) != current) {
            if (!TryAcquireSRWLockExclusive(&mutex)) return false;
            owner.store(current, std::memory_order_relaxed);
        }
#else
        if (!mutex.try_lock()) return false;
#endif
        ++depth;
        return true;
    }
    void unlock() noexcept {
#ifdef _WIN32
        if (--depth == 0) {
            owner.store(0, std::memory_order_relaxed);
            ReleaseSRWLockExclusive(&mutex);
        }
#else
        --depth;
        mutex.unlock();
#endif
    }
    bool RebindForForkChild() noexcept {
        // Only internal owners call this in the single-threaded fork child.
        // The copied native owner belongs to the parent. Windows uses an SRW
        // gate so rebinding does not discard a winpthreads event HANDLE.
        try {
            const auto inheritedDepth = depth;
#ifdef _WIN32
            InitializeSRWLock(&mutex);
            owner.store(0, std::memory_order_relaxed);
#else
            std::construct_at(&mutex);
#endif
            depth = 0;
            while (depth != inheritedDepth) lock();
        } catch (...) {
            errno = 35;
            return false;
        }
        return true;
    }

private:
#ifdef _WIN32
    SRWLOCK mutex = SRWLOCK_INIT;
    std::atomic<DWORD> owner{0};
#else
    std::recursive_mutex mutex;
#endif
    std::size_t depth = 0;
};

#endif
