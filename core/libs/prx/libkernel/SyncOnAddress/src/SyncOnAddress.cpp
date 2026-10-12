#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestProcess.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include "prx/libkernel/Time/include/TimedWait.hpp"

extern "C" int* APS5_VABI __error_nid_postfix();

namespace {

constexpr int SYNC_ON_ADDRESS_OK = 0;

constexpr int UMTX_OP_RESERVED1 = 1;
constexpr int UMTX_OP_WAIT = 2;
constexpr int UMTX_OP_WAKE = 3;
constexpr int UMTX_OP_WAIT_UINT = 11;
constexpr int UMTX_OP_WAIT_UINT_PRIVATE = 15;
constexpr int UMTX_OP_WAKE_PRIVATE = 16;
constexpr int UMTX_OP_NWAKE_PRIVATE = 21;
constexpr int UMTX_OP_SEM2_WAIT = 23;
constexpr int UMTX_OP_SEM2_WAKE = 24;
constexpr int UMTX_OP_MAX = 27;
constexpr std::uint32_t UMTX_ABSTIME = 1;
constexpr std::uint32_t USEM_HAS_WAITERS = 0x80000000u;
constexpr std::uint32_t USEM_MAX_COUNT = 0x7fffffffu;
constexpr std::uint32_t USYNC_PROCESS_SHARED = 1;
constexpr int GUEST_CLOCK_REALTIME = 0;

constexpr int GUEST_EFAULT = 14;
constexpr int GUEST_EINVAL = 22;
constexpr int GUEST_EOPNOTSUPP = 45;
constexpr int GUEST_ETIMEDOUT = 60;

constexpr std::int64_t NANOS_PER_SECOND = 1000000000;
constexpr std::uint64_t UNBOUNDED_WAIT_NANOS = 1ULL << 62;

struct UmtxTime {
    KernelTimespec timeout;
    std::uint32_t flags;
    std::uint32_t clockId;
};

struct AddressWaiter {
    TimedWait::Condition condition;
    bool woken = false;
};

struct Usem2 {
    std::uint32_t count;
    std::uint32_t flags;
};
static_assert(sizeof(Usem2) == 8 && offsetof(Usem2, flags) == 4);

struct SemaphoreState {
    std::mutex mutex;
    std::unordered_map<std::uintptr_t, std::list<AddressWaiter*>> queues;
    bool prepared = false;
};

SemaphoreState& Semaphores() {
    static auto* state = new SemaphoreState();
    return *state;
}

bool PrepareSemaphores(void*) {
    auto& state = Semaphores();
    state.prepared = state.mutex.try_lock();
    if (!state.prepared || !state.queues.empty()) {
        errno = 35;
        return false;
    }
    return true;
}

bool ReleaseSemaphores(void*) noexcept {
    auto& state = Semaphores();
    if (state.prepared) {
        state.prepared = false;
        state.mutex.unlock();
    }
    return true;
}

bool ParentSemaphores(void* context, bool) noexcept { return ReleaseSemaphores(context); }

struct SemaphoreForkRegistration {
    SemaphoreForkRegistration() {
        Semaphores();
        const GuestForkHooks hooks{560, PrepareSemaphores, ParentSemaphores, ReleaseSemaphores, nullptr};
        if (GuestRegisterForkHooks_nid_no_patch(&hooks) != 0)
            throw std::runtime_error("libkernel: registering semaphore fork hooks failed");
    }
} semaphoreForkRegistration;

std::mutex g_waitersLock;
std::unordered_map<std::uintptr_t, std::list<AddressWaiter*>> g_waiters;

template <class TValue>
bool IsAlignedAddress(std::uintptr_t address) {
    return address != 0 && address % alignof(TValue) == 0;
}

template <class TValue>
bool WaitOnAddressUntil(TValue* address, TValue expected, std::optional<std::uint64_t> deadlineNanos, const void* caller) {
    const auto key = reinterpret_cast<std::uintptr_t>(address);
    std::unique_lock<std::mutex> lock(g_waitersLock);
    if (std::atomic_ref<TValue>(*address).load() != expected) {
        return true;
    }

    AddressWaiter waiter;
    auto& queue = g_waiters[key];
    const auto position = queue.insert(queue.end(), &waiter);
    const auto isWoken = [&] { return waiter.woken; };
    const auto waitStart = std::chrono::steady_clock::now();
    if (!deadlineNanos) {
        waiter.condition.Wait(lock, isWoken);
    } else {
        waiter.condition.WaitUntil(lock, *deadlineNanos, isWoken);
    }
    const auto waited = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart);

    const bool woken = waiter.woken;
    if (!woken) {
        queue.erase(position);
        if (queue.empty()) {
            g_waiters.erase(key);
        }
    }
    lock.unlock();
    KernelTraceWait_nid_postfix("addr", caller, static_cast<std::uint64_t>(waited.count()), !woken);
    return woken;
}

template <class TValue>
int WaitOnAddress(TValue* address, TValue expected, const KernelUseconds* timeout, const void* caller) {
    const auto deadline = timeout == nullptr ? std::nullopt : std::optional<std::uint64_t>(TimedWait::DeadlineNanos(*timeout));
    return WaitOnAddressUntil(address, expected, deadline, caller) ? SYNC_ON_ADDRESS_OK : SCE_KERNEL_ERROR_ETIMEDOUT;
}

void WakeAddress(std::uintptr_t key, std::int32_t count, bool wakesAtLeastOne) {
    std::lock_guard<std::mutex> lock(g_waitersLock);
    const auto entry = g_waiters.find(key);
    if (entry == g_waiters.end()) {
        return;
    }
    auto& queue = entry->second;
    for (std::int32_t woken = 0; !queue.empty() && (woken < count || (wakesAtLeastOne && woken == 0)); ++woken) {
        AddressWaiter* waiter = queue.front();
        queue.pop_front();
        waiter->woken = true;
        waiter->condition.NotifyOne();
    }
    if (queue.empty()) {
        g_waiters.erase(entry);
    }
}

std::uint64_t SaturatingNanos(std::int64_t seconds, std::int64_t nanos) {
    if (seconds < 0) {
        return 0;
    }
    if (seconds >= static_cast<std::int64_t>(UNBOUNDED_WAIT_NANOS / NANOS_PER_SECOND)) {
        return UNBOUNDED_WAIT_NANOS;
    }
    const std::int64_t total = seconds * NANOS_PER_SECOND + nanos;
    return total <= 0 ? 0 : static_cast<std::uint64_t>(total);
}

int UmtxDeadline(const void* sizeArgument, const void* timeArgument, std::optional<std::uint64_t>& deadlineNanos) {
    deadlineNanos.reset();
    if (timeArgument == nullptr) {
        return 0;
    }
    UmtxTime time{{0, 0}, 0, GUEST_CLOCK_REALTIME};
    if (reinterpret_cast<std::uintptr_t>(sizeArgument) <= sizeof(KernelTimespec)) {
        time.timeout = *static_cast<const KernelTimespec*>(timeArgument);
    } else {
        time = *static_cast<const UmtxTime*>(timeArgument);
    }
    if (time.timeout.tv_sec < 0 || time.timeout.tv_nsec < 0 || time.timeout.tv_nsec >= NANOS_PER_SECOND) {
        if (sizeArgument == nullptr) {
            NotImplemented_nid_no_patch("_umtx_op timeout with size argument 0 that is not a valid timespec");
        }
        return GUEST_EINVAL;
    }

    std::uint64_t remaining = 0;
    if ((time.flags & UMTX_ABSTIME) == 0) {
        remaining = SaturatingNanos(time.timeout.tv_sec, time.timeout.tv_nsec);
    } else {
        KernelTimespec now{};
        clock_gettime_nid_postfix(static_cast<int>(time.clockId), &now);
        remaining = SaturatingNanos(time.timeout.tv_sec - now.tv_sec, time.timeout.tv_nsec - now.tv_nsec);
    }
    if (remaining < UNBOUNDED_WAIT_NANOS) {
        deadlineNanos = TimedWait::NowNanos() + remaining;
    }
    return 0;
}

template <class TValue>
int UmtxWait(void* object, std::uint64_t expected, const void* sizeArgument, const void* timeArgument, const void* caller) {
    std::optional<std::uint64_t> deadline;
    if (const int error = UmtxDeadline(sizeArgument, timeArgument, deadline); error != 0) {
        return error;
    }
    if (object == nullptr) {
        return GUEST_EFAULT;
    }
    if (!IsAlignedAddress<TValue>(reinterpret_cast<std::uintptr_t>(object))) {
        APS5_INVALID_ARG_EX;
    }
    if (expected > std::numeric_limits<TValue>::max()) {
        return 0;
    }
    return WaitOnAddressUntil(static_cast<TValue*>(object), static_cast<TValue>(expected), deadline, caller) ? 0 : GUEST_ETIMEDOUT;
}

int UmtxWake(void* object, std::uint64_t count, bool isPrivate) {
    if (object == nullptr) {
        return isPrivate ? 0 : GUEST_EFAULT;
    }
    WakeAddress(reinterpret_cast<std::uintptr_t>(object), static_cast<std::int32_t>(count), true);
    return 0;
}

int UmtxNwakePrivate(void* object, std::uint64_t value) {
    const auto count = static_cast<std::int32_t>(value);
    if (count <= 0) {
        return 0;
    }
    if (object == nullptr) {
        return GUEST_EFAULT;
    }
    const auto* addresses = static_cast<void* const*>(object);
    for (std::int32_t index = 0; index < count; ++index) {
        WakeAddress(reinterpret_cast<std::uintptr_t>(addresses[index]), std::numeric_limits<std::int32_t>::max(), true);
    }
    return 0;
}

Usem2* PrivateSemaphore(void* object) {
    if (object == nullptr) return nullptr;
    if (!IsAlignedAddress<Usem2>(reinterpret_cast<std::uintptr_t>(object))) APS5_INVALID_ARG_EX;
    auto* semaphore = static_cast<Usem2*>(object);
    if ((semaphore->flags & USYNC_PROCESS_SHARED) != 0)
        NotImplemented_nid_no_patch("_umtx_op SEM2 process-shared semaphore");
    return semaphore;
}

int UmtxSem2Wait(void* object, const void* sizeArgument, const void* timeArgument, const void* caller) {
    std::optional<std::uint64_t> deadline;
    if (const int error = UmtxDeadline(sizeArgument, timeArgument, deadline); error != 0) return error;
    auto* semaphore = PrivateSemaphore(object);
    if (semaphore == nullptr) return GUEST_EFAULT;
    auto& state = Semaphores();
    std::unique_lock lock(state.mutex);
    std::atomic_ref count(semaphore->count);
    auto value = count.load();
    for (;;) {
        if ((value & USEM_MAX_COUNT) != 0) return 0;
        if (value == USEM_HAS_WAITERS || count.compare_exchange_strong(value, USEM_HAS_WAITERS)) break;
    }
    const auto key = reinterpret_cast<std::uintptr_t>(object);
    AddressWaiter waiter;
    auto& queue = state.queues[key];
    const auto position = [&] {
        try { return queue.insert(queue.end(), &waiter); }
        catch (...) {
            if (queue.empty()) state.queues.erase(key);
            throw;
        }
    }();
    const auto remove = [&] {
        if (waiter.woken) return;
        queue.erase(position);
        if (queue.empty()) state.queues.erase(key);
    };
    const auto started = std::chrono::steady_clock::now();
    try {
        const auto woken = [&] { return waiter.woken; };
        if (deadline) waiter.condition.WaitUntil(lock, *deadline, woken);
        else waiter.condition.Wait(lock, woken);
    } catch (...) {
        if (!lock.owns_lock()) lock.lock();
        remove();
        throw;
    }
    remove();
    const bool woken = waiter.woken;
    lock.unlock();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
    KernelTraceWait_nid_postfix("usem", caller, static_cast<std::uint64_t>(elapsed.count()), !woken);
    return woken ? 0 : GUEST_ETIMEDOUT;
}

int UmtxSem2Wake(void* object) {
    auto* semaphore = PrivateSemaphore(object);
    if (semaphore == nullptr) return GUEST_EFAULT;
    auto& state = Semaphores();
    std::lock_guard lock(state.mutex);
    const auto entry = state.queues.find(reinterpret_cast<std::uintptr_t>(object));
    if (entry == state.queues.end()) return 0;
    auto& queue = entry->second;
    if (queue.size() == 1) std::atomic_ref(semaphore->count).fetch_and(USEM_MAX_COUNT);
    auto* waiter = queue.front();
    queue.pop_front();
    waiter->woken = true;
    waiter->condition.NotifyOne();
    if (queue.empty()) state.queues.erase(entry);
    return 0;
}

}  // namespace

extern "C" {

int APS5_VABI sceKernelSyncOnAddressWait(std::uint32_t* address, std::uint32_t expected, const KernelUseconds* timeout, const char* name) {
    (void)name;
    if (!IsAlignedAddress<std::uint32_t>(reinterpret_cast<std::uintptr_t>(address))) {
        APS5_INVALID_ARG_EX;
    }
    return WaitOnAddress(address, expected, timeout, __builtin_return_address(0));
}

int APS5_VABI sceKernelSyncOnAddressWait8(std::uint8_t* address, std::uint8_t expected, const KernelUseconds* timeout, const char* name) {
    (void)name;
    if (!IsAlignedAddress<std::uint8_t>(reinterpret_cast<std::uintptr_t>(address))) {
        APS5_INVALID_ARG_EX;
    }
    return WaitOnAddress(address, expected, timeout, __builtin_return_address(0));
}

int APS5_VABI sceKernelSyncOnAddressWait16(std::uint16_t* address, std::uint16_t expected, const KernelUseconds* timeout, const char* name) {
    (void)name;
    if (!IsAlignedAddress<std::uint16_t>(reinterpret_cast<std::uintptr_t>(address))) {
        APS5_INVALID_ARG_EX;
    }
    return WaitOnAddress(address, expected, timeout, __builtin_return_address(0));
}

int APS5_VABI sceKernelSyncOnAddressWait32(std::uint32_t* address, std::uint32_t expected, const KernelUseconds* timeout, const char* name) {
    (void)name;
    if (!IsAlignedAddress<std::uint32_t>(reinterpret_cast<std::uintptr_t>(address))) {
        APS5_INVALID_ARG_EX;
    }
    return WaitOnAddress(address, expected, timeout, __builtin_return_address(0));
}

int APS5_VABI sceKernelSyncOnAddressWait64(std::uint64_t* address, std::uint64_t expected, const KernelUseconds* timeout, const char* name) {
    (void)name;
    if (!IsAlignedAddress<std::uint64_t>(reinterpret_cast<std::uintptr_t>(address))) {
        APS5_INVALID_ARG_EX;
    }
    return WaitOnAddress(address, expected, timeout, __builtin_return_address(0));
}

int APS5_VABI sceKernelSyncOnAddressWake(void* address, std::int32_t count) {
    const auto key = reinterpret_cast<std::uintptr_t>(address);
    if (!IsAlignedAddress<std::uint8_t>(key) || count < 0) {
        APS5_INVALID_ARG_EX;
    }

    WakeAddress(key, count, false);
    return SYNC_ON_ADDRESS_OK;
}

int APS5_VABI _umtx_op_nid_postfix(void* object, int operation, std::uint64_t value, void* sizeArgument, void* timeArgument) {
    const void* caller = __builtin_return_address(0);
    int error = 0;
    switch (operation) {
        case UMTX_OP_WAIT:
            error = UmtxWait<std::uint64_t>(object, value, sizeArgument, timeArgument, caller);
            break;
        case UMTX_OP_WAIT_UINT:
        case UMTX_OP_WAIT_UINT_PRIVATE:
            error = UmtxWait<std::uint32_t>(object, value, sizeArgument, timeArgument, caller);
            break;
        case UMTX_OP_WAKE:
        case UMTX_OP_WAKE_PRIVATE:
            error = UmtxWake(object, value, operation == UMTX_OP_WAKE_PRIVATE);
            break;
        case UMTX_OP_NWAKE_PRIVATE:
            error = UmtxNwakePrivate(object, value);
            break;
        case UMTX_OP_SEM2_WAIT:
            error = UmtxSem2Wait(object, sizeArgument, timeArgument, caller);
            break;
        case UMTX_OP_SEM2_WAKE:
            error = UmtxSem2Wake(object);
            break;
        default:
            if (operation < 0 || operation >= UMTX_OP_MAX) {
                error = GUEST_EINVAL;
            } else if (operation == UMTX_OP_RESERVED1) {
                NotImplemented_nid_no_patch("_umtx_op operation 1");
            } else if (operation < UMTX_OP_RESERVED1) {
                error = GUEST_EOPNOTSUPP;
            } else {
                NotImplemented_nid_no_patch(("_umtx_op operation " + std::to_string(operation)).c_str());
            }
            break;
    }
    if (error == 0) {
        return 0;
    }
    *__error_nid_postfix() = error;
    return -1;
}

}
