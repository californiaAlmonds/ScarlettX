#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"
#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/rt_bvh.h"

namespace scarlettx {

// D3D12 DXR architectural constants (DirectX Raytracing 1.0 / 1.1 specification).
inline constexpr uint32_t kDxrShaderIdentifierSize = 32;
inline constexpr uint32_t kDxrShaderRecordAlignment = 32;
inline constexpr uint32_t kDxrShaderTableAlignment = 64;
inline constexpr uint32_t kDxrAsByteAlignment = 256;
inline constexpr uint32_t kDxrInstanceDescSize = 64;

enum class DxrTier : uint32_t {
    NotSupported = 0,
    Tier1_0 = 10,
    Tier1_1 = 11,
};

enum class DxrAsType : uint32_t {
    TopLevel = 0,
    BottomLevel = 1,
};

enum DxrAsBuildFlags : uint32_t {
    kDxrBuildNone = 0x0,
    kDxrBuildAllowUpdate = 0x1,
    kDxrBuildAllowCompaction = 0x2,
    kDxrBuildPreferFastTrace = 0x4,
    kDxrBuildPreferFastBuild = 0x8,
    kDxrBuildMinimizeMemory = 0x10,
    kDxrBuildPerformUpdate = 0x20,
};

enum DxrInstanceFlags : uint32_t {
    kDxrInstanceNone = 0x0,
    kDxrInstanceTriangleCullDisable = 0x1,
    kDxrInstanceTriangleFrontCounterClockwise = 0x2,
    kDxrInstanceForceOpaque = 0x4,
    kDxrInstanceForceNonOpaque = 0x8,
};

struct DxrCapabilities {
    uint32_t hardware_tier = 0;  // Matches D3D12_RAYTRACING_TIER (0, 10=1.0, 11=1.1)
    bool supports_device5 = false;
    bool supports_state_objects = false;
    bool supports_inline_rt = false;
    bool using_hardware_path = false;
    bool adapter_is_warp = false;
    std::string adapter_description;
};

struct DxrAsInputs {
    DxrAsType type = DxrAsType::BottomLevel;
    uint32_t build_flags = kDxrBuildPreferFastTrace;
    uint32_t primitive_count = 0;  // Triangle count for BLAS; instance count for TLAS
    uint32_t geometry_count = 1;   // Geometry desc count for BLAS (>= 1)
    uint32_t vertex_stride = 12;   // Bytes per vertex (default: float3 = 12)
    bool is_opaque = true;
};

struct DxrPrebuildInfo {
    uint64_t result_data_max_size_bytes = 0;
    uint64_t scratch_data_size_bytes = 0;
    uint64_t update_scratch_data_size_bytes = 0;
    bool from_hardware_driver = false;
};

struct DxrInstanceDesc {
    uint64_t blas_id = 0;
    // Row-major 3x4 affine transform matrix (matches D3D12_RAYTRACING_INSTANCE_DESC::Transform).
    float transform[3][4] = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
    };
    uint32_t instance_id = 0;                          // 24-bit user ID
    uint32_t instance_mask = 0xFFu;                    // 8-bit visibility mask
    uint32_t instance_contribution_to_hit_group_index = 0;  // 24-bit hit group offset
    uint32_t flags = kDxrInstanceNone;                 // 8-bit D3D12_RAYTRACING_INSTANCE_FLAGS
};

struct DxrAsRecordInfo {
    uint64_t id = 0;
    DxrAsType type = DxrAsType::BottomLevel;
    uint64_t gpu_virtual_address = 0;
    uint64_t result_size_bytes = 0;
    uint64_t scratch_size_bytes = 0;
    uint32_t build_flags = 0;
    uint32_t primitive_count = 0;
    uint32_t bvh_node_count = 0;
    bool hardware_built = false;
};

struct DxrShaderRecordDesc {
    std::string export_name;
    // 32-byte shader identifier (from ID3D12StateObjectProperties::GetShaderIdentifier,
    // or left zeroed to derive a deterministic 32-byte identifier from export_name).
    std::array<uint8_t, kDxrShaderIdentifierSize> shader_identifier{};
    // Optional local root arguments appended immediately after the 32-byte identifier.
    std::vector<uint8_t> local_root_arguments;
};

struct DxrShaderTableDesc {
    DxrShaderRecordDesc raygen;
    std::vector<DxrShaderRecordDesc> miss_records;
    std::vector<DxrShaderRecordDesc> hit_group_records;
    std::vector<DxrShaderRecordDesc> callable_records;
    // Optional caller-supplied 64-byte-aligned base GPU VA. If 0, the engine uploads
    // the SBT to a D3D12 upload buffer (when available) or assigns a 64B-aligned VA.
    uint64_t base_gpu_va = 0;
    uint32_t dispatch_width = 1;
    uint32_t dispatch_height = 1;
    uint32_t dispatch_depth = 1;
};

// ABI-exact mirrors of D3D12_GPU_VIRTUAL_ADDRESS_RANGE,
// D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE, and D3D12_DISPATCH_RAYS_DESC
// (verified via static_assert against <d3d12.h> in dxr_hardware.cpp).
struct DxrGpuVirtualAddressRange {
    uint64_t StartAddress = 0;
    uint64_t SizeInBytes = 0;
};

struct DxrGpuVirtualAddressRangeAndStride {
    uint64_t StartAddress = 0;
    uint64_t SizeInBytes = 0;
    uint64_t StrideInBytes = 0;
};

struct DxrDispatchRaysDesc {
    DxrGpuVirtualAddressRange RayGenerationShaderRecord{};
    DxrGpuVirtualAddressRangeAndStride MissShaderTable{};
    DxrGpuVirtualAddressRangeAndStride HitGroupTable{};
    DxrGpuVirtualAddressRangeAndStride CallableShaderTable{};
    uint32_t Width = 1;
    uint32_t Height = 1;
    uint32_t Depth = 1;
};

struct DxrShaderBindingTable {
    std::vector<uint8_t> buffer;
    uint64_t base_gpu_va = 0;
    uint64_t raygen_offset = 0;
    uint64_t miss_offset = 0;
    uint64_t hit_group_offset = 0;
    uint64_t callable_offset = 0;
    uint64_t raygen_record_size = 0;
    uint64_t miss_record_stride = 0;
    uint64_t hit_group_record_stride = 0;
    uint64_t callable_record_stride = 0;
    bool uploaded_to_gpu = false;
    DxrDispatchRaysDesc dispatch_rays_desc{};
};

struct RtDispatchDesc {
    uint32_t width = 160;
    uint32_t height = 120;
    uint32_t depth = 1;
    Vec3 camera_pos{0.0f, 1.0f, -3.5f};
    Vec3 camera_look_at{0.0f, 0.5f, 0.0f};
    Vec3 light_pos{1.5f, 3.0f, -1.5f};
    uint32_t max_bounces = 2;
    const DxrShaderBindingTable* sbt = nullptr;
};

// Clean-room Hardware DXR Tier 1.0 / 1.1 engine wrapping ID3D12Device5 and
// ID3D12GraphicsCommandList4, with transparent 256B-aligned software BVH fallback
// on WARP and non-DXR adapters.
class DxrHardwareEngine {
public:
    DxrHardwareEngine();
    ~DxrHardwareEngine();

    DxrHardwareEngine(const DxrHardwareEngine&) = delete;
    DxrHardwareEngine& operator=(const DxrHardwareEngine&) = delete;

    Status Initialize(D3D12Backend& backend, bool force_software_path = false);
    void Shutdown();
    bool IsInitialized() const noexcept;

    const DxrCapabilities& Capabilities() const noexcept { return caps_; }
    uint32_t hardware_tier() const noexcept { return caps_.hardware_tier; }
    bool supports_inline_rt() const noexcept { return caps_.supports_inline_rt; }
    bool supports_state_objects() const noexcept { return caps_.supports_state_objects; }
    bool using_hardware_path() const noexcept { return caps_.using_hardware_path; }

    Status QueryPrebuildInfo(const DxrAsInputs& inputs, DxrPrebuildInfo& out_info) const;

    Status BuildBlas(const std::vector<RtTriangle>& triangles,
                     uint32_t build_flags,
                     uint64_t& out_blas_id);

    Status UpdateBlas(uint64_t blas_id, const std::vector<RtTriangle>& triangles);

    Status BuildTlas(const std::vector<DxrInstanceDesc>& instances,
                     uint32_t build_flags,
                     uint64_t& out_tlas_id);

    Status GetAsInfo(uint64_t as_id, DxrAsRecordInfo& out_info) const;
    Status DestroyAs(uint64_t as_id);

    Status BuildShaderBindingTable(const DxrShaderTableDesc& desc,
                                   DxrShaderBindingTable& out_sbt);

    Status DispatchRays(uint64_t tlas_id,
                        const RtDispatchDesc& desc,
                        FrameBufferRgba8& out_frame,
                        RtStats* out_stats = nullptr,
                        uint64_t* out_frame_hash = nullptr);

    uint64_t LastFrameHash() const noexcept;
    uint64_t TotalDispatches() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    DxrCapabilities caps_{};
};

}  // namespace scarlettx
