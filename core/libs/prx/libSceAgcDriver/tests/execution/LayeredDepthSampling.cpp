#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "DepthFastClearHarness.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
namespace {
using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;
constexpr std::uint32_t Width = 64, Height = 64, Layers = 6, Threads = 32;
constexpr std::size_t LayerBytes = 65536, SurfaceBytes = LayerBytes * Layers;
alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};
alignas(256) std::array<std::uint32_t, Threads> Output{};
alignas(256) std::array<std::array<std::uint32_t, 9>, Layers> LoadCodes{};
class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(SurfaceBytes, LayerBytes));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, SurfaceBytes, PAGE_READWRITE, SurfaceBytes);
#else
        void* mapped = mmap(nullptr, SurfaceBytes + LayerBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + LayerBytes - 1) & ~(static_cast<std::uintptr_t>(LayerBytes) - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + SurfaceBytes != begin + SurfaceBytes + LayerBytes) munmap(reinterpret_cast<void*>(aligned + SurfaceBytes), begin + LayerBytes - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            if (AgcDriver::GuestMemory::WriteWatched()) GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, SurfaceBytes);
        }
#endif
        Require(block != nullptr, "array layer target: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, SurfaceBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, SurfaceBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, SurfaceBytes);
#else
        munmap(block, SurfaceBytes);
        if (AgcDriver::GuestMemory::WriteWatched()) GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, SurfaceBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
};

void Draw(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& compiled, std::uint64_t address, VkFormat format, float clear, bool write) {
    Pixels.fill(std::byte{0});
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &compiled.vertex, 0},
        {ShaderStage::Fragment, &compiled.pixel, PixelPushOffset(compiled.vertexPush, device.Target())}
    }};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.depth = AgcDriver::Graphics::DepthTarget{address, 0, {Width, Height}, format, clear, 0};
    state.depthTest = true;
    state.depthWrite = write;
    state.depthCompare = VK_COMPARE_OP_ALWAYS;
    state.stencilTest = false;
    state.stencilFront = VkStencilOpState{};
    state.stencilBack = VkStencilOpState{};
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(DepthFastClearHarness::Triangles().size()), 0, 1, 0, false};
    device.Draw(state, draw, shaders);
    device.WaitIdle();
}

void Read(AgcDriver::VulkanDevice& device, const void* texels, std::uint32_t type, std::uint32_t format, std::uint32_t layer, float expected) {
    auto& code = LoadCodes[layer];
    code = {0x7e020280, 0x7e040280u | layer, 0xf0001128, 0x00010300, 0x34080082, 0xbf8c3f70, 0xe0701000, 0x80000304, 0xbf810000};
    const auto address = reinterpret_cast<std::uintptr_t>(texels);
    const auto output = reinterpret_cast<std::uintptr_t>(Output.data());
    const std::array<std::uint32_t, 12> data{
        static_cast<std::uint32_t>(output), static_cast<std::uint32_t>(output >> 32u), sizeof(Output), 0x31016facu,
        static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u) | (format << 20u) | (((Width - 1u) & 3u) << 30u),
        ((Width - 1u) >> 2u) | ((Height - 1u) << 14u), 0xfacu | (0x18u << 20u) | (type << 28u), Layers - 1u, 0u, 0u, 0u,
    };
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, data, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto shader = ShaderRecompiler::Recompile(request);
    Output.fill(0xdeadbeefu);
    device.Dispatch(shader, 1, 1, 1);
    device.WaitIdle();
    for (const auto bits : Output) Require(std::abs(std::bit_cast<float>(bits) - expected) < 0.00002f, "depth layer " + std::to_string(layer) + " read " + std::to_string(std::bit_cast<float>(bits)) + " instead of " + std::to_string(expected));
}
}
int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock block;
        const auto shaders = DepthFastClearHarness::Compile(*device);
        for (auto format : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
            AgcDriver::Graphics::ClearDepthSurfaces(device->Device());
            for (std::uint32_t face = 0; face < Layers; ++face)
                Draw(*device, shaders, reinterpret_cast<std::uintptr_t>(block.Data()) + face * LayerBytes, format, (face + 1) * 0.125f, false);
            for (auto type : {11u, 13u}) {
                for (std::uint32_t face = 0; face < Layers; ++face)
                    Read(*device, block.Data(), type, format == VK_FORMAT_D32_SFLOAT ? 22u : 7u, face, (face + 1) * 0.125f);
            }
            Draw(*device, shaders, reinterpret_cast<std::uintptr_t>(block.Data()), format, 0.0f, true);
            for (auto type : {11u, 13u}) Read(*device, block.Data(), type, format == VK_FORMAT_D32_SFLOAT ? 22u : 7u, 0, 0.5f);
        }
        std::puts("layered depth sampling GPU test passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
