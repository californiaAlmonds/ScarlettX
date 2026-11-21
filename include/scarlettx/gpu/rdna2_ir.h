#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "scarlettx/core/status.h"
#include "scarlettx/gpu/rdna2_disasm.h"
#include "scarlettx/gpu/shader_ir.h"

namespace scarlettx {

using IrValueId = uint32_t;
constexpr IrValueId kInvalidIrValue = 0u;

enum class IrType : uint8_t {
    Void = 0,
    U32,
    I32,
    F32,
    Bool,
};

enum class IrOp : uint16_t {
    ConstU32 = 0,
    ConstF32,
    ThreadIdX,
    StageInput,     // Vertex/Pixel input lane (e.g. PS interpolated PARAM0 channel 0..3)
    LoadRootConst,  // Read 32-bit scalar constant from b0[imm_u32]
    LoadUav,        // Load 32-bit word from u0 at byte offset operands[0]
    StoreUav,       // Store operands[1] to u0 at byte offset operands[0]

    // Floating-point arithmetic
    FAdd,
    FSub,
    FMul,
    FFma,
    FMin,
    FMax,
    FRcp,
    FRsq,
    FSqrt,
    FFloor,
    FFract,
    FDot2,          // V_DOT2_F32_F16(a_packed, b_packed, c_f32)

    // Integer / bitwise arithmetic
    IAdd,
    ISub,
    IMul,
    IMin,
    IMax,
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    Shl,
    Shr,

    // Conversions
    I2F,
    F2I,
    U2F,
    F2U,

    // Comparisons (produce IrType::Bool)
    CmpEq,
    CmpNe,
    CmpLt,
    CmpGt,
    CmpGe,
    CmpLtU,
    CmpEqU,
    CmpGtU,
    CmpGeU,

    // Selection & SSA merge
    Select,         // operands: [cond_bool, true_val, false_val]
    Phi,            // incoming: (pred_block_id, value_id) pairs

    // Control flow & shader outputs
    Barrier,
    Branch,         // target_block = true_block
    CondBranch,     // operands[0] = cond_bool, true_block, false_block
    Export,         // operands[0..3] = rgba/xyzw, export_target = target
    Return,
};

struct IrInstruction {
    IrValueId id = kInvalidIrValue;  // 0 for void/side-effecting ops
    IrOp op = IrOp::Return;
    IrType type = IrType::Void;

    std::vector<IrValueId> operands;
    std::vector<std::pair<uint32_t, IrValueId>> phi_incoming;  // (pred_block_id, value_id)

    uint32_t imm_u32 = 0;
    float imm_f32 = 0.0f;
    uint32_t true_block = 0;
    uint32_t false_block = 0;
    uint8_t export_target = 0;
    uint8_t export_mask = 0xFu;
};

struct IrBasicBlock {
    uint32_t id = 0;
    uint32_t start_word_offset = 0;
    std::vector<uint32_t> predecessors;
    std::vector<uint32_t> successors;
    std::vector<IrInstruction> instructions;
};

struct Rdna2IrOptStats {
    uint32_t initial_ops = 0;
    uint32_t final_ops = 0;
    uint32_t folded_constants = 0;
    uint32_t eliminated_ops = 0;
};

struct Rdna2IrModule {
    ShaderStage stage = ShaderStage::Compute;
    uint32_t threadgroup_x = 1;
    uint32_t threadgroup_y = 1;
    uint32_t threadgroup_z = 1;
    uint32_t sgpr_count = 16;
    uint32_t vgpr_count = 16;

    uint32_t next_value_id = 1;
    uint32_t max_root_const_index = 0;
    bool uses_root_constants = false;
    bool uses_uav = false;
    bool exports_pos0 = false;
    bool exports_mrt0 = false;
    bool exports_param0 = false;

    std::vector<IrBasicBlock> blocks;
    Rdna2IrOptStats opt_stats{};

    IrValueId AllocateValueId() { return next_value_id++; }
    const IrInstruction* FindDef(IrValueId id) const noexcept;
};

const char* IrOpName(IrOp op) noexcept;
const char* IrTypeName(IrType type) noexcept;
bool IrOpHasSideEffects(IrOp op) noexcept;

// Builds a CFG + SSA IR module from decoded RDNA2 instructions.
Status BuildRdna2Ir(
    const Xs2ShaderContainer& container,
    const std::vector<Rdna2Instruction>& instructions,
    Rdna2IrModule& out_module);

// Runs Constant Folding and Dead-Code Elimination passes on an IR module.
Status OptimizeRdna2Ir(Rdna2IrModule& module, Rdna2IrOptStats* out_stats = nullptr);

// Formats the SSA IR module into human-readable text for diagnostics/debugging.
std::string DumpRdna2Ir(const Rdna2IrModule& module);

}  // namespace scarlettx
