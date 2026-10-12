#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/DaemonRuntime.hpp"
#include "prx/libc/include/GuestProcess.hpp"
#include "prx/libkernel/Pthread/Posix/Common.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include "prx/libkernel/Pthread/include/ThreadLifecycle.hpp"
#include "prx/libkernel/Pthread/include/WindowsThreadLocal.hpp"
#include "prx/libkernel/Pthread/include/Cancel.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <memory>
#include <map>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" int* APS5_VABI __error_nid_postfix();
extern "C" int APS5_VABI getpid_nid_postfix(void);
extern "C" Pthread APS5_VABI scePthreadSelf(void);

namespace {
using GuestHandler = void (APS5_VABI *)(int);
constexpr int MaxSignal = 128;
constexpr int GuestSigkill = 9;
constexpr int GuestSigstop = 17;
constexpr int SaRestart = 0x2;
constexpr int SaSiginfo = 0x40;
constexpr int SaResetHand = 0x4;
constexpr int SaNodefer = 0x10;
std::mutex& Registration() {
    static auto* mutex = new std::mutex();
    return *mutex;
}
GuestSigaction dispositions[MaxSignal + 1]{};
struct GuestSignalInfo {
    int signal;
    int error;
    int code;
    int pid;
    std::uint32_t uid;
    int status;
    void* address;
    std::uint64_t value;
    std::uint64_t reason[5];
};
static_assert(sizeof(GuestSignalInfo) == 80 && offsetof(GuestSignalInfo, address) == 24);
std::size_t pendingCount = 0;
class SignalQueue {
public:
    SignalQueue() = default;
    SignalQueue(const SignalQueue&) = delete;
    SignalQueue& operator=(const SignalQueue&) = delete;
    ~SignalQueue() { pendingCount -= records.size(); }
    explicit operator bool() const { return overflow != 0 || !records.empty(); }
    void Push(const GuestSignalInfo& info) {
        if (pendingCount >= 128) { overflow = info.signal; return; }
        records.push_back(info);
        ++pendingCount;
    }
    GuestSignalInfo Pop() {
        if (overflow != 0) {
            GuestSignalInfo info{};
            info.signal = overflow;
            overflow = 0;
            return info;
        }
        const auto info = records.front();
        records.erase(records.begin());
        --pendingCount;
        return info;
    }
    void Clear() {
        pendingCount -= records.size();
        records.clear();
        overflow = 0;
    }
private:
    std::vector<GuestSignalInfo> records;
    int overflow = 0;
};
using Pending = std::array<SignalQueue, MaxSignal + 1>;
struct SignalState {
    Pthread thread;
    GuestSignalSet mask{};
    GuestSignalSet selected{};
    Pending pending{};
    TimedWait::Condition changed;
    std::atomic<unsigned> criticalDepth{0};
    bool waiting = false;
    bool ready = false;
    std::array<std::atomic<std::uint32_t>, 4> nativePending{};
    std::uint64_t caught = 0;
    explicit SignalState(Pthread value) : thread(value) {}
};
struct SignalLocal {
    SignalState* current = nullptr;
    unsigned asynchronousDepth = 0;
#ifdef _WIN32
    ~SignalLocal();
#endif
};
SignalLocal& Local() {
#ifdef _WIN32
    return WindowsThreadLocal<SignalLocal>::Get();
#else
    static thread_local SignalLocal local;
    return local;
#endif
}
auto& SignalThreads() {
    static auto* threads = new std::map<Pthread, std::shared_ptr<SignalState>>();
    return *threads;
}
#ifdef _WIN32
SignalLocal::~SignalLocal() {
    if (current == nullptr) return;
    ++current->criticalDepth;
    std::lock_guard lock(Registration());
    const auto thread = current->thread;
    current = nullptr;
    SignalThreads().erase(thread);
}
#endif
Pending& ProcessPending() {
    static auto* pending = new Pending();
    return *pending;
}
bool Contains(const GuestSignalSet& set, int signal) {
    return (set.bits[(signal - 1) / 32] & (1u << ((signal - 1) % 32))) != 0;
}
void Add(GuestSignalSet& set, int signal) {
    set.bits[(signal - 1) / 32] |= 1u << ((signal - 1) % 32);
}
void Remove(GuestSignalSet& set, int signal) {
    set.bits[(signal - 1) / 32] &= ~(1u << ((signal - 1) % 32));
}
void Sanitize(GuestSignalSet& set) {
    Remove(set, GuestSigkill);
    Remove(set, GuestSigstop);
}
bool Ignored(int signal, const GuestSigaction& action) {
    if (action.handler == 1) return true;
    if (action.handler != 0) return false;
    return signal == 16 || signal == 19 || signal == 20 || signal == 23 || signal == 28 || signal == 29;
}
void Pump();
struct Critical {
    SignalState* state = Local().current;
    Critical() { if (state) ++state->criticalDepth; }
    ~Critical() { if (state) --state->criticalDepth; }
};
struct SignalApi {
    SignalState* state;
    SignalApi() : state((scePthreadSelf(), Local().current)) { ++state->criticalDepth; }
    ~SignalApi() noexcept(false) {
        --state->criticalDepth;
        if (state->criticalDepth == 0 && std::uncaught_exceptions() == 0) Pump();
    }
};
GuestSignalInfo Information(int signal, int code) {
    return {signal, 0, code, getpid_nid_postfix(), 0, 0, nullptr, 0, {}};
}
void CollectNative(SignalState& state) {
    for (int signal = 1; signal <= MaxSignal; ++signal) {
        const auto bit = 1u << ((signal - 1) % 32);
        if ((state.nativePending[(signal - 1) / 32].fetch_and(~bit) & bit) != 0 && !state.pending[signal])
            state.pending[signal].Push(Information(signal, 0x10006));
    }
}
std::optional<GuestSignalInfo> Take(SignalState& state, const GuestSignalSet& selected) {
    CollectNative(state);
    for (int signal = 1; signal <= MaxSignal; ++signal) {
        if (!Contains(selected, signal)) continue;
        auto& record = state.pending[signal] ? state.pending[signal] : ProcessPending()[signal];
        if (!record) continue;
        return record.Pop();
    }
    return std::nullopt;
}
void APS5_VABI DeliverPending_nid_no_patch(int asynchronous, void* context) {
    auto* state = Local().current;
    if (state == nullptr || state->criticalDepth != 0) return;
    struct DeliveryScope {
        bool asynchronous;
        explicit DeliveryScope(bool value) : asynchronous(value) { if (asynchronous) ++Local().asynchronousDepth; }
        ~DeliveryScope() { if (asynchronous) --Local().asynchronousDepth; }
    } deliveryScope(asynchronous != 0);
    for (;;) {
        GuestSigaction action{};
        GuestSignalSet previous{};
        std::optional<GuestSignalInfo> info;
        {
            Critical critical;
            std::lock_guard lock(Registration());
            GuestSignalSet eligible{};
            for (int i = 0; i != 4; ++i) eligible.bits[i] = ~state->mask.bits[i];
            info = Take(*state, eligible);
            if (!info) return;
            action = dispositions[info->signal];
            if (Ignored(info->signal, action)) continue;
            if (action.handler == 0) {
                if (info->signal == 17 || info->signal == 18 || info->signal == 21 || info->signal == 22)
                    throw std::runtime_error("signal: stopping and resuming the guest process is not implemented");
            }
            if (action.handler != 0 && (action.flags & 1) != 0)
                throw std::runtime_error("signal: handler execution on the alternate signal stack is not implemented");
            previous = state->mask;
            for (int i = 0; i != 4; ++i) state->mask.bits[i] |= action.mask.bits[i];
            if ((action.flags & SaNodefer) == 0) Add(state->mask, info->signal);
            Sanitize(state->mask);
            if ((action.flags & SaResetHand) != 0) dispositions[info->signal] = {};
        }
        if (action.handler == 0) {
            const int signal = info->signal;
            const bool core = signal == 3 || signal == 4 || signal == 5 || signal == 6 || signal == 7 ||
                signal == 8 || signal == 10 || signal == 11 || signal == 12 || signal == 24 || signal == 25;
            if (core && Local().asynchronousDepth == 0) GuestCaptureFatalSignal_nid_no_patch(signal, context);
            std::_Exit(128 + signal);
        }
        const int savedError = *__error_nid_postfix();
        const auto restore = [&] {
            *__error_nid_postfix() = savedError;
            Critical critical;
            std::lock_guard lock(Registration());
            state->mask = previous;
            ++state->caught;
            state->changed.NotifyAll();
        };
        try {
            if ((action.flags & SaSiginfo) != 0)
                reinterpret_cast<void (APS5_VABI *)(int, void*, void*)>(action.handler)(info->signal, &*info, context);
            else
                reinterpret_cast<GuestHandler>(action.handler)(info->signal);
        } catch (...) { restore(); throw; }
        restore();
    }
}
void Pump() {
    auto* state = Local().current;
    if (state == nullptr || state->criticalDepth != 0 || !state->ready) return;
    bool pending = false;
    {
        Critical critical;
        std::lock_guard lock(Registration());
        CollectNative(*state);
        for (int signal = 1; signal <= MaxSignal; ++signal)
            if (!Contains(state->mask, signal) && (state->pending[signal] || ProcessPending()[signal])) { pending = true; break; }
    }
    if (pending) GuestDeliverSignal_nid_no_patch(state->thread, DeliverPending_nid_no_patch, 0);
}
void Wake(SignalState& state, int signal) {
    state.changed.NotifyAll();
    if (state.thread == (Local().current ? Local().current->thread : nullptr) || state.waiting || !state.ready || Contains(state.mask, signal)) return;
    GuestDeliverSignal_nid_no_patch(state.thread, DeliverPending_nid_no_patch, signal);
}
int SendThread(Pthread thread, int signal, int code) {
    std::lock_guard lock(Registration());
    const auto found = SignalThreads().find(thread);
    if (found == SignalThreads().end()) return 3;
    if (signal == 0) return 0;
    if (Ignored(signal, dispositions[signal])) return 0;
    auto& state = *found->second;
    state.pending[signal].Push(Information(signal, code));
    Wake(state, signal);
    return 0;
}
void SendProcess(int signal, int code) {
    Critical critical;
    std::lock_guard lock(Registration());
    if (Ignored(signal, dispositions[signal])) return;
    ProcessPending()[signal].Push(Information(signal, code));
    for (auto& [thread, state] : SignalThreads()) {
        if (state->waiting && Contains(state->selected, signal)) {
            state->changed.NotifyAll();
            return;
        }
    }
    for (auto& [thread, state] : SignalThreads()) {
        if (!Contains(state->mask, signal)) { Wake(*state, signal); return; }
    }
}
int NativeSignal(int guest) {
    switch (guest) {
        case 2: return SIGINT;
        case 4: return SIGILL;
        case 6: return SIGABRT;
        case 8: return SIGFPE;
        case 11: return SIGSEGV;
        case 15: return SIGTERM;
        default: return 0;
    }
}
void Dispatch(int native) {
    int guest = 0;
    for (int candidate : {2, 4, 6, 8, 11, 15})
        if (NativeSignal(candidate) == native) { guest = candidate; break; }
    if (!guest) return;
#ifdef _WIN32
    // Preserve the guest's persistent registration across CRT delivery.
    std::signal(native, Dispatch);
#endif
#ifdef _WIN32
    auto* local = WindowsThreadLocal<SignalLocal>::GetIfPresent();
    auto* state = local ? local->current : nullptr;
#else
    auto* state = Local().current;
#endif
    if (state == nullptr) return;
    state->nativePending[(guest - 1) / 32].fetch_or(1u << ((guest - 1) % 32));
}
bool Install(int guest, const GuestSigaction& action) {
    const int native = NativeSignal(guest);
    if (!native) return true;
    auto hostHandler = action.handler == 1 ? SIG_IGN : action.handler > 1 ? Dispatch : SIG_DFL;
    return std::signal(native, hostHandler) != SIG_ERR;
}
}

extern "C" void GuestSignalThreadCreate_nid_no_patch(Pthread thread, Pthread parent) {
    Critical critical;
    std::lock_guard lock(Registration());
    auto state = std::make_shared<SignalState>(thread);
    if (const auto found = SignalThreads().find(parent); found != SignalThreads().end()) state->mask = found->second->mask;
    SignalThreads().emplace(thread, std::move(state));
}
extern "C" void GuestSignalThreadBind_nid_no_patch(Pthread thread) {
    if (Local().current && Local().current->thread == thread) return;
    {
        Critical critical;
        std::lock_guard lock(Registration());
        Local().current = SignalThreads().at(thread).get();
        Local().current->ready = true;
    }
#ifndef _WIN32
    sigset_t reserved;
    sigemptyset(&reserved);
    sigaddset(&reserved, SIGUSR2);
    pthread_sigmask(SIG_UNBLOCK, &reserved, nullptr);
#endif
    Pump();
}
extern "C" void GuestSignalThreadExit_nid_no_patch(Pthread thread) {
    auto* state = Local().current;
    const bool exiting = state && state->thread == thread;
    if (state) ++state->criticalDepth;
    {
        std::lock_guard lock(Registration());
        if (exiting) Local().current = nullptr;
        SignalThreads().erase(thread);
    }
    if (state && !exiting) --state->criticalDepth;
}

namespace {
constexpr int GuestSigalrm = 14;
struct AlarmState {
    std::mutex lock;
    std::condition_variable changed;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    bool threadStarted = false;
};

AlarmState& Alarms() {
    static auto* state = new AlarmState();
    return *state;
}

bool signalsPrepared = false;
SignalState* preparedState = nullptr;

bool PrepareSignals(void*) {
    auto& alarms = Alarms();
    preparedState = Local().current;
    if (preparedState) ++preparedState->criticalDepth;
    if (std::try_lock(Registration(), alarms.lock) != -1) {
        if (preparedState) --preparedState->criticalDepth;
        preparedState = nullptr;
        return false;
    }
    signalsPrepared = true;
    return true;
}

bool ParentSignals(void*, bool) noexcept {
    if (!signalsPrepared) return true;
    signalsPrepared = false;
    Alarms().lock.unlock();
    Registration().unlock();
    if (preparedState) --preparedState->criticalDepth;
    preparedState = nullptr;
    return true;
}

bool ChildSignals(void*) noexcept {
    Alarms().deadline.reset();
    Alarms().threadStarted = false;
    std::construct_at(&Alarms().changed);
    for (auto& queue : ProcessPending()) queue.Clear();
    for (auto it = SignalThreads().begin(); it != SignalThreads().end();) {
        if (it->second.get() == Local().current) {
            auto& state = *it->second;
            for (auto& queue : state.pending) queue.Clear();
            state.waiting = false;
            state.selected = {};
            state.caught = 0;
            for (auto& bits : state.nativePending) bits.store(0);
            std::construct_at(&state.changed);
            ++it;
        } else {
            std::construct_at(&it->second->changed);
            it = SignalThreads().erase(it);
        }
    }
    return ParentSignals(nullptr, true);
}

struct SignalForkRegistration {
    SignalForkRegistration() {
        const GuestForkHooks hooks{300, PrepareSignals, ParentSignals, ChildSignals, nullptr};
        if (GuestRegisterForkHooks_nid_no_patch(&hooks) != 0)
            throw std::runtime_error("libkernel: registering signal fork hooks failed");
    }
} signalForkRegistration;

void ExpireAlarm() {
    SendProcess(GuestSigalrm, 0x10006);
}

void RunAlarms() {
    auto& alarms = Alarms();
    std::unique_lock lock(alarms.lock);
    for (;;) {
        if (!alarms.deadline) {
            alarms.changed.wait(lock);
            continue;
        }
        const auto deadline = *alarms.deadline;
        if (alarms.changed.wait_until(lock, deadline) != std::cv_status::timeout || alarms.deadline != deadline) continue;
        alarms.deadline.reset();
        lock.unlock();
        ExpireAlarm();
        lock.lock();
    }
}
}

extern "C" unsigned int APS5_VABI GuestAlarm_nid_no_patch(unsigned int seconds) {
    SignalApi api;
    auto& alarms = Alarms();
    std::lock_guard lock(alarms.lock);
    const auto now = std::chrono::steady_clock::now();
    unsigned int remaining = 0;
    if (alarms.deadline && *alarms.deadline > now) {
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(*alarms.deadline - now).count();
        remaining = static_cast<unsigned int>((micros + 999999) / 1000000);
    }
    if (seconds == 0) alarms.deadline.reset();
    else alarms.deadline = now + std::chrono::seconds(seconds);
    if (!alarms.threadStarted) {
        std::thread(RunAlarms).detach();
        alarms.threadStarted = true;
    }
    alarms.changed.notify_all();
    return remaining;
}

struct GuestItimerval {
    KernelTimeval interval;
    KernelTimeval value;
};
static_assert(sizeof(GuestItimerval) == 32 && offsetof(GuestItimerval, value) == 16);

extern "C" int APS5_VABI getitimer_nid_postfix(int which, GuestItimerval* output) {
    if (which < 0 || which > 2) {
        *__error_nid_postfix() = 22;
        return -1;
    }
    if (which != 0) throw std::runtime_error("getitimer: CPU interval timers are not implemented");
    if (output == nullptr) {
        *__error_nid_postfix() = 14;
        return -1;
    }
    GuestItimerval result{};
    auto& alarms = Alarms();
    {
        std::lock_guard lock(alarms.lock);
        const auto now = std::chrono::steady_clock::now();
        if (alarms.deadline && *alarms.deadline > now) {
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(*alarms.deadline - now).count();
            result.value.tv_sec = micros / 1000000;
            result.value.tv_usec = micros % 1000000;
        }
    }
    *output = result;
    return 0;
}

struct GuestStack {
    void* sp;
    std::size_t size;
    int flags;
};
static_assert(sizeof(GuestStack) == 24 && offsetof(GuestStack, flags) == 16);
namespace {
constexpr int SsDisable = 4;
constexpr std::size_t MinSignalStackSize = 2048;
thread_local GuestStack alternateStack{nullptr, 0, SsDisable};

int WaitForSignal(const GuestSignalSet* set, GuestSignalInfo* output, const KernelTimespec* timeout, bool pause) {
    if (set == nullptr) { *__error_nid_postfix() = 14; return -1; }
    const bool validTimeout = !timeout || (timeout->tv_nsec >= 0 && timeout->tv_nsec < 1000000000);
    SignalApi api;
    ThreadCancel::Check();
    auto& state = *api.state;
    std::optional<std::uint64_t> deadline;
    if (timeout && validTimeout) {
        const auto now = TimedWait::NowNanos();
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        const auto seconds = static_cast<std::uint64_t>(timeout->tv_sec);
        const auto nanos = static_cast<std::uint64_t>(timeout->tv_nsec);
        deadline = timeout->tv_sec < 0 ? now : seconds > (maximum - now - nanos) / 1000000000 ? maximum : now + seconds * 1000000000 + nanos;
    }
    std::unique_lock lock(Registration());
    state.selected = *set;
    Sanitize(state.selected);
    state.waiting = true;
    const auto finish = [&] {
        state.waiting = false;
        state.selected = {};
    };
    for (;;) {
        if (!pause) {
            if (const auto info = Take(state, state.selected)) {
                finish();
                if (output) *output = *info;
                return info->signal;
            }
        }
        if (!validTimeout) {
            finish();
            *__error_nid_postfix() = 22;
            return -1;
        }
        const auto before = state.caught;
        state.waiting = false;
        lock.unlock();
        --state.criticalDepth;
        try { Pump(); }
        catch (...) {
            ++state.criticalDepth;
            lock.lock();
            finish();
            throw;
        }
        ++state.criticalDepth;
        lock.lock();
        state.waiting = true;
        if (state.caught != before) {
            finish();
            *__error_nid_postfix() = 4;
            return -1;
        }
        if (deadline && TimedWait::NowNanos() >= *deadline) {
            finish();
            *__error_nid_postfix() = 35;
            return -1;
        }
        if (ThreadCancel::Requested()) {
            finish();
            lock.unlock();
            ThreadCancel::Check();
        }
        bool ready = false;
        for (int signal = 1; signal <= MaxSignal; ++signal) {
            if ((state.pending[signal] || ProcessPending()[signal]) &&
                (Contains(state.selected, signal) || !Contains(state.mask, signal))) { ready = true; break; }
        }
        if (ready) continue;
        ThreadCancel::Wait(state.changed, lock, deadline);
    }
}
}

extern "C" {
GuestHandler APS5_VABI signal_nid_postfix(int guest, GuestHandler handler) {
    SignalApi api;
    const auto invalid = reinterpret_cast<GuestHandler>(static_cast<std::uintptr_t>(-1));
    if (guest < 1 || guest > MaxSignal || ((guest == GuestSigkill || guest == GuestSigstop) && handler != nullptr)) { *__error_nid_postfix() = 22; return invalid; }
    std::lock_guard lock(Registration());
    const GuestSigaction action{reinterpret_cast<std::uintptr_t>(handler), SaRestart, {}};
    if (!Install(guest, action)) { *__error_nid_postfix() = 22; return invalid; }
    const auto previous = reinterpret_cast<GuestHandler>(dispositions[guest].handler);
    dispositions[guest] = action;
    if (Ignored(guest, action)) {
        ProcessPending()[guest].Clear();
        for (auto& [thread, state] : SignalThreads()) state->pending[guest].Clear();
    }
    return previous;
}
int APS5_VABI sigaction_nid_postfix(int guest, const GuestSigaction* action, GuestSigaction* previous) {
    SignalApi api;
    if (guest < 1 || guest > MaxSignal) { *__error_nid_postfix() = 22; return -1; }
    if (action && (guest == GuestSigkill || guest == GuestSigstop) && action->handler != 0) { *__error_nid_postfix() = 22; return -1; }
    std::lock_guard lock(Registration());
    const GuestSigaction old = dispositions[guest];
    if (action) {
        if (!Install(guest, *action)) { *__error_nid_postfix() = 22; return -1; }
        dispositions[guest] = *action;
        if (Ignored(guest, *action)) {
            ProcessPending()[guest].Clear();
            for (auto& [thread, state] : SignalThreads()) state->pending[guest].Clear();
        }
    }
    if (previous) *previous = old;
    return 0;
}
int APS5_VABI raise_nid_postfix(int guest) {
    SignalApi api;
    if (guest < 1 || guest > MaxSignal) { *__error_nid_postfix() = 22; return -1; }
    const int result = SendThread(api.state->thread, guest, 0x10007);
    if (result != 0) *__error_nid_postfix() = result;
    return result == 0 ? 0 : -1;
}
int APS5_VABI kill_nid_postfix(int pid, int guest) {
    SignalApi api;
    if (guest < 0 || guest > MaxSignal) { *__error_nid_postfix() = 22; return -1; }
    const int self = getpid_nid_postfix();
    if (pid != self && pid != 0 && pid != -self) { *__error_nid_postfix() = 3; return -1; }
    if (guest != 0) SendProcess(guest, 0x10001);
    return 0;
}
int APS5_VABI pthread_kill_nid_postfix(Pthread thread, int guest) {
    SignalApi api;
    if (guest < 0 || guest > MaxSignal) return PosixThread::GUEST_EINVAL;
    if (thread == nullptr) return PosixThread::GUEST_ESRCH;
    return SendThread(thread, guest, 0x10007);
}
int APS5_VABI sigaltstack_nid_postfix(const GuestStack* stack, GuestStack* previous) {
    GuestStack replacement = alternateStack;
    if (stack != nullptr) {
        if ((stack->flags & ~SsDisable) != 0) { *__error_nid_postfix() = 22; return -1; }
        if (stack->flags & SsDisable) replacement.flags = SsDisable;
        else if (stack->size < MinSignalStackSize) { *__error_nid_postfix() = 12; return -1; }
        else replacement = {stack->sp, stack->size, 0};
    }
    if (previous != nullptr) *previous = alternateStack;
    alternateStack = replacement;
    return 0;
}
int APS5_VABI _sigprocmask_nid_postfix(int how, const GuestSignalSet* set, GuestSignalSet* previousSet) {
    SignalApi api;
    if (set && (how < 1 || how > 3)) { *__error_nid_postfix() = 22; return -1; }
    std::lock_guard lock(Registration());
    const auto replacement = set ? *set : GuestSignalSet{};
    if (previousSet != nullptr) *previousSet = api.state->mask;
    if (set != nullptr) {
        for (int i = 0; i != 4; ++i) {
            switch (how) {
                case 1: api.state->mask.bits[i] |= replacement.bits[i]; break;
                case 2: api.state->mask.bits[i] &= ~replacement.bits[i]; break;
                case 3: api.state->mask.bits[i] = replacement.bits[i]; break;
            }
        }
        Sanitize(api.state->mask);
    }
    return 0;
}

int APS5_VABI sigprocmask_nid_postfix(int how, const void* set, void* previousSet) {
    return _sigprocmask_nid_postfix(how, static_cast<const GuestSignalSet*>(set),
                                    static_cast<GuestSignalSet*>(previousSet));
}

int APS5_VABI pthread_sigmask_nid_postfix(int how, const GuestSignalSet* set, GuestSignalSet* previousSet) {
    if (set != nullptr && (how < 1 || how > 3)) return 22;
    return _sigprocmask_nid_postfix(how, set, previousSet);
}

int APS5_VABI sigtimedwait_nid_postfix(const GuestSignalSet* set, void* info, const void* timeout) {
    return WaitForSignal(set, static_cast<GuestSignalInfo*>(info), static_cast<const KernelTimespec*>(timeout), false);
}

int APS5_VABI sigwaitinfo_nid_postfix(const GuestSignalSet* set, void* info) {
    return sigtimedwait_nid_postfix(set, info, nullptr);
}

int APS5_VABI sigwait_nid_postfix(const GuestSignalSet* set, int* received) {
    if (received == nullptr) return 14;
    const int saved = *__error_nid_postfix();
    int result;
    do { result = sigtimedwait_nid_postfix(set, nullptr, nullptr); } while (result == -1 && *__error_nid_postfix() == 4);
    const int error = result < 0 ? *__error_nid_postfix() : 0;
    *__error_nid_postfix() = saved;
    if (result > 0) *received = result;
    return error;
}

int APS5_VABI pause_nid_postfix(void) {
    const GuestSignalSet empty{};
    return WaitForSignal(&empty, nullptr, nullptr, true);
}
}

extern "C" {

int APS5_VABI _is_signal_return_nid_postfix(std::uint64_t programCounter) {
    (void)programCounter;
    return 0;
}

}
