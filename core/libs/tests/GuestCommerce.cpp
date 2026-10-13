#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
int APS5_VABI sceNpCommerceDialogInitialize();
int APS5_VABI sceNpCommerceDialogTerminate();
int APS5_VABI sceNpCommerceDialogOpen(const void*);
int APS5_VABI sceNpCommerceDialogOpen2(const void*);
int APS5_VABI sceNpCommerceDialogGetStatus();
int APS5_VABI sceNpCommerceDialogUpdateStatus();
int APS5_VABI sceNpCommerceDialogGetResult(void*);
int APS5_VABI sceNpCommerceDialogClose();
int APS5_VABI sceNpCommerceShowPsStoreIcon(int);
int APS5_VABI sceNpCommerceHidePsStoreIcon();
int APS5_VABI sceNpCommerceSetPsStoreIconLayout(int);
}

namespace {

constexpr int NotInitialized = static_cast<int>(0x80b80003);
constexpr int AlreadyInitialized = static_cast<int>(0x80b80004);
constexpr int NotFinished = static_cast<int>(0x80b80005);
constexpr int Busy = static_cast<int>(0x80b80008);
constexpr int InvalidParameter = static_cast<int>(0x80b8000a);
constexpr int NotRunning = static_cast<int>(0x80b8000b);
constexpr int NullArgument = static_cast<int>(0x80b8000d);
using OpenFunction = int (APS5_VABI*)(const void*);

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class T, std::size_t N> void Write(std::array<std::uint8_t, N>& bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template<std::size_t N> auto Parameter(std::uint64_t userData, std::int32_t mode = 0) {
    std::array<std::uint8_t, N> bytes{};
    Write(bytes, 0, std::uint64_t{0x30});
    Write(bytes, 0x30, static_cast<std::uint32_t>(N));
    Write(bytes, 0x34, std::int32_t{1});
    Write(bytes, 0x38, mode);
    Write(bytes, 0x3c, std::uint32_t{7});
    Write(bytes, 0x58, userData);
    return bytes;
}

struct Output {
    std::array<std::uint8_t, 64> bytes;
    Output() { bytes.fill(0xa5); }
    void* Data() { return bytes.data() + 8; }
    void Unchanged() const {
        for (auto byte : bytes) Require(byte == 0xa5, "failed call changed output");
    }
    void Check(std::uint64_t userData) const {
        std::array<std::uint8_t, 64> expected;
        expected.fill(0xa5);
        std::fill(expected.begin() + 8, expected.begin() + 56, 0);
        Write(expected, 8, std::int32_t{1});
        Write(expected, 16, userData);
        Require(bytes == expected, "cancel result, authorization, user data, reserved bytes or guards differ");
    }
};

template<std::size_t N> void RoundTrip(OpenFunction open) {
    constexpr std::uint64_t cookie = 0xf123456789abcdef;
    auto parameter = Parameter<N>(cookie);
    const auto original = parameter;
    Require(sceNpCommerceDialogInitialize() == 0, "initialize");
    Require(open(parameter.data()) == 0, "open");
    Require(sceNpCommerceDialogGetStatus() == 2, "open enters running state");
    Output output;
    Require(sceNpCommerceDialogGetResult(output.Data()) == NotFinished, "no premature result");
    output.Unchanged();
    Require(sceNpCommerceDialogUpdateStatus() == 3, "offline completion");
    Require(sceNpCommerceDialogGetStatus() == 3, "completed status");
    Require(sceNpCommerceDialogGetResult(output.Data()) == 0, "get completed result");
    output.Check(cookie);
    Require(parameter == original, "input parameters modified");
    Require(sceNpCommerceDialogGetResult(output.Data()) == 0, "repeat result");
    output.Check(cookie);
    Require(sceNpCommerceDialogTerminate() == 0 && sceNpCommerceDialogGetStatus() == 0, "terminate");
}

void Validation() {
    Require(sceNpCommerceDialogOpen2(nullptr) == NotInitialized, "uninitialized error precedence");
    Require(sceNpCommerceDialogInitialize() == 0, "initialize validation");
    for (const auto open : {sceNpCommerceDialogOpen, sceNpCommerceDialogOpen2}) {
        Require(open(nullptr) == NullArgument, "null parameter");
        auto parameter = Parameter<0x88>(123);
        Write(parameter, 0, std::uint64_t{0x2f});
        Require(open(parameter.data()) == InvalidParameter, "short base parameter");
        Write(parameter, 0, std::uint64_t{0x30});
        Write(parameter, 0x30, std::uint32_t{0x7f});
        Require(open(parameter.data()) == InvalidParameter, "short parameter");
        Write(parameter, 0x30, std::uint32_t{0x88});
        Write(parameter, 0x38, std::int32_t{-1});
        Require(open(parameter.data()) == InvalidParameter, "negative mode");
        Write(parameter, 0x38, std::int32_t{6});
        Require(open(parameter.data()) == InvalidParameter, "unknown mode");
        Require(sceNpCommerceDialogGetStatus() == 1, "invalid open consumes no state");
    }
    auto shortOpen2 = Parameter<0x80>(42);
    Require(sceNpCommerceDialogOpen2(shortOpen2.data()) == InvalidParameter, "Open2 requires extended size");
    Output output;
    Require(sceNpCommerceDialogGetResult(nullptr) == NullArgument, "null result");
    Require(sceNpCommerceDialogGetResult(output.Data()) == NotFinished, "invalid opens produce no result");
    output.Unchanged();
    Require(sceNpCommerceDialogTerminate() == 0, "terminate validation");
}

void Lifecycle() {
    Require(sceNpCommerceDialogGetStatus() == 0 && sceNpCommerceDialogUpdateStatus() == 0, "initial state");
    Require(sceNpCommerceDialogTerminate() == NotInitialized, "terminate closed dialog");
    Require(sceNpCommerceDialogClose() == NotInitialized, "close before initialize");
    Require(sceNpCommerceDialogInitialize() == 0, "initialize lifecycle");
    Require(sceNpCommerceDialogInitialize() == AlreadyInitialized, "duplicate initialize");
    Require(sceNpCommerceDialogClose() == NotRunning, "close before open");
    auto parameter = Parameter<0x88>(77);
    Require(sceNpCommerceDialogOpen2(parameter.data()) == 0, "open lifecycle");
    Require(sceNpCommerceDialogOpen(nullptr) == Busy, "busy precedence");
    Require(sceNpCommerceDialogOpen2(parameter.data()) == Busy, "duplicate open");
    Require(sceNpCommerceDialogClose() == 0 && sceNpCommerceDialogGetStatus() == 3, "close cancels running dialog");
    Output output;
    Require(sceNpCommerceDialogGetResult(output.Data()) == 0, "closed result");
    output.Check(77);
    Require(sceNpCommerceDialogClose() == 0, "close completed dialog");
    Require(sceNpCommerceDialogTerminate() == 0, "terminate lifecycle");
    Output after;
    Require(sceNpCommerceDialogGetResult(after.Data()) == NotFinished, "no stale result after termination");
    after.Unchanged();
}

void Reuse() {
    Require(sceNpCommerceDialogInitialize() == 0, "initialize reuse");
    for (std::int32_t mode = 0; mode <= 5; ++mode) {
        auto parameter = Parameter<0x88>(100 + mode, mode);
        Write(parameter, 0x50, std::uint64_t{0x1234});
        Require(sceNpCommerceDialogOpen2(parameter.data()) == 0, "open each mode");
        Write(parameter, 0x58, std::uint64_t{999});
        Require(sceNpCommerceDialogUpdateStatus() == 3, "complete each mode");
        Output output;
        Require(sceNpCommerceDialogGetResult(output.Data()) == 0, "result each mode");
        output.Check(100 + mode);
    }
    auto parameter = Parameter<0x80>(987);
    Require(sceNpCommerceDialogOpen(parameter.data()) == 0, "legacy after Open2");
    Require(sceNpCommerceDialogTerminate() == 0, "terminate pending dialog");
    Require(sceNpCommerceDialogInitialize() == 0, "reinitialize");
    Output output;
    Require(sceNpCommerceDialogGetResult(output.Data()) == NotFinished, "reinitialization clears request");
    output.Unchanged();
    Require(sceNpCommerceDialogOpen(parameter.data()) == 0, "legacy reopen");
    Require(sceNpCommerceDialogUpdateStatus() == 3, "legacy completion");
    Require(sceNpCommerceDialogGetResult(output.Data()) == 0, "legacy fresh result");
    output.Check(987);
    Require(sceNpCommerceDialogTerminate() == 0, "terminate reuse");
}

void Concurrent() {
    Require(sceNpCommerceDialogInitialize() == 0, "initialize concurrent");
    constexpr int count = 12;
    std::barrier start(count);
    std::atomic<int> succeeded{0};
    std::atomic<int> rejected{0};
    std::atomic<std::uint64_t> winner{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < count; ++index) threads.emplace_back([&, index] {
        auto parameter = Parameter<0x88>(1000 + index);
        start.arrive_and_wait();
        const auto result = sceNpCommerceDialogOpen2(parameter.data());
        if (result == 0) { ++succeeded; winner = 1000 + index; }
        else if (result == Busy) ++rejected;
    });
    for (auto& thread : threads) thread.join();
    Require(succeeded == 1 && rejected == count - 1, "exactly one concurrent open succeeds");
    Require(sceNpCommerceDialogUpdateStatus() == 3, "complete winning request");
    std::atomic<bool> valid{true};
    threads.clear();
    for (int index = 0; index < count; ++index) threads.emplace_back([&] {
        try {
            for (int repeat = 0; repeat < 40; ++repeat) {
                Output output;
                Require(sceNpCommerceDialogGetResult(output.Data()) == 0, "concurrent result");
                output.Check(winner);
                Require(sceNpCommerceDialogGetStatus() == 3, "stable final status");
            }
        } catch (...) { valid = false; }
    });
    for (auto& thread : threads) thread.join();
    Require(valid, "concurrent result snapshot");
    Require(sceNpCommerceDialogTerminate() == 0, "terminate concurrent");
}

void Icons() {
    Require(sceNpCommerceShowPsStoreIcon(0) == 0 && sceNpCommerceSetPsStoreIconLayout(0) == 0 &&
        sceNpCommerceHidePsStoreIcon() == 0, "offline icon calls");
    Require(sceNpCommerceDialogGetStatus() == 0, "icons do not initialize dialogs");
    Require(sceNpCommerceDialogInitialize() == 0, "initialize icons");
    auto parameter = Parameter<0x88>(1234);
    Require(sceNpCommerceDialogOpen2(parameter.data()) == 0, "open icons");
    Require(sceNpCommerceShowPsStoreIcon(1) == 0 && sceNpCommerceSetPsStoreIconLayout(1) == 0 &&
        sceNpCommerceHidePsStoreIcon() == 0, "icons during dialog");
    Require(sceNpCommerceDialogGetStatus() == 2, "icons do not complete dialogs");
    Require(sceNpCommerceDialogUpdateStatus() == 3, "complete icons");
    Output output;
    Require(sceNpCommerceDialogGetResult(output.Data()) == 0, "icons preserve result");
    output.Check(1234);
    Require(sceNpCommerceDialogTerminate() == 0, "terminate icons");
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "expected test mode");
        const std::string mode = argv[1];
        if (mode == "legacy") RoundTrip<0x80>(sceNpCommerceDialogOpen);
        else if (mode == "open2") RoundTrip<0x88>(sceNpCommerceDialogOpen2);
        else if (mode == "validation") Validation();
        else if (mode == "lifecycle") Lifecycle();
        else if (mode == "reuse") Reuse();
        else if (mode == "concurrent") Concurrent();
        else if (mode == "icons") Icons();
        else throw std::runtime_error("unknown mode");
        std::cout << mode << ": PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
