#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <cstddef>
#include "prx/libc/include/WindowsProcessLayout.hpp"

namespace HostProcess::WindowsRuntime {

struct Preparation {
    Preparation() = default;
    ~Preparation();
    Preparation(const Preparation&) = delete;
    Preparation& operator=(const Preparation&) = delete;
    void Reset_nid_no_patch() noexcept;

    HMODULE ntdll = nullptr;
    HMODULE kernelbase = nullptr;
    // Balanced loader references, inherited by the native clone and released
    // independently by the Preparation destructor in both parent and child.
    HMODULE user32 = nullptr;
    HMODULE imm32 = nullptr;
    LONG (NTAPI* connect)(PWSTR, ULONG, PVOID, ULONG, PBOOLEAN) = nullptr;
    BOOL (WINAPI* registerImm)(void*, HMODULE) = nullptr;
    void* controlRoutine = nullptr;
    void* oldSharedBase = nullptr;
    void* oldStaticData = nullptr;
    std::size_t oldSharedBytes = 0;
    std::array<wchar_t, 128> directory{};
};

// Also exercised with synthetic image metadata: a mismatch must be rejected
// before cloning or touching any Windows globals.
bool MatchesImage_nid_no_patch(const void* image, std::size_t size, const SymbolIdentity& identity) noexcept;
std::uint32_t Prepare_nid_no_patch(Preparation& preparation) noexcept;
std::uint32_t Reconnect_nid_no_patch(const Preparation& preparation) noexcept;

}
#endif
