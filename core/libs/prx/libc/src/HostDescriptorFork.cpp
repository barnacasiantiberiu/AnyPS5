#include "prx/libc/include/GuestProcess.hpp"
#include "prx/libc/include/HostDescriptorFork.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <mutex>
#include <new>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
extern "C" _invalid_parameter_handler _set_thread_local_invalid_parameter_handler(_invalid_parameter_handler);
#endif

namespace {

struct DescriptorDepthTag {};
struct DescriptorDepth {
    unsigned value = 0;
};

std::mutex descriptorGate;
bool prepared = false;

#ifdef _WIN32

struct HandleState {
    HANDLE handle;
    DWORD flags = 0;
    bool changed = false;
};

std::vector<HandleState> handles;
constexpr int DescriptorLimit = 8192;

void ignoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) {}

class ParameterHandler {
public:
    ParameterHandler() : previous(_set_thread_local_invalid_parameter_handler(ignoreInvalidParameter)) {}
    ~ParameterHandler() { _set_thread_local_invalid_parameter_handler(previous); }
    ParameterHandler(const ParameterHandler&) = delete;
    ParameterHandler& operator=(const ParameterHandler&) = delete;

private:
    _invalid_parameter_handler previous;
};

bool restoreHandles() noexcept {
    bool success = true;
    for (auto& entry : handles) {
        if (!entry.changed) continue;
        if (!SetHandleInformation(entry.handle, HANDLE_FLAG_INHERIT, entry.flags & HANDLE_FLAG_INHERIT)) success = false;
        else entry.changed = false;
    }
    return success;
}

#endif

bool prepareDescriptors(void*) {
    try {
        BeginHostDescriptorOperation_nid_no_patch();
        prepared = true;
#ifdef _WIN32
        const int previousError = errno;
        ParameterHandler handler;
        handles.clear();
        handles.reserve(DescriptorLimit);
        for (int descriptor = 0; descriptor < DescriptorLimit; ++descriptor) {
            const auto handle = _get_osfhandle(descriptor);
            if (handle == -1 || handle == -2) continue;
            handles.push_back({reinterpret_cast<HANDLE>(handle)});
        }
        std::sort(handles.begin(), handles.end(), [](const HandleState& left, const HandleState& right) {
            return reinterpret_cast<std::uintptr_t>(left.handle) < reinterpret_cast<std::uintptr_t>(right.handle);
        });
        handles.erase(std::unique(handles.begin(), handles.end(), [](const HandleState& left, const HandleState& right) {
            return left.handle == right.handle;
        }), handles.end());
        for (auto& entry : handles) {
            if (!GetHandleInformation(entry.handle, &entry.flags)) {
                restoreHandles();
                errno = EIO;
                return false;
            }
            if ((entry.flags & HANDLE_FLAG_INHERIT) != 0) continue;
            if (!SetHandleInformation(entry.handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
                restoreHandles();
                errno = EIO;
                return false;
            }
            entry.changed = true;
        }
        errno = previousError;
#endif
        return true;
    } catch (const std::bad_alloc&) {
#ifdef _WIN32
        restoreHandles();
#endif
        errno = ENOMEM;
        return false;
    } catch (...) {
#ifdef _WIN32
        restoreHandles();
#endif
        errno = EIO;
        return false;
    }
}

bool releaseDescriptors(void*) noexcept {
    if (!prepared) return true;
    bool success = true;
#ifdef _WIN32
    success = restoreHandles();
    handles.clear();
#endif
    prepared = false;
    EndHostDescriptorOperation_nid_no_patch();
    if (!success) errno = EIO;
    return success;
}

bool parentDescriptors(void* context, bool) noexcept {
    return releaseDescriptors(context);
}

}

extern "C" void BeginHostDescriptorOperation_nid_no_patch() {
    auto& depth = HostThreadLocal<DescriptorDepth, DescriptorDepthTag>().value;
    if (depth == 0) descriptorGate.lock();
    ++depth;
}

extern "C" void EndHostDescriptorOperation_nid_no_patch() noexcept {
    auto& depth = HostThreadLocal<DescriptorDepth, DescriptorDepthTag>().value;
    if (depth == 0) std::terminate();
    if (--depth == 0) descriptorGate.unlock();
}

extern "C" int RegisterHostDescriptorFork_nid_no_patch() {
    try {
        static_cast<void>(HostThreadLocal<DescriptorDepth, DescriptorDepthTag>());
        const GuestForkHooks hooks{100, prepareDescriptors, parentDescriptors, releaseDescriptors, nullptr};
        return GuestRegisterForkHooks_nid_no_patch(&hooks);
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    } catch (...) {
        return EIO;
    }
}
