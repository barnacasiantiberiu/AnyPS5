#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/GuestProcess.hpp"
#include "prx/libc/include/DaemonRuntime.hpp"
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" {
int APS5_VABI daemon_nid_postfix(int, int);
int APS5_VABI getpid_nid_postfix();
int APS5_VABI getsid_nid_postfix(int);
int APS5_VABI chdir_nid_postfix(const char*);
char* APS5_VABI getcwd_nid_postfix(char*, std::size_t);
int APS5_VABI open_nid_postfix(const char*, int, int);
int APS5_VABI dup2_nid_postfix(int, int);
int APS5_VABI close_nid_postfix(int);
std::int64_t APS5_VABI read_nid_postfix(int, void*, std::uint64_t);
std::int64_t APS5_VABI write_nid_postfix(int, const char*, std::int64_t);
int APS5_VABI atexit_nid_postfix(void (APS5_VABI*)());
}

namespace {

constexpr char Input[] = "inherited daemon input";
constexpr char Output[] = "daemon stdout marker";
constexpr char ErrorOutput[] = "daemon stderr marker";
constexpr DWORD CaseTimeout = 10000;

struct Report {
    std::atomic<unsigned> completed{0};
    std::atomic<unsigned> entries{0};
    std::atomic<unsigned> hostCallbacks{0};
    std::atomic<unsigned> guestCallbacks{0};
    unsigned supervisorPid = 0;
    unsigned parentPid = 0;
    unsigned childPid = 0;
    std::uintptr_t childProcess = 0;
    unsigned stage = 0;
    int result = -1;
    int error = 0;
    std::uintptr_t stackBefore = 0;
    std::uintptr_t stackAfter = 0;
    std::uintptr_t heapBefore = 0;
    std::uintptr_t heapAfter = 0;
    char message[256]{};
};

static_assert(std::atomic<unsigned>::is_always_lock_free);
Report* activeReport = nullptr;

struct FailureState {
    int error = 0;
    bool signalFailure = false;
    bool ignoredValid = false;
    int signalCalls = 0;
    int parentCalls = 0;
    int descriptorCalls = 0;
};

FailureState failureState;

int APS5_VABI FailureSignal_nid_no_patch(int signal, const GuestSigaction* action, GuestSigaction* previous) {
    ++failureState.signalCalls;
    failureState.ignoredValid = signal == 1 && action && action->handler == 1 && action->flags == 0 &&
        action->mask.bits[0] == 0 && action->mask.bits[1] == 0 && action->mask.bits[2] == 0 && action->mask.bits[3] == 0;
    if (previous) *previous = {0x12345678u, 2, {{1, 2, 3, 4}}};
    if (failureState.signalFailure) { errno = 22; return -1; }
    return 0;
}

int FailureOpen() { ++failureState.descriptorCalls; return -1; }
int APS5_VABI FailureDuplicate_nid_no_patch(int, int) { ++failureState.descriptorCalls; return -1; }
int APS5_VABI FailureClose_nid_no_patch(int) { ++failureState.descriptorCalls; return -1; }
bool RejectFork(void*) { errno = failureState.error; return false; }
bool RestoreRejectedFork(void*, bool created) noexcept { ++failureState.parentCalls; return !created; }

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value(value) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return value; }
private:
    HANDLE value;
};

class MappingView {
public:
    explicit MappingView(HANDLE mapping) : value(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Report))) {
        if (!value) throw std::runtime_error("MapViewOfFile failed");
    }
    ~MappingView() { UnmapViewOfFile(value); }
    Report* Get() const { return static_cast<Report*>(value); }
private:
    void* value;
};

class JobCleanup {
public:
    explicit JobCleanup(HANDLE value) : value(value) {}
    ~JobCleanup() {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (QueryInformationJobObject(value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) &&
            accounting.ActiveProcesses == 0) return;
        TerminateJobObject(value, 72);
        const auto deadline = GetTickCount64() + 1000;
        while (GetTickCount64() < deadline) {
            if (QueryInformationJobObject(value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) &&
                accounting.ActiveProcesses == 0) return;
            Sleep(10);
        }
    }
private:
    HANDLE value;
};

class Directory {
public:
    Directory() : name("anyps5-daemon-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())),
        path(std::filesystem::current_path() / name) {
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("creating daemon test directory failed");
        std::ofstream(path / "stdin.bin", std::ios::binary) << Input;
        std::ofstream(path / "stdout.bin", std::ios::binary);
        std::ofstream(path / "stderr.bin", std::ios::binary);
    }
    ~Directory() {
        std::error_code error;
        for (const auto* name : {"stdin.bin", "stdout.bin", "stderr.bin"}) std::filesystem::remove(path / name, error);
        std::filesystem::remove(path, error);
    }
    const std::string& Name() const { return name; }
    std::string Read(const char* filename) const {
        std::ifstream stream(path / filename, std::ios::binary);
        if (!stream) throw std::runtime_error("reading daemon test stream failed");
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }
private:
    std::string name;
    std::filesystem::path path;
};

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void RunFailures() {
    const DaemonRuntime runtime{FailureSignal_nid_no_patch, FailureOpen, FailureDuplicate_nid_no_patch, FailureClose_nid_no_patch};
    LibcRegisterDaemonRuntime_nid_no_patch(&runtime);
    const GuestForkHooks rejection{0, RejectFork, RestoreRejectedFork, nullptr, nullptr};
    Check(GuestRegisterForkHooks_nid_no_patch(&rejection) == 0, "registering fork rejection failed");
    for (const bool signalFailure : {false, true}) {
        for (const int error : {1, 5, 12, 35}) {
            failureState = {error, signalFailure};
            std::array<char, 1024> before{}, after{};
            Check(getcwd_nid_postfix(before.data(), before.size()) == before.data(), "reading cwd before failure failed");
            const int result = daemon_nid_postfix(0, 0);
            const int resultError = errno;
            Check(result == -1 && resultError == error, "daemon fork failure lost its result or errno");
            Check(GetCurrentProcessId() == activeReport->parentPid, "failed daemon continued in a different process");
            Check(failureState.signalCalls == 1 && failureState.ignoredValid,
                "daemon fork failure did not retain the FreeBSD SIGHUP behavior");
            Check(failureState.parentCalls == 1, "daemon fork failure did not release prepared owners");
            Check(failureState.descriptorCalls == 0, "failed daemon modified standard descriptors");
            Check(getcwd_nid_postfix(after.data(), after.size()) == after.data() && std::strcmp(before.data(), after.data()) == 0,
                "failed daemon modified the guest cwd");
        }
    }
}

void HostExit() {
    if (activeReport && activeReport->parentPid == GetCurrentProcessId()) ++activeReport->hostCallbacks;
}

void APS5_VABI GuestExit_nid_no_patch() {
    if (activeReport && activeReport->parentPid == GetCurrentProcessId()) ++activeReport->guestCallbacks;
}

bool ContainChild(void* context) noexcept {
    const auto job = static_cast<HANDLE>(context);
    activeReport->childPid = GetCurrentProcessId();
    const Handle supervisor(OpenProcess(PROCESS_DUP_HANDLE, FALSE, activeReport->supervisorPid));
    HANDLE childProcess = nullptr;
    if (!supervisor.Get() || !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), supervisor.Get(), &childProcess,
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0)) return false;
    activeReport->childProcess = reinterpret_cast<std::uintptr_t>(childProcess);
    BOOL contained = FALSE;
    const bool success = IsProcessInJob(GetCurrentProcess(), job, &contained) &&
        (contained || AssignProcessToJobObject(job, GetCurrentProcess()));
    CloseHandle(job);
    return success;
}

[[noreturn]] void Stop(unsigned code) {
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) Sleep(INFINITE);
}

void Redirect(const char* path, int flags, int target) {
    const int descriptor = open_nid_postfix(path, flags, 0);
    Check(descriptor >= 0, "opening a guest standard stream failed");
    Check(dup2_nid_postfix(descriptor, target) == target, "assigning a guest standard stream failed");
    if (descriptor != target) Check(close_nid_postfix(descriptor) == 0, "closing source descriptor failed");
}

[[noreturn]] void Worker(int noChdir, int noClose, HANDLE mapping, HANDLE job, const char* directory, bool failures) {
    const MappingView view(mapping);
    activeReport = view.Get();
    ++activeReport->entries;
    activeReport->parentPid = GetCurrentProcessId();
    try {
        activeReport->stage = 1;
        Check(std::atexit(HostExit) == 0, "registering host atexit failed");
        Check(atexit_nid_postfix(GuestExit_nid_no_patch) == 0, "registering guest atexit failed");
        const GuestForkHooks containment{~0u, nullptr, nullptr, ContainChild, job};
        Check(GuestRegisterForkHooks_nid_no_patch(&containment) == 0, "registering child containment failed");
        if (failures) {
            RunFailures();
            activeReport->stage = 5;
            activeReport->result = 0;
            activeReport->completed.store(1, std::memory_order_release);
            Stop(0);
        }
        Check(chdir_nid_postfix(directory) == 0, "setting guest working directory failed");
        std::array<char, 1024> before{};
        Check(getcwd_nid_postfix(before.data(), before.size()) == before.data(), "reading initial guest cwd failed");
        Redirect("stdin.bin", 0, 0);
        Redirect("stdout.bin", 1, 1);
        Redirect("stderr.bin", 1, 2);
        activeReport->stage = 2;
        volatile std::uint64_t stack[32]{};
        const std::unique_ptr<std::uint64_t[]> heap(new std::uint64_t[32]);
        const std::uint64_t seed = GetTickCount64() ^ (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32);
        for (unsigned index = 0; index != 32; ++index) {
            stack[index] = seed ^ (0x9e3779b97f4a7c15ULL * (index + 1));
            heap[index] = ~stack[index];
        }
        activeReport->stackBefore = reinterpret_cast<std::uintptr_t>(&stack[0]);
        activeReport->heapBefore = reinterpret_cast<std::uintptr_t>(heap.get());
        const unsigned parentPid = GetCurrentProcessId();
        const int result = daemon_nid_postfix(noChdir, noClose);
        activeReport->stage = 3;
        activeReport->childPid = GetCurrentProcessId();
        activeReport->stackAfter = reinterpret_cast<std::uintptr_t>(&stack[0]);
        activeReport->heapAfter = reinterpret_cast<std::uintptr_t>(heap.get());
        Check(result == 0, "daemon did not return success in its child");
        Check(GetCurrentProcessId() != parentPid, "daemon returned in the original parent");
        Check(getpid_nid_postfix() == static_cast<int>(GetCurrentProcessId()), "guest pid differs from the native child pid");
        Check(getsid_nid_postfix(0) == getpid_nid_postfix(), "daemon child is not its session leader");
        for (unsigned index = 0; index != 32; ++index) {
            Check(stack[index] == (seed ^ (0x9e3779b97f4a7c15ULL * (index + 1))), "daemon lost stack values");
            Check(heap[index] == ~stack[index], "daemon lost heap values");
            heap[index] ^= 0xa55aa55aa55aa55aULL;
            stack[index] = ~heap[index];
            Check(stack[index] == ~heap[index], "daemon child cannot update cloned memory");
        }
        activeReport->stage = 4;
        std::array<char, 1024> after{};
        Check(getcwd_nid_postfix(after.data(), after.size()) == after.data(), "reading child guest cwd failed");
        Check(std::strcmp(after.data(), noChdir ? before.data() : "/") == 0, "daemon cwd flag was not respected");
        char input[sizeof(Input)]{};
        const auto received = read_nid_postfix(0, input, sizeof(input));
        if (noClose) {
            Check(received == sizeof(Input) - 1 && std::memcmp(input, Input, sizeof(Input) - 1) == 0,
                "daemon did not preserve stdin");
        } else {
            Check(received == 0, "daemon stdin is not an EOF-producing null stream");
            Check(write_nid_postfix(0, Input, sizeof(Input) - 1) == sizeof(Input) - 1, "daemon stdin is not writable null");
            Check(read_nid_postfix(1, input, sizeof(input)) == 0, "daemon stdout is not readable null");
            Check(read_nid_postfix(2, input, sizeof(input)) == 0, "daemon stderr is not readable null");
        }
        Check(write_nid_postfix(1, Output, sizeof(Output) - 1) == sizeof(Output) - 1, "daemon stdout write failed");
        Check(write_nid_postfix(2, ErrorOutput, sizeof(ErrorOutput) - 1) == sizeof(ErrorOutput) - 1, "daemon stderr write failed");
        activeReport->stage = 5;
        activeReport->result = 0;
    } catch (const std::exception& error) {
        activeReport->error = errno;
        std::snprintf(activeReport->message, sizeof(activeReport->message), "%s", error.what());
    } catch (...) {
        std::strcpy(activeReport->message, "unexpected non-standard exception");
    }
    activeReport->completed.store(1, std::memory_order_release);
    Stop(activeReport->result == 0 ? 0 : 70);
}

void Run(int noChdir, int noClose, bool failures = false) {
    const Directory directory;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    const Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(Report), nullptr));
    Check(mapping.Get() != nullptr, "creating report mapping failed");
    const MappingView view(mapping.Get());
    auto& report = *new (view.Get()) Report;
    report.supervisorPid = GetCurrentProcessId();
    const Handle job(CreateJobObjectW(&security, nullptr));
    Check(job.Get() != nullptr, "creating containment job failed");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Check(SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != 0,
        "setting job containment failed");
    const JobCleanup cleanup(job.Get());
    std::array<wchar_t, 32768> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    Check(length != 0 && length < executable.size(), "finding daemon test executable failed");
    std::wstring command = L"\"" + std::wstring(executable.data()) + (failures ? L"\" --failure-worker " : L"\" --worker ") + std::to_wstring(noChdir) + L" " +
        std::to_wstring(noClose) + L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(mapping.Get())) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(job.Get())) + L" " +
        std::wstring(directory.Name().begin(), directory.Name().end());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Check(CreateProcessW(executable.data(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr, nullptr, &startup, &process) != 0, "starting hidden daemon test worker failed");
    const Handle worker(process.hProcess);
    const Handle thread(process.hThread);
    if (!AssignProcessToJobObject(job.Get(), worker.Get())) {
        TerminateProcess(worker.Get(), 71);
        WaitForSingleObject(worker.Get(), 1000);
        throw std::runtime_error("assigning worker to containment job failed");
    }
    Check(ResumeThread(thread.Get()) != static_cast<DWORD>(-1), "resuming daemon test worker failed");
    const auto deadline = GetTickCount64() + CaseTimeout;
    bool finished = false;
    while (GetTickCount64() < deadline) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        Check(QueryInformationJobObject(job.Get(), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) != 0,
            "querying daemon containment job failed");
        if (accounting.ActiveProcesses == 0) { finished = true; break; }
        Sleep(10);
    }
    const bool reported = report.completed.load(std::memory_order_acquire) == 1;
    const Handle childProcess(reinterpret_cast<HANDLE>(report.childProcess));
    DWORD childCode = 0;
    if (childProcess.Get()) GetExitCodeProcess(childProcess.Get(), &childCode);
    if (!finished || !reported) {
        DWORD code = 0;
        GetExitCodeProcess(worker.Get(), &code);
        std::fprintf(stderr, "daemon(%d,%d): finished=%u, stage=%u, parent=%u, child=%u, worker exit=0x%lx, child exit=0x%lx\n",
            noChdir, noClose, static_cast<unsigned>(finished), report.stage, report.parentPid, report.childPid,
            static_cast<unsigned long>(code), static_cast<unsigned long>(childCode));
        std::fprintf(stderr, "child stdout: %s\nchild stderr: %s\n", directory.Read("stdout.bin").c_str(), directory.Read("stderr.bin").c_str());
    }
    Check(finished, "daemon subprocesses did not finish within ten seconds");
    Check(reported, "daemon child exited without reporting continuation");
    if (report.result != 0) {
        std::fprintf(stderr, "daemon(%d,%d) stage %u: %s (errno=%d, parent=%u, child=%u)\n",
            noChdir, noClose, report.stage, report.message, report.error, report.parentPid, report.childPid);
        throw std::runtime_error("daemon child functional checks failed");
    }
    DWORD code = 0;
    Check(GetExitCodeProcess(worker.Get(), &code) && code == 0, "daemon parent did not exit with status zero");
    Check(report.entries.load() == 1, "daemon restarted the worker instead of cloning its continuation");
    if (failures) {
        Check(report.parentPid == process.dwProcessId && report.childPid == 0 && !childProcess.Get(),
            "failed daemon created a child");
        std::puts("daemon fork failures: 8 errno and SIGHUP cases passed");
        return;
    }
    Check(childProcess.Get() && childCode == 0, "daemon child did not exit with status zero");
    Check(report.parentPid == process.dwProcessId && report.childPid != report.parentPid && report.childPid != 0,
        "daemon did not create a different child process");
    Check(report.hostCallbacks.load() == 0 && report.guestCallbacks.load() == 0, "daemon parent ran atexit callbacks");
    Check(report.stackBefore == report.stackAfter && report.heapBefore == report.heapAfter,
        "daemon did not preserve stack and heap addresses");
    Check(directory.Read("stdin.bin") == Input, "daemon changed the original stdin file");
    const auto output = directory.Read("stdout.bin");
    const auto errorOutput = directory.Read("stderr.bin");
    Check((output.find(Output) != std::string::npos) == (noClose != 0), "daemon stdout preservation/null flag was not respected");
    Check((errorOutput.find(ErrorOutput) != std::string::npos) == (noClose != 0), "daemon stderr preservation/null flag was not respected");
    if (noClose) {
        Check(output.find(Output) == output.rfind(Output), "daemon wrote stdout more than once");
        Check(errorOutput.find(ErrorOutput) == errorOutput.rfind(ErrorOutput), "daemon wrote stderr more than once");
    }
    std::printf("daemon(%d,%d): parent %u exited 0; child %u continued stack/heap, session, cwd and stdio checks passed\n",
        noChdir, noClose, report.parentPid, report.childPid);
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 7 && (std::strcmp(argv[1], "--worker") == 0 || std::strcmp(argv[1], "--failure-worker") == 0)) {
            Worker(std::atoi(argv[2]), std::atoi(argv[3]), reinterpret_cast<HANDLE>(std::strtoull(argv[4], nullptr, 10)),
                reinterpret_cast<HANDLE>(std::strtoull(argv[5], nullptr, 10)), argv[6], std::strcmp(argv[1], "--failure-worker") == 0);
        }
        if (argc == 2 && std::strcmp(argv[1], "failures") == 0) { Run(0, 0, true); return 0; }
        Check(argc == 3 && (std::strcmp(argv[1], "0") == 0 || std::strcmp(argv[1], "1") == 0) &&
            (std::strcmp(argv[2], "0") == 0 || std::strcmp(argv[2], "1") == 0), "usage: guest_daemon_functional_tests 0|1 0|1");
        Run(std::atoi(argv[1]), std::atoi(argv[2]));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
