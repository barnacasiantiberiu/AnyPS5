#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <immintrin.h>
#include "ModuleChecks.hpp"
#include "prx/libc/include/HostProcess.hpp"
#include <array>
#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" void CaptureHostCloneState(HostProcess::CloneResult*, void*, void*);

extern "C" void TestHostClone_nid_no_patch(HostProcess::CloneResult* result) {
    *result = HostProcess::Clone_nid_no_patch();
}

bool Preserved(const std::array<std::byte, 512>& before, const std::array<std::byte, 512>& after) {
    if (std::memcmp(before.data(), after.data(), 2) != 0) return false;
    std::uint32_t beforeControl = 0;
    std::uint32_t afterControl = 0;
    std::memcpy(&beforeControl, before.data() + 24, sizeof(beforeControl));
    std::memcpy(&afterControl, after.data() + 24, sizeof(afterControl));
    if ((beforeControl & ~0x3fu) != (afterControl & ~0x3fu)) return false;
    return std::memcmp(before.data() + 256, after.data() + 256, 160) == 0;
}

int main() {
    using ModuleTest::Require;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    Require(job != nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
    const auto originalControl = _mm_getcsr();
    const int originalRounding = std::fegetround();
    Require(std::fesetround(FE_DOWNWARD) == 0);
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x4000u);
    alignas(16) std::array<std::byte, 512> before{};
    alignas(16) std::array<std::byte, 512> after{};
    HostProcess::CloneResult cloned;
    CaptureHostCloneState(&cloned, before.data(), after.data());
    const bool preserved = Preserved(before, after);
    if (cloned.branch == HostProcess::Branch::Child) {
        TerminateProcess(GetCurrentProcess(), cloned.childInitializationStatus != 0 ? 85 : preserved ? 0 : 86);
        for (;;) {}
    }
    Require(std::fesetround(originalRounding) == 0);
    _mm_setcsr(originalControl);
    Require(cloned.branch == HostProcess::Branch::Parent);
    const auto process = reinterpret_cast<HANDLE>(cloned.processHandle);
    if (!AssignProcessToJobObject(job, process) || HostProcess::Resume_nid_no_patch(cloned.threadHandle) != 0) {
        TerminateProcess(process, 87);
        CloseHandle(job);
        Require(false);
    }
    const DWORD waited = WaitForSingleObject(process, 5000);
    if (waited != WAIT_OBJECT_0) TerminateProcess(process, 88);
    WaitForSingleObject(process, 1000);
    DWORD code = 89;
    Require(GetExitCodeProcess(process, &code));
    HostProcess::Close_nid_no_patch(cloned.threadHandle);
    HostProcess::Close_nid_no_patch(cloned.processHandle);
    CloseHandle(job);
    if (!preserved || code != 0) std::fprintf(stderr, "Fork register state: parent=%d child=%lu\n", preserved, code);
    Require(preserved && waited == WAIT_OBJECT_0 && code == 0);
}
