#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/General.hpp"
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#elif !defined(__APPLE__)
extern "C" void* __tls_get_addr(const std::uintptr_t*);
#endif

extern "C" void* APS5_VABI __tls_get_addr_nid_postfix(const std::uintptr_t* index) {
    if (!index) throw std::invalid_argument("__tls_get_addr: null TLS index");
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(index[0]), &module))
        throw std::invalid_argument("__tls_get_addr: TLS index is outside a loaded image");
    struct ModuleReference {
        HMODULE handle;
        ~ModuleReference() { FreeLibrary(handle); }
    } reference{module};
    const auto* image = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    const auto directory = pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!directory.VirtualAddress || directory.Size < sizeof(IMAGE_TLS_DIRECTORY64))
        throw std::invalid_argument("__tls_get_addr: image has no native TLS directory");
    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(image + directory.VirtualAddress);
    if (tls->AddressOfIndex != index[0] || tls->EndAddressOfRawData < tls->StartAddressOfRawData)
        throw std::invalid_argument("__tls_get_addr: invalid module TLS index");
    const auto length = tls->EndAddressOfRawData - tls->StartAddressOfRawData + tls->SizeOfZeroFill;
    if (index[1] >= length) throw std::invalid_argument("__tls_get_addr: offset outside module TLS");
    const auto slot = *reinterpret_cast<const DWORD*>(tls->AddressOfIndex);
    static_assert(offsetof(TEB, Reserved1) + 11 * sizeof(void*) == 0x58);
    auto** blocks = static_cast<unsigned char**>(NtCurrentTeb()->Reserved1[11]);
    if (!blocks || !blocks[slot]) throw std::runtime_error("__tls_get_addr: module TLS is not initialized for this thread");
    return blocks[slot] + index[1];
#elif defined(__APPLE__)
    NotImplemented_nid_no_patch("__tls_get_addr: Mach-O guest TLS descriptor resolution");
    return nullptr;
#else
    return ::__tls_get_addr(index);
#endif
}
