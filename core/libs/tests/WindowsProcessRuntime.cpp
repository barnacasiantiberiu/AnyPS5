#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "ModuleChecks.hpp"
#include "prx/libc/include/HostProcess.hpp"
#include "prx/libc/include/WindowsProcessRuntime.hpp"
#include <array>
#include <cstring>
#include <filesystem>
#include <thread>
#include <sys/stat.h>

namespace {
using ModuleTest::Require;
using namespace HostProcess::WindowsRuntime;

std::array<unsigned char, 4096> Image() {
    std::array<unsigned char, 4096> bytes{};
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 128;
    std::memcpy(bytes.data(), &dos, sizeof(dos));
    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG] = {512, sizeof(IMAGE_DEBUG_DIRECTORY)};
    std::memcpy(bytes.data() + 128, &nt, sizeof(nt));
    IMAGE_DEBUG_DIRECTORY debug{};
    debug.Type = IMAGE_DEBUG_TYPE_CODEVIEW;
    debug.SizeOfData = 32;
    debug.AddressOfRawData = 1024;
    std::memcpy(bytes.data() + 512, &debug, sizeof(debug));
    std::memcpy(bytes.data() + 1024, "RSDS", 4);
    std::memcpy(bytes.data() + 1028, NtdllIdentity.guid.data(), NtdllIdentity.guid.size());
    std::memcpy(bytes.data() + 1044, &NtdllIdentity.age, sizeof(NtdllIdentity.age));
    return bytes;
}

void Metadata() {
    auto image = Image();
    Require(MatchesImage_nid_no_patch(image.data(), image.size(), NtdllIdentity));
    Require(!MatchesImage_nid_no_patch(image.data(), image.size(), User32Identity));
    auto wrongAge = NtdllIdentity;
    ++wrongAge.age;
    Require(!MatchesImage_nid_no_patch(image.data(), image.size(), wrongAge));
    image[1028] ^= 1;
    Require(!MatchesImage_nid_no_patch(image.data(), image.size(), NtdllIdentity));
    image = Image();
    image[1024] = 'N';
    Require(!MatchesImage_nid_no_patch(image.data(), image.size(), NtdllIdentity));
    image = Image();
    Require(!MatchesImage_nid_no_patch(image.data(), 1030, NtdllIdentity));
    Require(!MatchesImage_nid_no_patch(image.data(), 8, NtdllIdentity));
    Require(!MatchesImage_nid_no_patch(nullptr, 4096, NtdllIdentity));
    IMAGE_DEBUG_DIRECTORY debug{};
    std::memcpy(&debug, image.data() + 512, sizeof(debug));
    debug.AddressOfRawData = 0xfffffff0u;
    std::memcpy(image.data() + 512, &debug, sizeof(debug));
    Require(!MatchesImage_nid_no_patch(image.data(), image.size(), NtdllIdentity));
    Preparation wrongModules{};
    Require(Reconnect_nid_no_patch(wrongModules) == NotImplemented);
}

bool Exercise() {
    // Thread detach must precede any time/path API: some such APIs happen to
    // initialize USER lazily and can otherwise hide the stale IMM32 mapping.
    unsigned value = 0;
    std::thread worker([&] { value = 43; });
    worker.join();
    if (value != 43) return false;
    DYNAMIC_TIME_ZONE_INFORMATION dynamic{};
    if (GetDynamicTimeZoneInformation(&dynamic) == TIME_ZONE_ID_INVALID) return false;
    TIME_ZONE_INFORMATION zone{};
    if (GetTimeZoneInformation(&zone) == TIME_ZONE_ID_INVALID) return false;
    SYSTEMTIME utc{}, local{};
    GetSystemTime(&utc);
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return false;
    const auto path = std::filesystem::current_path();
    struct _stat64 status{};
    return std::filesystem::is_directory(path) && _wstat64(path.c_str(), &status) == 0;
}

bool Clone(HANDLE job, bool nested) {
    const auto cloned = HostProcess::Clone_nid_no_patch();
    if (cloned.branch == HostProcess::Branch::Failed) {
        std::fprintf(stderr, "Win32 clone unsupported/failure: %08lx\n", static_cast<unsigned long>(cloned.nativeStatus));
        return false;
    }
    if (cloned.branch == HostProcess::Branch::Child) {
        const bool initialized = cloned.childInitializationStatus == 0;
        const bool passed = initialized && Exercise() && (!nested || Clone(job, false)) && Exercise();
        TerminateProcess(GetCurrentProcess(), passed ? 0 : 91);
        for (;;) {}
    }
    const auto child = reinterpret_cast<HANDLE>(cloned.processHandle);
    if (!AssignProcessToJobObject(job, child) || HostProcess::Resume_nid_no_patch(cloned.threadHandle) != 0) {
        TerminateProcess(child, 92);
    }
    const DWORD waited = WaitForSingleObject(child, nested ? 10000 : 5000);
    if (waited != WAIT_OBJECT_0) TerminateProcess(child, 93);
    WaitForSingleObject(child, 1000);
    DWORD code = 94;
    GetExitCodeProcess(child, &code);
    HostProcess::Close_nid_no_patch(cloned.threadHandle);
    HostProcess::Close_nid_no_patch(cloned.processHandle);
    if (code != 0) std::fprintf(stderr, "Win32 clone child exit: %08lx\n", code);
    return waited == WAIT_OBJECT_0 && code == 0;
}

void Runtime() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Require(LoadLibraryW(L"user32.dll") != nullptr);
    Require(LoadLibraryW(L"imm32.dll") != nullptr);
    Require(Exercise());
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    const auto job = CreateJobObjectW(&security, nullptr);
    Require(job != nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
    const bool passed = Clone(job, true);
    TerminateJobObject(job, 95);
    CloseHandle(job);
    Require(passed);
    Require(Exercise());
}
}

int main(int argc, char** argv) {
    Require(argc == 2);
    if (std::strcmp(argv[1], "metadata") == 0) Metadata();
    else if (std::strcmp(argv[1], "runtime") == 0) Runtime();
    else Require(false);
}
