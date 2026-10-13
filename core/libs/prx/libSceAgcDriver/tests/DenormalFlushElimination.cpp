#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/DenormalFlushEliminator.hpp"
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <initializer_list>
#include <utility>

using namespace ShaderRecompiler;

namespace {

struct Builder {
    IrProgram program;
    IrBlock* block = nullptr;

    Builder() {
        block = &newBlock();
        program.SetEntryBlock(*block);
    }

    IrBlock& newBlock() {
        auto& created = program.CreateBlock();
        program.BlockOrder().push_back(&created);
        return created;
    }

    IrValue& constant(std::uint32_t value) {
        auto& created = program.CreateValue(IrOpcode::Void, IrType::U32);
        created.SetImmediateU32(value);
        return created;
    }

    IrValue& emit(IrOpcode opcode, IrType type, std::initializer_list<IrValue*> arguments) {
        auto& created = program.CreateValue(opcode, type);
        for (auto* argument : arguments) created.AddArgument(argument);
        block->AppendInstruction(&created);
        return created;
    }

    IrValue& unknown() {
        return emit(IrOpcode::LaneId, IrType::U32, {});
    }

    IrValue& flush(IrValue& bits) {
        auto& exponent = emit(IrOpcode::BitwiseAnd32, IrType::U32, {&bits, &constant(0x7f800000u)});
        auto& denormal = emit(IrOpcode::IEqual32, IrType::Bool, {&exponent, &constant(0u)});
        auto& sign = emit(IrOpcode::BitwiseAnd32, IrType::U32, {&bits, &constant(0x80000000u)});
        return emit(IrOpcode::SelectU32, IrType::U32, {&denormal, &sign, &bits});
    }

    IrValue& phi(IrBlock& block, std::initializer_list<std::pair<IrBlock*, IrValue*>> incoming) {
        auto& created = program.CreateValue(IrOpcode::Phi, IrType::U32);
        block.AppendInstruction(&created);
        for (const auto& [predecessor, value] : incoming) created.AddPhiOperand(predecessor, value);
        return created;
    }

    void keep(IrValue& value) {
        (void)emit(IrOpcode::ReferenceU32, IrType::Void, {&value});
    }

    std::uint32_t eliminate() {
        return DenormalFlushEliminator{}.Eliminate(program).removedFlushes;
    }
};

bool expect(const char* name, bool condition) {
    if (!condition) std::fprintf(stderr, "%s\n", name);
    return condition;
}

bool run(const char* name, const std::function<bool()>& test) {
    try {
        return expect(name, test());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: %s\n", name, error.what());
        return false;
    }
}

}

int main() {
    bool passed = true;

    passed &= run("a flush of a flushed value is removed and the first flush stays", [] {
        Builder b;
        auto& first = b.flush(b.unknown());
        auto& second = b.flush(first);
        b.keep(second);
        return b.eliminate() == 1u && !second.HasUses() && first.HasUses();
    });

    passed &= run("a flush of an unknown value stays", [] {
        Builder b;
        auto& flushed = b.flush(b.unknown());
        b.keep(flushed);
        return b.eliminate() == 0u && flushed.HasUses();
    });

    passed &= run("a sign change or absolute value of a flushed value needs no flush", [] {
        Builder b;
        auto& first = b.flush(b.unknown());
        auto& negated = b.flush(b.emit(IrOpcode::BitwiseXor32, IrType::U32, {&first, &b.constant(0x80000000u)}));
        auto& absolute = b.flush(b.emit(IrOpcode::BitwiseAnd32, IrType::U32, {&b.constant(0x7fffffffu), &first}));
        b.keep(negated);
        b.keep(absolute);
        return b.eliminate() == 2u && !negated.HasUses() && !absolute.HasUses();
    });

    passed &= run("a select of flushed values needs no flush, one with an unknown arm does", [] {
        Builder b;
        auto& condition = b.emit(IrOpcode::ULessThan32, IrType::Bool, {&b.unknown(), &b.constant(16u)});
        auto& both = b.flush(b.emit(IrOpcode::SelectU32, IrType::U32, {&condition, &b.flush(b.unknown()), &b.flush(b.unknown())}));
        auto& mixed = b.flush(b.emit(IrOpcode::SelectU32, IrType::U32, {&condition, &b.flush(b.unknown()), &b.unknown()}));
        b.keep(both);
        b.keep(mixed);
        return b.eliminate() == 1u && !both.HasUses() && mixed.HasUses();
    });

    passed &= run("normal and zero constants need no flush, a denormal constant does", [] {
        Builder b;
        auto& one = b.flush(b.constant(0x3f800000u));
        auto& negativeZero = b.flush(b.constant(0x80000000u));
        auto& denormal = b.flush(b.constant(0x00000001u));
        b.keep(one);
        b.keep(negativeZero);
        b.keep(denormal);
        return b.eliminate() == 2u && !one.HasUses() && !negativeZero.HasUses() && denormal.HasUses();
    });

    passed &= run("an integer or f16 converted to f32 needs no flush", [] {
        Builder b;
        auto& integer = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(IrOpcode::ConvertF32U32, IrType::F32, {&b.unknown()})}));
        auto& half = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(IrOpcode::ConvertF32F16, IrType::F32, {&b.emit(IrOpcode::BitCastF16U16, IrType::F16, {&b.emit(IrOpcode::ConvertU16U32, IrType::U16, {&b.unknown()})})})}));
        b.keep(integer);
        b.keep(half);
        return b.eliminate() == 2u && !integer.HasUses() && !half.HasUses();
    });

    passed &= run("a clamped flushed value needs no flush, an arithmetic result does", [] {
        Builder b;
        auto& first = b.emit(IrOpcode::BitCastF32U32, IrType::F32, {&b.flush(b.unknown())});
        auto& clamped = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(IrOpcode::FPSaturate32, IrType::F32, {&first})}));
        auto& product = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(IrOpcode::FPMul32, IrType::F32, {&first, &first})}));
        b.keep(clamped);
        b.keep(product);
        return b.eliminate() == 1u && !clamped.HasUses() && product.HasUses();
    });

    passed &= run("a phi of flushed values needs no flush", [] {
        Builder b;
        IrBlock& head = *b.block;
        IrBlock& body = b.newBlock();
        IrBlock& merge = b.newBlock();
        head.AddBranch(&body);
        head.AddBranch(&merge);
        body.AddBranch(&merge);
        auto& fromHead = b.flush(b.unknown());
        b.block = &body;
        auto& fromBody = b.flush(b.unknown());
        b.block = &merge;
        auto& merged = b.flush(b.phi(merge, {{&head, &fromHead}, {&body, &fromBody}}));
        b.keep(merged);
        return b.eliminate() == 1u && !merged.HasUses();
    });

    passed &= run("a loop phi needs no flush when every value it takes is flushed, and keeps it otherwise", [] {
        const auto loop = [](bool flushedBackEdge) {
            Builder b;
            IrBlock& head = *b.block;
            IrBlock& body = b.newBlock();
            IrBlock& exit = b.newBlock();
            head.AddBranch(&body);
            body.AddBranch(&body);
            body.AddBranch(&exit);
            auto& initial = b.flush(b.unknown());
            b.block = &body;
            auto& carried = b.program.CreateValue(IrOpcode::Phi, IrType::U32);
            body.AppendInstruction(&carried);
            auto& next = b.flush(carried);
            auto& backEdge = flushedBackEdge ? next : b.emit(IrOpcode::BitwiseXor32, IrType::U32, {&b.unknown(), &next});
            carried.AddPhiOperand(&head, &initial);
            carried.AddPhiOperand(&body, &backEdge);
            b.block = &exit;
            b.keep(next);
            b.eliminate();
            return !next.HasUses();
        };
        return loop(true) && !loop(false);
    });

    passed &= run("min and max propagate the denormal-free property of both operands", [] {
        Builder b;
        for (auto opcode : {IrOpcode::FPMin32, IrOpcode::FPMax32}) {
            auto& first = b.emit(IrOpcode::BitCastF32U32, IrType::F32, {&b.flush(b.unknown())});
            auto& second = b.emit(IrOpcode::BitCastF32U32, IrType::F32, {&b.flush(b.unknown())});
            auto& unknown = b.emit(IrOpcode::BitCastF32U32, IrType::F32, {&b.unknown()});
            auto& safe = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(opcode, IrType::F32, {&first, &second})}));
            auto& unsafe = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(opcode, IrType::F32, {&first, &unknown})}));
            b.keep(safe);
            b.keep(unsafe);
            if (b.eliminate() != 1u || safe.HasUses() || !unsafe.HasUses()) return false;
        }
        return true;
    });

    passed &= run("known exponent and mantissa masks remove only redundant flushes", [] {
        Builder b;
        for (bool swapped : {false, true}) {
            for (auto mask : {0u, 0x80000000u, 0xff800000u}) {
                auto& value = b.unknown();
                auto& constant = b.constant(mask);
                auto& bits = b.emit(IrOpcode::BitwiseAnd32, IrType::U32, swapped ? std::initializer_list<IrValue*>{&constant, &value} : std::initializer_list<IrValue*>{&value, &constant});
                b.keep(b.flush(bits));
            }
            for (auto mask : {0x00800000u, 0x3f800000u, 0x7f800000u}) {
                auto& value = b.unknown();
                auto& constant = b.constant(mask);
                auto& bits = b.emit(IrOpcode::BitwiseOr32, IrType::U32, swapped ? std::initializer_list<IrValue*>{&constant, &value} : std::initializer_list<IrValue*>{&value, &constant});
                b.keep(b.flush(bits));
            }
        }
        auto& mantissa = b.flush(b.emit(IrOpcode::BitwiseAnd32, IrType::U32, {&b.unknown(), &b.constant(0x007fffffu)}));
        auto& quietBit = b.flush(b.emit(IrOpcode::BitwiseOr32, IrType::U32, {&b.unknown(), &b.constant(0x00400000u)}));
        b.keep(mantissa);
        b.keep(quietBit);
        return b.eliminate() == 12u && mantissa.HasUses() && quietBit.HasUses();
    });

    passed &= run("a sign-preserving loop proves its invariant without flushing the back edge", [] {
        for (bool safeEntry : {false, true}) {
            Builder b;
            IrBlock& head = *b.block;
            IrBlock& body = b.newBlock();
            IrBlock& exit = b.newBlock();
            head.AddBranch(&body);
            body.AddBranch(&body);
            body.AddBranch(&exit);
            auto& initial = safeEntry ? b.constant(0x3f800000u) : b.unknown();
            b.block = &body;
            auto& carried = b.phi(body, {{&head, &initial}});
            auto& next = b.emit(IrOpcode::BitwiseXor32, IrType::U32, {&carried, &b.constant(0x80000000u)});
            carried.AddPhiOperand(&body, &next);
            b.block = &exit;
            auto& result = b.flush(carried);
            b.keep(result);
            if (b.eliminate() != (safeEntry ? 1u : 0u) || result.HasUses() == safeEntry) return false;
        }
        return true;
    });

    passed &= run("an arithmetic back edge invalidates an otherwise grounded loop proof", [] {
        Builder b;
        IrBlock& head = *b.block;
        IrBlock& body = b.newBlock();
        IrBlock& exit = b.newBlock();
        head.AddBranch(&body);
        body.AddBranch(&body);
        body.AddBranch(&exit);
        auto& initial = b.constant(0x00800000u);
        b.block = &body;
        auto& carried = b.phi(body, {{&head, &initial}});
        auto& floating = b.emit(IrOpcode::BitCastF32U32, IrType::F32, {&carried});
        auto& product = b.emit(IrOpcode::FPMul32, IrType::F32, {&floating, &floating});
        auto& next = b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&product});
        carried.AddPhiOperand(&body, &next);
        b.block = &exit;
        auto& result = b.flush(carried);
        b.keep(result);
        return b.eliminate() == 0u && result.HasUses();
    });

    passed &= run("a cyclic select with an unknown arm retains its flush", [] {
        Builder b;
        IrBlock& head = *b.block;
        IrBlock& body = b.newBlock();
        IrBlock& exit = b.newBlock();
        head.AddBranch(&body);
        body.AddBranch(&body);
        body.AddBranch(&exit);
        auto& initial = b.constant(0x3f800000u);
        b.block = &body;
        auto& carried = b.phi(body, {{&head, &initial}});
        auto& condition = b.emit(IrOpcode::ULessThan32, IrType::Bool, {&b.unknown(), &b.constant(16u)});
        auto& next = b.emit(IrOpcode::SelectU32, IrType::U32, {&condition, &carried, &b.unknown()});
        carried.AddPhiOperand(&body, &next);
        b.block = &exit;
        auto& result = b.flush(carried);
        b.keep(result);
        return b.eliminate() == 0u && result.HasUses();
    });

    passed &= run("unanchored and empty phis cannot prove a value denormal-free", [] {
        Builder b;
        auto& empty = b.phi(*b.block, {});
        auto& cycle = b.phi(*b.block, {});
        cycle.AddPhiOperand(b.block, &cycle);
        auto& condition = b.emit(IrOpcode::ULessThan32, IrType::Bool, {&b.unknown(), &b.constant(16u)});
        auto& mixed = b.emit(IrOpcode::SelectU32, IrType::U32, {&condition, &cycle, &b.constant(0x3f800000u)});
        auto& first = b.flush(empty);
        auto& second = b.flush(cycle);
        auto& third = b.flush(mixed);
        b.keep(first);
        b.keep(second);
        b.keep(third);
        return b.eliminate() == 0u && first.HasUses() && second.HasUses() && third.HasUses();
    });

    passed &= run("exponent-preserving masks and sign-only masks propagate existing proofs", [] {
        for (bool swapped : {false, true}) {
            for (auto opcode : {IrOpcode::BitwiseAnd32, IrOpcode::BitwiseOr32, IrOpcode::BitwiseXor32}) {
                for (auto mask : {0u, 0x80000000u, 0xffff0000u, 0x7fffff00u, 0xff800001u}) {
                    const bool supported = opcode == IrOpcode::BitwiseAnd32 ? (mask & 0x7f800000u) == 0x7f800000u : (mask & 0x7fffffffu) == 0u;
                    if (!supported) continue;
                    Builder b;
                    auto& first = b.flush(b.unknown());
                    auto& constant = b.constant(mask);
                    auto& bits = b.emit(opcode, IrType::U32, swapped ? std::initializer_list<IrValue*>{&constant, &first} : std::initializer_list<IrValue*>{&first, &constant});
                    auto& result = b.flush(bits);
                    b.keep(result);
                    if (b.eliminate() != 1u || result.HasUses() || !first.HasUses()) return false;
                }
            }
        }
        return true;
    });

    passed &= run("special f32 bit patterns keep their existing flush semantics", [] {
        for (auto bits : {0u, 0x80000000u, 1u, 0x80000001u, 0x007fffffu, 0x807fffffu, 0x00800000u, 0x80800000u, 0x7f800000u, 0xff800000u, 0x7fc00001u, 0x7f800001u}) {
            Builder b;
            auto& result = b.flush(b.constant(bits));
            b.keep(result);
            const bool safe = (bits & 0x7f800000u) != 0u || (bits & 0x7fffffffu) == 0u;
            if (b.eliminate() != (safe ? 1u : 0u) || result.HasUses() == safe) return false;
        }
        return true;
    });

    passed &= run("nested replacements do not resurrect a previously eliminated flush", [] {
        Builder b;
        auto& first = b.flush(b.constant(0x3f800000u));
        auto& second = b.flush(first);
        b.keep(second);
        return b.eliminate() == 2u && !first.HasUses() && !second.HasUses() && b.eliminate() == 0u;
    });

    passed &= run("long value chains are analyzed without recursive stack growth", [] {
        Builder b;
        IrValue* value = &b.constant(0x3f800000u);
        auto& sign = b.constant(0x80000000u);
        for (std::size_t index = 0; index < 8192u; ++index) value = &b.emit(IrOpcode::BitwiseXor32, IrType::U32, {value, &sign});
        auto& result = b.flush(*value);
        b.keep(result);
        return b.eliminate() == 1u && !result.HasUses();
    });

    return passed ? 0 : 1;
}
