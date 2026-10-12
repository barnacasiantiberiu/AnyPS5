#include "ModuleChecks.hpp"
#include "SceTypes.hpp"
#include "prx/libc/include/GuestProcess.hpp"
#include "prx/libc/include/ApplicationHeap.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>

void GuestForkCallbacks_nid_no_patch();

extern "C" {
int APS5_VABI fork_nid_postfix();
int APS5_VABI waitpid_nid_postfix(int, int*, int);
int APS5_VABI wait_nid_postfix(int*);
int APS5_VABI getpid_nid_postfix();
int APS5_VABI getppid_nid_postfix();
int APS5_VABI getsid_nid_postfix(int);
int APS5_VABI setsid_nid_postfix();
[[noreturn]] void APS5_VABI _exit_nid_postfix(int);
int* APS5_VABI __error_nid_postfix();
void* APS5_VABI malloc_nid_postfix(std::size_t);
void APS5_VABI free_nid_postfix(void*);
Pthread APS5_VABI scePthreadSelf();
int APS5_VABI open_nid_postfix(const char*, int, int);
int APS5_VABI close_nid_postfix(int);
int APS5_VABI fcntl_nid_postfix(int, int, ...);
int APS5_VABI ioctl_nid_postfix(int, std::uint64_t, void*);
std::int64_t APS5_VABI read_nid_postfix(int, void*, std::uint64_t);
std::int64_t APS5_VABI write_nid_postfix(int, const char*, std::int64_t);
std::int64_t APS5_VABI sceKernelLseek(int, std::int64_t, int);
int APS5_VABI getdents_nid_postfix(int, char*, int);
int APS5_VABI sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t, int, std::int64_t*);
int APS5_VABI sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
int APS5_VABI sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
int APS5_VABI sceKernelEnableDmemAliasing();
int APS5_VABI sceKernelMunmap(void*, std::size_t);
void APS5_VABI _Locksyslock_nid_postfix();
void APS5_VABI _Unlocksyslock_nid_postfix();
int APS5_VABI std_execute_once_nid_postfix(int*, int (APS5_VABI *)(void*, void*, void**), void*);
int APS5_VABI setenv_nid_postfix(const char*, const char*, int);
char* APS5_VABI getenv_nid_postfix(const char*);
void APS5_VABI srand_nid_postfix(unsigned);
int APS5_VABI rand_nid_postfix();
int APS5_VABI __cxa_atexit_nid_postfix(void (APS5_VABI *)(void*), void*, void*);
void APS5_VABI cxa_finalize_nid_postfix(void*);
}

namespace {

using ModuleTest::Require;
using ModuleTest::RequireThrows;
constexpr int ChildExit = 23;
constexpr int StatusGuard = 0x76543210;
int globalValue = 17;
thread_local int nativeTls = 29;

struct TlsTag {};

struct EventPair {
    EventPair() {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        ready = CreateEventW(&security, TRUE, FALSE, nullptr);
        release = CreateEventW(&security, TRUE, FALSE, nullptr);
        Require(ready && release);
    }
    ~EventPair() {
        CloseHandle(ready);
        CloseHandle(release);
    }
    HANDLE ready = nullptr;
    HANDLE release = nullptr;
};

void Await(HANDLE event) {
    Require(WaitForSingleObject(event, 3000) == WAIT_OBJECT_0);
}

void Reap(int pid, int expected = ChildExit) {
    int status = StatusGuard;
    Require(waitpid_nid_postfix(pid, &status, 0) == pid);
    Require(status == (expected << 8));
    status = StatusGuard;
    Require(waitpid_nid_postfix(pid, &status, 0) == -1);
    Require(*__error_nid_postfix() == 10 && status == StatusGuard);
}

void Memory() {
    std::array<void*, 10> allocator{};
    ApplicationHeapRegister_nid_no_patch(allocator.data());
    volatile int local = 21;
    auto heap = std::make_unique<int>(31);
    auto* guest = static_cast<int*>(malloc_nid_postfix(sizeof(int)));
    Require(guest != nullptr);
    *guest = 37;
    auto& hostTls = HostThreadLocal<int, TlsTag>();
    hostTls = 41;
    nativeTls = 43;
    const auto self = scePthreadSelf();
    const int parent = getpid_nid_postfix();
    const int child = fork_nid_postfix();
    Require(child >= 0);
    if (child == 0) {
        Require(getpid_nid_postfix() != parent && getppid_nid_postfix() == parent);
        Require(globalValue == 17 && local == 21 && *heap == 31 && *guest == 37);
        Require(HostThreadLocal<int, TlsTag>() == 41 && nativeTls == 43);
        Require(scePthreadSelf() == self);
        globalValue = 61;
        local = 67;
        *heap = 71;
        *guest = 73;
        HostThreadLocal<int, TlsTag>() = 79;
        nativeTls = 83;
        free_nid_postfix(guest);
        guest = static_cast<int*>(malloc_nid_postfix(4096));
        Require(guest != nullptr);
        *guest = 89;
        free_nid_postfix(guest);
        int result = 0;
        std::thread worker([&] { result = 97; });
        worker.join();
        Require(result == 97 && local == 67 && globalValue == 61 && *heap == 71);
        _exit_nid_postfix(ChildExit);
    }
    Require(child != parent);
    Reap(child);
    Require(globalValue == 17 && local == 21 && *heap == 31 && *guest == 37);
    Require(HostThreadLocal<int, TlsTag>() == 41 && nativeTls == 43 && scePthreadSelf() == self);
    free_nid_postfix(guest);
}

void Descriptors() {
    EventPair events;
    const std::string path = "anyps5-fork-descriptor-" + std::to_string(getpid_nid_postfix()) + ".dat";
    const int original = open_nid_postfix(path.c_str(), 0x602, 0600);
    Require(original >= 0);
    Require(write_nid_postfix(original, "abcdefgh", 8) == 8);
    Require(sceKernelLseek(original, 2, 0) == 2);
    const int high = fcntl_nid_postfix(original, 0, 600);
    Require(high >= 600);
    Require(fcntl_nid_postfix(high, 2, 1) == 0);
    const int child = fork_nid_postfix();
    Require(child >= 0);
    if (child == 0) {
        Require(fcntl_nid_postfix(high, 1) == 1);
        std::array<char, 2> bytes{};
        Require(read_nid_postfix(high, bytes.data(), bytes.size()) == 2);
        Require(bytes[0] == 'c' && bytes[1] == 'd');
        Require(fcntl_nid_postfix(high, 4, 6) == 0);
        Require(fcntl_nid_postfix(high, 2, 0) == 0);
        Require(SetEvent(events.ready));
        Await(events.release);
        Require((fcntl_nid_postfix(high, 3) & 4) == 0);
        Require(read_nid_postfix(high, bytes.data(), bytes.size()) == 2);
        Require(bytes[0] == 'g' && bytes[1] == 'h');
        Require(close_nid_postfix(high) == 0);
        Require(close_nid_postfix(original) == 0);
        _exit_nid_postfix(ChildExit);
    }
    Await(events.ready);
    Require(fcntl_nid_postfix(high, 1) == 1);
    Require((fcntl_nid_postfix(original, 3) & 4) != 0);
    Require((fcntl_nid_postfix(high, 3) & 4) != 0);
    std::array<char, 2> bytes{};
    Require(read_nid_postfix(original, bytes.data(), bytes.size()) == 2);
    Require(bytes[0] == 'e' && bytes[1] == 'f');
    Require(fcntl_nid_postfix(original, 4, 2) == 0);
    Require((fcntl_nid_postfix(high, 3) & 4) == 0);
    Require(SetEvent(events.release));
    Reap(child);
    Require(sceKernelLseek(original, 0, 1) == 8);
    Require(close_nid_postfix(high) == 0);
    Require(close_nid_postfix(original) == 0);
    Require(std::filesystem::remove(path));
}

void Sessions() {
    EventPair events;
    const int parent = getpid_nid_postfix();
    const int session = getsid_nid_postfix(0);
    Require(session == parent);
    Require(setsid_nid_postfix() == -1 && *__error_nid_postfix() == 1);
    const int child = fork_nid_postfix();
    Require(child >= 0);
    if (child == 0) {
        Require(getsid_nid_postfix(0) == session);
        const int self = getpid_nid_postfix();
        Require(setsid_nid_postfix() == self);
        Require(getsid_nid_postfix(self) == self);
        Require(setsid_nid_postfix() == -1 && *__error_nid_postfix() == 1);
        Require(SetEvent(events.ready));
        Await(events.release);
        _exit_nid_postfix(ChildExit);
    }
    Await(events.ready);
    Require(getsid_nid_postfix(child) == child);
    Require(getsid_nid_postfix(0) == session);
    int status = StatusGuard;
    Require(waitpid_nid_postfix(0, &status, 1) == -1);
    Require(*__error_nid_postfix() == 10 && status == StatusGuard);
    Require(SetEvent(events.release));
    Require(waitpid_nid_postfix(-child, &status, 0) == child && status == (ChildExit << 8));
    Require(getsid_nid_postfix(child) == -1 && *__error_nid_postfix() == 3);
    Require(getsid_nid_postfix(-2) == -1 && *__error_nid_postfix() == 3);
}

void DescriptorConcurrency() {
    EventPair events;
    const std::string path = "anyps5-fork-flags-" + std::to_string(getpid_nid_postfix()) + ".dat";
    const int descriptor = open_nid_postfix(path.c_str(), 0x602, 0600);
    Require(descriptor >= 0);
    const int alias = fcntl_nid_postfix(descriptor, 0, 20);
    Require(alias >= 20);
    const int child = fork_nid_postfix();
    Require(child >= 0);
    if (child == 0) {
        Require(SetEvent(events.ready));
        Await(events.release);
        for (int iteration = 0; iteration < 500; ++iteration) {
            int enabled = iteration & 1;
            Require(ioctl_nid_postfix(alias, 0x8004667e, &enabled) == 0);
        }
        int enabled = 1;
        Require(ioctl_nid_postfix(alias, 0x8004667e, &enabled) == 0);
        _exit_nid_postfix(ChildExit);
    }
    Await(events.ready);
    Require(SetEvent(events.release));
    for (int iteration = 0; iteration < 500; ++iteration)
        Require(fcntl_nid_postfix(descriptor, 15, (iteration & 1) ? 0 : -1) == 0);
    Require(fcntl_nid_postfix(descriptor, 15, 0) == 0);
    Reap(child);
    Require(fcntl_nid_postfix(descriptor, 3) == 0x206);
    Require(fcntl_nid_postfix(alias, 3) == 0x206);
    Require(close_nid_postfix(alias) == 0);
    Require(close_nid_postfix(descriptor) == 0);
    Require(std::filesystem::remove(path));
}

void Directory() {
    const std::string path = "anyps5-fork-directory-" + std::to_string(getpid_nid_postfix());
    Require(std::filesystem::create_directory(path));
    { std::ofstream stream(std::filesystem::path(path) / "a"); stream << 'x'; }
    const int descriptor = open_nid_postfix(path.c_str(), 0x20000, 0);
    Require(descriptor >= 0);
    std::array<char, 12> entry{};
    Require(getdents_nid_postfix(descriptor, entry.data(), entry.size()) == 12);
    Require(std::strcmp(entry.data() + 8, ".") == 0);
    int child = fork_nid_postfix();
    Require(child >= 0);
    if (child == 0) {
        Require(getdents_nid_postfix(descriptor, entry.data(), entry.size()) == 12);
        Require(std::strcmp(entry.data() + 8, "..") == 0);
        Require(close_nid_postfix(descriptor) == 0);
        _exit_nid_postfix(ChildExit);
    }
    Reap(child);
    Require(getdents_nid_postfix(descriptor, entry.data(), entry.size()) == 12);
    Require(std::strcmp(entry.data() + 8, "a") == 0);
    Require(getdents_nid_postfix(descriptor, entry.data(), entry.size()) == 0);
    child = fork_nid_postfix();
    Require(child >= 0);
    if (child == 0) {
        Require(sceKernelLseek(descriptor, 0, 0) == 0);
        _exit_nid_postfix(ChildExit);
    }
    Reap(child);
    Require(getdents_nid_postfix(descriptor, entry.data(), entry.size()) == 12);
    Require(std::strcmp(entry.data() + 8, ".") == 0);
    Require(close_nid_postfix(descriptor) == 0);
    Require(std::filesystem::remove(std::filesystem::path(path) / "a"));
    Require(std::filesystem::remove(path));
}

void Orphan() {
    EventPair events;
    EventPair completed;
    const int child = fork_nid_postfix();
    Require(child >= 0);
    if (child == 0) {
        const int self = getpid_nid_postfix();
        const int grandchild = fork_nid_postfix();
        Require(grandchild >= 0);
        if (grandchild == 0) {
            Require(getppid_nid_postfix() == self);
            Require(SetEvent(events.ready));
            Await(events.release);
            Require(getppid_nid_postfix() == 1);
            Require(SetEvent(completed.ready));
            _exit_nid_postfix(ChildExit);
        }
        Await(events.ready);
        _exit_nid_postfix(ChildExit);
    }
    Await(events.ready);
    Reap(child);
    Require(SetEvent(events.release));
    Await(completed.ready);
}

void Mappings() {
    constexpr std::size_t page = 0x4000;
    std::int64_t physical = 0;
    Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, page, 0, 0, &physical) == 0);
    Require(sceKernelEnableDmemAliasing() == 0);
    void* first = nullptr;
    void* second = nullptr;
    Require(sceKernelMapDirectMemory(&first, page, 3, 0, physical, 0) == 0);
    Require(sceKernelMapDirectMemory(&second, page, 3, 0, physical, 0) == 0);
    static_cast<unsigned char*>(first)[0] = 41;
    auto* privatePages = static_cast<unsigned char*>(GuestArena::GuestArenaAllocate_nid_postfix(page * 2, page));
    Require(privatePages != nullptr);
    GuestArena::GuestArenaCommit_nid_postfix(privatePages, page, PAGE_READWRITE, page);
    privatePages[0] = 43;
    const int child = fork_nid_postfix();
    Require(child >= 0);
    if (child == 0) {
        Require(static_cast<unsigned char*>(first)[0] == 41 && static_cast<unsigned char*>(second)[0] == 41);
        Require(privatePages[0] == 43);
        auto* nativeAlias = GuestArena::GuestArenaMapAlias_nid_postfix(reinterpret_cast<std::uintptr_t>(first), page);
        Require(nativeAlias != nullptr && static_cast<unsigned char*>(nativeAlias)[0] == 41);
        static_cast<unsigned char*>(nativeAlias)[0] = 47;
        GuestArena::GuestArenaUnmapAlias_nid_postfix(nativeAlias);
        Require(sceKernelMunmap(second, page) == 0);
        second = nullptr;
        Require(sceKernelMapDirectMemory(&second, page, 3, 0, physical, 0) == 0);
        Require(static_cast<unsigned char*>(second)[0] == 47);
        static_cast<unsigned char*>(second)[1] = 53;
        GuestArena::GuestArenaReset_nid_postfix(privatePages, page);
        GuestArena::GuestArenaCommit_nid_postfix(privatePages, page * 2, PAGE_READWRITE, page);
        Require(privatePages[0] == 0 && privatePages[page] == 0);
        privatePages[0] = 59;
        privatePages[page] = 61;
        auto* fresh = GuestArena::GuestArenaAllocate_nid_postfix(page, page);
        Require(fresh != nullptr);
        GuestArena::GuestArenaCommit_nid_postfix(fresh, page, PAGE_READWRITE, page);
        *static_cast<unsigned char*>(fresh) = 67;
        GuestArena::GuestArenaReset_nid_postfix(fresh, page);
        GuestArena::GuestArenaRelease_nid_postfix(fresh, page);
        Require(sceKernelMunmap(first, page) == 0);
        Require(sceKernelMunmap(second, page) == 0);
        Require(sceKernelReleaseDirectMemory(physical, page) == 0);
        _exit_nid_postfix(ChildExit);
    }
    Reap(child);
    Require(static_cast<unsigned char*>(first)[0] == 47 && static_cast<unsigned char*>(first)[1] == 53);
    Require(static_cast<unsigned char*>(second)[0] == 47 && static_cast<unsigned char*>(second)[1] == 53);
    Require(privatePages[0] == 43);
    Require(sceKernelMunmap(first, page) == 0);
    Require(sceKernelMunmap(second, page) == 0);
    Require(sceKernelReleaseDirectMemory(physical, page) == 0);
    GuestArena::GuestArenaReset_nid_postfix(privatePages, page);
    GuestArena::GuestArenaRelease_nid_postfix(privatePages, page * 2);
}

void Waits() {
    EventPair events;
    const int child = fork_nid_postfix();
    Require(child >= 0);
    if (child == 0) {
        Require(SetEvent(events.ready));
        Await(events.release);
        _exit_nid_postfix(ChildExit);
    }
    Await(events.ready);
    int status = StatusGuard;
    Require(waitpid_nid_postfix(child, &status, 1) == 0 && status == StatusGuard);
    Require(waitpid_nid_postfix(-1, &status, 1) == 0 && status == StatusGuard);
    Require(waitpid_nid_postfix(child, &status, 0x40) == -1);
    Require(*__error_nid_postfix() == 22 && status == StatusGuard);
    RequireThrows([&] { waitpid_nid_postfix(child, &status, 2); }, "waitpid");
    Require(status == StatusGuard);
    Require(SetEvent(events.release));
    Require(waitpid_nid_postfix(child, &status, 8) == child && status == (ChildExit << 8));
    Require(waitpid_nid_postfix(child, &status, 8) == child && status == (ChildExit << 8));
    Require(wait_nid_postfix(&status) == child && status == (ChildExit << 8));
    status = StatusGuard;
    Require(wait_nid_postfix(&status) == -1 && *__error_nid_postfix() == 10 && status == StatusGuard);
    const int next = fork_nid_postfix();
    Require(next >= 0);
    if (next == 0) _exit_nid_postfix(0);
    Require(waitpid_nid_postfix(next, nullptr, 0) == next);
}

struct HookState {
    int number;
    int failure = 0;
    std::vector<int>* order;
};

bool Prepare(void* opaque) {
    auto& state = *static_cast<HookState*>(opaque);
    state.order->push_back(state.number);
    if (state.failure == 1) { errno = 12; return false; }
    if (state.failure == 4) throw std::runtime_error("fork prepare rejection");
    return true;
}

bool Parent(void* opaque, bool created) noexcept {
    auto& state = *static_cast<HookState*>(opaque);
    state.order->push_back(state.number + (created ? 10 : 30));
    return state.failure != 2;
}

bool Child(void* opaque) noexcept {
    auto& state = *static_cast<HookState*>(opaque);
    state.order->push_back(state.number + 20);
    if (state.failure == 3) { errno = 35; return false; }
    return true;
}

void Hooks(int failure) {
    EventPair events;
    std::vector<int> order;
    order.reserve(32);
    HookState first{1, 0, &order};
    HookState second{2, failure, &order};
    const GuestForkHooks firstHooks{4000, Prepare, Parent, Child, &first};
    const GuestForkHooks secondHooks{4001, Prepare, Parent, Child, &second};
    Require(GuestRegisterForkHooks_nid_no_patch(&secondHooks) == 0);
    Require(GuestRegisterForkHooks_nid_no_patch(&firstHooks) == 0);
    Require(GuestRegisterForkHooks_nid_no_patch(&firstHooks) == 0);
    Require(GuestRegisterForkHooks_nid_no_patch(nullptr) == 22);
    int child = fork_nid_postfix();
    if (failure != 0) {
        if (child == 0) {
            SetEvent(events.ready);
            _exit_nid_postfix(92);
        }
        Require(child == -1);
        Require(*__error_nid_postfix() == (failure == 1 ? 12 : failure == 2 ? 5 : 35));
        Require(WaitForSingleObject(events.ready, 0) == WAIT_TIMEOUT);
        Require(order == (failure == 1 ? std::vector<int>{1, 2, 32, 31} : std::vector<int>{1, 2, 12, 11}));
        int status = StatusGuard;
        Require(waitpid_nid_postfix(-1, &status, 1) == -1);
        Require(*__error_nid_postfix() == 10 && status == StatusGuard);
        if (failure == 1) {
            second.failure = 4;
            order.clear();
            RequireThrows([] { fork_nid_postfix(); }, "fork prepare rejection");
            Require(order == std::vector<int>({1, 2, 32, 31}));
        }
        second.failure = 0;
        order.clear();
        child = fork_nid_postfix();
    }
    Require(child >= 0);
    if (child == 0) {
        Require(order == std::vector<int>({1, 2, 22, 21}));
        _exit_nid_postfix(ChildExit);
    }
    Require(order == std::vector<int>({1, 2, 12, 11}));
    Reap(child);
}

void Handles() {
    int child = fork_nid_postfix();
    Require(child >= 0);
    if (child == 0) _exit_nid_postfix(ChildExit);
    Reap(child);
    DWORD before = 0;
    Require(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (int iteration = 0; iteration < 8; ++iteration) {
        child = fork_nid_postfix();
        Require(child >= 0);
        if (child == 0) _exit_nid_postfix(ChildExit);
        Reap(child);
    }
    DWORD after = 0;
    Require(GetProcessHandleCount(GetCurrentProcess(), &after));
    Require(after == before);
}

void APS5_VABI CountDestructor(void* value) { ++*static_cast<int*>(value); }

int APS5_VABI ForkOnce(void*, void* value, void**) {
    *static_cast<int*>(value) = fork_nid_postfix();
    return 1;
}

int APS5_VABI CountOnce(void*, void* value, void**) {
    ++*static_cast<int*>(value);
    return 1;
}

void RuntimeState() {
    Require(setenv_nid_postfix("ANYPS5_FORK_STATE", "parent", 1) == 0);
    srand_nid_postfix(321);
    const int expected = rand_nid_postfix();
    srand_nid_postfix(321);
    int destructorCalls = 0;
    int dso = 0;
    Require(__cxa_atexit_nid_postfix(CountDestructor, &destructorCalls, &dso) == 0);
    _Locksyslock_nid_postfix();
    _Locksyslock_nid_postfix();
    int once = 0;
    int child = -2;
    Require(std_execute_once_nid_postfix(&once, ForkOnce, &child) == 1);
    Require(once == 1 && child >= 0);
    _Unlocksyslock_nid_postfix();
    _Unlocksyslock_nid_postfix();
    Require(rand_nid_postfix() == expected);
    Require(std::strcmp(getenv_nid_postfix("ANYPS5_FORK_STATE"), "parent") == 0);
    if (child == 0) {
        std::thread nextOwner([] {
            _Locksyslock_nid_postfix();
            _Locksyslock_nid_postfix();
            int freshOnce = 0;
            int calls = 0;
            Require(std_execute_once_nid_postfix(&freshOnce, CountOnce, &calls) == 1);
            Require(std_execute_once_nid_postfix(&freshOnce, CountOnce, &calls) == 1);
            Require(freshOnce == 1 && calls == 1);
            _Unlocksyslock_nid_postfix();
            _Unlocksyslock_nid_postfix();
        });
        nextOwner.join();
        Require(setenv_nid_postfix("ANYPS5_FORK_STATE", "child", 1) == 0);
        cxa_finalize_nid_postfix(&dso);
        Require(destructorCalls == 1);
        _exit_nid_postfix(ChildExit);
    }
    Reap(child);
    Require(destructorCalls == 0);
    Require(std::strcmp(getenv_nid_postfix("ANYPS5_FORK_STATE"), "parent") == 0);
    cxa_finalize_nid_postfix(&dso);
    Require(destructorCalls == 1);
}

void RejectedOwners() {
    const auto expectFailure = [] {
        const int child = fork_nid_postfix();
        if (child == 0) _exit_nid_postfix(96);
        if (child > 0) {
            int status = 0;
            waitpid_nid_postfix(child, &status, 0);
        }
        Require(child == -1 && *__error_nid_postfix() == 35);
    };
    GuestAllocations::GuestAllocationsSetGpuMapObserver_nid_postfix([](const GuestAllocations::Mapped&, std::uint64_t) {});
    expectFailure();
    GuestAllocations::GuestAllocationsSetGpuMapObserver_nid_postfix(nullptr);
    GuestAllocations::GuestAllocationsSetPinWaiter_nid_postfix([](std::uintptr_t, std::size_t) { return true; });
    expectFailure();
    GuestAllocations::GuestAllocationsSetPinWaiter_nid_postfix(nullptr);
    EventPair held;
    std::thread owner([&] {
        _Locksyslock_nid_postfix();
        Require(SetEvent(held.ready));
        Await(held.release);
        _Unlocksyslock_nid_postfix();
    });
    Await(held.ready);
    expectFailure();
    Require(SetEvent(held.release));
    owner.join();
    const int child = fork_nid_postfix();
    Require(child >= 0);
    if (child == 0) _exit_nid_postfix(ChildExit);
    Reap(child);
}

void Run(std::string_view name) {
    if (name == "memory") Memory();
    else if (name == "descriptors") Descriptors();
    else if (name == "descriptor-concurrency") DescriptorConcurrency();
    else if (name == "directory") Directory();
    else if (name == "orphan") Orphan();
    else if (name == "mappings") Mappings();
    else if (name == "sessions") Sessions();
    else if (name == "waits") Waits();
    else if (name == "hooks-order") Hooks(0);
    else if (name == "hooks-prepare-failure") Hooks(1);
    else if (name == "hooks-parent-failure") Hooks(2);
    else if (name == "hooks-child-failure") Hooks(3);
    else if (name == "handles") Handles();
    else if (name == "runtime-state") RuntimeState();
    else if (name == "rejected-owners") RejectedOwners();
    else if (name == "callback-owners") GuestForkCallbacks_nid_no_patch();
    else Require(false);
}

bool ContainChild(void* context) noexcept {
    BOOL contained = FALSE;
    const auto job = static_cast<HANDLE>(context);
    return IsProcessInJob(GetCurrentProcess(), job, &contained) &&
        (contained || AssignProcessToJobObject(job, GetCurrentProcess()));
}

int Supervise(const char* name) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    const HANDLE job = CreateJobObjectW(&security, nullptr);
    Require(job != nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
    std::array<wchar_t, 32768> path{};
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    Require(size != 0 && size < path.size());
    std::wstring command = L"\"" + std::wstring(path.data(), size) + L"\" --run ";
    for (const char* at = name; *at; ++at) command.push_back(static_cast<unsigned char>(*at));
    command += L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(job));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION worker{};
    Require(CreateProcessW(path.data(), command.data(), nullptr, nullptr, TRUE,
                           CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &worker));
    const bool assigned = AssignProcessToJobObject(job, worker.hProcess) != 0;
    if (!assigned) TerminateProcess(worker.hProcess, 93);
    Require(assigned);
    Require(ResumeThread(worker.hThread) != static_cast<DWORD>(-1));
    CloseHandle(worker.hThread);
    const DWORD waited = WaitForSingleObject(worker.hProcess, 15000);
    DWORD code = 94;
    if (waited == WAIT_OBJECT_0) Require(GetExitCodeProcess(worker.hProcess, &code));
    else TerminateJobObject(job, code);
    // The inherited job handle permits nested forks to join the same job;
    // explicitly terminate remaining descendants even when a worker crashes.
    Require(TerminateJobObject(job, 94));
    CloseHandle(worker.hProcess);
    CloseHandle(job);
    if (code != 0) std::fprintf(stderr, "Fork case %s exited %lu, wait=%lu\n", name, code, waited);
    Require(waited == WAIT_OBJECT_0 && code == 0);
    return 0;
}

}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc == 4 && std::strcmp(argv[1], "--run") == 0) {
        const auto job = reinterpret_cast<HANDLE>(std::stoull(argv[3]));
        const GuestForkHooks containment{~0u, nullptr, nullptr, ContainChild, job};
        Require(GuestRegisterForkHooks_nid_no_patch(&containment) == 0);
        Run(argv[2]);
        CloseHandle(job);
        return 0;
    }
    Require(argc == 2);
    return Supervise(argv[1]);
}
