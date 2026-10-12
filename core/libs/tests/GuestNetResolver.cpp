#include "prx/libc/include/general/VabiMacros.hpp"
#include "ModuleChecks.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

extern "C" {
int APS5_VABI sceNetInit_nid_postfix();
int* APS5_VABI sceNetErrnoLoc();
int APS5_VABI sceNetResolverCreate(const char*, int, int);
int APS5_VABI sceNetResolverDestroy(int);
int APS5_VABI sceNetResolverGetError(int, int*);
int APS5_VABI sceNetResolverAbort(int, int);
int APS5_VABI sceNetResolverStartNtoa(int, const char*, void*, int, int, int);
int APS5_VABI sceNetResolverStartNtoaMultipleRecords(int, const char*, void*, int, int, int);
int APS5_VABI sceNetResolverStartNtoaMultipleRecordsEx(int, const char*, void*, int, int, int);
int APS5_VABI sceNetResolverStartAton(int, const void*, char*, int, int, int, int);
}

using ModuleTest::Require;
using ModuleTest::RequireThrows;
constexpr int Interrupted = static_cast<int>(0x80410104u);
constexpr int BadDescriptor = static_cast<int>(0x80410109u);
constexpr int Invalid = static_cast<int>(0x80410116u);
using Lookup = int (APS5_VABI*)(int, const char*, void*, int, int, int);

static void CheckRecords(Lookup lookup, int resolver) {
    alignas(8) std::array<std::uint8_t, 400> storage;
    storage.fill(0xA5);
    auto* records = storage.data() + 8;
    Require(lookup(resolver, "127.0.0.1", records, 0, 0, 0) == 0);
    std::array<std::uint8_t, 384> expected{};
    expected[0] = 127;
    expected[3] = 1;
    const std::int32_t family = 2, count = 1;
    std::memcpy(expected.data() + 16, &family, 4);
    std::memcpy(expected.data() + 320, &count, 4);
    std::memcpy(expected.data() + 324, &count, 4);
    Require(std::memcmp(records, expected.data(), expected.size()) == 0);
    Require(std::all_of(storage.begin(), storage.begin() + 8, [](auto byte) { return byte == 0xA5; }));
    Require(std::all_of(storage.end() - 8, storage.end(), [](auto byte) { return byte == 0xA5; }));
    const auto saved = storage;
    Require(lookup(resolver, nullptr, records, 0, 0, 0) == Invalid);
    Require(lookup(resolver, "127.0.0.1", nullptr, 0, 0, 0) == Invalid);
    Require(lookup(-1, "127.0.0.1", records, 0, 0, 0) == BadDescriptor);
    for (const int flags : {1, 0x10000, -1}) {
        RequireThrows([&] { lookup(resolver, "127.0.0.1", records, 0, 0, flags); }, "flags");
    }
    Require(storage == saved);
    Require(sceNetResolverAbort(resolver, 1) == 0);
    Require(lookup(resolver, "127.0.0.1", records, 0, 0, 0) == Interrupted);
    Require(*sceNetErrnoLoc() == 4 && storage == saved);
    int error = 0;
    Require(sceNetResolverGetError(resolver, &error) == 0 && error == Interrupted);
    Require(lookup(resolver, "127.0.0.1", records, 0, 0, 0) == 0);
    Require(sceNetResolverGetError(resolver, &error) == 0 && error == 0);
    Require(storage == saved);
}

int main() {
    Require(sceNetInit_nid_postfix() == 0);
    Require(sceNetResolverAbort(-1, 0) == BadDescriptor);
    for (int cycle = 0; cycle < 16; ++cycle) {
        const int resolver = sceNetResolverCreate("pc-resolver", 0, 0);
        const int other = sceNetResolverCreate("pc-independent", 0, 0);
        Require(resolver >= 0 && other >= 0 && resolver != other);
        CheckRecords(sceNetResolverStartNtoaMultipleRecords, resolver);
        CheckRecords(sceNetResolverStartNtoaMultipleRecordsEx, resolver);
        for (const int flags : {4, -1}) {
            RequireThrows([&] { sceNetResolverAbort(resolver, flags); }, "flags");
        }
        Require(sceNetResolverAbort(resolver, 3) == 0);
        Require(sceNetResolverAbort(resolver, 3) == 0);
        Require(sceNetResolverAbort(resolver, 0) == 0);
        std::array<std::uint8_t, 4> address{};
        Require(sceNetResolverStartNtoa(other, "127.0.0.1", address.data(), 0, 0, 0) == 0);
        Require((address == std::array<std::uint8_t, 4>{127, 0, 0, 1}));
        Require(sceNetResolverStartNtoa(resolver, nullptr, address.data(), 0, 0, 0) == Invalid);
        const auto saved = address;
        Require(sceNetResolverStartNtoa(resolver, "127.0.0.1", address.data(), 0, 0, 0) == Interrupted);
        Require(address == saved);
        Require(sceNetResolverStartNtoa(resolver, "127.0.0.1", address.data(), 0, 0, 0) == 0);
        std::array<char, 32> name;
        name.fill('X');
        Require(sceNetResolverStartAton(resolver, address.data(), nullptr, 32, 0, 0, 0) == Invalid);
        Require(sceNetResolverStartAton(resolver, address.data(), name.data(), 0, 0, 0, 0) == Invalid);
        Require(sceNetResolverStartAton(resolver, address.data(), name.data(), 32, 0, 0, 0) == Interrupted);
        Require(std::all_of(name.begin(), name.end(), [](char byte) { return byte == 'X'; }));
        Require(sceNetResolverGetError(resolver, nullptr) == Invalid);
        Require(sceNetResolverAbort(resolver, 3) == 0);
        Require(sceNetResolverDestroy(resolver) == 0);
        Require(sceNetResolverDestroy(resolver) == BadDescriptor);
        Require(sceNetResolverAbort(resolver, 0) == BadDescriptor);
        Require(sceNetResolverStartNtoa(resolver, "127.0.0.1", address.data(), 0, 0, 0) == BadDescriptor);
        int error = 123;
        Require(sceNetResolverGetError(resolver, &error) == BadDescriptor && error == 123);
        Require(sceNetResolverStartNtoa(other, "127.0.0.1", address.data(), 0, 0, 0) == 0);
        Require(sceNetResolverDestroy(other) == 0);
    }
}
