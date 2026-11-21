#include "scarlettx/gpu/rdna2_lifter.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "scarlettx/gpu/shader_ir.h"

namespace scarlettx {

namespace {

uint32_t FloatToBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

std::string HexU32(uint32_t v) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%08xu", v);
    return buf;
}

// ---------------------------------------------------------------------------
// SPIR-V 1.3 Constants (Khronos SPIR-V 1.3 Specification)
// ---------------------------------------------------------------------------
constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr uint32_t kSpirvVersion13 = 0x00010300u;
constexpr uint32_t kSpirvGeneratorScarlettX = 0x53583200u;

enum SpirvOp : uint16_t {
    SpvOpExtInstImport = 11,
    SpvOpExtInst = 12,
    SpvOpMemoryModel = 14,
    SpvOpEntryPoint = 15,
    SpvOpExecutionMode = 16,
    SpvOpCapability = 17,
    SpvOpTypeVoid = 19,
    SpvOpTypeBool = 20,
    SpvOpTypeInt = 21,
    SpvOpTypeFloat = 22,
    SpvOpTypeVector = 23,
    SpvOpTypeArray = 28,
    SpvOpTypeRuntimeArray = 29,
    SpvOpTypeStruct = 30,
    SpvOpTypePointer = 32,
    SpvOpTypeFunction = 33,
    SpvOpConstantTrue = 41,
    SpvOpConstantFalse = 42,
    SpvOpConstant = 43,
    SpvOpFunction = 54,
    SpvOpFunctionEnd = 56,
    SpvOpVariable = 59,
    SpvOpLoad = 61,
    SpvOpStore = 62,
    SpvOpAccessChain = 65,
    SpvOpDecorate = 71,
    SpvOpMemberDecorate = 72,
    SpvOpCompositeConstruct = 80,
    SpvOpCompositeExtract = 81,
    SpvOpConvertFToU = 109,
    SpvOpConvertFToS = 110,
    SpvOpConvertSToF = 111,
    SpvOpConvertUToF = 112,
    SpvOpBitcast = 124,
    SpvOpIAdd = 128,
    SpvOpFAdd = 129,
    SpvOpISub = 130,
    SpvOpFSub = 131,
    SpvOpIMul = 132,
    SpvOpFMul = 133,
    SpvOpFDiv = 136,
    SpvOpDot = 148,
    SpvOpSelect = 169,
    SpvOpIEqual = 170,
    SpvOpINotEqual = 171,
    SpvOpUGreaterThan = 172,
    SpvOpSGreaterThan = 173,
    SpvOpUGreaterThanEqual = 174,
    SpvOpSGreaterThanEqual = 175,
    SpvOpULessThan = 176,
    SpvOpSLessThan = 177,
    SpvOpFOrdEqual = 180,
    SpvOpFOrdNotEqual = 182,
    SpvOpFOrdLessThan = 184,
    SpvOpFOrdGreaterThan = 186,
    SpvOpFOrdGreaterThanEqual = 188,
    SpvOpShiftRightLogical = 194,
    SpvOpShiftRightArithmetic = 195,
    SpvOpShiftLeftLogical = 196,
    SpvOpBitwiseOr = 197,
    SpvOpBitwiseXor = 198,
    SpvOpBitwiseAnd = 199,
    SpvOpNot = 200,
    SpvOpControlBarrier = 224,
    SpvOpPhi = 245,
    SpvOpLabel = 248,
    SpvOpBranch = 249,
    SpvOpBranchConditional = 250,
    SpvOpReturn = 253,
};

enum GlslStd450Op : uint32_t {
    GlslFloor = 8,
    GlslFract = 10,
    GlslSqrt = 31,
    GlslInverseSqrt = 32,
    GlslFMin = 37,
    GlslUMin = 38,
    GlslSMin = 39,
    GlslFMax = 40,
    GlslUMax = 41,
    GlslSMax = 42,
    GlslFma = 50,
    GlslUnpackHalf2x16 = 58,
};

void EmitSpvInst(std::vector<uint32_t>& buf, uint16_t opcode, const std::vector<uint32_t>& operands) {
    const uint32_t wc = static_cast<uint32_t>(operands.size() + 1u);
    buf.push_back((wc << 16) | static_cast<uint32_t>(opcode));
    buf.insert(buf.end(), operands.begin(), operands.end());
}

void AppendSpvString(std::vector<uint32_t>& operands, const char* str) {
    const size_t len = std::strlen(str) + 1;  // include null terminator
    const size_t words = (len + 3) / 4;
    for (size_t w = 0; w < words; ++w) {
        uint32_t word = 0;
        for (size_t b = 0; b < 4; ++b) {
            const size_t idx = w * 4 + b;
            if (idx < len) {
                word |= static_cast<uint32_t>(static_cast<uint8_t>(str[idx])) << (b * 8);
            }
        }
        operands.push_back(word);
    }
}

bool ModuleUsesStageInput(const Rdna2IrModule& ir) {
    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.op == IrOp::StageInput) return true;
        }
    }
    return false;
}

bool ModuleUsesThreadId(const Rdna2IrModule& ir) {
    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.op == IrOp::ThreadIdX) return true;
        }
    }
    return false;
}

// Helper for SPIR-V canonical value type (integers unified as u32, floats as f32, bools as bool).
enum class SpvValKind : uint8_t {
    U32,
    F32,
    Bool,
};

SpvValKind CanonicalSpvKind(IrType t) {
    if (t == IrType::F32) return SpvValKind::F32;
    if (t == IrType::Bool) return SpvValKind::Bool;
    return SpvValKind::U32;
}

// ---------------------------------------------------------------------------
// HLSL Structured Emitter
// ---------------------------------------------------------------------------
class HlslEmitter {
public:
    explicit HlslEmitter(const Rdna2IrModule& ir) : ir_(ir) {
        for (const auto& bb : ir_.blocks) {
            for (const auto& inst : bb.instructions) {
                if (inst.id != kInvalidIrValue) {
                    defs_[inst.id] = &inst;
                }
            }
        }
    }

    Status Emit(std::string& out_hlsl, std::string& out_target) {
        if (ir_.blocks.empty()) {
            return Status::Error(StatusCode::ShaderError, "Cannot emit HLSL from empty IR module");
        }

        const bool uses_stage_in = ModuleUsesStageInput(ir_);
        const bool uses_tid = ModuleUsesThreadId(ir_);

        std::ostringstream hdr;
        hdr << "// Generated by ScarlettX Clean-Room RDNA2 (XS2) -> HLSL Lifter\n";

        if (ir_.uses_root_constants) {
            hdr << "cbuffer ScarlettRootConstants : register(b0) {\n";
            for (uint32_t i = 0; i <= ir_.max_root_const_index; ++i) {
                hdr << "    uint _rc" << i << ";\n";
            }
            hdr << "};\n";
        }
        if (ir_.uses_uav) {
            hdr << "RWByteAddressBuffer _uav0 : register(u0);\n";
        }

        switch (ir_.stage) {
            case ShaderStage::Compute:
                out_target = "cs_5_0";
                hdr << "[numthreads(" << ir_.threadgroup_x << ", " << ir_.threadgroup_y << ", "
                    << ir_.threadgroup_z << ")]\n";
                hdr << "void main(uint3 _tid : SV_DispatchThreadID) {\n";
                (void)uses_tid;
                break;
            case ShaderStage::Vertex:
                out_target = "vs_5_0";
                hdr << "struct VSOut {\n";
                hdr << "    float4 pos : SV_POSITION;\n";
                if (ir_.exports_param0) {
                    hdr << "    float4 param0 : TEXCOORD0;\n";
                }
                hdr << "};\n";
                hdr << "VSOut main(uint _vid : SV_VertexID) {\n";
                hdr << "    VSOut _out;\n";
                hdr << "    _out.pos = float4(0.0, 0.0, 0.0, 1.0);\n";
                if (ir_.exports_param0) {
                    hdr << "    _out.param0 = float4(0.0, 0.0, 0.0, 1.0);\n";
                }
                break;
            case ShaderStage::Pixel:
                out_target = "ps_5_0";
                hdr << "struct PSIn {\n";
                hdr << "    float4 pos : SV_POSITION;\n";
                if (uses_stage_in) {
                    hdr << "    float4 param0 : TEXCOORD0;\n";
                }
                hdr << "};\n";
                hdr << "float4 main(PSIn _in) : SV_TARGET0 {\n";
                hdr << "    float4 _mrt0 = float4(0.0, 0.0, 0.0, 1.0);\n";
                break;
            default:
                return Status::Error(StatusCode::UnsupportedFormat, "Unsupported shader stage for HLSL emission");
        }

        // Pre-declare all SSA value locals so multi-block structured/fallback CFG always compiles cleanly.
        for (const auto& bb : ir_.blocks) {
            for (const auto& inst : bb.instructions) {
                if (inst.id == kInvalidIrValue) continue;
                if (inst.type == IrType::F32) {
                    hdr << "    float _v" << inst.id << " = 0.0;\n";
                } else if (inst.type == IrType::Bool) {
                    hdr << "    bool _v" << inst.id << " = false;\n";
                } else {
                    hdr << "    uint _v" << inst.id << " = 0u;\n";
                }
            }
        }

        // Try structured CFG emission first; fallback to general loop-based CFG state machine if irreducible.
        std::ostringstream body;
        std::unordered_set<uint32_t> visited;
        if (EmitStructured(body, 0u, UINT32_MAX, 1, visited)) {
            hdr << body.str();
        } else {
            EmitStateMachine(hdr);
            // State machine exits loop via _bb = 0xFFFFFFFFu; emit final return for graphics stages.
            if (ir_.stage == ShaderStage::Vertex) {
                hdr << "    return _out;\n";
            } else if (ir_.stage == ShaderStage::Pixel) {
                hdr << "    return _mrt0;\n";
            }
        }
        hdr << "}\n";

        out_hlsl = hdr.str();
        return Status::Ok();
    }

private:
    std::string Indent(int depth) const {
        return std::string(static_cast<size_t>(depth) * 4u, ' ');
    }

    IrType TypeOf(IrValueId id) const {
        auto it = defs_.find(id);
        return it != defs_.end() ? it->second->type : IrType::U32;
    }

    std::string AsU32(IrValueId id) const {
        const IrType t = TypeOf(id);
        if (t == IrType::F32) return "asuint(_v" + std::to_string(id) + ")";
        if (t == IrType::Bool) return "(_v" + std::to_string(id) + " ? 1u : 0u)";
        return "_v" + std::to_string(id);
    }

    std::string AsI32(IrValueId id) const {
        return "asint(" + AsU32(id) + ")";
    }

    std::string AsF32(IrValueId id) const {
        const IrType t = TypeOf(id);
        if (t == IrType::F32) return "_v" + std::to_string(id);
        return "asfloat(" + AsU32(id) + ")";
    }

    std::string AsBool(IrValueId id) const {
        const IrType t = TypeOf(id);
        if (t == IrType::Bool) return "_v" + std::to_string(id);
        return "(" + AsU32(id) + " != 0u)";
    }

    std::string EmitReturnStmt() const {
        if (ir_.stage == ShaderStage::Vertex) return "return _out;";
        if (ir_.stage == ShaderStage::Pixel) return "return _mrt0;";
        return "return;";
    }

    void EmitPhiCopies(std::ostringstream& os, uint32_t from_bb, uint32_t to_bb, int depth) const {
        if (to_bb >= ir_.blocks.size()) return;
        const auto& dst_bb = ir_.blocks[to_bb];
        for (const auto& inst : dst_bb.instructions) {
            if (inst.op != IrOp::Phi) continue;
            for (const auto& inc : inst.phi_incoming) {
                if (inc.first == from_bb) {
                    os << Indent(depth) << "_v" << inst.id << " = ";
                    if (inst.type == IrType::F32) {
                        os << AsF32(inc.second);
                    } else if (inst.type == IrType::Bool) {
                        os << AsBool(inc.second);
                    } else {
                        os << AsU32(inc.second);
                    }
                    os << ";\n";
                    break;
                }
            }
        }
    }

    bool HasPhiCopies(uint32_t from_bb, uint32_t to_bb) const {
        if (to_bb >= ir_.blocks.size()) return false;
        for (const auto& inst : ir_.blocks[to_bb].instructions) {
            if (inst.op != IrOp::Phi) continue;
            for (const auto& inc : inst.phi_incoming) {
                if (inc.first == from_bb) return true;
            }
        }
        return false;
    }

    void EmitNonTerminators(std::ostringstream& os, const IrBasicBlock& bb, int depth) const {
        const std::string ind = Indent(depth);
        for (const auto& inst : bb.instructions) {
            if (inst.op == IrOp::Phi || inst.op == IrOp::Branch ||
                inst.op == IrOp::CondBranch || inst.op == IrOp::Return) {
                continue;
            }

            switch (inst.op) {
                case IrOp::ConstU32:
                    if (inst.type == IrType::Bool) {
                        os << ind << "_v" << inst.id << " = " << (inst.imm_u32 != 0u ? "true" : "false") << ";\n";
                    } else {
                        os << ind << "_v" << inst.id << " = " << HexU32(inst.imm_u32) << ";\n";
                    }
                    break;
                case IrOp::ConstF32:
                    os << ind << "_v" << inst.id << " = asfloat(" << HexU32(FloatToBits(inst.imm_f32)) << ");\n";
                    break;
                case IrOp::ThreadIdX:
                    if (ir_.stage == ShaderStage::Vertex) {
                        os << ind << "_v" << inst.id << " = _vid;\n";
                    } else {
                        os << ind << "_v" << inst.id << " = _tid.x;\n";
                    }
                    break;
                case IrOp::StageInput: {
                    static constexpr char kSwiz[4] = {'x', 'y', 'z', 'w'};
                    os << ind << "_v" << inst.id << " = _in.param0." << kSwiz[inst.imm_u32 & 3u] << ";\n";
                    break;
                }
                case IrOp::LoadRootConst:
                    os << ind << "_v" << inst.id << " = _rc" << inst.imm_u32 << ";\n";
                    break;
                case IrOp::LoadUav:
                    os << ind << "_v" << inst.id << " = _uav0.Load(" << AsU32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::StoreUav:
                    os << ind << "_uav0.Store(" << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FAdd:
                    os << ind << "_v" << inst.id << " = (" << AsF32(inst.operands[0]) << " + " << AsF32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FSub:
                    os << ind << "_v" << inst.id << " = (" << AsF32(inst.operands[0]) << " - " << AsF32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FMul:
                    os << ind << "_v" << inst.id << " = (" << AsF32(inst.operands[0]) << " * " << AsF32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FFma:
                    os << ind << "_v" << inst.id << " = mad(" << AsF32(inst.operands[0]) << ", "
                       << AsF32(inst.operands[1]) << ", " << AsF32(inst.operands[2]) << ");\n";
                    break;
                case IrOp::FMin:
                    os << ind << "_v" << inst.id << " = min(" << AsF32(inst.operands[0]) << ", " << AsF32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FMax:
                    os << ind << "_v" << inst.id << " = max(" << AsF32(inst.operands[0]) << ", " << AsF32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FRcp:
                    os << ind << "_v" << inst.id << " = rcp(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::FRsq:
                    os << ind << "_v" << inst.id << " = rsqrt(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::FSqrt:
                    os << ind << "_v" << inst.id << " = sqrt(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::FFloor:
                    os << ind << "_v" << inst.id << " = floor(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::FFract:
                    os << ind << "_v" << inst.id << " = frac(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::FDot2: {
                    const std::string a = AsU32(inst.operands[0]);
                    const std::string b = AsU32(inst.operands[1]);
                    const std::string c = AsF32(inst.operands[2]);
                    os << ind << "_v" << inst.id << " = (f16tof32(" << a << " & 0xFFFFu) * f16tof32(" << b
                       << " & 0xFFFFu) + f16tof32((" << a << " >> 16u) & 0xFFFFu) * f16tof32((" << b
                       << " >> 16u) & 0xFFFFu) + " << c << ");\n";
                    break;
                }
                case IrOp::IAdd:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " + " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::ISub:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " - " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::IMul:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " * " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::IMin:
                    if (inst.type == IrType::I32) {
                        os << ind << "_v" << inst.id << " = asuint(min(" << AsI32(inst.operands[0]) << ", " << AsI32(inst.operands[1]) << "));\n";
                    } else {
                        os << ind << "_v" << inst.id << " = min(" << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1]) << ");\n";
                    }
                    break;
                case IrOp::IMax:
                    if (inst.type == IrType::I32) {
                        os << ind << "_v" << inst.id << " = asuint(max(" << AsI32(inst.operands[0]) << ", " << AsI32(inst.operands[1]) << "));\n";
                    } else {
                        os << ind << "_v" << inst.id << " = max(" << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1]) << ");\n";
                    }
                    break;
                case IrOp::BitAnd:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " & " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::BitOr:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " | " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::BitXor:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " ^ " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::BitNot:
                    os << ind << "_v" << inst.id << " = (~" << AsU32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::Shl:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " << (" << AsU32(inst.operands[1]) << " & 31u));\n";
                    break;
                case IrOp::Shr:
                    if (inst.type == IrType::I32) {
                        os << ind << "_v" << inst.id << " = asuint(" << AsI32(inst.operands[0]) << " >> (" << AsU32(inst.operands[1]) << " & 31u));\n";
                    } else {
                        os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " >> (" << AsU32(inst.operands[1]) << " & 31u));\n";
                    }
                    break;
                case IrOp::I2F:
                    os << ind << "_v" << inst.id << " = (float)(" << AsI32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::U2F:
                    os << ind << "_v" << inst.id << " = (float)(" << AsU32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::F2I:
                    os << ind << "_v" << inst.id << " = asuint((int)(" << AsF32(inst.operands[0]) << "));\n";
                    break;
                case IrOp::F2U:
                    os << ind << "_v" << inst.id << " = (uint)(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::CmpEq:
                case IrOp::CmpNe:
                case IrOp::CmpLt:
                case IrOp::CmpGt:
                case IrOp::CmpGe: {
                    const bool is_flt = (TypeOf(inst.operands[0]) == IrType::F32 || TypeOf(inst.operands[1]) == IrType::F32);
                    const std::string a = is_flt ? AsF32(inst.operands[0]) : AsI32(inst.operands[0]);
                    const std::string b = is_flt ? AsF32(inst.operands[1]) : AsI32(inst.operands[1]);
                    const char* sym = "==";
                    if (inst.op == IrOp::CmpNe) sym = "!=";
                    else if (inst.op == IrOp::CmpLt) sym = "<";
                    else if (inst.op == IrOp::CmpGt) sym = ">";
                    else if (inst.op == IrOp::CmpGe) sym = ">=";
                    os << ind << "_v" << inst.id << " = (" << a << " " << sym << " " << b << ");\n";
                    break;
                }
                case IrOp::CmpLtU:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " < " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::CmpEqU:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " == " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::CmpGtU:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " > " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::CmpGeU:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " >= " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::Select:
                    if (inst.type == IrType::F32) {
                        os << ind << "_v" << inst.id << " = (" << AsBool(inst.operands[0]) << " ? "
                           << AsF32(inst.operands[1]) << " : " << AsF32(inst.operands[2]) << ");\n";
                    } else if (inst.type == IrType::Bool) {
                        os << ind << "_v" << inst.id << " = (" << AsBool(inst.operands[0]) << " ? "
                           << AsBool(inst.operands[1]) << " : " << AsBool(inst.operands[2]) << ");\n";
                    } else {
                        os << ind << "_v" << inst.id << " = (" << AsBool(inst.operands[0]) << " ? "
                           << AsU32(inst.operands[1]) << " : " << AsU32(inst.operands[2]) << ");\n";
                    }
                    break;
                case IrOp::Barrier:
                    if (ir_.stage == ShaderStage::Compute) {
                        os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                    }
                    break;
                case IrOp::Export: {
                    const std::string vec = "float4(" + AsF32(inst.operands[0]) + ", " + AsF32(inst.operands[1]) +
                                            ", " + AsF32(inst.operands[2]) + ", " + AsF32(inst.operands[3]) + ")";
                    if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0) &&
                        ir_.stage == ShaderStage::Vertex) {
                        os << ind << "_out.pos = " << vec << ";\n";
                    } else if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Param0) &&
                               ir_.stage == ShaderStage::Vertex) {
                        os << ind << "_out.param0 = " << vec << ";\n";
                    } else if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Mrt0) &&
                               ir_.stage == ShaderStage::Pixel) {
                        os << ind << "_mrt0 = " << vec << ";\n";
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    // Finds the single immediate branch target of a straight-line block, or UINT32_MAX.
    uint32_t SingleBranchTarget(uint32_t bb_id) const {
        if (bb_id >= ir_.blocks.size()) return UINT32_MAX;
        const auto& ins = ir_.blocks[bb_id].instructions;
        if (ins.empty()) return UINT32_MAX;
        if (ins.back().op == IrOp::Branch) return ins.back().true_block;
        return UINT32_MAX;
    }

    bool IsReturnBlock(uint32_t bb_id) const {
        if (bb_id >= ir_.blocks.size()) return false;
        const auto& ins = ir_.blocks[bb_id].instructions;
        return !ins.empty() && ins.back().op == IrOp::Return;
    }

    bool EmitStructured(
        std::ostringstream& os,
        uint32_t bb_id,
        uint32_t stop_bb,
        int depth,
        std::unordered_set<uint32_t>& active) const {
        while (bb_id != stop_bb) {
            if (bb_id >= ir_.blocks.size()) return false;
            if (!active.insert(bb_id).second) return false;  // cycle -> fallback to state machine

            const auto& bb = ir_.blocks[bb_id];
            EmitNonTerminators(os, bb, depth);
            if (bb.instructions.empty()) {
                active.erase(bb_id);
                return true;
            }

            const auto& term = bb.instructions.back();
            if (term.op == IrOp::Return) {
                os << Indent(depth) << EmitReturnStmt() << "\n";
                active.erase(bb_id);
                return true;
            }
            if (term.op == IrOp::Branch) {
                const uint32_t next = term.true_block;
                EmitPhiCopies(os, bb_id, next, depth);
                active.erase(bb_id);
                bb_id = next;
                continue;
            }
            if (term.op == IrOp::CondBranch) {
                const uint32_t t_bb = term.true_block;
                const uint32_t f_bb = term.false_block;
                const std::string cond = AsBool(term.operands[0]);

                const uint32_t t_next = SingleBranchTarget(t_bb);
                const uint32_t f_next = SingleBranchTarget(f_bb);

                // Pattern A: if (cond) { T } -> join F
                if (t_bb != f_bb && t_next == f_bb && t_bb != stop_bb) {
                    os << Indent(depth) << "if (" << cond << ") {\n";
                    if (!EmitStructured(os, t_bb, f_bb, depth + 1, active)) return false;
                    os << Indent(depth) << "}";
                    if (HasPhiCopies(bb_id, f_bb)) {
                        os << " else {\n";
                        EmitPhiCopies(os, bb_id, f_bb, depth + 1);
                        os << Indent(depth) << "}";
                    }
                    os << "\n";
                    active.erase(bb_id);
                    bb_id = f_bb;
                    continue;
                }

                // Pattern A': if (!cond) { F } -> join T
                if (t_bb != f_bb && f_next == t_bb && f_bb != stop_bb) {
                    os << Indent(depth) << "if (!(" << cond << ")) {\n";
                    if (!EmitStructured(os, f_bb, t_bb, depth + 1, active)) return false;
                    os << Indent(depth) << "}";
                    if (HasPhiCopies(bb_id, t_bb)) {
                        os << " else {\n";
                        EmitPhiCopies(os, bb_id, t_bb, depth + 1);
                        os << Indent(depth) << "}";
                    }
                    os << "\n";
                    active.erase(bb_id);
                    bb_id = t_bb;
                    continue;
                }

                // Pattern B: if (cond) { T } else { F } -> join J
                if (t_bb != f_bb && t_next != UINT32_MAX && t_next == f_next) {
                    const uint32_t join_bb = t_next;
                    os << Indent(depth) << "if (" << cond << ") {\n";
                    if (!EmitStructured(os, t_bb, join_bb, depth + 1, active)) return false;
                    os << Indent(depth) << "} else {\n";
                    if (!EmitStructured(os, f_bb, join_bb, depth + 1, active)) return false;
                    os << Indent(depth) << "}\n";
                    active.erase(bb_id);
                    bb_id = join_bb;
                    continue;
                }

                // Pattern C: early return on either branch
                if (IsReturnBlock(t_bb) && !IsReturnBlock(f_bb)) {
                    os << Indent(depth) << "if (" << cond << ") {\n";
                    if (!EmitStructured(os, t_bb, UINT32_MAX, depth + 1, active)) return false;
                    os << Indent(depth) << "}\n";
                    active.erase(bb_id);
                    bb_id = f_bb;
                    continue;
                }
                if (IsReturnBlock(f_bb) && !IsReturnBlock(t_bb)) {
                    os << Indent(depth) << "if (!(" << cond << ")) {\n";
                    if (!EmitStructured(os, f_bb, UINT32_MAX, depth + 1, active)) return false;
                    os << Indent(depth) << "}\n";
                    active.erase(bb_id);
                    bb_id = t_bb;
                    continue;
                }
                if (IsReturnBlock(t_bb) && IsReturnBlock(f_bb)) {
                    os << Indent(depth) << "if (" << cond << ") {\n";
                    if (!EmitStructured(os, t_bb, UINT32_MAX, depth + 1, active)) return false;
                    os << Indent(depth) << "} else {\n";
                    if (!EmitStructured(os, f_bb, UINT32_MAX, depth + 1, active)) return false;
                    os << Indent(depth) << "}\n";
                    active.erase(bb_id);
                    return true;
                }

                return false;  // Complex CFG -> state machine fallback
            }

            active.erase(bb_id);
            return true;
        }
        return true;
    }

    void EmitStateMachine(std::ostringstream& os) const {
        os << "    uint _bb = 0u;\n";
        os << "    [loop] while (_bb != 0xFFFFFFFFu) {\n";
        for (size_t i = 0; i < ir_.blocks.size(); ++i) {
            const auto& bb = ir_.blocks[i];
            os << "        " << (i == 0 ? "if" : "else if") << " (_bb == " << bb.id << "u) {\n";
            EmitNonTerminators(os, bb, 3);
            if (!bb.instructions.empty()) {
                const auto& term = bb.instructions.back();
                if (term.op == IrOp::Return) {
                    os << "            _bb = 0xFFFFFFFFu;\n";
                } else if (term.op == IrOp::Branch) {
                    EmitPhiCopies(os, bb.id, term.true_block, 3);
                    os << "            _bb = " << term.true_block << "u;\n";
                } else if (term.op == IrOp::CondBranch) {
                    os << "            if (" << AsBool(term.operands[0]) << ") {\n";
                    EmitPhiCopies(os, bb.id, term.true_block, 4);
                    os << "                _bb = " << term.true_block << "u;\n";
                    os << "            } else {\n";
                    EmitPhiCopies(os, bb.id, term.false_block, 4);
                    os << "                _bb = " << term.false_block << "u;\n";
                    os << "            }\n";
                } else {
                    os << "            _bb = 0xFFFFFFFFu;\n";
                }
            } else {
                os << "            _bb = 0xFFFFFFFFu;\n";
            }
            os << "        }\n";
        }
        os << "        else { _bb = 0xFFFFFFFFu; }\n";
        os << "    }\n";
    }

    const Rdna2IrModule& ir_;
    std::unordered_map<IrValueId, const IrInstruction*> defs_;
};

}  // namespace

// ---------------------------------------------------------------------------
// SPIR-V 1.3 Binary Emitter
// ---------------------------------------------------------------------------
Status EmitSpirv(const Rdna2IrModule& ir, std::vector<uint32_t>& out_spirv) {
    out_spirv.clear();
    if (ir.blocks.empty()) {
        return Status::Error(StatusCode::ShaderError, "Cannot emit SPIR-V from empty IR module");
    }

    uint32_t next_id = 1;
    auto alloc_id = [&]() -> uint32_t { return next_id++; };

    std::vector<uint32_t> sec_caps;
    std::vector<uint32_t> sec_ext;
    std::vector<uint32_t> sec_mem;
    std::vector<uint32_t> sec_entry;
    std::vector<uint32_t> sec_exec;
    std::vector<uint32_t> sec_decor;
    std::vector<uint32_t> sec_types;
    std::vector<uint32_t> sec_globals;
    std::vector<uint32_t> sec_fn;

    // 1. Capabilities, ExtInstImport, MemoryModel
    EmitSpvInst(sec_caps, SpvOpCapability, {1u});  // Shader
    const uint32_t glsl_ext_id = alloc_id();
    {
        std::vector<uint32_t> ops = {glsl_ext_id};
        AppendSpvString(ops, "GLSL.std.450");
        EmitSpvInst(sec_ext, SpvOpExtInstImport, ops);
    }
    EmitSpvInst(sec_mem, SpvOpMemoryModel, {0u, 1u});  // Logical, GLSL450

    // 2. Core types
    const uint32_t t_void = alloc_id();
    const uint32_t t_bool = alloc_id();
    const uint32_t t_u32 = alloc_id();
    const uint32_t t_i32 = alloc_id();
    const uint32_t t_f32 = alloc_id();
    const uint32_t t_v2f32 = alloc_id();
    const uint32_t t_v3u32 = alloc_id();
    const uint32_t t_v4f32 = alloc_id();
    const uint32_t t_fn_void = alloc_id();

    EmitSpvInst(sec_types, SpvOpTypeVoid, {t_void});
    EmitSpvInst(sec_types, SpvOpTypeBool, {t_bool});
    EmitSpvInst(sec_types, SpvOpTypeInt, {t_u32, 32u, 0u});
    EmitSpvInst(sec_types, SpvOpTypeInt, {t_i32, 32u, 1u});
    EmitSpvInst(sec_types, SpvOpTypeFloat, {t_f32, 32u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v2f32, t_f32, 2u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v3u32, t_u32, 3u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v4f32, t_f32, 4u});
    EmitSpvInst(sec_types, SpvOpTypeFunction, {t_fn_void, t_void});

    // Deduplicated constant pool (emitted into sec_types)
    std::unordered_map<uint32_t, uint32_t> const_u32_map;
    std::unordered_map<uint32_t, uint32_t> const_f32_map;
    uint32_t const_true_id = 0;
    uint32_t const_false_id = 0;

    auto get_const_u32 = [&](uint32_t v) -> uint32_t {
        auto it = const_u32_map.find(v);
        if (it != const_u32_map.end()) return it->second;
        uint32_t id = alloc_id();
        EmitSpvInst(sec_types, SpvOpConstant, {t_u32, id, v});
        const_u32_map[v] = id;
        return id;
    };

    auto get_const_f32 = [&](float f) -> uint32_t {
        const uint32_t bits = FloatToBits(f);
        auto it = const_f32_map.find(bits);
        if (it != const_f32_map.end()) return it->second;
        uint32_t id = alloc_id();
        EmitSpvInst(sec_types, SpvOpConstant, {t_f32, id, bits});
        const_f32_map[bits] = id;
        return id;
    };

    auto get_const_bool = [&](bool v) -> uint32_t {
        if (v) {
            if (!const_true_id) {
                const_true_id = alloc_id();
                EmitSpvInst(sec_types, SpvOpConstantTrue, {t_bool, const_true_id});
            }
            return const_true_id;
        }
        if (!const_false_id) {
            const_false_id = alloc_id();
            EmitSpvInst(sec_types, SpvOpConstantFalse, {t_bool, const_false_id});
        }
        return const_false_id;
    };

    const uint32_t c_u32_0 = get_const_u32(0u);
    const uint32_t c_u32_1 = get_const_u32(1u);
    const uint32_t c_u32_2 = get_const_u32(2u);
    const uint32_t c_u32_31 = get_const_u32(31u);
    const uint32_t c_f32_1 = get_const_f32(1.0f);

    // 3. Interface variables & descriptors
    std::vector<uint32_t> interface_vars;
    uint32_t var_gid = 0;
    uint32_t t_ptr_in_u32 = 0;
    uint32_t var_vid = 0;
    uint32_t var_pos0 = 0;
    uint32_t var_param0_out = 0;
    uint32_t var_param0_in = 0;
    uint32_t var_mrt0 = 0;
    uint32_t var_uav0 = 0;
    uint32_t t_ptr_unif_u32 = 0;
    uint32_t var_rc = 0;

    if (ir.uses_uav || ir.uses_root_constants) {
        t_ptr_unif_u32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_unif_u32, 2u, t_u32});  // Uniform
    }

    if (ir.uses_uav) {
        const uint32_t t_rta_u32 = alloc_id();
        const uint32_t t_uav_struct = alloc_id();
        const uint32_t t_ptr_uav_struct = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeRuntimeArray, {t_rta_u32, t_u32});
        EmitSpvInst(sec_types, SpvOpTypeStruct, {t_uav_struct, t_rta_u32});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uav_struct, 2u, t_uav_struct});  // Uniform
        var_uav0 = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uav_struct, var_uav0, 2u});

        EmitSpvInst(sec_decor, SpvOpDecorate, {t_rta_u32, 6u, 4u});          // ArrayStride 4
        EmitSpvInst(sec_decor, SpvOpMemberDecorate, {t_uav_struct, 0u, 35u, 0u});  // Offset 0
        EmitSpvInst(sec_decor, SpvOpDecorate, {t_uav_struct, 3u});           // BufferBlock
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_uav0, 34u, 0u});          // DescriptorSet 0
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_uav0, 33u, 0u});          // Binding 0
    }

    if (ir.uses_root_constants) {
        const uint32_t rc_count = ir.max_root_const_index + 1u;
        const uint32_t c_rc_len = get_const_u32(rc_count);
        const uint32_t t_rc_arr = alloc_id();
        const uint32_t t_rc_struct = alloc_id();
        const uint32_t t_ptr_rc_struct = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeArray, {t_rc_arr, t_u32, c_rc_len});
        EmitSpvInst(sec_types, SpvOpTypeStruct, {t_rc_struct, t_rc_arr});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_rc_struct, 2u, t_rc_struct});  // Uniform
        var_rc = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_rc_struct, var_rc, 2u});

        EmitSpvInst(sec_decor, SpvOpDecorate, {t_rc_arr, 6u, 4u});           // ArrayStride 4
        EmitSpvInst(sec_decor, SpvOpMemberDecorate, {t_rc_struct, 0u, 35u, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {t_rc_struct, 2u});            // Block
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_rc, 34u, 0u});            // DescriptorSet 0
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_rc, 33u, 1u});            // Binding 1
    }

    const bool uses_tid = ModuleUsesThreadId(ir);
    const bool uses_stage_in = ModuleUsesStageInput(ir);

    if (ir.stage == ShaderStage::Compute && uses_tid) {
        const uint32_t t_ptr_in_v3u32 = alloc_id();
        t_ptr_in_u32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_v3u32, 1u, t_v3u32});  // Input
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_u32, 1u, t_u32});
        var_gid = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_in_v3u32, var_gid, 1u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_gid, 11u, 28u});  // BuiltIn GlobalInvocationId
        interface_vars.push_back(var_gid);
    }

    if (ir.stage == ShaderStage::Vertex) {
        if (uses_tid) {
            t_ptr_in_u32 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_u32, 1u, t_u32});
            var_vid = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_in_u32, var_vid, 1u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_vid, 11u, 42u});  // BuiltIn VertexIndex
            interface_vars.push_back(var_vid);
        }
        const uint32_t t_ptr_out_v4f32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_v4f32, 3u, t_v4f32});  // Output
        var_pos0 = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_out_v4f32, var_pos0, 3u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_pos0, 11u, 0u});  // BuiltIn Position
        interface_vars.push_back(var_pos0);

        if (ir.exports_param0) {
            var_param0_out = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_out_v4f32, var_param0_out, 3u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_param0_out, 30u, 0u});  // Location 0
            interface_vars.push_back(var_param0_out);
        }
    }

    if (ir.stage == ShaderStage::Pixel) {
        if (uses_stage_in) {
            const uint32_t t_ptr_in_v4f32 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_v4f32, 1u, t_v4f32});
            var_param0_in = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_in_v4f32, var_param0_in, 1u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_param0_in, 30u, 0u});  // Location 0
            interface_vars.push_back(var_param0_in);
        }
        const uint32_t t_ptr_out_v4f32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_v4f32, 3u, t_v4f32});
        var_mrt0 = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_out_v4f32, var_mrt0, 3u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_mrt0, 30u, 0u});  // Location 0
        interface_vars.push_back(var_mrt0);
    }

    // 4. Entry point & execution mode
    const uint32_t main_fn_id = alloc_id();
    {
        uint32_t exec_model = 5u;  // GLCompute
        if (ir.stage == ShaderStage::Vertex) exec_model = 0u;
        else if (ir.stage == ShaderStage::Pixel) exec_model = 4u;

        std::vector<uint32_t> ops = {exec_model, main_fn_id};
        AppendSpvString(ops, "main");
        ops.insert(ops.end(), interface_vars.begin(), interface_vars.end());
        EmitSpvInst(sec_entry, SpvOpEntryPoint, ops);

        if (ir.stage == ShaderStage::Compute) {
            EmitSpvInst(sec_exec, SpvOpExecutionMode,
                        {main_fn_id, 17u, ir.threadgroup_x, ir.threadgroup_y, ir.threadgroup_z});
        } else if (ir.stage == ShaderStage::Pixel) {
            EmitSpvInst(sec_exec, SpvOpExecutionMode, {main_fn_id, 7u});  // OriginUpperLeft
        }
    }

    // 5. Pre-allocate block labels and value IDs.
    std::vector<uint32_t> bb_labels(ir.blocks.size(), 0u);
    for (size_t i = 0; i < ir.blocks.size(); ++i) {
        bb_labels[i] = alloc_id();
    }

    std::unordered_map<IrValueId, uint32_t> val_spv_id;
    std::unordered_map<IrValueId, SpvValKind> val_kind;
    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.id == kInvalidIrValue) continue;
            val_kind[inst.id] = CanonicalSpvKind(inst.type);
            if (inst.op == IrOp::ConstU32) {
                if (inst.type == IrType::Bool) {
                    val_spv_id[inst.id] = get_const_bool(inst.imm_u32 != 0u);
                } else {
                    val_spv_id[inst.id] = get_const_u32(inst.imm_u32);
                }
            } else if (inst.op == IrOp::ConstF32) {
                val_spv_id[inst.id] = get_const_f32(inst.imm_f32);
            } else {
                val_spv_id[inst.id] = alloc_id();
            }
        }
    }

    auto spv_type_for_kind = [&](SpvValKind k) -> uint32_t {
        if (k == SpvValKind::F32) return t_f32;
        if (k == SpvValKind::Bool) return t_bool;
        return t_u32;
    };

    auto get_as_u32 = [&](IrValueId v) -> uint32_t {
        const uint32_t id = val_spv_id[v];
        const SpvValKind k = val_kind[v];
        if (k == SpvValKind::U32) return id;
        if (k == SpvValKind::F32) {
            uint32_t cast_id = alloc_id();
            EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, cast_id, id});
            return cast_id;
        }
        uint32_t sel_id = alloc_id();
        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, sel_id, id, c_u32_1, c_u32_0});
        return sel_id;
    };

    auto get_as_i32 = [&](IrValueId v) -> uint32_t {
        uint32_t u = get_as_u32(v);
        uint32_t i = alloc_id();
        EmitSpvInst(sec_fn, SpvOpBitcast, {t_i32, i, u});
        return i;
    };

    auto get_as_f32 = [&](IrValueId v) -> uint32_t {
        const uint32_t id = val_spv_id[v];
        const SpvValKind k = val_kind[v];
        if (k == SpvValKind::F32) return id;
        uint32_t u = get_as_u32(v);
        uint32_t cast_id = alloc_id();
        EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, cast_id, u});
        return cast_id;
    };

    auto get_as_bool = [&](IrValueId v) -> uint32_t {
        const uint32_t id = val_spv_id[v];
        const SpvValKind k = val_kind[v];
        if (k == SpvValKind::Bool) return id;
        uint32_t u = get_as_u32(v);
        uint32_t cmp_id = alloc_id();
        EmitSpvInst(sec_fn, SpvOpINotEqual, {t_bool, cmp_id, u, c_u32_0});
        return cmp_id;
    };

    // Precompute any Phi incoming edge bitcasts (so they can be emitted at the end of the predecessor block).
    struct PhiEdgeFixup {
        uint32_t target_phi_id;
        SpvValKind expected_kind;
        IrValueId incoming_val;
        uint32_t coerced_spv_id;
    };
    std::unordered_map<uint64_t, uint32_t> phi_edge_override;  // (phi_ir_id << 32 | pred_bb) -> spv_id
    std::vector<std::vector<PhiEdgeFixup>> block_exit_fixups(ir.blocks.size());

    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.op != IrOp::Phi) continue;
            const SpvValKind expected = CanonicalSpvKind(inst.type);
            for (const auto& inc : inst.phi_incoming) {
                const uint32_t pred = inc.first;
                const IrValueId v = inc.second;
                if (val_kind[v] != expected && pred < ir.blocks.size()) {
                    const uint32_t coerced = alloc_id();
                    block_exit_fixups[pred].push_back({inst.id, expected, v, coerced});
                    phi_edge_override[(static_cast<uint64_t>(inst.id) << 32) | pred] = coerced;
                }
            }
        }
    }

    // 6. Emit Function Body
    EmitSpvInst(sec_fn, SpvOpFunction, {t_void, main_fn_id, 0u, t_fn_void});

    for (size_t bi = 0; bi < ir.blocks.size(); ++bi) {
        const auto& bb = ir.blocks[bi];
        EmitSpvInst(sec_fn, SpvOpLabel, {bb_labels[bi]});

        // Emit OpPhi instructions strictly first after OpLabel.
        for (const auto& inst : bb.instructions) {
            if (inst.op != IrOp::Phi) continue;
            const uint32_t res_type = spv_type_for_kind(CanonicalSpvKind(inst.type));
            std::vector<uint32_t> ops = {res_type, val_spv_id[inst.id]};
            for (const auto& inc : inst.phi_incoming) {
                const uint64_t key = (static_cast<uint64_t>(inst.id) << 32) | inc.first;
                auto ov = phi_edge_override.find(key);
                const uint32_t v_id = (ov != phi_edge_override.end()) ? ov->second : val_spv_id[inc.second];
                ops.push_back(v_id);
                ops.push_back(bb_labels[inc.first]);
            }
            EmitSpvInst(sec_fn, SpvOpPhi, ops);
        }

        // Emit non-Phi, non-terminator instructions.
        for (const auto& inst : bb.instructions) {
            if (inst.op == IrOp::Phi || inst.op == IrOp::ConstU32 || inst.op == IrOp::ConstF32 ||
                inst.op == IrOp::Branch || inst.op == IrOp::CondBranch || inst.op == IrOp::Return) {
                continue;
            }
            const uint32_t res = (inst.id != kInvalidIrValue) ? val_spv_id[inst.id] : 0u;

            switch (inst.op) {
                case IrOp::ThreadIdX:
                    if (ir.stage == ShaderStage::Compute) {
                        uint32_t ptr_x = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_in_u32, ptr_x, var_gid, c_u32_0});
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr_x});
                    } else if (ir.stage == ShaderStage::Vertex) {
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, var_vid});
                    }
                    break;
                case IrOp::StageInput: {
                    uint32_t v4 = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_v4f32, v4, var_param0_in});
                    EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_f32, res, v4, inst.imm_u32 & 3u});
                    break;
                }
                case IrOp::LoadRootConst: {
                    uint32_t idx = get_const_u32(inst.imm_u32);
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_unif_u32, ptr, var_rc, c_u32_0, idx});
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr});
                    break;
                }
                case IrOp::LoadUav: {
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t dword_idx = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_unif_u32, ptr, var_uav0, c_u32_0, dword_idx});
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr});
                    break;
                }
                case IrOp::StoreUav: {
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t data_u32 = get_as_u32(inst.operands[1]);
                    uint32_t dword_idx = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_unif_u32, ptr, var_uav0, c_u32_0, dword_idx});
                    EmitSpvInst(sec_fn, SpvOpStore, {ptr, data_u32});
                    break;
                }
                case IrOp::FAdd:
                    EmitSpvInst(sec_fn, SpvOpFAdd, {t_f32, res, get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    break;
                case IrOp::FSub:
                    EmitSpvInst(sec_fn, SpvOpFSub, {t_f32, res, get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    break;
                case IrOp::FMul:
                    EmitSpvInst(sec_fn, SpvOpFMul, {t_f32, res, get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    break;
                case IrOp::FFma:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslFma,
                                                       get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1]),
                                                       get_as_f32(inst.operands[2])});
                    break;
                case IrOp::FMin:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslFMin,
                                                       get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    break;
                case IrOp::FMax:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslFMax,
                                                       get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    break;
                case IrOp::FRcp:
                    EmitSpvInst(sec_fn, SpvOpFDiv, {t_f32, res, c_f32_1, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::FRsq:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslInverseSqrt, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::FSqrt:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslSqrt, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::FFloor:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslFloor, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::FFract:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslFract, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::FDot2: {
                    uint32_t va = alloc_id();
                    uint32_t vb = alloc_id();
                    uint32_t dp = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_v2f32, va, glsl_ext_id, GlslUnpackHalf2x16, get_as_u32(inst.operands[0])});
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_v2f32, vb, glsl_ext_id, GlslUnpackHalf2x16, get_as_u32(inst.operands[1])});
                    EmitSpvInst(sec_fn, SpvOpDot, {t_f32, dp, va, vb});
                    EmitSpvInst(sec_fn, SpvOpFAdd, {t_f32, res, dp, get_as_f32(inst.operands[2])});
                    break;
                }
                case IrOp::IAdd:
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::ISub:
                    EmitSpvInst(sec_fn, SpvOpISub, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::IMul:
                    EmitSpvInst(sec_fn, SpvOpIMul, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::IMin:
                    if (inst.type == IrType::I32) {
                        uint32_t tmp = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_i32, tmp, glsl_ext_id, GlslSMin,
                                                           get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, tmp});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_u32, res, glsl_ext_id, GlslUMin,
                                                           get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    }
                    break;
                case IrOp::IMax:
                    if (inst.type == IrType::I32) {
                        uint32_t tmp = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_i32, tmp, glsl_ext_id, GlslSMax,
                                                           get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, tmp});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_u32, res, glsl_ext_id, GlslUMax,
                                                           get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    }
                    break;
                case IrOp::BitAnd:
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::BitOr:
                    EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::BitXor:
                    EmitSpvInst(sec_fn, SpvOpBitwiseXor, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::BitNot:
                    EmitSpvInst(sec_fn, SpvOpNot, {t_u32, res, get_as_u32(inst.operands[0])});
                    break;
                case IrOp::Shl: {
                    uint32_t masked = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked, get_as_u32(inst.operands[1]), c_u32_31});
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, res, get_as_u32(inst.operands[0]), masked});
                    break;
                }
                case IrOp::Shr: {
                    uint32_t masked = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked, get_as_u32(inst.operands[1]), c_u32_31});
                    if (inst.type == IrType::I32) {
                        uint32_t tmp = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpShiftRightArithmetic, {t_i32, tmp, get_as_i32(inst.operands[0]), masked});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, tmp});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, res, get_as_u32(inst.operands[0]), masked});
                    }
                    break;
                }
                case IrOp::I2F:
                    EmitSpvInst(sec_fn, SpvOpConvertSToF, {t_f32, res, get_as_i32(inst.operands[0])});
                    break;
                case IrOp::U2F:
                    EmitSpvInst(sec_fn, SpvOpConvertUToF, {t_f32, res, get_as_u32(inst.operands[0])});
                    break;
                case IrOp::F2I: {
                    uint32_t tmp = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpConvertFToS, {t_i32, tmp, get_as_f32(inst.operands[0])});
                    EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, tmp});
                    break;
                }
                case IrOp::F2U:
                    EmitSpvInst(sec_fn, SpvOpConvertFToU, {t_u32, res, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::CmpEq:
                case IrOp::CmpNe:
                case IrOp::CmpLt:
                case IrOp::CmpGt:
                case IrOp::CmpGe: {
                    const bool is_flt = (val_kind[inst.operands[0]] == SpvValKind::F32 ||
                                         val_kind[inst.operands[1]] == SpvValKind::F32);
                    if (is_flt) {
                        uint16_t spv_op = SpvOpFOrdEqual;
                        if (inst.op == IrOp::CmpNe) spv_op = SpvOpFOrdNotEqual;
                        else if (inst.op == IrOp::CmpLt) spv_op = SpvOpFOrdLessThan;
                        else if (inst.op == IrOp::CmpGt) spv_op = SpvOpFOrdGreaterThan;
                        else if (inst.op == IrOp::CmpGe) spv_op = SpvOpFOrdGreaterThanEqual;
                        EmitSpvInst(sec_fn, spv_op, {t_bool, res, get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    } else {
                        uint16_t spv_op = SpvOpIEqual;
                        if (inst.op == IrOp::CmpNe) spv_op = SpvOpINotEqual;
                        else if (inst.op == IrOp::CmpLt) spv_op = SpvOpSLessThan;
                        else if (inst.op == IrOp::CmpGt) spv_op = SpvOpSGreaterThan;
                        else if (inst.op == IrOp::CmpGe) spv_op = SpvOpSGreaterThanEqual;
                        EmitSpvInst(sec_fn, spv_op, {t_bool, res, get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                    }
                    break;
                }
                case IrOp::CmpLtU:
                    EmitSpvInst(sec_fn, SpvOpULessThan, {t_bool, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::CmpEqU:
                    EmitSpvInst(sec_fn, SpvOpIEqual, {t_bool, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::CmpGtU:
                    EmitSpvInst(sec_fn, SpvOpUGreaterThan, {t_bool, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::CmpGeU:
                    EmitSpvInst(sec_fn, SpvOpUGreaterThanEqual, {t_bool, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::Select: {
                    const SpvValKind k = CanonicalSpvKind(inst.type);
                    const uint32_t c = get_as_bool(inst.operands[0]);
                    if (k == SpvValKind::F32) {
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, res, c, get_as_f32(inst.operands[1]), get_as_f32(inst.operands[2])});
                    } else if (k == SpvValKind::Bool) {
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_bool, res, c, get_as_bool(inst.operands[1]), get_as_bool(inst.operands[2])});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, res, c, get_as_u32(inst.operands[1]), get_as_u32(inst.operands[2])});
                    }
                    break;
                }
                case IrOp::Barrier: {
                    uint32_t c_sem = get_const_u32(0x108u);
                    EmitSpvInst(sec_fn, SpvOpControlBarrier, {c_u32_2, c_u32_2, c_sem});
                    break;
                }
                case IrOp::Export: {
                    uint32_t vec_id = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpCompositeConstruct, {
                        t_v4f32, vec_id,
                        get_as_f32(inst.operands[0]),
                        get_as_f32(inst.operands[1]),
                        get_as_f32(inst.operands[2]),
                        get_as_f32(inst.operands[3]),
                    });
                    if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0) && var_pos0 != 0u) {
                        EmitSpvInst(sec_fn, SpvOpStore, {var_pos0, vec_id});
                    } else if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Param0) && var_param0_out != 0u) {
                        EmitSpvInst(sec_fn, SpvOpStore, {var_param0_out, vec_id});
                    } else if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Mrt0) && var_mrt0 != 0u) {
                        EmitSpvInst(sec_fn, SpvOpStore, {var_mrt0, vec_id});
                    }
                    break;
                }
                default:
                    break;
            }
        }

        // Emit any Phi edge coercions right before the block terminator.
        for (const auto& fx : block_exit_fixups[bi]) {
            if (fx.expected_kind == SpvValKind::F32) {
                uint32_t u = get_as_u32(fx.incoming_val);
                EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, fx.coerced_spv_id, u});
            } else if (fx.expected_kind == SpvValKind::Bool) {
                uint32_t u = get_as_u32(fx.incoming_val);
                EmitSpvInst(sec_fn, SpvOpINotEqual, {t_bool, fx.coerced_spv_id, u, c_u32_0});
            } else {
                uint32_t u = get_as_u32(fx.incoming_val);
                EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, fx.coerced_spv_id, u});
            }
        }

        // Emit block terminator.
        if (!bb.instructions.empty()) {
            const auto& term = bb.instructions.back();
            if (term.op == IrOp::Branch) {
                EmitSpvInst(sec_fn, SpvOpBranch, {bb_labels[term.true_block]});
            } else if (term.op == IrOp::CondBranch) {
                const uint32_t cond = get_as_bool(term.operands[0]);
                EmitSpvInst(sec_fn, SpvOpBranchConditional, {cond, bb_labels[term.true_block], bb_labels[term.false_block]});
            } else {
                EmitSpvInst(sec_fn, SpvOpReturn, {});
            }
        } else {
            EmitSpvInst(sec_fn, SpvOpReturn, {});
        }
    }

    EmitSpvInst(sec_fn, SpvOpFunctionEnd, {});

    // 7. Assemble final SPIR-V 1.3 binary stream
    const size_t total_words = 5u + sec_caps.size() + sec_ext.size() + sec_mem.size() +
                               sec_entry.size() + sec_exec.size() + sec_decor.size() +
                               sec_types.size() + sec_globals.size() + sec_fn.size();
    out_spirv.reserve(total_words);
    out_spirv.push_back(kSpirvMagic);
    out_spirv.push_back(kSpirvVersion13);
    out_spirv.push_back(kSpirvGeneratorScarlettX);
    out_spirv.push_back(next_id);  // Bound
    out_spirv.push_back(0u);       // Schema

    auto append_sec = [&](const std::vector<uint32_t>& s) {
        out_spirv.insert(out_spirv.end(), s.begin(), s.end());
    };
    append_sec(sec_caps);
    append_sec(sec_ext);
    append_sec(sec_mem);
    append_sec(sec_entry);
    append_sec(sec_exec);
    append_sec(sec_decor);
    append_sec(sec_types);
    append_sec(sec_globals);
    append_sec(sec_fn);

    return Status::Ok();
}

Status ValidateSpirvBinary(const std::vector<uint32_t>& spirv) {
    if (spirv.size() < 5u) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V binary shorter than 5-word header");
    }
    if (spirv[0] != kSpirvMagic) {
        return Status::Error(StatusCode::ShaderError, "Invalid SPIR-V magic number");
    }
    if (spirv[1] != kSpirvVersion13) {
        return Status::Error(StatusCode::ShaderError, "Unexpected SPIR-V version (expected 1.3)");
    }
    const uint32_t bound = spirv[3];
    if (bound < 2u) {
        return Status::Error(StatusCode::ShaderError, "Invalid SPIR-V ID bound");
    }
    if (spirv[4] != 0u) {
        return Status::Error(StatusCode::ShaderError, "Non-zero SPIR-V reserved schema word");
    }

    bool saw_capability = false;
    bool saw_memory_model = false;
    bool saw_entry_point = false;
    bool saw_function = false;
    bool saw_return = false;
    bool saw_function_end = false;
    int fn_depth = 0;
    bool allow_phi = false;

    size_t pos = 5u;
    while (pos < spirv.size()) {
        const uint32_t header = spirv[pos];
        const uint16_t word_count = static_cast<uint16_t>((header >> 16) & 0xFFFFu);
        const uint16_t opcode = static_cast<uint16_t>(header & 0xFFFFu);
        if (word_count == 0u || pos + word_count > spirv.size()) {
            return Status::Error(StatusCode::ShaderError, "Truncated or zero-length SPIR-V instruction");
        }

        if (opcode != SpvOpPhi && opcode != SpvOpLabel) {
            allow_phi = false;
        }

        switch (opcode) {
            case SpvOpCapability:
                if (word_count != 2u) return Status::Error(StatusCode::ShaderError, "Malformed OpCapability");
                saw_capability = true;
                break;
            case SpvOpMemoryModel:
                if (word_count != 3u) return Status::Error(StatusCode::ShaderError, "Malformed OpMemoryModel");
                saw_memory_model = true;
                break;
            case SpvOpEntryPoint:
                if (word_count < 4u || spirv[pos + 2] == 0u || spirv[pos + 2] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpEntryPoint");
                }
                saw_entry_point = true;
                break;
            case SpvOpDecorate:
                if (word_count < 3u || spirv[pos + 1] == 0u || spirv[pos + 1] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpDecorate");
                }
                break;
            case SpvOpMemberDecorate:
                if (word_count < 4u || spirv[pos + 1] == 0u || spirv[pos + 1] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpMemberDecorate");
                }
                break;
            case SpvOpFunction:
                if (word_count != 5u || spirv[pos + 1] >= bound || spirv[pos + 2] >= bound || spirv[pos + 4] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpFunction");
                }
                saw_function = true;
                ++fn_depth;
                break;
            case SpvOpFunctionEnd:
                if (word_count != 1u || fn_depth <= 0) {
                    return Status::Error(StatusCode::ShaderError, "Unbalanced OpFunctionEnd");
                }
                --fn_depth;
                saw_function_end = true;
                break;
            case SpvOpLabel:
                if (word_count != 2u || spirv[pos + 1] == 0u || spirv[pos + 1] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpLabel");
                }
                allow_phi = true;
                break;
            case SpvOpPhi:
                if (!allow_phi || word_count < 5u || ((word_count - 3u) & 1u) != 0u ||
                    spirv[pos + 1] >= bound || spirv[pos + 2] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed or misplaced OpPhi");
                }
                for (uint16_t k = 3u; k < word_count; ++k) {
                    if (spirv[pos + k] == 0u || spirv[pos + k] >= bound) {
                        return Status::Error(StatusCode::ShaderError, "OpPhi operand ID out of bound");
                    }
                }
                break;
            case SpvOpBranch:
                if (word_count != 2u || spirv[pos + 1] == 0u || spirv[pos + 1] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpBranch");
                }
                break;
            case SpvOpBranchConditional:
                if (word_count < 4u || spirv[pos + 1] >= bound || spirv[pos + 2] >= bound || spirv[pos + 3] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpBranchConditional");
                }
                break;
            case SpvOpReturn:
                if (word_count != 1u) return Status::Error(StatusCode::ShaderError, "Malformed OpReturn");
                saw_return = true;
                break;
            default:
                break;
        }
        pos += word_count;
    }

    if (!saw_capability || !saw_memory_model || !saw_entry_point ||
        !saw_function || !saw_return || !saw_function_end || fn_depth != 0) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V module missing required sections or unbalanced function");
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// HLSL Emission & Full XS2 -> PC Lifting Entry Points
// ---------------------------------------------------------------------------
Status EmitHlsl(const Rdna2IrModule& ir, std::string& out_hlsl, std::string& out_target) {
    HlslEmitter emitter(ir);
    return emitter.Emit(out_hlsl, out_target);
}

Status LiftRdna2Xs2ToPcDxbc(
    const std::vector<uint8_t>& xs2_bytes,
    Rdna2LiftResult& out_result) {
    out_result = Rdna2LiftResult{};

    if (auto st = ParseXs2Container(xs2_bytes, out_result.container); !st.ok()) {
        return st;
    }
    if (auto st = DisassembleRdna2(out_result.container.code_words, out_result.instructions); !st.ok()) {
        return st;
    }
    if (auto st = BuildRdna2Ir(out_result.container, out_result.instructions, out_result.ir); !st.ok()) {
        return st;
    }
    if (auto st = OptimizeRdna2Ir(out_result.ir); !st.ok()) {
        return st;
    }
    if (auto st = EmitHlsl(out_result.ir, out_result.hlsl_source, out_result.hlsl_target); !st.ok()) {
        return st;
    }
    if (auto st = CompileHlslToDxbc(out_result.hlsl_source, "main", out_result.hlsl_target, out_result.pc_dxbc);
        !st.ok()) {
        return st;
    }
    out_result.pc_dxbc_hash = HashShaderBytes(out_result.pc_dxbc.data(), out_result.pc_dxbc.size());
    return Status::Ok();
}

Status LiftRdna2Xs2Shader(
    const std::vector<uint8_t>& xs2_bytes,
    Rdna2LiftResult& out_result) {
    if (auto st = LiftRdna2Xs2ToPcDxbc(xs2_bytes, out_result); !st.ok()) {
        return st;
    }
    if (auto st = EmitSpirv(out_result.ir, out_result.spirv_words); !st.ok()) {
        return st;
    }
    if (auto st = ValidateSpirvBinary(out_result.spirv_words); !st.ok()) {
        return st;
    }
    return Status::Ok();
}

}  // namespace scarlettx
