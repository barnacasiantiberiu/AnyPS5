#include "SceTypes.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C" {
int APS5_VABI sceKernelInstallExceptionHandler(int, void*);
int APS5_VABI sceKernelRemoveExceptionHandler(int);
int APS5_VABI sceKernelRaiseException(Pthread, int);
int APS5_VABI scePthreadCreate(Pthread*, const PthreadAttr*, PthreadEntry, void*, const char*);
int APS5_VABI scePthreadJoin(Pthread, void**);
int APS5_VABI sceKernelSyncOnAddressWait(std::uint32_t*, std::uint32_t, const KernelUseconds*, const char*);
int APS5_VABI sceKernelSyncOnAddressWake(void*, std::int32_t);
}

static constexpr unsigned Workers = 8;
static std::array<std::uint32_t, Workers> addresses{};
static std::uint32_t parked = 0;
static std::atomic<bool> stop{false};
static std::atomic<unsigned> started{0}, acknowledged{0}, resumed{0}, generation{0};
static std::atomic<unsigned long long> operations{0};
static void Require(bool value) { if (!value) std::abort(); }

static void APS5_VABI Handler(int signal, void*) {
    Require(signal == 30);
    const auto round = generation.load();
    Require(sceKernelSyncOnAddressWake(&parked, 0x7fffffff) == 0);
    acknowledged.fetch_add(1);
    while (generation.load() == round) {
        const KernelUseconds timeout = 1000;
        const auto result = sceKernelSyncOnAddressWait(&parked, 0, &timeout, "GC resume");
        Require(result == 0 || result == static_cast<int>(0x8002003c));
    }
    resumed.fetch_add(1);
}

static void* APS5_VABI Worker(void* opaque) {
    const auto index = reinterpret_cast<std::uintptr_t>(opaque);
    started.fetch_add(1);
    while (!stop.load()) {
        Require(sceKernelSyncOnAddressWake(&addresses[(index + 1) % Workers], 0x7fffffff) == 0);
        const KernelUseconds timeout = 100;
        const auto result = sceKernelSyncOnAddressWait(&addresses[index], 0, &timeout, "worker");
        Require(result == 0 || result == static_cast<int>(0x8002003c));
        operations.fetch_add(1);
    }
    return nullptr;
}

template<class Predicate>
static void Await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "signal/address deadlock: ack=%u resumed=%u operations=%llu\n",
                         acknowledged.load(), resumed.load(), operations.load());
            std::abort();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int main() {
    Require(sceKernelInstallExceptionHandler(30, reinterpret_cast<void*>(&Handler)) == 0);
    std::array<Pthread, Workers> threads{};
    for (unsigned i = 0; i < Workers; ++i)
        Require(scePthreadCreate(&threads[i], nullptr, Worker, reinterpret_cast<void*>(static_cast<std::uintptr_t>(i)), "address signal") == 0);
    Await([] { return started.load() == Workers && operations.load() >= 100; });
    for (unsigned round = 0; round < 100; ++round) {
        for (auto thread : threads) Require(sceKernelRaiseException(thread, 30) == 0);
        Await([&] { return acknowledged.load() == (round + 1) * Workers; });
        generation.fetch_add(1);
        Require(sceKernelSyncOnAddressWake(&parked, 0x7fffffff) == 0);
        Await([&] { return resumed.load() == (round + 1) * Workers; });
    }
    stop.store(true);
    for (auto thread : threads) Require(scePthreadJoin(thread, nullptr) == 0);
    Require(sceKernelRemoveExceptionHandler(30) == 0);
    std::printf("Delivered and resumed %u signals during %llu address operations\n", acknowledged.load(), operations.load());
}
