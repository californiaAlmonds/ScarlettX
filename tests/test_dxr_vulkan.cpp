#include "sx_check.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/dxr_hardware.h"
#include "scarlettx/gpu/rt_bvh.h"
#include "scarlettx/gpu/vulkan_backend.h"

namespace {

constexpr uint32_t SpvWord(uint16_t word_count, uint16_t opcode) {
    return (static_cast<uint32_t>(word_count) << 16u) | static_cast<uint32_t>(opcode);
}

// Hand-crafted clean-room valid SPIR-V 1.3 GLCompute shader:
//   layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
//   layout(set = 0, binding = 0) buffer InBuf  { uint data[]; } inBuf;
//   layout(set = 0, binding = 1) buffer OutBuf { uint data[]; } outBuf;
//   void main() {
//       uint i = gl_GlobalInvocationID.x;
//       outBuf.data[i] = <op1>(inBuf.data[i], imm1) + imm2;
//   }
// op1_opcode: 132 = OpIMul, 198 = OpBitwiseXor.
std::vector<uint32_t> BuildSpirvComputeKernel(uint16_t op1_opcode, uint32_t imm1, uint32_t imm2) {
    // ID table (Bound = 26):
    // 1: void, 2: fn_void, 3: u32, 4: vec3<u32>, 5: ptr<Input, vec3<u32>>
    // 6: gl_GlobalInvocationID, 7: ptr<Input, u32>, 8: runtime_array<u32>
    // 9: struct{runtime_array<u32>}, 10: ptr<StorageBuffer, struct>
    // 11: inBuf (binding 0), 12: outBuf (binding 1), 13: ptr<StorageBuffer, u32>
    // 14: const 0u, 15: const imm1, 16: const imm2
    // 17: main, 18: entry label, 19: ptr_gid_x, 20: idx, 21: ptr_in_elem
    // 22: val, 23: tmp1, 24: res, 25: ptr_out_elem
    std::vector<uint32_t> w;
    // Header (SPIR-V 1.3)
    w.push_back(0x07230203u);  // Magic
    w.push_back(0x00010300u);  // Version 1.3
    w.push_back(0x00000000u);  // Generator
    w.push_back(26u);          // Bound
    w.push_back(0u);           // Schema

    // OpCapability Shader (1)
    w.push_back(SpvWord(2, 17)); w.push_back(1);
    // OpMemoryModel Logical(0) GLSL450(1)
    w.push_back(SpvWord(3, 14)); w.push_back(0); w.push_back(1);
    // OpEntryPoint GLCompute(5) %17 "main" %6
    // "main\0" = 5 bytes -> 2 words: 0x6E69616D, 0x00000000
    w.push_back(SpvWord(6, 15)); w.push_back(5); w.push_back(17);
    w.push_back(0x6E69616Du); w.push_back(0x00000000u); w.push_back(6);
    // OpExecutionMode %17 LocalSize(17) 1 1 1
    w.push_back(SpvWord(6, 16)); w.push_back(17); w.push_back(17);
    w.push_back(1); w.push_back(1); w.push_back(1);

    // Decorations
    // OpDecorate %8 ArrayStride(6) 4
    w.push_back(SpvWord(4, 71)); w.push_back(8); w.push_back(6); w.push_back(4);
    // OpMemberDecorate %9 0 Offset(35) 0
    w.push_back(SpvWord(5, 72)); w.push_back(9); w.push_back(0); w.push_back(35); w.push_back(0);
    // OpDecorate %9 Block(2)
    w.push_back(SpvWord(3, 71)); w.push_back(9); w.push_back(2);
    // OpDecorate %11 DescriptorSet(34) 0 ; Binding(33) 0
    w.push_back(SpvWord(4, 71)); w.push_back(11); w.push_back(34); w.push_back(0);
    w.push_back(SpvWord(4, 71)); w.push_back(11); w.push_back(33); w.push_back(0);
    // OpDecorate %12 DescriptorSet(34) 0 ; Binding(33) 1
    w.push_back(SpvWord(4, 71)); w.push_back(12); w.push_back(34); w.push_back(0);
    w.push_back(SpvWord(4, 71)); w.push_back(12); w.push_back(33); w.push_back(1);
    // OpDecorate %6 BuiltIn(11) GlobalInvocationId(28)
    w.push_back(SpvWord(4, 71)); w.push_back(6); w.push_back(11); w.push_back(28);

    // Types, constants, global variables
    // %1 = OpTypeVoid
    w.push_back(SpvWord(2, 19)); w.push_back(1);
    // %2 = OpTypeFunction %1
    w.push_back(SpvWord(3, 33)); w.push_back(2); w.push_back(1);
    // %3 = OpTypeInt 32 0 (uint)
    w.push_back(SpvWord(4, 21)); w.push_back(3); w.push_back(32); w.push_back(0);
    // %4 = OpTypeVector %3 3
    w.push_back(SpvWord(4, 23)); w.push_back(4); w.push_back(3); w.push_back(3);
    // %5 = OpTypePointer Input(1) %4
    w.push_back(SpvWord(4, 32)); w.push_back(5); w.push_back(1); w.push_back(4);
    // %6 = OpVariable %5 Input(1)
    w.push_back(SpvWord(4, 59)); w.push_back(5); w.push_back(6); w.push_back(1);
    // %7 = OpTypePointer Input(1) %3
    w.push_back(SpvWord(4, 32)); w.push_back(7); w.push_back(1); w.push_back(3);
    // %8 = OpTypeRuntimeArray %3
    w.push_back(SpvWord(3, 29)); w.push_back(8); w.push_back(3);
    // %9 = OpTypeStruct %8
    w.push_back(SpvWord(3, 30)); w.push_back(9); w.push_back(8);
    // %10 = OpTypePointer StorageBuffer(12) %9
    w.push_back(SpvWord(4, 32)); w.push_back(10); w.push_back(12); w.push_back(9);
    // %11 = OpVariable %10 StorageBuffer(12)
    w.push_back(SpvWord(4, 59)); w.push_back(10); w.push_back(11); w.push_back(12);
    // %12 = OpVariable %10 StorageBuffer(12)
    w.push_back(SpvWord(4, 59)); w.push_back(10); w.push_back(12); w.push_back(12);
    // %13 = OpTypePointer StorageBuffer(12) %3
    w.push_back(SpvWord(4, 32)); w.push_back(13); w.push_back(12); w.push_back(3);
    // %14 = OpConstant %3 0
    w.push_back(SpvWord(4, 43)); w.push_back(3); w.push_back(14); w.push_back(0);
    // %15 = OpConstant %3 imm1
    w.push_back(SpvWord(4, 43)); w.push_back(3); w.push_back(15); w.push_back(imm1);
    // %16 = OpConstant %3 imm2
    w.push_back(SpvWord(4, 43)); w.push_back(3); w.push_back(16); w.push_back(imm2);

    // Function %17 (main)
    // OpFunction %1 %17 None(0) %2
    w.push_back(SpvWord(5, 54)); w.push_back(1); w.push_back(17); w.push_back(0); w.push_back(2);
    // OpLabel %18
    w.push_back(SpvWord(2, 248)); w.push_back(18);
    // %19 = OpAccessChain %7 %6 %14 (gl_GlobalInvocationID.x)
    w.push_back(SpvWord(5, 65)); w.push_back(7); w.push_back(19); w.push_back(6); w.push_back(14);
    // %20 = OpLoad %3 %19
    w.push_back(SpvWord(4, 61)); w.push_back(3); w.push_back(20); w.push_back(19);
    // %21 = OpAccessChain %13 %11 %14 %20 (&inBuf.data[idx])
    w.push_back(SpvWord(6, 65)); w.push_back(13); w.push_back(21); w.push_back(11); w.push_back(14); w.push_back(20);
    // %22 = OpLoad %3 %21
    w.push_back(SpvWord(4, 61)); w.push_back(3); w.push_back(22); w.push_back(21);
    // %23 = <op1_opcode> %3 %22 %15
    w.push_back(SpvWord(5, op1_opcode)); w.push_back(3); w.push_back(23); w.push_back(22); w.push_back(15);
    // %24 = OpIAdd(128) %3 %23 %16
    w.push_back(SpvWord(5, 128)); w.push_back(3); w.push_back(24); w.push_back(23); w.push_back(16);
    // %25 = OpAccessChain %13 %12 %14 %20 (&outBuf.data[idx])
    w.push_back(SpvWord(6, 65)); w.push_back(13); w.push_back(25); w.push_back(12); w.push_back(14); w.push_back(20);
    // OpStore %25 %24
    w.push_back(SpvWord(3, 62)); w.push_back(25); w.push_back(24);
    // OpReturn
    w.push_back(SpvWord(1, 253));
    // OpFunctionEnd
    w.push_back(SpvWord(1, 56));
    return w;
}

uint64_t RunDxrPipelineTest(bool prefer_warp) {
    scarlettx::D3D12Backend backend;
    auto st = backend.Initialize(prefer_warp, false);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(backend.IsInitialized());

    scarlettx::DxrHardwareEngine dxr;
    SX_REQUIRE(!dxr.IsInitialized());

    // Uninitialized calls must fail cleanly.
    scarlettx::DxrPrebuildInfo dummy_pre{};
    scarlettx::DxrAsInputs dummy_in{};
    dummy_in.primitive_count = 12;
    SX_REQUIRE(!dxr.QueryPrebuildInfo(dummy_in, dummy_pre).ok());

    st = dxr.Initialize(backend);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(dxr.IsInitialized());

    const auto& caps = dxr.Capabilities();
    std::cout << "[test_dxr_vulkan][dxr] prefer_warp=" << (prefer_warp ? "true" : "false")
              << " adapter=\"" << caps.adapter_description << "\""
              << " is_warp=" << (caps.adapter_is_warp ? "yes" : "no")
              << " tier=" << dxr.hardware_tier()
              << " state_obj=" << (dxr.supports_state_objects() ? "yes" : "no")
              << " inline_rt=" << (dxr.supports_inline_rt() ? "yes" : "no")
              << " hw_path=" << (dxr.using_hardware_path() ? "yes" : "no") << "\n";

    if (prefer_warp) {
        SX_REQUIRE(!dxr.using_hardware_path());
    }

    // 1. Prebuild info checks (256-byte alignment, BLAS & TLAS, AllowUpdate flag)
    scarlettx::DxrAsInputs blas_in{};
    blas_in.type = scarlettx::DxrAsType::BottomLevel;
    blas_in.build_flags = scarlettx::kDxrBuildPreferFastTrace | scarlettx::kDxrBuildAllowUpdate;
    blas_in.primitive_count = 12;
    blas_in.geometry_count = 1;
    scarlettx::DxrPrebuildInfo blas_pre{};
    SX_REQUIRE(dxr.QueryPrebuildInfo(blas_in, blas_pre).ok());
    SX_REQUIRE(blas_pre.result_data_max_size_bytes > 0);
    SX_REQUIRE(blas_pre.scratch_data_size_bytes > 0);
    SX_REQUIRE(blas_pre.update_scratch_data_size_bytes > 0);
    SX_REQUIRE((blas_pre.result_data_max_size_bytes % scarlettx::kDxrAsByteAlignment) == 0);
    SX_REQUIRE((blas_pre.scratch_data_size_bytes % scarlettx::kDxrAsByteAlignment) == 0);
    SX_REQUIRE((blas_pre.update_scratch_data_size_bytes % scarlettx::kDxrAsByteAlignment) == 0);
    SX_REQUIRE(blas_pre.from_hardware_driver == dxr.using_hardware_path());

    scarlettx::DxrAsInputs tlas_in{};
    tlas_in.type = scarlettx::DxrAsType::TopLevel;
    tlas_in.build_flags = scarlettx::kDxrBuildPreferFastTrace;
    tlas_in.primitive_count = 2;
    scarlettx::DxrPrebuildInfo tlas_pre{};
    SX_REQUIRE(dxr.QueryPrebuildInfo(tlas_in, tlas_pre).ok());
    SX_REQUIRE(tlas_pre.result_data_max_size_bytes > 0);
    SX_REQUIRE(tlas_pre.scratch_data_size_bytes > 0);
    SX_REQUIRE((tlas_pre.result_data_max_size_bytes % scarlettx::kDxrAsByteAlignment) == 0);
    SX_REQUIRE((tlas_pre.scratch_data_size_bytes % scarlettx::kDxrAsByteAlignment) == 0);
    SX_REQUIRE(tlas_pre.from_hardware_driver == dxr.using_hardware_path());

    // Invalid prebuild input rejected.
    scarlettx::DxrAsInputs bad_in{};
    bad_in.primitive_count = 0;
    SX_REQUIRE(!dxr.QueryPrebuildInfo(bad_in, blas_pre).ok());

    // 2. Build BLAS (default 12-triangle Cornell scene + secondary updatable triangle BLAS)
    auto scene_tris = scarlettx::BuildDefaultRtTestScene();
    SX_REQUIRE(scene_tris.size() == 12);

    uint64_t blas1 = 0;
    SX_REQUIRE(dxr.BuildBlas(scene_tris, scarlettx::kDxrBuildPreferFastTrace, blas1).ok());
    SX_REQUIRE(blas1 != 0);

    scarlettx::DxrAsRecordInfo blas1_info{};
    SX_REQUIRE(dxr.GetAsInfo(blas1, blas1_info).ok());
    SX_REQUIRE(blas1_info.type == scarlettx::DxrAsType::BottomLevel);
    SX_REQUIRE(blas1_info.primitive_count == 12);
    SX_REQUIRE(blas1_info.bvh_node_count > 1);
    SX_REQUIRE(blas1_info.gpu_virtual_address != 0);
    SX_REQUIRE((blas1_info.gpu_virtual_address % scarlettx::kDxrAsByteAlignment) == 0);
    SX_REQUIRE(blas1_info.hardware_built == dxr.using_hardware_path());

    // Updatable BLAS + refit test
    std::vector<scarlettx::RtTriangle> extra_tris = {
        {{-0.25f, 1.75f, 0.25f}, {0.25f, 1.75f, 0.25f}, {0.0f, 2.20f, 0.25f}, {0.20f, 0.90f, 0.95f}, 0.25f},
    };
    uint64_t blas2 = 0;
    SX_REQUIRE(dxr.BuildBlas(extra_tris,
                             scarlettx::kDxrBuildAllowUpdate | scarlettx::kDxrBuildPreferFastBuild,
                             blas2).ok());
    extra_tris[0].v2.y = 2.35f;
    SX_REQUIRE(dxr.UpdateBlas(blas2, extra_tris).ok());
    // Updating non-updatable BLAS must fail cleanly.
    SX_REQUIRE(!dxr.UpdateBlas(blas1, scene_tris).ok());

    // 3. Build TLAS with 3x4 affine transforms
    scarlettx::DxrInstanceDesc inst0{};
    inst0.blas_id = blas1;
    inst0.instance_id = 0x10;
    inst0.instance_mask = 0xFF;
    inst0.instance_contribution_to_hit_group_index = 0;

    scarlettx::DxrInstanceDesc inst1{};
    inst1.blas_id = blas2;
    inst1.instance_id = 0x20;
    inst1.instance_mask = 0xFF;
    inst1.instance_contribution_to_hit_group_index = 1;
    // Scale 0.9x + translate (+0.35, +0.10, -0.15)
    inst1.transform[0][0] = 0.9f; inst1.transform[0][3] = 0.35f;
    inst1.transform[1][1] = 0.9f; inst1.transform[1][3] = 0.10f;
    inst1.transform[2][2] = 0.9f; inst1.transform[2][3] = -0.15f;

    uint64_t tlas_id = 0;
    SX_REQUIRE(dxr.BuildTlas({inst0, inst1}, scarlettx::kDxrBuildPreferFastTrace, tlas_id).ok());
    SX_REQUIRE(tlas_id != 0);

    scarlettx::DxrAsRecordInfo tlas_info{};
    SX_REQUIRE(dxr.GetAsInfo(tlas_id, tlas_info).ok());
    SX_REQUIRE(tlas_info.type == scarlettx::DxrAsType::TopLevel);
    SX_REQUIRE(tlas_info.primitive_count == 2);
    SX_REQUIRE(tlas_info.gpu_virtual_address != 0);
    SX_REQUIRE((tlas_info.gpu_virtual_address % scarlettx::kDxrAsByteAlignment) == 0);
    SX_REQUIRE(tlas_info.hardware_built == dxr.using_hardware_path());

    // 4. Shader Binding Table alignment & D3D12_DISPATCH_RAYS_DESC verification
    scarlettx::DxrShaderTableDesc sbt_desc{};
    sbt_desc.raygen.export_name = "ScarlettRayGen";
    sbt_desc.raygen.local_root_arguments = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};  // 32+8 -> 64B

    scarlettx::DxrShaderRecordDesc miss0{};
    miss0.export_name = "SkyMiss";
    scarlettx::DxrShaderRecordDesc miss1{};
    miss1.export_name = "ShadowMiss";
    miss1.local_root_arguments.assign(16, 0xAB);  // 32+16 -> 64B stride
    sbt_desc.miss_records = {miss0, miss1};

    scarlettx::DxrShaderRecordDesc hit0{};
    hit0.export_name = "PrimaryHitGroup";
    hit0.local_root_arguments.assign(40, 0xCD);  // 32+40 = 72 -> 96B stride
    scarlettx::DxrShaderRecordDesc hit1{};
    hit1.export_name = "ReflectiveHitGroup";
    sbt_desc.hit_group_records = {hit0, hit1};

    scarlettx::DxrShaderRecordDesc call0{};
    call0.export_name = "ProceduralMaterialCallable";  // 32B stride
    sbt_desc.callable_records = {call0};

    sbt_desc.dispatch_width = 96;
    sbt_desc.dispatch_height = 72;
    sbt_desc.dispatch_depth = 1;

    scarlettx::DxrShaderBindingTable sbt{};
    SX_REQUIRE(dxr.BuildShaderBindingTable(sbt_desc, sbt).ok());
    SX_REQUIRE(sbt.uploaded_to_gpu);
    SX_REQUIRE(sbt.base_gpu_va != 0);
    SX_REQUIRE((sbt.base_gpu_va % scarlettx::kDxrShaderTableAlignment) == 0);
    SX_REQUIRE((sbt.raygen_offset % scarlettx::kDxrShaderTableAlignment) == 0);
    SX_REQUIRE((sbt.miss_offset % scarlettx::kDxrShaderTableAlignment) == 0);
    SX_REQUIRE((sbt.hit_group_offset % scarlettx::kDxrShaderTableAlignment) == 0);
    SX_REQUIRE((sbt.callable_offset % scarlettx::kDxrShaderTableAlignment) == 0);
    SX_REQUIRE(sbt.raygen_record_size == 64);
    SX_REQUIRE(sbt.miss_record_stride == 64);
    SX_REQUIRE(sbt.hit_group_record_stride == 96);
    SX_REQUIRE(sbt.callable_record_stride == 32);

    const auto& dd = sbt.dispatch_rays_desc;
    SX_REQUIRE(dd.RayGenerationShaderRecord.StartAddress == sbt.base_gpu_va + sbt.raygen_offset);
    SX_REQUIRE(dd.RayGenerationShaderRecord.SizeInBytes == 64);
    SX_REQUIRE((dd.MissShaderTable.StartAddress % scarlettx::kDxrShaderTableAlignment) == 0);
    SX_REQUIRE(dd.MissShaderTable.StrideInBytes == 64);
    SX_REQUIRE(dd.MissShaderTable.SizeInBytes == 128);
    SX_REQUIRE((dd.HitGroupTable.StartAddress % scarlettx::kDxrShaderTableAlignment) == 0);
    SX_REQUIRE(dd.HitGroupTable.StrideInBytes == 96);
    SX_REQUIRE(dd.HitGroupTable.SizeInBytes == 192);
    SX_REQUIRE((dd.CallableShaderTable.StartAddress % scarlettx::kDxrShaderTableAlignment) == 0);
    SX_REQUIRE(dd.CallableShaderTable.StrideInBytes == 32);
    SX_REQUIRE(dd.CallableShaderTable.SizeInBytes == 32);
    SX_REQUIRE(dd.Width == 96 && dd.Height == 72 && dd.Depth == 1);

    // Verify local root arguments are intact right after the 32-byte shader identifier.
    SX_REQUIRE(sbt.buffer[sbt.raygen_offset + 32] == 0x11);
    SX_REQUIRE(sbt.buffer[sbt.raygen_offset + 39] == 0x88);
    SX_REQUIRE(sbt.buffer[sbt.miss_offset + 64 + 32] == 0xAB);
    SX_REQUIRE(sbt.buffer[sbt.hit_group_offset + 32] == 0xCD);
    SX_REQUIRE(sbt.buffer[sbt.hit_group_offset + 71] == 0xCD);

    // Misaligned caller base_gpu_va must be rejected.
    scarlettx::DxrShaderTableDesc bad_sbt_desc = sbt_desc;
    bad_sbt_desc.base_gpu_va = 0x100020ull;  // 32B aligned, NOT 64B aligned
    scarlettx::DxrShaderBindingTable bad_sbt{};
    SX_REQUIRE(!dxr.BuildShaderBindingTable(bad_sbt_desc, bad_sbt).ok());

    // 5. DispatchRays & determinism check
    scarlettx::RtDispatchDesc disp{};
    disp.width = 96;
    disp.height = 72;
    disp.depth = 1;
    disp.max_bounces = 2;
    disp.sbt = &sbt;

    scarlettx::FrameBufferRgba8 frame1;
    scarlettx::RtStats stats1{};
    uint64_t hash1 = 0;
    SX_REQUIRE(dxr.DispatchRays(tlas_id, disp, frame1, &stats1, &hash1).ok());
    SX_REQUIRE(frame1.width == 96 && frame1.height == 72);
    SX_REQUIRE(frame1.pixels.size() == 96u * 72u * 4u);
    SX_REQUIRE(hash1 != 0);
    SX_REQUIRE(hash1 == frame1.ComputeFnv1aHash());
    SX_REQUIRE(dxr.LastFrameHash() == hash1);
    SX_REQUIRE(dxr.TotalDispatches() == 1);
    SX_REQUIRE(stats1.primary_rays == 96u * 72u);
    SX_REQUIRE(stats1.shadow_rays > 0);
    SX_REQUIRE(stats1.reflection_rays > 0);

    scarlettx::FrameBufferRgba8 frame2;
    uint64_t hash2 = 0;
    SX_REQUIRE(dxr.DispatchRays(tlas_id, disp, frame2, nullptr, &hash2).ok());
    SX_REQUIRE(hash1 == hash2);

    // 6. Transform sensitivity: shifting instance 0 by +0.6 on X must change rendered image.
    scarlettx::DxrInstanceDesc shifted = inst0;
    shifted.transform[0][3] = 0.6f;
    uint64_t tlas_shifted = 0;
    SX_REQUIRE(dxr.BuildTlas({shifted}, scarlettx::kDxrBuildPreferFastTrace, tlas_shifted).ok());
    scarlettx::FrameBufferRgba8 frame_shifted;
    uint64_t hash_shifted = 0;
    SX_REQUIRE(dxr.DispatchRays(tlas_shifted, disp, frame_shifted, nullptr, &hash_shifted).ok());
    SX_REQUIRE(hash_shifted != hash1);

    // 7. Resource teardown & cleanup
    SX_REQUIRE(dxr.DestroyAs(tlas_shifted).ok());
    SX_REQUIRE(!dxr.DestroyAs(tlas_shifted).ok());
    SX_REQUIRE(dxr.DestroyAs(tlas_id).ok());
    SX_REQUIRE(dxr.DestroyAs(blas2).ok());
    SX_REQUIRE(dxr.DestroyAs(blas1).ok());

    dxr.Shutdown();
    SX_REQUIRE(!dxr.IsInitialized());
    backend.Shutdown();
    return hash1;
}

void RunSpirvValidationTests() {
    auto valid_spv = BuildSpirvComputeKernel(132, 2u, 7u);
    scarlettx::SpirvValidationInfo info{};
    SX_REQUIRE(scarlettx::VulkanBackend::ValidateSpirvBinary(valid_spv, &info).ok());
    SX_REQUIRE(info.version_major == 1);
    SX_REQUIRE(info.version_minor == 3);
    SX_REQUIRE(info.bound == 26);
    SX_REQUIRE(info.has_compute_entry);
    SX_REQUIRE(info.entry_point_name == "main");
    SX_REQUIRE(info.local_size_x == 1 && info.local_size_y == 1 && info.local_size_z == 1);
    SX_REQUIRE(info.instruction_count > 10);

    // Empty / truncated header
    SX_REQUIRE(!scarlettx::VulkanBackend::ValidateSpirvBinary({}).ok());
    SX_REQUIRE(!scarlettx::VulkanBackend::ValidateSpirvBinary({0x07230203u, 0x00010300u, 0, 10}).ok());

    // Bad magic
    auto bad_magic = valid_spv;
    bad_magic[0] = 0xDEADBEEFu;
    SX_REQUIRE(!scarlettx::VulkanBackend::ValidateSpirvBinary(bad_magic).ok());

    // Bad version
    auto bad_ver = valid_spv;
    bad_ver[1] = 0x01020304u;
    SX_REQUIRE(!scarlettx::VulkanBackend::ValidateSpirvBinary(bad_ver).ok());

    // Zero bound
    auto bad_bound = valid_spv;
    bad_bound[3] = 0;
    SX_REQUIRE(!scarlettx::VulkanBackend::ValidateSpirvBinary(bad_bound).ok());

    // Non-zero schema
    auto bad_schema = valid_spv;
    bad_schema[4] = 1;
    SX_REQUIRE(!scarlettx::VulkanBackend::ValidateSpirvBinary(bad_schema).ok());

    // Truncated instruction
    auto truncated = valid_spv;
    truncated.pop_back();
    truncated.pop_back();  // Cuts off OpReturn/OpFunctionEnd leaving unclosed function or truncated body
    SX_REQUIRE(!scarlettx::VulkanBackend::ValidateSpirvBinary(truncated).ok());

    // Bad word count (0)
    auto zero_wc = valid_spv;
    zero_wc[5] = 0x00000011u;  // word_count = 0
    SX_REQUIRE(!scarlettx::VulkanBackend::ValidateSpirvBinary(zero_wc).ok());

    // Overrun word count
    auto overrun_wc = valid_spv;
    overrun_wc[5] = SpvWord(9999, 17);
    SX_REQUIRE(!scarlettx::VulkanBackend::ValidateSpirvBinary(overrun_wc).ok());

    // Missing OpMemoryModel
    std::vector<uint32_t> no_mem = {0x07230203u, 0x00010300u, 0, 4, 0, SpvWord(2, 17), 1};
    SX_REQUIRE(!scarlettx::VulkanBackend::ValidateSpirvBinary(no_mem).ok());

    std::cout << "[test_dxr_vulkan][spirv] All 11 SPIR-V validator tests PASSED\n";
}

void RunVulkanBackendTests() {
    auto spv_mul_add = BuildSpirvComputeKernel(132, 2u, 7u);       // out[i] = in[i] * 2 + 7
    auto spv_xor_add = BuildSpirvComputeKernel(198, 0x5A5Au, 13u); // out[i] = (in[i] ^ 0x5A5A) + 13

    std::vector<uint32_t> input(64);
    for (uint32_t i = 0; i < 64; ++i) {
        input[i] = i * 17u + 3u;
    }

    // 1. Live VulkanBackend::Initialize(allow_software_fallback = true)
    scarlettx::VulkanBackend vk;
    SX_REQUIRE(!vk.IsInitialized());
    auto st = vk.Initialize(true);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(vk.IsInitialized());

    const auto& dev = vk.GetDeviceInfo();
    SX_REQUIRE(!dev.device_name.empty());
    SX_REQUIRE(dev.supports_compute);
    SX_REQUIRE(dev.api_major >= 1);

    std::cout << "[test_dxr_vulkan][vulkan] Device=\"" << dev.device_name << "\""
              << " API=" << dev.api_major << "." << dev.api_minor << "." << dev.api_patch
              << " vendor=0x" << std::hex << dev.vendor_id
              << " device=0x" << dev.device_id << std::dec
              << " hw=" << (dev.is_hardware ? "yes" : "no")
              << " fallback=" << (vk.is_fallback_mode() ? "yes" : "no") << "\n";

    // Kernel 1: out[i] = in[i] * 2 + 7
    std::vector<uint32_t> out1;
    st = vk.ExecuteComputeShader(spv_mul_add, input, out1, static_cast<uint32_t>(input.size()));
    SX_REQUIRE(st.ok());
    SX_REQUIRE(out1.size() == input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        SX_REQUIRE(out1[i] == input[i] * 2u + 7u);
    }

    // Kernel 2: out[i] = (in[i] ^ 0x5A5A) + 13
    std::vector<uint32_t> out2;
    st = vk.ExecuteComputeShader(spv_xor_add, input, out2, static_cast<uint32_t>(input.size()));
    SX_REQUIRE(st.ok());
    SX_REQUIRE(out2.size() == input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        SX_REQUIRE(out2[i] == ((input[i] ^ 0x5A5Au) + 13u));
    }

    // Invalid SPIR-V rejected cleanly during ExecuteComputeShader
    std::vector<uint32_t> bad_out;
    SX_REQUIRE(!vk.ExecuteComputeShader({0x12345678u}, input, bad_out, 1).ok());
    SX_REQUIRE(!vk.ExecuteComputeShader(spv_mul_add, input, bad_out, 0).ok());

    // RecordCommandList test (valid + invalid dispatch dims)
    scarlettx::VulkanCommandListDesc cl_desc{};
    cl_desc.commands = {
        {scarlettx::VulkanCommandOp::BindComputePipeline, 1, 1, 1, 1},
        {scarlettx::VulkanCommandOp::BindDescriptorSets, 1, 1, 1, 2},
        {scarlettx::VulkanCommandOp::Dispatch, 8, 1, 1, 0},
        {scarlettx::VulkanCommandOp::PipelineBarrier, 0, 0, 0, 0},
    };
    scarlettx::VulkanCommandListResult cl_res{};
    SX_REQUIRE(vk.RecordCommandList(cl_desc, cl_res).ok());
    SX_REQUIRE(cl_res.recorded_commands == 4);
    SX_REQUIRE(cl_res.dispatch_count == 1);
    SX_REQUIRE(cl_res.barrier_count == 1);
    SX_REQUIRE(cl_res.executed_on_vulkan_icd == !vk.is_fallback_mode());

    scarlettx::VulkanCommandListDesc bad_cl{};
    bad_cl.commands = {{scarlettx::VulkanCommandOp::Dispatch, 0, 1, 1, 0}};
    SX_REQUIRE(!vk.RecordCommandList(bad_cl, cl_res).ok());

    // Clean Shutdown + Re-Initialize roundtrip
    vk.Shutdown();
    SX_REQUIRE(!vk.IsInitialized());
    SX_REQUIRE(vk.Initialize(true).ok());
    SX_REQUIRE(vk.IsInitialized());
    std::vector<uint32_t> out_reinit;
    SX_REQUIRE(vk.ExecuteComputeShader(spv_mul_add, input, out_reinit, static_cast<uint32_t>(input.size())).ok());
    SX_REQUIRE(out_reinit == out1);
    vk.Shutdown();

    // 2. Explicit software SPIR-V compute fallback parity test
    scarlettx::VulkanBackend vk_sw;
    SX_REQUIRE(vk_sw.InitializeSoftwareFallback().ok());
    SX_REQUIRE(vk_sw.IsInitialized());
    SX_REQUIRE(vk_sw.is_fallback_mode());

    std::vector<uint32_t> sw_out1, sw_out2;
    SX_REQUIRE(vk_sw.ExecuteComputeShader(spv_mul_add, input, sw_out1, static_cast<uint32_t>(input.size())).ok());
    SX_REQUIRE(vk_sw.ExecuteComputeShader(spv_xor_add, input, sw_out2, static_cast<uint32_t>(input.size())).ok());
    SX_REQUIRE(sw_out1 == out1);
    SX_REQUIRE(sw_out2 == out2);
    vk_sw.Shutdown();
}

}  // namespace

int main() {
    const uint64_t warp_hash = RunDxrPipelineTest(true);
    const uint64_t hw_hash = RunDxrPipelineTest(false);
    SX_REQUIRE(warp_hash == hw_hash);

    RunSpirvValidationTests();
    RunVulkanBackendTests();

    std::cout << "[test_dxr_vulkan] ALL PASSED (dxr_hash=0x" << std::hex << hw_hash << std::dec << ")\n";
    return 0;
}
