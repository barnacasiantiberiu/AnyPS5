#include "ModuleChecks.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <link.h>
#endif

extern "C" int APS5_VABI sceKernelInternalMemoryGetModuleSegmentInfo_nid_postfix(void*);

using ModuleTest::Require;

namespace {

struct Segment {
    std::uint64_t address;
    std::uint64_t size;
};

Segment Expected() {
#ifdef _WIN32
    const auto* image = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    const auto* sections = IMAGE_FIRST_SECTION(pe);
    for (std::size_t i = 0; i < pe->FileHeader.NumberOfSections; ++i) {
        if (std::strncmp(reinterpret_cast<const char*>(sections[i].Name), ".text", IMAGE_SIZEOF_SHORT_NAME) != 0) continue;
        Require((sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0);
        return {reinterpret_cast<std::uintptr_t>(image) + sections[i].VirtualAddress, sections[i].Misc.VirtualSize};
    }
#else
    Segment segment{};
    dl_iterate_phdr(+[](dl_phdr_info* image, std::size_t, void* context) {
        if (image->dlpi_name && *image->dlpi_name) return 0;
        for (std::uint16_t i = 0; i < image->dlpi_phnum; ++i) {
            const auto& header = image->dlpi_phdr[i];
            if (header.p_type != PT_LOAD) continue;
            *static_cast<Segment*>(context) = {image->dlpi_addr + header.p_vaddr, header.p_memsz};
            return 1;
        }
        return 0;
    }, &segment);
    if (segment.size) return segment;
#endif
    Require(false);
    return {};
}

void Check(const Segment& expected) {
    struct Output { std::array<unsigned char, 16> before; Segment segment; std::array<unsigned char, 16> after; } output;
    std::memset(&output, 0xab, sizeof(output));
    Require(sceKernelInternalMemoryGetModuleSegmentInfo_nid_postfix(&output.segment) == 0);
    Require(output.segment.address == expected.address && output.segment.size == expected.size);
    for (const auto value : output.before) Require(value == 0xab);
    for (const auto value : output.after) Require(value == 0xab);
    Require(output.segment.size > 0);
    Require(std::memcmp(reinterpret_cast<const void*>(output.segment.address), reinterpret_cast<const void*>(expected.address), 16) == 0);
}

}

int main() {
    const auto expected = Expected();
    Check(expected);
    ModuleTest::RequireThrows([] { sceKernelInternalMemoryGetModuleSegmentInfo_nid_postfix(nullptr); }, "null output");
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 4; ++i) workers.emplace_back([&] { for (unsigned iteration = 0; iteration < 8; ++iteration) Check(expected); });
    for (auto& worker : workers) worker.join();
    Check(expected);
}
