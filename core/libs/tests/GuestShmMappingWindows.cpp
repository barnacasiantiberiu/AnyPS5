#include "ModuleChecks.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

extern "C" {
int APS5_VABI shm_open_nid_postfix(const char*, int, int);
int APS5_VABI shm_unlink_nid_postfix(const char*);
int APS5_VABI ftruncate_nid_postfix(int, std::int64_t);
int APS5_VABI close_nid_postfix(int);
int* APS5_VABI __error_nid_postfix();
std::int64_t APS5_VABI pread_nid_postfix(int, void*, std::size_t, std::int64_t);
std::int64_t APS5_VABI pwrite_nid_disambig1_nid_postfix(int, const void*, std::size_t, std::int64_t);
void* APS5_VABI mmap_nid_postfix(void*, std::size_t, int, int, int, std::int64_t);
int APS5_VABI munmap_nid_postfix(void*, std::size_t);
int APS5_VABI mprotect_nid_postfix(void*, std::size_t, int);
int APS5_VABI fork_nid_postfix();
int APS5_VABI waitpid_nid_postfix(int, int*, int);
[[noreturn]] void APS5_VABI _exit_nid_postfix(int);
}

namespace {
using ModuleTest::Require;
constexpr std::size_t Page = 0x4000;
constexpr auto Failed = static_cast<std::uintptr_t>(-1);

std::byte* Map(int fd, std::size_t bytes, int kind, std::int64_t offset = 0, void* at = nullptr) {
    *__error_nid_postfix() = 731;
    auto* mapped = mmap_nid_postfix(at, bytes, 3, kind, fd, offset);
    Require(reinterpret_cast<std::uintptr_t>(mapped) != Failed);
    Require(reinterpret_cast<std::uintptr_t>(mapped) % Page == 0);
    Require(*__error_nid_postfix() == 731);
    return static_cast<std::byte*>(mapped);
}

void Mapping(bool forkCase) {
    const int fd = shm_open_nid_postfix(reinterpret_cast<const char*>(1), 2, 0600);
    Require(fd >= 0 && ftruncate_nid_postfix(fd, 4 * Page) == 0);
    const std::uint32_t initial = 0x10203040;
    Require(pwrite_nid_disambig1_nid_postfix(fd, &initial, sizeof(initial), Page + 12) == sizeof(initial));
    auto* first = Map(fd, 3 * Page, 1, Page);
    auto* alias = Map(fd, 3 * Page, 1, Page);
    auto* privateView = Map(fd, 3 * Page, 2, Page);
    Require(std::memcmp(first + 12, &initial, sizeof(initial)) == 0);
    Require(std::memcmp(alias + 12, &initial, sizeof(initial)) == 0);
    Require(std::memcmp(privateView + 12, &initial, sizeof(initial)) == 0);
    first[33] = std::byte{0x51};
    Require(alias[33] == std::byte{0x51});
    std::byte value{};
    Require(pread_nid_postfix(fd, &value, 1, Page + 33) == 1 && value == std::byte{0x51});
    value = std::byte{0x62};
    Require(pwrite_nid_disambig1_nid_postfix(fd, &value, 1, Page + 34) == 1);
    Require(first[34] == value && alias[34] == value);
    privateView[35] = std::byte{0x73};
    Require(first[35] == std::byte{0} && alias[35] == std::byte{0});
    Require(mprotect_nid_postfix(privateView, Page, 1) == 0);
    Require(mprotect_nid_postfix(privateView, Page, 3) == 0);
    privateView[36] = std::byte{0x74};
    Require(first[36] == std::byte{0} && privateView[35] == std::byte{0x73});
    if (forkCase) {
        const int child = fork_nid_postfix();
        Require(child >= 0);
        if (child == 0) {
            first[40] = std::byte{0x81};
            privateView[41] = std::byte{0x82};
            _exit_nid_postfix(privateView[35] == std::byte{0x73} ? 23 : 24);
        }
        int status = -1;
        Require(waitpid_nid_postfix(child, &status, 0) == child && status == (23 << 8));
        Require(first[40] == std::byte{0x81} && alias[40] == std::byte{0x81});
        Require(privateView[41] == std::byte{0} && first[41] == std::byte{0});
    }
    Require(close_nid_postfix(fd) == 0);
    first[Page + 21] = std::byte{0x91};
    Require(alias[Page + 21] == std::byte{0x91});
    Require(munmap_nid_postfix(first + Page, Page) == 0);
    first[2 * Page + 19] = std::byte{0xa1};
    Require(alias[2 * Page + 19] == std::byte{0xa1});
    Require(munmap_nid_postfix(first, Page) == 0);
    Require(munmap_nid_postfix(first + 2 * Page, Page) == 0);
    Require(munmap_nid_postfix(alias, 3 * Page) == 0);
    Require(munmap_nid_postfix(privateView, 3 * Page) == 0);
}

void ReadonlyRights() {
    const std::string name = "/anyps5-mapping-rights-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    const int writer = shm_open_nid_postfix(name.c_str(), 2 | 0x200 | 0x800, 0600);
    Require(writer >= 0 && ftruncate_nid_postfix(writer, 2 * Page) == 0);
    const std::byte initial{0x35};
    Require(pwrite_nid_disambig1_nid_postfix(writer, &initial, 1, 31) == 1);
    const int reader = shm_open_nid_postfix(name.c_str(), 0, 0);
    Require(reader >= 0);
    Require(shm_unlink_nid_postfix(name.c_str()) == 0);
    *__error_nid_postfix() = 731;
    Require(reinterpret_cast<std::uintptr_t>(mmap_nid_postfix(nullptr, Page, 3, 1, reader, 0)) == Failed);
    Require(*__error_nid_postfix() == 13);
    *__error_nid_postfix() = 731;
    auto* shared = static_cast<std::byte*>(mmap_nid_postfix(nullptr, 2 * Page, 1, 1, reader, 0));
    Require(reinterpret_cast<std::uintptr_t>(shared) != Failed && *__error_nid_postfix() == 731);
    Require(shared[31] == initial);
    Require(mprotect_nid_postfix(shared, 2 * Page, 3) == -1 && *__error_nid_postfix() == 13);
    MEMORY_BASIC_INFORMATION memory{};
    Require(VirtualQuery(shared, &memory, sizeof(memory)) == sizeof(memory) && memory.Protect == PAGE_READONLY);
    Require(VirtualQuery(shared + Page, &memory, sizeof(memory)) == sizeof(memory) && memory.Protect == PAGE_READONLY);
    Require(shared[31] == initial);
    Require(mprotect_nid_postfix(shared, 2 * Page, 0) == 0);
    Require(mprotect_nid_postfix(shared, 2 * Page, 1) == 0 && shared[31] == initial);
    auto* privateView = Map(reader, 2 * Page, 2);
    privateView[31] = std::byte{0x46};
    privateView[Page + 32] = std::byte{0x57};
    Require(shared[31] == initial && shared[Page + 32] == std::byte{0});
    Require(mprotect_nid_postfix(privateView, 2 * Page, 1) == 0);
    Require(mprotect_nid_postfix(privateView, 2 * Page, 3) == 0);
    Require(privateView[31] == std::byte{0x46} && privateView[Page + 32] == std::byte{0x57});
    privateView[32] = std::byte{0x68};
    std::byte persisted{};
    Require(pread_nid_postfix(reader, &persisted, 1, 31) == 1 && persisted == initial);
    Require(pread_nid_postfix(reader, &persisted, 1, 32) == 1 && persisted == std::byte{0});
    Require(close_nid_postfix(reader) == 0 && close_nid_postfix(writer) == 0);
    privateView[33] = std::byte{0x79};
    Require(shared[33] == std::byte{0} && shared[31] == initial);
    Require(munmap_nid_postfix(shared, 2 * Page) == 0);
    Require(munmap_nid_postfix(privateView, 2 * Page) == 0);
}

void OutsideArenaFixed() {
    const int fd = shm_open_nid_postfix(reinterpret_cast<const char*>(1), 2, 0600);
    Require(fd >= 0 && ftruncate_nid_postfix(fd, 2 * Page) == 0);
    auto* original = static_cast<std::byte*>(mmap_nid_postfix(nullptr, 2 * Page, 3, 2 | 0x1000, -1, 0));
    Require(reinterpret_cast<std::uintptr_t>(original) != Failed);
    auto* misaligned = reinterpret_cast<std::uintptr_t>(original) % 0x200000 == 0 ? original + Page : original;
    misaligned[0] = std::byte{0xc9};
    misaligned[Page - 1] = std::byte{0xda};
    *__error_nid_postfix() = 731;
    Require(reinterpret_cast<std::uintptr_t>(mmap_nid_postfix(misaligned, Page, 3, 1 | 0x10 | 0x1000000, fd, 0)) == Failed);
    Require(*__error_nid_postfix() == 22);
    Require(misaligned[0] == std::byte{0xc9} && misaligned[Page - 1] == std::byte{0xda});
    Require(munmap_nid_postfix(original, 2 * Page) == 0);
    auto* foreign = static_cast<std::byte*>(VirtualAlloc(nullptr, Page, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
    Require(foreign != nullptr && reinterpret_cast<std::uintptr_t>(foreign) % Page == 0);
    Require(!GuestArena::GuestArenaContains_nid_postfix(foreign, Page));
    foreign[0] = std::byte{0xa7};
    foreign[Page - 1] = std::byte{0xb8};
    *__error_nid_postfix() = 731;
    Require(reinterpret_cast<std::uintptr_t>(mmap_nid_postfix(foreign, Page, 3, 1 | 0x10, fd, 0)) == Failed);
    Require(*__error_nid_postfix() == 22);
    Require(foreign[0] == std::byte{0xa7} && foreign[Page - 1] == std::byte{0xb8});
    MEMORY_BASIC_INFORMATION memory{};
    Require(VirtualQuery(foreign, &memory, sizeof(memory)) == sizeof(memory));
    Require(memory.AllocationBase == foreign && memory.Type == MEM_PRIVATE && memory.Protect == PAGE_READWRITE);
    Require(VirtualFree(foreign, 0, MEM_RELEASE) != 0);
    Require(close_nid_postfix(fd) == 0);
}
}

int main(int argc, char** argv) {
    const bool forkCase = argc == 2 && std::string_view(argv[1]) == "fork";
    Mapping(forkCase);
    if (!forkCase) {
        ReadonlyRights();
        OutsideArenaFixed();
    }
}
