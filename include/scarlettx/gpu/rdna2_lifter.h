#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"
#include "scarlettx/gpu/rdna2_disasm.h"
#include "scarlettx/gpu/rdna2_ir.h"

namespace scarlettx {

struct Rdna2LiftResult {
    Xs2ShaderContainer container;
    std::vector<Rdna2Instruction> instructions;
    Rdna2IrModule ir;
    std::vector<uint32_t> spirv_words;
    std::string hlsl_source;
    std::string hlsl_target;
    std::vector<uint8_t> pc_dxbc;
    uint64_t pc_dxbc_hash = 0;
};

// Emits a valid SPIR-V 1.3 binary module from an RDNA2 SSA IR module.
Status EmitSpirv(const Rdna2IrModule& ir, std::vector<uint32_t>& out_spirv);

// Structurally validates a SPIR-V 1.3 binary word stream (header, word counts, ID bounds,
// required sections, entry point, function balance).
Status ValidateSpirvBinary(const std::vector<uint32_t>& spirv);

// Emits clean, compilable HLSL (cs_5_0 / vs_5_0 / ps_5_0) from an RDNA2 SSA IR module.
Status EmitHlsl(const Rdna2IrModule& ir, std::string& out_hlsl, std::string& out_target);

// End-to-end RDNA2 XS2\0 -> Disasm -> SSA IR -> Optimize -> HLSL -> live PC DXBC compilation.
Status LiftRdna2Xs2ToPcDxbc(
    const std::vector<uint8_t>& xs2_bytes,
    Rdna2LiftResult& out_result);

// Full RDNA2 XS2\0 lifting pipeline producing BOTH SPIR-V 1.3 and compiled PC DXBC.
Status LiftRdna2Xs2Shader(
    const std::vector<uint8_t>& xs2_bytes,
    Rdna2LiftResult& out_result);

}  // namespace scarlettx
