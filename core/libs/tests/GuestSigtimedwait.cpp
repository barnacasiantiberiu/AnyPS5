#include "prx/libc/include/DaemonRuntime.hpp"
#include "SceTypes.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>

using Handler = void (APS5_VABI *)(int);
extern "C" {
int APS5_VABI sigtimedwait_nid_postfix(const GuestSignalSet*, void*, const KernelTimespec*);
int APS5_VABI sigwaitinfo_nid_postfix(const GuestSignalSet*, void*);
int APS5_VABI sigwait_nid_postfix(const GuestSignalSet*, int*);
int APS5_VABI pause_nid_postfix();
Handler APS5_VABI signal_nid_postfix(int, Handler);
int APS5_VABI sigaction_nid_postfix(int, const GuestSigaction*, GuestSigaction*);
int APS5_VABI raise_nid_postfix(int);
int APS5_VABI kill_nid_postfix(int, int);
int APS5_VABI pthread_kill_nid_postfix(Pthread, int);
int APS5_VABI pthread_sigmask_nid_postfix(int, const GuestSignalSet*, GuestSignalSet*);
int APS5_VABI _sigprocmask_nid_postfix(int, const GuestSignalSet*, GuestSignalSet*);
int APS5_VABI scePthreadCreate(Pthread*, const PthreadAttr*, PthreadEntry, void*, const char*);
int APS5_VABI scePthreadJoin(Pthread, void**);
int APS5_VABI scePthreadCancel(Pthread);
Pthread APS5_VABI scePthreadSelf();
int APS5_VABI getpid_nid_postfix();
int* APS5_VABI __error_nid_postfix();
unsigned int APS5_VABI GuestAlarm_nid_no_patch(unsigned int);
}

struct SignalInfo {
    int signal, error, code, pid;
    std::uint32_t uid;
    int status;
    void* address;
    std::uint64_t value;
    std::uint64_t reason[5];
};
static_assert(sizeof(SignalInfo) == 80);
static std::atomic<int> callbacks{}, callbackSignal{}, infoCode{};
static std::atomic<Pthread> callbackThread{};
static std::atomic<bool> contextReceived{};
static GuestSignalSet callbackMask{};
static int nesting = 0, maximumNesting = 0, nestedCalls = 0;

static void Require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "Signal wait test: %s\n", message);
    std::abort();
}
static GuestSignalSet Set(std::initializer_list<int> signals) {
    GuestSignalSet result{};
    for (const int signal : signals) result.bits[(signal - 1) / 32] |= 1u << ((signal - 1) % 32);
    return result;
}
static bool Same(const GuestSignalSet& a, const GuestSignalSet& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
static void Mask(const GuestSignalSet& mask) { Require(pthread_sigmask_nid_postfix(3, &mask, nullptr) == 0, "set thread mask"); }
template<class Predicate>
static void Until(Predicate predicate, const char* message) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate() && std::chrono::steady_clock::now() < end) std::this_thread::yield();
    Require(predicate(), message);
}
static void APS5_VABI Callback(int signal) {
    callbackThread.store(scePthreadSelf());
    callbackSignal.store(signal);
    callbacks.fetch_add(1);
}
static void APS5_VABI InfoCallback(int signal, void* information, void* context) {
    infoCode.store(static_cast<SignalInfo*>(information)->code);
    contextReceived.store(context != nullptr);
    pthread_sigmask_nid_postfix(0, nullptr, &callbackMask);
    Callback(signal);
    *__error_nid_postfix() = 999;
}
static void APS5_VABI NestedCallback(int signal) {
    ++nesting;
    if (nesting > maximumNesting) maximumNesting = nesting;
    if (++nestedCalls == 1) raise_nid_postfix(signal);
    --nesting;
}
static void APS5_VABI ThrowingCallback(int) { throw std::runtime_error("signal callback failure"); }
static void Join(Pthread thread) { Require(scePthreadJoin(thread, nullptr) == 0, "join guest thread"); }
static Pthread Start(PthreadEntry entry, void* data) {
    Pthread thread = nullptr;
    Require(scePthreadCreate(&thread, nullptr, entry, data, "signal-test") == 0, "create guest thread");
    return thread;
}

#ifdef _WIN32
static void HostAdoptionLifetime() {
    Mask(Set({}));
    for (int iteration = 0; iteration != 3; ++iteration) {
        std::atomic<Pthread> adopted{nullptr};
        std::atomic<bool> leave{false};
        std::thread native([&] {
            Mask(Set({30}));
            adopted.store(scePthreadSelf());
            while (!leave.load()) std::atomic_signal_fence(std::memory_order_seq_cst);
        });
        Until([&] { return adopted.load() != nullptr; }, "host thread adopted");
        const auto handle = adopted.load();
        Require(pthread_kill_nid_postfix(handle, 0) == 0, "live adopted thread accepts signal zero");
        for (int i = 0; i != 128; ++i)
            Require(pthread_kill_nid_postfix(handle, 30) == 0, "adopted thread retains blocked records");
        leave.store(true);
        native.join();
        Require(pthread_kill_nid_postfix(handle, 0) == 3, "exited adopted thread rejects signal zero");
        Require(pthread_kill_nid_postfix(handle, 30) == 3, "exited adopted thread rejects signal delivery");
        Mask(Set({30}));
        Require(raise_nid_postfix(30) == 0, "queue after adopted thread exit");
        const auto selected = Set({30});
        const KernelTimespec poll{0, 0};
        SignalInfo info{};
        Require(sigtimedwait_nid_postfix(&selected, &info, &poll) == 30 && info.code == 0x10007,
            "adopted thread exit releases pending metadata capacity");
        Mask(Set({}));
    }
}
#endif

static void PendingAndErrors() {
    const auto set = Set({30, 100, 128});
    Mask(set);
    const KernelTimespec poll{0, 0};
    struct GuardedInfo { std::uint64_t before; SignalInfo info; std::uint64_t after; } output;
    std::memset(&output, 0xa5, sizeof(output));
    const auto unchanged = output;
    Require(sigtimedwait_nid_postfix(&set, &output.info, &poll) == -1 && *__error_nid_postfix() == 35, "empty poll returns EAGAIN");
    Require(std::memcmp(&output, &unchanged, sizeof(output)) == 0, "failed poll leaves output untouched");
    for (const auto invalid : {KernelTimespec{0, -1}, KernelTimespec{0, 1000000000}}) {
        Require(sigtimedwait_nid_postfix(&set, &output.info, &invalid) == -1 && *__error_nid_postfix() == 22, "invalid timeout returns EINVAL");
        Require(std::memcmp(&output, &unchanged, sizeof(output)) == 0, "invalid timeout leaves output untouched");
    }
    const KernelTimespec expired{-1, 0};
    Require(sigtimedwait_nid_postfix(&set, nullptr, &expired) == -1 && *__error_nid_postfix() == 35, "negative seconds expire with FreeBSD EAGAIN");
    Require(sigtimedwait_nid_postfix(nullptr, &output.info, &poll) == -1 && *__error_nid_postfix() == 14, "null set returns EFAULT");
    *__error_nid_postfix() = 741;
    int received = 19;
    Require(sigwait_nid_postfix(nullptr, &received) == 14 && received == 19 && *__error_nid_postfix() == 741, "sigwait error preserves output and errno");
    Require(sigwait_nid_postfix(&set, nullptr) == 14 && *__error_nid_postfix() == 741, "sigwait rejects null output");
    GuestSignalSet previous{{1, 2, 3, 4}};
    const auto previousCopy = previous;
    Require(_sigprocmask_nid_postfix(0, &set, &previous) == -1 && *__error_nid_postfix() == 22 && Same(previous, previousCopy), "invalid mask operation leaves output untouched");
    Require(raise_nid_postfix(30) == 0 && raise_nid_postfix(30) == 0, "blocked signal becomes pending");
    *__error_nid_postfix() = 742;
    Require(sigtimedwait_nid_postfix(&set, &output.info, &poll) == 30 && *__error_nid_postfix() == 742, "consume pending signal preserving errno");
    Require(output.info.signal == 30 && output.info.code == 0x10007 && output.info.pid == getpid_nid_postfix() && output.info.uid == 0, "thread-directed SI_LWP information");
    Require(output.info.error == 0 && output.info.status == 0 && output.info.address == nullptr && output.info.value == 0, "unused signal fields initialized");
    for (const auto value : output.info.reason) Require(value == 0, "reason padding initialized");
    Require(output.before == unchanged.before && output.after == unchanged.after, "siginfo writes exactly 80 bytes");
    const KernelTimespec invalidPending{0, 1000000000};
    Require(sigtimedwait_nid_postfix(&set, nullptr, &invalidPending) == 30, "second SI_LWP record retained and consumed before timeout validation");
    Require(sigtimedwait_nid_postfix(&set, nullptr, &poll) == -1 && *__error_nid_postfix() == 35, "both queued signal records consumed");
    for (int i = 0; i != 130; ++i) Require(raise_nid_postfix(30) == 0, "queue through FreeBSD default capacity");
    Require(sigtimedwait_nid_postfix(&set, &output.info, &poll) == 30 && output.info.code == 0 && output.info.pid == 0, "overflow coalesces into metadata-free signal first");
    for (int i = 0; i != 128; ++i)
        Require(sigtimedwait_nid_postfix(&set, &output.info, &poll) == 30 && output.info.code == 0x10007, "queued metadata survives overflow");
    Require(sigtimedwait_nid_postfix(&set, nullptr, &poll) == -1 && *__error_nid_postfix() == 35, "overflow and all metadata consumed exactly once");
    Require(raise_nid_postfix(100) == 0 && sigwaitinfo_nid_postfix(&set, nullptr) == 100, "third mask word retained");
    Require(raise_nid_postfix(128) == 0 && sigwait_nid_postfix(&set, &received) == 0 && received == 128, "last signal bit and sigwait result");
    const KernelTimespec shortWait{0, 30000000};
    const auto begin = std::chrono::steady_clock::now();
    Require(sigtimedwait_nid_postfix(&set, nullptr, &shortWait) == -1 && *__error_nid_postfix() == 35, "relative timeout expires");
    Require(std::chrono::steady_clock::now() - begin >= std::chrono::milliseconds(20), "relative wait does not return immediately");
    Mask(Set({9, 17}));
    GuestSignalSet actual{};
    Require(pthread_sigmask_nid_postfix(0, nullptr, &actual) == 0 && Same(actual, Set({})), "SIGKILL and SIGSTOP cannot be blocked");
}

struct Waiting {
    GuestSignalSet mask = Set({30, 100});
    std::atomic<bool> ready{false};
    int mode = 0, result = -99, error = -99, received = -99;
    SignalInfo info{};
};
static void* APS5_VABI Waiter(void* argument) {
    auto& data = *static_cast<Waiting*>(argument);
    Mask(data.mask);
    *__error_nid_postfix() = 731;
    data.ready.store(true);
    if (data.mode == 0) data.result = sigwaitinfo_nid_postfix(&data.mask, &data.info);
    else if (data.mode == 1) data.result = sigwait_nid_postfix(&data.mask, &data.received);
    else {
        const KernelTimespec timeout{0, 180000000};
        data.result = sigtimedwait_nid_postfix(&data.mask, &data.info, &timeout);
    }
    data.error = *__error_nid_postfix();
    return nullptr;
}
static void ThreadAndProcessPending() {
    Mask(Set({30, 100}));
    for (const int mode : {0, 1}) {
        Waiting data;
        data.mode = mode;
        auto thread = Start(Waiter, &data);
        Until([&] { return data.ready.load(); }, "waiter ready");
        Require(pthread_kill_nid_postfix(thread, 0) == 0, "live thread signal zero");
        Require(pthread_kill_nid_postfix(thread, 100) == 0, "cross-thread delivery");
        Join(thread);
        Require(data.result == (mode == 0 ? 100 : 0) && data.error == 731, "wait consumes target signal preserving errno");
        Require(mode == 0 ? data.info.signal == 100 : data.received == 100, "wait output contains number");
        Require(pthread_kill_nid_postfix(thread, 0) == 3 && pthread_kill_nid_postfix(thread, 30) == 3, "exited thread rejected");
    }
    Waiting first, second;
    first.mode = second.mode = 2;
    auto a = Start(Waiter, &first), b = Start(Waiter, &second);
    Until([&] { return first.ready.load() && second.ready.load(); }, "two waiters ready");
    Require(pthread_kill_nid_postfix(a, 30) == 0, "thread-specific signal");
    Join(a); Join(b);
    Require(first.result == 30 && second.result == -1 && second.error == 35, "second waiter cannot steal thread signal");
    first.ready.store(false); second.ready.store(false);
    a = Start(Waiter, &first); b = Start(Waiter, &second);
    Until([&] { return first.ready.load() && second.ready.load(); }, "process waiters ready");
    Require(kill_nid_postfix(getpid_nid_postfix(), 30) == 0, "process-directed signal");
    Join(a); Join(b);
    Require((first.result == 30) != (second.result == 30), "one process signal consumed by exactly one waiter");
    const auto& consumed = first.result == 30 ? first : second;
    const auto& expired = first.result == 30 ? second : first;
    Require(consumed.info.code == 0x10001 && expired.result == -1 && expired.error == 35, "SI_USER metadata and second waiter timeout");
    Require(pthread_kill_nid_postfix(reinterpret_cast<Pthread>(std::uintptr_t{0x10}), 0) == 3, "unknown handle rejected without dereference");
    Require(pthread_kill_nid_postfix(nullptr, 129) == 22, "invalid signal checked first");
}

struct Spinner {
    std::atomic<bool> ready{false}, unmask{false}, unmasked{false}, stop{false};
    GuestSignalSet inherited{};
};
static void* APS5_VABI Spin(void* argument) {
    auto& data = *static_cast<Spinner*>(argument);
    pthread_sigmask_nid_postfix(0, nullptr, &data.inherited);
    data.ready.store(true);
    while (!data.unmask.load()) std::atomic_signal_fence(std::memory_order_seq_cst);
    Mask(Set({}));
    data.unmasked.store(true);
    while (!data.stop.load()) std::atomic_signal_fence(std::memory_order_seq_cst);
    return nullptr;
}
static void AsynchronousDelivery() {
    const auto blocked = Set({30, 100, 128});
    Mask(blocked);
    signal_nid_postfix(30, Callback);
    Spinner data;
    callbacks.store(0);
    auto thread = Start(Spin, &data);
    Until([&] { return data.ready.load(); }, "spinner ready");
    Require(Same(data.inherited, blocked), "full mask inherited by new thread");
    Require(pthread_kill_nid_postfix(thread, 30) == 0 && callbacks.load() == 0, "masked target retains pending callback");
    data.unmask.store(true);
    Until([&] { return data.unmasked.load() && callbacks.load() == 1; }, "unblocking delivers pending callback");
    Require(callbackThread.load() == thread && callbackSignal.load() == 30, "callback executes on target thread");
    Require(pthread_kill_nid_postfix(thread, 30) == 0, "signal busy thread outside guest APIs");
    Until([&] { return callbacks.load() == 2; }, "actual asynchronous callback while spinning");
    GuestSignalSet current{};
    pthread_sigmask_nid_postfix(0, nullptr, &current);
    Require(Same(current, blocked), "child mask does not modify parent");
    data.stop.store(true);
    Join(thread);
    Require(pthread_kill_nid_postfix(thread, 0) == 3, "finished spinner rejected");
    Mask(Set({}));
    const GuestSigaction action{reinterpret_cast<std::uintptr_t>(InfoCallback), 0x40, Set({100})};
    Require(sigaction_nid_postfix(30, &action, nullptr) == 0, "install SA_SIGINFO callback");
    *__error_nid_postfix() = 888;
    Require(raise_nid_postfix(30) == 0 && infoCode.load() == 0x10007 && contextReceived.load(), "SA_SIGINFO metadata and context supplied");
    Require(*__error_nid_postfix() == 888 && Same(callbackMask, Set({30, 100})), "handler mask and interrupted errno");
    pthread_sigmask_nid_postfix(0, nullptr, &current);
    Require(Same(current, Set({})), "handler mask restored");
    signal_nid_postfix(30, Callback);
}

static void HandlerState() {
    for (const int flags : {0, 0x10}) {
        nesting = maximumNesting = nestedCalls = 0;
        const GuestSigaction action{reinterpret_cast<std::uintptr_t>(NestedCallback), flags, {}};
        Require(sigaction_nid_postfix(30, &action, nullptr) == 0 && raise_nid_postfix(30) == 0, "nested handler delivery");
        Require(nestedCalls == 2 && nesting == 0 && maximumNesting == (flags ? 2 : 1), "SA_NODEFER controls reentrant delivery");
    }
    const GuestSigaction reset{reinterpret_cast<std::uintptr_t>(Callback), 4, {}};
    GuestSigaction observed{};
    Require(sigaction_nid_postfix(30, &reset, nullptr) == 0 && raise_nid_postfix(30) == 0, "one-shot handler executes");
    Require(sigaction_nid_postfix(30, nullptr, &observed) == 0 && observed.handler == 0, "SA_RESETHAND resets disposition");
    const GuestSigaction throwing{reinterpret_cast<std::uintptr_t>(ThrowingCallback), 0, Set({100})};
    Require(sigaction_nid_postfix(30, &throwing, nullptr) == 0, "install throwing callback");
    *__error_nid_postfix() = 743;
    bool caught = false;
    try { raise_nid_postfix(30); } catch (const std::runtime_error&) { caught = true; }
    GuestSignalSet actual{};
    Require(caught && *__error_nid_postfix() == 743, "callback exception preserves interrupted errno");
    Require(pthread_sigmask_nid_postfix(0, nullptr, &actual) == 0 && Same(actual, Set({})), "callback exception restores interrupted mask");
    signal_nid_postfix(30, Callback);
}

struct QueryingMask {
    std::atomic<bool> ready{false}, stop{false};
};
static void APS5_VABI CriticalCallback(int signal) {
    Callback(signal);
    *__error_nid_postfix() = 991;
}
static void* APS5_VABI QueryMask(void* argument) {
    auto& data = *static_cast<QueryingMask*>(argument);
    Mask(Set({}));
    data.ready.store(true);
    while (!data.stop.load()) {
        GuestSignalSet mask{};
        *__error_nid_postfix() = 725;
        Require(pthread_sigmask_nid_postfix(0, nullptr, &mask) == 0 && Same(mask, Set({})), "concurrent mask query remains valid");
        Require(*__error_nid_postfix() == 725, "deferred handler preserves target errno");
    }
    return nullptr;
}
static void CriticalDelivery() {
    QueryingMask data;
    signal_nid_postfix(127, CriticalCallback);
    callbacks.store(0);
    auto thread = Start(QueryMask, &data);
    Until([&] { return data.ready.load(); }, "mask query worker ready");
    for (int i = 1; i <= 16; ++i) {
        *__error_nid_postfix() = 751;
        Require(pthread_kill_nid_postfix(thread, 127) == 0 && *__error_nid_postfix() == 751, "send while target queries its signal mask preserves sender errno");
        Until([&] { return callbacks.load() == i; }, "deferred handler runs after target releases signal state");
        Require(callbackThread.load() == thread && callbackSignal.load() == 127, "deferred handler keeps target thread identity");
    }
    data.stop.store(true);
    Join(thread);
    signal_nid_postfix(127, nullptr);
}

struct Interrupted {
    std::atomic<bool> ready{false};
    int mode = 0, result = 0, error = 0, received = 0;
};
static std::atomic<bool> interruptHandlerEntered{}, secondInterruptDelivered{};
static void APS5_VABI InterruptHandler(int signal) {
    if (signal == 31) {
        callbackThread.store(scePthreadSelf());
        secondInterruptDelivered.store(true);
        return;
    }
    interruptHandlerEntered.store(true);
    Until([] { return secondInterruptDelivered.load(); }, "second signal interrupts handler running inside pause");
}
static void* APS5_VABI InterruptWait(void* argument) {
    auto& data = *static_cast<Interrupted*>(argument);
    const auto selected = Set({100});
    Mask(selected);
    data.ready.store(true);
    if (data.mode == 0) data.result = pause_nid_postfix();
    else if (data.mode == 1) data.result = sigwaitinfo_nid_postfix(&selected, nullptr);
    else data.result = sigwait_nid_postfix(&selected, &data.received);
    data.error = *__error_nid_postfix();
    return nullptr;
}
static void InterruptionsAndCancellation() {
    {
        interruptHandlerEntered.store(false);
        secondInterruptDelivered.store(false);
        signal_nid_postfix(30, InterruptHandler);
        signal_nid_postfix(31, InterruptHandler);
        Interrupted data;
        auto thread = Start(InterruptWait, &data);
        Until([&] { return data.ready.load(); }, "nested interrupt waiter ready");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Require(pthread_kill_nid_postfix(thread, 30) == 0, "first signal interrupts pause");
        Until([] { return interruptHandlerEntered.load(); }, "first interrupt handler entered");
        Require(pthread_kill_nid_postfix(thread, 31) == 0, "second unblocked signal targets active handler");
        Join(thread);
        Require(secondInterruptDelivered.load() && callbackThread.load() == thread, "nested interrupt runs on the waiting thread");
        Require(data.result == -1 && data.error == 4, "pause reports EINTR after nested handlers return");
        signal_nid_postfix(30, Callback);
        signal_nid_postfix(31, nullptr);
    }
    for (const int mode : {0, 1, 2}) {
        Interrupted data;
        data.mode = mode;
        const auto before = callbacks.load();
        auto thread = Start(InterruptWait, &data);
        Until([&] { return data.ready.load(); }, "interruptible waiter ready");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Require(pthread_kill_nid_postfix(thread, 30) == 0, "interrupt waiting target");
        Until([&] { return callbacks.load() > before; }, "interrupting callback returns");
        if (mode == 2) Require(pthread_kill_nid_postfix(thread, 100) == 0, "sigwait continues after EINTR");
        Join(thread);
        if (mode == 2) Require(data.result == 0 && data.received == 100, "sigwait hides EINTR");
        else Require(data.result == -1 && data.error == 4, "pause and sigwaitinfo return EINTR");
    }
    Waiting data;
    auto thread = Start(Waiter, &data);
    Until([&] { return data.ready.load(); }, "cancelable waiter ready");
    Require(scePthreadCancel(thread) == 0, "cancel signal wait");
    void* returned = nullptr;
    Require(scePthreadJoin(thread, &returned) == 0 && returned == reinterpret_cast<void*>(std::uintptr_t{1}), "signal wait is a cancellation point");
    Require(pthread_kill_nid_postfix(thread, 0) == 3, "canceled waiter removed");
    const auto alarm = Set({14});
    Mask(alarm);
    GuestAlarm_nid_no_patch(1);
    const KernelTimespec timeout{2, 0};
    Require(sigtimedwait_nid_postfix(&alarm, nullptr, &timeout) == 14, "alarm feeds process signal wait");
    GuestAlarm_nid_no_patch(0);
    Mask(Set({}));
}
int main() {
#ifdef _WIN32
    HostAdoptionLifetime();
#endif
    PendingAndErrors();
    ThreadAndProcessPending();
    AsynchronousDelivery();
    HandlerState();
    CriticalDelivery();
    InterruptionsAndCancellation();
}
