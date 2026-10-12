#include "prx/libc/include/GuestProcess.hpp"
#include "prx/libc/include/HostDescriptorFork.hpp"
#include "prx/libc/include/HostProcess.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr int GuestEperm = 1;
constexpr int GuestEsrch = 3;
constexpr int GuestEintr = 4;
constexpr int GuestEio = 5;
constexpr int GuestEchild = 10;
constexpr int GuestEnomem = 12;
constexpr int GuestEinval = 22;
constexpr int GuestEagain = 35;
constexpr int GuestEnosys = 78;
constexpr int GuestEoverflow = 84;
constexpr unsigned WaitNoHang = 1;
constexpr unsigned WaitNoReap = 8;
constexpr unsigned WaitOptions = 0x8000003f;
constexpr unsigned UnsupportedWaitOptions = 0x80000026;
constexpr unsigned ChildReadyTimeout = 5000;

int Fail(int error) {
    errno = error;
    return -1;
}

std::vector<GuestForkHooks>& ForkHooks() {
    static auto* hooks = new std::vector<GuestForkHooks>();
    return *hooks;
}

#ifdef _WIN32
SRWLOCK processLock = SRWLOCK_INIT;
#else
std::mutex processLock;
#endif

class ProcessLock {
public:
    ProcessLock() {
#ifdef _WIN32
        AcquireSRWLockExclusive(&processLock);
#else
        processLock.lock();
#endif
    }
    ~ProcessLock() {
        if (!held) return;
#ifdef _WIN32
        ReleaseSRWLockExclusive(&processLock);
#else
        processLock.unlock();
#endif
    }
    void Child() {
#ifdef _WIN32
        InitializeSRWLock(&processLock);
#else
        processLock.unlock();
#endif
        held = false;
    }
    ProcessLock(const ProcessLock&) = delete;
    ProcessLock& operator=(const ProcessLock&) = delete;

private:
    bool held = true;
};

bool ParentHooks(std::size_t prepared, bool created) noexcept {
    bool success = true;
    while (prepared != 0) {
        const auto& hook = ForkHooks()[--prepared];
        if (hook.parent && !hook.parent(hook.context, created)) success = false;
    }
    return success;
}

bool PrepareHooks(std::size_t& prepared) {
    for (const auto& hook : ForkHooks()) {
        ++prepared;
        if (hook.prepare) {
            errno = 0;
            if (!hook.prepare(hook.context)) return false;
        }
    }
    return true;
}

int ChildHooks(std::size_t prepared) noexcept {
    int error = 0;
    while (prepared != 0) {
        const auto& hook = ForkHooks()[--prepared];
        if (hook.child) {
            errno = 0;
            if (!hook.child(hook.context) && error == 0) error = errno > 0 ? errno : GuestEagain;
        }
    }
    return error;
}

#ifdef _WIN32

struct Identity {
    volatile LONG pid = 0;
    volatile LONG parent = 0;
    volatile LONG group = 0;
    volatile LONG session = 0;
    volatile LONG initialized = 0;
};

LONG Read(volatile LONG& value) {
    return InterlockedCompareExchange(&value, 0, 0);
}

struct ChildProcess {
    HANDLE process = nullptr;
    HANDLE mapping = nullptr;
    Identity* identity = nullptr;
    HANDLE parent = nullptr;
};

std::map<int, ChildProcess>& Children() {
    static auto* processes = new std::map<int, ChildProcess>();
    return *processes;
}
Identity initialIdentity;
Identity* currentIdentity = nullptr;
HANDLE currentMapping = nullptr;
HANDLE currentParent = nullptr;

int NativeParent() {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Querying parent process");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    int parent = -1;
    const DWORD self = GetCurrentProcessId();
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == self) {
            if (entry.th32ParentProcessID <= static_cast<DWORD>(std::numeric_limits<int>::max()))
                parent = static_cast<int>(entry.th32ParentProcessID);
            break;
        }
    }
    CloseHandle(snapshot);
    if (parent < 0) throw std::runtime_error("Current process is absent or its parent identifier exceeds the guest range");
    return parent;
}

Identity& CurrentIdentity() {
    if (currentIdentity == nullptr) {
        const DWORD self = GetCurrentProcessId();
        if (self > static_cast<DWORD>(std::numeric_limits<int>::max()))
            throw std::runtime_error("Guest process identifier exceeds the signed guest range");
        initialIdentity.pid = static_cast<LONG>(self);
        initialIdentity.parent = NativeParent();
        currentParent = OpenProcess(SYNCHRONIZE, TRUE, static_cast<DWORD>(initialIdentity.parent));
        initialIdentity.group = static_cast<LONG>(self);
        initialIdentity.session = static_cast<LONG>(self);
        initialIdentity.initialized = 1;
        currentIdentity = &initialIdentity;
    }
    return *currentIdentity;
}

void CloseIdentity(ChildProcess& child) noexcept {
    if (child.identity) UnmapViewOfFile(child.identity);
    if (child.mapping) CloseHandle(child.mapping);
    if (child.parent) CloseHandle(child.parent);
    child.identity = nullptr;
    child.mapping = nullptr;
    child.parent = nullptr;
}

void CloseChild(ChildProcess& child) noexcept {
    CloseIdentity(child);
    if (child.process) CloseHandle(child.process);
    child.process = nullptr;
}

int WindowsError(DWORD error) {
    if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY) return GuestEnomem;
    if (error == ERROR_ACCESS_DENIED) return GuestEperm;
    if (error == ERROR_INVALID_PARAMETER) return GuestEinval;
    if (error == ERROR_CALL_NOT_IMPLEMENTED || error == ERROR_PROC_NOT_FOUND) return GuestEnosys;
    return GuestEagain;
}

int CloneError(std::uint32_t status) {
    if (status == 0xc0000017u || status == 0xc000009au) return GuestEnomem;
    if (status == 0xc0000022u) return GuestEperm;
    if (status == 0xc0000139u || status == 0xc0000002u) return GuestEnosys;
    return GuestEagain;
}

ChildProcess NewIdentity(Identity& parent) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(Identity), nullptr);
    if (mapping == nullptr) return {};
    auto* identity = static_cast<Identity*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Identity)));
    if (identity == nullptr) {
        const auto error = GetLastError();
        CloseHandle(mapping);
        SetLastError(error);
        return {};
    }
    identity->pid = 0;
    identity->parent = Read(parent.pid);
    identity->group = Read(parent.group);
    identity->session = Read(parent.session);
    identity->initialized = 0;
    HANDLE parentProcess = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parentProcess,
                         SYNCHRONIZE, TRUE, 0)) {
        const auto error = GetLastError();
        UnmapViewOfFile(identity);
        CloseHandle(mapping);
        SetLastError(error);
        return {};
    }
    return {nullptr, mapping, identity, parentProcess};
}

[[noreturn]] void StopChild() noexcept {
    TerminateProcess(GetCurrentProcess(), 127);
    for (;;) Sleep(INFINITE);
}

void DiscardClone(ChildProcess& child) noexcept {
    if (child.process) {
        TerminateProcess(child.process, 127);
        WaitForSingleObject(child.process, 1000);
    }
    CloseChild(child);
}

int ChildReady(const ChildProcess& child) {
    const auto deadline = GetTickCount64() + ChildReadyTimeout;
    for (;;) {
        const LONG state = Read(child.identity->initialized);
        if (state > 0) return 0;
        if (state < 0) return -state;
        const DWORD waited = WaitForSingleObject(child.process, 10);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED || GetTickCount64() >= deadline) {
            // A successfully initialized child may already have exited between
            // the first state read and the process wait.
            const LONG finalState = Read(child.identity->initialized);
            return finalState > 0 ? 0 : finalState < 0 ? -finalState : GuestEagain;
        }
    }
}

bool Selected(int requested, int childPid, Identity& identity, LONG group) {
    if (requested > 0) return requested == childPid;
    if (requested == -1) return true;
    const auto expected = requested == 0 ? static_cast<std::int64_t>(group) : -static_cast<std::int64_t>(requested);
    return Read(identity.group) == expected;
}

int ExitStatus(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_STACK_OVERFLOW: return 11;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION: return 4;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return 8;
        case 0xc000013au: return 2;
        default: return static_cast<int>((code & 0xffu) << 8u);
    }
}

#else

int PosixError(int error) {
    if (error == EAGAIN) return GuestEagain;
    if (error == ENOSYS) return GuestEnosys;
    if (error == EOVERFLOW) return GuestEoverflow;
    return error;
}

ssize_t SendHandshake(int descriptor, unsigned char state) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
    const int enabled = 1;
    if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) return -1;
#endif
    ssize_t sent;
    do { sent = ::send(descriptor, &state, sizeof(state), flags); } while (sent < 0 && errno == EINTR);
    return sent;
}

#endif

}

extern "C" int GuestRegisterForkHooks_nid_no_patch(const GuestForkHooks* hooks) {
    if (!hooks || (!hooks->prepare && !hooks->parent && !hooks->child)) return GuestEinval;
    const ProcessLock lock;
    for (const auto& entry : ForkHooks()) {
        if (entry.context == hooks->context && entry.prepare == hooks->prepare &&
            entry.parent == hooks->parent && entry.child == hooks->child) return 0;
    }
    try {
        const auto at = std::upper_bound(ForkHooks().begin(), ForkHooks().end(), hooks->priority,
            [](unsigned priority, const GuestForkHooks& entry) { return priority < entry.priority; });
        ForkHooks().insert(at, *hooks);
        return 0;
    } catch (const std::bad_alloc&) {
        return GuestEnomem;
    }
}

extern "C" int GuestFork_nid_no_patch() {
    if (const int error = RegisterHostDescriptorFork_nid_no_patch()) return Fail(error);
    ProcessLock lock;
    std::size_t prepared = 0;
#ifdef _WIN32
    auto pending = NewIdentity(CurrentIdentity());
    if (!pending.identity) return Fail(WindowsError(GetLastError()));
    try {
        Children().emplace(0, pending);
        if (!PrepareHooks(prepared)) {
            const int error = errno > 0 ? errno : GuestEagain;
            const bool restored = ParentHooks(prepared, false);
            Children().erase(0);
            CloseChild(pending);
            return Fail(restored ? error : GuestEio);
        }
    } catch (const std::bad_alloc&) {
        const bool restored = ParentHooks(prepared, false);
        Children().erase(0);
        CloseChild(pending);
        return Fail(restored ? GuestEnomem : GuestEio);
    } catch (...) {
        ParentHooks(prepared, false);
        Children().erase(0);
        CloseChild(pending);
        throw;
    }
    const auto result = HostProcess::Clone_nid_no_patch();
    if (result.branch == HostProcess::Branch::Failed) {
        const bool restored = ParentHooks(prepared, false);
        Children().erase(0);
        CloseChild(pending);
        return Fail(restored ? CloneError(result.nativeStatus) : GuestEio);
    }
    if (result.branch == HostProcess::Branch::Child) {
        if (result.childInitializationStatus != 0) {
            InterlockedExchange(&pending.identity->initialized, -CloneError(result.childInitializationStatus));
            StopChild();
        }
        lock.Child();
        for (auto& [pid, child] : Children()) {
            if (pid != 0) CloseIdentity(child);
        }
        Children().clear();
        if (currentMapping) {
            UnmapViewOfFile(currentIdentity);
            CloseHandle(currentMapping);
        }
        currentIdentity = pending.identity;
        currentMapping = pending.mapping;
        if (currentParent) CloseHandle(currentParent);
        currentParent = pending.parent;
        InterlockedExchange(&currentIdentity->pid, static_cast<LONG>(result.processId));
        if (const int error = ChildHooks(prepared)) {
            InterlockedExchange(&currentIdentity->initialized, -error);
            StopChild();
        }
        InterlockedExchange(&currentIdentity->initialized, 1);
        return 0;
    }
    pending.process = reinterpret_cast<HANDLE>(result.processHandle);
    CloseHandle(pending.parent);
    pending.parent = nullptr;
    if (!SetHandleInformation(pending.process, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(reinterpret_cast<HANDLE>(result.threadHandle), HANDLE_FLAG_INHERIT, 0)) {
        ParentHooks(prepared, true);
        HostProcess::Close_nid_no_patch(result.threadHandle);
        Children().erase(0);
        DiscardClone(pending);
        return Fail(GuestEio);
    }
    if (result.processId > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) || Children().contains(static_cast<int>(result.processId))) {
        const bool restored = ParentHooks(prepared, true);
        HostProcess::Close_nid_no_patch(result.threadHandle);
        Children().erase(0);
        DiscardClone(pending);
        return Fail(restored ? GuestEoverflow : GuestEio);
    }
    const int pid = static_cast<int>(result.processId);
    InterlockedExchange(&pending.identity->pid, static_cast<LONG>(pid));
    auto entry = Children().extract(0);
    entry.key() = pid;
    entry.mapped() = pending;
    Children().insert(std::move(entry));
    if (!ParentHooks(prepared, true)) {
        HostProcess::Close_nid_no_patch(result.threadHandle);
        Children().erase(pid);
        DiscardClone(pending);
        return Fail(GuestEio);
    }
    const int resumed = HostProcess::Resume_nid_no_patch(result.threadHandle);
    const DWORD resumeError = resumed == 0 ? ERROR_SUCCESS : GetLastError();
    HostProcess::Close_nid_no_patch(result.threadHandle);
    const int error = resumed == 0 ? ChildReady(pending) : WindowsError(resumeError);
    if (error != 0) {
        Children().erase(pid);
        DiscardClone(pending);
        return Fail(error);
    }
    return pid;
#else
    int ready[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ready) != 0) return Fail(PosixError(errno));
    int release[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, release) != 0) {
        const int error = PosixError(errno);
        ::close(ready[0]);
        ::close(ready[1]);
        return Fail(error);
    }
    try {
        if (!PrepareHooks(prepared)) {
            const int error = errno > 0 ? errno : GuestEagain;
            const bool restored = ParentHooks(prepared, false);
            ::close(ready[0]);
            ::close(ready[1]);
            ::close(release[0]);
            ::close(release[1]);
            return Fail(restored ? error : GuestEio);
        }
    } catch (...) {
        ParentHooks(prepared, false);
        ::close(ready[0]);
        ::close(ready[1]);
        ::close(release[0]);
        ::close(release[1]);
        throw;
    }
    const auto result = HostProcess::Clone_nid_no_patch();
    if (result.branch == HostProcess::Branch::Failed) {
        const bool restored = ParentHooks(prepared, false);
        ::close(ready[0]);
        ::close(ready[1]);
        ::close(release[0]);
        ::close(release[1]);
        return Fail(restored ? PosixError(static_cast<int>(result.nativeStatus)) : GuestEio);
    }
    if (result.branch == HostProcess::Branch::Child) {
        lock.Child();
        ::close(ready[0]);
        ::close(release[1]);
        unsigned char permission = 0;
        ssize_t acknowledged;
        do { acknowledged = ::read(release[0], &permission, sizeof(permission)); } while (acknowledged < 0 && errno == EINTR);
        ::close(release[0]);
        if (acknowledged != 1 || permission != 1) ::_exit(127);
        const int error = ChildHooks(prepared);
        const unsigned char state = static_cast<unsigned char>(error);
        const auto sent = SendHandshake(ready[1], state);
        ::close(ready[1]);
        if (error != 0 || sent != 1) ::_exit(127);
        return 0;
    }
    ::close(ready[1]);
    ::close(release[0]);
    const bool restored = ParentHooks(prepared, true);
    const unsigned char permission = restored ? 1 : 0;
    const auto sent = SendHandshake(release[1], permission);
    ::close(release[1]);
    if (!restored || sent != 1) {
        ::close(ready[0]);
        ::kill(static_cast<pid_t>(result.processId), SIGKILL);
        while (::waitpid(static_cast<pid_t>(result.processId), nullptr, 0) < 0 && errno == EINTR) {}
        return Fail(GuestEio);
    }
    pollfd channel{ready[0], POLLIN, 0};
    int polled;
    do { polled = ::poll(&channel, 1, ChildReadyTimeout); } while (polled < 0 && errno == EINTR);
    unsigned char state = 0;
    ssize_t received = -1;
    if (polled > 0) {
        do { received = ::read(ready[0], &state, sizeof(state)); } while (received < 0 && errno == EINTR);
    }
    ::close(ready[0]);
    if (received != 1 || state != 0) {
        ::kill(static_cast<pid_t>(result.processId), SIGKILL);
        while (::waitpid(static_cast<pid_t>(result.processId), nullptr, 0) < 0 && errno == EINTR) {}
        return Fail(received == 1 && state > 0 ? state : GuestEagain);
    }
    return static_cast<int>(result.processId);
#endif
}

extern "C" int GuestWaitPid_nid_no_patch(int pid, int* status, int options) {
    if ((static_cast<unsigned>(options) & ~WaitOptions) != 0) return Fail(GuestEinval);
    if ((static_cast<unsigned>(options) & UnsupportedWaitOptions) != 0)
        throw std::runtime_error("waitpid: stopped, continued, traced and alternate-thread child events are not implemented");
#ifdef _WIN32
    for (;;) {
        bool selected = false;
        {
            const ProcessLock lock;
            const LONG group = Read(CurrentIdentity().group);
            for (auto found = Children().begin(); found != Children().end(); ++found) {
                const auto childPid = found->first;
                auto& child = found->second;
                if (childPid == 0 || !Selected(pid, childPid, *child.identity, group)) continue;
                selected = true;
                const DWORD result = WaitForSingleObject(child.process, 0);
                if (result == WAIT_TIMEOUT) continue;
                if (result == WAIT_FAILED) return Fail(GuestEio);
                DWORD code = 0;
                if (!GetExitCodeProcess(child.process, &code)) return Fail(GuestEio);
                if (status) *status = ExitStatus(code);
                if ((options & WaitNoReap) == 0) {
                    CloseChild(child);
                    Children().erase(found);
                }
                return childPid;
            }
        }
        if (!selected) return Fail(GuestEchild);
        if ((options & WaitNoHang) != 0) return 0;
        Sleep(1);
    }
#else
    if ((options & WaitNoReap) != 0) throw std::runtime_error("waitpid: WNOWAIT is not implemented on this host");
    const auto result = ::waitpid(pid, status, (options & WaitNoHang) != 0 ? WNOHANG : 0);
    return result < 0 ? Fail(PosixError(errno)) : static_cast<int>(result);
#endif
}

extern "C" int GuestSetSid_nid_no_patch() {
#ifdef _WIN32
    const ProcessLock lock;
    auto& identity = CurrentIdentity();
    const LONG self = Read(identity.pid);
    if (Read(identity.group) == self) return Fail(GuestEperm);
    InterlockedExchange(&identity.group, self);
    InterlockedExchange(&identity.session, self);
    return self;
#else
    const auto result = ::setsid();
    return result < 0 ? Fail(PosixError(errno)) : static_cast<int>(result);
#endif
}

extern "C" int GuestGetSid_nid_no_patch(int pid) {
#ifdef _WIN32
    const ProcessLock lock;
    auto& identity = CurrentIdentity();
    if (pid == 0 || pid == Read(identity.pid)) return Read(identity.session);
    const auto found = Children().find(pid);
    if (found == Children().end()) return Fail(GuestEsrch);
    return Read(found->second.identity->session);
#else
    const auto result = ::getsid(pid);
    return result < 0 ? Fail(PosixError(errno)) : static_cast<int>(result);
#endif
}

extern "C" int GuestGetProcessGroup_nid_no_patch(int pid) {
#ifdef _WIN32
    const ProcessLock lock;
    auto& identity = CurrentIdentity();
    if (pid == 0 || pid == Read(identity.pid)) return Read(identity.group);
    const auto found = Children().find(pid);
    if (found == Children().end()) return Fail(GuestEsrch);
    return Read(found->second.identity->group);
#else
    const auto result = ::getpgid(pid);
    return result < 0 ? Fail(PosixError(errno)) : static_cast<int>(result);
#endif
}

extern "C" int GuestGetParentPid_nid_no_patch() {
#ifdef _WIN32
    const ProcessLock lock;
    auto& identity = CurrentIdentity();
    if (currentParent && WaitForSingleObject(currentParent, 0) == WAIT_OBJECT_0)
        InterlockedExchange(&identity.parent, 1);
    return Read(identity.parent);
#else
    return static_cast<int>(::getppid());
#endif
}
