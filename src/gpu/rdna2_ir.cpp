#include "scarlettx/gpu/rdna2_ir.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace scarlettx {

namespace {

uint32_t FloatToBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

float BitsToFloat(uint32_t bits) {
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

bool IsBranchOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::S_BRANCH:
        case Rdna2Opcode::S_CBRANCH_SCC0:
        case Rdna2Opcode::S_CBRANCH_SCC1:
        case Rdna2Opcode::S_CBRANCH_VCCZ:
        case Rdna2Opcode::S_CBRANCH_VCCNZ:
        case Rdna2Opcode::S_CBRANCH_EXECZ:
        case Rdna2Opcode::S_CBRANCH_EXECNZ:
            return true;
        default:
            return false;
    }
}

bool IsConditionalBranchOpcode(Rdna2Opcode op) {
    return IsBranchOpcode(op) && op != Rdna2Opcode::S_BRANCH;
}

void AddUnique(std::vector<uint32_t>& vec, uint32_t val) {
    if (std::find(vec.begin(), vec.end(), val) == vec.end()) {
        vec.push_back(val);
    }
}

struct RegState {
    std::array<IrValueId, 256> vgpr{};
    std::array<IrValueId, 106> sgpr{};
    IrValueId scc = kInvalidIrValue;
    IrValueId vcc = kInvalidIrValue;
};

}  // namespace

const IrInstruction* Rdna2IrModule::FindDef(IrValueId id) const noexcept {
    if (id == kInvalidIrValue) return nullptr;
    for (const auto& bb : blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.id == id) return &inst;
        }
    }
    return nullptr;
}

const char* IrOpName(IrOp op) noexcept {
    switch (op) {
        case IrOp::ConstU32:      return "const.u32";
        case IrOp::ConstF32:      return "const.f32";
        case IrOp::ThreadIdX:     return "thread_id.x";
        case IrOp::StageInput:    return "stage_input";
        case IrOp::LoadRootConst: return "load_root_const";
        case IrOp::LoadUav:       return "load_uav";
        case IrOp::StoreUav:      return "store_uav";
        case IrOp::FAdd:          return "fadd";
        case IrOp::FSub:          return "fsub";
        case IrOp::FMul:          return "fmul";
        case IrOp::FFma:          return "ffma";
        case IrOp::FMin:          return "fmin";
        case IrOp::FMax:          return "fmax";
        case IrOp::FRcp:          return "frcp";
        case IrOp::FRsq:          return "frsq";
        case IrOp::FSqrt:         return "fsqrt";
        case IrOp::FFloor:        return "ffloor";
        case IrOp::FFract:        return "ffract";
        case IrOp::FDot2:         return "fdot2";
        case IrOp::IAdd:          return "iadd";
        case IrOp::ISub:          return "isub";
        case IrOp::IMul:          return "imul";
        case IrOp::IMin:          return "imin";
        case IrOp::IMax:          return "imax";
        case IrOp::BitAnd:        return "and";
        case IrOp::BitOr:         return "or";
        case IrOp::BitXor:        return "xor";
        case IrOp::BitNot:        return "not";
        case IrOp::Shl:           return "shl";
        case IrOp::Shr:           return "shr";
        case IrOp::I2F:           return "i2f";
        case IrOp::F2I:           return "f2i";
        case IrOp::U2F:           return "u2f";
        case IrOp::F2U:           return "f2u";
        case IrOp::CmpEq:         return "cmp.eq";
        case IrOp::CmpNe:         return "cmp.ne";
        case IrOp::CmpLt:         return "cmp.lt";
        case IrOp::CmpGt:         return "cmp.gt";
        case IrOp::CmpGe:         return "cmp.ge";
        case IrOp::CmpLtU:        return "cmp.ltu";
        case IrOp::CmpEqU:        return "cmp.equ";
        case IrOp::CmpGtU:        return "cmp.gtu";
        case IrOp::CmpGeU:        return "cmp.geu";
        case IrOp::Select:        return "select";
        case IrOp::Phi:           return "phi";
        case IrOp::Barrier:       return "barrier";
        case IrOp::Branch:        return "br";
        case IrOp::CondBranch:    return "cbr";
        case IrOp::Export:        return "export";
        case IrOp::Return:        return "ret";
        default:                  return "unknown";
    }
}

const char* IrTypeName(IrType type) noexcept {
    switch (type) {
        case IrType::Void: return "void";
        case IrType::U32:  return "u32";
        case IrType::I32:  return "i32";
        case IrType::F32:  return "f32";
        case IrType::Bool: return "bool";
        default:           return "?";
    }
}

bool IrOpHasSideEffects(IrOp op) noexcept {
    switch (op) {
        case IrOp::StoreUav:
        case IrOp::Barrier:
        case IrOp::Branch:
        case IrOp::CondBranch:
        case IrOp::Export:
        case IrOp::Return:
            return true;
        default:
            return false;
    }
}

Status BuildRdna2Ir(
    const Xs2ShaderContainer& container,
    const std::vector<Rdna2Instruction>& instructions,
    Rdna2IrModule& out_module) {
    out_module = Rdna2IrModule{};
    out_module.stage = container.stage;
    out_module.threadgroup_x = container.threadgroup_x;
    out_module.threadgroup_y = container.threadgroup_y;
    out_module.threadgroup_z = container.threadgroup_z;
    out_module.sgpr_count = container.sgpr_count;
    out_module.vgpr_count = container.vgpr_count;

    if (instructions.empty()) {
        return Status::Error(StatusCode::ParseError, "Cannot build IR from empty instruction stream");
    }

    // 1. Map word offsets -> instruction indices & identify basic block leaders.
    std::unordered_map<uint32_t, size_t> word_to_idx;
    word_to_idx.reserve(instructions.size());
    for (size_t i = 0; i < instructions.size(); ++i) {
        word_to_idx[instructions[i].word_offset] = i;
    }

    std::set<size_t> leader_set;
    leader_set.insert(0);
    std::unordered_map<size_t, size_t> branch_target_idx;

    for (size_t i = 0; i < instructions.size(); ++i) {
        const auto& inst = instructions[i];
        if (IsBranchOpcode(inst.opcode)) {
            const int64_t target_word =
                static_cast<int64_t>(inst.word_offset) + 1 + static_cast<int64_t>(inst.simm16);
            if (target_word < 0) {
                return Status::Error(StatusCode::ParseError, "RDNA2 branch target out of range (< 0)");
            }
            auto it = word_to_idx.find(static_cast<uint32_t>(target_word));
            if (it == word_to_idx.end()) {
                return Status::Error(StatusCode::ParseError, "RDNA2 branch target does not land on instruction boundary");
            }
            branch_target_idx[i] = it->second;
            leader_set.insert(it->second);
            if (i + 1 < instructions.size()) {
                leader_set.insert(i + 1);
            }
        } else if (inst.opcode == Rdna2Opcode::S_ENDPGM) {
            if (i + 1 < instructions.size()) {
                leader_set.insert(i + 1);
            }
        }
    }

    std::vector<size_t> leaders(leader_set.begin(), leader_set.end());
    std::unordered_map<size_t, uint32_t> inst_idx_to_bb;
    out_module.blocks.resize(leaders.size());
    for (size_t b = 0; b < leaders.size(); ++b) {
        out_module.blocks[b].id = static_cast<uint32_t>(b);
        out_module.blocks[b].start_word_offset = instructions[leaders[b]].word_offset;
        inst_idx_to_bb[leaders[b]] = static_cast<uint32_t>(b);
    }

    // Build CFG edges between basic blocks.
    for (size_t b = 0; b < leaders.size(); ++b) {
        const size_t start_i = leaders[b];
        const size_t end_i = (b + 1 < leaders.size()) ? leaders[b + 1] : instructions.size();
        const size_t last_i = end_i - 1;
        const auto& last = instructions[last_i];

        if (last.opcode == Rdna2Opcode::S_ENDPGM) {
            // Terminal block
        } else if (last.opcode == Rdna2Opcode::S_BRANCH) {
            const uint32_t tgt_bb = inst_idx_to_bb[branch_target_idx[last_i]];
            AddUnique(out_module.blocks[b].successors, tgt_bb);
            AddUnique(out_module.blocks[tgt_bb].predecessors, static_cast<uint32_t>(b));
        } else if (IsConditionalBranchOpcode(last.opcode)) {
            const uint32_t tgt_bb = inst_idx_to_bb[branch_target_idx[last_i]];
            AddUnique(out_module.blocks[b].successors, tgt_bb);
            AddUnique(out_module.blocks[tgt_bb].predecessors, static_cast<uint32_t>(b));
            if (b + 1 < leaders.size()) {
                const uint32_t fall_bb = static_cast<uint32_t>(b + 1);
                AddUnique(out_module.blocks[b].successors, fall_bb);
                AddUnique(out_module.blocks[fall_bb].predecessors, static_cast<uint32_t>(b));
            }
        } else if (b + 1 < leaders.size()) {
            const uint32_t fall_bb = static_cast<uint32_t>(b + 1);
            AddUnique(out_module.blocks[b].successors, fall_bb);
            AddUnique(out_module.blocks[fall_bb].predecessors, static_cast<uint32_t>(b));
        }
    }

    // 2. Entry-block hoisted input/constant definitions so they dominate all blocks.
    std::vector<IrInstruction> entry_preamble;
    std::unordered_map<IrValueId, IrType> value_types;

    auto register_preamble = [&](IrOp op, IrType type, uint32_t imm_u = 0, float imm_f = 0.0f) -> IrValueId {
        IrInstruction inst{};
        inst.id = out_module.AllocateValueId();
        inst.op = op;
        inst.type = type;
        inst.imm_u32 = imm_u;
        inst.imm_f32 = imm_f;
        value_types[inst.id] = type;
        entry_preamble.push_back(std::move(inst));
        return entry_preamble.back().id;
    };

    IrValueId tid_x_val = kInvalidIrValue;
    IrValueId zero_u32_val = kInvalidIrValue;
    IrValueId zero_f32_val = kInvalidIrValue;
    std::array<IrValueId, 4> ps_inputs{};
    std::unordered_map<uint32_t, IrValueId> root_const_vals;

    auto get_zero_u32 = [&]() -> IrValueId {
        if (zero_u32_val == kInvalidIrValue) {
            zero_u32_val = register_preamble(IrOp::ConstU32, IrType::U32, 0u, 0.0f);
        }
        return zero_u32_val;
    };

    auto get_zero_f32 = [&]() -> IrValueId {
        if (zero_f32_val == kInvalidIrValue) {
            zero_f32_val = register_preamble(IrOp::ConstF32, IrType::F32, 0u, 0.0f);
        }
        return zero_f32_val;
    };

    auto get_tid_x = [&]() -> IrValueId {
        if (tid_x_val == kInvalidIrValue) {
            tid_x_val = register_preamble(IrOp::ThreadIdX, IrType::U32, 0u, 0.0f);
        }
        return tid_x_val;
    };

    auto get_ps_input = [&](uint32_t ch) -> IrValueId {
        if (ch < 4u && ps_inputs[ch] == kInvalidIrValue) {
            ps_inputs[ch] = register_preamble(IrOp::StageInput, IrType::F32, ch, 0.0f);
        }
        return ch < 4u ? ps_inputs[ch] : get_zero_f32();
    };

    auto get_root_const = [&](uint32_t idx) -> IrValueId {
        auto it = root_const_vals.find(idx);
        if (it != root_const_vals.end()) return it->second;
        out_module.uses_root_constants = true;
        if (idx > out_module.max_root_const_index) {
            out_module.max_root_const_index = idx;
        }
        IrValueId v = register_preamble(IrOp::LoadRootConst, IrType::U32, idx, 0.0f);
        root_const_vals[idx] = v;
        return v;
    };

    auto get_type = [&](IrValueId id) -> IrType {
        auto it = value_types.find(id);
        return it != value_types.end() ? it->second : IrType::U32;
    };

    // 3. Lower each basic block into SSA IR.
    std::vector<RegState> exit_states(leaders.size());
    std::vector<bool> block_processed(leaders.size(), false);

    for (size_t b = 0; b < leaders.size(); ++b) {
        auto& bb = out_module.blocks[b];
        RegState state{};

        auto emit_in_bb = [&](IrOp op, IrType type, std::vector<IrValueId> ops = {},
                              uint32_t imm_u = 0, float imm_f = 0.0f) -> IrValueId {
            IrInstruction inst{};
            inst.op = op;
            inst.type = type;
            inst.operands = std::move(ops);
            inst.imm_u32 = imm_u;
            inst.imm_f32 = imm_f;
            if (type != IrType::Void) {
                inst.id = out_module.AllocateValueId();
                value_types[inst.id] = type;
            }
            bb.instructions.push_back(std::move(inst));
            return bb.instructions.back().id;
        };

        // Merge register state from predecessors (emitting Phi nodes at join blocks).
        std::vector<uint32_t> ready_preds;
        for (uint32_t p : bb.predecessors) {
            if (block_processed[p]) ready_preds.push_back(p);
        }

        if (ready_preds.size() == 1) {
            state = exit_states[ready_preds[0]];
        } else if (ready_preds.size() > 1) {
            auto merge_reg = [&](auto getter) -> IrValueId {
                IrValueId first = kInvalidIrValue;
                bool differs = false;
                bool any_set = false;
                for (uint32_t p : ready_preds) {
                    IrValueId v = getter(exit_states[p]);
                    if (v != kInvalidIrValue) any_set = true;
                    if (first == kInvalidIrValue) {
                        first = v;
                    } else if (v != first) {
                        differs = true;
                    }
                }
                if (!any_set) return kInvalidIrValue;
                if (!differs) return first;

                IrType phi_type = IrType::U32;
                for (uint32_t p : ready_preds) {
                    IrValueId v = getter(exit_states[p]);
                    if (v != kInvalidIrValue) {
                        phi_type = get_type(v);
                        break;
                    }
                }

                IrInstruction phi{};
                phi.id = out_module.AllocateValueId();
                phi.op = IrOp::Phi;
                phi.type = phi_type;
                for (uint32_t p : ready_preds) {
                    IrValueId v = getter(exit_states[p]);
                    if (v == kInvalidIrValue) {
                        v = (phi_type == IrType::F32) ? get_zero_f32() : get_zero_u32();
                    }
                    phi.phi_incoming.emplace_back(p, v);
                }
                value_types[phi.id] = phi_type;
                bb.instructions.push_back(std::move(phi));
                return bb.instructions.back().id;
            };

            for (size_t r = 0; r < 256; ++r) {
                state.vgpr[r] = merge_reg([r](const RegState& s) { return s.vgpr[r]; });
            }
            for (size_t r = 0; r < 106; ++r) {
                state.sgpr[r] = merge_reg([r](const RegState& s) { return s.sgpr[r]; });
            }
            state.scc = merge_reg([](const RegState& s) { return s.scc; });
            state.vcc = merge_reg([](const RegState& s) { return s.vcc; });
        }

        auto read_vgpr = [&](uint8_t idx, bool expect_float) -> IrValueId {
            if (state.vgpr[idx] != kInvalidIrValue) return state.vgpr[idx];
            if (out_module.stage == ShaderStage::Pixel && idx < 4u) {
                state.vgpr[idx] = get_ps_input(idx);
                return state.vgpr[idx];
            }
            if ((out_module.stage == ShaderStage::Compute || out_module.stage == ShaderStage::Vertex) && idx == 0u) {
                state.vgpr[idx] = get_tid_x();
                return state.vgpr[idx];
            }
            return expect_float ? get_zero_f32() : get_zero_u32();
        };

        auto read_sgpr = [&](uint8_t idx) -> IrValueId {
            if (state.sgpr[idx] != kInvalidIrValue) return state.sgpr[idx];
            if (idx < 16u) {
                state.sgpr[idx] = get_root_const(idx);
                return state.sgpr[idx];
            }
            return get_zero_u32();
        };

        auto eval_operand = [&](const Rdna2Operand& op, bool expect_float) -> IrValueId {
            IrValueId val = kInvalidIrValue;
            switch (op.kind) {
                case Rdna2OperandKind::Vgpr:
                    val = read_vgpr(static_cast<uint8_t>(op.reg_index), expect_float);
                    break;
                case Rdna2OperandKind::Sgpr:
                    val = read_sgpr(static_cast<uint8_t>(op.reg_index));
                    break;
                case Rdna2OperandKind::InlineInt:
                    if (expect_float && op.int_value == 0) {
                        val = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
                    } else {
                        val = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(op.int_value), 0.0f);
                    }
                    break;
                case Rdna2OperandKind::InlineFloat:
                    val = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, op.literal_bits, op.float_value);
                    break;
                case Rdna2OperandKind::Literal32:
                    if (expect_float) {
                        val = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, op.literal_bits, BitsToFloat(op.literal_bits));
                    } else {
                        val = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, op.literal_bits, 0.0f);
                    }
                    break;
                case Rdna2OperandKind::VccLo:
                    val = (state.vcc != kInvalidIrValue) ? state.vcc : get_zero_u32();
                    break;
                case Rdna2OperandKind::Scc:
                    val = (state.scc != kInvalidIrValue) ? state.scc : get_zero_u32();
                    break;
                default:
                    val = expect_float ? get_zero_f32() : get_zero_u32();
                    break;
            }
            if (expect_float) {
                if (op.abs) {
                    IrValueId z = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
                    IrValueId neg_v = emit_in_bb(IrOp::FSub, IrType::F32, {z, val});
                    val = emit_in_bb(IrOp::FMax, IrType::F32, {val, neg_v});
                }
                if (op.neg) {
                    IrValueId z = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
                    val = emit_in_bb(IrOp::FSub, IrType::F32, {z, val});
                }
            }
            return val;
        };

        auto apply_clamp = [&](IrValueId val, bool clamp) -> IrValueId {
            if (!clamp) return val;
            IrValueId z = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
            IrValueId o = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, FloatToBits(1.0f), 1.0f);
            IrValueId lo = emit_in_bb(IrOp::FMax, IrType::F32, {val, z});
            return emit_in_bb(IrOp::FMin, IrType::F32, {lo, o});
        };

        auto ensure_bool = [&](IrValueId v) -> IrValueId {
            if (v == kInvalidIrValue) {
                IrValueId z = get_zero_u32();
                return emit_in_bb(IrOp::CmpNe, IrType::Bool, {z, z});
            }
            if (get_type(v) == IrType::Bool) return v;
            IrValueId z = get_zero_u32();
            return emit_in_bb(IrOp::CmpNe, IrType::Bool, {v, z});
        };

        auto write_dst = [&](const Rdna2Operand& dst, IrValueId val) {
            if (dst.kind == Rdna2OperandKind::Vgpr) {
                state.vgpr[dst.reg_index] = val;
            } else if (dst.kind == Rdna2OperandKind::Sgpr) {
                state.sgpr[dst.reg_index] = val;
            } else if (dst.kind == Rdna2OperandKind::VccLo) {
                state.vcc = val;
            } else if (dst.kind == Rdna2OperandKind::Scc) {
                state.scc = val;
            }
        };

        const size_t start_i = leaders[b];
        const size_t end_i = (b + 1 < leaders.size()) ? leaders[b + 1] : instructions.size();
        bool terminated = false;

        for (size_t i = start_i; i < end_i; ++i) {
            const auto& inst = instructions[i];
            switch (inst.opcode) {
                // ----- SOP2 -----
                case Rdna2Opcode::S_ADD_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IAdd, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_SUB_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::ISub, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_ADD_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IAdd, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_SUB_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::ISub, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_MIN_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMin, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_MIN_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMin, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_MAX_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMax, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_MAX_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMax, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_CSELECT_B32: {
                    IrValueId c = ensure_bool(state.scc);
                    IrValueId a = eval_operand(inst.src[0], false);
                    IrValueId bval = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::Select, get_type(a), {c, a, bval}));
                    break;
                }
                case Rdna2Opcode::S_AND_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitAnd, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_OR_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitOr, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_XOR_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitXor, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_LSHL_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::Shl, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_LSHR_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::Shr, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_ASHR_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::Shr, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_MUL_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMul, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;

                // ----- SOP1 -----
                case Rdna2Opcode::S_MOV_B32: {
                    const bool is_flt = (inst.src[0].kind == Rdna2OperandKind::InlineFloat);
                    write_dst(inst.dst, eval_operand(inst.src[0], is_flt));
                    break;
                }
                case Rdna2Opcode::S_NOT_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitNot, IrType::U32, {eval_operand(inst.src[0], false)}));
                    break;
                case Rdna2Opcode::S_GETPC_B64: {
                    IrValueId pc_lo = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, (inst.word_offset + 1u) * 4u, 0.0f);
                    write_dst(inst.dst, pc_lo);
                    if (inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index + 1u < 106u) {
                        state.sgpr[inst.dst.reg_index + 1u] = get_zero_u32();
                    }
                    break;
                }

                // ----- SOPC -----
                case Rdna2Opcode::S_CMP_EQ_I32:
                    state.scc = emit_in_bb(IrOp::CmpEq, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LG_I32:
                    state.scc = emit_in_bb(IrOp::CmpNe, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_GT_I32:
                    state.scc = emit_in_bb(IrOp::CmpGt, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_GE_I32:
                    state.scc = emit_in_bb(IrOp::CmpGe, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LT_I32:
                    state.scc = emit_in_bb(IrOp::CmpLt, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LE_I32:
                    state.scc = emit_in_bb(IrOp::CmpGe, IrType::Bool, {eval_operand(inst.src[1], false), eval_operand(inst.src[0], false)});
                    break;
                case Rdna2Opcode::S_CMP_EQ_U32:
                    state.scc = emit_in_bb(IrOp::CmpEqU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LG_U32:
                    state.scc = emit_in_bb(IrOp::CmpNe, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_GT_U32:
                    state.scc = emit_in_bb(IrOp::CmpGtU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_GE_U32:
                    state.scc = emit_in_bb(IrOp::CmpGeU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LT_U32:
                    state.scc = emit_in_bb(IrOp::CmpLtU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LE_U32:
                    state.scc = emit_in_bb(IrOp::CmpGeU, IrType::Bool, {eval_operand(inst.src[1], false), eval_operand(inst.src[0], false)});
                    break;

                // ----- SOPK -----
                case Rdna2Opcode::S_MOVK_I32: {
                    const uint32_t imm = static_cast<uint32_t>(static_cast<int32_t>(inst.simm16));
                    write_dst(inst.dst, emit_in_bb(IrOp::ConstU32, IrType::I32, {}, imm, 0.0f));
                    break;
                }
                case Rdna2Opcode::S_ADDK_I32: {
                    const uint32_t imm = static_cast<uint32_t>(static_cast<int32_t>(inst.simm16));
                    IrValueId cur = read_sgpr(static_cast<uint8_t>(inst.dst.reg_index));
                    IrValueId k = emit_in_bb(IrOp::ConstU32, IrType::I32, {}, imm, 0.0f);
                    write_dst(inst.dst, emit_in_bb(IrOp::IAdd, IrType::I32, {cur, k}));
                    break;
                }
                case Rdna2Opcode::S_MULK_I32: {
                    const uint32_t imm = static_cast<uint32_t>(static_cast<int32_t>(inst.simm16));
                    IrValueId cur = read_sgpr(static_cast<uint8_t>(inst.dst.reg_index));
                    IrValueId k = emit_in_bb(IrOp::ConstU32, IrType::I32, {}, imm, 0.0f);
                    write_dst(inst.dst, emit_in_bb(IrOp::IMul, IrType::I32, {cur, k}));
                    break;
                }

                // ----- SOPP -----
                case Rdna2Opcode::S_NOP:
                case Rdna2Opcode::S_WAITCNT:
                    break;
                case Rdna2Opcode::S_BARRIER:
                    emit_in_bb(IrOp::Barrier, IrType::Void);
                    break;
                case Rdna2Opcode::S_ENDPGM:
                    emit_in_bb(IrOp::Return, IrType::Void);
                    terminated = true;
                    break;
                case Rdna2Opcode::S_BRANCH: {
                    IrInstruction br{};
                    br.op = IrOp::Branch;
                    br.type = IrType::Void;
                    br.true_block = inst_idx_to_bb[branch_target_idx[i]];
                    bb.instructions.push_back(std::move(br));
                    terminated = true;
                    break;
                }
                case Rdna2Opcode::S_CBRANCH_SCC0:
                case Rdna2Opcode::S_CBRANCH_SCC1:
                case Rdna2Opcode::S_CBRANCH_VCCZ:
                case Rdna2Opcode::S_CBRANCH_VCCNZ:
                case Rdna2Opcode::S_CBRANCH_EXECZ:
                case Rdna2Opcode::S_CBRANCH_EXECNZ: {
                    const bool uses_scc = (inst.opcode == Rdna2Opcode::S_CBRANCH_SCC0 || inst.opcode == Rdna2Opcode::S_CBRANCH_SCC1);
                    const bool branch_when_false = (inst.opcode == Rdna2Opcode::S_CBRANCH_SCC0 ||
                                                    inst.opcode == Rdna2Opcode::S_CBRANCH_VCCZ ||
                                                    inst.opcode == Rdna2Opcode::S_CBRANCH_EXECZ);
                    IrValueId cond = ensure_bool(uses_scc ? state.scc : state.vcc);
                    const uint32_t tgt_bb = inst_idx_to_bb[branch_target_idx[i]];
                    const uint32_t fall_bb = (b + 1 < leaders.size()) ? static_cast<uint32_t>(b + 1) : tgt_bb;
                    IrInstruction cbr{};
                    cbr.op = IrOp::CondBranch;
                    cbr.type = IrType::Void;
                    cbr.operands.push_back(cond);
                    if (branch_when_false) {
                        cbr.true_block = fall_bb;
                        cbr.false_block = tgt_bb;
                    } else {
                        cbr.true_block = tgt_bb;
                        cbr.false_block = fall_bb;
                    }
                    bb.instructions.push_back(std::move(cbr));
                    terminated = true;
                    break;
                }

                // ----- VOP2 / promoted VOP3 -----
                case Rdna2Opcode::V_ADD_F32: {
                    IrValueId r = emit_in_bb(IrOp::FAdd, IrType::F32, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_SUB_F32: {
                    IrValueId r = emit_in_bb(IrOp::FSub, IrType::F32, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_SUBREV_F32: {
                    IrValueId r = emit_in_bb(IrOp::FSub, IrType::F32, {eval_operand(inst.src[1], true), eval_operand(inst.src[0], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MUL_F32: {
                    IrValueId r = emit_in_bb(IrOp::FMul, IrType::F32, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MIN_F32: {
                    IrValueId r = emit_in_bb(IrOp::FMin, IrType::F32, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MAX_F32: {
                    IrValueId r = emit_in_bb(IrOp::FMax, IrType::F32, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MIN_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMin, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_MAX_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMax, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_MIN_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMin, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_MAX_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMax, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_FMAC_F32: {
                    IrValueId a = eval_operand(inst.src[0], true);
                    IrValueId bval = eval_operand(inst.src[1], true);
                    IrValueId acc = read_vgpr(static_cast<uint8_t>(inst.dst.reg_index), true);
                    IrValueId r = emit_in_bb(IrOp::FFma, IrType::F32, {a, bval, acc});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_AND_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitAnd, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_OR_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitOr, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_XOR_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitXor, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_LSHLREV_B32: {
                    // AMD ISA: vdst = vsrc1 << (src0 & 31)
                    IrValueId shift = eval_operand(inst.src[0], false);
                    IrValueId val = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::Shl, IrType::U32, {val, shift}));
                    break;
                }
                case Rdna2Opcode::V_LSHRREV_B32: {
                    IrValueId shift = eval_operand(inst.src[0], false);
                    IrValueId val = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::Shr, IrType::U32, {val, shift}));
                    break;
                }
                case Rdna2Opcode::V_ASHRREV_I32: {
                    IrValueId shift = eval_operand(inst.src[0], false);
                    IrValueId val = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::Shr, IrType::I32, {val, shift}));
                    break;
                }
                case Rdna2Opcode::V_ADD_NC_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IAdd, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_SUB_NC_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::ISub, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CNDMASK_B32: {
                    // AMD ISA: D = cond ? src1 : src0
                    IrValueId cond_raw = (inst.src_count >= 3)
                                             ? eval_operand(inst.src[2], false)
                                             : state.vcc;
                    IrValueId cond = ensure_bool(cond_raw);
                    const bool s0_flt = (inst.src[0].kind == Rdna2OperandKind::InlineFloat);
                    const bool s1_flt = (inst.src[1].kind == Rdna2OperandKind::InlineFloat);
                    IrValueId v0 = eval_operand(inst.src[0], s0_flt);
                    IrValueId v1 = eval_operand(inst.src[1], s1_flt);
                    IrType sel_type = (get_type(v1) == IrType::F32 || get_type(v0) == IrType::F32)
                                          ? IrType::F32
                                          : IrType::U32;
                    write_dst(inst.dst, emit_in_bb(IrOp::Select, sel_type, {cond, v1, v0}));
                    break;
                }

                // ----- VOP1 -----
                case Rdna2Opcode::V_NOP:
                    break;
                case Rdna2Opcode::V_MOV_B32: {
                    const bool is_flt = (inst.src[0].kind == Rdna2OperandKind::InlineFloat);
                    write_dst(inst.dst, eval_operand(inst.src[0], is_flt));
                    break;
                }
                case Rdna2Opcode::V_CVT_F32_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::I2F, IrType::F32, {eval_operand(inst.src[0], false)}));
                    break;
                case Rdna2Opcode::V_CVT_I32_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::F2I, IrType::I32, {eval_operand(inst.src[0], true)}));
                    break;
                case Rdna2Opcode::V_CVT_F32_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::U2F, IrType::F32, {eval_operand(inst.src[0], false)}));
                    break;
                case Rdna2Opcode::V_CVT_U32_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::F2U, IrType::U32, {eval_operand(inst.src[0], true)}));
                    break;
                case Rdna2Opcode::V_RCP_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FRcp, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;
                case Rdna2Opcode::V_RSQ_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FRsq, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;
                case Rdna2Opcode::V_SQRT_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FSqrt, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;
                case Rdna2Opcode::V_FLOOR_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FFloor, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;
                case Rdna2Opcode::V_FRACT_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FFract, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;

                // ----- VOPC -----
                case Rdna2Opcode::V_CMP_LT_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpLt, IrType::Bool, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)}));
                    break;
                case Rdna2Opcode::V_CMP_EQ_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpEq, IrType::Bool, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)}));
                    break;
                case Rdna2Opcode::V_CMP_LE_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpGe, IrType::Bool, {eval_operand(inst.src[1], true), eval_operand(inst.src[0], true)}));
                    break;
                case Rdna2Opcode::V_CMP_GT_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpGt, IrType::Bool, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)}));
                    break;
                case Rdna2Opcode::V_CMP_LG_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpNe, IrType::Bool, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)}));
                    break;
                case Rdna2Opcode::V_CMP_GE_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpGe, IrType::Bool, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)}));
                    break;
                case Rdna2Opcode::V_CMP_LT_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpLt, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CMP_EQ_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpEq, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CMP_GT_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpGt, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CMP_GE_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpGe, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CMP_LT_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpLtU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CMP_EQ_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpEqU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CMP_GT_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpGtU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CMP_GE_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpGeU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;

                // ----- VOP3 (native 3-op) -----
                case Rdna2Opcode::V_FMA_F32:
                case Rdna2Opcode::V_MAD_F32: {
                    IrValueId r = emit_in_bb(IrOp::FFma, IrType::F32, {
                        eval_operand(inst.src[0], true),
                        eval_operand(inst.src[1], true),
                        eval_operand(inst.src[2], true),
                    });
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_ADD3_U32: {
                    IrValueId ab = emit_in_bb(IrOp::IAdd, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    write_dst(inst.dst, emit_in_bb(IrOp::IAdd, IrType::U32, {ab, eval_operand(inst.src[2], false)}));
                    break;
                }
                case Rdna2Opcode::V_DOT2_F32_F16: {
                    IrValueId r = emit_in_bb(IrOp::FDot2, IrType::F32, {
                        eval_operand(inst.src[0], false),
                        eval_operand(inst.src[1], false),
                        eval_operand(inst.src[2], true),
                    });
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }

                // ----- SMEM -----
                case Rdna2Opcode::S_LOAD_DWORD:
                case Rdna2Opcode::S_BUFFER_LOAD_DWORD: {
                    const uint32_t byte_off = inst.mem_offset >= 0 ? static_cast<uint32_t>(inst.mem_offset) : 0u;
                    const uint32_t slot = byte_off / 4u;
                    if (slot < 16u && (inst.soffset == 0x7Du || inst.soffset == 0x7Cu || inst.soffset == 128u)) {
                        write_dst(inst.dst, get_root_const(slot));
                    } else {
                        out_module.uses_uav = true;
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, byte_off, 0.0f);
                        if (inst.soffset < 106u) {
                            off = emit_in_bb(IrOp::IAdd, IrType::U32, {off, read_sgpr(inst.soffset)});
                        }
                        write_dst(inst.dst, emit_in_bb(IrOp::LoadUav, IrType::U32, {off}));
                    }
                    break;
                }

                // ----- MUBUF -----
                case Rdna2Opcode::BUFFER_LOAD_DWORD:
                case Rdna2Opcode::BUFFER_STORE_DWORD: {
                    out_module.uses_uav = true;
                    IrValueId addr = kInvalidIrValue;
                    if (inst.offen) {
                        addr = eval_operand(inst.src[1], false);
                    } else if (inst.idxen) {
                        IrValueId idx = eval_operand(inst.src[1], false);
                        IrValueId sh = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 2u, 0.0f);
                        addr = emit_in_bb(IrOp::Shl, IrType::U32, {idx, sh});
                    } else {
                        addr = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0u, 0.0f);
                    }
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    if (inst.soffset != 128u && inst.soffset != 125u) {
                        Rdna2Operand sop = DecodeRdna2SourceOperand(inst.soffset);
                        IrValueId soff_v = eval_operand(sop, false);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, soff_v});
                    }
                    if (inst.opcode == Rdna2Opcode::BUFFER_LOAD_DWORD) {
                        write_dst(inst.dst, emit_in_bb(IrOp::LoadUav, IrType::U32, {addr}));
                    } else {
                        IrValueId data = eval_operand(inst.src[0], false);
                        emit_in_bb(IrOp::StoreUav, IrType::Void, {addr, data});
                    }
                    break;
                }

                // ----- GLOBAL / FLAT -----
                case Rdna2Opcode::GLOBAL_LOAD_DWORD:
                case Rdna2Opcode::FLAT_LOAD_DWORD: {
                    out_module.uses_uav = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    write_dst(inst.dst, emit_in_bb(IrOp::LoadUav, IrType::U32, {addr}));
                    break;
                }
                case Rdna2Opcode::GLOBAL_STORE_DWORD:
                case Rdna2Opcode::FLAT_STORE_DWORD: {
                    out_module.uses_uav = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    IrValueId data = eval_operand(inst.src[1], false);
                    emit_in_bb(IrOp::StoreUav, IrType::Void, {addr, data});
                    break;
                }

                // ----- EXP -----
                case Rdna2Opcode::EXP: {
                    std::vector<IrValueId> ch(4);
                    for (uint8_t c = 0; c < 4; ++c) {
                        if ((inst.exp_en & (1u << c)) != 0u) {
                            ch[c] = eval_operand(inst.src[c], true);
                        } else if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0) && c == 3u) {
                            ch[c] = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, FloatToBits(1.0f), 1.0f);
                        } else {
                            ch[c] = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
                        }
                    }
                    IrInstruction ex{};
                    ex.op = IrOp::Export;
                    ex.type = IrType::Void;
                    ex.operands = std::move(ch);
                    ex.export_target = inst.exp_target;
                    ex.export_mask = inst.exp_en;
                    bb.instructions.push_back(std::move(ex));

                    if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0)) {
                        out_module.exports_pos0 = true;
                    } else if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Mrt0)) {
                        out_module.exports_mrt0 = true;
                    } else if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Param0)) {
                        out_module.exports_param0 = true;
                    }
                    break;
                }

                default:
                    return Status::Error(
                        StatusCode::NotImplemented,
                        std::string("Unhandled RDNA2 opcode in IR builder: ") + Rdna2OpcodeName(inst.opcode));
            }
        }

        if (!terminated) {
            if (b + 1 < leaders.size()) {
                IrInstruction br{};
                br.op = IrOp::Branch;
                br.type = IrType::Void;
                br.true_block = static_cast<uint32_t>(b + 1);
                bb.instructions.push_back(std::move(br));
            } else {
                emit_in_bb(IrOp::Return, IrType::Void);
            }
        }

        exit_states[b] = state;
        block_processed[b] = true;
    }

    // Prepend hoisted entry preamble into block 0.
    if (!entry_preamble.empty()) {
        auto& bb0 = out_module.blocks[0].instructions;
        bb0.insert(bb0.begin(), std::make_move_iterator(entry_preamble.begin()),
                   std::make_move_iterator(entry_preamble.end()));
    }

    uint32_t total_ops = 0;
    for (const auto& bb : out_module.blocks) {
        total_ops += static_cast<uint32_t>(bb.instructions.size());
    }
    out_module.opt_stats.initial_ops = total_ops;
    out_module.opt_stats.final_ops = total_ops;

    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Constant Folding + Dead-Code Elimination Passes
// ---------------------------------------------------------------------------
Status OptimizeRdna2Ir(Rdna2IrModule& module, Rdna2IrOptStats* out_stats) {
    Rdna2IrOptStats stats{};
    for (const auto& bb : module.blocks) {
        stats.initial_ops += static_cast<uint32_t>(bb.instructions.size());
    }

    // Build value definition lookup table.
    std::unordered_map<IrValueId, IrInstruction*> defs;
    auto rebuild_defs = [&]() {
        defs.clear();
        for (auto& bb : module.blocks) {
            for (auto& inst : bb.instructions) {
                if (inst.id != kInvalidIrValue) {
                    defs[inst.id] = &inst;
                }
            }
        }
    };

    auto get_const_u32 = [&](IrValueId id, uint32_t& out_v) -> bool {
        auto it = defs.find(id);
        if (it == defs.end()) return false;
        const auto* d = it->second;
        if (d->op == IrOp::ConstU32) {
            out_v = d->imm_u32;
            return true;
        }
        if (d->op == IrOp::ConstF32) {
            out_v = FloatToBits(d->imm_f32);
            return true;
        }
        return false;
    };

    auto get_const_f32 = [&](IrValueId id, float& out_v) -> bool {
        auto it = defs.find(id);
        if (it == defs.end()) return false;
        const auto* d = it->second;
        if (d->op == IrOp::ConstF32) {
            out_v = d->imm_f32;
            return true;
        }
        if (d->op == IrOp::ConstU32) {
            out_v = BitsToFloat(d->imm_u32);
            return true;
        }
        return false;
    };

    // Pass 1: Iterative Constant Folding
    for (int iter = 0; iter < 16; ++iter) {
        rebuild_defs();
        bool changed = false;

        for (auto& bb : module.blocks) {
            for (auto& inst : bb.instructions) {
                if (inst.id == kInvalidIrValue || IrOpHasSideEffects(inst.op)) continue;
                if (inst.op == IrOp::ConstU32 || inst.op == IrOp::ConstF32 ||
                    inst.op == IrOp::ThreadIdX || inst.op == IrOp::StageInput ||
                    inst.op == IrOp::LoadRootConst || inst.op == IrOp::LoadUav) {
                    continue;
                }

                auto fold_to_f32 = [&](float v) {
                    inst.op = IrOp::ConstF32;
                    inst.type = IrType::F32;
                    inst.operands.clear();
                    inst.phi_incoming.clear();
                    inst.imm_f32 = v;
                    inst.imm_u32 = FloatToBits(v);
                    ++stats.folded_constants;
                    changed = true;
                };

                auto fold_to_u32 = [&](uint32_t v, IrType t) {
                    inst.op = IrOp::ConstU32;
                    inst.type = t;
                    inst.operands.clear();
                    inst.phi_incoming.clear();
                    inst.imm_u32 = v;
                    inst.imm_f32 = BitsToFloat(v);
                    ++stats.folded_constants;
                    changed = true;
                };

                float fa = 0.0f, fb = 0.0f, fc = 0.0f;
                uint32_t ua = 0, ub = 0;

                switch (inst.op) {
                    case IrOp::FAdd:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) fold_to_f32(fa + fb);
                        break;
                    case IrOp::FSub:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) fold_to_f32(fa - fb);
                        break;
                    case IrOp::FMul:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) fold_to_f32(fa * fb);
                        break;
                    case IrOp::FFma:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb) &&
                            get_const_f32(inst.operands[2], fc)) {
                            fold_to_f32(std::fma(fa, fb, fc));
                        }
                        break;
                    case IrOp::FMin:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) fold_to_f32(std::fmin(fa, fb));
                        break;
                    case IrOp::FMax:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) fold_to_f32(std::fmax(fa, fb));
                        break;
                    case IrOp::FRcp:
                        if (get_const_f32(inst.operands[0], fa) && fa != 0.0f) fold_to_f32(1.0f / fa);
                        break;
                    case IrOp::FSqrt:
                        if (get_const_f32(inst.operands[0], fa) && fa >= 0.0f) fold_to_f32(std::sqrt(fa));
                        break;
                    case IrOp::FRsq:
                        if (get_const_f32(inst.operands[0], fa) && fa > 0.0f) fold_to_f32(1.0f / std::sqrt(fa));
                        break;
                    case IrOp::FFloor:
                        if (get_const_f32(inst.operands[0], fa)) fold_to_f32(std::floor(fa));
                        break;
                    case IrOp::FFract:
                        if (get_const_f32(inst.operands[0], fa)) fold_to_f32(fa - std::floor(fa));
                        break;
                    case IrOp::IAdd:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua + ub, inst.type);
                        break;
                    case IrOp::ISub:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua - ub, inst.type);
                        break;
                    case IrOp::IMul:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua * ub, inst.type);
                        break;
                    case IrOp::IMin:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const uint32_t r = (inst.type == IrType::I32)
                                                   ? static_cast<uint32_t>(std::min(static_cast<int32_t>(ua), static_cast<int32_t>(ub)))
                                                   : std::min(ua, ub);
                            fold_to_u32(r, inst.type);
                        }
                        break;
                    case IrOp::IMax:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const uint32_t r = (inst.type == IrType::I32)
                                                   ? static_cast<uint32_t>(std::max(static_cast<int32_t>(ua), static_cast<int32_t>(ub)))
                                                   : std::max(ua, ub);
                            fold_to_u32(r, inst.type);
                        }
                        break;
                    case IrOp::BitAnd:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua & ub, IrType::U32);
                        break;
                    case IrOp::BitOr:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua | ub, IrType::U32);
                        break;
                    case IrOp::BitXor:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua ^ ub, IrType::U32);
                        break;
                    case IrOp::BitNot:
                        if (get_const_u32(inst.operands[0], ua)) fold_to_u32(~ua, IrType::U32);
                        break;
                    case IrOp::Shl:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua << (ub & 31u), IrType::U32);
                        break;
                    case IrOp::Shr:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const uint32_t r = (inst.type == IrType::I32)
                                                   ? static_cast<uint32_t>(static_cast<int32_t>(ua) >> (ub & 31u))
                                                   : (ua >> (ub & 31u));
                            fold_to_u32(r, inst.type);
                        }
                        break;
                    case IrOp::I2F:
                        if (get_const_u32(inst.operands[0], ua)) fold_to_f32(static_cast<float>(static_cast<int32_t>(ua)));
                        break;
                    case IrOp::U2F:
                        if (get_const_u32(inst.operands[0], ua)) fold_to_f32(static_cast<float>(ua));
                        break;
                    case IrOp::F2I:
                        if (get_const_f32(inst.operands[0], fa)) fold_to_u32(static_cast<uint32_t>(static_cast<int32_t>(fa)), IrType::I32);
                        break;
                    case IrOp::F2U:
                        if (get_const_f32(inst.operands[0], fa)) fold_to_u32(static_cast<uint32_t>(fa), IrType::U32);
                        break;
                    case IrOp::CmpEq:
                    case IrOp::CmpNe:
                    case IrOp::CmpLt:
                    case IrOp::CmpGt:
                    case IrOp::CmpGe: {
                        const auto* d0 = defs.count(inst.operands[0]) ? defs[inst.operands[0]] : nullptr;
                        const auto* d1 = defs.count(inst.operands[1]) ? defs[inst.operands[1]] : nullptr;
                        if (d0 && d1 && (d0->type == IrType::F32 || d1->type == IrType::F32)) {
                            if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) {
                                bool r = false;
                                if (inst.op == IrOp::CmpEq) r = (fa == fb);
                                else if (inst.op == IrOp::CmpNe) r = (fa != fb);
                                else if (inst.op == IrOp::CmpLt) r = (fa < fb);
                                else if (inst.op == IrOp::CmpGt) r = (fa > fb);
                                else if (inst.op == IrOp::CmpGe) r = (fa >= fb);
                                fold_to_u32(r ? 1u : 0u, IrType::Bool);
                            }
                        } else if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const int32_t sa = static_cast<int32_t>(ua);
                            const int32_t sb = static_cast<int32_t>(ub);
                            bool r = false;
                            if (inst.op == IrOp::CmpEq) r = (sa == sb);
                            else if (inst.op == IrOp::CmpNe) r = (sa != sb);
                            else if (inst.op == IrOp::CmpLt) r = (sa < sb);
                            else if (inst.op == IrOp::CmpGt) r = (sa > sb);
                            else if (inst.op == IrOp::CmpGe) r = (sa >= sb);
                            fold_to_u32(r ? 1u : 0u, IrType::Bool);
                        }
                        break;
                    }
                    case IrOp::CmpLtU:
                    case IrOp::CmpEqU:
                    case IrOp::CmpGtU:
                    case IrOp::CmpGeU:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            bool r = false;
                            if (inst.op == IrOp::CmpLtU) r = (ua < ub);
                            else if (inst.op == IrOp::CmpEqU) r = (ua == ub);
                            else if (inst.op == IrOp::CmpGtU) r = (ua > ub);
                            else if (inst.op == IrOp::CmpGeU) r = (ua >= ub);
                            fold_to_u32(r ? 1u : 0u, IrType::Bool);
                        }
                        break;
                    case IrOp::Select:
                        if (get_const_u32(inst.operands[0], ua)) {
                            const IrValueId chosen = (ua != 0u) ? inst.operands[1] : inst.operands[2];
                            if (inst.type == IrType::F32 && get_const_f32(chosen, fa)) {
                                fold_to_f32(fa);
                            } else if (inst.type != IrType::F32 && get_const_u32(chosen, ub)) {
                                fold_to_u32(ub, inst.type);
                            }
                        }
                        break;
                    default:
                        break;
                }
            }
        }
        if (!changed) break;
    }

    // Pass 2: Dead-Code Elimination (mark-and-sweep from side-effecting roots)
    rebuild_defs();
    std::unordered_set<IrValueId> live;
    std::vector<IrValueId> worklist;

    auto mark_use = [&](IrValueId id) {
        if (id != kInvalidIrValue && live.insert(id).second) {
            worklist.push_back(id);
        }
    };

    for (const auto& bb : module.blocks) {
        for (const auto& inst : bb.instructions) {
            if (IrOpHasSideEffects(inst.op)) {
                for (IrValueId op_id : inst.operands) {
                    mark_use(op_id);
                }
            }
        }
    }

    while (!worklist.empty()) {
        const IrValueId cur = worklist.back();
        worklist.pop_back();
        auto it = defs.find(cur);
        if (it == defs.end()) continue;
        const auto* d = it->second;
        for (IrValueId op_id : d->operands) {
            mark_use(op_id);
        }
        for (const auto& incoming : d->phi_incoming) {
            mark_use(incoming.second);
        }
    }

    for (auto& bb : module.blocks) {
        std::vector<IrInstruction> kept;
        kept.reserve(bb.instructions.size());
        for (auto& inst : bb.instructions) {
            if (IrOpHasSideEffects(inst.op) || (inst.id != kInvalidIrValue && live.count(inst.id) > 0)) {
                kept.push_back(std::move(inst));
            } else {
                ++stats.eliminated_ops;
            }
        }
        bb.instructions = std::move(kept);
    }

    for (const auto& bb : module.blocks) {
        stats.final_ops += static_cast<uint32_t>(bb.instructions.size());
    }

    module.opt_stats = stats;
    if (out_stats) {
        *out_stats = stats;
    }
    return Status::Ok();
}

std::string DumpRdna2Ir(const Rdna2IrModule& module) {
    std::ostringstream os;
    os << "; ScarlettX RDNA2 SSA IR (blocks=" << module.blocks.size()
       << ", tg=" << module.threadgroup_x << "x" << module.threadgroup_y << "x" << module.threadgroup_z << ")\n";
    for (const auto& bb : module.blocks) {
        os << "bb" << bb.id << ":";
        if (!bb.predecessors.empty()) {
            os << "  ; preds=";
            for (size_t i = 0; i < bb.predecessors.size(); ++i) {
                if (i) os << ",";
                os << "bb" << bb.predecessors[i];
            }
        }
        os << "\n";
        for (const auto& inst : bb.instructions) {
            os << "  ";
            if (inst.id != kInvalidIrValue) {
                os << "%" << inst.id << " : " << IrTypeName(inst.type) << " = ";
            }
            os << IrOpName(inst.op);
            if (inst.op == IrOp::ConstU32) {
                os << " " << inst.imm_u32;
            } else if (inst.op == IrOp::ConstF32) {
                os << " " << inst.imm_f32;
            } else if (inst.op == IrOp::LoadRootConst || inst.op == IrOp::StageInput) {
                os << " " << inst.imm_u32;
            } else if (inst.op == IrOp::Phi) {
                for (size_t i = 0; i < inst.phi_incoming.size(); ++i) {
                    os << (i ? ", " : " ") << "[bb" << inst.phi_incoming[i].first << ": %" << inst.phi_incoming[i].second << "]";
                }
            } else if (inst.op == IrOp::Branch) {
                os << " bb" << inst.true_block;
            } else if (inst.op == IrOp::CondBranch) {
                os << " %" << inst.operands[0] << ", bb" << inst.true_block << ", bb" << inst.false_block;
            } else {
                for (size_t i = 0; i < inst.operands.size(); ++i) {
                    os << (i ? ", " : " ") << "%" << inst.operands[i];
                }
                if (inst.op == IrOp::Export) {
                    os << " (target=" << static_cast<uint32_t>(inst.export_target) << ")";
                }
            }
            os << "\n";
        }
    }
    return os.str();
}

}  // namespace scarlettx
