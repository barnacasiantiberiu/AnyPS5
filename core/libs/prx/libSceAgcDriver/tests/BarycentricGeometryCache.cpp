#include "Recompiler.hpp"
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string_view>

using namespace ShaderRecompiler;

namespace {

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<typename TAction>
void rejects(TAction action, std::string_view diagnostic) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        check(std::string_view(error.what()).find(diagnostic) != std::string_view::npos, "unexpected rejection");
        return;
    }
    throw std::runtime_error("invalid cached request was accepted");
}

}

int main() {
    try {
        RecompileResult vertex, fragment;
        vertex.parameterExports = {0u, 1u};
        fragment.fragmentParameters = {{0u, 0u, false, true}};
        fragment.barycentricEmulation = {true, true, false};
        const std::array<std::uint32_t, 2> capabilities{spv::CapabilityShader, spv::CapabilityGeometry};
        SpirvTarget target{};
        target.vulkanVersion = 0x00401000u;
        target.spirvVersion = 0x00010300u;
        target.supportedCapabilities = capabilities;
        const GeometryStageLimits limits{64u, 128u, 256u, 1024u, 128u};
        const auto first = BuildBarycentricGeometryShader(vertex, fragment, target, limits);
        const auto reused = BuildBarycentricGeometryShader(vertex, fragment, target, limits);
        check(!first.cacheHit && reused.cacheHit, "identical geometry was regenerated");
        check(first.spirv.data() == reused.spirv.data(), "cached geometry words were copied");
        auto edited = reused;
        edited.spirv[0] = 0u;
        check(BuildBarycentricGeometryShader(vertex, fragment, target, limits).spirv.Words()[0] == spv::MagicNumber, "caller mutation corrupted the cache");

        auto changed = fragment;
        changed.fragmentParameters[0].sourceLocation = 1u;
        const auto relocated = BuildBarycentricGeometryShader(vertex, changed, target, limits);
        check(!relocated.cacheHit && relocated.spirv != first.spirv, "changed source location reused stale geometry");
        changed = fragment;
        changed.barycentricEmulation = {true, true, true};
        const auto linear = BuildBarycentricGeometryShader(vertex, changed, target, limits);
        check(!linear.cacheHit && linear.spirv != first.spirv, "changed interpolation reused stale geometry");
        changed = fragment;
        changed.fragmentParameters[0].perVertex = false;
        const auto passed = BuildBarycentricGeometryShader(vertex, changed, target, limits);
        check(!passed.cacheHit && passed.spirv != first.spirv, "changed interface reused stale geometry");
        auto newer = target;
        newer.vulkanVersion = 0x00402000u;
        newer.spirvVersion = 0x00010500u;
        const auto upgraded = BuildBarycentricGeometryShader(vertex, fragment, newer, limits);
        check(!upgraded.cacheHit && upgraded.spirv.Words()[1] == newer.spirvVersion, "changed target reused stale geometry");

        rejects([&] { static_cast<void>(BuildBarycentricGeometryShader(vertex, fragment, target, std::nullopt)); }, "unavailable");
        rejects([&] { static_cast<void>(BuildBarycentricGeometryShader(vertex, fragment, target, GeometryStageLimits{})); }, "exceeds device limits");
        auto noGeometry = target;
        noGeometry.supportedCapabilities = std::span(capabilities).first(1);
        rejects([&] { static_cast<void>(BuildBarycentricGeometryShader(vertex, fragment, noGeometry, limits)); }, "capability is unavailable");
        vertex.parameterExports.clear();
        rejects([&] { static_cast<void>(BuildBarycentricGeometryShader(vertex, fragment, target, limits)); }, "not exported");
        std::puts("Barycentric geometry cache checks passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
