#include "scarlettx/gpu/rdna2_disasm.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace scarlettx {

namespace {

uint32_t ReadLe32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

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

int32_t SignExtend(uint32_t value, unsigned bits) {
    const uint32_t shift = 32u - bits;
    return static_cast<int32_t>(value << shift) >> shift;
}

// Public AMD RDNA2 / GFX10.3 inline float constants (codes 240..248)
constexpr uint32_t kInlineFloatBits[9] = {
    0x3F000000u,  // 240:  0.5f
    0xBF000000u,  // 241: -0.5f
    0x3F800000u,  // 242:  1.0f
    0xBF800000u,  // 243: -1.0f
    0x40000000u,  // 244:  2.0f
    0xC0000000u,  // 245: -2.0f
    0x40800000u,  // 246:  4.0f
    0xC0800000u,  // 247: -4.0f
    0x3E22F983u,  // 248:  1.0f / (2.0f * pi)
};

// ---------------------------------------------------------------------------
// GFX10.3 / RDNA2 Opcode Tables (Public AMD RDNA2 ISA Reference Guide)
// ---------------------------------------------------------------------------
Rdna2Opcode DecodeSop2Opcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::S_ADD_U32;
        case 0x01: return Rdna2Opcode::S_SUB_U32;
        case 0x02: return Rdna2Opcode::S_ADD_I32;
        case 0x03: return Rdna2Opcode::S_SUB_I32;
        case 0x06: return Rdna2Opcode::S_MIN_I32;
        case 0x07: return Rdna2Opcode::S_MIN_U32;
        case 0x08: return Rdna2Opcode::S_MAX_I32;
        case 0x09: return Rdna2Opcode::S_MAX_U32;
        case 0x0A: return Rdna2Opcode::S_CSELECT_B32;
        case 0x0E: return Rdna2Opcode::S_AND_B32;
        case 0x10: return Rdna2Opcode::S_OR_B32;
        case 0x12: return Rdna2Opcode::S_XOR_B32;
        case 0x1E: return Rdna2Opcode::S_LSHL_B32;
        case 0x20: return Rdna2Opcode::S_LSHR_B32;
        case 0x22: return Rdna2Opcode::S_ASHR_I32;
        case 0x2C: return Rdna2Opcode::S_MUL_I32;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeSop2Opcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::S_ADD_U32:     return 0x00;
        case Rdna2Opcode::S_SUB_U32:     return 0x01;
        case Rdna2Opcode::S_ADD_I32:     return 0x02;
        case Rdna2Opcode::S_SUB_I32:     return 0x03;
        case Rdna2Opcode::S_MIN_I32:     return 0x06;
        case Rdna2Opcode::S_MIN_U32:     return 0x07;
        case Rdna2Opcode::S_MAX_I32:     return 0x08;
        case Rdna2Opcode::S_MAX_U32:     return 0x09;
        case Rdna2Opcode::S_CSELECT_B32: return 0x0A;
        case Rdna2Opcode::S_AND_B32:     return 0x0E;
        case Rdna2Opcode::S_OR_B32:      return 0x10;
        case Rdna2Opcode::S_XOR_B32:     return 0x12;
        case Rdna2Opcode::S_LSHL_B32:    return 0x1E;
        case Rdna2Opcode::S_LSHR_B32:    return 0x20;
        case Rdna2Opcode::S_ASHR_I32:    return 0x22;
        case Rdna2Opcode::S_MUL_I32:     return 0x2C;
        default:                         return 0xFFFF;
    }
}

Rdna2Opcode DecodeSop1Opcode(uint16_t raw) {
    switch (raw) {
        case 0x03: return Rdna2Opcode::S_MOV_B32;
        case 0x07: return Rdna2Opcode::S_NOT_B32;
        case 0x1F: return Rdna2Opcode::S_GETPC_B64;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeSop1Opcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::S_MOV_B32:   return 0x03;
        case Rdna2Opcode::S_NOT_B32:   return 0x07;
        case Rdna2Opcode::S_GETPC_B64: return 0x1F;
        default:                       return 0xFFFF;
    }
}

Rdna2Opcode DecodeSopcOpcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::S_CMP_EQ_I32;
        case 0x01: return Rdna2Opcode::S_CMP_LG_I32;
        case 0x02: return Rdna2Opcode::S_CMP_GT_I32;
        case 0x03: return Rdna2Opcode::S_CMP_GE_I32;
        case 0x04: return Rdna2Opcode::S_CMP_LT_I32;
        case 0x05: return Rdna2Opcode::S_CMP_LE_I32;
        case 0x06: return Rdna2Opcode::S_CMP_EQ_U32;
        case 0x07: return Rdna2Opcode::S_CMP_LG_U32;
        case 0x08: return Rdna2Opcode::S_CMP_GT_U32;
        case 0x09: return Rdna2Opcode::S_CMP_GE_U32;
        case 0x0A: return Rdna2Opcode::S_CMP_LT_U32;
        case 0x0B: return Rdna2Opcode::S_CMP_LE_U32;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeSopcOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::S_CMP_EQ_I32: return 0x00;
        case Rdna2Opcode::S_CMP_LG_I32: return 0x01;
        case Rdna2Opcode::S_CMP_GT_I32: return 0x02;
        case Rdna2Opcode::S_CMP_GE_I32: return 0x03;
        case Rdna2Opcode::S_CMP_LT_I32: return 0x04;
        case Rdna2Opcode::S_CMP_LE_I32: return 0x05;
        case Rdna2Opcode::S_CMP_EQ_U32: return 0x06;
        case Rdna2Opcode::S_CMP_LG_U32: return 0x07;
        case Rdna2Opcode::S_CMP_GT_U32: return 0x08;
        case Rdna2Opcode::S_CMP_GE_U32: return 0x09;
        case Rdna2Opcode::S_CMP_LT_U32: return 0x0A;
        case Rdna2Opcode::S_CMP_LE_U32: return 0x0B;
        default:                        return 0xFFFF;
    }
}

Rdna2Opcode DecodeSopkOpcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::S_MOVK_I32;
        case 0x0F: return Rdna2Opcode::S_ADDK_I32;
        case 0x10: return Rdna2Opcode::S_MULK_I32;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeSopkOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::S_MOVK_I32: return 0x00;
        case Rdna2Opcode::S_ADDK_I32: return 0x0F;
        case Rdna2Opcode::S_MULK_I32: return 0x10;
        default:                      return 0xFFFF;
    }
}

Rdna2Opcode DecodeSoppOpcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::S_NOP;
        case 0x01: return Rdna2Opcode::S_ENDPGM;
        case 0x02: return Rdna2Opcode::S_BRANCH;
        case 0x04: return Rdna2Opcode::S_CBRANCH_SCC0;
        case 0x05: return Rdna2Opcode::S_CBRANCH_SCC1;
        case 0x06: return Rdna2Opcode::S_CBRANCH_VCCZ;
        case 0x07: return Rdna2Opcode::S_CBRANCH_VCCNZ;
        case 0x08: return Rdna2Opcode::S_CBRANCH_EXECZ;
        case 0x09: return Rdna2Opcode::S_CBRANCH_EXECNZ;
        case 0x0A: return Rdna2Opcode::S_BARRIER;
        case 0x0C: return Rdna2Opcode::S_WAITCNT;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeSoppOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::S_NOP:            return 0x00;
        case Rdna2Opcode::S_ENDPGM:         return 0x01;
        case Rdna2Opcode::S_BRANCH:         return 0x02;
        case Rdna2Opcode::S_CBRANCH_SCC0:   return 0x04;
        case Rdna2Opcode::S_CBRANCH_SCC1:   return 0x05;
        case Rdna2Opcode::S_CBRANCH_VCCZ:   return 0x06;
        case Rdna2Opcode::S_CBRANCH_VCCNZ:  return 0x07;
        case Rdna2Opcode::S_CBRANCH_EXECZ:  return 0x08;
        case Rdna2Opcode::S_CBRANCH_EXECNZ: return 0x09;
        case Rdna2Opcode::S_BARRIER:        return 0x0A;
        case Rdna2Opcode::S_WAITCNT:        return 0x0C;
        default:                            return 0xFFFF;
    }
}

Rdna2Opcode DecodeVop2Opcode(uint16_t raw) {
    switch (raw) {
        case 0x01: return Rdna2Opcode::V_CNDMASK_B32;
        case 0x03: return Rdna2Opcode::V_ADD_F32;
        case 0x04: return Rdna2Opcode::V_SUB_F32;
        case 0x05: return Rdna2Opcode::V_SUBREV_F32;
        case 0x08: return Rdna2Opcode::V_MUL_F32;
        case 0x0F: return Rdna2Opcode::V_MIN_F32;
        case 0x10: return Rdna2Opcode::V_MAX_F32;
        case 0x11: return Rdna2Opcode::V_MIN_I32;
        case 0x12: return Rdna2Opcode::V_MAX_I32;
        case 0x13: return Rdna2Opcode::V_MIN_U32;
        case 0x14: return Rdna2Opcode::V_MAX_U32;
        case 0x16: return Rdna2Opcode::V_LSHRREV_B32;
        case 0x17: return Rdna2Opcode::V_ASHRREV_I32;
        case 0x18: return Rdna2Opcode::V_LSHLREV_B32;
        case 0x1B: return Rdna2Opcode::V_AND_B32;
        case 0x1C: return Rdna2Opcode::V_OR_B32;
        case 0x1D: return Rdna2Opcode::V_XOR_B32;
        case 0x25: return Rdna2Opcode::V_ADD_NC_U32;
        case 0x26: return Rdna2Opcode::V_SUB_NC_U32;
        case 0x2B: return Rdna2Opcode::V_FMAC_F32;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeVop2Opcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::V_CNDMASK_B32: return 0x01;
        case Rdna2Opcode::V_ADD_F32:     return 0x03;
        case Rdna2Opcode::V_SUB_F32:     return 0x04;
        case Rdna2Opcode::V_SUBREV_F32:  return 0x05;
        case Rdna2Opcode::V_MUL_F32:     return 0x08;
        case Rdna2Opcode::V_MIN_F32:     return 0x0F;
        case Rdna2Opcode::V_MAX_F32:     return 0x10;
        case Rdna2Opcode::V_MIN_I32:     return 0x11;
        case Rdna2Opcode::V_MAX_I32:     return 0x12;
        case Rdna2Opcode::V_MIN_U32:     return 0x13;
        case Rdna2Opcode::V_MAX_U32:     return 0x14;
        case Rdna2Opcode::V_LSHRREV_B32: return 0x16;
        case Rdna2Opcode::V_ASHRREV_I32: return 0x17;
        case Rdna2Opcode::V_LSHLREV_B32: return 0x18;
        case Rdna2Opcode::V_AND_B32:     return 0x1B;
        case Rdna2Opcode::V_OR_B32:      return 0x1C;
        case Rdna2Opcode::V_XOR_B32:     return 0x1D;
        case Rdna2Opcode::V_ADD_NC_U32:  return 0x25;
        case Rdna2Opcode::V_SUB_NC_U32:  return 0x26;
        case Rdna2Opcode::V_FMAC_F32:    return 0x2B;
        default:                         return 0xFFFF;
    }
}

Rdna2Opcode DecodeVop1Opcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::V_NOP;
        case 0x01: return Rdna2Opcode::V_MOV_B32;
        case 0x05: return Rdna2Opcode::V_CVT_F32_I32;
        case 0x06: return Rdna2Opcode::V_CVT_F32_U32;
        case 0x07: return Rdna2Opcode::V_CVT_U32_F32;
        case 0x08: return Rdna2Opcode::V_CVT_I32_F32;
        case 0x20: return Rdna2Opcode::V_FRACT_F32;
        case 0x24: return Rdna2Opcode::V_FLOOR_F32;
        case 0x2A: return Rdna2Opcode::V_RCP_F32;
        case 0x2E: return Rdna2Opcode::V_RSQ_F32;
        case 0x33: return Rdna2Opcode::V_SQRT_F32;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeVop1Opcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::V_NOP:         return 0x00;
        case Rdna2Opcode::V_MOV_B32:     return 0x01;
        case Rdna2Opcode::V_CVT_F32_I32: return 0x05;
        case Rdna2Opcode::V_CVT_F32_U32: return 0x06;
        case Rdna2Opcode::V_CVT_U32_F32: return 0x07;
        case Rdna2Opcode::V_CVT_I32_F32: return 0x08;
        case Rdna2Opcode::V_FRACT_F32:   return 0x20;
        case Rdna2Opcode::V_FLOOR_F32:   return 0x24;
        case Rdna2Opcode::V_RCP_F32:     return 0x2A;
        case Rdna2Opcode::V_RSQ_F32:     return 0x2E;
        case Rdna2Opcode::V_SQRT_F32:    return 0x33;
        default:                         return 0xFFFF;
    }
}

Rdna2Opcode DecodeVopcOpcode(uint16_t raw) {
    switch (raw) {
        case 0x11: return Rdna2Opcode::V_CMP_LT_F32;
        case 0x12: return Rdna2Opcode::V_CMP_EQ_F32;
        case 0x13: return Rdna2Opcode::V_CMP_LE_F32;
        case 0x14: return Rdna2Opcode::V_CMP_GT_F32;
        case 0x15: return Rdna2Opcode::V_CMP_LG_F32;
        case 0x16: return Rdna2Opcode::V_CMP_GE_F32;
        case 0x81: return Rdna2Opcode::V_CMP_LT_I32;
        case 0x82: return Rdna2Opcode::V_CMP_EQ_I32;
        case 0x84: return Rdna2Opcode::V_CMP_GT_I32;
        case 0x86: return Rdna2Opcode::V_CMP_GE_I32;
        case 0xC1: return Rdna2Opcode::V_CMP_LT_U32;
        case 0xC2: return Rdna2Opcode::V_CMP_EQ_U32;
        case 0xC4: return Rdna2Opcode::V_CMP_GT_U32;
        case 0xC6: return Rdna2Opcode::V_CMP_GE_U32;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeVopcOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::V_CMP_LT_F32: return 0x11;
        case Rdna2Opcode::V_CMP_EQ_F32: return 0x12;
        case Rdna2Opcode::V_CMP_LE_F32: return 0x13;
        case Rdna2Opcode::V_CMP_GT_F32: return 0x14;
        case Rdna2Opcode::V_CMP_LG_F32: return 0x15;
        case Rdna2Opcode::V_CMP_GE_F32: return 0x16;
        case Rdna2Opcode::V_CMP_LT_I32: return 0x81;
        case Rdna2Opcode::V_CMP_EQ_I32: return 0x82;
        case Rdna2Opcode::V_CMP_GT_I32: return 0x84;
        case Rdna2Opcode::V_CMP_GE_I32: return 0x86;
        case Rdna2Opcode::V_CMP_LT_U32: return 0xC1;
        case Rdna2Opcode::V_CMP_EQ_U32: return 0xC2;
        case Rdna2Opcode::V_CMP_GT_U32: return 0xC4;
        case Rdna2Opcode::V_CMP_GE_U32: return 0xC6;
        default:                        return 0xFFFF;
    }
}

Rdna2Opcode DecodeVop3Opcode(uint16_t raw, uint8_t& out_src_count) {
    // Native 3-operand VOP3 opcodes (GFX10.3)
    switch (raw) {
        case 0x101: out_src_count = 3; return Rdna2Opcode::V_CNDMASK_B32;
        case 0x141: out_src_count = 3; return Rdna2Opcode::V_MAD_F32;
        case 0x14B: out_src_count = 3; return Rdna2Opcode::V_FMA_F32;
        case 0x36D: out_src_count = 3; return Rdna2Opcode::V_ADD3_U32;
        case 0x373: out_src_count = 3; return Rdna2Opcode::V_DOT2_F32_F16;
        default: break;
    }
    // Promoted VOPC (0x000..0x0FF)
    if (raw < 0x100u) {
        Rdna2Opcode op = DecodeVopcOpcode(raw);
        if (op != Rdna2Opcode::Unknown) {
            out_src_count = 2;
            return op;
        }
    }
    // Promoted VOP2 (0x100..0x13F)
    if (raw >= 0x100u && raw < 0x140u) {
        Rdna2Opcode op = DecodeVop2Opcode(static_cast<uint16_t>(raw - 0x100u));
        if (op != Rdna2Opcode::Unknown) {
            out_src_count = 2;
            return op;
        }
    }
    // Promoted VOP1 (0x180..0x1FF)
    if (raw >= 0x180u && raw < 0x200u) {
        Rdna2Opcode op = DecodeVop1Opcode(static_cast<uint16_t>(raw - 0x180u));
        if (op != Rdna2Opcode::Unknown) {
            out_src_count = 1;
            return op;
        }
    }
    out_src_count = 3;
    return Rdna2Opcode::Unknown;
}

uint16_t EncodeVop3Opcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::V_CNDMASK_B32:  return 0x101;
        case Rdna2Opcode::V_MAD_F32:      return 0x141;
        case Rdna2Opcode::V_FMA_F32:      return 0x14B;
        case Rdna2Opcode::V_ADD3_U32:     return 0x36D;
        case Rdna2Opcode::V_DOT2_F32_F16: return 0x373;
        default: break;
    }
    uint16_t v2 = EncodeVop2Opcode(op);
    if (v2 != 0xFFFFu) return static_cast<uint16_t>(0x100u + v2);
    uint16_t v1 = EncodeVop1Opcode(op);
    if (v1 != 0xFFFFu) return static_cast<uint16_t>(0x180u + v1);
    uint16_t vc = EncodeVopcOpcode(op);
    if (vc != 0xFFFFu) return vc;
    return 0xFFFFu;
}

Rdna2Opcode DecodeSmemOpcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::S_LOAD_DWORD;
        case 0x08: return Rdna2Opcode::S_BUFFER_LOAD_DWORD;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeSmemOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::S_LOAD_DWORD:        return 0x00;
        case Rdna2Opcode::S_BUFFER_LOAD_DWORD: return 0x08;
        default:                               return 0xFFFF;
    }
}

Rdna2Opcode DecodeMubufOpcode(uint16_t raw) {
    switch (raw) {
        case 0x14: return Rdna2Opcode::BUFFER_LOAD_DWORD;
        case 0x1C: return Rdna2Opcode::BUFFER_STORE_DWORD;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeMubufOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::BUFFER_LOAD_DWORD:  return 0x14;
        case Rdna2Opcode::BUFFER_STORE_DWORD: return 0x1C;
        default:                              return 0xFFFF;
    }
}

Rdna2Opcode DecodeFlatOpcode(uint16_t raw, uint8_t seg) {
    if (seg == 2u) {
        switch (raw) {
            case 0x0C: return Rdna2Opcode::GLOBAL_LOAD_DWORD;
            case 0x1C: return Rdna2Opcode::GLOBAL_STORE_DWORD;
            default:   return Rdna2Opcode::Unknown;
        }
    }
    if (seg == 0u) {
        switch (raw) {
            case 0x0C: return Rdna2Opcode::FLAT_LOAD_DWORD;
            case 0x1C: return Rdna2Opcode::FLAT_STORE_DWORD;
            default:   return Rdna2Opcode::Unknown;
        }
    }
    return Rdna2Opcode::Unknown;
}

uint16_t EncodeFlatOpcode(Rdna2Opcode op, uint8_t& out_seg) {
    switch (op) {
        case Rdna2Opcode::GLOBAL_LOAD_DWORD:  out_seg = 2; return 0x0C;
        case Rdna2Opcode::GLOBAL_STORE_DWORD: out_seg = 2; return 0x1C;
        case Rdna2Opcode::FLAT_LOAD_DWORD:    out_seg = 0; return 0x0C;
        case Rdna2Opcode::FLAT_STORE_DWORD:   out_seg = 0; return 0x1C;
        default:                              out_seg = 0; return 0xFFFF;
    }
}

std::string FormatExpTarget(uint8_t target) {
    if (target <= 7u) {
        return "mrt" + std::to_string(target);
    }
    if (target == 8u) return "mrtz";
    if (target == 9u) return "null";
    if (target >= 12u && target <= 15u) {
        return "pos" + std::to_string(target - 12u);
    }
    if (target >= 32u && target <= 63u) {
        return "param" + std::to_string(target - 32u);
    }
    return "tgt" + std::to_string(target);
}

}  // namespace

// ---------------------------------------------------------------------------
// XS2\0 Container Serialization & Parsing
// ---------------------------------------------------------------------------
Status ParseXs2Container(const std::vector<uint8_t>& bytes, Xs2ShaderContainer& out_container) {
    out_container = Xs2ShaderContainer{};
    if (bytes.size() < kXs2HeaderSizeBytes) {
        return Status::Error(StatusCode::ParseError, "XS2 container smaller than 40-byte header");
    }
    if (std::memcmp(bytes.data(), "XS2\0", 4) != 0) {
        return Status::Error(StatusCode::UnsupportedFormat, "Invalid XS2 container magic (expected 'XS2\\0')");
    }

    const uint32_t version = ReadLe32(bytes.data() + 4);
    const uint32_t stage_raw = ReadLe32(bytes.data() + 8);
    const uint32_t sgpr_count = ReadLe32(bytes.data() + 12);
    const uint32_t vgpr_count = ReadLe32(bytes.data() + 16);
    const uint32_t tg_x = ReadLe32(bytes.data() + 20);
    const uint32_t tg_y = ReadLe32(bytes.data() + 24);
    const uint32_t tg_z = ReadLe32(bytes.data() + 28);
    const uint32_t entry_offset = ReadLe32(bytes.data() + 32);
    const uint32_t code_size_bytes = ReadLe32(bytes.data() + 36);

    if (version == 0 || version > kXs2ContainerVersion) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported XS2 container version");
    }

    const auto stage = static_cast<ShaderStage>(stage_raw);
    if (stage != ShaderStage::Vertex && stage != ShaderStage::Pixel && stage != ShaderStage::Compute) {
        return Status::Error(StatusCode::ParseError, "Unsupported XS2 shader stage (expected VS/PS/CS)");
    }

    if (entry_offset < kXs2HeaderSizeBytes || (entry_offset & 3u) != 0u) {
        return Status::Error(StatusCode::ParseError, "Invalid XS2 entry_offset");
    }
    if (code_size_bytes == 0 || (code_size_bytes & 3u) != 0u) {
        return Status::Error(StatusCode::ParseError, "Invalid XS2 code_size_bytes (must be non-zero multiple of 4)");
    }
    const uint64_t code_end = static_cast<uint64_t>(entry_offset) + static_cast<uint64_t>(code_size_bytes);
    if (code_end > bytes.size()) {
        return Status::Error(StatusCode::ParseError, "XS2 code section extends past end of container");
    }

    out_container.version = version;
    out_container.stage = stage;
    out_container.sgpr_count = sgpr_count;
    out_container.vgpr_count = vgpr_count;
    out_container.threadgroup_x = tg_x == 0 ? 1u : tg_x;
    out_container.threadgroup_y = tg_y == 0 ? 1u : tg_y;
    out_container.threadgroup_z = tg_z == 0 ? 1u : tg_z;
    out_container.entry_offset = entry_offset;

    const uint32_t word_count = code_size_bytes / 4u;
    out_container.code_words.resize(word_count);
    for (uint32_t i = 0; i < word_count; ++i) {
        out_container.code_words[i] = ReadLe32(bytes.data() + entry_offset + i * 4u);
    }
    return Status::Ok();
}

std::vector<uint8_t> BuildXs2Container(const Xs2ShaderContainer& container) {
    const uint32_t entry_offset = container.entry_offset >= kXs2HeaderSizeBytes
                                      ? ((container.entry_offset + 3u) & ~3u)
                                      : kXs2HeaderSizeBytes;
    const uint32_t code_size_bytes = static_cast<uint32_t>(container.code_words.size() * 4u);

    std::vector<uint8_t> out;
    out.reserve(entry_offset + code_size_bytes);
    out.push_back('X');
    out.push_back('S');
    out.push_back('2');
    out.push_back('\0');
    AppendLe32(out, container.version ? container.version : kXs2ContainerVersion);
    AppendLe32(out, static_cast<uint32_t>(container.stage));
    AppendLe32(out, container.sgpr_count);
    AppendLe32(out, container.vgpr_count);
    AppendLe32(out, container.threadgroup_x ? container.threadgroup_x : 1u);
    AppendLe32(out, container.threadgroup_y ? container.threadgroup_y : 1u);
    AppendLe32(out, container.threadgroup_z ? container.threadgroup_z : 1u);
    AppendLe32(out, entry_offset);
    AppendLe32(out, code_size_bytes);

    while (out.size() < entry_offset) {
        out.push_back(0u);
    }
    for (uint32_t w : container.code_words) {
        AppendLe32(out, w);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Operand Factories & Codec
// ---------------------------------------------------------------------------
Rdna2Operand Rdna2Operand::Sgpr(uint8_t idx) {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::Sgpr;
    op.raw_code = idx;
    op.reg_index = idx;
    return op;
}

Rdna2Operand Rdna2Operand::Vgpr(uint8_t idx) {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::Vgpr;
    op.raw_code = static_cast<uint16_t>(256u + idx);
    op.reg_index = idx;
    return op;
}

Rdna2Operand Rdna2Operand::InlineInt(int32_t val) {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::InlineInt;
    op.int_value = val;
    if (val >= 0 && val <= 64) {
        op.raw_code = static_cast<uint16_t>(128 + val);
    } else if (val >= -16 && val <= -1) {
        op.raw_code = static_cast<uint16_t>(192 - val);
    } else {
        op.kind = Rdna2OperandKind::Literal32;
        op.raw_code = 255;
        op.literal_bits = static_cast<uint32_t>(val);
    }
    return op;
}

Rdna2Operand Rdna2Operand::InlineFloat(float val) {
    const uint32_t bits = FloatToBits(val);
    if (bits == 0x00000000u) {
        Rdna2Operand op{};
        op.kind = Rdna2OperandKind::InlineInt;
        op.raw_code = 128;
        op.int_value = 0;
        op.float_value = 0.0f;
        return op;
    }
    for (uint16_t i = 0; i < 9; ++i) {
        if (bits == kInlineFloatBits[i]) {
            Rdna2Operand op{};
            op.kind = Rdna2OperandKind::InlineFloat;
            op.raw_code = static_cast<uint16_t>(240u + i);
            op.float_value = BitsToFloat(bits);
            op.literal_bits = bits;
            return op;
        }
    }
    return LiteralF32(val);
}

Rdna2Operand Rdna2Operand::LiteralU32(uint32_t bits) {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::Literal32;
    op.raw_code = 255;
    op.literal_bits = bits;
    op.int_value = static_cast<int32_t>(bits);
    op.float_value = BitsToFloat(bits);
    return op;
}

Rdna2Operand Rdna2Operand::LiteralF32(float val) {
    return LiteralU32(FloatToBits(val));
}

Rdna2Operand Rdna2Operand::VccLo() {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::VccLo;
    op.raw_code = 106;
    return op;
}

Rdna2Operand Rdna2Operand::Scc() {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::Scc;
    op.raw_code = 253;
    return op;
}

Rdna2Operand Rdna2Operand::Null() {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::Null;
    op.raw_code = 125;
    return op;
}

Rdna2Operand DecodeRdna2SourceOperand(uint16_t code, uint32_t literal_bits) {
    Rdna2Operand op{};
    op.raw_code = code & 0x1FFu;
    const uint16_t c = op.raw_code;

    if (c <= 105u) {
        op.kind = Rdna2OperandKind::Sgpr;
        op.reg_index = c;
    } else if (c == 106u) {
        op.kind = Rdna2OperandKind::VccLo;
    } else if (c == 107u) {
        op.kind = Rdna2OperandKind::VccHi;
    } else if (c == 124u) {
        op.kind = Rdna2OperandKind::M0;
    } else if (c == 125u) {
        op.kind = Rdna2OperandKind::Null;
    } else if (c == 126u) {
        op.kind = Rdna2OperandKind::ExecLo;
    } else if (c == 127u) {
        op.kind = Rdna2OperandKind::ExecHi;
    } else if (c >= 128u && c <= 192u) {
        op.kind = Rdna2OperandKind::InlineInt;
        op.int_value = static_cast<int32_t>(c) - 128;
        op.literal_bits = static_cast<uint32_t>(op.int_value);
    } else if (c >= 193u && c <= 208u) {
        op.kind = Rdna2OperandKind::InlineInt;
        op.int_value = -static_cast<int32_t>(c - 192u);
        op.literal_bits = static_cast<uint32_t>(op.int_value);
    } else if (c >= 240u && c <= 248u) {
        op.kind = Rdna2OperandKind::InlineFloat;
        op.literal_bits = kInlineFloatBits[c - 240u];
        op.float_value = BitsToFloat(op.literal_bits);
    } else if (c == 253u) {
        op.kind = Rdna2OperandKind::Scc;
    } else if (c == 255u) {
        op.kind = Rdna2OperandKind::Literal32;
        op.literal_bits = literal_bits;
        op.int_value = static_cast<int32_t>(literal_bits);
        op.float_value = BitsToFloat(literal_bits);
    } else if (c >= 256u && c <= 511u) {
        op.kind = Rdna2OperandKind::Vgpr;
        op.reg_index = static_cast<uint16_t>(c - 256u);
    } else {
        op.kind = Rdna2OperandKind::Null;
    }
    return op;
}

bool EncodeRdna2SourceOperand(
    const Rdna2Operand& op,
    uint16_t& out_code,
    uint32_t& out_literal,
    bool& out_needs_literal) {
    out_needs_literal = false;
    out_literal = 0;
    switch (op.kind) {
        case Rdna2OperandKind::Sgpr:
            if (op.reg_index > 105u) return false;
            out_code = op.reg_index;
            return true;
        case Rdna2OperandKind::Vgpr:
            if (op.reg_index > 255u) return false;
            out_code = static_cast<uint16_t>(256u + op.reg_index);
            return true;
        case Rdna2OperandKind::VccLo:
            out_code = 106u;
            return true;
        case Rdna2OperandKind::VccHi:
            out_code = 107u;
            return true;
        case Rdna2OperandKind::M0:
            out_code = 124u;
            return true;
        case Rdna2OperandKind::Null:
            out_code = 125u;
            return true;
        case Rdna2OperandKind::ExecLo:
            out_code = 126u;
            return true;
        case Rdna2OperandKind::ExecHi:
            out_code = 127u;
            return true;
        case Rdna2OperandKind::Scc:
            out_code = 253u;
            return true;
        case Rdna2OperandKind::InlineInt:
            if (op.int_value >= 0 && op.int_value <= 64) {
                out_code = static_cast<uint16_t>(128 + op.int_value);
                return true;
            }
            if (op.int_value >= -16 && op.int_value <= -1) {
                out_code = static_cast<uint16_t>(192 - op.int_value);
                return true;
            }
            out_code = 255u;
            out_literal = static_cast<uint32_t>(op.int_value);
            out_needs_literal = true;
            return true;
        case Rdna2OperandKind::InlineFloat: {
            const uint32_t bits = FloatToBits(op.float_value);
            if (bits == 0u) {
                out_code = 128u;
                return true;
            }
            for (uint16_t i = 0; i < 9; ++i) {
                if (bits == kInlineFloatBits[i]) {
                    out_code = static_cast<uint16_t>(240u + i);
                    return true;
                }
            }
            out_code = 255u;
            out_literal = bits;
            out_needs_literal = true;
            return true;
        }
        case Rdna2OperandKind::Literal32:
            out_code = 255u;
            out_literal = op.literal_bits;
            out_needs_literal = true;
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Names & Disassembly Formatting
// ---------------------------------------------------------------------------
const char* Rdna2EncodingName(Rdna2Encoding enc) noexcept {
    switch (enc) {
        case Rdna2Encoding::SOP2:  return "SOP2";
        case Rdna2Encoding::SOP1:  return "SOP1";
        case Rdna2Encoding::SOPC:  return "SOPC";
        case Rdna2Encoding::SOPK:  return "SOPK";
        case Rdna2Encoding::SOPP:  return "SOPP";
        case Rdna2Encoding::VOP2:  return "VOP2";
        case Rdna2Encoding::VOP1:  return "VOP1";
        case Rdna2Encoding::VOPC:  return "VOPC";
        case Rdna2Encoding::VOP3:  return "VOP3";
        case Rdna2Encoding::SMEM:  return "SMEM";
        case Rdna2Encoding::MUBUF: return "MUBUF";
        case Rdna2Encoding::FLAT:  return "FLAT";
        case Rdna2Encoding::EXP:   return "EXP";
        default:                   return "UNKNOWN";
    }
}

const char* Rdna2OpcodeName(Rdna2Opcode op) noexcept {
    switch (op) {
        case Rdna2Opcode::S_ADD_U32:          return "s_add_u32";
        case Rdna2Opcode::S_SUB_U32:          return "s_sub_u32";
        case Rdna2Opcode::S_ADD_I32:          return "s_add_i32";
        case Rdna2Opcode::S_SUB_I32:          return "s_sub_i32";
        case Rdna2Opcode::S_MIN_I32:          return "s_min_i32";
        case Rdna2Opcode::S_MIN_U32:          return "s_min_u32";
        case Rdna2Opcode::S_MAX_I32:          return "s_max_i32";
        case Rdna2Opcode::S_MAX_U32:          return "s_max_u32";
        case Rdna2Opcode::S_CSELECT_B32:      return "s_cselect_b32";
        case Rdna2Opcode::S_AND_B32:          return "s_and_b32";
        case Rdna2Opcode::S_OR_B32:           return "s_or_b32";
        case Rdna2Opcode::S_XOR_B32:          return "s_xor_b32";
        case Rdna2Opcode::S_LSHL_B32:         return "s_lshl_b32";
        case Rdna2Opcode::S_LSHR_B32:         return "s_lshr_b32";
        case Rdna2Opcode::S_ASHR_I32:         return "s_ashr_i32";
        case Rdna2Opcode::S_MUL_I32:          return "s_mul_i32";
        case Rdna2Opcode::S_MOV_B32:          return "s_mov_b32";
        case Rdna2Opcode::S_NOT_B32:          return "s_not_b32";
        case Rdna2Opcode::S_GETPC_B64:        return "s_getpc_b64";
        case Rdna2Opcode::S_CMP_EQ_I32:       return "s_cmp_eq_i32";
        case Rdna2Opcode::S_CMP_LG_I32:       return "s_cmp_lg_i32";
        case Rdna2Opcode::S_CMP_GT_I32:       return "s_cmp_gt_i32";
        case Rdna2Opcode::S_CMP_GE_I32:       return "s_cmp_ge_i32";
        case Rdna2Opcode::S_CMP_LT_I32:       return "s_cmp_lt_i32";
        case Rdna2Opcode::S_CMP_LE_I32:       return "s_cmp_le_i32";
        case Rdna2Opcode::S_CMP_EQ_U32:       return "s_cmp_eq_u32";
        case Rdna2Opcode::S_CMP_LG_U32:       return "s_cmp_lg_u32";
        case Rdna2Opcode::S_CMP_GT_U32:       return "s_cmp_gt_u32";
        case Rdna2Opcode::S_CMP_GE_U32:       return "s_cmp_ge_u32";
        case Rdna2Opcode::S_CMP_LT_U32:       return "s_cmp_lt_u32";
        case Rdna2Opcode::S_CMP_LE_U32:       return "s_cmp_le_u32";
        case Rdna2Opcode::S_MOVK_I32:         return "s_movk_i32";
        case Rdna2Opcode::S_ADDK_I32:         return "s_addk_i32";
        case Rdna2Opcode::S_MULK_I32:         return "s_mulk_i32";
        case Rdna2Opcode::S_NOP:              return "s_nop";
        case Rdna2Opcode::S_ENDPGM:           return "s_endpgm";
        case Rdna2Opcode::S_BRANCH:           return "s_branch";
        case Rdna2Opcode::S_CBRANCH_SCC0:     return "s_cbranch_scc0";
        case Rdna2Opcode::S_CBRANCH_SCC1:     return "s_cbranch_scc1";
        case Rdna2Opcode::S_CBRANCH_VCCZ:     return "s_cbranch_vccz";
        case Rdna2Opcode::S_CBRANCH_VCCNZ:    return "s_cbranch_vccnz";
        case Rdna2Opcode::S_CBRANCH_EXECZ:    return "s_cbranch_execz";
        case Rdna2Opcode::S_CBRANCH_EXECNZ:   return "s_cbranch_execnz";
        case Rdna2Opcode::S_WAITCNT:          return "s_waitcnt";
        case Rdna2Opcode::S_BARRIER:          return "s_barrier";
        case Rdna2Opcode::V_CNDMASK_B32:      return "v_cndmask_b32";
        case Rdna2Opcode::V_ADD_F32:          return "v_add_f32";
        case Rdna2Opcode::V_SUB_F32:          return "v_sub_f32";
        case Rdna2Opcode::V_SUBREV_F32:       return "v_subrev_f32";
        case Rdna2Opcode::V_MUL_F32:          return "v_mul_f32";
        case Rdna2Opcode::V_MIN_F32:          return "v_min_f32";
        case Rdna2Opcode::V_MAX_F32:          return "v_max_f32";
        case Rdna2Opcode::V_MIN_I32:          return "v_min_i32";
        case Rdna2Opcode::V_MAX_I32:          return "v_max_i32";
        case Rdna2Opcode::V_MIN_U32:          return "v_min_u32";
        case Rdna2Opcode::V_MAX_U32:          return "v_max_u32";
        case Rdna2Opcode::V_FMAC_F32:         return "v_fmac_f32";
        case Rdna2Opcode::V_AND_B32:          return "v_and_b32";
        case Rdna2Opcode::V_OR_B32:           return "v_or_b32";
        case Rdna2Opcode::V_XOR_B32:          return "v_xor_b32";
        case Rdna2Opcode::V_LSHLREV_B32:      return "v_lshlrev_b32";
        case Rdna2Opcode::V_LSHRREV_B32:      return "v_lshrrev_b32";
        case Rdna2Opcode::V_ASHRREV_I32:      return "v_ashrrev_i32";
        case Rdna2Opcode::V_ADD_NC_U32:       return "v_add_nc_u32";
        case Rdna2Opcode::V_SUB_NC_U32:       return "v_sub_nc_u32";
        case Rdna2Opcode::V_NOP:              return "v_nop";
        case Rdna2Opcode::V_MOV_B32:          return "v_mov_b32";
        case Rdna2Opcode::V_CVT_F32_I32:      return "v_cvt_f32_i32";
        case Rdna2Opcode::V_CVT_I32_F32:      return "v_cvt_i32_f32";
        case Rdna2Opcode::V_CVT_F32_U32:      return "v_cvt_f32_u32";
        case Rdna2Opcode::V_CVT_U32_F32:      return "v_cvt_u32_f32";
        case Rdna2Opcode::V_RCP_F32:          return "v_rcp_f32";
        case Rdna2Opcode::V_RSQ_F32:          return "v_rsq_f32";
        case Rdna2Opcode::V_SQRT_F32:         return "v_sqrt_f32";
        case Rdna2Opcode::V_FLOOR_F32:        return "v_floor_f32";
        case Rdna2Opcode::V_FRACT_F32:        return "v_fract_f32";
        case Rdna2Opcode::V_CMP_LT_F32:       return "v_cmp_lt_f32";
        case Rdna2Opcode::V_CMP_EQ_F32:       return "v_cmp_eq_f32";
        case Rdna2Opcode::V_CMP_LE_F32:       return "v_cmp_le_f32";
        case Rdna2Opcode::V_CMP_GT_F32:       return "v_cmp_gt_f32";
        case Rdna2Opcode::V_CMP_LG_F32:       return "v_cmp_lg_f32";
        case Rdna2Opcode::V_CMP_GE_F32:       return "v_cmp_ge_f32";
        case Rdna2Opcode::V_CMP_LT_I32:       return "v_cmp_lt_i32";
        case Rdna2Opcode::V_CMP_EQ_I32:       return "v_cmp_eq_i32";
        case Rdna2Opcode::V_CMP_GT_I32:       return "v_cmp_gt_i32";
        case Rdna2Opcode::V_CMP_GE_I32:       return "v_cmp_ge_i32";
        case Rdna2Opcode::V_CMP_LT_U32:       return "v_cmp_lt_u32";
        case Rdna2Opcode::V_CMP_EQ_U32:       return "v_cmp_eq_u32";
        case Rdna2Opcode::V_CMP_GT_U32:       return "v_cmp_gt_u32";
        case Rdna2Opcode::V_CMP_GE_U32:       return "v_cmp_ge_u32";
        case Rdna2Opcode::V_FMA_F32:          return "v_fma_f32";
        case Rdna2Opcode::V_MAD_F32:          return "v_mad_f32";
        case Rdna2Opcode::V_ADD3_U32:         return "v_add3_u32";
        case Rdna2Opcode::V_DOT2_F32_F16:     return "v_dot2_f32_f16";
        case Rdna2Opcode::S_LOAD_DWORD:       return "s_load_dword";
        case Rdna2Opcode::S_BUFFER_LOAD_DWORD:return "s_buffer_load_dword";
        case Rdna2Opcode::BUFFER_LOAD_DWORD:  return "buffer_load_dword";
        case Rdna2Opcode::BUFFER_STORE_DWORD: return "buffer_store_dword";
        case Rdna2Opcode::GLOBAL_LOAD_DWORD:  return "global_load_dword";
        case Rdna2Opcode::GLOBAL_STORE_DWORD: return "global_store_dword";
        case Rdna2Opcode::FLAT_LOAD_DWORD:    return "flat_load_dword";
        case Rdna2Opcode::FLAT_STORE_DWORD:   return "flat_store_dword";
        case Rdna2Opcode::EXP:                return "exp";
        default:                              return "unknown";
    }
}

std::string FormatRdna2Operand(const Rdna2Operand& op) {
    std::string base;
    char buf[64]{};
    switch (op.kind) {
        case Rdna2OperandKind::Sgpr:
            base = "s" + std::to_string(op.reg_index);
            break;
        case Rdna2OperandKind::Vgpr:
            base = "v" + std::to_string(op.reg_index);
            break;
        case Rdna2OperandKind::InlineInt:
            base = std::to_string(op.int_value);
            break;
        case Rdna2OperandKind::InlineFloat:
            std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(op.float_value));
            base = buf;
            break;
        case Rdna2OperandKind::Literal32:
            std::snprintf(buf, sizeof(buf), "0x%08x", op.literal_bits);
            base = buf;
            break;
        case Rdna2OperandKind::VccLo:  base = "vcc_lo"; break;
        case Rdna2OperandKind::VccHi:  base = "vcc_hi"; break;
        case Rdna2OperandKind::ExecLo: base = "exec_lo"; break;
        case Rdna2OperandKind::ExecHi: base = "exec_hi"; break;
        case Rdna2OperandKind::Scc:    base = "scc"; break;
        case Rdna2OperandKind::M0:     base = "m0"; break;
        case Rdna2OperandKind::Null:   base = "null"; break;
        default:                       base = "<none>"; break;
    }
    if (op.abs) base = "|" + base + "|";
    if (op.neg) base = "-" + base;
    return base;
}

std::string FormatRdna2Instruction(const Rdna2Instruction& inst) {
    std::ostringstream os;
    os << inst.mnemonic;

    switch (inst.encoding) {
        case Rdna2Encoding::SOPP: {
            if (inst.opcode == Rdna2Opcode::S_ENDPGM || inst.opcode == Rdna2Opcode::S_BARRIER) {
                break;
            }
            os << " " << inst.simm16;
            break;
        }
        case Rdna2Encoding::SOPK: {
            os << " " << FormatRdna2Operand(inst.dst) << ", " << inst.simm16;
            break;
        }
        case Rdna2Encoding::SOPC: {
            os << " " << FormatRdna2Operand(inst.src[0]) << ", " << FormatRdna2Operand(inst.src[1]);
            break;
        }
        case Rdna2Encoding::VOPC: {
            os << " vcc_lo, " << FormatRdna2Operand(inst.src[0]) << ", " << FormatRdna2Operand(inst.src[1]);
            break;
        }
        case Rdna2Encoding::SMEM: {
            const uint32_t base_s = static_cast<uint32_t>(inst.sbase) * 2u;
            const uint32_t end_s = (inst.opcode == Rdna2Opcode::S_BUFFER_LOAD_DWORD) ? (base_s + 3u) : (base_s + 1u);
            os << " " << FormatRdna2Operand(inst.dst) << ", s[" << base_s << ":" << end_s << "], ";
            if (inst.mem_offset != 0 || inst.soffset == 0x7Du || inst.soffset == 0x7Cu) {
                os << inst.mem_offset;
            } else {
                os << "s" << static_cast<uint32_t>(inst.soffset);
            }
            break;
        }
        case Rdna2Encoding::MUBUF: {
            const uint32_t base_s = static_cast<uint32_t>(inst.srsrc) * 4u;
            os << " " << FormatRdna2Operand(inst.src[0]) << ", " << FormatRdna2Operand(inst.src[1])
               << ", s[" << base_s << ":" << (base_s + 3u) << "], "
               << FormatRdna2Operand(DecodeRdna2SourceOperand(inst.soffset));
            if (inst.idxen) os << " idxen";
            if (inst.offen) os << " offen";
            if (inst.mem_offset != 0) os << " offset:" << inst.mem_offset;
            break;
        }
        case Rdna2Encoding::FLAT: {
            if (inst.opcode == Rdna2Opcode::GLOBAL_STORE_DWORD || inst.opcode == Rdna2Opcode::FLAT_STORE_DWORD) {
                os << " " << FormatRdna2Operand(inst.src[0]) << ", " << FormatRdna2Operand(inst.src[1]);
            } else {
                os << " " << FormatRdna2Operand(inst.dst) << ", " << FormatRdna2Operand(inst.src[0]);
            }
            if (inst.saddr != 0x7Fu) {
                os << ", s[" << static_cast<uint32_t>(inst.saddr) << ":" << (static_cast<uint32_t>(inst.saddr) + 1u) << "]";
            } else {
                os << ", off";
            }
            if (inst.mem_offset != 0) os << " offset:" << inst.mem_offset;
            break;
        }
        case Rdna2Encoding::EXP: {
            os << " " << FormatExpTarget(inst.exp_target);
            for (uint8_t i = 0; i < 4; ++i) {
                os << ", ";
                if ((inst.exp_en & (1u << i)) != 0u) {
                    os << FormatRdna2Operand(inst.src[i]);
                } else {
                    os << "off";
                }
            }
            if (inst.exp_done) os << " done";
            if (inst.exp_compr) os << " compr";
            if (inst.exp_vm) os << " vm";
            break;
        }
        default: {
            bool first = true;
            if (inst.dst.kind != Rdna2OperandKind::None) {
                os << " " << FormatRdna2Operand(inst.dst);
                first = false;
            }
            for (uint8_t i = 0; i < inst.src_count; ++i) {
                os << (first ? " " : ", ") << FormatRdna2Operand(inst.src[i]);
                first = false;
            }
            if (inst.clamp) os << " clamp";
            break;
        }
    }
    return os.str();
}

// ---------------------------------------------------------------------------
// RDNA2 / GFX10.3 Binary Disassembler
// ---------------------------------------------------------------------------
Status DisassembleRdna2(
    const std::vector<uint32_t>& words,
    std::vector<Rdna2Instruction>& out_instructions) {
    out_instructions.clear();
    if (words.empty()) {
        return Status::Error(StatusCode::ParseError, "Empty RDNA2 instruction stream");
    }

    size_t pc = 0;
    while (pc < words.size()) {
        const uint32_t w0 = words[pc];
        Rdna2Instruction inst{};
        inst.word_offset = static_cast<uint32_t>(pc);
        inst.raw_words[0] = w0;
        inst.word_count = 1;

        auto consume_word = [&](uint32_t& out_w) -> Status {
            if (pc + inst.word_count >= words.size()) {
                return Status::Error(StatusCode::ParseError, "Truncated multi-word RDNA2 instruction");
            }
            out_w = words[pc + inst.word_count];
            inst.raw_words[inst.word_count] = out_w;
            ++inst.word_count;
            return Status::Ok();
        };

        if ((w0 >> 23) == 0x17Du) {
            // SOP1: [31:23] == 101111101
            inst.encoding = Rdna2Encoding::SOP1;
            const uint8_t sdst = static_cast<uint8_t>((w0 >> 16) & 0x7Fu);
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 8) & 0xFFu);
            const uint16_t ssrc0 = static_cast<uint16_t>(w0 & 0xFFu);
            uint32_t lit = 0;
            if (ssrc0 == 255u) {
                if (auto st = consume_word(lit); !st.ok()) return st;
            }
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeSop1Opcode(raw_op);
            inst.dst = Rdna2Operand::Sgpr(sdst);
            if (inst.opcode != Rdna2Opcode::S_GETPC_B64) {
                inst.src[0] = DecodeRdna2SourceOperand(ssrc0, lit);
                inst.src_count = 1;
            }
        } else if ((w0 >> 23) == 0x17Eu) {
            // SOPC: [31:23] == 101111110
            inst.encoding = Rdna2Encoding::SOPC;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 16) & 0x7Fu);
            const uint16_t ssrc1 = static_cast<uint16_t>((w0 >> 8) & 0xFFu);
            const uint16_t ssrc0 = static_cast<uint16_t>(w0 & 0xFFu);
            uint32_t lit = 0;
            if (ssrc0 == 255u || ssrc1 == 255u) {
                if (auto st = consume_word(lit); !st.ok()) return st;
            }
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeSopcOpcode(raw_op);
            inst.dst = Rdna2Operand::Scc();
            inst.src[0] = DecodeRdna2SourceOperand(ssrc0, lit);
            inst.src[1] = DecodeRdna2SourceOperand(ssrc1, lit);
            inst.src_count = 2;
        } else if ((w0 >> 23) == 0x17Fu) {
            // SOPP: [31:23] == 101111111
            inst.encoding = Rdna2Encoding::SOPP;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 16) & 0x7Fu);
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeSoppOpcode(raw_op);
            inst.simm16 = static_cast<int16_t>(w0 & 0xFFFFu);
        } else if ((w0 >> 28) == 0xBu) {
            // SOPK: [31:28] == 1011
            inst.encoding = Rdna2Encoding::SOPK;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 23) & 0x1Fu);
            const uint8_t sdst = static_cast<uint8_t>((w0 >> 16) & 0x7Fu);
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeSopkOpcode(raw_op);
            inst.dst = Rdna2Operand::Sgpr(sdst);
            inst.simm16 = static_cast<int16_t>(w0 & 0xFFFFu);
        } else if ((w0 >> 30) == 0x2u) {
            // SOP2: [31:30] == 10
            inst.encoding = Rdna2Encoding::SOP2;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 23) & 0x7Fu);
            const uint8_t sdst = static_cast<uint8_t>((w0 >> 16) & 0x7Fu);
            const uint16_t ssrc1 = static_cast<uint16_t>((w0 >> 8) & 0xFFu);
            const uint16_t ssrc0 = static_cast<uint16_t>(w0 & 0xFFu);
            uint32_t lit = 0;
            if (ssrc0 == 255u || ssrc1 == 255u) {
                if (auto st = consume_word(lit); !st.ok()) return st;
            }
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeSop2Opcode(raw_op);
            inst.dst = Rdna2Operand::Sgpr(sdst);
            inst.src[0] = DecodeRdna2SourceOperand(ssrc0, lit);
            inst.src[1] = DecodeRdna2SourceOperand(ssrc1, lit);
            inst.src_count = 2;
        } else if ((w0 >> 25) == 0x3Fu) {
            // VOP1: [31:25] == 0111111
            inst.encoding = Rdna2Encoding::VOP1;
            const uint8_t vdst = static_cast<uint8_t>((w0 >> 17) & 0xFFu);
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 9) & 0xFFu);
            const uint16_t src0 = static_cast<uint16_t>(w0 & 0x1FFu);
            uint32_t lit = 0;
            if (src0 == 255u) {
                if (auto st = consume_word(lit); !st.ok()) return st;
            }
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeVop1Opcode(raw_op);
            if (inst.opcode != Rdna2Opcode::V_NOP) {
                inst.dst = Rdna2Operand::Vgpr(vdst);
                inst.src[0] = DecodeRdna2SourceOperand(src0, lit);
                inst.src_count = 1;
            }
        } else if ((w0 >> 25) == 0x3Eu) {
            // VOPC: [31:25] == 0111110
            inst.encoding = Rdna2Encoding::VOPC;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 17) & 0xFFu);
            const uint8_t vsrc1 = static_cast<uint8_t>((w0 >> 9) & 0xFFu);
            const uint16_t src0 = static_cast<uint16_t>(w0 & 0x1FFu);
            uint32_t lit = 0;
            if (src0 == 255u) {
                if (auto st = consume_word(lit); !st.ok()) return st;
            }
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeVopcOpcode(raw_op);
            inst.dst = Rdna2Operand::VccLo();
            inst.src[0] = DecodeRdna2SourceOperand(src0, lit);
            inst.src[1] = Rdna2Operand::Vgpr(vsrc1);
            inst.src_count = 2;
        } else if ((w0 >> 31) == 0x0u) {
            // VOP2: [31] == 0
            inst.encoding = Rdna2Encoding::VOP2;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 25) & 0x3Fu);
            const uint8_t vdst = static_cast<uint8_t>((w0 >> 17) & 0xFFu);
            const uint8_t vsrc1 = static_cast<uint8_t>((w0 >> 9) & 0xFFu);
            const uint16_t src0 = static_cast<uint16_t>(w0 & 0x1FFu);
            uint32_t lit = 0;
            if (src0 == 255u) {
                if (auto st = consume_word(lit); !st.ok()) return st;
            }
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeVop2Opcode(raw_op);
            inst.dst = Rdna2Operand::Vgpr(vdst);
            inst.src[0] = DecodeRdna2SourceOperand(src0, lit);
            inst.src[1] = Rdna2Operand::Vgpr(vsrc1);
            inst.src_count = 2;
            if (inst.opcode == Rdna2Opcode::V_CNDMASK_B32) {
                inst.src[2] = Rdna2Operand::VccLo();
                inst.src_count = 3;
            }
        } else if ((w0 >> 26) == 0x35u) {
            // VOP3 (64-bit, or 96-bit with literal32 in GFX10.3): [31:26] == 110101
            inst.encoding = Rdna2Encoding::VOP3;
            uint32_t w1 = 0;
            if (auto st = consume_word(w1); !st.ok()) return st;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 16) & 0x3FFu);
            inst.clamp = ((w0 >> 15) & 1u) != 0u;
            const uint8_t abs_bits = static_cast<uint8_t>((w0 >> 8) & 0x7u);
            const uint8_t vdst = static_cast<uint8_t>(w0 & 0xFFu);

            const uint8_t neg_bits = static_cast<uint8_t>((w1 >> 29) & 0x7u);
            inst.omod = static_cast<uint8_t>((w1 >> 27) & 0x3u);
            const uint16_t src2 = static_cast<uint16_t>((w1 >> 18) & 0x1FFu);
            const uint16_t src1 = static_cast<uint16_t>((w1 >> 9) & 0x1FFu);
            const uint16_t src0 = static_cast<uint16_t>(w1 & 0x1FFu);

            uint32_t lit = 0;
            if (src0 == 255u || src1 == 255u || src2 == 255u) {
                if (auto st = consume_word(lit); !st.ok()) return st;
            }

            uint8_t sc = 3;
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeVop3Opcode(raw_op, sc);
            inst.src_count = sc;
            if (raw_op < 0x100u) {
                inst.dst = Rdna2Operand::Sgpr(vdst);
            } else {
                inst.dst = Rdna2Operand::Vgpr(vdst);
            }
            const uint16_t raw_srcs[3] = {src0, src1, src2};
            for (uint8_t i = 0; i < sc; ++i) {
                inst.src[i] = DecodeRdna2SourceOperand(raw_srcs[i], lit);
                inst.src[i].abs = ((abs_bits >> i) & 1u) != 0u;
                inst.src[i].neg = ((neg_bits >> i) & 1u) != 0u;
            }
        } else if ((w0 >> 26) == 0x3Du) {
            // SMEM (64-bit): [31:26] == 111101
            inst.encoding = Rdna2Encoding::SMEM;
            uint32_t w1 = 0;
            if (auto st = consume_word(w1); !st.ok()) return st;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 18) & 0xFFu);
            inst.dlc = ((w0 >> 17) & 1u) != 0u;
            inst.glc = ((w0 >> 16) & 1u) != 0u;
            const uint8_t sdata = static_cast<uint8_t>((w0 >> 6) & 0x7Fu);
            inst.sbase = static_cast<uint8_t>(w0 & 0x3Fu);
            inst.soffset = static_cast<uint8_t>((w1 >> 25) & 0x7Fu);
            inst.mem_offset = SignExtend(w1 & 0x1FFFFFu, 21);
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeSmemOpcode(raw_op);
            inst.dst = Rdna2Operand::Sgpr(sdata);
            inst.src[0] = Rdna2Operand::Sgpr(static_cast<uint8_t>(inst.sbase * 2u));
            inst.src_count = 1;
        } else if ((w0 >> 26) == 0x38u) {
            // MUBUF (64-bit): [31:26] == 111000
            inst.encoding = Rdna2Encoding::MUBUF;
            uint32_t w1 = 0;
            if (auto st = consume_word(w1); !st.ok()) return st;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 18) & 0x7Fu);
            inst.slc = ((w0 >> 16) & 1u) != 0u;
            inst.dlc = ((w0 >> 15) & 1u) != 0u;
            inst.glc = ((w0 >> 14) & 1u) != 0u;
            inst.idxen = ((w0 >> 13) & 1u) != 0u;
            inst.offen = ((w0 >> 12) & 1u) != 0u;
            inst.mem_offset = static_cast<int32_t>(w0 & 0xFFFu);
            inst.soffset = static_cast<uint8_t>((w1 >> 24) & 0xFFu);
            inst.srsrc = static_cast<uint8_t>((w1 >> 16) & 0x3Fu);
            const uint8_t vdata = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
            const uint8_t vaddr = static_cast<uint8_t>(w1 & 0xFFu);
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeMubufOpcode(raw_op);
            if (inst.opcode == Rdna2Opcode::BUFFER_LOAD_DWORD) {
                inst.dst = Rdna2Operand::Vgpr(vdata);
            }
            inst.src[0] = Rdna2Operand::Vgpr(vdata);
            inst.src[1] = Rdna2Operand::Vgpr(vaddr);
            inst.src_count = 2;
        } else if ((w0 >> 26) == 0x37u) {
            // FLAT / GLOBAL (64-bit): [31:26] == 110111
            inst.encoding = Rdna2Encoding::FLAT;
            uint32_t w1 = 0;
            if (auto st = consume_word(w1); !st.ok()) return st;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 18) & 0x7Fu);
            inst.slc = ((w0 >> 17) & 1u) != 0u;
            inst.glc = ((w0 >> 16) & 1u) != 0u;
            inst.flat_seg = static_cast<uint8_t>((w0 >> 14) & 0x3u);
            inst.dlc = ((w0 >> 12) & 1u) != 0u;
            inst.mem_offset = (inst.flat_seg == 0u)
                                  ? static_cast<int32_t>(w0 & 0xFFFu)
                                  : SignExtend(w0 & 0xFFFu, 12);
            const uint8_t vdst = static_cast<uint8_t>((w1 >> 24) & 0xFFu);
            inst.saddr = static_cast<uint8_t>((w1 >> 16) & 0x7Fu);
            const uint8_t vdata = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
            const uint8_t vaddr = static_cast<uint8_t>(w1 & 0xFFu);
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeFlatOpcode(raw_op, inst.flat_seg);
            if (inst.opcode == Rdna2Opcode::GLOBAL_LOAD_DWORD || inst.opcode == Rdna2Opcode::FLAT_LOAD_DWORD) {
                inst.dst = Rdna2Operand::Vgpr(vdst);
                inst.src[0] = Rdna2Operand::Vgpr(vaddr);
                inst.src_count = 1;
            } else {
                inst.src[0] = Rdna2Operand::Vgpr(vaddr);
                inst.src[1] = Rdna2Operand::Vgpr(vdata);
                inst.src_count = 2;
            }
        } else if ((w0 >> 26) == 0x3Eu) {
            // EXP (64-bit): [31:26] == 111110
            inst.encoding = Rdna2Encoding::EXP;
            uint32_t w1 = 0;
            if (auto st = consume_word(w1); !st.ok()) return st;
            inst.opcode = Rdna2Opcode::EXP;
            inst.exp_vm = ((w0 >> 12) & 1u) != 0u;
            inst.exp_done = ((w0 >> 11) & 1u) != 0u;
            inst.exp_compr = ((w0 >> 10) & 1u) != 0u;
            inst.exp_target = static_cast<uint8_t>((w0 >> 4) & 0x3Fu);
            inst.exp_en = static_cast<uint8_t>(w0 & 0xFu);
            inst.src[0] = Rdna2Operand::Vgpr(static_cast<uint8_t>(w1 & 0xFFu));
            inst.src[1] = Rdna2Operand::Vgpr(static_cast<uint8_t>((w1 >> 8) & 0xFFu));
            inst.src[2] = Rdna2Operand::Vgpr(static_cast<uint8_t>((w1 >> 16) & 0xFFu));
            inst.src[3] = Rdna2Operand::Vgpr(static_cast<uint8_t>((w1 >> 24) & 0xFFu));
            inst.src_count = 4;
        } else {
            return Status::Error(StatusCode::ParseError, "Unrecognized RDNA2 instruction encoding");
        }

        if (inst.opcode == Rdna2Opcode::Unknown) {
            return Status::Error(
                StatusCode::UnsupportedFormat,
                std::string("Unsupported RDNA2 opcode in ") + Rdna2EncodingName(inst.encoding));
        }

        inst.mnemonic = Rdna2OpcodeName(inst.opcode);
        inst.disasm = FormatRdna2Instruction(inst);
        out_instructions.push_back(std::move(inst));
        pc += inst.word_count;
    }

    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Clean-room RDNA2 / GFX10.3 Binary Assembler
// ---------------------------------------------------------------------------
void Rdna2Assembler::EmitSop2(
    Rdna2Opcode op,
    uint8_t sdst,
    const Rdna2Operand& ssrc0,
    const Rdna2Operand& ssrc1) {
    const uint16_t raw_op = EncodeSop2Opcode(op);
    uint16_t c0 = 0, c1 = 0;
    uint32_t lit0 = 0, lit1 = 0;
    bool need0 = false, need1 = false;
    EncodeRdna2SourceOperand(ssrc0, c0, lit0, need0);
    EncodeRdna2SourceOperand(ssrc1, c1, lit1, need1);
    const uint32_t w0 = (0x2u << 30) | (static_cast<uint32_t>(raw_op & 0x7Fu) << 23) |
                        (static_cast<uint32_t>(sdst & 0x7Fu) << 16) |
                        (static_cast<uint32_t>(c1 & 0xFFu) << 8) |
                        static_cast<uint32_t>(c0 & 0xFFu);
    words_.push_back(w0);
    if (need0) {
        words_.push_back(lit0);
    } else if (need1) {
        words_.push_back(lit1);
    }
}

void Rdna2Assembler::EmitSop1(Rdna2Opcode op, uint8_t sdst, const Rdna2Operand& ssrc0) {
    const uint16_t raw_op = EncodeSop1Opcode(op);
    uint16_t c0 = 0;
    uint32_t lit0 = 0;
    bool need0 = false;
    EncodeRdna2SourceOperand(ssrc0, c0, lit0, need0);
    const uint32_t w0 = (0x17Du << 23) | (static_cast<uint32_t>(sdst & 0x7Fu) << 16) |
                        (static_cast<uint32_t>(raw_op & 0xFFu) << 8) |
                        static_cast<uint32_t>(c0 & 0xFFu);
    words_.push_back(w0);
    if (need0) words_.push_back(lit0);
}

void Rdna2Assembler::EmitSopc(Rdna2Opcode op, const Rdna2Operand& ssrc0, const Rdna2Operand& ssrc1) {
    const uint16_t raw_op = EncodeSopcOpcode(op);
    uint16_t c0 = 0, c1 = 0;
    uint32_t lit0 = 0, lit1 = 0;
    bool need0 = false, need1 = false;
    EncodeRdna2SourceOperand(ssrc0, c0, lit0, need0);
    EncodeRdna2SourceOperand(ssrc1, c1, lit1, need1);
    const uint32_t w0 = (0x17Eu << 23) | (static_cast<uint32_t>(raw_op & 0x7Fu) << 16) |
                        (static_cast<uint32_t>(c1 & 0xFFu) << 8) |
                        static_cast<uint32_t>(c0 & 0xFFu);
    words_.push_back(w0);
    if (need0) {
        words_.push_back(lit0);
    } else if (need1) {
        words_.push_back(lit1);
    }
}

void Rdna2Assembler::EmitSopk(Rdna2Opcode op, uint8_t sdst, int16_t simm16) {
    const uint16_t raw_op = EncodeSopkOpcode(op);
    const uint32_t w0 = (0xBu << 28) | (static_cast<uint32_t>(raw_op & 0x1Fu) << 23) |
                        (static_cast<uint32_t>(sdst & 0x7Fu) << 16) |
                        static_cast<uint32_t>(static_cast<uint16_t>(simm16));
    words_.push_back(w0);
}

uint32_t Rdna2Assembler::EmitSopp(Rdna2Opcode op, int16_t simm16) {
    const uint32_t pos = CurrentWordOffset();
    const uint16_t raw_op = EncodeSoppOpcode(op);
    const uint32_t w0 = (0x17Fu << 23) | (static_cast<uint32_t>(raw_op & 0x7Fu) << 16) |
                        static_cast<uint32_t>(static_cast<uint16_t>(simm16));
    words_.push_back(w0);
    return pos;
}

void Rdna2Assembler::PatchBranch(uint32_t branch_word_offset, uint32_t target_word_offset) {
    if (branch_word_offset >= words_.size()) return;
    const int32_t rel = static_cast<int32_t>(target_word_offset) - static_cast<int32_t>(branch_word_offset + 1u);
    const uint16_t imm = static_cast<uint16_t>(static_cast<int16_t>(rel));
    words_[branch_word_offset] = (words_[branch_word_offset] & 0xFFFF0000u) | static_cast<uint32_t>(imm);
}

void Rdna2Assembler::EmitVop2(Rdna2Opcode op, uint8_t vdst, const Rdna2Operand& src0, uint8_t vsrc1) {
    const uint16_t raw_op = EncodeVop2Opcode(op);
    uint16_t c0 = 0;
    uint32_t lit0 = 0;
    bool need0 = false;
    EncodeRdna2SourceOperand(src0, c0, lit0, need0);
    const uint32_t w0 = (static_cast<uint32_t>(raw_op & 0x3Fu) << 25) |
                        (static_cast<uint32_t>(vdst) << 17) |
                        (static_cast<uint32_t>(vsrc1) << 9) |
                        static_cast<uint32_t>(c0 & 0x1FFu);
    words_.push_back(w0);
    if (need0) words_.push_back(lit0);
}

void Rdna2Assembler::EmitVop1(Rdna2Opcode op, uint8_t vdst, const Rdna2Operand& src0) {
    const uint16_t raw_op = EncodeVop1Opcode(op);
    uint16_t c0 = 0;
    uint32_t lit0 = 0;
    bool need0 = false;
    EncodeRdna2SourceOperand(src0, c0, lit0, need0);
    const uint32_t w0 = (0x3Fu << 25) | (static_cast<uint32_t>(vdst) << 17) |
                        (static_cast<uint32_t>(raw_op & 0xFFu) << 9) |
                        static_cast<uint32_t>(c0 & 0x1FFu);
    words_.push_back(w0);
    if (need0) words_.push_back(lit0);
}

void Rdna2Assembler::EmitVopc(Rdna2Opcode op, const Rdna2Operand& src0, uint8_t vsrc1) {
    const uint16_t raw_op = EncodeVopcOpcode(op);
    uint16_t c0 = 0;
    uint32_t lit0 = 0;
    bool need0 = false;
    EncodeRdna2SourceOperand(src0, c0, lit0, need0);
    const uint32_t w0 = (0x3Eu << 25) | (static_cast<uint32_t>(raw_op & 0xFFu) << 17) |
                        (static_cast<uint32_t>(vsrc1) << 9) |
                        static_cast<uint32_t>(c0 & 0x1FFu);
    words_.push_back(w0);
    if (need0) words_.push_back(lit0);
}

void Rdna2Assembler::EmitVop3(
    Rdna2Opcode op,
    uint8_t vdst,
    const Rdna2Operand& src0,
    const Rdna2Operand& src1,
    const Rdna2Operand& src2,
    bool clamp) {
    const uint16_t raw_op = EncodeVop3Opcode(op);
    uint16_t c0 = 0, c1 = 0, c2 = 0;
    uint32_t lit0 = 0, lit1 = 0, lit2 = 0;
    bool need0 = false, need1 = false, need2 = false;
    EncodeRdna2SourceOperand(src0, c0, lit0, need0);
    EncodeRdna2SourceOperand(src1, c1, lit1, need1);
    EncodeRdna2SourceOperand(src2, c2, lit2, need2);

    const uint32_t abs_bits = (src0.abs ? 1u : 0u) | (src1.abs ? 2u : 0u) | (src2.abs ? 4u : 0u);
    const uint32_t neg_bits = (src0.neg ? 1u : 0u) | (src1.neg ? 2u : 0u) | (src2.neg ? 4u : 0u);

    const uint32_t w0 = (0x35u << 26) | (static_cast<uint32_t>(raw_op & 0x3FFu) << 16) |
                        (clamp ? (1u << 15) : 0u) | (abs_bits << 8) | static_cast<uint32_t>(vdst);
    const uint32_t w1 = (neg_bits << 29) | (static_cast<uint32_t>(c2 & 0x1FFu) << 18) |
                        (static_cast<uint32_t>(c1 & 0x1FFu) << 9) | static_cast<uint32_t>(c0 & 0x1FFu);
    words_.push_back(w0);
    words_.push_back(w1);
    if (need0) {
        words_.push_back(lit0);
    } else if (need1) {
        words_.push_back(lit1);
    } else if (need2) {
        words_.push_back(lit2);
    }
}

void Rdna2Assembler::EmitSmem(
    Rdna2Opcode op,
    uint8_t sdata,
    uint8_t sbase_sgpr,
    int32_t offset,
    uint8_t soffset) {
    const uint16_t raw_op = EncodeSmemOpcode(op);
    const uint32_t sbase = static_cast<uint32_t>(sbase_sgpr >> 1) & 0x3Fu;
    const uint32_t w0 = (0x3Du << 26) | (static_cast<uint32_t>(raw_op & 0xFFu) << 18) |
                        (static_cast<uint32_t>(sdata & 0x7Fu) << 6) | sbase;
    const uint32_t w1 = (static_cast<uint32_t>(soffset & 0x7Fu) << 25) |
                        (static_cast<uint32_t>(offset) & 0x1FFFFFu);
    words_.push_back(w0);
    words_.push_back(w1);
}

void Rdna2Assembler::EmitMubuf(
    Rdna2Opcode op,
    uint8_t vdata,
    uint8_t vaddr,
    uint8_t srsrc_sgpr,
    uint16_t offset,
    bool offen,
    bool idxen,
    uint8_t soffset) {
    const uint16_t raw_op = EncodeMubufOpcode(op);
    const uint32_t srsrc = static_cast<uint32_t>(srsrc_sgpr >> 2) & 0x3Fu;
    const uint32_t w0 = (0x38u << 26) | (static_cast<uint32_t>(raw_op & 0x7Fu) << 18) |
                        (idxen ? (1u << 13) : 0u) | (offen ? (1u << 12) : 0u) |
                        static_cast<uint32_t>(offset & 0xFFFu);
    const uint32_t w1 = (static_cast<uint32_t>(soffset) << 24) | (srsrc << 16) |
                        (static_cast<uint32_t>(vdata) << 8) | static_cast<uint32_t>(vaddr);
    words_.push_back(w0);
    words_.push_back(w1);
}

void Rdna2Assembler::EmitGlobal(
    Rdna2Opcode op,
    uint8_t vdst_or_data,
    uint8_t vaddr,
    int16_t offset,
    uint8_t saddr) {
    uint8_t seg = 2;
    const uint16_t raw_op = EncodeFlatOpcode(op, seg);
    const bool is_load = (op == Rdna2Opcode::GLOBAL_LOAD_DWORD || op == Rdna2Opcode::FLAT_LOAD_DWORD);
    const uint32_t w0 = (0x37u << 26) | (static_cast<uint32_t>(raw_op & 0x7Fu) << 18) |
                        (static_cast<uint32_t>(seg & 0x3u) << 14) |
                        (static_cast<uint32_t>(offset) & 0xFFFu);
    const uint32_t w1 = (is_load ? (static_cast<uint32_t>(vdst_or_data) << 24) : 0u) |
                        (static_cast<uint32_t>(saddr & 0x7Fu) << 16) |
                        (!is_load ? (static_cast<uint32_t>(vdst_or_data) << 8) : 0u) |
                        static_cast<uint32_t>(vaddr);
    words_.push_back(w0);
    words_.push_back(w1);
}

void Rdna2Assembler::EmitExp(
    uint8_t target,
    uint8_t vsrc0,
    uint8_t vsrc1,
    uint8_t vsrc2,
    uint8_t vsrc3,
    uint8_t en,
    bool done,
    bool compr,
    bool vm) {
    const uint32_t w0 = (0x3Eu << 26) | (vm ? (1u << 12) : 0u) | (done ? (1u << 11) : 0u) |
                        (compr ? (1u << 10) : 0u) | (static_cast<uint32_t>(target & 0x3Fu) << 4) |
                        static_cast<uint32_t>(en & 0xFu);
    const uint32_t w1 = static_cast<uint32_t>(vsrc0) | (static_cast<uint32_t>(vsrc1) << 8) |
                        (static_cast<uint32_t>(vsrc2) << 16) | (static_cast<uint32_t>(vsrc3) << 24);
    words_.push_back(w0);
    words_.push_back(w1);
}

std::vector<uint8_t> Rdna2Assembler::BuildXs2(
    ShaderStage stage,
    uint32_t tg_x,
    uint32_t tg_y,
    uint32_t tg_z,
    uint32_t sgpr_count,
    uint32_t vgpr_count) const {
    Xs2ShaderContainer c{};
    c.version = kXs2ContainerVersion;
    c.stage = stage;
    c.sgpr_count = sgpr_count;
    c.vgpr_count = vgpr_count;
    c.threadgroup_x = tg_x;
    c.threadgroup_y = tg_y;
    c.threadgroup_z = tg_z;
    c.entry_offset = kXs2HeaderSizeBytes;
    c.code_words = words_;
    return BuildXs2Container(c);
}

}  // namespace scarlettx
