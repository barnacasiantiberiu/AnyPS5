#include "Ngs2Internal.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <vector>

namespace {
struct Registration { Ngs2ReportHandler handler; std::uintptr_t userData; };
std::map<Ngs2Handle, Registration>& Handlers() {
    static auto* const handlers = new std::map<Ngs2Handle, Registration>();
    return *handlers;
}
Ngs2Handle nextHandle = 1;
thread_local bool dispatching = false;
}

void Ngs2ReportCpuLoad(Ngs2Handle system, double ratio) {
    if (dispatching || Handlers().empty()) return;
    struct DispatchScope {
        DispatchScope() { dispatching = true; }
        ~DispatchScope() { dispatching = false; }
    } scope;
    std::vector<Ngs2Handle> pending;
    for (const auto& [handle, registration] : Handlers()) pending.push_back(handle);
    const float load = static_cast<float>(std::clamp(ratio, 0.0, double(std::numeric_limits<float>::max())));
    const Ngs2ReportCpuLoadData data{{sizeof(data), system, SCE_NGS2_REPORT_CPU_LOAD, SCE_NGS2_OK}, load, 0, load, 0};
    for (auto handle : pending) {
        const auto found = Handlers().find(handle);
        if (found == Handlers().end()) continue;
        const auto registration = found->second;
        registration.handler(&data.header, registration.userData);
    }
}

int Ngs2ReportRegisterHandler(std::uint32_t report_type, Ngs2ReportHandler handler,
                                         std::uintptr_t user_data, Ngs2Handle* out_handle) {
    if (!out_handle) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (!handler) throw std::invalid_argument("NGS2: null report handler");
    if (report_type != SCE_NGS2_REPORT_CPU_LOAD) throw std::runtime_error("NGS2: unsupported report type " + Ngs2Hex(report_type));
    std::lock_guard lock(Ngs2Mutex());
    if (nextHandle == 0) throw std::overflow_error("NGS2: report handles exhausted");
    const auto handle = nextHandle;
    Handlers().emplace(handle, Registration{handler, user_data});
    ++nextHandle;
    *out_handle = handle;
    return SCE_NGS2_OK;
}
int Ngs2ReportUnregisterHandler(Ngs2Handle report_handle) {
    std::lock_guard lock(Ngs2Mutex());
    if (Handlers().erase(report_handle) != 1) throw std::invalid_argument("NGS2: invalid report handle");
    return SCE_NGS2_OK;
}
