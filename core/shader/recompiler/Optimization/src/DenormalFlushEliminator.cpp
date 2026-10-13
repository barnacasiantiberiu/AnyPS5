#include "Optimization/DenormalFlushEliminator.hpp"
#include "Optimization/DenormalValueProof.hpp"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ShaderRecompiler {

namespace {

bool isImmediate(const IrValue* value, std::uint32_t bits) {
    return value->HasImmediate() && value->ImmediateU32() == bits;
}

const IrValue* maskedOperand(const IrValue* value, IrOpcode opcode, std::uint32_t bits) {
    if (value->Opcode() != opcode || value->ArgumentCount() != 2u) return nullptr;
    const IrValue* lhs = value->Argument(0)->Resolve();
    const IrValue* rhs = value->Argument(1)->Resolve();
    if (isImmediate(rhs, bits)) return lhs;
    if (isImmediate(lhs, bits)) return rhs;
    return nullptr;
}

IrValue* flushedSource(const IrValue& value) {
    if (value.Opcode() != IrOpcode::SelectU32 || value.ArgumentCount() != 3u) return nullptr;
    IrValue* source = value.Argument(2)->Resolve();
    const IrValue* exponent = maskedOperand(value.Argument(0)->Resolve(), IrOpcode::IEqual32, 0u);
    if (exponent == nullptr || maskedOperand(exponent, IrOpcode::BitwiseAnd32, 0x7f800000u) != source) return nullptr;
    return maskedOperand(value.Argument(1)->Resolve(), IrOpcode::BitwiseAnd32, 0x80000000u) == source ? source : nullptr;
}

class DenormalFreeValues {
public:
    void Add(const IrValue* value) {
        indexOf(value);
    }

    void Analyze() {
        for (std::size_t index = 0; index < values.size(); ++index) classify(index, *values[index]);
        safe = proof.Solve();
    }

    bool Contains(const IrValue* value) const {
        const auto found = indices.find(value->Resolve());
        return found != indices.end() && safe[found->second] != 0u;
    }

private:
    std::size_t indexOf(const IrValue* value) {
        value = value->Resolve();
        const auto [found, inserted] = indices.try_emplace(value, values.size());
        if (inserted) {
            values.push_back(value);
            proof.AddValue();
        }
        return found->second;
    }

    void dependsOn(std::size_t index, const IrValue* value) {
        const auto dependency = indexOf(value);
        proof.AddDependency(index, dependency);
    }

    void classifyMask(std::size_t index, const IrValue& value) {
        for (std::size_t operand = 0; operand < 2u; ++operand) {
            const auto* mask = value.Argument(operand)->Resolve();
            if (!mask->HasImmediate()) continue;
            const auto bits = mask->ImmediateU32();
            const auto opcode = value.Opcode();
            if ((opcode == IrOpcode::BitwiseAnd32 && AndMaskForcesDenormalFree(bits)) ||
                (opcode == IrOpcode::BitwiseOr32 && OrMaskForcesDenormalFree(bits))) {
                proof.SetProven(index);
                return;
            }
        }
        for (std::size_t operand = 0; operand < 2u; ++operand) {
            const auto* mask = value.Argument(operand)->Resolve();
            if (!mask->HasImmediate()) continue;
            const auto bits = mask->ImmediateU32();
            const auto opcode = value.Opcode();
            if ((opcode == IrOpcode::BitwiseAnd32 && AndMaskPreservesDenormalFree(bits)) ||
                ((opcode == IrOpcode::BitwiseOr32 || opcode == IrOpcode::BitwiseXor32) && SignOnlyF32Mask(bits))) {
                dependsOn(index, value.Argument(1u - operand));
                return;
            }
        }
    }

    void classify(std::size_t index, const IrValue& value) {
        if (value.HasImmediate()) {
            if (DenormalFreeF32Bits(value.ImmediateU32())) proof.SetProven(index);
            return;
        }
        if (flushedSource(value) != nullptr) {
            proof.SetProven(index);
            return;
        }
        switch (value.Opcode()) {
            case IrOpcode::ConvertIToF32:
            case IrOpcode::ConvertUToF32:
            case IrOpcode::ConvertF32S32:
            case IrOpcode::ConvertF32U32:
            case IrOpcode::ConvertF32F16:
                proof.SetProven(index);
                return;
            case IrOpcode::BitCastU32F32:
            case IrOpcode::BitCastF32U32:
            case IrOpcode::FPAbs32:
            case IrOpcode::FPNeg32:
            case IrOpcode::FPSaturate32:
                dependsOn(index, value.Argument(0));
                return;
            case IrOpcode::BitwiseXor32:
            case IrOpcode::BitwiseOr32:
            case IrOpcode::BitwiseAnd32:
                classifyMask(index, value);
                return;
            case IrOpcode::FPMin32:
            case IrOpcode::FPMax32:
                dependsOn(index, value.Argument(0));
                dependsOn(index, value.Argument(1));
                return;
            case IrOpcode::SelectU32:
            case IrOpcode::SelectF32:
                dependsOn(index, value.Argument(1));
                dependsOn(index, value.Argument(2));
                return;
            case IrOpcode::Phi:
                for (std::size_t operand = 0; operand < value.ArgumentCount(); ++operand) dependsOn(index, value.Argument(operand));
                return;
            default:
                return;
        }
    }

    DenormalValueProof proof;
    std::unordered_map<const IrValue*, std::size_t> indices;
    std::vector<const IrValue*> values;
    std::vector<std::uint8_t> safe;
};

}

DenormalFlushEliminationStats DenormalFlushEliminator::Eliminate(IrProgram& program) const {
    DenormalFlushEliminationStats stats;
    DenormalFreeValues denormalFree;
    std::vector<IrValue*> candidates;
    for (auto* block : program.BlockOrder()) {
        for (auto* inst : block->Instructions()) {
            if (!inst->HasUses()) continue;
            IrValue* source = flushedSource(*inst);
            if (source == nullptr) continue;
            denormalFree.Add(source);
            candidates.push_back(inst);
        }
    }
    if (candidates.empty()) return stats;
    denormalFree.Analyze();
    for (auto* inst : candidates) {
        if (!inst->HasUses()) continue;
        IrValue* source = flushedSource(*inst);
        if (source == nullptr || !denormalFree.Contains(source)) continue;
        inst->ReplaceAllUsesWith(source);
        ++stats.removedFlushes;
    }
    return stats;
}

}
