#include "Equeue.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Socket/include/SocketRuntime.hpp"
#include "prx/libkernel/Equeue/DescriptorEvents.hpp"
#include "prx/libkernel/File/include/DescriptorControl.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestProcess.hpp"
#include "prx/libc/include/HostDescriptorFork.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

extern "C" int* APS5_VABI __error_nid_postfix();

static std::unordered_map<KernelEqueue, KernelEqueueRef> g_equeues;
static std::unordered_map<int, KernelEqueueRef> g_descriptorQueues;
static std::mutex g_equeueMutex;
static uint64_t g_nextEqueue = 1;

static int PosixError(int error) {
    *__error_nid_postfix() = error;
    return -1;
}

static bool UnsupportedFilter(std::int16_t filter) {
    switch (filter) {
        case -3:
        case -4:
        case -5:
        case -6:
        case -8:
        case -9:
        case -10:
        case EVFILT_VIDEO_OUT:
        case EVFILT_HRTIMER:
        case EVFILT_AMPR:
            return true;
        default:
            return false;
    }
}

static void RegisterEqueueForkHooks() {
    static const bool registered = [] {
        static bool locked = false;
        const GuestForkHooks hooks{150, [](void*) {
            locked = g_equeueMutex.try_lock();
            if (!locked || !g_equeues.empty() || !g_descriptorQueues.empty()) {
                *__error_nid_postfix() = 35;
                return false;
            }
            return true;
        }, [](void*, bool) noexcept {
            if (locked) g_equeueMutex.unlock();
            locked = false;
            return true;
        }, [](void*) noexcept {
            if (locked) g_equeueMutex.unlock();
            locked = false;
            return true;
        }, nullptr};
        if (GuestRegisterForkHooks_nid_no_patch(&hooks) != 0) throw std::runtime_error("equeue: cannot register fork ownership");
        return true;
    }();
    static_cast<void>(registered);
}

extern "C" {

KernelEqueuePrivate::KernelEqueuePrivate(KernelEqueue handle) : m_handle(handle) {}

KernelEqueuePrivate::~KernelEqueuePrivate() {
    Close();
}

uint64_t KernelEqueuePrivate::MonotonicNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

void KernelEqueuePrivate::Close() {
    std::unique_lock lock(m_mutex);
    if (m_closed) {
        return;
    }
    m_closed = true;
    for (auto& ev : m_events) {
        if (ev.filter.deleteEventFunc != nullptr) {
            auto owner = ev.filter.owner;
            ev.filter.deleteEventFunc(m_handle, &ev);
        }
    }
    m_events.clear();
    m_cond.NotifyAll();
    WakePollers();
}

void KernelEqueuePrivate::TriggerExpiredTimers(uint64_t nowNs) {
    for (auto& ev : m_events) {
        if (ev.deadlineNs != 0 && ev.deadlineNs <= nowNs) {
            if (ev.event.filter == EVFILT_TIMER) {
                const uint64_t count = ev.intervalNs == 0
                    ? (ev.triggered ? 0 : 1)
                    : 1 + (nowNs - ev.deadlineNs) / ev.intervalNs;
                ev.event.data += static_cast<intptr_t>(count);
                ev.deadlineNs += count * ev.intervalNs;
            }
            ev.triggered = true;
        }
    }
}

bool KernelEqueuePrivate::NextTimerWaitMicros(uint64_t nowNs, uint32_t* out) const {
    uint64_t nearest = std::numeric_limits<uint64_t>::max();
    for (const auto& ev : m_events) {
        if (!ev.triggered && ev.deadlineNs != 0) {
            nearest = std::min(nearest, ev.deadlineNs);
        }
    }
    if (nearest == std::numeric_limits<uint64_t>::max()) {
        return false;
    }
    const uint64_t remainingNs = nearest > nowNs ? nearest - nowNs : 0;
    const uint64_t roundedUs = std::max<uint64_t>(1, (remainingNs + 999u) / 1000u);
    *out = static_cast<uint32_t>(std::min<uint64_t>(roundedUs, std::numeric_limits<uint32_t>::max()));
    return true;
}

bool KernelEqueuePrivate::PollEvents() {
    bool polled = false;
    for (auto& ev : m_events) {
        if (ev.enabled && ev.filter.pollFunc != nullptr) {
            ev.triggered = ev.filter.pollFunc(&ev);
            polled = true;
        }
    }
    return polled;
}

void KernelEqueuePrivate::WakePollers() {
    for (const auto waker : m_pollers) {
        GuestSockets::Wake(waker);
    }
}

void KernelEqueuePrivate::WaitForDescriptors(std::unique_lock<std::mutex>& lock, std::uint64_t deadlineNanos) {
    if (std::any_of(m_events.begin(), m_events.end(), [](const auto& event) { return event.enabled && event.filter.timedPolling; })) {
        const auto slice = TimedWait::NowNanos() + 1000000;
        m_cond.WaitUntil(lock, deadlineNanos == 0 ? slice : std::min(slice, deadlineNanos));
        return;
    }
    std::vector<GuestSockets::Interest> interests;
    for (const auto& e : m_events) {
        if (e.enabled && e.filter.pollFunc != nullptr) {
            interests.push_back({static_cast<int>(e.event.ident), e.event.filter == EVFILT_WRITE});
        }
    }
    const auto waker = GuestSockets::CurrentWaker();
    m_pollers.push_back(waker);
    lock.unlock();
    const bool waited = GuestSockets::WaitAny(interests, deadlineNanos);
    lock.lock();
    m_pollers.erase(std::find(m_pollers.begin(), m_pollers.end(), waker));
    if (!waited) {
        m_cond.WaitUntil(lock, deadlineNanos);
    }
}

int KernelEqueuePrivate::GetTriggeredEvents(KernelEvent* ev, int num) {
    std::unique_lock lock(m_mutex);
    if (m_closed) {
        return SCE_KERNEL_ERROR_EBADF;
    }
    TriggerExpiredTimers(MonotonicNs());
    PollEvents();
    return CollectTriggered(ev, num);
}

int KernelEqueuePrivate::PollReadiness() {
    std::lock_guard lock(m_mutex);
    if (m_closed) return -1;
    TriggerExpiredTimers(MonotonicNs());
    PollEvents();
    return std::any_of(m_events.begin(), m_events.end(), [](const auto& event) { return event.enabled && event.triggered; }) ? 1 : 0;
}

int KernelEqueuePrivate::CollectTriggered(KernelEvent* ev, int num) {
    int ret = 0;
    for (auto it = m_events.begin(); it != m_events.end() && ret < num;) {
        auto& e = *it;
        bool erase = false;
        while (e.enabled && e.triggered && ret < num) {
            ev[ret++] = e.event;
            if ((e.event.flags & EV_ONESHOT) != 0) {
                erase = true;
                break;
            }
            if ((e.event.flags & EV_DISPATCH) != 0) e.enabled = false;
            if (e.filter.resetFunc != nullptr) {
                e.filter.resetFunc(&e);
            } else if ((e.event.flags & EV_CLEAR) != 0) {
                e.triggered = false;
                e.event.fflags = 0;
                e.event.data = 0;
            }
            if (e.pendingEvents.empty()) {
                break;
            }
            e.event = e.pendingEvents.front();
            e.pendingEvents.pop_front();
            e.triggered = true;
        }
        if (erase && e.filter.deleteEventFunc) e.filter.deleteEventFunc(m_handle, &e);
        it = erase ? m_events.erase(it) : std::next(it);
    }
    return ret;
}

int KernelEqueuePrivate::WaitForEvents(KernelEvent* ev, int num, uint32_t micros) {
    return WaitUntil(ev, num, micros == 0 ? 0 : TimedWait::DeadlineNanos(micros));
}

int KernelEqueuePrivate::WaitUntil(KernelEvent* ev, int num, std::uint64_t deadline) {
    std::unique_lock lock(m_mutex);
    if (m_closed) {
        return SCE_KERNEL_ERROR_EBADF;
    }
    for (;;) {
        TriggerExpiredTimers(MonotonicNs());
        const bool polling = PollEvents();
        const int ret = CollectTriggered(ev, num);
        if (ret != 0) {
            return ret;
        }
        if (m_closed) {
            return SCE_KERNEL_ERROR_EBADF;
        }
        if (deadline == 0) {
            uint32_t timerWait = 0;
            const bool hasTimer = NextTimerWaitMicros(MonotonicNs(), &timerWait);
            const std::uint64_t wake = hasTimer ? TimedWait::NowNanos() + static_cast<std::uint64_t>(timerWait) * 1000ULL : 0;
            if (polling) {
                WaitForDescriptors(lock, wake);
            } else if (hasTimer) {
                m_cond.WaitUntil(lock, wake);
            } else {
                m_cond.Wait(lock);
            }
        } else {
            uint32_t timerWait = 0;
            const bool hasTimer = NextTimerWaitMicros(MonotonicNs(), &timerWait);
            const std::uint64_t now = TimedWait::NowNanos();
            if (now >= deadline) {
                return 0;
            }
            const std::uint64_t timerDeadline = now + static_cast<std::uint64_t>(timerWait) * 1000ULL;
            const std::uint64_t wake = hasTimer ? std::min(deadline, timerDeadline) : deadline;
            if (polling) {
                WaitForDescriptors(lock, wake);
            } else {
                m_cond.WaitUntil(lock, wake);
            }
        }
    }
}

int KernelEqueuePrivate::AddEvent(const KernelEqueueEvent& event) {
    std::unique_lock lock(m_mutex);
    if (m_closed) {
        return SCE_KERNEL_ERROR_EBADF;
    }
    auto it = std::find_if(m_events.begin(), m_events.end(),
        [ident = event.event.ident, filter = event.event.filter](const auto& e) {
            return e.event.ident == ident && e.event.filter == filter;
        }
    );
    if (it != m_events.end()) {
        if (event.event.filter == EVFILT_TIMER) {
            TriggerExpiredTimers(MonotonicNs());
        }
        it->deadlineNs = event.deadlineNs;
        it->intervalNs = event.intervalNs;
        it->event.udata = event.event.udata;
        if (event.event.filter == EVFILT_READ || event.event.filter == EVFILT_WRITE) {
            it->filter = event.filter;
            it->event.flags = event.event.flags;
            it->triggered = false;
        }
        for (auto& pending : it->pendingEvents) {
            pending.udata = event.event.udata;
        }
    } else {
        m_events.push_back(event);
    }
    m_cond.NotifyOne();
    WakePollers();
    return EQUEUE_OK;
}

int KernelEqueuePrivate::ChangeEvent(const KernelEvent& change) {
    constexpr std::uint32_t UserMask = 0x00ffffff;
    constexpr std::uint32_t Trigger = 0x01000000;
    constexpr std::uint16_t AcceptedFlags = EV_ADD | EV_DELETE | EV_ENABLE | EV_DISABLE | EV_ONESHOT | EV_CLEAR | EV_RECEIPT | EV_DISPATCH;
    if ((change.flags & ~AcceptedFlags) != 0) return SCE_KERNEL_ERROR_EINVAL;
    if (UnsupportedFilter(change.filter)) NotImplemented_nid_no_patch("kevent: filter runtime integration");
    if (change.filter != EVFILT_READ && change.filter != EVFILT_WRITE && change.filter != EVFILT_TIMER && change.filter != EVFILT_USER)
        return SCE_KERNEL_ERROR_EINVAL;
    const HostDescriptorOperation operation;
    if (change.flags & EV_DELETE) return DeleteEvent(change.ident, change.filter);
    KernelEqueueEvent prepared{};
    prepared.event = change;
    prepared.event.flags &= EV_ONESHOT | EV_CLEAR | EV_DISPATCH;
    prepared.enabled = (change.flags & EV_DISABLE) == 0;
    if (change.flags & EV_ADD) {
        if (change.filter == EVFILT_READ || change.filter == EVFILT_WRITE) {
            if ((change.fflags & ~1u) != 0 || ((change.fflags & 1) && change.data < 0)) return SCE_KERNEL_ERROR_EINVAL;
            if (const int error = PrepareDescriptorEvent_nid_no_patch(prepared, (change.fflags & 1) ? static_cast<std::size_t>(change.data) : 0)) return error;
            prepared.event.fflags = 0;
            prepared.event.data = 0;
            prepared.filter.resetFunc = [](KernelEqueueEvent* event) { event->triggered = false; };
        } else if (change.filter == EVFILT_TIMER) {
            if (change.data < 0 || change.fflags != 0) return SCE_KERNEL_ERROR_EINVAL;
            const auto now = MonotonicNs();
            const auto limit = std::numeric_limits<std::uint64_t>::max();
            const auto milliseconds = static_cast<std::uint64_t>(change.data);
            const auto interval = milliseconds > (limit - now) / 1000000 ? limit - now : std::max<std::uint64_t>(1, milliseconds) * 1000000;
            prepared.intervalNs = (change.flags & EV_ONESHOT) ? 0 : interval;
            prepared.deadlineNs = now + interval;
            prepared.event.flags |= EV_CLEAR;
            prepared.event.data = 0;
        } else {
            prepared.event.fflags &= UserMask;
            prepared.triggered = (change.fflags & Trigger) != 0;
            prepared.filter.resetFunc = [](KernelEqueueEvent* event) {
                if (event->event.flags & EV_CLEAR) event->triggered = false;
            };
        }
    }
    std::unique_lock lock(m_mutex);
    if (m_closed) return SCE_KERNEL_ERROR_EBADF;
    auto found = std::find_if(m_events.begin(), m_events.end(), [&](const auto& event) {
        return event.event.ident == change.ident && event.event.filter == change.filter;
    });
    if (found == m_events.end()) {
        if ((change.flags & EV_ADD) == 0) return SCE_KERNEL_ERROR_ENOENT;
        m_events.push_back(std::move(prepared));
        found = std::prev(m_events.end());
    } else {
        if (change.filter == EVFILT_USER) {
            const auto flags = change.fflags & UserMask;
            switch (change.fflags & 0xc0000000) {
                case 0x40000000: found->event.fflags &= flags; break;
                case 0x80000000: found->event.fflags |= flags; break;
                case 0xc0000000: found->event.fflags = flags; break;
                default: break;
            }
            found->event.data = change.data;
            if (change.fflags & Trigger) found->triggered = true;
            if (change.flags & EV_CLEAR) found->triggered = false;
        } else if (change.flags & EV_ADD) {
            if (found->filter.deleteEventFunc) found->filter.deleteEventFunc(m_handle, &*found);
            *found = std::move(prepared);
        }
        found->event.udata = change.udata;
        if ((change.flags & EV_ADD) && (change.flags & EV_DISABLE) == 0) found->enabled = true;
    }
    if (change.flags & EV_DISABLE) found->enabled = false;
    if (change.flags & EV_ENABLE) found->enabled = true;
    m_cond.NotifyAll();
    WakePollers();
    return 0;
}

int KernelEqueuePrivate::TriggerEvent(uintptr_t ident, int16_t filter, void* triggerData) {
    std::unique_lock lock(m_mutex);
    if (m_closed) {
        return SCE_KERNEL_ERROR_EBADF;
    }
    auto it = std::find_if(m_events.begin(), m_events.end(),
        [ident, filter](const auto& e) {
            return e.event.ident == ident && e.event.filter == filter;
        }
    );
    if (it == m_events.end()) {
        return SCE_KERNEL_ERROR_ENOENT;
    }
    if (it->filter.triggerFunc != nullptr) {
        it->filter.triggerFunc(&*it, triggerData);
    } else {
        it->triggered = true;
    }
    m_cond.NotifyOne();
    WakePollers();
    return EQUEUE_OK;
}

int KernelEqueuePrivate::DeleteEvent(uintptr_t ident, int16_t filter) {
    std::unique_lock lock(m_mutex);
    if (m_closed) {
        return SCE_KERNEL_ERROR_EBADF;
    }
    auto it = std::find_if(m_events.begin(), m_events.end(),
        [ident, filter](const auto& e) {
            return e.event.ident == ident && e.event.filter == filter;
        }
    );
    if (it == m_events.end()) {
        return SCE_KERNEL_ERROR_ENOENT;
    }
    if (it->filter.deleteEventFunc != nullptr) {
        auto owner = it->filter.owner;
        it->filter.deleteEventFunc(m_handle, &*it);
    }
    m_events.erase(it);
    WakePollers();
    return EQUEUE_OK;
}

void KernelEqueuePrivate::RemoveDescriptorEvents(uintptr_t ident) {
    std::unique_lock lock(m_mutex);
    const auto removed = std::erase_if(m_events, [ident](const auto& e) {
        return e.event.ident == ident && (e.event.filter == EVFILT_READ || e.event.filter == EVFILT_WRITE);
    });
    if (removed != 0) {
        WakePollers();
    }
}

KernelEqueueRef EqueuePin_nid_postfix(KernelEqueue eq) {
    if (eq == 0) {
        return {};
    }
    std::unique_lock lock(g_equeueMutex);
    auto it = g_equeues.find(eq);
    return it != g_equeues.end() ? it->second : KernelEqueueRef{};
}

int APS5_VABI EqueueAddEvent_nid_postfix(KernelEqueue eq, const KernelEqueueEvent& event) {
    auto owner = EqueuePin_nid_postfix(eq);
    if (!owner) {
        return SCE_KERNEL_ERROR_EBADF;
    }
    return owner->AddEvent(event);
}

int APS5_VABI EqueueTriggerEvent_nid_postfix(KernelEqueue eq, uintptr_t ident, int16_t filter, void* triggerData) {
    auto owner = EqueuePin_nid_postfix(eq);
    if (!owner) {
        return SCE_KERNEL_ERROR_EBADF;
    }
    return owner->TriggerEvent(ident, filter, triggerData);
}

int APS5_VABI EqueueDeleteEvent_nid_postfix(KernelEqueue eq, uintptr_t ident, int16_t filter) {
    auto owner = EqueuePin_nid_postfix(eq);
    if (!owner) {
        return SCE_KERNEL_ERROR_EBADF;
    }
    return owner->DeleteEvent(ident, filter);
}


int APS5_VABI kqueue_nid_postfix() {
    RegisterEqueueForkHooks();
    const HostDescriptorOperation operation;
#ifdef _WIN32
    const int descriptor = ::_open("NUL", _O_RDWR | _O_BINARY);
#else
    const int descriptor = ::open("/dev/null", O_RDWR);
#endif
    if (descriptor < 0) return PosixError(errno == EMFILE ? 24 : errno == ENFILE ? 23 : errno == ENOMEM ? 12 : 5);
    try {
        auto owner = std::make_shared<KernelEqueuePrivate>(0);
        File::RememberDescriptor(descriptor, 2);
        std::lock_guard lock(g_equeueMutex);
        g_descriptorQueues.emplace(descriptor, std::move(owner));
        return descriptor;
    } catch (...) {
        File::ForgetDescriptor(descriptor);
#ifdef _WIN32
        ::_close(descriptor);
#else
        ::close(descriptor);
#endif
        return PosixError(12);
    }
}

int APS5_VABI kevent_nid_postfix(int descriptor, const KernelEvent* changes, int changeCount,
    KernelEvent* events, int eventCount, const KernelTimespec* timeout) {
    KernelEqueueRef owner;
    {
        std::lock_guard lock(g_equeueMutex);
        const auto found = g_descriptorQueues.find(descriptor);
        if (found == g_descriptorQueues.end()) return PosixError(9);
        owner = found->second;
    }
    if (changeCount < 0 || eventCount < 0) return PosixError(22);
    if ((changeCount != 0 && changes == nullptr) || (eventCount != 0 && events == nullptr)) return PosixError(14);
    if (timeout && (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000)) return PosixError(22);
    try {
        std::vector<KernelEvent> input;
        if (changeCount != 0) input.assign(changes, changes + changeCount);
        const GuestArena::HostWrite destination(events, static_cast<std::size_t>(eventCount) * sizeof(KernelEvent));
        if (!destination.Open()) return PosixError(14);
        int receipts = 0;
        for (const auto& change : input) {
            if (change.filter == 0) continue;
            if (UnsupportedFilter(change.filter))
                NotImplemented_nid_no_patch("kevent: filter runtime integration");
            if ((change.filter == EVFILT_READ || change.filter == EVFILT_WRITE) && (change.flags & EV_CLEAR))
                NotImplemented_nid_no_patch("kevent: descriptor EV_CLEAR notifications");
            if (change.filter == EVFILT_WRITE && change.ident >= GuestSockets::FirstDescriptor && (change.fflags & 1) && change.data > 1)
                NotImplemented_nid_no_patch("kevent: socket write low watermark");
            const int result = owner->ChangeEvent(change);
            if (result != 0 || (change.flags & EV_RECEIPT)) {
                if (receipts == eventCount) return result != 0 ? PosixError(result & 0xffff) : 0;
                events[receipts] = change;
                events[receipts].flags = EV_ERROR;
                events[receipts++].data = result & 0xffff;
            }
        }
        if (receipts != 0 || eventCount == 0) return receipts;
        int result;
        if (timeout && timeout->tv_sec == 0 && timeout->tv_nsec == 0) result = owner->GetTriggeredEvents(events, eventCount);
        else {
            std::uint64_t deadline = 0;
            if (timeout) {
                const auto now = TimedWait::NowNanos();
                const auto max = std::numeric_limits<std::uint64_t>::max();
                const auto seconds = static_cast<std::uint64_t>(timeout->tv_sec);
                const auto nanos = static_cast<std::uint64_t>(timeout->tv_nsec);
                deadline = seconds > (max - now - nanos) / 1000000000 ? max : now + seconds * 1000000000 + nanos;
            }
            result = owner->WaitUntil(events, eventCount, deadline);
        }
        return result < 0 ? PosixError(result & 0xffff) : result;
    } catch (const std::bad_alloc&) {
        return PosixError(12);
    }
}

int APS5_VABI sceKernelCreateEqueue(KernelEqueue* eq, const char* name) {
    if (eq == nullptr || name == nullptr) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    RegisterEqueueForkHooks();
    const HostDescriptorOperation operation;
    std::unique_lock lock(g_equeueMutex);
    if (g_nextEqueue > static_cast<uint64_t>(std::numeric_limits<KernelEqueue>::max())) {
        throw std::runtime_error("equeue handle space exhausted");
    }
    *eq = static_cast<KernelEqueue>(g_nextEqueue++);
    auto owner = std::make_shared<KernelEqueuePrivate>(*eq);
    owner->SetName(std::string(name));
    g_equeues.emplace(*eq, std::move(owner));
    return EQUEUE_OK;
}

int APS5_VABI sceKernelDeleteEqueue(KernelEqueue eq) {
    KernelEqueueRef owner;
    {
        std::unique_lock lock(g_equeueMutex);
        auto it = g_equeues.find(eq);
        if (it == g_equeues.end()) {
            return SCE_KERNEL_ERROR_EBADF;
        }
        owner = std::move(it->second);
        g_equeues.erase(it);
    }
    owner->Close();
    return EQUEUE_OK;
}

int APS5_VABI sceKernelWaitEqueue(KernelEqueue eq, KernelEvent* ev, int num, int* out, const KernelUseconds* timo) {
    auto owner = EqueuePin_nid_postfix(eq);
    if (!owner) {
        return SCE_KERNEL_ERROR_EBADF;
    }
    if (ev == nullptr) {
        return SCE_KERNEL_ERROR_EFAULT;
    }
    if (num < 1 || out == nullptr) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    const auto waitStart = std::chrono::steady_clock::now();
    if (timo == nullptr) {
        *out = owner->WaitForEvents(ev, num, 0);
    } else if (*timo == 0) {
        *out = owner->GetTriggeredEvents(ev, num);
    } else {
        *out = owner->WaitForEvents(ev, num, *timo);
    }
    if (timo == nullptr || *timo != 0) {
        KernelTraceWait_nid_postfix("equeue", __builtin_return_address(0), static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count()), *out == 0);
    }
    if (*out == SCE_KERNEL_ERROR_EBADF) {
        return SCE_KERNEL_ERROR_EBADF;
    }
    if (*out == 0) {
        return SCE_KERNEL_ERROR_ETIMEDOUT;
    }
    return EQUEUE_OK;
}

int APS5_VABI sceKernelAddUserEvent(KernelEqueue eq, int id) {
    KernelEqueueEvent event{};
    event.event.ident = static_cast<uintptr_t>(id);
    event.event.filter = EVFILT_USER;
    event.event.flags = EV_ADD;
    event.filter.triggerFunc = [](KernelEqueueEvent* e, void* data) {
        e->triggered = true;
        e->event.data = reinterpret_cast<intptr_t>(data);
        e->event.udata = data;
    };
    event.filter.resetFunc = [](KernelEqueueEvent* e) {
        if ((e->event.flags & EV_CLEAR) != 0) {
            e->triggered = false;
            e->event.fflags = 0;
            e->event.data = 0;
        }
    };
    return EqueueAddEvent_nid_postfix(eq, event);
}

int APS5_VABI sceKernelAddUserEventEdge(KernelEqueue eq, int id) {
    KernelEqueueEvent event{};
    event.event.ident = static_cast<uintptr_t>(id);
    event.event.filter = EVFILT_USER;
    event.event.flags = EV_ADD | EV_CLEAR;
    event.filter.triggerFunc = [](KernelEqueueEvent* e, void* data) {
        e->triggered = true;
        e->event.data = reinterpret_cast<intptr_t>(data);
        e->event.udata = data;
    };
    event.filter.resetFunc = [](KernelEqueueEvent* e) {
        if ((e->event.flags & EV_CLEAR) != 0) {
            e->triggered = false;
            e->event.fflags = 0;
            e->event.data = 0;
        }
    };
    return EqueueAddEvent_nid_postfix(eq, event);
}

int APS5_VABI sceKernelTriggerUserEvent(KernelEqueue eq, int id, void* udata) {
    return EqueueTriggerEvent_nid_postfix(eq, static_cast<uintptr_t>(id), EVFILT_USER, udata);
}

int APS5_VABI sceKernelDeleteUserEvent(KernelEqueue eq, int id) {
    return EqueueDeleteEvent_nid_postfix(eq, static_cast<uintptr_t>(id), EVFILT_USER);
}

int APS5_VABI sceKernelAddHRTimerEvent(KernelEqueue eq, int id, const KernelTimespec* ts, void* udata) {
    if (ts == nullptr) {
        return SCE_KERNEL_ERROR_EFAULT;
    }
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000LL) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    const uint64_t delayNs =
        static_cast<uint64_t>(ts->tv_sec) * 1000000000ULL +
        static_cast<uint64_t>(ts->tv_nsec);
    const uint64_t nowNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
    KernelEqueueEvent event{};
    event.deadlineNs = (delayNs <= std::numeric_limits<uint64_t>::max() - nowNs)
        ? nowNs + delayNs : std::numeric_limits<uint64_t>::max();
    event.event.ident = static_cast<uintptr_t>(id);
    event.event.filter = EVFILT_HRTIMER;
    event.event.flags = EV_ADD | EV_ONESHOT;
    event.event.udata = udata;
    return EqueueAddEvent_nid_postfix(eq, event);
}

int APS5_VABI sceKernelDeleteHRTimerEvent(KernelEqueue eq, int id) {
    return EqueueDeleteEvent_nid_postfix(eq, static_cast<uintptr_t>(id), EVFILT_HRTIMER);
}

int APS5_VABI sceKernelAddTimerEvent(KernelEqueue eq, int id, KernelUseconds usec, void* udata) {
    const uint64_t intervalNs = static_cast<uint64_t>(usec) * 1000ULL;
    KernelEqueueEvent event{};
    event.deadlineNs = KernelEqueuePrivate::MonotonicNs() + intervalNs;
    event.intervalNs = intervalNs;
    event.event.ident = static_cast<uintptr_t>(id);
    event.event.filter = EVFILT_TIMER;
    event.event.flags = EV_ADD | EV_CLEAR;
    event.event.udata = udata;
    return EqueueAddEvent_nid_postfix(eq, event);
}

int APS5_VABI sceKernelDeleteTimerEvent(KernelEqueue eq, int id) {
    return EqueueDeleteEvent_nid_postfix(eq, static_cast<uintptr_t>(id), EVFILT_TIMER);
}

int APS5_VABI sceKernelAddAmprEvent(KernelEqueue eq, int id, void* udata) {
    if (eq == 0) {
        return EQUEUE_OK;
    }
    KernelEqueueEvent event{};
    event.event.ident = static_cast<uintptr_t>(id);
    event.event.filter = EVFILT_AMPR;
    event.event.flags = EV_ADD | EV_CLEAR;
    event.event.udata = udata;
    event.filter.triggerFunc = [](KernelEqueueEvent* e, void* data) {
        KernelEvent triggered = e->event;
        triggered.data = static_cast<intptr_t>(reinterpret_cast<uintptr_t>(data));
        if (e->triggered) {
            e->pendingEvents.push_back(triggered);
        } else {
            e->event = triggered;
            e->triggered = true;
        }
    };
    event.filter.resetFunc = [](KernelEqueueEvent* e) {
        if ((e->event.flags & EV_CLEAR) != 0) {
            e->triggered = false;
            e->event.fflags = 0;
            e->event.data = 0;
        }
    };
    EqueueAddEvent_nid_postfix(eq, event);
    return EQUEUE_OK;
}

int APS5_VABI sceKernelAddAmprSystemEvent(KernelEqueue eq, int id, void* udata) {
    return sceKernelAddAmprEvent(eq, id, udata);
}

int APS5_VABI sceKernelDeleteAmprEvent(KernelEqueue eq, int id) {
    if (eq == 0) {
        return EQUEUE_OK;
    }
    EqueueDeleteEvent_nid_postfix(eq, static_cast<uintptr_t>(id), EVFILT_AMPR);
    return EQUEUE_OK;
}

int APS5_VABI sceKernelDeleteAmprSystemEvent(KernelEqueue eq, int id) {
    return sceKernelDeleteAmprEvent(eq, id);
}

static int AddDescriptorEvent(KernelEqueue eq, int fd, std::size_t size, void* udata, int16_t filter) {
    const HostDescriptorOperation operation;
    if (!EqueuePin_nid_postfix(eq)) return SCE_KERNEL_ERROR_EBADF;
    if (fd >= GuestSockets::FirstDescriptor && filter == EVFILT_WRITE && size > 1)
        NotImplemented_nid_no_patch("sceKernelAddWriteEvent: socket write low watermark");
    KernelEqueueEvent event{};
    event.event.ident = static_cast<uintptr_t>(fd);
    event.event.filter = filter;
    event.event.flags = EV_ADD;
    event.event.udata = udata;
    if (const int error = PrepareDescriptorEvent_nid_no_patch(event, size)) return error;
    event.filter.resetFunc = [](KernelEqueueEvent* e) {
        e->triggered = false;
    };
    return EqueueAddEvent_nid_postfix(eq, event);
}

int APS5_VABI sceKernelAddReadEvent(KernelEqueue eq, int fd, std::size_t size, void* udata) {
    return AddDescriptorEvent(eq, fd, size, udata, EVFILT_READ);
}

int APS5_VABI sceKernelDeleteReadEvent(KernelEqueue eq, int fd) {
    return EqueueDeleteEvent_nid_postfix(eq, static_cast<uintptr_t>(fd), EVFILT_READ);
}

int APS5_VABI sceKernelAddWriteEvent(KernelEqueue eq, int fd, std::size_t size, void* udata) {
    return AddDescriptorEvent(eq, fd, size, udata, EVFILT_WRITE);
}

int APS5_VABI sceKernelDeleteWriteEvent(KernelEqueue eq, int fd) {
    return EqueueDeleteEvent_nid_postfix(eq, static_cast<uintptr_t>(fd), EVFILT_WRITE);
}

}

void EqueueDescriptorClosed(int descriptor) {
    std::vector<KernelEqueueRef> owners;
    KernelEqueueRef closed;
    {
        std::unique_lock lock(g_equeueMutex);
        const auto found = g_descriptorQueues.find(descriptor);
        if (found != g_descriptorQueues.end()) {
            closed = found->second;
            g_descriptorQueues.erase(found);
            if (std::any_of(g_descriptorQueues.begin(), g_descriptorQueues.end(), [&](const auto& entry) { return entry.second == closed; })) closed.reset();
        }
        for (const auto& [handle, owner] : g_equeues) {
            owners.push_back(owner);
        }
        for (const auto& [handle, owner] : g_descriptorQueues) owners.push_back(owner);
    }
    if (closed) closed->Close();
    for (const auto& owner : owners) {
        owner->RemoveDescriptorEvents(static_cast<uintptr_t>(descriptor));
    }
}

bool IsKqueueDescriptor_nid_no_patch(int descriptor) {
    std::lock_guard lock(g_equeueMutex);
    return g_descriptorQueues.contains(descriptor);
}

int PollKqueueDescriptor_nid_no_patch(int descriptor, int events) {
    KernelEqueueRef owner;
    {
        std::lock_guard lock(g_equeueMutex);
        const auto found = g_descriptorQueues.find(descriptor);
        if (found == g_descriptorQueues.end()) return -1;
        owner = found->second;
    }
    const int ready = owner->PollReadiness();
    return ready < 0 ? -1 : ready == 0 ? 0 : events & 0x41;
}

void DuplicateKqueueDescriptor_nid_no_patch(int source, int target) {
    if (source == target) return;
    EqueueDescriptorClosed(target);
    std::lock_guard lock(g_equeueMutex);
    const auto found = g_descriptorQueues.find(source);
    if (found != g_descriptorQueues.end()) g_descriptorQueues[target] = found->second;
}
