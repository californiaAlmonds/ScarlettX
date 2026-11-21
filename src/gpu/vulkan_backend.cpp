#include "scarlettx/gpu/vulkan_backend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#define SX_VKAPI_PTR __stdcall
#else
#define SX_VKAPI_PTR
#endif

namespace scarlettx {

namespace {

// ---------------------------------------------------------------------------
// Clean-room minimal Vulkan 1.2/1.3 C ABI declarations (zero SDK headers).
// Follows the public Khronos Vulkan 1.2/1.3 Specification ABI for x86-64.
// ---------------------------------------------------------------------------
using VkFlags = uint32_t;
using VkBool32 = uint32_t;
using VkDeviceSize = uint64_t;
using VkResult = int32_t;

struct VkInstance_T;
struct VkPhysicalDevice_T;
struct VkDevice_T;
struct VkQueue_T;
struct VkCommandBuffer_T;

using VkInstance = VkInstance_T*;
using VkPhysicalDevice = VkPhysicalDevice_T*;
using VkDevice = VkDevice_T*;
using VkQueue = VkQueue_T*;
using VkCommandBuffer = VkCommandBuffer_T*;

using VkDeviceMemory = uint64_t;
using VkBuffer = uint64_t;
using VkShaderModule = uint64_t;
using VkPipelineCache = uint64_t;
using VkPipelineLayout = uint64_t;
using VkPipeline = uint64_t;
using VkDescriptorSetLayout = uint64_t;
using VkDescriptorPool = uint64_t;
using VkDescriptorSet = uint64_t;
using VkCommandPool = uint64_t;
using VkFence = uint64_t;

inline constexpr uint64_t kVkNullHandle = 0;
inline constexpr VkDeviceSize kVkWholeSize = ~0ull;
inline constexpr uint32_t kVkQueueFamilyIgnored = ~0u;

inline constexpr VkResult kVkSuccess = 0;

constexpr uint32_t VkMakeApiVersion(uint32_t variant, uint32_t major, uint32_t minor, uint32_t patch) noexcept {
    return (variant << 29u) | (major << 22u) | (minor << 12u) | patch;
}
constexpr uint32_t VkApiMajor(uint32_t v) noexcept { return (v >> 22u) & 0x7Fu; }
constexpr uint32_t VkApiMinor(uint32_t v) noexcept { return (v >> 12u) & 0x3FFu; }
constexpr uint32_t VkApiPatch(uint32_t v) noexcept { return v & 0xFFFu; }

inline constexpr uint32_t kVkApiVersion1_0 = VkMakeApiVersion(0, 1, 0, 0);
inline constexpr uint32_t kVkApiVersion1_2 = VkMakeApiVersion(0, 1, 2, 0);
inline constexpr uint32_t kVkApiVersion1_3 = VkMakeApiVersion(0, 1, 3, 0);

// VkStructureType constants
inline constexpr int32_t kVkStypeApplicationInfo = 0;
inline constexpr int32_t kVkStypeInstanceCreateInfo = 1;
inline constexpr int32_t kVkStypeDeviceQueueCreateInfo = 2;
inline constexpr int32_t kVkStypeDeviceCreateInfo = 3;
inline constexpr int32_t kVkStypeSubmitInfo = 4;
inline constexpr int32_t kVkStypeMemoryAllocateInfo = 5;
inline constexpr int32_t kVkStypeMappedMemoryRange = 6;
inline constexpr int32_t kVkStypeFenceCreateInfo = 8;
inline constexpr int32_t kVkStypeBufferCreateInfo = 12;
inline constexpr int32_t kVkStypeShaderModuleCreateInfo = 16;
inline constexpr int32_t kVkStypePipelineShaderStageCreateInfo = 18;
inline constexpr int32_t kVkStypeComputePipelineCreateInfo = 29;
inline constexpr int32_t kVkStypePipelineLayoutCreateInfo = 30;
inline constexpr int32_t kVkStypeDescriptorSetLayoutCreateInfo = 32;
inline constexpr int32_t kVkStypeDescriptorPoolCreateInfo = 33;
inline constexpr int32_t kVkStypeDescriptorSetAllocateInfo = 34;
inline constexpr int32_t kVkStypeWriteDescriptorSet = 35;
inline constexpr int32_t kVkStypeCommandPoolCreateInfo = 39;
inline constexpr int32_t kVkStypeCommandBufferAllocateInfo = 40;
inline constexpr int32_t kVkStypeCommandBufferBeginInfo = 42;
inline constexpr int32_t kVkStypeBufferMemoryBarrier = 44;
inline constexpr int32_t kVkStypeMemoryBarrier = 46;

// Flags & enums
inline constexpr uint32_t kVkQueueGraphicsBit = 0x00000001u;
inline constexpr uint32_t kVkQueueComputeBit = 0x00000002u;
inline constexpr uint32_t kVkMemoryPropertyHostVisibleBit = 0x00000002u;
inline constexpr uint32_t kVkMemoryPropertyHostCoherentBit = 0x00000004u;
inline constexpr uint32_t kVkBufferUsageStorageBufferBit = 0x00000020u;
inline constexpr int32_t kVkSharingModeExclusive = 0;
inline constexpr int32_t kVkDescriptorTypeStorageBuffer = 7;
inline constexpr uint32_t kVkShaderStageComputeBit = 0x00000020u;
inline constexpr int32_t kVkPipelineBindPointCompute = 1;
inline constexpr uint32_t kVkCommandPoolCreateResetCmdBufBit = 0x00000002u;
inline constexpr int32_t kVkCommandBufferLevelPrimary = 0;
inline constexpr uint32_t kVkCommandBufferUsageOneTimeSubmitBit = 0x00000001u;
inline constexpr uint32_t kVkPipelineStageComputeShaderBit = 0x00000800u;
inline constexpr uint32_t kVkPipelineStageHostBit = 0x00004000u;
inline constexpr uint32_t kVkAccessShaderWriteBit = 0x00000040u;
inline constexpr uint32_t kVkAccessHostReadBit = 0x00002000u;

struct VkApplicationInfo {
    int32_t sType;
    const void* pNext;
    const char* pApplicationName;
    uint32_t applicationVersion;
    const char* pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
};

struct VkInstanceCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};

// Generously padded after pipelineCacheUUID to cover VkPhysicalDeviceLimits (504B)
// + VkPhysicalDeviceSparseProperties (20B) with zero risk of stack overflow.
struct alignas(8) VkPhysicalDevicePropertiesRaw {
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t deviceType;
    char deviceName[256];
    uint8_t pipelineCacheUUID[16];
    alignas(8) uint8_t limits_and_sparse_storage[1024];
};
static_assert(offsetof(VkPhysicalDevicePropertiesRaw, deviceName) == 20, "VkPhysicalDeviceProperties layout");
static_assert(offsetof(VkPhysicalDevicePropertiesRaw, pipelineCacheUUID) == 276, "UUID offset");

struct VkExtent3D {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
};

struct VkQueueFamilyProperties {
    VkFlags queueFlags;
    uint32_t queueCount;
    uint32_t timestampValidBits;
    VkExtent3D minImageTransferGranularity;
};

struct VkMemoryType {
    VkFlags propertyFlags;
    uint32_t heapIndex;
};

struct VkMemoryHeap {
    VkDeviceSize size;
    VkFlags flags;
};

struct VkPhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount;
    VkMemoryType memoryTypes[32];
    uint32_t memoryHeapCount;
    VkMemoryHeap memoryHeaps[16];
};

struct VkDeviceQueueCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float* pQueuePriorities;
};

struct VkDeviceCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfo* pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
    const void* pEnabledFeatures;
};

struct VkCommandPoolCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t queueFamilyIndex;
};

struct VkCommandBufferAllocateInfo {
    int32_t sType;
    const void* pNext;
    VkCommandPool commandPool;
    int32_t level;
    uint32_t commandBufferCount;
};

struct VkCommandBufferBeginInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    const void* pInheritanceInfo;
};

struct VkSubmitInfo {
    int32_t sType;
    const void* pNext;
    uint32_t waitSemaphoreCount;
    const uint64_t* pWaitSemaphores;
    const VkFlags* pWaitDstStageMask;
    uint32_t commandBufferCount;
    const VkCommandBuffer* pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const uint64_t* pSignalSemaphores;
};

struct VkBufferCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkDeviceSize size;
    VkFlags usage;
    int32_t sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
};

struct VkMemoryRequirements {
    VkDeviceSize size;
    VkDeviceSize alignment;
    uint32_t memoryTypeBits;
};

struct VkMemoryAllocateInfo {
    int32_t sType;
    const void* pNext;
    VkDeviceSize allocationSize;
    uint32_t memoryTypeIndex;
};

struct VkMappedMemoryRange {
    int32_t sType;
    const void* pNext;
    VkDeviceMemory memory;
    VkDeviceSize offset;
    VkDeviceSize size;
};

struct VkShaderModuleCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    size_t codeSize;
    const uint32_t* pCode;
};

struct VkDescriptorSetLayoutBinding {
    uint32_t binding;
    int32_t descriptorType;
    uint32_t descriptorCount;
    VkFlags stageFlags;
    const uint64_t* pImmutableSamplers;
};

struct VkDescriptorSetLayoutCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t bindingCount;
    const VkDescriptorSetLayoutBinding* pBindings;
};

struct VkPipelineLayoutCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t setLayoutCount;
    const VkDescriptorSetLayout* pSetLayouts;
    uint32_t pushConstantRangeCount;
    const void* pPushConstantRanges;
};

struct VkPipelineShaderStageCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkFlags stage;
    VkShaderModule module;
    const char* pName;
    const void* pSpecializationInfo;
};

struct VkComputePipelineCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkPipelineShaderStageCreateInfo stage;
    VkPipelineLayout layout;
    VkPipeline basePipelineHandle;
    int32_t basePipelineIndex;
};

struct VkDescriptorPoolSize {
    int32_t type;
    uint32_t descriptorCount;
};

struct VkDescriptorPoolCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t maxSets;
    uint32_t poolSizeCount;
    const VkDescriptorPoolSize* pPoolSizes;
};

struct VkDescriptorSetAllocateInfo {
    int32_t sType;
    const void* pNext;
    VkDescriptorPool descriptorPool;
    uint32_t descriptorSetCount;
    const VkDescriptorSetLayout* pSetLayouts;
};

struct VkDescriptorBufferInfo {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize range;
};

struct VkWriteDescriptorSet {
    int32_t sType;
    const void* pNext;
    VkDescriptorSet dstSet;
    uint32_t dstBinding;
    uint32_t dstArrayElement;
    uint32_t descriptorCount;
    int32_t descriptorType;
    const void* pImageInfo;
    const VkDescriptorBufferInfo* pBufferInfo;
    const uint64_t* pTexelBufferView;
};

struct VkMemoryBarrier {
    int32_t sType;
    const void* pNext;
    VkFlags srcAccessMask;
    VkFlags dstAccessMask;
};

struct VkBufferMemoryBarrier {
    int32_t sType;
    const void* pNext;
    VkFlags srcAccessMask;
    VkFlags dstAccessMask;
    uint32_t srcQueueFamilyIndex;
    uint32_t dstQueueFamilyIndex;
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize size;
};

using PFN_vkVoidFunction = void(SX_VKAPI_PTR*)();
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(SX_VKAPI_PTR*)(VkInstance, const char*);
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction(SX_VKAPI_PTR*)(VkDevice, const char*);
using PFN_vkEnumerateInstanceVersion = VkResult(SX_VKAPI_PTR*)(uint32_t*);
using PFN_vkCreateInstance = VkResult(SX_VKAPI_PTR*)(const VkInstanceCreateInfo*, const void*, VkInstance*);
using PFN_vkDestroyInstance = void(SX_VKAPI_PTR*)(VkInstance, const void*);
using PFN_vkEnumeratePhysicalDevices = VkResult(SX_VKAPI_PTR*)(VkInstance, uint32_t*, VkPhysicalDevice*);
using PFN_vkGetPhysicalDeviceProperties = void(SX_VKAPI_PTR*)(VkPhysicalDevice, VkPhysicalDevicePropertiesRaw*);
using PFN_vkGetPhysicalDeviceQueueFamilyProperties = void(SX_VKAPI_PTR*)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*);
using PFN_vkGetPhysicalDeviceMemoryProperties = void(SX_VKAPI_PTR*)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);
using PFN_vkCreateDevice = VkResult(SX_VKAPI_PTR*)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*);
using PFN_vkDestroyDevice = void(SX_VKAPI_PTR*)(VkDevice, const void*);
using PFN_vkDeviceWaitIdle = VkResult(SX_VKAPI_PTR*)(VkDevice);
using PFN_vkGetDeviceQueue = void(SX_VKAPI_PTR*)(VkDevice, uint32_t, uint32_t, VkQueue*);
using PFN_vkQueueSubmit = VkResult(SX_VKAPI_PTR*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
using PFN_vkQueueWaitIdle = VkResult(SX_VKAPI_PTR*)(VkQueue);
using PFN_vkCreateCommandPool = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkCommandPoolCreateInfo*, const void*, VkCommandPool*);
using PFN_vkDestroyCommandPool = void(SX_VKAPI_PTR*)(VkDevice, VkCommandPool, const void*);
using PFN_vkAllocateCommandBuffers = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
using PFN_vkFreeCommandBuffers = void(SX_VKAPI_PTR*)(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*);
using PFN_vkBeginCommandBuffer = VkResult(SX_VKAPI_PTR*)(VkCommandBuffer, const VkCommandBufferBeginInfo*);
using PFN_vkEndCommandBuffer = VkResult(SX_VKAPI_PTR*)(VkCommandBuffer);
using PFN_vkCmdBindPipeline = void(SX_VKAPI_PTR*)(VkCommandBuffer, int32_t, VkPipeline);
using PFN_vkCmdBindDescriptorSets = void(SX_VKAPI_PTR*)(VkCommandBuffer, int32_t, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*);
using PFN_vkCmdDispatch = void(SX_VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
using PFN_vkCmdPipelineBarrier = void(SX_VKAPI_PTR*)(VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const void*);
using PFN_vkCreateBuffer = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkBufferCreateInfo*, const void*, VkBuffer*);
using PFN_vkDestroyBuffer = void(SX_VKAPI_PTR*)(VkDevice, VkBuffer, const void*);
using PFN_vkGetBufferMemoryRequirements = void(SX_VKAPI_PTR*)(VkDevice, VkBuffer, VkMemoryRequirements*);
using PFN_vkAllocateMemory = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkMemoryAllocateInfo*, const void*, VkDeviceMemory*);
using PFN_vkFreeMemory = void(SX_VKAPI_PTR*)(VkDevice, VkDeviceMemory, const void*);
using PFN_vkBindBufferMemory = VkResult(SX_VKAPI_PTR*)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
using PFN_vkMapMemory = VkResult(SX_VKAPI_PTR*)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**);
using PFN_vkUnmapMemory = void(SX_VKAPI_PTR*)(VkDevice, VkDeviceMemory);
using PFN_vkFlushMappedMemoryRanges = VkResult(SX_VKAPI_PTR*)(VkDevice, uint32_t, const VkMappedMemoryRange*);
using PFN_vkInvalidateMappedMemoryRanges = VkResult(SX_VKAPI_PTR*)(VkDevice, uint32_t, const VkMappedMemoryRange*);
using PFN_vkCreateShaderModule = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkShaderModuleCreateInfo*, const void*, VkShaderModule*);
using PFN_vkDestroyShaderModule = void(SX_VKAPI_PTR*)(VkDevice, VkShaderModule, const void*);
using PFN_vkCreateDescriptorSetLayout = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const void*, VkDescriptorSetLayout*);
using PFN_vkDestroyDescriptorSetLayout = void(SX_VKAPI_PTR*)(VkDevice, VkDescriptorSetLayout, const void*);
using PFN_vkCreatePipelineLayout = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkPipelineLayoutCreateInfo*, const void*, VkPipelineLayout*);
using PFN_vkDestroyPipelineLayout = void(SX_VKAPI_PTR*)(VkDevice, VkPipelineLayout, const void*);
using PFN_vkCreateComputePipelines = VkResult(SX_VKAPI_PTR*)(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const void*, VkPipeline*);
using PFN_vkDestroyPipeline = void(SX_VKAPI_PTR*)(VkDevice, VkPipeline, const void*);
using PFN_vkCreateDescriptorPool = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkDescriptorPoolCreateInfo*, const void*, VkDescriptorPool*);
using PFN_vkDestroyDescriptorPool = void(SX_VKAPI_PTR*)(VkDevice, VkDescriptorPool, const void*);
using PFN_vkAllocateDescriptorSets = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*);
using PFN_vkUpdateDescriptorSets = void(SX_VKAPI_PTR*)(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const void*);

// ---------------------------------------------------------------------------
// SPIR-V opcodes & constants (Khronos SPIR-V 1.0–1.6 Specification)
// ---------------------------------------------------------------------------
inline constexpr uint16_t kSpvOpMemoryModel = 14;
inline constexpr uint16_t kSpvOpEntryPoint = 15;
inline constexpr uint16_t kSpvOpExecutionMode = 16;
inline constexpr uint16_t kSpvOpCapability = 17;
inline constexpr uint16_t kSpvOpTypeVoid = 19;
inline constexpr uint16_t kSpvOpTypeBool = 20;
inline constexpr uint16_t kSpvOpTypeInt = 21;
inline constexpr uint16_t kSpvOpTypeFloat = 22;
inline constexpr uint16_t kSpvOpTypeVector = 23;
inline constexpr uint16_t kSpvOpTypeRuntimeArray = 29;
inline constexpr uint16_t kSpvOpTypeStruct = 30;
inline constexpr uint16_t kSpvOpTypePointer = 32;
inline constexpr uint16_t kSpvOpTypeFunction = 33;
inline constexpr uint16_t kSpvOpConstant = 43;
inline constexpr uint16_t kSpvOpFunction = 54;
inline constexpr uint16_t kSpvOpFunctionEnd = 56;
inline constexpr uint16_t kSpvOpVariable = 59;
inline constexpr uint16_t kSpvOpLoad = 61;
inline constexpr uint16_t kSpvOpStore = 62;
inline constexpr uint16_t kSpvOpAccessChain = 65;
inline constexpr uint16_t kSpvOpDecorate = 71;
inline constexpr uint16_t kSpvOpIAdd = 128;
inline constexpr uint16_t kSpvOpFAdd = 129;
inline constexpr uint16_t kSpvOpISub = 130;
inline constexpr uint16_t kSpvOpFSub = 131;
inline constexpr uint16_t kSpvOpIMul = 132;
inline constexpr uint16_t kSpvOpFMul = 133;
inline constexpr uint16_t kSpvOpUDiv = 134;
inline constexpr uint16_t kSpvOpSDiv = 135;
inline constexpr uint16_t kSpvOpFDiv = 136;
inline constexpr uint16_t kSpvOpUMod = 137;
inline constexpr uint16_t kSpvOpShiftRightLogical = 194;
inline constexpr uint16_t kSpvOpShiftRightArithmetic = 195;
inline constexpr uint16_t kSpvOpShiftLeftLogical = 196;
inline constexpr uint16_t kSpvOpBitwiseOr = 197;
inline constexpr uint16_t kSpvOpBitwiseXor = 198;
inline constexpr uint16_t kSpvOpBitwiseAnd = 199;
inline constexpr uint16_t kSpvOpLabel = 248;
inline constexpr uint16_t kSpvOpReturn = 253;

inline constexpr uint32_t kSpvExecModelGLCompute = 5;
inline constexpr uint32_t kSpvExecModeLocalSize = 17;
inline constexpr uint32_t kSpvDecBuiltIn = 11;
inline constexpr uint32_t kSpvDecBinding = 33;
inline constexpr uint32_t kSpvDecDescriptorSet = 34;
inline constexpr uint32_t kSpvBuiltInGlobalInvocationId = 28;

// Extracts a NUL-terminated UTF-8 literal string from SPIR-V words [start_word, end_word).
bool ReadSpirvString(const std::vector<uint32_t>& words,
                     size_t start_word,
                     size_t end_word,
                     std::string& out_str,
                     size_t& out_next_word) {
    out_str.clear();
    for (size_t w = start_word; w < end_word; ++w) {
        const uint32_t val = words[w];
        for (int b = 0; b < 4; ++b) {
            const char ch = static_cast<char>((val >> (b * 8)) & 0xFFu);
            if (ch == '\0') {
                out_next_word = w + 1;
                return true;
            }
            out_str.push_back(ch);
        }
    }
    return false;
}

// Deterministic clean-room SPIR-V compute interpreter used when no Vulkan ICD is present
// (or when InitializeSoftwareFallback() is requested). Executes real SPIR-V instructions
// on storage buffers binding 0 (input) and binding 1 (output).
Status ExecuteSpirvComputeSoftware(const std::vector<uint32_t>& spirv,
                                   const SpirvValidationInfo& val_info,
                                   const std::vector<uint32_t>& input_u32,
                                   std::vector<uint32_t>& output_u32,
                                   uint32_t group_x) {
    struct PointerRef {
        enum class Kind : uint8_t { None, GlobalInvocationId, InputBuffer, OutputBuffer };
        Kind kind = Kind::None;
        uint32_t index = 0;
        bool has_element = false;
    };

    const uint32_t bound = spirv[3];
    std::unordered_map<uint32_t, uint32_t> constants_u32;
    std::unordered_map<uint32_t, uint32_t> var_binding;
    std::unordered_map<uint32_t, uint32_t> var_builtin;
    uint32_t entry_func_id = 0;

    // First pass: collect entry point, decorations, and constants.
    size_t pos = 5;
    size_t entry_body_start = 0;
    size_t entry_body_end = 0;
    uint32_t current_func = 0;
    size_t current_func_start = 0;

    while (pos < spirv.size()) {
        const uint32_t instr = spirv[pos];
        const uint16_t wc = static_cast<uint16_t>(instr >> 16);
        const uint16_t op = static_cast<uint16_t>(instr & 0xFFFFu);

        if (op == kSpvOpEntryPoint && wc >= 4 && spirv[pos + 1] == kSpvExecModelGLCompute) {
            entry_func_id = spirv[pos + 2];
        } else if (op == kSpvOpDecorate && wc >= 4) {
            const uint32_t target = spirv[pos + 1];
            const uint32_t dec = spirv[pos + 2];
            if (dec == kSpvDecBinding) {
                var_binding[target] = spirv[pos + 3];
            } else if (dec == kSpvDecBuiltIn) {
                var_builtin[target] = spirv[pos + 3];
            }
        } else if (op == kSpvOpConstant && wc >= 4) {
            constants_u32[spirv[pos + 2]] = spirv[pos + 3];
        } else if (op == kSpvOpFunction && wc >= 5) {
            current_func = spirv[pos + 2];
            current_func_start = pos + wc;
        } else if (op == kSpvOpFunctionEnd) {
            if (current_func == entry_func_id && entry_func_id != 0) {
                entry_body_start = current_func_start;
                entry_body_end = pos;
            }
            current_func = 0;
        }
        pos += wc;
    }

    if (entry_func_id == 0 || entry_body_start == 0 || entry_body_start >= entry_body_end) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V compute entry function body not found");
    }

    const uint32_t local_x = std::max<uint32_t>(1, val_info.local_size_x);
    const uint32_t total_invocations = group_x * local_x;
    if (output_u32.size() < std::max<size_t>(input_u32.size(), total_invocations)) {
        output_u32.resize(std::max<size_t>(input_u32.size(), total_invocations), 0u);
    }

    std::vector<uint32_t> regs(bound, 0u);
    std::vector<PointerRef> ptrs(bound);

    for (const auto& [id, cval] : constants_u32) {
        if (id < bound) regs[id] = cval;
    }

    // Bind known root variables.
    std::vector<PointerRef> root_ptrs(bound);
    for (const auto& [id, bi] : var_builtin) {
        if (id < bound && bi == kSpvBuiltInGlobalInvocationId) {
            root_ptrs[id] = {PointerRef::Kind::GlobalInvocationId, 0, false};
        }
    }
    for (const auto& [id, binding] : var_binding) {
        if (id < bound) {
            if (binding == 0) {
                root_ptrs[id] = {PointerRef::Kind::InputBuffer, 0, false};
            } else if (binding == 1) {
                root_ptrs[id] = {PointerRef::Kind::OutputBuffer, 0, false};
            }
        }
    }

    auto read_float = [&](uint32_t id) -> float {
        float f = 0.0f;
        std::memcpy(&f, &regs[id], sizeof(float));
        return f;
    };
    auto write_float = [&](uint32_t id, float f) {
        std::memcpy(&regs[id], &f, sizeof(float));
    };

    for (uint32_t gid_x = 0; gid_x < total_invocations; ++gid_x) {
        ptrs = root_ptrs;
        const uint32_t gid[3] = {gid_x, 0u, 0u};

        size_t ip = entry_body_start;
        while (ip < entry_body_end) {
            const uint32_t instr = spirv[ip];
            const uint16_t wc = static_cast<uint16_t>(instr >> 16);
            const uint16_t op = static_cast<uint16_t>(instr & 0xFFFFu);

            switch (op) {
                case kSpvOpLabel:
                    break;
                case kSpvOpAccessChain: {
                    if (wc >= 5) {
                        const uint32_t res = spirv[ip + 2];
                        const uint32_t base = spirv[ip + 3];
                        PointerRef ref = (base < bound) ? ptrs[base] : PointerRef{};
                        if (ref.kind == PointerRef::Kind::GlobalInvocationId) {
                            const uint32_t comp_id = spirv[ip + 4];
                            ref.index = (comp_id < bound) ? regs[comp_id] : 0u;
                            ref.has_element = true;
                        } else if (ref.kind == PointerRef::Kind::InputBuffer ||
                                   ref.kind == PointerRef::Kind::OutputBuffer) {
                            // Member 0 is runtime array; last index is array element index.
                            const uint32_t elem_id = spirv[ip + wc - 1];
                            ref.index = (elem_id < bound) ? regs[elem_id] : 0u;
                            ref.has_element = true;
                        }
                        if (res < bound) ptrs[res] = ref;
                    }
                    break;
                }
                case kSpvOpLoad: {
                    if (wc >= 4) {
                        const uint32_t res = spirv[ip + 2];
                        const uint32_t ptr_id = spirv[ip + 3];
                        const PointerRef ref = (ptr_id < bound) ? ptrs[ptr_id] : PointerRef{};
                        uint32_t loaded = 0;
                        if (ref.kind == PointerRef::Kind::GlobalInvocationId) {
                            loaded = (ref.index < 3) ? gid[ref.index] : 0u;
                        } else if (ref.kind == PointerRef::Kind::InputBuffer) {
                            loaded = (ref.index < input_u32.size()) ? input_u32[ref.index] : 0u;
                        } else if (ref.kind == PointerRef::Kind::OutputBuffer) {
                            loaded = (ref.index < output_u32.size()) ? output_u32[ref.index] : 0u;
                        }
                        if (res < bound) regs[res] = loaded;
                    }
                    break;
                }
                case kSpvOpStore: {
                    if (wc >= 3) {
                        const uint32_t ptr_id = spirv[ip + 1];
                        const uint32_t val_id = spirv[ip + 2];
                        const PointerRef ref = (ptr_id < bound) ? ptrs[ptr_id] : PointerRef{};
                        const uint32_t val = (val_id < bound) ? regs[val_id] : 0u;
                        if (ref.kind == PointerRef::Kind::OutputBuffer && ref.index < output_u32.size()) {
                            output_u32[ref.index] = val;
                        }
                    }
                    break;
                }
                case kSpvOpIAdd:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] + regs[spirv[ip + 4]];
                    break;
                case kSpvOpISub:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] - regs[spirv[ip + 4]];
                    break;
                case kSpvOpIMul:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] * regs[spirv[ip + 4]];
                    break;
                case kSpvOpUDiv:
                    if (wc >= 5 && spirv[ip + 2] < bound) {
                        const uint32_t d = regs[spirv[ip + 4]];
                        regs[spirv[ip + 2]] = (d != 0) ? (regs[spirv[ip + 3]] / d) : 0u;
                    }
                    break;
                case kSpvOpSDiv:
                    if (wc >= 5 && spirv[ip + 2] < bound) {
                        const int32_t a = static_cast<int32_t>(regs[spirv[ip + 3]]);
                        const int32_t b = static_cast<int32_t>(regs[spirv[ip + 4]]);
                        regs[spirv[ip + 2]] = (b != 0) ? static_cast<uint32_t>(a / b) : 0u;
                    }
                    break;
                case kSpvOpUMod:
                    if (wc >= 5 && spirv[ip + 2] < bound) {
                        const uint32_t d = regs[spirv[ip + 4]];
                        regs[spirv[ip + 2]] = (d != 0) ? (regs[spirv[ip + 3]] % d) : 0u;
                    }
                    break;
                case kSpvOpBitwiseAnd:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] & regs[spirv[ip + 4]];
                    break;
                case kSpvOpBitwiseOr:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] | regs[spirv[ip + 4]];
                    break;
                case kSpvOpBitwiseXor:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] ^ regs[spirv[ip + 4]];
                    break;
                case kSpvOpShiftLeftLogical:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] << (regs[spirv[ip + 4]] & 31u);
                    break;
                case kSpvOpShiftRightLogical:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] >> (regs[spirv[ip + 4]] & 31u);
                    break;
                case kSpvOpShiftRightArithmetic:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = static_cast<uint32_t>(
                            static_cast<int32_t>(regs[spirv[ip + 3]]) >> (regs[spirv[ip + 4]] & 31u));
                    break;
                case kSpvOpFAdd:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        write_float(spirv[ip + 2], read_float(spirv[ip + 3]) + read_float(spirv[ip + 4]));
                    break;
                case kSpvOpFSub:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        write_float(spirv[ip + 2], read_float(spirv[ip + 3]) - read_float(spirv[ip + 4]));
                    break;
                case kSpvOpFMul:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        write_float(spirv[ip + 2], read_float(spirv[ip + 3]) * read_float(spirv[ip + 4]));
                    break;
                case kSpvOpFDiv:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        write_float(spirv[ip + 2], read_float(spirv[ip + 3]) / read_float(spirv[ip + 4]));
                    break;
                case kSpvOpReturn:
                    ip = entry_body_end;
                    continue;
                default:
                    return Status::Error(StatusCode::ShaderError,
                                         "Unsupported SPIR-V opcode in software fallback interpreter");
            }
            ip += wc;
        }
    }

    return Status::Ok();
}

int DeviceTypeRank(uint32_t device_type) noexcept {
    switch (device_type) {
        case 2: return 5;  // Discrete GPU
        case 1: return 4;  // Integrated GPU
        case 3: return 3;  // Virtual GPU
        case 4: return 2;  // CPU (Lavapipe / SwiftShader)
        default: return 1;
    }
}

}  // namespace

struct VulkanBackend::Impl {
    mutable std::mutex mutex;
    bool initialized = false;
    bool fallback_mode = false;

#if defined(_WIN32)
    HMODULE vulkan_dll = nullptr;
#endif
    VkInstance instance = nullptr;
    VkPhysicalDevice physical_device = nullptr;
    VkDevice device = nullptr;
    VkQueue compute_queue = nullptr;
    VkCommandPool command_pool = kVkNullHandle;
    uint32_t compute_queue_family = 0;
    VkPhysicalDeviceMemoryProperties mem_props{};

    // Dynamically resolved Vulkan entry points
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion = nullptr;
    PFN_vkCreateInstance vkCreateInstance = nullptr;
    PFN_vkDestroyInstance vkDestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateDevice vkCreateDevice = nullptr;
    PFN_vkDestroyDevice vkDestroyDevice = nullptr;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
    PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit = nullptr;
    PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;
    PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
    PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
    PFN_vkCmdDispatch vkCmdDispatch = nullptr;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
    PFN_vkCreateBuffer vkCreateBuffer = nullptr;
    PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
    PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
    PFN_vkMapMemory vkMapMemory = nullptr;
    PFN_vkUnmapMemory vkUnmapMemory = nullptr;
    PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges = nullptr;
    PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges = nullptr;
    PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
    PFN_vkCreateComputePipelines vkCreateComputePipelines = nullptr;
    PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;

    template <typename Fn>
    bool LoadInstProc(Fn& out, const char* name) {
        out = reinterpret_cast<Fn>(vkGetInstanceProcAddr(instance, name));
        return out != nullptr;
    }

    template <typename Fn>
    bool LoadDevProc(Fn& out, const char* name) {
        out = reinterpret_cast<Fn>(vkGetDeviceProcAddr(device, name));
        return out != nullptr;
    }

    uint32_t FindMemoryType(uint32_t type_bits, VkFlags required_flags, bool& out_is_coherent) const noexcept {
        // First pass: prefer HOST_VISIBLE | HOST_COHERENT.
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
            if ((type_bits & (1u << i)) != 0) {
                const VkFlags f = mem_props.memoryTypes[i].propertyFlags;
                if ((f & required_flags) == required_flags) {
                    out_is_coherent = (f & kVkMemoryPropertyHostCoherentBit) != 0;
                    return i;
                }
            }
        }
        // Fallback: HOST_VISIBLE only (caller flushes/invalidates manually).
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
            if ((type_bits & (1u << i)) != 0) {
                const VkFlags f = mem_props.memoryTypes[i].propertyFlags;
                if ((f & kVkMemoryPropertyHostVisibleBit) != 0) {
                    out_is_coherent = (f & kVkMemoryPropertyHostCoherentBit) != 0;
                    return i;
                }
            }
        }
        out_is_coherent = false;
        return std::numeric_limits<uint32_t>::max();
    }

    void Cleanup() {
        if (device && vkDeviceWaitIdle) {
            vkDeviceWaitIdle(device);
        }
        if (device && command_pool != kVkNullHandle && vkDestroyCommandPool) {
            vkDestroyCommandPool(device, command_pool, nullptr);
            command_pool = kVkNullHandle;
        }
        if (device && vkDestroyDevice) {
            vkDestroyDevice(device, nullptr);
            device = nullptr;
        }
        compute_queue = nullptr;
        physical_device = nullptr;
        if (instance && vkDestroyInstance) {
            vkDestroyInstance(instance, nullptr);
            instance = nullptr;
        }
#if defined(_WIN32)
        if (vulkan_dll) {
            FreeLibrary(vulkan_dll);
            vulkan_dll = nullptr;
        }
#endif
        vkGetInstanceProcAddr = nullptr;
        vkGetDeviceProcAddr = nullptr;
        initialized = false;
        fallback_mode = false;
    }

    ~Impl() { Cleanup(); }
};

VulkanBackend::VulkanBackend() : impl_(std::make_unique<Impl>()) {}
VulkanBackend::~VulkanBackend() = default;

bool VulkanBackend::IsInitialized() const noexcept {
    return impl_ && impl_->initialized;
}

bool VulkanBackend::is_fallback_mode() const noexcept {
    return impl_ && impl_->fallback_mode;
}

Status VulkanBackend::ValidateSpirvBinary(const std::vector<uint32_t>& spirv,
                                          SpirvValidationInfo* out_info) {
    if (spirv.size() < 5) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V binary too small (< 5-word header)");
    }
    if (spirv[0] != kSpirvMagicNumber) {
        return Status::Error(StatusCode::ShaderError, "Invalid SPIR-V magic number (expected 0x07230203)");
    }

    const uint32_t version = spirv[1];
    if ((version & 0xFF0000FFu) != 0) {
        return Status::Error(StatusCode::ShaderError, "Malformed SPIR-V version word");
    }
    const uint32_t major = (version >> 16u) & 0xFFu;
    const uint32_t minor = (version >> 8u) & 0xFFu;
    if (major != 1 || minor > 6) {
        return Status::Error(StatusCode::ShaderError, "Unsupported SPIR-V version");
    }

    const uint32_t bound = spirv[3];
    if (bound == 0) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V ID bound must be > 0");
    }
    if (spirv[4] != 0) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V reserved schema word must be 0");
    }

    SpirvValidationInfo info{};
    info.version_major = major;
    info.version_minor = minor;
    info.generator = spirv[2];
    info.bound = bound;

    bool saw_memory_model = false;
    bool in_function = false;
    uint32_t compute_entry_id = 0;

    size_t pos = 5;
    while (pos < spirv.size()) {
        const uint32_t word = spirv[pos];
        const uint16_t wc = static_cast<uint16_t>(word >> 16u);
        const uint16_t op = static_cast<uint16_t>(word & 0xFFFFu);

        if (wc == 0) {
            return Status::Error(StatusCode::ShaderError, "SPIR-V instruction has zero word count");
        }
        if (pos + wc > spirv.size()) {
            return Status::Error(StatusCode::ShaderError, "Truncated SPIR-V instruction exceeds binary length");
        }

        ++info.instruction_count;

        switch (op) {
            case kSpvOpMemoryModel:
                if (wc != 3) {
                    return Status::Error(StatusCode::ShaderError, "Invalid OpMemoryModel word count");
                }
                saw_memory_model = true;
                break;

            case kSpvOpEntryPoint: {
                if (wc < 4) {
                    return Status::Error(StatusCode::ShaderError, "Invalid OpEntryPoint word count");
                }
                const uint32_t exec_model = spirv[pos + 1];
                const uint32_t entry_id = spirv[pos + 2];
                if (entry_id == 0 || entry_id >= bound) {
                    return Status::Error(StatusCode::ShaderError, "OpEntryPoint ID out of bound");
                }
                std::string name;
                size_t next_word = 0;
                if (!ReadSpirvString(spirv, pos + 3, pos + wc, name, next_word) || name.empty()) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpEntryPoint name string");
                }
                ++info.entry_point_count;
                if (exec_model == kSpvExecModelGLCompute) {
                    info.has_compute_entry = true;
                    info.entry_point_name = name;
                    compute_entry_id = entry_id;
                }
                break;
            }

            case kSpvOpExecutionMode: {
                if (wc < 3) {
                    return Status::Error(StatusCode::ShaderError, "Invalid OpExecutionMode word count");
                }
                const uint32_t target = spirv[pos + 1];
                const uint32_t mode = spirv[pos + 2];
                if (mode == kSpvExecModeLocalSize && wc >= 6 &&
                    (compute_entry_id == 0 || target == compute_entry_id)) {
                    info.local_size_x = spirv[pos + 3];
                    info.local_size_y = spirv[pos + 4];
                    info.local_size_z = spirv[pos + 5];
                    if (info.local_size_x == 0 || info.local_size_y == 0 || info.local_size_z == 0) {
                        return Status::Error(StatusCode::ShaderError, "LocalSize dimensions must be > 0");
                    }
                }
                break;
            }

            case kSpvOpFunction:
                if (in_function || wc != 5) {
                    return Status::Error(StatusCode::ShaderError, "Malformed or nested OpFunction");
                }
                if (spirv[pos + 2] == 0 || spirv[pos + 2] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "OpFunction result ID out of bound");
                }
                in_function = true;
                break;

            case kSpvOpFunctionEnd:
                if (!in_function || wc != 1) {
                    return Status::Error(StatusCode::ShaderError, "Unmatched OpFunctionEnd");
                }
                in_function = false;
                break;

            case kSpvOpTypeVoid:
            case kSpvOpTypeBool:
            case kSpvOpTypeInt:
            case kSpvOpTypeFloat:
            case kSpvOpTypeVector:
            case kSpvOpTypeRuntimeArray:
            case kSpvOpTypeStruct:
            case kSpvOpTypePointer:
            case kSpvOpTypeFunction:
                if (wc < 2 || spirv[pos + 1] == 0 || spirv[pos + 1] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Type instruction result ID out of bound");
                }
                break;

            case kSpvOpConstant:
            case kSpvOpVariable:
            case kSpvOpLoad:
            case kSpvOpAccessChain:
                if (wc < 3 || spirv[pos + 2] == 0 || spirv[pos + 2] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Instruction result ID out of bound");
                }
                break;

            default:
                break;
        }

        pos += wc;
    }

    if (info.instruction_count == 0) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V binary contains no instructions");
    }
    if (!saw_memory_model) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V binary missing mandatory OpMemoryModel");
    }
    if (in_function) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V binary has unclosed OpFunction");
    }

    if (out_info) {
        *out_info = std::move(info);
    }
    return Status::Ok();
}

Status VulkanBackend::InitializeSoftwareFallback() {
    Shutdown();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->initialized = true;
    impl_->fallback_mode = true;
    device_info_ = VulkanDeviceInfo{};
    device_info_.device_name = "ScarlettX Software SPIR-V Compute Fallback";
    device_info_.vendor_id = 0x5358;
    device_info_.device_id = 0x0001;
    device_info_.api_version = kVkApiVersion1_2;
    device_info_.api_major = 1;
    device_info_.api_minor = 2;
    device_info_.api_patch = 0;
    device_info_.device_type = VulkanDeviceType::Cpu;
    device_info_.supports_compute = true;
    device_info_.supports_graphics = false;
    device_info_.is_hardware = false;
    device_info_.is_fallback = true;
    return Status::Ok();
}

Status VulkanBackend::Initialize(bool allow_software_fallback) {
    Shutdown();

    std::string failure_reason;

#if defined(_WIN32)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);

        auto try_init_vulkan = [&]() -> bool {
            impl_->vulkan_dll = LoadLibraryA("vulkan-1.dll");
            if (!impl_->vulkan_dll) {
                failure_reason = "vulkan-1.dll not found";
                return false;
            }

            impl_->vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                GetProcAddress(impl_->vulkan_dll, "vkGetInstanceProcAddr"));
            if (!impl_->vkGetInstanceProcAddr) {
                failure_reason = "vkGetInstanceProcAddr export not found in vulkan-1.dll";
                return false;
            }

            impl_->vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
                impl_->vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
            impl_->vkEnumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
                impl_->vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
            if (!impl_->vkCreateInstance) {
                failure_reason = "vkCreateInstance not available";
                return false;
            }

            uint32_t loader_version = kVkApiVersion1_0;
            if (impl_->vkEnumerateInstanceVersion) {
                uint32_t queried = 0;
                if (impl_->vkEnumerateInstanceVersion(&queried) == kVkSuccess && queried >= kVkApiVersion1_0) {
                    loader_version = queried;
                }
            }

            // Target Vulkan 1.2 (or 1.3 if supported; gracefully cap to loader version).
            uint32_t target_api = kVkApiVersion1_2;
            if (loader_version >= kVkApiVersion1_3) {
                target_api = kVkApiVersion1_3;
            } else if (loader_version < kVkApiVersion1_2) {
                target_api = loader_version;
            }

            VkApplicationInfo app_info{};
            app_info.sType = kVkStypeApplicationInfo;
            app_info.pApplicationName = "ScarlettX";
            app_info.applicationVersion = VkMakeApiVersion(0, 0, 1, 0);
            app_info.pEngineName = "ScarlettX-Vulkan";
            app_info.engineVersion = VkMakeApiVersion(0, 0, 1, 0);
            app_info.apiVersion = target_api;

            VkInstanceCreateInfo inst_info{};
            inst_info.sType = kVkStypeInstanceCreateInfo;
            inst_info.pApplicationInfo = &app_info;

            VkResult vr = impl_->vkCreateInstance(&inst_info, nullptr, &impl_->instance);
            if (vr != kVkSuccess || !impl_->instance) {
                // Retry with Vulkan 1.2 if 1.3 was rejected by a loader shim.
                app_info.apiVersion = kVkApiVersion1_2;
                vr = impl_->vkCreateInstance(&inst_info, nullptr, &impl_->instance);
            }
            if (vr != kVkSuccess || !impl_->instance) {
                failure_reason = "vkCreateInstance failed (VkResult=" + std::to_string(vr) + ")";
                return false;
            }

            if (!impl_->LoadInstProc(impl_->vkDestroyInstance, "vkDestroyInstance") ||
                !impl_->LoadInstProc(impl_->vkEnumeratePhysicalDevices, "vkEnumeratePhysicalDevices") ||
                !impl_->LoadInstProc(impl_->vkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties") ||
                !impl_->LoadInstProc(impl_->vkGetPhysicalDeviceQueueFamilyProperties,
                                     "vkGetPhysicalDeviceQueueFamilyProperties") ||
                !impl_->LoadInstProc(impl_->vkGetPhysicalDeviceMemoryProperties,
                                     "vkGetPhysicalDeviceMemoryProperties") ||
                !impl_->LoadInstProc(impl_->vkCreateDevice, "vkCreateDevice") ||
                !impl_->LoadInstProc(impl_->vkGetDeviceProcAddr, "vkGetDeviceProcAddr")) {
                failure_reason = "Failed to resolve core Vulkan instance procedures";
                return false;
            }

            uint32_t dev_count = 0;
            vr = impl_->vkEnumeratePhysicalDevices(impl_->instance, &dev_count, nullptr);
            if (vr != kVkSuccess || dev_count == 0) {
                failure_reason = "No Vulkan physical devices enumerated";
                return false;
            }

            std::vector<VkPhysicalDevice> phys_devs(dev_count, nullptr);
            vr = impl_->vkEnumeratePhysicalDevices(impl_->instance, &dev_count, phys_devs.data());
            if (vr != kVkSuccess || dev_count == 0) {
                failure_reason = "vkEnumeratePhysicalDevices failed";
                return false;
            }
            phys_devs.resize(dev_count);

            VkPhysicalDevice best_dev = nullptr;
            VkPhysicalDevicePropertiesRaw best_props{};
            uint32_t best_queue_family = std::numeric_limits<uint32_t>::max();
            bool best_has_graphics = false;
            int best_rank = -1;

            for (VkPhysicalDevice candidate : phys_devs) {
                if (!candidate) continue;
                VkPhysicalDevicePropertiesRaw props{};
                impl_->vkGetPhysicalDeviceProperties(candidate, &props);

                uint32_t qf_count = 0;
                impl_->vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qf_count, nullptr);
                if (qf_count == 0) continue;
                std::vector<VkQueueFamilyProperties> qfs(qf_count);
                impl_->vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qf_count, qfs.data());

                uint32_t compute_qf = std::numeric_limits<uint32_t>::max();
                bool has_graphics = false;
                for (uint32_t q = 0; q < qf_count; ++q) {
                    if (qfs[q].queueCount == 0) continue;
                    if ((qfs[q].queueFlags & kVkQueueGraphicsBit) != 0) {
                        has_graphics = true;
                    }
                    if ((qfs[q].queueFlags & kVkQueueComputeBit) != 0) {
                        // Prefer dedicated compute queue if available, otherwise first compute-capable queue.
                        if (compute_qf == std::numeric_limits<uint32_t>::max() ||
                            (qfs[q].queueFlags & kVkQueueGraphicsBit) == 0) {
                            compute_qf = q;
                        }
                    }
                }
                if (compute_qf == std::numeric_limits<uint32_t>::max()) continue;

                const int rank = DeviceTypeRank(props.deviceType);
                if (rank > best_rank) {
                    best_rank = rank;
                    best_dev = candidate;
                    best_props = props;
                    best_queue_family = compute_qf;
                    best_has_graphics = has_graphics;
                }
            }

            if (!best_dev) {
                failure_reason = "No Vulkan physical device with a compute-capable queue family";
                return false;
            }

            impl_->physical_device = best_dev;
            impl_->compute_queue_family = best_queue_family;
            impl_->vkGetPhysicalDeviceMemoryProperties(best_dev, &impl_->mem_props);

            const float queue_priority = 1.0f;
            VkDeviceQueueCreateInfo queue_ci{};
            queue_ci.sType = kVkStypeDeviceQueueCreateInfo;
            queue_ci.queueFamilyIndex = best_queue_family;
            queue_ci.queueCount = 1;
            queue_ci.pQueuePriorities = &queue_priority;

            VkDeviceCreateInfo dev_ci{};
            dev_ci.sType = kVkStypeDeviceCreateInfo;
            dev_ci.queueCreateInfoCount = 1;
            dev_ci.pQueueCreateInfos = &queue_ci;

            vr = impl_->vkCreateDevice(best_dev, &dev_ci, nullptr, &impl_->device);
            if (vr != kVkSuccess || !impl_->device) {
                failure_reason = "vkCreateDevice failed (VkResult=" + std::to_string(vr) + ")";
                return false;
            }

            const bool loaded_dev =
                impl_->LoadDevProc(impl_->vkDestroyDevice, "vkDestroyDevice") &&
                impl_->LoadDevProc(impl_->vkDeviceWaitIdle, "vkDeviceWaitIdle") &&
                impl_->LoadDevProc(impl_->vkGetDeviceQueue, "vkGetDeviceQueue") &&
                impl_->LoadDevProc(impl_->vkQueueSubmit, "vkQueueSubmit") &&
                impl_->LoadDevProc(impl_->vkQueueWaitIdle, "vkQueueWaitIdle") &&
                impl_->LoadDevProc(impl_->vkCreateCommandPool, "vkCreateCommandPool") &&
                impl_->LoadDevProc(impl_->vkDestroyCommandPool, "vkDestroyCommandPool") &&
                impl_->LoadDevProc(impl_->vkAllocateCommandBuffers, "vkAllocateCommandBuffers") &&
                impl_->LoadDevProc(impl_->vkFreeCommandBuffers, "vkFreeCommandBuffers") &&
                impl_->LoadDevProc(impl_->vkBeginCommandBuffer, "vkBeginCommandBuffer") &&
                impl_->LoadDevProc(impl_->vkEndCommandBuffer, "vkEndCommandBuffer") &&
                impl_->LoadDevProc(impl_->vkCmdBindPipeline, "vkCmdBindPipeline") &&
                impl_->LoadDevProc(impl_->vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets") &&
                impl_->LoadDevProc(impl_->vkCmdDispatch, "vkCmdDispatch") &&
                impl_->LoadDevProc(impl_->vkCmdPipelineBarrier, "vkCmdPipelineBarrier") &&
                impl_->LoadDevProc(impl_->vkCreateBuffer, "vkCreateBuffer") &&
                impl_->LoadDevProc(impl_->vkDestroyBuffer, "vkDestroyBuffer") &&
                impl_->LoadDevProc(impl_->vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements") &&
                impl_->LoadDevProc(impl_->vkAllocateMemory, "vkAllocateMemory") &&
                impl_->LoadDevProc(impl_->vkFreeMemory, "vkFreeMemory") &&
                impl_->LoadDevProc(impl_->vkBindBufferMemory, "vkBindBufferMemory") &&
                impl_->LoadDevProc(impl_->vkMapMemory, "vkMapMemory") &&
                impl_->LoadDevProc(impl_->vkUnmapMemory, "vkUnmapMemory") &&
                impl_->LoadDevProc(impl_->vkFlushMappedMemoryRanges, "vkFlushMappedMemoryRanges") &&
                impl_->LoadDevProc(impl_->vkInvalidateMappedMemoryRanges, "vkInvalidateMappedMemoryRanges") &&
                impl_->LoadDevProc(impl_->vkCreateShaderModule, "vkCreateShaderModule") &&
                impl_->LoadDevProc(impl_->vkDestroyShaderModule, "vkDestroyShaderModule") &&
                impl_->LoadDevProc(impl_->vkCreateDescriptorSetLayout, "vkCreateDescriptorSetLayout") &&
                impl_->LoadDevProc(impl_->vkDestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout") &&
                impl_->LoadDevProc(impl_->vkCreatePipelineLayout, "vkCreatePipelineLayout") &&
                impl_->LoadDevProc(impl_->vkDestroyPipelineLayout, "vkDestroyPipelineLayout") &&
                impl_->LoadDevProc(impl_->vkCreateComputePipelines, "vkCreateComputePipelines") &&
                impl_->LoadDevProc(impl_->vkDestroyPipeline, "vkDestroyPipeline") &&
                impl_->LoadDevProc(impl_->vkCreateDescriptorPool, "vkCreateDescriptorPool") &&
                impl_->LoadDevProc(impl_->vkDestroyDescriptorPool, "vkDestroyDescriptorPool") &&
                impl_->LoadDevProc(impl_->vkAllocateDescriptorSets, "vkAllocateDescriptorSets") &&
                impl_->LoadDevProc(impl_->vkUpdateDescriptorSets, "vkUpdateDescriptorSets");

            if (!loaded_dev) {
                failure_reason = "Failed to resolve core Vulkan device procedures";
                return false;
            }

            impl_->vkGetDeviceQueue(impl_->device, best_queue_family, 0, &impl_->compute_queue);
            if (!impl_->compute_queue) {
                failure_reason = "vkGetDeviceQueue returned null compute queue";
                return false;
            }

            VkCommandPoolCreateInfo pool_ci{};
            pool_ci.sType = kVkStypeCommandPoolCreateInfo;
            pool_ci.flags = kVkCommandPoolCreateResetCmdBufBit;
            pool_ci.queueFamilyIndex = best_queue_family;

            vr = impl_->vkCreateCommandPool(impl_->device, &pool_ci, nullptr, &impl_->command_pool);
            if (vr != kVkSuccess || impl_->command_pool == kVkNullHandle) {
                failure_reason = "vkCreateCommandPool failed (VkResult=" + std::to_string(vr) + ")";
                return false;
            }

            best_props.deviceName[sizeof(best_props.deviceName) - 1] = '\0';
            device_info_ = VulkanDeviceInfo{};
            device_info_.device_name = best_props.deviceName;
            device_info_.vendor_id = best_props.vendorID;
            device_info_.device_id = best_props.deviceID;
            device_info_.api_version = best_props.apiVersion;
            device_info_.api_major = VkApiMajor(best_props.apiVersion);
            device_info_.api_minor = VkApiMinor(best_props.apiVersion);
            device_info_.api_patch = VkApiPatch(best_props.apiVersion);
            device_info_.driver_version = best_props.driverVersion;
            device_info_.device_type = static_cast<VulkanDeviceType>(best_props.deviceType);
            device_info_.compute_queue_family = best_queue_family;
            device_info_.supports_compute = true;
            device_info_.supports_graphics = best_has_graphics;
            device_info_.is_hardware =
                (device_info_.device_type == VulkanDeviceType::DiscreteGpu ||
                 device_info_.device_type == VulkanDeviceType::IntegratedGpu);
            device_info_.is_fallback = false;

            impl_->initialized = true;
            impl_->fallback_mode = false;

            std::cerr << "[ScarlettX][vulkan] ICD Device: \"" << device_info_.device_name
                      << "\" API=" << device_info_.api_major << "." << device_info_.api_minor
                      << "." << device_info_.api_patch
                      << " vendor=0x" << std::hex << device_info_.vendor_id
                      << " device=0x" << device_info_.device_id << std::dec
                      << " compute_qf=" << device_info_.compute_queue_family << "\n";
            return true;
        };

        if (try_init_vulkan()) {
            return Status::Ok();
        }
        impl_->Cleanup();
        device_info_ = VulkanDeviceInfo{};
    }
#else
    failure_reason = "Dynamic vulkan-1.dll loader requires Windows";
#endif

    if (!allow_software_fallback) {
        return Status::Error(StatusCode::DeviceError, failure_reason);
    }
    std::cerr << "[ScarlettX][vulkan] Falling back to software SPIR-V compute (" << failure_reason << ")\n";
    return InitializeSoftwareFallback();
}

void VulkanBackend::Shutdown() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->Cleanup();
    device_info_ = VulkanDeviceInfo{};
}

Status VulkanBackend::ExecuteComputeShader(const std::vector<uint32_t>& spirv,
                                           const std::vector<uint32_t>& input_u32,
                                           std::vector<uint32_t>& output_u32,
                                           uint32_t group_x) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    if (group_x == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Compute dispatch group_x must be > 0");
    }
    if (input_u32.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Compute input buffer must not be empty");
    }

    SpirvValidationInfo spv_info{};
    Status val_st = ValidateSpirvBinary(spirv, &spv_info);
    if (!val_st.ok()) {
        return val_st;
    }
    if (!spv_info.has_compute_entry || spv_info.entry_point_name.empty()) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V module has no GLCompute entry point");
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);

    const uint32_t element_count =
        std::max<uint32_t>(static_cast<uint32_t>(input_u32.size()), group_x * spv_info.local_size_x);
    output_u32.assign(element_count, 0u);

    if (impl_->fallback_mode || !impl_->device) {
        return ExecuteSpirvComputeSoftware(spirv, spv_info, input_u32, output_u32, group_x);
    }

    const VkDeviceSize byte_size = static_cast<VkDeviceSize>(element_count) * sizeof(uint32_t);

    VkBuffer in_buf = kVkNullHandle;
    VkBuffer out_buf = kVkNullHandle;
    VkDeviceMemory in_mem = kVkNullHandle;
    VkDeviceMemory out_mem = kVkNullHandle;
    VkShaderModule shader_mod = kVkNullHandle;
    VkDescriptorSetLayout ds_layout = kVkNullHandle;
    VkPipelineLayout pipe_layout = kVkNullHandle;
    VkPipeline pipeline = kVkNullHandle;
    VkDescriptorPool desc_pool = kVkNullHandle;
    VkCommandBuffer cmd_buf = nullptr;

    auto cleanup = [&]() {
        if (cmd_buf) {
            impl_->vkFreeCommandBuffers(impl_->device, impl_->command_pool, 1, &cmd_buf);
            cmd_buf = nullptr;
        }
        if (desc_pool != kVkNullHandle) {
            impl_->vkDestroyDescriptorPool(impl_->device, desc_pool, nullptr);
            desc_pool = kVkNullHandle;
        }
        if (pipeline != kVkNullHandle) {
            impl_->vkDestroyPipeline(impl_->device, pipeline, nullptr);
            pipeline = kVkNullHandle;
        }
        if (pipe_layout != kVkNullHandle) {
            impl_->vkDestroyPipelineLayout(impl_->device, pipe_layout, nullptr);
            pipe_layout = kVkNullHandle;
        }
        if (ds_layout != kVkNullHandle) {
            impl_->vkDestroyDescriptorSetLayout(impl_->device, ds_layout, nullptr);
            ds_layout = kVkNullHandle;
        }
        if (shader_mod != kVkNullHandle) {
            impl_->vkDestroyShaderModule(impl_->device, shader_mod, nullptr);
            shader_mod = kVkNullHandle;
        }
        if (out_buf != kVkNullHandle) {
            impl_->vkDestroyBuffer(impl_->device, out_buf, nullptr);
            out_buf = kVkNullHandle;
        }
        if (in_buf != kVkNullHandle) {
            impl_->vkDestroyBuffer(impl_->device, in_buf, nullptr);
            in_buf = kVkNullHandle;
        }
        if (out_mem != kVkNullHandle) {
            impl_->vkFreeMemory(impl_->device, out_mem, nullptr);
            out_mem = kVkNullHandle;
        }
        if (in_mem != kVkNullHandle) {
            impl_->vkFreeMemory(impl_->device, in_mem, nullptr);
            in_mem = kVkNullHandle;
        }
    };

    auto create_storage_buf = [&](VkBuffer& buf, VkDeviceMemory& mem, bool& coherent) -> Status {
        VkBufferCreateInfo bci{};
        bci.sType = kVkStypeBufferCreateInfo;
        bci.size = byte_size;
        bci.usage = kVkBufferUsageStorageBufferBit;
        bci.sharingMode = kVkSharingModeExclusive;

        if (impl_->vkCreateBuffer(impl_->device, &bci, nullptr, &buf) != kVkSuccess) {
            return Status::Error(StatusCode::DeviceError, "vkCreateBuffer failed");
        }
        VkMemoryRequirements req{};
        impl_->vkGetBufferMemoryRequirements(impl_->device, buf, &req);
        const uint32_t mem_idx = impl_->FindMemoryType(
            req.memoryTypeBits, kVkMemoryPropertyHostVisibleBit | kVkMemoryPropertyHostCoherentBit, coherent);
        if (mem_idx == std::numeric_limits<uint32_t>::max()) {
            return Status::Error(StatusCode::OutOfMemory, "No HOST_VISIBLE Vulkan memory type available");
        }

        VkMemoryAllocateInfo mai{};
        mai.sType = kVkStypeMemoryAllocateInfo;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = mem_idx;
        if (impl_->vkAllocateMemory(impl_->device, &mai, nullptr, &mem) != kVkSuccess) {
            return Status::Error(StatusCode::OutOfMemory, "vkAllocateMemory failed");
        }
        if (impl_->vkBindBufferMemory(impl_->device, buf, mem, 0) != kVkSuccess) {
            return Status::Error(StatusCode::DeviceError, "vkBindBufferMemory failed");
        }
        return Status::Ok();
    };

    bool in_coherent = true;
    bool out_coherent = true;
    Status st = create_storage_buf(in_buf, in_mem, in_coherent);
    if (!st.ok()) { cleanup(); return st; }
    st = create_storage_buf(out_buf, out_mem, out_coherent);
    if (!st.ok()) { cleanup(); return st; }

    // Upload input buffer and zero-initialize output buffer.
    {
        void* mapped = nullptr;
        if (impl_->vkMapMemory(impl_->device, in_mem, 0, byte_size, 0, &mapped) != kVkSuccess || !mapped) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "vkMapMemory(input) failed");
        }
        std::memset(mapped, 0, static_cast<size_t>(byte_size));
        std::memcpy(mapped, input_u32.data(), input_u32.size() * sizeof(uint32_t));
        if (!in_coherent) {
            VkMappedMemoryRange rng{kVkStypeMappedMemoryRange, nullptr, in_mem, 0, kVkWholeSize};
            impl_->vkFlushMappedMemoryRanges(impl_->device, 1, &rng);
        }
        impl_->vkUnmapMemory(impl_->device, in_mem);
    }
    {
        void* mapped = nullptr;
        if (impl_->vkMapMemory(impl_->device, out_mem, 0, byte_size, 0, &mapped) != kVkSuccess || !mapped) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "vkMapMemory(output init) failed");
        }
        std::memset(mapped, 0, static_cast<size_t>(byte_size));
        if (!out_coherent) {
            VkMappedMemoryRange rng{kVkStypeMappedMemoryRange, nullptr, out_mem, 0, kVkWholeSize};
            impl_->vkFlushMappedMemoryRanges(impl_->device, 1, &rng);
        }
        impl_->vkUnmapMemory(impl_->device, out_mem);
    }

    VkShaderModuleCreateInfo sm_ci{};
    sm_ci.sType = kVkStypeShaderModuleCreateInfo;
    sm_ci.codeSize = spirv.size() * sizeof(uint32_t);
    sm_ci.pCode = spirv.data();
    if (impl_->vkCreateShaderModule(impl_->device, &sm_ci, nullptr, &shader_mod) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::ShaderError, "vkCreateShaderModule failed");
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = kVkDescriptorTypeStorageBuffer;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = kVkShaderStageComputeBit;
    bindings[1].binding = 1;
    bindings[1].descriptorType = kVkDescriptorTypeStorageBuffer;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = kVkShaderStageComputeBit;

    VkDescriptorSetLayoutCreateInfo dsl_ci{};
    dsl_ci.sType = kVkStypeDescriptorSetLayoutCreateInfo;
    dsl_ci.bindingCount = 2;
    dsl_ci.pBindings = bindings;
    if (impl_->vkCreateDescriptorSetLayout(impl_->device, &dsl_ci, nullptr, &ds_layout) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkCreateDescriptorSetLayout failed");
    }

    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType = kVkStypePipelineLayoutCreateInfo;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &ds_layout;
    if (impl_->vkCreatePipelineLayout(impl_->device, &pl_ci, nullptr, &pipe_layout) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkCreatePipelineLayout failed");
    }

    VkComputePipelineCreateInfo cp_ci{};
    cp_ci.sType = kVkStypeComputePipelineCreateInfo;
    cp_ci.stage.sType = kVkStypePipelineShaderStageCreateInfo;
    cp_ci.stage.stage = kVkShaderStageComputeBit;
    cp_ci.stage.module = shader_mod;
    cp_ci.stage.pName = spv_info.entry_point_name.c_str();
    cp_ci.layout = pipe_layout;
    cp_ci.basePipelineIndex = -1;
    if (impl_->vkCreateComputePipelines(impl_->device, kVkNullHandle, 1, &cp_ci, nullptr, &pipeline) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::ShaderError, "vkCreateComputePipelines failed");
    }

    VkDescriptorPoolSize pool_size{kVkDescriptorTypeStorageBuffer, 2};
    VkDescriptorPoolCreateInfo dp_ci{};
    dp_ci.sType = kVkStypeDescriptorPoolCreateInfo;
    dp_ci.maxSets = 1;
    dp_ci.poolSizeCount = 1;
    dp_ci.pPoolSizes = &pool_size;
    if (impl_->vkCreateDescriptorPool(impl_->device, &dp_ci, nullptr, &desc_pool) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkCreateDescriptorPool failed");
    }

    VkDescriptorSet desc_set = kVkNullHandle;
    VkDescriptorSetAllocateInfo dsa_ci{};
    dsa_ci.sType = kVkStypeDescriptorSetAllocateInfo;
    dsa_ci.descriptorPool = desc_pool;
    dsa_ci.descriptorSetCount = 1;
    dsa_ci.pSetLayouts = &ds_layout;
    if (impl_->vkAllocateDescriptorSets(impl_->device, &dsa_ci, &desc_set) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkAllocateDescriptorSets failed");
    }

    VkDescriptorBufferInfo in_binfo{in_buf, 0, byte_size};
    VkDescriptorBufferInfo out_binfo{out_buf, 0, byte_size};
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = kVkStypeWriteDescriptorSet;
    writes[0].dstSet = desc_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = kVkDescriptorTypeStorageBuffer;
    writes[0].pBufferInfo = &in_binfo;

    writes[1].sType = kVkStypeWriteDescriptorSet;
    writes[1].dstSet = desc_set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = kVkDescriptorTypeStorageBuffer;
    writes[1].pBufferInfo = &out_binfo;
    impl_->vkUpdateDescriptorSets(impl_->device, 2, writes, 0, nullptr);

    VkCommandBufferAllocateInfo cba_ci{};
    cba_ci.sType = kVkStypeCommandBufferAllocateInfo;
    cba_ci.commandPool = impl_->command_pool;
    cba_ci.level = kVkCommandBufferLevelPrimary;
    cba_ci.commandBufferCount = 1;
    if (impl_->vkAllocateCommandBuffers(impl_->device, &cba_ci, &cmd_buf) != kVkSuccess || !cmd_buf) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkAllocateCommandBuffers failed");
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = kVkStypeCommandBufferBeginInfo;
    begin_info.flags = kVkCommandBufferUsageOneTimeSubmitBit;
    if (impl_->vkBeginCommandBuffer(cmd_buf, &begin_info) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkBeginCommandBuffer failed");
    }

    impl_->vkCmdBindPipeline(cmd_buf, kVkPipelineBindPointCompute, pipeline);
    impl_->vkCmdBindDescriptorSets(cmd_buf, kVkPipelineBindPointCompute, pipe_layout, 0, 1, &desc_set, 0, nullptr);
    impl_->vkCmdDispatch(cmd_buf, group_x, 1, 1);

    VkBufferMemoryBarrier out_barrier{};
    out_barrier.sType = kVkStypeBufferMemoryBarrier;
    out_barrier.srcAccessMask = kVkAccessShaderWriteBit;
    out_barrier.dstAccessMask = kVkAccessHostReadBit;
    out_barrier.srcQueueFamilyIndex = kVkQueueFamilyIgnored;
    out_barrier.dstQueueFamilyIndex = kVkQueueFamilyIgnored;
    out_barrier.buffer = out_buf;
    out_barrier.offset = 0;
    out_barrier.size = byte_size;
    impl_->vkCmdPipelineBarrier(cmd_buf,
                                kVkPipelineStageComputeShaderBit,
                                kVkPipelineStageHostBit,
                                0, 0, nullptr, 1, &out_barrier, 0, nullptr);

    if (impl_->vkEndCommandBuffer(cmd_buf) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkEndCommandBuffer failed");
    }

    VkSubmitInfo submit_info{};
    submit_info.sType = kVkStypeSubmitInfo;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buf;
    if (impl_->vkQueueSubmit(impl_->compute_queue, 1, &submit_info, kVkNullHandle) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkQueueSubmit failed");
    }
    if (impl_->vkQueueWaitIdle(impl_->compute_queue) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkQueueWaitIdle failed");
    }

    // Read back GPU results.
    {
        void* mapped = nullptr;
        if (impl_->vkMapMemory(impl_->device, out_mem, 0, byte_size, 0, &mapped) != kVkSuccess || !mapped) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "vkMapMemory(output readback) failed");
        }
        if (!out_coherent) {
            VkMappedMemoryRange rng{kVkStypeMappedMemoryRange, nullptr, out_mem, 0, kVkWholeSize};
            impl_->vkInvalidateMappedMemoryRanges(impl_->device, 1, &rng);
        }
        std::memcpy(output_u32.data(), mapped, static_cast<size_t>(byte_size));
        impl_->vkUnmapMemory(impl_->device, out_mem);
    }

    cleanup();
    return Status::Ok();
}

Status VulkanBackend::RecordCommandList(const VulkanCommandListDesc& desc,
                                        VulkanCommandListResult& out_result) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }

    out_result = VulkanCommandListResult{};

    // Validate commands before recording.
    for (const auto& cmd : desc.commands) {
        if (cmd.op == VulkanCommandOp::Dispatch && (cmd.x == 0 || cmd.y == 0 || cmd.z == 0)) {
            return Status::Error(StatusCode::InvalidArgument, "Dispatch command dimensions must be > 0");
        }
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (impl_->fallback_mode || !impl_->device) {
        for (const auto& cmd : desc.commands) {
            ++out_result.recorded_commands;
            if (cmd.op == VulkanCommandOp::Dispatch) ++out_result.dispatch_count;
            if (cmd.op == VulkanCommandOp::PipelineBarrier) ++out_result.barrier_count;
        }
        out_result.executed_on_vulkan_icd = false;
        return Status::Ok();
    }

    VkCommandBuffer cmd_buf = nullptr;
    VkCommandBufferAllocateInfo cba_ci{};
    cba_ci.sType = kVkStypeCommandBufferAllocateInfo;
    cba_ci.commandPool = impl_->command_pool;
    cba_ci.level = kVkCommandBufferLevelPrimary;
    cba_ci.commandBufferCount = 1;
    if (impl_->vkAllocateCommandBuffers(impl_->device, &cba_ci, &cmd_buf) != kVkSuccess || !cmd_buf) {
        return Status::Error(StatusCode::DeviceError, "RecordCommandList: vkAllocateCommandBuffers failed");
    }

    auto free_cb = [&]() {
        if (cmd_buf) {
            impl_->vkFreeCommandBuffers(impl_->device, impl_->command_pool, 1, &cmd_buf);
            cmd_buf = nullptr;
        }
    };

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = kVkStypeCommandBufferBeginInfo;
    begin_info.flags = kVkCommandBufferUsageOneTimeSubmitBit;
    if (impl_->vkBeginCommandBuffer(cmd_buf, &begin_info) != kVkSuccess) {
        free_cb();
        return Status::Error(StatusCode::DeviceError, "RecordCommandList: vkBeginCommandBuffer failed");
    }

    for (const auto& cmd : desc.commands) {
        ++out_result.recorded_commands;
        if (cmd.op == VulkanCommandOp::PipelineBarrier) {
            VkMemoryBarrier mb{};
            mb.sType = kVkStypeMemoryBarrier;
            mb.srcAccessMask = kVkAccessShaderWriteBit;
            mb.dstAccessMask = kVkAccessHostReadBit;
            impl_->vkCmdPipelineBarrier(cmd_buf,
                                        kVkPipelineStageComputeShaderBit,
                                        kVkPipelineStageHostBit,
                                        0, 1, &mb, 0, nullptr, 0, nullptr);
            ++out_result.barrier_count;
        } else if (cmd.op == VulkanCommandOp::Dispatch) {
            // Note: Dispatch without a bound pipeline is only tracked unless executed via ExecuteComputeShader.
            ++out_result.dispatch_count;
        }
    }

    if (impl_->vkEndCommandBuffer(cmd_buf) != kVkSuccess) {
        free_cb();
        return Status::Error(StatusCode::DeviceError, "RecordCommandList: vkEndCommandBuffer failed");
    }

    if (desc.submit_empty_fence_sync) {
        VkSubmitInfo si{};
        si.sType = kVkStypeSubmitInfo;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd_buf;
        if (impl_->vkQueueSubmit(impl_->compute_queue, 1, &si, kVkNullHandle) != kVkSuccess ||
            impl_->vkQueueWaitIdle(impl_->compute_queue) != kVkSuccess) {
            free_cb();
            return Status::Error(StatusCode::DeviceError, "RecordCommandList: queue submit/wait failed");
        }
    }

    free_cb();
    out_result.executed_on_vulkan_icd = true;
    return Status::Ok();
}

}  // namespace scarlettx
