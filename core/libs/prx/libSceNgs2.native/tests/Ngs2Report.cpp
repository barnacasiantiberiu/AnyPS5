#include "Ngs2Test.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <thread>

extern "C" {
int APS5_VABI sceNgs2ReportRegisterHandler(std::uint32_t, Ngs2ReportHandler, std::uintptr_t, Ngs2Handle*);
int APS5_VABI sceNgs2ReportUnregisterHandler(Ngs2Handle);
}

struct Context {
    Ngs2Handle system{}, self{}, other{}, added{};
    unsigned calls = 0;
    bool mutate = false, nested = false, destroy = false, fail = false;
};
static void Render(Ngs2Handle system) {
    std::array<float, Grain * 2> pcm;
    pcm.fill(9);
    const Ngs2RenderBufferInfo output{pcm.data(), sizeof(pcm), SCE_NGS2_WAVEFORM_TYPE_PCM_F32L, 2};
    Require(sceNgs2SystemRender(system, &output, 1) == 0);
    for (float sample : pcm) Require(sample == 0);
}
static void APS5_VABI Callback(const Ngs2ReportDataHeader* header, std::uintptr_t userData) {
    auto& context = *reinterpret_cast<Context*>(userData);
    Require(header && header->size == sizeof(Ngs2ReportCpuLoadData));
    Require(header->handle == context.system && header->type == 5 && header->result == 0);
    const auto& data = *reinterpret_cast<const Ngs2ReportCpuLoadData*>(header);
    Require(std::isfinite(data.total_ratio) && data.total_ratio >= 0);
    Require(data.process_ratio == data.total_ratio && data.flush_ratio == 0 && data.feedback_ratio == 0);
    ++context.calls;
    if (context.fail) throw std::runtime_error("test callback failure");
    if (context.nested) { context.nested = false; Render(context.system); }
    if (context.mutate) {
        context.mutate = false;
        Require(sceNgs2ReportUnregisterHandler(context.self) == 0);
        Require(sceNgs2ReportUnregisterHandler(context.other) == 0);
        Require(sceNgs2ReportRegisterHandler(5, Callback, userData, &context.added) == 0);
    }
    if (context.destroy) Require(sceNgs2SystemDestroy(context.system, nullptr) == 0);
}
static Ngs2Handle Register(Context& context) {
    Ngs2Handle handle = 0;
    Require(sceNgs2ReportRegisterHandler(5, Callback, reinterpret_cast<std::uintptr_t>(&context), &handle) == 0);
    Require(handle != 0);
    return handle;
}
template<class F> static void Throws(F call) {
    bool threw = false;
    try { call(); } catch (const std::exception&) { threw = true; }
    Require(threw);
}
int main() {
    Context first{CreateSystem()};
    first.self = Register(first);
    Context second{first.system};
    second.self = Register(second);
    Require(first.self != second.self);
    Render(first.system);
    Require(first.calls == 1 && second.calls == 1);
    first.other = second.self;
    first.mutate = true;
    first.nested = true;
    Render(first.system);
    Require(first.calls == 2 && second.calls == 1 && first.added > second.self);
    Render(first.system);
    Require(first.calls == 3 && second.calls == 1);
    Throws([&] { sceNgs2ReportUnregisterHandler(first.self); });
    Throws([&] { sceNgs2ReportUnregisterHandler(0); });
    first.fail = true;
    Throws([&] { Render(first.system); });
    first.fail = false;
    Render(first.system);
    Require(first.calls == 5);
    Require(sceNgs2ReportUnregisterHandler(first.added) == 0);
    Render(first.system);
    Require(first.calls == 5);

    std::atomic<bool> start = false;
    std::thread renderer([&] {
        while (!start.load()) std::this_thread::yield();
        for (unsigned i = 0; i < 100; ++i) Render(first.system);
    });
    start = true;
    for (unsigned i = 0; i < 100; ++i) {
        Context transient{first.system};
        const auto handle = Register(transient);
        std::this_thread::yield();
        Require(sceNgs2ReportUnregisterHandler(handle) == 0);
        const auto calls = transient.calls;
        std::this_thread::yield();
        Require(transient.calls == calls);
    }
    renderer.join();
    Ngs2Handle sentinel = 0x1234;
    Throws([&] { sceNgs2ReportRegisterHandler(5, nullptr, 0, &sentinel); });
    Throws([&] { sceNgs2ReportRegisterHandler(0xffffffff, Callback, 0, &sentinel); });
    Require(sentinel == 0x1234);
    Require(sceNgs2ReportRegisterHandler(5, Callback, 0, nullptr) == SCE_NGS2_ERROR_INVALID_OUT_ADDRESS);
    first.destroy = true;
    first.self = Register(first);
    Render(first.system);
    Require(sceNgs2ReportUnregisterHandler(first.self) == 0);
}
