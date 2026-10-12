#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_DAEMONRUNTIME_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_DAEMONRUNTIME_HPP

#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstddef>
#include <cstdint>

struct GuestSignalSet {
    std::uint32_t bits[4];
};

struct GuestSigaction {
    std::uintptr_t handler;
    int flags;
    GuestSignalSet mask;
};
static_assert(sizeof(GuestSigaction) == 32 && offsetof(GuestSigaction, flags) == 8 && offsetof(GuestSigaction, mask) == 12);

struct DaemonRuntime {
    int (APS5_VABI *signalAction)(int, const GuestSigaction*, GuestSigaction*);
    int (*openNull)();
    int (APS5_VABI *duplicate)(int, int);
    int (APS5_VABI *close)(int);
};

extern "C" void LibcRegisterDaemonRuntime_nid_no_patch(const DaemonRuntime* runtime);

#endif
