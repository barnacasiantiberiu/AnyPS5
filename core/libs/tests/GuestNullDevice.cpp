#include "ModuleChecks.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"
#include <array>
#include <cstdint>

extern "C" {
int APS5_VABI open_nid_postfix(const char*, int, int);
int APS5_VABI close_nid_postfix(int);
int APS5_VABI dup_nid_postfix(int);
int* APS5_VABI __error_nid_postfix();
int APS5_VABI fcntl_nid_postfix(int, int, ...);
std::int64_t APS5_VABI read_nid_postfix(int, void*, std::uint64_t);
std::int64_t APS5_VABI write_nid_postfix(int, const char*, std::int64_t);
}

int main() {
    using ModuleTest::Require;
    constexpr char payload[] = "discarded null-device output";
    for (const int mode : {SCE_KERNEL_O_RDONLY, SCE_KERNEL_O_WRONLY, SCE_KERNEL_O_RDWR}) {
        const int descriptor = open_nid_postfix("/dev/null", mode, 0);
        Require(descriptor >= 0);
        Require((fcntl_nid_postfix(descriptor, 3) & 3) == mode);
        const int copy = dup_nid_postfix(descriptor);
        Require(copy >= 0 && copy != descriptor);
        Require(close_nid_postfix(descriptor) == 0);
        std::array<char, 32> output;
        output.fill('G');
        const auto unchanged = output;
        if (mode != SCE_KERNEL_O_WRONLY) {
            Require(read_nid_postfix(copy, output.data(), output.size()) == 0);
            Require(output == unchanged);
        }
        if (mode != SCE_KERNEL_O_RDONLY) {
            Require(write_nid_postfix(copy, payload, sizeof(payload) - 1) == sizeof(payload) - 1);
        }
        if (mode == SCE_KERNEL_O_RDWR) {
            Require(read_nid_postfix(copy, output.data(), output.size()) == 0);
            Require(output == unchanged);
        }
        Require(close_nid_postfix(copy) == 0);
    }
    Require(open_nid_postfix("/dev/null", SCE_KERNEL_O_DIRECTORY, 0) == -1);
    Require(*__error_nid_postfix() == 20);
    Require(open_nid_postfix("/dev/null", SCE_KERNEL_O_CREAT | SCE_KERNEL_O_EXCL | SCE_KERNEL_O_RDWR, 0600) == -1);
    Require(*__error_nid_postfix() == 17);
    const int existing = open_nid_postfix("/dev/null", SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC | SCE_KERNEL_O_RDWR, 0600);
    Require(existing >= 0);
    Require(close_nid_postfix(existing) == 0);
}
