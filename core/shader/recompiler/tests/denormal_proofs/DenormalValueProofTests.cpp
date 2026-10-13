#include "Optimization/DenormalValueProof.hpp"
#include <array>
#include <cstdio>
#include <exception>
#include <random>
#include <stdexcept>
#include <vector>

using namespace ShaderRecompiler;

namespace {

void require(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
}

void cycles() {
    DenormalValueProof proof;
    for (std::size_t index = 0; index < 10u; ++index) proof.AddValue();
    proof.SetProven(0u);
    proof.AddDependency(1u, 0u);
    proof.AddDependency(1u, 2u);
    proof.AddDependency(2u, 1u);
    proof.AddDependency(3u, 3u);
    proof.AddDependency(4u, 0u);
    proof.AddDependency(4u, 3u);
    proof.AddDependency(5u, 0u);
    proof.AddDependency(5u, 6u);
    proof.AddDependency(6u, 5u);
    proof.AddDependency(6u, 7u);
    proof.AddDependency(9u, 0u);
    proof.AddDependency(9u, 8u);
    const auto safe = proof.Solve();
    require(safe == std::vector<std::uint8_t>{1u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}, "anchored, unanchored and poisoned cycles");
}

void deepGraph() {
    constexpr std::size_t count = 100000u;
    DenormalValueProof proof;
    for (std::size_t index = 0; index < count; ++index) proof.AddValue();
    proof.SetProven(0u);
    for (std::size_t index = 1; index < count; ++index) proof.AddDependency(index, index - 1u);
    proof.AddDependency(1u, count - 1u);
    const auto safe = proof.Solve();
    for (const auto value : safe) require(value != 0u, "deep anchored graph must not recurse or lose facts");
}

bool referenceProof(std::size_t root, const std::vector<bool>& proven, const std::vector<std::vector<std::size_t>>& dependencies) {
    const auto reachesSeed = [&](std::size_t start) {
        std::vector<bool> seen(proven.size(), false);
        std::vector<std::size_t> pending{start};
        seen[start] = true;
        for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
            const auto index = pending[cursor];
            if (proven[index]) return true;
            for (const auto dependency : dependencies[index]) {
                if (seen[dependency]) continue;
                seen[dependency] = true;
                pending.push_back(dependency);
            }
        }
        return false;
    };
    std::vector<bool> seen(proven.size(), false);
    std::vector<std::size_t> pending{root};
    seen[root] = true;
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto index = pending[cursor];
        if (!reachesSeed(index)) return false;
        for (const auto dependency : dependencies[index]) {
            if (seen[dependency]) continue;
            seen[dependency] = true;
            pending.push_back(dependency);
        }
    }
    return true;
}

void randomGraphs() {
    std::mt19937 random(0x41505335u);
    for (std::size_t trial = 0; trial < 10000u; ++trial) {
        const std::size_t count = 1u + random() % 12u;
        DenormalValueProof proof;
        std::vector<bool> proven(count, false);
        std::vector<std::vector<std::size_t>> dependencies(count);
        for (std::size_t index = 0; index < count; ++index) proof.AddValue();
        for (std::size_t index = 0; index < count; ++index) {
            if (random() % 4u == 0u) {
                proof.SetProven(index);
                proven[index] = true;
                continue;
            }
            const auto operands = random() % 4u;
            for (std::size_t operand = 0; operand < operands; ++operand) {
                const auto dependency = random() % count;
                dependencies[index].push_back(dependency);
                proof.AddDependency(index, dependency);
            }
        }
        const auto safe = proof.Solve();
        for (std::size_t index = 0; index < count; ++index) {
            require((safe[index] != 0u) == referenceProof(index, proven, dependencies), "random graph differs from reference traversal");
        }
    }
}

void bitMasks() {
    constexpr std::array<std::uint32_t, 14> values{
        0u, 0x80000000u, 1u, 0x80000001u, 0x007fffffu, 0x807fffffu, 0x00800000u,
        0x80800000u, 0x3f800000u, 0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00001u, 0x7f800001u};
    constexpr std::array<std::uint32_t, 13> masks{
        0u, 0xffffffffu, 0x80000000u, 0x7fffffffu, 0x7f800000u, 0xff800000u, 0xffff0000u,
        0x7fff0000u, 0x007fffffu, 0x00400000u, 0x00800000u, 1u, 0x3f800000u};
    const auto verify = [](std::uint32_t value, std::uint32_t mask) {
        if (AndMaskForcesDenormalFree(mask) || (DenormalFreeF32Bits(value) && AndMaskPreservesDenormalFree(mask))) {
            require(DenormalFreeF32Bits(value & mask), "unsafe AND-mask proof");
        }
        if (OrMaskForcesDenormalFree(mask) || (DenormalFreeF32Bits(value) && SignOnlyF32Mask(mask))) {
            require(DenormalFreeF32Bits(value | mask), "unsafe OR-mask proof");
        }
        if (DenormalFreeF32Bits(value) && SignOnlyF32Mask(mask)) require(DenormalFreeF32Bits(value ^ mask), "unsafe XOR-mask proof");
    };
    for (const auto value : values) for (const auto mask : masks) verify(value, mask);
    std::mt19937 random(0x44454e4fu);
    for (std::size_t index = 0; index < 1000000u; ++index) {
        const auto value = static_cast<std::uint32_t>(random());
        const auto mask = static_cast<std::uint32_t>(random());
        verify(value, mask);
        verify(value, mask | F32ExponentMask);
        verify(value, mask & ~F32MantissaMask);
        verify(value, mask & 0x80000000u);
    }
    require(!DenormalFreeF32Bits(1u) && !DenormalFreeF32Bits(0x80000001u), "subnormals are not safe leaves");
    require(!AndMaskPreservesDenormalFree(0x007fffffu), "a mantissa-only mask does not preserve safety");
    require(!OrMaskForcesDenormalFree(0x00400000u), "the quiet-NaN bit alone does not force a nonzero exponent");
}

void invalidGraphs() {
    DenormalValueProof proof;
    require(proof.Solve().empty(), "empty graph");
    proof.AddValue();
    require(proof.Solve() == std::vector<std::uint8_t>{0u}, "empty dependencies are unknown, not a proof");
    bool rejected = false;
    try { proof.AddDependency(0u, 1u); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "out-of-range dependency must be rejected");
    proof.SetProven(0u);
    rejected = false;
    try { proof.AddDependency(0u, 0u); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "proven leaves cannot acquire dependencies");
}

}

int main() {
    try {
        cycles();
        deepGraph();
        randomGraphs();
        bitMasks();
        invalidGraphs();
        std::puts("PASS: cyclic proofs, 100000-node graph, 10000 random graphs, 4000182 bit-mask checks, invalid inputs");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
