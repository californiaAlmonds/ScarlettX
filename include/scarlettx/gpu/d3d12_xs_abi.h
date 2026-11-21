#pragma once

#include <cstdint>
#include <cstddef>

#include "scarlettx/gdk/xgameruntime_abi.h"

#if defined(_WIN32)
#  if defined(SCARLETTX_BUILDING_D3D12_XS)
#    define SCARLETTX_D3D12_API __declspec(dllexport)
#  else
#    define SCARLETTX_D3D12_API __declspec(dllimport)
#  endif
#  if defined(SCARLETTX_BUILDING_XBOX_BVH)
#    define SCARLETTX_BVH_API __declspec(dllexport)
#  else
#    define SCARLETTX_BVH_API __declspec(dllimport)
#  endif
#else
#  define SCARLETTX_D3D12_API
#  define SCARLETTX_BVH_API
#endif

extern "C" {

// ---------------------------------------------------------------------------
// Scarlett D3D12_XS ABI enums and structs
// ---------------------------------------------------------------------------
enum D3D12XBOX_PROCESS_DEBUG_FLAGS : uint32_t {
    D3D12XBOX_PROCESS_DEBUG_FLAG_NONE = 0x0,
    D3D12XBOX_PROCESS_DEBUG_FLAG_DEBUG = 0x1,
    D3D12XBOX_PROCESS_DEBUG_FLAG_INSTRUMENTED = 0x2,
    D3D12XBOX_PROCESS_DEBUG_FLAG_ENABLE_COMMON_STATE_PROMOTION = 0x4,
};

enum D3D12XBOX_FRAME_INTERVAL : uint32_t {
    D3D12XBOX_FRAME_INTERVAL_NONE = 0,
    D3D12XBOX_FRAME_INTERVAL_60_HZ = 1,
    D3D12XBOX_FRAME_INTERVAL_30_HZ = 2,
    D3D12XBOX_FRAME_INTERVAL_24_HZ = 3,
    D3D12XBOX_FRAME_INTERVAL_120_HZ = 4,
};

enum D3D12XBOX_FRAME_EVENT : uint32_t {
    D3D12XBOX_FRAME_EVENT_ORIGIN = 0,
    D3D12XBOX_FRAME_EVENT_VSYNC = 1,
};

enum D3D12XBOX_PRESENT_FLAGS : uint32_t {
    D3D12XBOX_PRESENT_FLAG_NONE = 0x0,
    D3D12XBOX_PRESENT_FLAG_IMMEDIATE = 0x1,
    D3D12XBOX_PRESENT_FLAG_ALLOW_TEARING = 0x2,
};

enum D3D12XBOX_RT_BACKEND : uint32_t {
    D3D12XBOX_RT_BACKEND_NONE = 0,
    D3D12XBOX_RT_BACKEND_HARDWARE_DXR = 1,
    D3D12XBOX_RT_BACKEND_SOFTWARE_BVH = 2,
};

enum D3D12XBOX_AS_TYPE : uint32_t {
    D3D12XBOX_AS_TYPE_TOP_LEVEL = 0,
    D3D12XBOX_AS_TYPE_BOTTOM_LEVEL = 1,
};

enum D3D12XBOX_AS_BUILD_FLAGS : uint32_t {
    D3D12XBOX_AS_BUILD_FLAG_NONE = 0x0,
    D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE = 0x1,
    D3D12XBOX_AS_BUILD_FLAG_ALLOW_COMPACTION = 0x2,
    D3D12XBOX_AS_BUILD_FLAG_PREFER_FAST_TRACE = 0x4,
    D3D12XBOX_AS_BUILD_FLAG_PREFER_FAST_BUILD = 0x8,
    D3D12XBOX_AS_BUILD_FLAG_PERFORM_UPDATE = 0x20,
};

typedef uint64_t D3D12XBOX_FRAME_PIPELINE_TOKEN;
constexpr D3D12XBOX_FRAME_PIPELINE_TOKEN D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL = 0;

struct D3D12XBOX_CREATE_DEVICE_PARAMETERS {
    uint32_t Version;
    uint32_t ProcessDebugFlags;
    uint32_t GraphicsCommandQueueRingSizeBytes;
    uint32_t GraphicsScratchMemorySizeBytes;
    uint32_t ComputeScratchMemorySizeBytes;
    uint32_t PreferWarp;
};

struct D3D12XBOX_PRESENT_PLANE_PARAMETERS {
    uint32_t Width;
    uint32_t Height;
    uint64_t Token;
};

struct D3D12XBOX_PRESENT_PARAMETERS {
    D3D12XBOX_FRAME_PIPELINE_TOKEN Token;
    uint32_t PlaneCount;
    uint32_t Flags;
    const D3D12XBOX_PRESENT_PLANE_PARAMETERS* Planes;
    void* CommandQueue;  // Optional ID3D12CommandQueue*
};

struct D3D12XBOX_FRAME_STATISTICS {
    uint64_t PresentedFrameCount;
    D3D12XBOX_FRAME_PIPELINE_TOKEN LastPresentedToken;
    D3D12XBOX_FRAME_PIPELINE_TOKEN LastIssuedToken;
    uint32_t IntervalHz;
    uint32_t Suspended;
    uint64_t LastFrameDurationUs;
};

struct D3D12XBOX_CAPABILITIES {
    uint32_t RaytracingTier;  // 0 = none, 10 = 1.0, 11 = 1.1
    uint32_t IsWarpAdapter;
    uint32_t MaxRenderWidth;
    uint32_t MaxRenderHeight;
    char AdapterName[128];
};

struct D3D12XBOX_RAYTRACING_CAPABILITIES {
    uint32_t HardwareRaytracingTier;  // 0 = none, 10 = 1.0, 11 = 1.1
    uint32_t ActiveBackend;           // D3D12XBOX_RT_BACKEND
    uint32_t SupportsInlineRaytracing;
    uint32_t MaxRecursionDepth;
    char AdapterName[128];
};

struct D3D12XBOX_RT_AS_PREBUILD_INFO {
    uint64_t ResultDataMaxSizeInBytes;
    uint64_t ScratchDataSizeInBytes;
    uint64_t UpdateScratchDataSizeInBytes;
};

struct D3D12XBOX_RT_AS_INPUTS {
    uint32_t Type;        // D3D12XBOX_AS_TYPE
    uint32_t Flags;       // D3D12XBOX_AS_BUILD_FLAGS
    uint32_t NumDescs;    // Triangle count (BLAS) or instance/BLAS count (TLAS)
    uint32_t Reserved;
};

struct SCARLETT_BVH_TRIANGLE {
    float v0[3];
    float v1[3];
    float v2[3];
    float color[3];
    float reflectivity;
};

struct SCARLETT_BVH_BUILD_DESC {
    const SCARLETT_BVH_TRIANGLE* Triangles;
    uint32_t TriangleCount;
    uint32_t BuildFlags;
};

struct SCARLETT_RT_DISPATCH_DESC {
    uint32_t Width;
    uint32_t Height;
    float CameraPos[3];
    float CameraLookAt[3];
    float LightPos[3];
    uint32_t MaxBounces;
};

struct D3D12XBOX_RT_AS_BUILD_DESC {
    D3D12XBOX_RT_AS_INPUTS Inputs;
    const SCARLETT_BVH_TRIANGLE* Triangles;  // Used when Type == BOTTOM_LEVEL
    const uint64_t* BlasHandles;             // Used when Type == TOP_LEVEL
};

struct SCARLETT_BVH_STATS {
    uint32_t NodeCount;
    uint32_t TriangleCount;
    float BoundsMin[3];
    float BoundsMax[3];
};

struct SCARLETT_RAY_DESC {
    float Origin[3];
    float TMin;
    float Direction[3];
    float TMax;
};

struct SCARLETT_RAY_HIT {
    uint32_t Hit;
    uint32_t TriangleIndex;
    float T;
    float U;
    float V;
    float Normal[3];
};

// ---------------------------------------------------------------------------
// d3d12_xs.dll — Verified Xbox Series X|S entry points (from EmbeddedXvd)
// ---------------------------------------------------------------------------
SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12XboxCreateDevice(
    void* pAdapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParameters,
    const void* riid,
    void** ppvDevice);
SCARLETTX_D3D12_API void SCARLETTX_GDK_CALLBACK D3D12XboxSetProcessDebugFlags(
    uint32_t flags);
SCARLETTX_D3D12_API uint32_t SCARLETTX_GDK_CALLBACK D3D12XboxGetProcessDebugFlags(void);
SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12XsCreateDevice(
    void* pAdapter,
    uint32_t minimumFeatureLevel,
    const void* riid,
    void** ppDevice);
SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12XsGetDebugInterface(
    const void* riid,
    void** ppvDebug);
SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12XsSerializeRootSignature(
    const void* pRootSignature,
    uint32_t version,
    void** ppBlob,
    void** ppErrorBlob);
SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12XsSerializeVersionedRootSignature(
    const void* pRootSignature,
    void** ppBlob,
    void** ppErrorBlob);
SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12XsCreateRootSignatureDeserializer(
    const void* pSrcData,
    size_t srcDataSizeInBytes,
    const void* pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer);
SCARLETTX_D3D12_API int32_t SCARLETTX_GDK_CALLBACK D3D12XsEnableExperimentalFeatures(
    uint32_t numFeatures,
    const void* pIIDs,
    void* pConfigurationStructs,
    uint32_t* pConfigurationStructSizes);

// ---------------------------------------------------------------------------
// ScarlettX synthetic handle-based test harness APIs (NOT retail Xbox ABI)
// ---------------------------------------------------------------------------
SCARLETTX_D3D12_API int32_t D3D12XboxCreateSyntheticDevice(
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* params,
    uint64_t* out_device_handle);
SCARLETTX_D3D12_API int32_t D3D12XboxDestroyDevice(uint64_t device_handle);
SCARLETTX_D3D12_API int32_t D3D12XboxGetNativeDevice(
    uint64_t device_handle,
    void** out_id3d12_device);
SCARLETTX_D3D12_API int32_t D3D12XboxQueryCapabilities(
    uint64_t device_handle,
    D3D12XBOX_CAPABILITIES* out_caps);
SCARLETTX_D3D12_API int32_t D3D12XboxQueryRaytracingCapabilities(
    uint64_t device_handle,
    D3D12XBOX_RAYTRACING_CAPABILITIES* out_rt_caps);
SCARLETTX_D3D12_API int32_t D3D12XboxSetFrameIntervalX(
    uint64_t device_handle,
    uint32_t interval,
    uint32_t flags);
SCARLETTX_D3D12_API int32_t D3D12XboxScheduleFrameEventX(
    uint64_t device_handle,
    uint32_t frame_event,
    uint64_t offset_us,
    void* event_handle);
SCARLETTX_D3D12_API int32_t D3D12XboxWaitFrameEventX(
    uint64_t device_handle,
    uint64_t* out_frame_token);
SCARLETTX_D3D12_API int32_t D3D12XboxWaitFrameEventXEx(
    uint64_t device_handle,
    uint32_t frame_event,
    uint32_t timeout_ms,
    D3D12XBOX_FRAME_PIPELINE_TOKEN* out_frame_token);
SCARLETTX_D3D12_API int32_t D3D12XboxRenderTestTriangle(
    uint64_t device_handle,
    uint32_t width,
    uint32_t height,
    const char* optional_ppm_path,
    uint64_t* out_pixel_hash);
SCARLETTX_D3D12_API int32_t D3D12XboxPresentX(
    uint64_t device_handle,
    const D3D12XBOX_PRESENT_PLANE_PARAMETERS* plane);
SCARLETTX_D3D12_API int32_t D3D12XboxPresentXParams(
    uint64_t device_handle,
    const D3D12XBOX_PRESENT_PARAMETERS* params);
SCARLETTX_D3D12_API int32_t D3D12XboxGetFrameStatisticsX(
    uint64_t device_handle,
    D3D12XBOX_FRAME_STATISTICS* out_stats);
SCARLETTX_D3D12_API int32_t D3D12XboxSuspendX(
    uint64_t device_handle,
    uint32_t flags);
SCARLETTX_D3D12_API int32_t D3D12XboxResumeX(
    uint64_t device_handle);
SCARLETTX_D3D12_API int32_t D3D12XboxGetResourceAllocationInfo(
    uint64_t device_handle,
    uint64_t width,
    uint32_t height,
    uint32_t dxgi_format,
    uint64_t* out_size_bytes,
    uint64_t* out_alignment);
SCARLETTX_D3D12_API int32_t D3D12XboxGetRaytracingAccelerationStructurePrebuildInfo(
    uint64_t device_handle,
    const D3D12XBOX_RT_AS_INPUTS* inputs,
    D3D12XBOX_RT_AS_PREBUILD_INFO* out_info);
SCARLETTX_D3D12_API int32_t D3D12XboxBuildRaytracingAccelerationStructure(
    uint64_t device_handle,
    const D3D12XBOX_RT_AS_BUILD_DESC* desc,
    uint64_t* out_as_handle);
SCARLETTX_D3D12_API int32_t D3D12XboxDestroyRaytracingAccelerationStructure(
    uint64_t device_handle,
    uint64_t as_handle);

// ---------------------------------------------------------------------------
// Milestone 6: Shader Inspection, Root Signatures, Descriptor Heaps & PSOs
// ---------------------------------------------------------------------------
typedef struct D3D12XBOX_SHADER_BYTECODE {
    const void* Bytecode;
    uint64_t BytecodeLength;
} D3D12XBOX_SHADER_BYTECODE;

typedef struct D3D12XBOX_SHADER_INSPECTION {
    uint32_t ContainerKind;       // 0=Unknown, 1=DXBC, 2=DXIL, 3=ScarlettRDNA2
    uint32_t Stage;               // 0=Unknown, 1=VS, 2=PS, 3=CS, 4=GS, 5=HS, 6=DS, 7=Lib
    uint32_t ShaderModelMajor;
    uint32_t ShaderModelMinor;
    uint32_t ChunkCount;
    uint32_t InputParamCount;
    uint32_t OutputParamCount;
    uint32_t HasRaytracingExports;
    uint32_t HasEmbeddedRootSignature;
    uint32_t HasXboxPrivateChunks;
    uint64_t ContentHash;
} D3D12XBOX_SHADER_INSPECTION;

typedef struct D3D12XBOX_ROOT_SIGNATURE_DESC {
    uint32_t NumRootConstants32Bit;  // b0 root constants (0..16)
    uint32_t NumCbvs;                // root CBVs starting at b1
    uint32_t NumSrvs;                // root SRVs starting at t0
    uint32_t NumUavs;                // root UAVs starting at u0
    uint32_t Flags;                  // D3D12_ROOT_SIGNATURE_FLAGS (0 = default allow IA)
} D3D12XBOX_ROOT_SIGNATURE_DESC;

typedef enum D3D12XBOX_DESCRIPTOR_HEAP_TYPE {
    D3D12XBOX_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV = 0,
    D3D12XBOX_DESCRIPTOR_HEAP_TYPE_SAMPLER = 1,
    D3D12XBOX_DESCRIPTOR_HEAP_TYPE_RTV = 2,
    D3D12XBOX_DESCRIPTOR_HEAP_TYPE_DSV = 3
} D3D12XBOX_DESCRIPTOR_HEAP_TYPE;

typedef struct D3D12XBOX_DESCRIPTOR_HEAP_DESC {
    uint32_t Type;            // D3D12XBOX_DESCRIPTOR_HEAP_TYPE
    uint32_t NumDescriptors;
    uint32_t ShaderVisible;   // 1 for shader-visible CBV_SRV_UAV / SAMPLER
} D3D12XBOX_DESCRIPTOR_HEAP_DESC;

typedef struct D3D12XBOX_GRAPHICS_PSO_DESC {
    uint64_t RootSignatureHandle;
    D3D12XBOX_SHADER_BYTECODE VS;
    D3D12XBOX_SHADER_BYTECODE PS;
    uint32_t RtvFormat;          // DXGI_FORMAT (0 => DXGI_FORMAT_R8G8B8A8_UNORM)
    uint32_t PrimitiveTopology;  // 0/3 => Triangle
    uint32_t SampleCount;        // 0/1 => 1
    uint32_t Flags;
} D3D12XBOX_GRAPHICS_PSO_DESC;

typedef struct D3D12XBOX_COMPUTE_PSO_DESC {
    uint64_t RootSignatureHandle;
    D3D12XBOX_SHADER_BYTECODE CS;
    uint32_t Flags;
} D3D12XBOX_COMPUTE_PSO_DESC;

typedef struct D3D12XBOX_PIPELINE_CACHE_STATS {
    uint64_t CacheHits;
    uint64_t CacheMisses;
    uint32_t LiveRootSignatures;
    uint32_t LiveDescriptorHeaps;
    uint32_t LiveGraphicsPsos;
    uint32_t LiveComputePsos;
    uint32_t LiveRaytracingPsos;
} D3D12XBOX_PIPELINE_CACHE_STATS;

typedef struct D3D12XBOX_RAYTRACING_PSO_DESC {
    uint64_t GlobalRootSignatureHandle;
    D3D12XBOX_SHADER_BYTECODE DxilLibrary;
    uint32_t MaxPayloadSizeInBytes;
    uint32_t MaxAttributeSizeInBytes;
    uint32_t MaxTraceRecursionDepth;
} D3D12XBOX_RAYTRACING_PSO_DESC;

SCARLETTX_D3D12_API int32_t D3D12XboxInspectShader(
    const D3D12XBOX_SHADER_BYTECODE* shader,
    D3D12XBOX_SHADER_INSPECTION* out_info);

SCARLETTX_D3D12_API int32_t D3D12XboxCreateRootSignature(
    uint64_t device_handle,
    const D3D12XBOX_ROOT_SIGNATURE_DESC* desc,
    uint64_t* out_root_sig_handle);
SCARLETTX_D3D12_API int32_t D3D12XboxDestroyRootSignature(
    uint64_t device_handle,
    uint64_t root_sig_handle);

SCARLETTX_D3D12_API int32_t D3D12XboxCreateDescriptorHeap(
    uint64_t device_handle,
    const D3D12XBOX_DESCRIPTOR_HEAP_DESC* desc,
    uint64_t* out_heap_handle);
SCARLETTX_D3D12_API int32_t D3D12XboxDestroyDescriptorHeap(
    uint64_t device_handle,
    uint64_t heap_handle);
SCARLETTX_D3D12_API uint32_t D3D12XboxGetDescriptorHandleIncrementSize(
    uint64_t device_handle,
    uint32_t heap_type);

SCARLETTX_D3D12_API int32_t D3D12XboxCreateGraphicsPipelineState(
    uint64_t device_handle,
    const D3D12XBOX_GRAPHICS_PSO_DESC* desc,
    uint64_t* out_pso_handle);
SCARLETTX_D3D12_API int32_t D3D12XboxCreateComputePipelineState(
    uint64_t device_handle,
    const D3D12XBOX_COMPUTE_PSO_DESC* desc,
    uint64_t* out_pso_handle);
SCARLETTX_D3D12_API int32_t D3D12XboxCreateRaytracingPipelineState(
    uint64_t device_handle,
    const D3D12XBOX_RAYTRACING_PSO_DESC* desc,
    uint64_t* out_pso_handle);
SCARLETTX_D3D12_API int32_t D3D12XboxDestroyPipelineState(
    uint64_t device_handle,
    uint64_t pso_handle);

SCARLETTX_D3D12_API int32_t D3D12XboxDispatchCompute(
    uint64_t device_handle,
    uint64_t pso_handle,
    uint32_t group_x,
    uint32_t group_y,
    uint32_t group_z,
    const uint32_t* root_constants,
    uint32_t root_constant_count,
    void* out_uav_data,
    uint32_t uav_byte_size);

SCARLETTX_D3D12_API int32_t D3D12XboxRenderWithPipelineState(
    uint64_t device_handle,
    uint64_t pso_handle,
    uint32_t width,
    uint32_t height,
    uint32_t vertex_count,
    const char* optional_ppm_path,
    uint64_t* out_pixel_hash);

SCARLETTX_D3D12_API int32_t D3D12XboxGetPipelineCacheStats(
    uint64_t device_handle,
    D3D12XBOX_PIPELINE_CACHE_STATS* out_stats);

// xgameruntime.dll exports are declared in scarlettx/gdk/xgameruntime_abi.h.

// ---------------------------------------------------------------------------
// XboxBvhBuilder.dll exports
// ---------------------------------------------------------------------------
SCARLETTX_BVH_API int32_t XboxBvhGetPrebuildInfo(
    uint32_t triangle_count,
    uint32_t build_flags,
    D3D12XBOX_RT_AS_PREBUILD_INFO* out_info);
SCARLETTX_BVH_API int32_t XboxBvhBuildBlas(
    const SCARLETT_BVH_BUILD_DESC* desc,
    uint64_t* out_blas_handle);
SCARLETTX_BVH_API int32_t XboxBvhUpdateBlas(
    uint64_t blas_handle,
    const SCARLETT_BVH_BUILD_DESC* desc);
SCARLETTX_BVH_API int32_t XboxBvhBuildTlas(
    const uint64_t* blas_handles,
    uint32_t blas_count,
    uint64_t* out_tlas_handle);
SCARLETTX_BVH_API int32_t XboxBvhGetStats(
    uint64_t handle,
    SCARLETT_BVH_STATS* out_stats);
SCARLETTX_BVH_API int32_t XboxBvhTraceSingleRay(
    uint64_t tlas_handle,
    const SCARLETT_RAY_DESC* ray,
    SCARLETT_RAY_HIT* out_hit);
SCARLETTX_BVH_API int32_t XboxBvhDispatchRays(
    uint64_t tlas_handle,
    const SCARLETT_RT_DISPATCH_DESC* desc,
    const char* optional_ppm_path,
    uint64_t* out_frame_hash);
SCARLETTX_BVH_API int32_t XboxBvhDestroy(uint64_t handle);

enum D3D12XBOX_AS_COPY_MODE : uint32_t {
    D3D12XBOX_AS_COPY_MODE_CLONE = 0,
    D3D12XBOX_AS_COPY_MODE_COMPACT = 1,
};

SCARLETTX_BVH_API int32_t XboxBvhGetCompactedSize(
    uint64_t blas_handle,
    uint64_t* out_size_bytes);
SCARLETTX_BVH_API int32_t XboxBvhCompactBlas(
    uint64_t src_blas_handle,
    uint64_t* out_compacted_handle);
SCARLETTX_BVH_API int32_t XboxBvhCopyAccelerationStructure(
    uint64_t src_handle,
    uint32_t copy_mode,
    uint64_t* out_dst_handle);

}  // extern "C"
