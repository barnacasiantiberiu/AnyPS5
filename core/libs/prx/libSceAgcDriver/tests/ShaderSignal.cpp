#include "SceTypes.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderPreparation.hpp"
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

namespace {
using AgcDriver::DriverDetail::ShaderPreparationTransaction;
std::atomic<unsigned> releaseStage{0}, workerStage{0}, handled{0};
std::atomic<bool> inside{false};
bool commit = false;
void Require(bool value) { if (!value) std::abort(); }
template<class Predicate> void Await(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
        Require(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
void APS5_VABI Handler(int signal, void*) {
    Require(signal == 30 && !inside.load());
    ShaderPreparationTransaction reentry;
    reentry.Hold();
    reentry.Commit();
    handled.fetch_add(1);
}
void* APS5_VABI Worker(void*) {
    {
        ShaderPreparationTransaction outer;
        outer.Hold();
        inside.store(true);
        {
            ShaderPreparationTransaction inner;
            inner.Hold();
            workerStage.store(1);
            Await([] { return releaseStage.load() >= 1; });
            if (commit) inner.Commit();
        }
        workerStage.store(2);
        Await([] { return releaseStage.load() >= 2; });
        if (commit) outer.Commit();
        inside.store(false);
    }
    Await([] { return handled.load() == 1; });
    return nullptr;
}
}

int main() {
    Require(sceKernelInstallExceptionHandler(30, reinterpret_cast<void*>(&Handler)) == 0);
    for (const bool publish : {false, true}) {
        commit = publish;
        releaseStage.store(0);
        workerStage.store(0);
        handled.store(0);
        Pthread thread{};
        Require(scePthreadCreate(&thread, nullptr, Worker, nullptr, "shader signal") == 0);
        Await([] { return workerStage.load() == 1; });
        Require(sceKernelRaiseException(thread, 30) == 0);
        Require(handled.load() == 0);
        releaseStage.store(1);
        Await([] { return workerStage.load() == 2; });
        Require(handled.load() == 0);
        releaseStage.store(2);
        Require(scePthreadJoin(thread, nullptr) == 0);
        Require(handled.load() == 1);
    }
    Require(sceKernelRemoveExceptionHandler(30) == 0);
}
