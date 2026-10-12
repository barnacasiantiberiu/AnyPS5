#include "prx/libc/include/General.hpp"
#include "prx/libc/include/HeapDiagnostics.hpp"
#include "prx/libc/include/GuestProcess.hpp"
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <stdexcept>

#ifdef _WIN32
#include "prx/libc/include/specifics/itanium/UnwindAbi.hpp"
extern "C" _Unwind_Reason_Code APS5_VABI _Unwind_Backtrace_nid_postfix(_Unwind_Trace_Fn trace, void* argument);
extern "C" _Unwind_Ptr APS5_VABI _Unwind_GetIP_nid_postfix(_Unwind_Context* context);
#else
#include <execinfo.h>
#endif

namespace {

std::uint64_t mspaceAtomicIdMask = 0;
std::array<std::uint64_t, 64> mstateTable{};
std::mutex reportMutex;
bool reportPrepared = false;

bool PrepareReports(void*) {
    reportMutex.lock();
    reportPrepared = true;
    return true;
}

bool ReleaseReports(void*) noexcept {
    if (reportPrepared) {
        reportPrepared = false;
        reportMutex.unlock();
    }
    return true;
}

bool ParentReports(void* context, bool) noexcept { return ReleaseReports(context); }

struct ReportForkRegistration {
    ReportForkRegistration() {
        const GuestForkHooks hooks{50, PrepareReports, ParentReports, ReleaseReports, nullptr};
        if (GuestRegisterForkHooks_nid_no_patch(&hooks) != 0)
            throw std::runtime_error("libc: registering heap diagnostic fork hooks failed");
    }
} reportForkRegistration;

static_assert(sizeof(LibcHeapInfo) == 32);
static_assert(offsetof(LibcHeapInfo, mspace_atomic_id_mask) == 16);
static_assert(offsetof(LibcHeapInfo, mstate_table) == 24);

struct Backtrace {
    std::array<void*, 32> frames{};
    std::size_t count = 0;
};

#ifdef _WIN32

_Unwind_Reason_Code CaptureFrame(_Unwind_Context* context, void* argument) {
    auto& trace = *static_cast<Backtrace*>(argument);
    const auto address = _Unwind_GetIP_nid_postfix(context);
    if (address == 0 || trace.count == trace.frames.size()) return _URC_END_OF_STACK;
    trace.frames[trace.count++] = reinterpret_cast<void*>(address);
    return trace.count == trace.frames.size() ? _URC_NORMAL_STOP : _URC_NO_REASON;
}

#endif

void WriteBacktrace() {
    Backtrace trace;
#ifdef _WIN32
    _Unwind_Backtrace_nid_postfix(CaptureFrame, &trace);
#else
    const int count = ::backtrace(trace.frames.data(), static_cast<int>(trace.frames.size()));
    if (count > 0) trace.count = static_cast<std::size_t>(count);
#endif
    for (std::size_t index = 0; index < trace.count; ++index)
        std::fprintf(stderr, "  #%zu 0x%llx\n", index,
                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(trace.frames[index])));
    if (trace.count == 0) std::fputs("  host backtrace unavailable\n", stderr);
}

}

extern "C" {

void LibcHeapTraceInfo_nid_no_patch(LibcHeapInfo* info) {
    if (info == nullptr)
        throw std::invalid_argument("heap trace info: null output");
    if (info->size != sizeof(LibcHeapInfo))
        throw std::invalid_argument("heap trace info: unsupported structure size");
    info->unknown2 = 0;
    info->mspace_atomic_id_mask = &mspaceAtomicIdMask;
    info->mstate_table = mstateTable.data();
}

void APS5_VABI sceLibcInternalBacktraceForGame_nid_postfix(const char* heapName) {
    const int savedError = errno;
    {
        std::lock_guard lock(reportMutex);
        std::fprintf(stderr, "[%s] heap backtrace (host frames):\n", heapName ? heapName : "");
        WriteBacktrace();
        std::fflush(stderr);
    }
    errno = savedError;
}

void APS5_VABI sceLibcInternalHeapErrorReportForGame_nid_postfix(void* heap, void* block, std::uint32_t error) {
    const int savedError = errno;
    {
        std::lock_guard lock(reportMutex);
        std::fprintf(stderr, "[libc] heap error 0x%x: heap %p, block %p (host frames):\n", error, heap, block);
        WriteBacktrace();
        std::fflush(stderr);
    }
    errno = savedError;
}

}
