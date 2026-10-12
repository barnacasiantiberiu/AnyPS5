#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <thread>
#include <windows.h>

extern "C" {
void* APS5_VABI __tls_get_addr_nid_postfix(const std::uintptr_t*);
extern const std::uint64_t AnyPS5TestTlsTemplate[2];
}

static void Require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

static bool Rejects(const std::uintptr_t* index) {
    try { __tls_get_addr_nid_postfix(index); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}

int main() {
    const auto* image = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(
        image + pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);
    const auto offset = reinterpret_cast<std::uintptr_t>(AnyPS5TestTlsTemplate) - tls->StartAddressOfRawData;
    const std::array<std::uintptr_t, 2> index{tls->AddressOfIndex, offset};
    auto* first = static_cast<std::uint64_t*>(__tls_get_addr_nid_postfix(index.data()));
    Require(first[0] == 0x1122334455667788ull && first[1] == 0x8877665544332211ull, "TLS template was not copied by the native loader");
    first[0] = 17;
    Require(__tls_get_addr_nid_postfix(index.data()) == first, "TLS address changed within the same thread");
    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            auto* second = static_cast<std::uint64_t*>(__tls_get_addr_nid_postfix(index.data()));
            Require(second != first && second[0] == 0x1122334455667788ull, "new thread inherited another thread's TLS state");
            second[0] = 42;
            Require(first[0] == 17, "TLS writes escaped their owning thread");
            auto tail = index;
            tail[1] += sizeof(std::uint64_t);
            Require(__tls_get_addr_nid_postfix(tail.data()) == second + 1, "TLS offset was not applied");
        } catch (...) { failure = std::current_exception(); }
    });
    worker.join();
    if (failure) std::rethrow_exception(failure);
    Require(first[0] == 17 && AnyPS5TestTlsTemplate[0] == 0x1122334455667788ull, "thread-local writes changed the image template");
    Require(Rejects(nullptr), "null TLS index was accepted");
    auto bad = index;
    bad[0] = reinterpret_cast<std::uintptr_t>(AnyPS5TestTlsTemplate);
    Require(Rejects(bad.data()), "non-index image address was accepted");
    bad = index;
    bad[1] = tls->EndAddressOfRawData - tls->StartAddressOfRawData + tls->SizeOfZeroFill;
    Require(Rejects(bad.data()), "TLS boundary overrun was accepted");
    bad[0] = 1;
    Require(Rejects(bad.data()), "foreign TLS index was accepted");
}
