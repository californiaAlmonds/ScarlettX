#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

inline constexpr uint32_t kSpirvMagicNumber = 0x07230203u;

enum class VulkanDeviceType : uint32_t {
    Other = 0,
    IntegratedGpu = 1,
    DiscreteGpu = 2,
    VirtualGpu = 3,
    Cpu = 4,
};

struct VulkanDeviceInfo {
    std::string device_name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t api_version = 0;
    uint32_t api_major = 0;
    uint32_t api_minor = 0;
    uint32_t api_patch = 0;
    uint32_t driver_version = 0;
    VulkanDeviceType device_type = VulkanDeviceType::Other;
    uint32_t compute_queue_family = 0;
    bool supports_compute = false;
    bool supports_graphics = false;
    bool is_hardware = false;
    bool is_fallback = false;
};

struct SpirvValidationInfo {
    uint32_t version_major = 0;
    uint32_t version_minor = 0;
    uint32_t generator = 0;
    uint32_t bound = 0;
    uint32_t instruction_count = 0;
    uint32_t entry_point_count = 0;
    bool has_compute_entry = false;
    std::string entry_point_name;
    uint32_t local_size_x = 1;
    uint32_t local_size_y = 1;
    uint32_t local_size_z = 1;
};

enum class VulkanCommandOp : uint32_t {
    Nop = 0,
    PipelineBarrier = 1,
    BindComputePipeline = 2,
    BindDescriptorSets = 3,
    Dispatch = 4,
};

struct VulkanCommandItem {
    VulkanCommandOp op = VulkanCommandOp::Nop;
    uint32_t x = 1;
    uint32_t y = 1;
    uint32_t z = 1;
    uint64_t resource_id = 0;
};

struct VulkanCommandListDesc {
    std::vector<VulkanCommandItem> commands;
    bool submit_empty_fence_sync = true;
};

struct VulkanCommandListResult {
    uint32_t recorded_commands = 0;
    uint32_t dispatch_count = 0;
    uint32_t barrier_count = 0;
    bool executed_on_vulkan_icd = false;
};

// Clean-room Dynamic Vulkan 1.2/1.3 backend with zero Vulkan SDK header dependencies.
// Dynamically loads vulkan-1.dll at runtime via LoadLibraryA + vkGetInstanceProcAddr,
// executes real hardware/ICD compute pipelines when available, validates SPIR-V binaries,
// and provides a deterministic SPIR-V compute interpreter fallback when no ICD is present.
class VulkanBackend {
public:
    VulkanBackend();
    ~VulkanBackend();

    VulkanBackend(const VulkanBackend&) = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

    // Attempts to initialize a real Vulkan 1.2/1.3 ICD via vulkan-1.dll.
    // If no ICD is available and allow_software_fallback is true, enters deterministic
    // software SPIR-V compute fallback mode.
    Status Initialize(bool allow_software_fallback = true);

    // Explicitly initializes the deterministic software SPIR-V compute fallback mode
    // (useful for testing fallback behavior even on hosts with hardware Vulkan GPUs).
    Status InitializeSoftwareFallback();

    void Shutdown();
    bool IsInitialized() const noexcept;
    bool is_fallback_mode() const noexcept;

    const VulkanDeviceInfo& GetDeviceInfo() const noexcept { return device_info_; }

    // Validates SPIR-V binary magic (0x07230203), version, header bounds, instruction
    // word lengths, OpMemoryModel presence, and balanced OpFunction/OpFunctionEnd blocks.
    static Status ValidateSpirvBinary(const std::vector<uint32_t>& spirv,
                                      SpirvValidationInfo* out_info = nullptr);

    // Validates SPIR-V and executes a 1D compute shader with storage buffer binding 0 (input)
    // and binding 1 (output) across group_x workgroups on real Vulkan GPU or software fallback.
    Status ExecuteComputeShader(const std::vector<uint32_t>& spirv,
                                const std::vector<uint32_t>& input_u32,
                                std::vector<uint32_t>& output_u32,
                                uint32_t group_x);

    // Records a command list (real VkCommandBuffer begin/barrier/end/submit on Vulkan ICD,
    // or validated recording in software fallback mode).
    Status RecordCommandList(const VulkanCommandListDesc& desc,
                             VulkanCommandListResult& out_result);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    VulkanDeviceInfo device_info_{};
};

}  // namespace scarlettx
