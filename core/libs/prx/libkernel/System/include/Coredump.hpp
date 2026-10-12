#ifndef CORE_LIBS_PRX_LIBKERNEL_SYSTEM_COREDUMP_HPP
#define CORE_LIBS_PRX_LIBKERNEL_SYSTEM_COREDUMP_HPP

#include <cstddef>
#include <cstdint>
#include "prx/libc/include/general/VabiMacros.hpp"

struct GuestCoredumpStopInfoCpu {
    void* thread;
    std::int32_t reasonCode;
    std::uint32_t reserved;
};

struct GuestCoredumpThreadContextInfo {
    std::uint64_t rdi, rsi, rdx, rcx, r8, r9, rax, rbx, rbp;
    std::uint64_t r10, r11, r12, r13, r14, r15, rip, rflags, rsp;
};

static_assert(sizeof(GuestCoredumpStopInfoCpu) == 16);
static_assert(sizeof(GuestCoredumpThreadContextInfo) == 144);

using GuestCoredumpHandler = int (APS5_VABI *)(void*);

extern "C" int GuestCoredumpCapture_nid_no_patch(const GuestCoredumpStopInfoCpu* stop,
    const GuestCoredumpThreadContextInfo* context, const char* hostDirectory);

#endif
