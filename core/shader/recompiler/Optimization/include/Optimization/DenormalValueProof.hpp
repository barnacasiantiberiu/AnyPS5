#ifndef CORE_SHADER_RECOMPILER_OPTIMIZATION_DENORMALVALUEPROOF_HPP
#define CORE_SHADER_RECOMPILER_OPTIMIZATION_DENORMALVALUEPROOF_HPP

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace ShaderRecompiler {

inline constexpr std::uint32_t F32ExponentMask = 0x7f800000u;
inline constexpr std::uint32_t F32MantissaMask = 0x007fffffu;

constexpr bool DenormalFreeF32Bits(std::uint32_t bits) {
    return (bits & F32ExponentMask) != 0u || (bits & 0x7fffffffu) == 0u;
}

constexpr bool AndMaskForcesDenormalFree(std::uint32_t mask) {
    return (mask & F32MantissaMask) == 0u;
}

constexpr bool AndMaskPreservesDenormalFree(std::uint32_t mask) {
    return (mask & F32ExponentMask) == F32ExponentMask;
}

constexpr bool OrMaskForcesDenormalFree(std::uint32_t mask) {
    return (mask & F32ExponentMask) != 0u;
}

constexpr bool SignOnlyF32Mask(std::uint32_t mask) {
    return (mask & 0x7fffffffu) == 0u;
}

class DenormalValueProof {
public:
    std::size_t AddValue() {
        const auto index = nodes.size();
        nodes.emplace_back();
        return index;
    }

    void SetProven(std::size_t index) {
        requireIndex(index);
        if (!nodes[index].dependencies.empty()) throw std::runtime_error("denormal proof: a proven value cannot depend on another value");
        nodes[index].proven = true;
    }

    void AddDependency(std::size_t index, std::size_t dependency) {
        requireIndex(index);
        requireIndex(dependency);
        if (nodes[index].proven) throw std::runtime_error("denormal proof: a proven value cannot acquire dependencies");
        nodes[index].dependencies.push_back(dependency);
    }

    std::vector<std::uint8_t> Solve() const {
        std::vector<std::vector<std::size_t>> users(nodes.size());
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            for (const auto dependency : nodes[index].dependencies) users[dependency].push_back(index);
        }
        std::vector<std::uint8_t> grounded(nodes.size(), 0u);
        std::vector<std::size_t> pending;
        pending.reserve(nodes.size());
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            if (nodes[index].proven) {
                grounded[index] = 1u;
                pending.push_back(index);
            }
        }
        for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
            for (const auto user : users[pending[cursor]]) {
                if (grounded[user] != 0u) continue;
                grounded[user] = 1u;
                pending.push_back(user);
            }
        }
        auto safe = grounded;
        pending.clear();
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            if (safe[index] == 0u) pending.push_back(index);
        }
        for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
            for (const auto user : users[pending[cursor]]) {
                if (safe[user] == 0u) continue;
                safe[user] = 0u;
                pending.push_back(user);
            }
        }
        return safe;
    }

private:
    struct Node {
        bool proven = false;
        std::vector<std::size_t> dependencies;
    };

    void requireIndex(std::size_t index) const {
        if (index >= nodes.size()) throw std::runtime_error("denormal proof: invalid value index");
    }

    std::vector<Node> nodes;
};

}

#endif
