#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"
#include "scarlettx/gpu/shader_ir.h"

namespace scarlettx {

// ---------------------------------------------------------------------------
// Clean-room Scarlett RDNA2 / GFX10.3 ('XS2\0') Shader Container
// ---------------------------------------------------------------------------
constexpr uint32_t kXs2ContainerVersion = 1u;
constexpr uint32_t kXs2HeaderSizeBytes = 40u;

struct Xs2ShaderContainer {
    uint32_t version = kXs2ContainerVersion;
    ShaderStage stage = ShaderStage::Compute;
    uint32_t sgpr_count = 16;
    uint32_t vgpr_count = 16;
    uint32_t threadgroup_x = 1;
    uint32_t threadgroup_y = 1;
    uint32_t threadgroup_z = 1;
    uint32_t entry_offset = kXs2HeaderSizeBytes;
    std::vector<uint32_t> code_words;
};

Status ParseXs2Container(const std::vector<uint8_t>& bytes, Xs2ShaderContainer& out_container);
std::vector<uint8_t> BuildXs2Container(const Xs2ShaderContainer& container);

// ---------------------------------------------------------------------------
// AMD RDNA2 / GFX10.3 Binary Instruction Model (Public ISA Reference Guide)
// ---------------------------------------------------------------------------
enum class Rdna2Encoding : uint8_t {
    Unknown = 0,
    SOP2,
    SOP1,
    SOPC,
    SOPK,
    SOPP,
    VOP2,
    VOP1,
    VOPC,
    VOP3,
    SMEM,
    MUBUF,
    FLAT,
    EXP,
};

enum class Rdna2Opcode : uint16_t {
    Unknown = 0,

    // SOP2
    S_ADD_U32,
    S_SUB_U32,
    S_ADD_I32,
    S_SUB_I32,
    S_MIN_I32,
    S_MIN_U32,
    S_MAX_I32,
    S_MAX_U32,
    S_CSELECT_B32,
    S_AND_B32,
    S_OR_B32,
    S_XOR_B32,
    S_LSHL_B32,
    S_LSHR_B32,
    S_ASHR_I32,
    S_MUL_I32,

    // SOP1
    S_MOV_B32,
    S_NOT_B32,
    S_GETPC_B64,

    // SOPC
    S_CMP_EQ_I32,
    S_CMP_LG_I32,
    S_CMP_GT_I32,
    S_CMP_GE_I32,
    S_CMP_LT_I32,
    S_CMP_LE_I32,
    S_CMP_EQ_U32,
    S_CMP_LG_U32,
    S_CMP_GT_U32,
    S_CMP_GE_U32,
    S_CMP_LT_U32,
    S_CMP_LE_U32,

    // SOPK
    S_MOVK_I32,
    S_ADDK_I32,
    S_MULK_I32,

    // SOPP
    S_NOP,
    S_ENDPGM,
    S_BRANCH,
    S_CBRANCH_SCC0,
    S_CBRANCH_SCC1,
    S_CBRANCH_VCCZ,
    S_CBRANCH_VCCNZ,
    S_CBRANCH_EXECZ,
    S_CBRANCH_EXECNZ,
    S_WAITCNT,
    S_BARRIER,

    // VOP2
    V_CNDMASK_B32,
    V_ADD_F32,
    V_SUB_F32,
    V_SUBREV_F32,
    V_MUL_F32,
    V_MIN_F32,
    V_MAX_F32,
    V_MIN_I32,
    V_MAX_I32,
    V_MIN_U32,
    V_MAX_U32,
    V_FMAC_F32,
    V_AND_B32,
    V_OR_B32,
    V_XOR_B32,
    V_LSHLREV_B32,
    V_LSHRREV_B32,
    V_ASHRREV_I32,
    V_ADD_NC_U32,
    V_SUB_NC_U32,

    // VOP1
    V_NOP,
    V_MOV_B32,
    V_CVT_F32_I32,
    V_CVT_I32_F32,
    V_CVT_F32_U32,
    V_CVT_U32_F32,
    V_RCP_F32,
    V_RSQ_F32,
    V_SQRT_F32,
    V_FLOOR_F32,
    V_FRACT_F32,

    // VOPC
    V_CMP_LT_F32,
    V_CMP_EQ_F32,
    V_CMP_LE_F32,
    V_CMP_GT_F32,
    V_CMP_LG_F32,
    V_CMP_GE_F32,
    V_CMP_LT_I32,
    V_CMP_EQ_I32,
    V_CMP_GT_I32,
    V_CMP_GE_I32,
    V_CMP_LT_U32,
    V_CMP_EQ_U32,
    V_CMP_GT_U32,
    V_CMP_GE_U32,

    // VOP3 (native 3-operand)
    V_FMA_F32,
    V_MAD_F32,
    V_ADD3_U32,
    V_DOT2_F32_F16,

    // SMEM
    S_LOAD_DWORD,
    S_BUFFER_LOAD_DWORD,

    // MUBUF
    BUFFER_LOAD_DWORD,
    BUFFER_STORE_DWORD,

    // FLAT / GLOBAL
    GLOBAL_LOAD_DWORD,
    GLOBAL_STORE_DWORD,
    FLAT_LOAD_DWORD,
    FLAT_STORE_DWORD,

    // EXP
    EXP,
};

enum class Rdna2OperandKind : uint8_t {
    None = 0,
    Sgpr,
    Vgpr,
    InlineInt,
    InlineFloat,
    Literal32,
    VccLo,
    VccHi,
    ExecLo,
    ExecHi,
    Scc,
    M0,
    Null,
};

struct Rdna2Operand {
    Rdna2OperandKind kind = Rdna2OperandKind::None;
    uint16_t raw_code = 0;   // 9-bit RDNA2 source code or register index
    uint16_t reg_index = 0;  // SGPR (0..105) or VGPR (0..255)
    int32_t int_value = 0;
    float float_value = 0.0f;
    uint32_t literal_bits = 0;
    bool neg = false;
    bool abs = false;

    static Rdna2Operand Sgpr(uint8_t idx);
    static Rdna2Operand Vgpr(uint8_t idx);
    static Rdna2Operand InlineInt(int32_t val);
    static Rdna2Operand InlineFloat(float val);
    static Rdna2Operand LiteralU32(uint32_t bits);
    static Rdna2Operand LiteralF32(float val);
    static Rdna2Operand VccLo();
    static Rdna2Operand Scc();
    static Rdna2Operand Null();
};

enum class Rdna2ExportTarget : uint8_t {
    Mrt0 = 0,
    Mrt1 = 1,
    Mrt2 = 2,
    Mrt3 = 3,
    Mrt4 = 4,
    Mrt5 = 5,
    Mrt6 = 6,
    Mrt7 = 7,
    MrtZ = 8,
    Null = 9,
    Pos0 = 12,
    Pos1 = 13,
    Pos2 = 14,
    Pos3 = 15,
    Param0 = 32,
    Param1 = 33,
    Param2 = 34,
    Param3 = 35,
};

struct Rdna2Instruction {
    uint32_t word_offset = 0;   // Index of word0 in code_words
    uint32_t word_count = 1;    // 1 (32-bit), 2 (64-bit), or 3 (VOP3 + literal32)
    uint32_t raw_words[3] = {0, 0, 0};

    Rdna2Encoding encoding = Rdna2Encoding::Unknown;
    Rdna2Opcode opcode = Rdna2Opcode::Unknown;
    uint16_t raw_opcode = 0;

    Rdna2Operand dst{};
    Rdna2Operand src[4]{};
    uint8_t src_count = 0;

    // Immediate / modifier / memory / export metadata
    int16_t simm16 = 0;       // SOPK / SOPP signed 16-bit immediate
    int32_t mem_offset = 0;   // SMEM (21-bit signed), MUBUF (12-bit unsigned), FLAT/GLOBAL (12-bit signed)
    uint8_t sbase = 0;        // SMEM base SGPR index (s[sbase*2])
    uint8_t srsrc = 0;        // MUBUF descriptor SGPR base (s[srsrc*4])
    uint8_t soffset = 0;      // SMEM / MUBUF scalar offset code
    uint8_t saddr = 0x7Fu;    // GLOBAL/FLAT scalar base SGPR (0x7F = off)
    uint8_t flat_seg = 0;     // 0=FLAT, 1=SCRATCH, 2=GLOBAL
    bool offen = false;
    bool idxen = false;
    bool glc = false;
    bool slc = false;
    bool dlc = false;
    bool clamp = false;
    uint8_t omod = 0;

    // EXP metadata
    uint8_t exp_target = 0;
    uint8_t exp_en = 0;
    bool exp_done = false;
    bool exp_compr = false;
    bool exp_vm = false;

    std::string mnemonic;
    std::string disasm;
};

// Decodes a 9-bit RDNA2 source operand code (0..511), optionally consuming a 32-bit literal.
Rdna2Operand DecodeRdna2SourceOperand(uint16_t code, uint32_t literal_bits = 0);

// Encodes an operand into a 9-bit RDNA2 source code; sets out_needs_literal if code == 255.
bool EncodeRdna2SourceOperand(
    const Rdna2Operand& op,
    uint16_t& out_code,
    uint32_t& out_literal,
    bool& out_needs_literal);

const char* Rdna2OpcodeName(Rdna2Opcode op) noexcept;
const char* Rdna2EncodingName(Rdna2Encoding enc) noexcept;
std::string FormatRdna2Operand(const Rdna2Operand& op);
std::string FormatRdna2Instruction(const Rdna2Instruction& inst);

// Disassembles a raw 32-bit RDNA2 / GFX10.3 instruction word stream.
Status DisassembleRdna2(
    const std::vector<uint32_t>& words,
    std::vector<Rdna2Instruction>& out_instructions);

// ---------------------------------------------------------------------------
// Clean-room RDNA2 / GFX10.3 Binary Instruction Assembler
// ---------------------------------------------------------------------------
class Rdna2Assembler {
public:
    void Clear() { words_.clear(); }
    const std::vector<uint32_t>& Words() const noexcept { return words_; }
    uint32_t CurrentWordOffset() const noexcept { return static_cast<uint32_t>(words_.size()); }

    // SOP2
    void EmitSop2(Rdna2Opcode op, uint8_t sdst, const Rdna2Operand& ssrc0, const Rdna2Operand& ssrc1);
    // SOP1
    void EmitSop1(Rdna2Opcode op, uint8_t sdst, const Rdna2Operand& ssrc0);
    // SOPC
    void EmitSopc(Rdna2Opcode op, const Rdna2Operand& ssrc0, const Rdna2Operand& ssrc1);
    // SOPK
    void EmitSopk(Rdna2Opcode op, uint8_t sdst, int16_t simm16);
    // SOPP (returns word offset of emitted instruction so branch offsets can be patched)
    uint32_t EmitSopp(Rdna2Opcode op, int16_t simm16 = 0);
    void PatchBranch(uint32_t branch_word_offset, uint32_t target_word_offset);

    // VOP2 (automatically emits 64-bit word when src0 is a 32-bit literal)
    void EmitVop2(Rdna2Opcode op, uint8_t vdst, const Rdna2Operand& src0, uint8_t vsrc1);
    // VOP1 (automatically emits 64-bit word when src0 is a 32-bit literal)
    void EmitVop1(Rdna2Opcode op, uint8_t vdst, const Rdna2Operand& src0);
    // VOPC (automatically emits 64-bit word when src0 is a 32-bit literal)
    void EmitVopc(Rdna2Opcode op, const Rdna2Operand& src0, uint8_t vsrc1);
    // VOP3 (64-bit, or 96-bit if one source is a 32-bit literal)
    void EmitVop3(
        Rdna2Opcode op,
        uint8_t vdst,
        const Rdna2Operand& src0,
        const Rdna2Operand& src1,
        const Rdna2Operand& src2 = Rdna2Operand::InlineInt(0),
        bool clamp = false);

    // SMEM (64-bit)
    void EmitSmem(Rdna2Opcode op, uint8_t sdata, uint8_t sbase_sgpr, int32_t offset, uint8_t soffset = 0x7Du);

    // MUBUF (64-bit)
    void EmitMubuf(
        Rdna2Opcode op,
        uint8_t vdata,
        uint8_t vaddr,
        uint8_t srsrc_sgpr,
        uint16_t offset = 0,
        bool offen = true,
        bool idxen = false,
        uint8_t soffset = 128);

    // GLOBAL / FLAT (64-bit)
    void EmitGlobal(
        Rdna2Opcode op,
        uint8_t vdst_or_data,
        uint8_t vaddr,
        int16_t offset = 0,
        uint8_t saddr = 0x7Fu);

    // EXP (64-bit)
    void EmitExp(
        uint8_t target,
        uint8_t vsrc0,
        uint8_t vsrc1,
        uint8_t vsrc2,
        uint8_t vsrc3,
        uint8_t en = 0xFu,
        bool done = true,
        bool compr = false,
        bool vm = false);

    // Helper: wrap assembled words into an XS2\0 container
    std::vector<uint8_t> BuildXs2(
        ShaderStage stage,
        uint32_t tg_x = 1,
        uint32_t tg_y = 1,
        uint32_t tg_z = 1,
        uint32_t sgpr_count = 16,
        uint32_t vgpr_count = 16) const;

private:
    std::vector<uint32_t> words_;
};

}  // namespace scarlettx
