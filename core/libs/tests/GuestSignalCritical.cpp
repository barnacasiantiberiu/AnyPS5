#include "SceTypes.hpp"
#include "prx/libkernel/Time/include/SignalMutex.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

extern "C" {
int APS5_VABI sceKernelInstallExceptionHandler(int, void*);
int APS5_VABI sceKernelRemoveExceptionHandler(int);
int APS5_VABI sceKernelRaiseException(Pthread, int);
int APS5_VABI scePthreadCreate(Pthread*, const PthreadAttr*, PthreadEntry, void*, const char*);
int APS5_VABI scePthreadJoin(Pthread, void**);
}

static TimedWait::SignalMutex outer, inner;
static std::atomic<unsigned> releaseStage{0}, workerStage{0}, handled{0};
static void Require(bool value) { if (!value) std::abort(); }
template<class Predicate> static void Await(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
        Require(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
static void APS5_VABI Handler(int signum, void*) {
    Require(signum == 30);
    std::lock_guard first(outer);
    std::lock_guard second(inner);
    handled.fetch_add(1);
}
static void* APS5_VABI Worker(void*) {
    std::unique_lock first(outer);
    {
        std::lock_guard second(inner);
        workerStage.store(1);
        Await([] { return releaseStage.load() >= 1; });
    }
    workerStage.store(2);
    Await([] { return releaseStage.load() >= 2; });
    first.unlock();
    Await([] { return handled.load() == 1; });
    return nullptr;
}
int main() {
    Require(sceKernelInstallExceptionHandler(30, reinterpret_cast<void*>(&Handler)) == 0);
    Pthread thread{};
    Require(scePthreadCreate(&thread, nullptr, Worker, nullptr, "critical signal") == 0);
    Await([] { return workerStage.load() == 1; });
    Require(sceKernelRaiseException(thread, 30) == 0);
    Require(handled.load() == 0);
    releaseStage.store(1);
    Await([] { return workerStage.load() == 2; });
    Require(handled.load() == 0);
    releaseStage.store(2);
    Require(scePthreadJoin(thread, nullptr) == 0);
    Require(handled.load() == 1);
    Require(sceKernelRemoveExceptionHandler(30) == 0);
}
