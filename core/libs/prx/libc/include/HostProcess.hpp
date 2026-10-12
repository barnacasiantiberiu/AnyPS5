#pragma once

#include <cstdint>

namespace HostProcess {

enum class Branch { Failed, Parent, Child };

struct CloneResult {
    Branch branch = Branch::Failed;
    std::uint32_t processId = 0;
    std::uintptr_t processHandle = 0;
    std::uintptr_t threadHandle = 0;
    std::uint32_t nativeStatus = 0;
    // A child whose Win32 initialization failed must be terminated and reaped
    // through the coordinator handshake; it must never take the parent path.
    std::uint32_t childInitializationStatus = 0;
};

CloneResult Clone_nid_no_patch();
int Resume_nid_no_patch(std::uintptr_t threadHandle);
void Close_nid_no_patch(std::uintptr_t handle);

}
