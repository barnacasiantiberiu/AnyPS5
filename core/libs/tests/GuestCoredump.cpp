#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ModuleChecks.hpp"
#include "prx/libkernel/System/include/Coredump.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestProcess.hpp"
#include "prx/libc/include/DaemonRuntime.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

extern "C" {
int APS5_VABI sceCoredumpRegisterCoredumpHandler(GuestCoredumpHandler, std::size_t, void*);
int APS5_VABI sceCoredumpUnregisterCoredumpHandler();
int APS5_VABI sceCoredumpAttachUserFile(std::uint32_t, const char*);
int APS5_VABI sceCoredumpAttachMemoryRegionAsUserFile(std::uint32_t, const void*, std::size_t, const char*);
int APS5_VABI sceCoredumpAttachMemoryRegion(std::uint32_t, const void*, std::size_t);
int APS5_VABI sceCoredumpAttachUserMemoryFile(std::uint32_t, const void*, std::size_t);
std::intptr_t APS5_VABI sceCoredumpWriteUserData(const void*, std::size_t);
void APS5_VABI sceCoredumpDebugTextOut(const char*, int);
int APS5_VABI sceCoredumpGetStopInfoCpu(GuestCoredumpStopInfoCpu*, std::size_t);
int APS5_VABI sceCoredumpGetThreadContextInfo(GuestCoredumpThreadContextInfo*, std::size_t);
Pthread APS5_VABI scePthreadSelf();
int APS5_VABI scePthreadAttrInit(PthreadAttr*);
int APS5_VABI scePthreadAttrDestroy(PthreadAttr*);
int APS5_VABI scePthreadAttrGet(Pthread, PthreadAttr*);
int APS5_VABI scePthreadAttrGetstack(const PthreadAttr*, void**, std::size_t*);
int APS5_VABI sigaction_nid_postfix(int, const GuestSigaction*, GuestSigaction*);
int APS5_VABI sigprocmask_nid_postfix(int, const void*, void*);
int APS5_VABI raise_nid_postfix(int);
}

using ModuleTest::Require;
using ModuleTest::RequireThrows;

namespace {

constexpr int InvalidParameter = static_cast<int>(0x81180000u);
constexpr int NotRegistered = static_cast<int>(0x81180001u);
constexpr int AlreadyRegistered = static_cast<int>(0x81180002u);
constexpr int NotInHandler = static_cast<int>(0x81180003u);

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("anyps5-coredump-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() { Require(std::filesystem::create_directory(path)); }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

std::string Read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    Require(stream.is_open());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

struct Case {
    GuestCoredumpStopInfoCpu stop{};
    GuestCoredumpThreadContextInfo context{};
    std::thread::id caller = std::this_thread::get_id();
    Pthread callerGuest = scePthreadSelf();
    std::string directory;
    int calls = 0;
    bool fail = false;
};

int APS5_VABI Handler(void* argument) {
    auto& test = *static_cast<Case*>(argument);
    ++test.calls;
    Require(std::this_thread::get_id() != test.caller);
    Require(scePthreadSelf() != test.callerGuest);
    if (test.fail) throw std::runtime_error("deliberate coredump callback failure");
    PthreadAttr attr = nullptr;
    Require(scePthreadAttrInit(&attr) == 0);
    Require(scePthreadAttrGet(scePthreadSelf(), &attr) == 0);
    void* stack = nullptr;
    std::size_t stackSize = 0;
    Require(scePthreadAttrGetstack(&attr, &stack, &stackSize) == 0);
    Require(stackSize == 65536);
    Require(scePthreadAttrDestroy(&attr) == 0);
    Require(sceCoredumpRegisterCoredumpHandler(Handler, 65536, argument) == AlreadyRegistered);
    RequireThrows([] { sceCoredumpUnregisterCoredumpHandler(); }, "during capture");
    RequireThrows([&] { GuestCoredumpCapture_nid_no_patch(&test.stop, &test.context, test.directory.c_str()); }, "recursive");

    struct CpuOutput { GuestCoredumpStopInfoCpu info; std::array<unsigned char, 16> guard; } cpu;
    std::memset(&cpu, 0xa5, sizeof(cpu));
    Require(sceCoredumpGetStopInfoCpu(&cpu.info, sizeof(cpu.info) - 1) == InvalidParameter);
    for (auto byte : std::as_bytes(std::span(&cpu, 1))) Require(byte == std::byte{0xa5});
    Require(sceCoredumpGetStopInfoCpu(nullptr, sizeof(cpu.info)) == InvalidParameter);
    Require(sceCoredumpGetStopInfoCpu(&cpu.info, sizeof(cpu.info)) == 0);
    Require(std::memcmp(&cpu.info, &test.stop, sizeof(cpu.info)) == 0);
    for (const auto byte : cpu.guard) Require(byte == 0xa5);

    struct RegisterOutput { GuestCoredumpThreadContextInfo info; std::array<unsigned char, 16> guard; } registers;
    std::memset(&registers, 0x5a, sizeof(registers));
    Require(sceCoredumpGetThreadContextInfo(&registers.info, sizeof(registers.info) + 1) == InvalidParameter);
    for (auto byte : std::as_bytes(std::span(&registers, 1))) Require(byte == std::byte{0x5a});
    Require(sceCoredumpGetThreadContextInfo(&registers.info, sizeof(registers.info)) == 0);
    Require(std::memcmp(&registers.info, &test.context, sizeof(registers.info)) == 0);
    for (const auto byte : registers.guard) Require(byte == 0x5a);
    int otherResult = 0;
    std::thread other([&] { otherResult = sceCoredumpGetStopInfoCpu(&cpu.info, sizeof(cpu.info)); });
    other.join();
    Require(otherResult == NotInHandler);

    Require(sceCoredumpWriteUserData(nullptr, 0) == 0);
    Require(sceCoredumpWriteUserData(nullptr, 4) == InvalidParameter);
    Require(sceCoredumpWriteUserData("x", std::numeric_limits<std::size_t>::max()) == InvalidParameter);
    Require(sceCoredumpWriteUserData("abc\0xyz", 7) == 7);
    Require(sceCoredumpWriteUserData("!", 1) == 1);
    sceCoredumpDebugTextOut("hello\0world ignored", 11);
    sceCoredumpDebugTextOut("\n", 1);
    RequireThrows([] { sceCoredumpDebugTextOut(nullptr, 1); }, "unreadable");
    RequireThrows([] { sceCoredumpDebugTextOut("x", -1); }, "negative");
    std::array<char, 4> memory{'a', 'b', '\0', 'd'};
    Require(sceCoredumpAttachMemoryRegionAsUserFile(31, memory.data(), memory.size(), "../guest-name.bin") == 0);
    memory[0] = 'X';
    Require(sceCoredumpAttachMemoryRegion(32, memory.data(), memory.size()) == 0);
    memory[1] = 'Y';
    Require(sceCoredumpAttachUserMemoryFile(33, memory.data(), memory.size()) == 0);
    Require(sceCoredumpAttachUserFile(34, "/coredump-test/source.bin") == 0);
    Require(sceCoredumpAttachMemoryRegion(2, nullptr, 4) == InvalidParameter);
    Require(sceCoredumpAttachMemoryRegionAsUserFile(2, memory.data(), 4, nullptr) == InvalidParameter);
    Require(sceCoredumpAttachUserFile(2, nullptr) == InvalidParameter);
    return 73;
}

void Outside() {
    GuestCoredumpStopInfoCpu info;
    std::memset(&info, 0xcc, sizeof(info));
    Require(sceCoredumpGetStopInfoCpu(&info, sizeof(info)) == NotInHandler);
    for (auto byte : std::as_bytes(std::span(&info, 1))) Require(byte == std::byte{0xcc});
    Require(sceCoredumpGetThreadContextInfo(nullptr, 0) == NotInHandler);
    Require(sceCoredumpAttachUserFile(1, nullptr) == NotInHandler);
    Require(sceCoredumpAttachMemoryRegionAsUserFile(1, nullptr, 0, nullptr) == NotInHandler);
    Require(sceCoredumpAttachMemoryRegion(1, nullptr, 0) == NotInHandler);
    Require(sceCoredumpAttachUserMemoryFile(1, nullptr, 0) == NotInHandler);
    Require(sceCoredumpWriteUserData(nullptr, 0) == NotInHandler);
    RequireThrows([] { sceCoredumpDebugTextOut("a", 1); }, "outside handler");
}

void Lifecycle(bool failure) {
    TemporaryDirectory directory;
    Case test;
    test.directory = (directory.path / "reports").string();
    test.stop = {test.callerGuest, 11, 0};
    auto* values = reinterpret_cast<std::uint64_t*>(&test.context);
    for (std::size_t i = 0; i < sizeof(test.context) / sizeof(*values); ++i) values[i] = 0x1234567800000000ull + i;
    {
        std::ofstream source(directory.path / "source.bin", std::ios::binary);
        source.write("source\0bytes", 12);
    }
    AddPathAlias_nid_no_patch("/coredump-test", directory.path.string().c_str());
    Require(sceCoredumpUnregisterCoredumpHandler() == NotRegistered);
    Require(GuestCoredumpCapture_nid_no_patch(&test.stop, &test.context, test.directory.c_str()) == NotRegistered);
    Require(sceCoredumpRegisterCoredumpHandler(nullptr, 65536, &test) == InvalidParameter);
    Require(sceCoredumpRegisterCoredumpHandler(Handler, 16383, &test) == InvalidParameter);
    Require(sceCoredumpRegisterCoredumpHandler(Handler, 0x20000000, &test) == InvalidParameter);
    Outside();
    Require(sceCoredumpRegisterCoredumpHandler(Handler, 65536, &test) == 0);
    const int child = GuestFork_nid_no_patch();
    const int forkError = errno;
    if (child == 0) std::_Exit(96);
    if (child > 0) {
        int status = 0;
        GuestWaitPid_nid_no_patch(child, &status, 0);
    }
    Require(child == -1 && forkError == 35);
    if (failure) {
        test.fail = true;
        RequireThrows([&] { GuestCoredumpCapture_nid_no_patch(&test.stop, &test.context, test.directory.c_str()); }, "deliberate");
        Require(!std::filesystem::exists(test.directory));
        test.fail = false;
    }
    Require(GuestCoredumpCapture_nid_no_patch(&test.stop, &test.context, test.directory.c_str()) == 0);
    Outside();
    Require(sceCoredumpUnregisterCoredumpHandler() == 0);
    Require(sceCoredumpUnregisterCoredumpHandler() == NotRegistered);
    std::vector<std::filesystem::path> reports;
    for (const auto& entry : std::filesystem::directory_iterator(test.directory)) reports.push_back(entry.path());
    Require(reports.size() == 1);
    const auto& report = reports.front();
    Require(Read(report / "cpu.bin") == std::string(reinterpret_cast<char*>(&test.stop), sizeof(test.stop)));
    Require(Read(report / "registers.bin") == std::string(reinterpret_cast<char*>(&test.context), sizeof(test.context)));
    Require(Read(report / "userdata.bin") == std::string("abc\0xyz!", 8));
    Require(Read(report / "debug.txt") == std::string("hello\0world\n", 12));
    Require(Read(report / "attachment-0.bin") == std::string("ab\0d", 4));
    Require(Read(report / "attachment-1.bin") == std::string("Xb\0d", 4));
    Require(Read(report / "attachment-2.bin") == std::string("XY\0d", 4));
    Require(Read(report / "attachment-3.bin") == std::string("source\0bytes", 12));
    const auto manifest = Read(report / "attachments.txt");
    Require(manifest.find("handler-result 73") != std::string::npos);
    Require(manifest.find("named-memory 31 4 \"../guest-name.bin\"") != std::string::npos);
    Require(manifest.find("file 34 12 \"/coredump-test/source.bin\"") != std::string::npos);
    Require(!std::filesystem::exists(std::filesystem::path(test.directory) / "guest-name.bin"));
    Require(test.calls == (failure ? 2 : 1));
    Require(sceCoredumpRegisterCoredumpHandler(Handler, 65536, &test) == 0);
    Require(sceCoredumpUnregisterCoredumpHandler() == 0);
    RemovePathAlias_nid_no_patch("/coredump-test");
}

constexpr char FatalMarker[] = "public fatal signal coredump\0saved";

struct FatalCase {
    Pthread caller = scePthreadSelf();
    std::uintptr_t stackLow = 0;
    std::uintptr_t stackHigh = 0;
};

int APS5_VABI FatalHandler_nid_no_patch(void* argument) {
    const auto& test = *static_cast<const FatalCase*>(argument);
    GuestCoredumpStopInfoCpu stop{};
    GuestCoredumpThreadContextInfo context{};
    Require(sceCoredumpGetStopInfoCpu(&stop, sizeof(stop)) == 0);
    Require(sceCoredumpGetThreadContextInfo(&context, sizeof(context)) == 0);
    Require(stop.reasonCode == 11 && stop.thread == test.caller && stop.reserved == 0);
    Require(scePthreadSelf() != test.caller);
    Require(context.rip != 0 && context.rsp >= test.stackLow && context.rsp < test.stackHigh);
    Require(sceCoredumpAttachMemoryRegion(71, &context, sizeof(context)) == 0);
    Require(sceCoredumpWriteUserData(FatalMarker, sizeof(FatalMarker)) == sizeof(FatalMarker));
    return 91;
}

[[noreturn]] void FatalChild() {
    FatalCase test;
#ifdef _WIN32
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    test.stackLow = low;
    test.stackHigh = high;
#elif defined(__APPLE__)
    const auto self = pthread_self();
    test.stackHigh = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(self));
    test.stackLow = test.stackHigh - pthread_get_stacksize_np(self);
#else
    pthread_attr_t attr;
    Require(pthread_getattr_np(pthread_self(), &attr) == 0);
    void* address = nullptr;
    std::size_t size = 0;
    Require(pthread_attr_getstack(&attr, &address, &size) == 0);
    Require(pthread_attr_destroy(&attr) == 0);
    test.stackLow = reinterpret_cast<std::uintptr_t>(address);
    test.stackHigh = test.stackLow + size;
#endif
    Require(test.stackLow != 0 && test.stackHigh > test.stackLow);
    Require(sceCoredumpRegisterCoredumpHandler(FatalHandler_nid_no_patch, 65536, &test) == 0);
    const GuestSigaction defaults{0, 1, {}};
    const GuestSignalSet unblocked{};
    Require(sigaction_nid_postfix(11, &defaults, nullptr) == 0);
    Require(sigprocmask_nid_postfix(3, &unblocked, nullptr) == 0);
    raise_nid_postfix(11);
    std::_Exit(95);
}

void FatalProcess(const std::filesystem::path& executable) {
    TemporaryDirectory directory;
#ifdef _WIN32
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    Require(job != nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    const bool limited = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    if (!limited) CloseHandle(job);
    Require(limited);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    std::wstring command = L"\"" + executable.wstring() + L"\" fatal-child";
    const bool created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, directory.path.c_str(), &startup, &child);
    if (!created) CloseHandle(job);
    Require(created);
    const bool assigned = AssignProcessToJobObject(job, child.hProcess);
    const bool resumed = assigned && ResumeThread(child.hThread) != static_cast<DWORD>(-1);
    const DWORD waited = resumed ? WaitForSingleObject(child.hProcess, 5000) : WAIT_FAILED;
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 96);
        WaitForSingleObject(child.hProcess, 5000);
    }
    DWORD exitCode = 0;
    const bool queried = GetExitCodeProcess(child.hProcess, &exitCode);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    CloseHandle(job);
    Require(assigned && resumed && waited == WAIT_OBJECT_0 && queried);
    Require(exitCode == 139);
#else
    const std::string executableName = executable.string();
    const std::string directoryName = directory.path.string();
    const pid_t child = ::fork();
    Require(child >= 0);
    if (child == 0) {
        if (::chdir(directoryName.c_str()) == 0)
            ::execl(executableName.c_str(), executableName.c_str(), "fatal-child", static_cast<char*>(nullptr));
        ::_exit(96);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int status = 0;
    pid_t waited = 0;
    while ((waited = ::waitpid(child, &status, WNOHANG)) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (waited != child) {
        ::kill(child, SIGKILL);
        ::waitpid(child, &status, 0);
    }
    Require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 139);
#endif
    const auto reports = directory.path / "coredumps";
    Require(std::filesystem::is_directory(reports));
    std::vector<std::filesystem::path> saved;
    for (const auto& entry : std::filesystem::directory_iterator(reports)) saved.push_back(entry.path());
    Require(saved.size() == 1);
    const auto& report = saved.front();
    Require(Read(report / "userdata.bin") == std::string(FatalMarker, sizeof(FatalMarker)));
    GuestCoredumpStopInfoCpu stop{};
    GuestCoredumpThreadContextInfo context{};
    const auto cpuBytes = Read(report / "cpu.bin");
    const auto registerBytes = Read(report / "registers.bin");
    Require(cpuBytes.size() == sizeof(stop) && registerBytes.size() == sizeof(context));
    std::memcpy(&stop, cpuBytes.data(), sizeof(stop));
    std::memcpy(&context, registerBytes.data(), sizeof(context));
    Require(stop.reasonCode == 11 && stop.thread != nullptr && stop.reserved == 0);
    Require(context.rip != 0 && context.rsp != 0);
    Require(Read(report / "attachment-0.bin") == registerBytes);
    Require(Read(report / "attachments.txt").find("handler-result 91") != std::string::npos);
}

}

int main(int argc, char** argv) {
    Require(argc == 2);
    const std::string mode = argv[1];
    if (mode == "fatal-child") FatalChild();
    if (mode == "fatal") {
        FatalProcess(std::filesystem::absolute(argv[0]));
        return 0;
    }
    Require(mode == "capture" || mode == "failure");
    Lifecycle(mode == "failure");
}
