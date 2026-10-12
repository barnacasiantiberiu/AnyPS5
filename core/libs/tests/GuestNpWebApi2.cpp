#include "SceTypes.hpp"
#include "ModuleChecks.hpp"
#include <array>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

struct NpWebApi2MemoryPoolStats {
    std::size_t pool_size;
    std::size_t max_inuse_size;
    std::size_t current_inuse_size;
    std::int32_t reserved;
};

extern "C" {
int APS5_VABI sceNpWebApi2Initialize(int, std::size_t);
int APS5_VABI sceNpWebApi2Terminate(int);
int APS5_VABI sceNpWebApi2GetMemoryPoolStats(int, NpWebApi2MemoryPoolStats*);
int APS5_VABI sceNpWebApi2PushEventCreateHandle(int);
int APS5_VABI sceNpWebApi2PushEventAbortHandle(int, int);
int APS5_VABI sceNpWebApi2PushEventDeleteHandle(int, int);
int APS5_VABI sceNpWebApi2PushEventCreateFilter(int, int, const char*, std::uint32_t, const void*, std::size_t);
}

static constexpr int LibContextNotFound = static_cast<int>(0x80553404);
static constexpr int Aborted = static_cast<int>(0x80553409);
static constexpr int HandleNotFound = static_cast<int>(0x8055340D);

using ModuleTest::Require;

template <typename TCall>
static bool Throws(TCall call) {
    try {
        call();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

static void TestPushEventAbortHandle() {
    const int context = sceNpWebApi2Initialize(1, 0x8000);
    const int other = sceNpWebApi2Initialize(1, 0x8000);
    const std::uint64_t filterParam[2]{};
    const int handle = sceNpWebApi2PushEventCreateHandle(context);
    const int kept = sceNpWebApi2PushEventCreateHandle(context);
    Require(handle > 0 && kept > 0 && handle != kept);
    Require(sceNpWebApi2PushEventCreateFilter(context, handle, nullptr, 0, filterParam, 1) > 0);
    Require(sceNpWebApi2PushEventAbortHandle(context, handle) == 0);
    Require(sceNpWebApi2PushEventCreateFilter(context, handle, nullptr, 0, filterParam, 1) == Aborted);
    Require(sceNpWebApi2PushEventAbortHandle(context, handle) == 0);
    Require(sceNpWebApi2PushEventCreateFilter(context, kept, nullptr, 0, filterParam, 1) > 0);
    Require(sceNpWebApi2PushEventAbortHandle(other, handle) == HandleNotFound);
    Require(sceNpWebApi2PushEventAbortHandle(context, 0x7FFFFFFF) == HandleNotFound);
    Require(sceNpWebApi2PushEventAbortHandle(0x7FFFFFFF, handle) == LibContextNotFound);
    Require(sceNpWebApi2PushEventCreateHandle(0x7FFFFFFF) == LibContextNotFound);
    Require(sceNpWebApi2PushEventDeleteHandle(context, handle) == 0);
    Require(sceNpWebApi2PushEventAbortHandle(context, handle) == HandleNotFound);
    Require(sceNpWebApi2PushEventDeleteHandle(context, handle) == HandleNotFound);
    Require(sceNpWebApi2Terminate(context) == 0);
    Require(sceNpWebApi2PushEventAbortHandle(context, kept) == LibContextNotFound);
    Require(sceNpWebApi2Terminate(other) == 0);
}

static void TestConcurrentHandles() {
    const int context = sceNpWebApi2Initialize(1, 0x8000);
    const int other = sceNpWebApi2Initialize(1, 0x8000);
    Require(context > 0 && other > 0 && context != other);
    const int independent = sceNpWebApi2PushEventCreateHandle(other);
    Require(independent > 0);
    std::array<std::thread, 4> workers;
    for (auto& worker : workers) {
        worker = std::thread([&] {
            for (int cycle = 0; cycle < 32; ++cycle) {
                const int handle = sceNpWebApi2PushEventCreateHandle(context);
                Require(handle > 0);
                Require(sceNpWebApi2PushEventCreateFilter(other, handle, nullptr, 0, nullptr, 0) == HandleNotFound);
                Require(sceNpWebApi2PushEventDeleteHandle(other, handle) == HandleNotFound);
                Require(sceNpWebApi2PushEventCreateFilter(context, handle, nullptr, 0, nullptr, 0) > 0);
                Require(sceNpWebApi2PushEventAbortHandle(context, handle) == 0);
                Require(sceNpWebApi2PushEventCreateFilter(context, handle, nullptr, 0, nullptr, 0) == Aborted);
                Require(sceNpWebApi2PushEventDeleteHandle(context, handle) == 0);
                Require(sceNpWebApi2PushEventCreateFilter(context, handle, nullptr, 0, nullptr, 0) == HandleNotFound);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    const int stale = sceNpWebApi2PushEventCreateHandle(context);
    Require(stale > 0);
    Require(sceNpWebApi2Terminate(context) == 0);
    Require(sceNpWebApi2PushEventCreateFilter(context, stale, nullptr, 0, nullptr, 0) == LibContextNotFound);
    Require(sceNpWebApi2PushEventDeleteHandle(context, stale) == LibContextNotFound);
    const int replacement = sceNpWebApi2Initialize(1, 0x8000);
    Require(replacement > 0 && replacement != context);
    Require(sceNpWebApi2PushEventAbortHandle(replacement, stale) == HandleNotFound);
    Require(sceNpWebApi2PushEventCreateFilter(other, independent, nullptr, 0, nullptr, 0) > 0);
    Require(sceNpWebApi2PushEventDeleteHandle(other, independent) == 0);
    Require(sceNpWebApi2Terminate(other) == 0);
    Require(sceNpWebApi2Terminate(replacement) == 0);
}

int main() {
    TestPushEventAbortHandle();
    TestConcurrentHandles();
    const int context = sceNpWebApi2Initialize(1, 0x20000);
    const int other = sceNpWebApi2Initialize(1, 0x8000);
    Require(context > 0 && other > 0 && context != other);
    NpWebApi2MemoryPoolStats stats{1, 1, 1, 1};
    Require(sceNpWebApi2GetMemoryPoolStats(context, &stats) == 0);
    Require(stats.pool_size == 0x20000 && stats.max_inuse_size == 0 && stats.current_inuse_size == 0 && stats.reserved == 0);
    Require(sceNpWebApi2GetMemoryPoolStats(other, &stats) == 0 && stats.pool_size == 0x8000);
    Require(Throws([&] { sceNpWebApi2GetMemoryPoolStats(context, nullptr); }));
    Require(sceNpWebApi2Terminate(context) == 0);
    Require(Throws([&] { sceNpWebApi2GetMemoryPoolStats(context, &stats); }));
    Require(sceNpWebApi2Terminate(other) == 0);
    return 0;
}
