#include "prx/libc/include/HostProcess.hpp"
#include "prx/libc/include/WindowsProcessRuntime.hpp"
#include <array>
#include <cerrno>
#include <cstddef>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <immintrin.h>
#else
#include <unistd.h>
#endif

namespace HostProcess {

#ifdef _WIN32

namespace {

struct CloneInformation {
    ULONG length;
    HANDLE process;
    HANDLE thread;
    CLIENT_ID clientId;
    std::array<std::byte, 64> imageInformation;
};

static_assert(offsetof(CloneInformation, process) == 8);
static_assert(offsetof(CloneInformation, clientId) == 24);
static_assert(offsetof(CloneInformation, imageInformation) == 40);
static_assert(sizeof(CloneInformation) == 104);

using CloneFunction = LONG (NTAPI*)(ULONG, void*, void*, void*, CloneInformation*);
constexpr LONG ProcessCloned = 0x129;
constexpr ULONG CreateSuspended = 1;
constexpr ULONG InheritHandles = 2;
constexpr std::uint32_t MissingEntryPoint = 0xc0000139u;

}

#endif

CloneResult Clone_nid_no_patch() {
#ifdef _WIN32
    const auto clone = reinterpret_cast<CloneFunction>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlCloneUserProcess")));
    if (clone == nullptr) return {Branch::Failed, 0, 0, 0, MissingEntryPoint};
    WindowsRuntime::Preparation runtime;
    if (const auto failure = WindowsRuntime::Prepare_nid_no_patch(runtime)) return {Branch::Failed, 0, 0, 0, failure};
    CloneInformation information{};
    alignas(16) std::array<std::byte, 512> floatingState;
    _fxsave64(floatingState.data());
    const LONG status = clone(CreateSuspended | InheritHandles, nullptr, nullptr, nullptr, &information);
    _fxrstor64(floatingState.data());
    if (status == ProcessCloned) {
        const auto initialized = WindowsRuntime::Reconnect_nid_no_patch(runtime);
        return {Branch::Child, GetCurrentProcessId(), 0, 0, static_cast<std::uint32_t>(status), initialized};
    }
    if (status < 0) return {Branch::Failed, 0, 0, 0, static_cast<std::uint32_t>(status)};
    return {Branch::Parent,
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(information.clientId.UniqueProcess)),
            reinterpret_cast<std::uintptr_t>(information.process),
            reinterpret_cast<std::uintptr_t>(information.thread),
            static_cast<std::uint32_t>(status)};
#else
    const auto child = ::fork();
    if (child < 0) return {Branch::Failed, 0, 0, 0, static_cast<std::uint32_t>(errno)};
    if (child == 0) return {Branch::Child, static_cast<std::uint32_t>(::getpid()), 0, 0, 0};
    return {Branch::Parent, static_cast<std::uint32_t>(child), 0, 0, 0};
#endif
}

int Resume_nid_no_patch(std::uintptr_t threadHandle) {
#ifdef _WIN32
    return ResumeThread(reinterpret_cast<HANDLE>(threadHandle)) == static_cast<DWORD>(-1) ? -1 : 0;
#else
    (void)threadHandle;
    return 0;
#endif
}

void Close_nid_no_patch(std::uintptr_t handle) {
#ifdef _WIN32
    if (handle != 0) CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    (void)handle;
#endif
}

}
