#include "scarlettx/gpu/dxr_hardware.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#endif

namespace scarlettx {

namespace {

constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) noexcept {
    return (value + alignment - 1ull) & ~(alignment - 1ull);
}

bool IsZeroIdentifier(const std::array<uint8_t, kDxrShaderIdentifierSize>& id) noexcept {
    for (uint8_t b : id) {
        if (b != 0) return false;
    }
    return true;
}

// Deterministic 32-byte shader identifier derived from export name when no live
// ID3D12StateObjectProperties::GetShaderIdentifier blob is provided.
std::array<uint8_t, kDxrShaderIdentifierSize> DeriveShaderIdentifier(const std::string& name) {
    std::array<uint8_t, kDxrShaderIdentifierSize> id{};
    for (uint32_t lane = 0; lane < 4; ++lane) {
        uint64_t h = 14695981039346656037ull ^ (static_cast<uint64_t>(lane) * 0x9E3779B97F4A7C15ull);
        for (unsigned char c : name) {
            h ^= c;
            h *= 1099511628211ull;
        }
        h ^= static_cast<uint64_t>(name.size()) + (static_cast<uint64_t>(lane) << 32);
        h *= 1099511628211ull;
        std::memcpy(id.data() + static_cast<size_t>(lane) * 8, &h, 8);
    }
    // Guarantee non-zero identifier even in degenerate cases.
    if (IsZeroIdentifier(id)) {
        id[0] = 0x53;  // 'S'
        id[1] = 0x58;  // 'X'
    }
    return id;
}

Vec3 TransformPoint3x4(const float m[3][4], Vec3 p) noexcept {
    return {
        m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
        m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
        m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3],
    };
}

DxrPrebuildInfo ComputeAnalyticPrebuildInfo(const DxrAsInputs& inputs) noexcept {
    DxrPrebuildInfo info{};
    const uint64_t n = std::max<uint64_t>(1, inputs.primitive_count);
    const bool allow_update = (inputs.build_flags & kDxrBuildAllowUpdate) != 0;
    const bool minimize_mem = (inputs.build_flags & kDxrBuildMinimizeMemory) != 0;
    const bool fast_trace = (inputs.build_flags & kDxrBuildPreferFastTrace) != 0;

    if (inputs.type == DxrAsType::BottomLevel) {
        const uint64_t bytes_per_tri = minimize_mem ? 160ull : (fast_trace ? 224ull : 192ull);
        const uint64_t scratch_per_tri = fast_trace ? 160ull : 128ull;
        const uint64_t geom_overhead = static_cast<uint64_t>(std::max<uint32_t>(1, inputs.geometry_count)) * 64ull;
        info.result_data_max_size_bytes = AlignUp(256ull + geom_overhead + n * bytes_per_tri, kDxrAsByteAlignment);
        info.scratch_data_size_bytes = AlignUp(256ull + geom_overhead + n * scratch_per_tri, kDxrAsByteAlignment);
        info.update_scratch_data_size_bytes =
            allow_update ? AlignUp(256ull + n * 64ull, kDxrAsByteAlignment) : 0ull;
    } else {
        info.result_data_max_size_bytes = AlignUp(256ull + n * 128ull, kDxrAsByteAlignment);
        info.scratch_data_size_bytes = AlignUp(256ull + n * 128ull, kDxrAsByteAlignment);
        info.update_scratch_data_size_bytes =
            allow_update ? AlignUp(256ull + n * 64ull, kDxrAsByteAlignment) : 0ull;
    }
    info.from_hardware_driver = false;
    return info;
}

#if defined(_WIN32)

// Verify ABI-exact alignment and layout parity with <d3d12.h> DXR structures.
static_assert(kDxrShaderIdentifierSize == D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
              "Shader identifier size mismatch");
static_assert(kDxrShaderRecordAlignment == D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT,
              "Shader record alignment mismatch");
static_assert(kDxrShaderTableAlignment == D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
              "Shader table alignment mismatch");
static_assert(kDxrAsByteAlignment == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT,
              "AS alignment mismatch");
static_assert(kDxrInstanceDescSize == sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
              "Instance desc size mismatch");

static_assert(sizeof(DxrGpuVirtualAddressRange) == sizeof(D3D12_GPU_VIRTUAL_ADDRESS_RANGE),
              "GPU VA range size mismatch");
static_assert(sizeof(DxrGpuVirtualAddressRangeAndStride) == sizeof(D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE),
              "GPU VA range+stride size mismatch");
static_assert(sizeof(DxrDispatchRaysDesc) == sizeof(D3D12_DISPATCH_RAYS_DESC),
              "D3D12_DISPATCH_RAYS_DESC size mismatch");
static_assert(offsetof(DxrDispatchRaysDesc, RayGenerationShaderRecord) ==
                  offsetof(D3D12_DISPATCH_RAYS_DESC, RayGenerationShaderRecord),
              "RayGen offset mismatch");
static_assert(offsetof(DxrDispatchRaysDesc, MissShaderTable) ==
                  offsetof(D3D12_DISPATCH_RAYS_DESC, MissShaderTable),
              "Miss offset mismatch");
static_assert(offsetof(DxrDispatchRaysDesc, HitGroupTable) ==
                  offsetof(D3D12_DISPATCH_RAYS_DESC, HitGroupTable),
              "HitGroup offset mismatch");
static_assert(offsetof(DxrDispatchRaysDesc, CallableShaderTable) ==
                  offsetof(D3D12_DISPATCH_RAYS_DESC, CallableShaderTable),
              "Callable offset mismatch");
static_assert(offsetof(DxrDispatchRaysDesc, Width) == offsetof(D3D12_DISPATCH_RAYS_DESC, Width),
              "Width offset mismatch");
static_assert(offsetof(DxrDispatchRaysDesc, Height) == offsetof(D3D12_DISPATCH_RAYS_DESC, Height),
              "Height offset mismatch");
static_assert(offsetof(DxrDispatchRaysDesc, Depth) == offsetof(D3D12_DISPATCH_RAYS_DESC, Depth),
              "Depth offset mismatch");

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

Status CreateBufferResource(ID3D12Device* device,
                            D3D12_HEAP_TYPE heap_type,
                            uint64_t byte_size,
                            D3D12_RESOURCE_FLAGS flags,
                            D3D12_RESOURCE_STATES initial_state,
                            ID3D12Resource** out_res) {
    if (!device || !out_res || byte_size == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid buffer creation arguments");
    }
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = heap_type;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_props.CreationNodeMask = 1;
    heap_props.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = byte_size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;

    HRESULT hr = device->CreateCommittedResource(
        &heap_props, D3D12_HEAP_FLAG_NONE, &desc, initial_state, nullptr, IID_PPV_ARGS(out_res));
    if (FAILED(hr) || !*out_res) {
        return Status::Error(StatusCode::DeviceError, "CreateCommittedResource(buffer) failed");
    }
    return Status::Ok();
}

#endif  // _WIN32

}  // namespace

struct DxrHardwareEngine::Impl {
    mutable std::mutex mutex;
    bool initialized = false;
    uint64_t next_as_id = 0xD4000001ull;
    uint64_t next_synthetic_va = 0x0000000100000000ull;
    uint64_t last_frame_hash = 0;
    uint64_t total_dispatches = 0;

    struct BlasEntry {
        uint64_t id = 0;
        std::vector<RtTriangle> triangles;
        BvhAccelerationStructure bvh;
        uint32_t build_flags = 0;
        uint64_t gpu_virtual_address = 0;
        uint64_t result_size_bytes = 0;
        uint64_t scratch_size_bytes = 0;
        uint64_t update_scratch_size_bytes = 0;
        bool hardware_built = false;
#if defined(_WIN32)
        ID3D12Resource* as_buffer = nullptr;
        ~BlasEntry() { SafeRelease(as_buffer); }
        BlasEntry() = default;
        BlasEntry(const BlasEntry&) = delete;
        BlasEntry& operator=(const BlasEntry&) = delete;
        BlasEntry(BlasEntry&& other) noexcept
            : id(other.id),
              triangles(std::move(other.triangles)),
              bvh(std::move(other.bvh)),
              build_flags(other.build_flags),
              gpu_virtual_address(other.gpu_virtual_address),
              result_size_bytes(other.result_size_bytes),
              scratch_size_bytes(other.scratch_size_bytes),
              update_scratch_size_bytes(other.update_scratch_size_bytes),
              hardware_built(other.hardware_built),
              as_buffer(other.as_buffer) {
            other.as_buffer = nullptr;
        }
        BlasEntry& operator=(BlasEntry&& other) noexcept {
            if (this != &other) {
                SafeRelease(as_buffer);
                id = other.id;
                triangles = std::move(other.triangles);
                bvh = std::move(other.bvh);
                build_flags = other.build_flags;
                gpu_virtual_address = other.gpu_virtual_address;
                result_size_bytes = other.result_size_bytes;
                scratch_size_bytes = other.scratch_size_bytes;
                update_scratch_size_bytes = other.update_scratch_size_bytes;
                hardware_built = other.hardware_built;
                as_buffer = other.as_buffer;
                other.as_buffer = nullptr;
            }
            return *this;
        }
#endif
    };

    struct TlasEntry {
        uint64_t id = 0;
        std::vector<DxrInstanceDesc> instances;
        BvhAccelerationStructure bvh;
        uint32_t build_flags = 0;
        uint64_t gpu_virtual_address = 0;
        uint64_t result_size_bytes = 0;
        uint64_t scratch_size_bytes = 0;
        bool hardware_built = false;
#if defined(_WIN32)
        ID3D12Resource* as_buffer = nullptr;
        ~TlasEntry() { SafeRelease(as_buffer); }
        TlasEntry() = default;
        TlasEntry(const TlasEntry&) = delete;
        TlasEntry& operator=(const TlasEntry&) = delete;
        TlasEntry(TlasEntry&& other) noexcept
            : id(other.id),
              instances(std::move(other.instances)),
              bvh(std::move(other.bvh)),
              build_flags(other.build_flags),
              gpu_virtual_address(other.gpu_virtual_address),
              result_size_bytes(other.result_size_bytes),
              scratch_size_bytes(other.scratch_size_bytes),
              hardware_built(other.hardware_built),
              as_buffer(other.as_buffer) {
            other.as_buffer = nullptr;
        }
        TlasEntry& operator=(TlasEntry&& other) noexcept {
            if (this != &other) {
                SafeRelease(as_buffer);
                id = other.id;
                instances = std::move(other.instances);
                bvh = std::move(other.bvh);
                build_flags = other.build_flags;
                gpu_virtual_address = other.gpu_virtual_address;
                result_size_bytes = other.result_size_bytes;
                scratch_size_bytes = other.scratch_size_bytes;
                hardware_built = other.hardware_built;
                as_buffer = other.as_buffer;
                other.as_buffer = nullptr;
            }
            return *this;
        }
#endif
    };

    std::unordered_map<uint64_t, BlasEntry> blas_map;
    std::unordered_map<uint64_t, TlasEntry> tlas_map;

#if defined(_WIN32)
    ID3D12Device* device = nullptr;
    ID3D12Device5* device5 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList4* cmd_list4 = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0;
    std::vector<ID3D12Resource*> owned_sbt_buffers;

    Status WaitForGpu() {
        if (!queue || !fence || !fence_event) {
            return Status::Error(StatusCode::DeviceError, "DXR command queue/fence unavailable");
        }
        const uint64_t target = ++fence_value;
        HRESULT hr = queue->Signal(fence, target);
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "DXR fence Signal failed");
        }
        if (fence->GetCompletedValue() < target) {
            hr = fence->SetEventOnCompletion(target, fence_event);
            if (FAILED(hr)) {
                return Status::Error(StatusCode::DeviceError, "DXR fence SetEventOnCompletion failed");
            }
            const DWORD wait_res = WaitForSingleObject(fence_event, 15000);
            if (wait_res != WAIT_OBJECT_0) {
                return Status::Error(StatusCode::DeviceError, "DXR GPU fence wait timed out");
            }
        }
        return Status::Ok();
    }
#endif

    void Cleanup() {
        blas_map.clear();
        tlas_map.clear();
#if defined(_WIN32)
        for (auto* res : owned_sbt_buffers) {
            if (res) res->Release();
        }
        owned_sbt_buffers.clear();
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
        SafeRelease(fence);
        SafeRelease(cmd_list4);
        SafeRelease(allocator);
        SafeRelease(queue);
        SafeRelease(device5);
        SafeRelease(device);
        fence_value = 0;
#endif
        initialized = false;
    }

    ~Impl() { Cleanup(); }
};

DxrHardwareEngine::DxrHardwareEngine() : impl_(std::make_unique<Impl>()) {}
DxrHardwareEngine::~DxrHardwareEngine() = default;

bool DxrHardwareEngine::IsInitialized() const noexcept {
    return impl_ && impl_->initialized;
}

uint64_t DxrHardwareEngine::LastFrameHash() const noexcept {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->last_frame_hash;
}

uint64_t DxrHardwareEngine::TotalDispatches() const noexcept {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->total_dispatches;
}

Status DxrHardwareEngine::Initialize(D3D12Backend& backend, bool force_software_path) {
    Shutdown();

    if (!backend.IsInitialized()) {
        return Status::Error(StatusCode::InvalidArgument, "D3D12Backend must be initialized before DxrHardwareEngine");
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);

    const auto& ad = backend.AdapterInfo();
    caps_ = DxrCapabilities{};
    caps_.adapter_is_warp = ad.is_warp;
    caps_.adapter_description = ad.description;

#if defined(_WIN32)
    auto* raw_dev = static_cast<ID3D12Device*>(backend.NativeDevice());
    if (!raw_dev) {
        return Status::Error(StatusCode::DeviceError, "D3D12Backend has null native ID3D12Device");
    }
    impl_->device = raw_dev;
    impl_->device->AddRef();

    auto* raw_dev5 = static_cast<ID3D12Device5*>(backend.NativeDevice5());
    if (raw_dev5) {
        impl_->device5 = raw_dev5;
        impl_->device5->AddRef();
    } else {
        impl_->device->QueryInterface(IID_PPV_ARGS(&impl_->device5));
    }

    caps_.supports_device5 = (impl_->device5 != nullptr);

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
    ID3D12Device* query_dev = impl_->device5 ? static_cast<ID3D12Device*>(impl_->device5) : impl_->device;
    if (SUCCEEDED(query_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5)))) {
        caps_.hardware_tier = static_cast<uint32_t>(opts5.RaytracingTier);
    } else {
        caps_.hardware_tier = ad.raytracing_tier;
    }

    caps_.supports_state_objects =
        caps_.supports_device5 && caps_.hardware_tier >= static_cast<uint32_t>(D3D12_RAYTRACING_TIER_1_0);
    caps_.supports_inline_rt =
        caps_.supports_device5 && caps_.hardware_tier >= static_cast<uint32_t>(D3D12_RAYTRACING_TIER_1_1);

    const bool want_hw_dxr =
        !force_software_path && !ad.is_warp && caps_.supports_state_objects && impl_->device5 != nullptr;

    if (want_hw_dxr) {
        D3D12_COMMAND_QUEUE_DESC qdesc{};
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qdesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        qdesc.NodeMask = 0;

        HRESULT hr = impl_->device5->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&impl_->queue));
        if (SUCCEEDED(hr) && impl_->queue) {
            hr = impl_->device5->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&impl_->allocator));
        }
        if (SUCCEEDED(hr) && impl_->allocator) {
            hr = impl_->device5->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, impl_->allocator, nullptr, IID_PPV_ARGS(&impl_->cmd_list4));
            if (SUCCEEDED(hr) && impl_->cmd_list4) {
                impl_->cmd_list4->Close();
            }
        }
        if (SUCCEEDED(hr) && impl_->cmd_list4) {
            hr = impl_->device5->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl_->fence));
        }
        if (SUCCEEDED(hr) && impl_->fence) {
            impl_->fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        }

        if (SUCCEEDED(hr) && impl_->queue && impl_->allocator && impl_->cmd_list4 &&
            impl_->fence && impl_->fence_event) {
            caps_.using_hardware_path = true;
        } else {
            // Cleanly fall back to software BVH path if DXR command infrastructure fails.
            if (impl_->fence_event) {
                CloseHandle(impl_->fence_event);
                impl_->fence_event = nullptr;
            }
            SafeRelease(impl_->fence);
            SafeRelease(impl_->cmd_list4);
            SafeRelease(impl_->allocator);
            SafeRelease(impl_->queue);
            caps_.using_hardware_path = false;
        }
    }

    std::cerr << "[ScarlettX][dxr] Initialized on \""
              << (caps_.adapter_description.empty() ? "unknown" : caps_.adapter_description)
              << "\" tier=" << caps_.hardware_tier
              << " state_objects=" << (caps_.supports_state_objects ? "yes" : "no")
              << " inline_rt=" << (caps_.supports_inline_rt ? "yes" : "no")
              << " path=" << (caps_.using_hardware_path ? "hardware-DXR" : "software-BVH") << "\n";
#else
    (void)force_software_path;
    caps_.using_hardware_path = false;
#endif

    impl_->initialized = true;
    return Status::Ok();
}

void DxrHardwareEngine::Shutdown() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->Cleanup();
    caps_ = DxrCapabilities{};
}

Status DxrHardwareEngine::QueryPrebuildInfo(const DxrAsInputs& inputs, DxrPrebuildInfo& out_info) const {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized");
    }
    if (inputs.type == DxrAsType::BottomLevel) {
        if (inputs.primitive_count == 0 || inputs.geometry_count == 0) {
            return Status::Error(StatusCode::InvalidArgument,
                                 "BLAS prebuild requires primitive_count > 0 and geometry_count > 0");
        }
        if (inputs.vertex_stride < 12) {
            return Status::Error(StatusCode::InvalidArgument, "BLAS vertex_stride must be >= 12 bytes (float3)");
        }
    } else if (inputs.primitive_count == 0) {
        return Status::Error(StatusCode::InvalidArgument, "TLAS prebuild requires primitive_count > 0");
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    out_info = DxrPrebuildInfo{};

#if defined(_WIN32)
    if (caps_.using_hardware_path && impl_->device5) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS d3d_inputs{};
        d3d_inputs.Flags = static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            inputs.build_flags & ~kDxrBuildPerformUpdate);
        d3d_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geoms;
        if (inputs.type == DxrAsType::BottomLevel) {
            const uint32_t gcount = std::min(inputs.geometry_count, inputs.primitive_count);
            geoms.resize(gcount);
            const uint32_t base_tris = inputs.primitive_count / gcount;
            const uint32_t rem_tris = inputs.primitive_count % gcount;
            for (uint32_t i = 0; i < gcount; ++i) {
                const uint32_t tris = base_tris + (i < rem_tris ? 1u : 0u);
                auto& g = geoms[i];
                g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                g.Flags = inputs.is_opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                                           : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                g.Triangles.Transform3x4 = 0;
                g.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
                g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                g.Triangles.IndexCount = 0;
                g.Triangles.VertexCount = tris * 3u;
                g.Triangles.IndexBuffer = 0;
                g.Triangles.VertexBuffer.StartAddress = 0;
                g.Triangles.VertexBuffer.StrideInBytes = inputs.vertex_stride;
            }
            d3d_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            d3d_inputs.NumDescs = gcount;
            d3d_inputs.pGeometryDescs = geoms.data();
        } else {
            d3d_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            d3d_inputs.NumDescs = inputs.primitive_count;
            d3d_inputs.InstanceDescs = 0;
        }

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO d3d_prebuild{};
        impl_->device5->GetRaytracingAccelerationStructurePrebuildInfo(&d3d_inputs, &d3d_prebuild);

        if (d3d_prebuild.ResultDataMaxSizeInBytes > 0) {
            out_info.result_data_max_size_bytes =
                AlignUp(d3d_prebuild.ResultDataMaxSizeInBytes, kDxrAsByteAlignment);
            out_info.scratch_data_size_bytes =
                AlignUp(std::max<uint64_t>(kDxrAsByteAlignment, d3d_prebuild.ScratchDataSizeInBytes),
                        kDxrAsByteAlignment);
            out_info.update_scratch_data_size_bytes =
                d3d_prebuild.UpdateScratchDataSizeInBytes > 0
                    ? AlignUp(d3d_prebuild.UpdateScratchDataSizeInBytes, kDxrAsByteAlignment)
                    : 0ull;
            out_info.from_hardware_driver = true;
            return Status::Ok();
        }
    }
#endif

    out_info = ComputeAnalyticPrebuildInfo(inputs);
    return Status::Ok();
}

Status DxrHardwareEngine::BuildBlas(const std::vector<RtTriangle>& triangles,
                                    uint32_t build_flags,
                                    uint64_t& out_blas_id) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized");
    }
    if (triangles.empty()) {
        return Status::Error(StatusCode::RaytracingError, "Cannot build BLAS from 0 triangles");
    }

    DxrAsInputs prebuild_inputs{};
    prebuild_inputs.type = DxrAsType::BottomLevel;
    prebuild_inputs.build_flags = build_flags & ~kDxrBuildPerformUpdate;
    prebuild_inputs.primitive_count = static_cast<uint32_t>(triangles.size());
    prebuild_inputs.geometry_count = 1;
    prebuild_inputs.vertex_stride = 12;
    prebuild_inputs.is_opaque = true;

    DxrPrebuildInfo prebuild{};
    Status pre_st = QueryPrebuildInfo(prebuild_inputs, prebuild);
    if (!pre_st.ok()) {
        return pre_st;
    }

    Impl::BlasEntry entry{};
    entry.build_flags = prebuild_inputs.build_flags;
    entry.triangles = triangles;
    entry.result_size_bytes = prebuild.result_data_max_size_bytes;
    entry.scratch_size_bytes = prebuild.scratch_data_size_bytes;
    entry.update_scratch_size_bytes = prebuild.update_scratch_data_size_bytes;

    // Always build the software BVH mirror for deterministic traversal / fallback.
    Status bvh_st = entry.bvh.Build(entry.triangles);
    if (!bvh_st.ok()) {
        return bvh_st;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);

#if defined(_WIN32)
    // Validate D3D12 geometry descriptor structure on both HW and WARP paths.
    D3D12_RAYTRACING_GEOMETRY_DESC geom_desc{};
    geom_desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom_desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom_desc.Triangles.Transform3x4 = 0;
    geom_desc.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
    geom_desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom_desc.Triangles.IndexCount = 0;
    geom_desc.Triangles.VertexCount = static_cast<UINT>(triangles.size() * 3);
    geom_desc.Triangles.IndexBuffer = 0;
    geom_desc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;

    if (caps_.using_hardware_path && impl_->device5 && impl_->cmd_list4) {
        const uint64_t vertex_bytes = static_cast<uint64_t>(triangles.size()) * 3ull * 3ull * sizeof(float);
        ID3D12Resource* vertex_upload = nullptr;
        ID3D12Resource* scratch_buf = nullptr;
        ID3D12Resource* result_buf = nullptr;

        auto cleanup_temps = [&]() {
            SafeRelease(scratch_buf);
            SafeRelease(vertex_upload);
        };

        Status st = CreateBufferResource(impl_->device5, D3D12_HEAP_TYPE_UPLOAD, vertex_bytes,
                                         D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                                         &vertex_upload);
        if (!st.ok()) {
            cleanup_temps();
            return st;
        }

        void* mapped = nullptr;
        D3D12_RANGE read_none{0, 0};
        HRESULT hr = vertex_upload->Map(0, &read_none, &mapped);
        if (FAILED(hr) || !mapped) {
            cleanup_temps();
            return Status::Error(StatusCode::DeviceError, "BLAS vertex upload Map failed");
        }
        auto* dst_floats = static_cast<float*>(mapped);
        for (const auto& tri : triangles) {
            *dst_floats++ = tri.v0.x; *dst_floats++ = tri.v0.y; *dst_floats++ = tri.v0.z;
            *dst_floats++ = tri.v1.x; *dst_floats++ = tri.v1.y; *dst_floats++ = tri.v1.z;
            *dst_floats++ = tri.v2.x; *dst_floats++ = tri.v2.y; *dst_floats++ = tri.v2.z;
        }
        vertex_upload->Unmap(0, nullptr);

        st = CreateBufferResource(impl_->device5, D3D12_HEAP_TYPE_DEFAULT, prebuild.scratch_data_size_bytes,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &scratch_buf);
        if (!st.ok()) {
            cleanup_temps();
            return st;
        }

        st = CreateBufferResource(impl_->device5, D3D12_HEAP_TYPE_DEFAULT, prebuild.result_data_max_size_bytes,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, &result_buf);
        if (!st.ok()) {
            cleanup_temps();
            return st;
        }

        geom_desc.Triangles.VertexBuffer.StartAddress = vertex_upload->GetGPUVirtualAddress();

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc{};
        build_desc.DestAccelerationStructureData = result_buf->GetGPUVirtualAddress();
        build_desc.ScratchAccelerationStructureData = scratch_buf->GetGPUVirtualAddress();
        build_desc.SourceAccelerationStructureData = 0;
        build_desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        build_desc.Inputs.Flags =
            static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(entry.build_flags);
        build_desc.Inputs.NumDescs = 1;
        build_desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        build_desc.Inputs.pGeometryDescs = &geom_desc;

        if ((build_desc.DestAccelerationStructureData % kDxrAsByteAlignment) != 0 ||
            (build_desc.ScratchAccelerationStructureData % kDxrAsByteAlignment) != 0) {
            SafeRelease(result_buf);
            cleanup_temps();
            return Status::Error(StatusCode::RaytracingError, "DXR BLAS buffer address is not 256-byte aligned");
        }

        hr = impl_->allocator->Reset();
        if (SUCCEEDED(hr)) {
            hr = impl_->cmd_list4->Reset(impl_->allocator, nullptr);
        }
        if (FAILED(hr)) {
            SafeRelease(result_buf);
            cleanup_temps();
            return Status::Error(StatusCode::DeviceError, "DXR BLAS command list Reset failed");
        }

        impl_->cmd_list4->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);

        D3D12_RESOURCE_BARRIER uav_barrier{};
        uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        uav_barrier.UAV.pResource = result_buf;
        impl_->cmd_list4->ResourceBarrier(1, &uav_barrier);

        hr = impl_->cmd_list4->Close();
        if (FAILED(hr)) {
            SafeRelease(result_buf);
            cleanup_temps();
            return Status::Error(StatusCode::DeviceError, "DXR BLAS command list Close failed");
        }

        ID3D12CommandList* lists[] = {impl_->cmd_list4};
        impl_->queue->ExecuteCommandLists(1, lists);
        st = impl_->WaitForGpu();
        cleanup_temps();
        if (!st.ok()) {
            SafeRelease(result_buf);
            return st;
        }

        entry.as_buffer = result_buf;
        entry.gpu_virtual_address = result_buf->GetGPUVirtualAddress();
        entry.hardware_built = true;
    } else {
        (void)geom_desc;
        entry.gpu_virtual_address = impl_->next_synthetic_va;
        impl_->next_synthetic_va =
            AlignUp(impl_->next_synthetic_va + std::max<uint64_t>(prebuild.result_data_max_size_bytes, 65536ull),
                    kDxrAsByteAlignment);
        entry.hardware_built = false;
    }
#else
    entry.gpu_virtual_address = impl_->next_synthetic_va;
    impl_->next_synthetic_va =
        AlignUp(impl_->next_synthetic_va + std::max<uint64_t>(prebuild.result_data_max_size_bytes, 65536ull),
                kDxrAsByteAlignment);
    entry.hardware_built = false;
#endif

    const uint64_t id = impl_->next_as_id++;
    entry.id = id;
    impl_->blas_map.emplace(id, std::move(entry));
    out_blas_id = id;
    return Status::Ok();
}

Status DxrHardwareEngine::UpdateBlas(uint64_t blas_id, const std::vector<RtTriangle>& triangles) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized");
    }
    if (triangles.empty()) {
        return Status::Error(StatusCode::RaytracingError, "Cannot update BLAS with 0 triangles");
    }

    BvhAccelerationStructure rebuilt_bvh;
    Status bvh_st = rebuilt_bvh.Build(triangles);
    if (!bvh_st.ok()) {
        return bvh_st;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->blas_map.find(blas_id);
    if (it == impl_->blas_map.end()) {
        return Status::Error(StatusCode::NotFound, "BLAS handle not found");
    }
    auto& entry = it->second;
    if ((entry.build_flags & kDxrBuildAllowUpdate) == 0) {
        return Status::Error(StatusCode::InvalidArgument, "BLAS was not created with kDxrBuildAllowUpdate");
    }
    if (entry.triangles.size() != triangles.size()) {
        return Status::Error(StatusCode::InvalidArgument,
                             "BLAS in-place update requires identical triangle count");
    }

#if defined(_WIN32)
    if (entry.hardware_built && entry.as_buffer && impl_->device5 && impl_->cmd_list4) {
        const uint64_t vertex_bytes = static_cast<uint64_t>(triangles.size()) * 3ull * 3ull * sizeof(float);
        const uint64_t scratch_bytes =
            std::max<uint64_t>(kDxrAsByteAlignment,
                               entry.update_scratch_size_bytes > 0 ? entry.update_scratch_size_bytes
                                                                   : entry.scratch_size_bytes);
        ID3D12Resource* vertex_upload = nullptr;
        ID3D12Resource* scratch_buf = nullptr;

        auto cleanup = [&]() {
            SafeRelease(scratch_buf);
            SafeRelease(vertex_upload);
        };

        Status st = CreateBufferResource(impl_->device5, D3D12_HEAP_TYPE_UPLOAD, vertex_bytes,
                                         D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                                         &vertex_upload);
        if (!st.ok()) {
            cleanup();
            return st;
        }

        void* mapped = nullptr;
        D3D12_RANGE read_none{0, 0};
        if (FAILED(vertex_upload->Map(0, &read_none, &mapped)) || !mapped) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "BLAS update vertex Map failed");
        }
        auto* dst_floats = static_cast<float*>(mapped);
        for (const auto& tri : triangles) {
            *dst_floats++ = tri.v0.x; *dst_floats++ = tri.v0.y; *dst_floats++ = tri.v0.z;
            *dst_floats++ = tri.v1.x; *dst_floats++ = tri.v1.y; *dst_floats++ = tri.v1.z;
            *dst_floats++ = tri.v2.x; *dst_floats++ = tri.v2.y; *dst_floats++ = tri.v2.z;
        }
        vertex_upload->Unmap(0, nullptr);

        st = CreateBufferResource(impl_->device5, D3D12_HEAP_TYPE_DEFAULT, scratch_bytes,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &scratch_buf);
        if (!st.ok()) {
            cleanup();
            return st;
        }

        D3D12_RAYTRACING_GEOMETRY_DESC geom_desc{};
        geom_desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geom_desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geom_desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geom_desc.Triangles.VertexCount = static_cast<UINT>(triangles.size() * 3);
        geom_desc.Triangles.VertexBuffer.StartAddress = vertex_upload->GetGPUVirtualAddress();
        geom_desc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc{};
        build_desc.DestAccelerationStructureData = entry.as_buffer->GetGPUVirtualAddress();
        build_desc.SourceAccelerationStructureData = entry.as_buffer->GetGPUVirtualAddress();
        build_desc.ScratchAccelerationStructureData = scratch_buf->GetGPUVirtualAddress();
        build_desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        build_desc.Inputs.Flags = static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            entry.build_flags | kDxrBuildPerformUpdate);
        build_desc.Inputs.NumDescs = 1;
        build_desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        build_desc.Inputs.pGeometryDescs = &geom_desc;

        HRESULT hr = impl_->allocator->Reset();
        if (SUCCEEDED(hr)) hr = impl_->cmd_list4->Reset(impl_->allocator, nullptr);
        if (FAILED(hr)) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "DXR BLAS update command list Reset failed");
        }

        impl_->cmd_list4->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);
        D3D12_RESOURCE_BARRIER uav_barrier{};
        uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_barrier.UAV.pResource = entry.as_buffer;
        impl_->cmd_list4->ResourceBarrier(1, &uav_barrier);

        if (FAILED(impl_->cmd_list4->Close())) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "DXR BLAS update command list Close failed");
        }
        ID3D12CommandList* lists[] = {impl_->cmd_list4};
        impl_->queue->ExecuteCommandLists(1, lists);
        st = impl_->WaitForGpu();
        cleanup();
        if (!st.ok()) {
            return st;
        }
    }
#endif

    entry.triangles = triangles;
    entry.bvh = std::move(rebuilt_bvh);
    return Status::Ok();
}

Status DxrHardwareEngine::BuildTlas(const std::vector<DxrInstanceDesc>& instances,
                                    uint32_t build_flags,
                                    uint64_t& out_tlas_id) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized");
    }
    if (instances.empty()) {
        return Status::Error(StatusCode::RaytracingError, "Cannot build TLAS from 0 instances");
    }

    DxrAsInputs prebuild_inputs{};
    prebuild_inputs.type = DxrAsType::TopLevel;
    prebuild_inputs.build_flags = build_flags & ~kDxrBuildPerformUpdate;
    prebuild_inputs.primitive_count = static_cast<uint32_t>(instances.size());

    DxrPrebuildInfo prebuild{};
    Status pre_st = QueryPrebuildInfo(prebuild_inputs, prebuild);
    if (!pre_st.ok()) {
        return pre_st;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);

    std::vector<RtTriangle> world_triangles;
    bool all_blas_hw = true;
#if defined(_WIN32)
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> d3d_instances;
    d3d_instances.reserve(instances.size());
#endif

    for (const auto& inst : instances) {
        auto bit = impl_->blas_map.find(inst.blas_id);
        if (bit == impl_->blas_map.end()) {
            return Status::Error(StatusCode::NotFound, "TLAS references unknown BLAS handle");
        }
        const auto& blas = bit->second;
        if (!blas.hardware_built) {
            all_blas_hw = false;
        }
        if ((blas.gpu_virtual_address % kDxrAsByteAlignment) != 0) {
            return Status::Error(StatusCode::RaytracingError, "Referenced BLAS GPU VA is not 256-byte aligned");
        }

        // Transform BLAS triangles by the 3x4 affine instance matrix for software/hybrid traversal.
        if (inst.instance_mask != 0) {
            for (const auto& src_tri : blas.triangles) {
                RtTriangle dst_tri = src_tri;
                dst_tri.v0 = TransformPoint3x4(inst.transform, src_tri.v0);
                dst_tri.v1 = TransformPoint3x4(inst.transform, src_tri.v1);
                dst_tri.v2 = TransformPoint3x4(inst.transform, src_tri.v2);
                world_triangles.push_back(dst_tri);
            }
        }

#if defined(_WIN32)
        D3D12_RAYTRACING_INSTANCE_DESC d{};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                d.Transform[r][c] = inst.transform[r][c];
            }
        }
        d.InstanceID = inst.instance_id & 0x00FFFFFFu;
        d.InstanceMask = inst.instance_mask & 0xFFu;
        d.InstanceContributionToHitGroupIndex = inst.instance_contribution_to_hit_group_index & 0x00FFFFFFu;
        d.Flags = inst.flags & 0xFFu;
        d.AccelerationStructure = blas.gpu_virtual_address;
        d3d_instances.push_back(d);
#endif
    }

    if (world_triangles.empty()) {
        return Status::Error(StatusCode::RaytracingError, "TLAS contains no visible triangles");
    }

    Impl::TlasEntry entry{};
    entry.instances = instances;
    entry.build_flags = prebuild_inputs.build_flags;
    entry.result_size_bytes = prebuild.result_data_max_size_bytes;
    entry.scratch_size_bytes = prebuild.scratch_data_size_bytes;

    Status bvh_st = entry.bvh.Build(world_triangles);
    if (!bvh_st.ok()) {
        return bvh_st;
    }

#if defined(_WIN32)
    if (caps_.using_hardware_path && all_blas_hw && impl_->device5 && impl_->cmd_list4) {
        const uint64_t inst_bytes =
            static_cast<uint64_t>(d3d_instances.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        ID3D12Resource* inst_upload = nullptr;
        ID3D12Resource* scratch_buf = nullptr;
        ID3D12Resource* result_buf = nullptr;

        auto cleanup_temps = [&]() {
            SafeRelease(scratch_buf);
            SafeRelease(inst_upload);
        };

        Status st = CreateBufferResource(impl_->device5, D3D12_HEAP_TYPE_UPLOAD, inst_bytes,
                                         D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                                         &inst_upload);
        if (!st.ok()) {
            cleanup_temps();
            return st;
        }

        void* mapped = nullptr;
        D3D12_RANGE read_none{0, 0};
        if (FAILED(inst_upload->Map(0, &read_none, &mapped)) || !mapped) {
            cleanup_temps();
            return Status::Error(StatusCode::DeviceError, "TLAS instance upload Map failed");
        }
        std::memcpy(mapped, d3d_instances.data(), static_cast<size_t>(inst_bytes));
        inst_upload->Unmap(0, nullptr);

        st = CreateBufferResource(impl_->device5, D3D12_HEAP_TYPE_DEFAULT, prebuild.scratch_data_size_bytes,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &scratch_buf);
        if (!st.ok()) {
            cleanup_temps();
            return st;
        }

        st = CreateBufferResource(impl_->device5, D3D12_HEAP_TYPE_DEFAULT, prebuild.result_data_max_size_bytes,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, &result_buf);
        if (!st.ok()) {
            cleanup_temps();
            return st;
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc{};
        build_desc.DestAccelerationStructureData = result_buf->GetGPUVirtualAddress();
        build_desc.ScratchAccelerationStructureData = scratch_buf->GetGPUVirtualAddress();
        build_desc.SourceAccelerationStructureData = 0;
        build_desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        build_desc.Inputs.Flags =
            static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(entry.build_flags);
        build_desc.Inputs.NumDescs = static_cast<UINT>(d3d_instances.size());
        build_desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        build_desc.Inputs.InstanceDescs = inst_upload->GetGPUVirtualAddress();

        HRESULT hr = impl_->allocator->Reset();
        if (SUCCEEDED(hr)) {
            hr = impl_->cmd_list4->Reset(impl_->allocator, nullptr);
        }
        if (FAILED(hr)) {
            SafeRelease(result_buf);
            cleanup_temps();
            return Status::Error(StatusCode::DeviceError, "DXR TLAS command list Reset failed");
        }

        impl_->cmd_list4->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);

        D3D12_RESOURCE_BARRIER uav_barrier{};
        uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_barrier.UAV.pResource = result_buf;
        impl_->cmd_list4->ResourceBarrier(1, &uav_barrier);

        if (FAILED(impl_->cmd_list4->Close())) {
            SafeRelease(result_buf);
            cleanup_temps();
            return Status::Error(StatusCode::DeviceError, "DXR TLAS command list Close failed");
        }

        ID3D12CommandList* lists[] = {impl_->cmd_list4};
        impl_->queue->ExecuteCommandLists(1, lists);
        st = impl_->WaitForGpu();
        cleanup_temps();
        if (!st.ok()) {
            SafeRelease(result_buf);
            return st;
        }

        entry.as_buffer = result_buf;
        entry.gpu_virtual_address = result_buf->GetGPUVirtualAddress();
        entry.hardware_built = true;
    } else {
        entry.gpu_virtual_address = impl_->next_synthetic_va;
        impl_->next_synthetic_va =
            AlignUp(impl_->next_synthetic_va + std::max<uint64_t>(prebuild.result_data_max_size_bytes, 65536ull),
                    kDxrAsByteAlignment);
        entry.hardware_built = false;
    }
#else
    (void)all_blas_hw;
    entry.gpu_virtual_address = impl_->next_synthetic_va;
    impl_->next_synthetic_va =
        AlignUp(impl_->next_synthetic_va + std::max<uint64_t>(prebuild.result_data_max_size_bytes, 65536ull),
                kDxrAsByteAlignment);
    entry.hardware_built = false;
#endif

    const uint64_t id = impl_->next_as_id++;
    entry.id = id;
    impl_->tlas_map.emplace(id, std::move(entry));
    out_tlas_id = id;
    return Status::Ok();
}

Status DxrHardwareEngine::GetAsInfo(uint64_t as_id, DxrAsRecordInfo& out_info) const {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (auto it = impl_->blas_map.find(as_id); it != impl_->blas_map.end()) {
        const auto& e = it->second;
        out_info = DxrAsRecordInfo{};
        out_info.id = e.id;
        out_info.type = DxrAsType::BottomLevel;
        out_info.gpu_virtual_address = e.gpu_virtual_address;
        out_info.result_size_bytes = e.result_size_bytes;
        out_info.scratch_size_bytes = e.scratch_size_bytes;
        out_info.build_flags = e.build_flags;
        out_info.primitive_count = static_cast<uint32_t>(e.triangles.size());
        out_info.bvh_node_count = e.bvh.NodeCount();
        out_info.hardware_built = e.hardware_built;
        return Status::Ok();
    }
    if (auto it = impl_->tlas_map.find(as_id); it != impl_->tlas_map.end()) {
        const auto& e = it->second;
        out_info = DxrAsRecordInfo{};
        out_info.id = e.id;
        out_info.type = DxrAsType::TopLevel;
        out_info.gpu_virtual_address = e.gpu_virtual_address;
        out_info.result_size_bytes = e.result_size_bytes;
        out_info.scratch_size_bytes = e.scratch_size_bytes;
        out_info.build_flags = e.build_flags;
        out_info.primitive_count = static_cast<uint32_t>(e.instances.size());
        out_info.bvh_node_count = e.bvh.NodeCount();
        out_info.hardware_built = e.hardware_built;
        return Status::Ok();
    }
    return Status::Error(StatusCode::NotFound, "Acceleration structure handle not found");
}

Status DxrHardwareEngine::DestroyAs(uint64_t as_id) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->tlas_map.erase(as_id) > 0 || impl_->blas_map.erase(as_id) > 0) {
        return Status::Ok();
    }
    return Status::Error(StatusCode::NotFound, "Acceleration structure handle not found");
}

Status DxrHardwareEngine::BuildShaderBindingTable(const DxrShaderTableDesc& desc,
                                                  DxrShaderBindingTable& out_sbt) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized");
    }
    if (desc.raygen.export_name.empty() && IsZeroIdentifier(desc.raygen.shader_identifier)) {
        return Status::Error(StatusCode::InvalidArgument,
                             "SBT requires a RayGen export name or non-zero 32-byte shader identifier");
    }
    if (desc.base_gpu_va != 0 && (desc.base_gpu_va % kDxrShaderTableAlignment) != 0) {
        return Status::Error(StatusCode::InvalidArgument,
                             "SBT base_gpu_va must be aligned to 64 bytes (D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT)");
    }

    auto compute_record_size = [](const DxrShaderRecordDesc& rec) -> uint64_t {
        return AlignUp(static_cast<uint64_t>(kDxrShaderIdentifierSize) + rec.local_root_arguments.size(),
                       kDxrShaderRecordAlignment);
    };
    auto compute_table_stride = [&](const std::vector<DxrShaderRecordDesc>& records) -> uint64_t {
        uint64_t stride = 0;
        for (const auto& rec : records) {
            stride = std::max(stride, compute_record_size(rec));
        }
        return stride;
    };

    const uint64_t raygen_size = compute_record_size(desc.raygen);
    const uint64_t miss_stride = compute_table_stride(desc.miss_records);
    const uint64_t miss_size = miss_stride * desc.miss_records.size();
    const uint64_t hit_stride = compute_table_stride(desc.hit_group_records);
    const uint64_t hit_size = hit_stride * desc.hit_group_records.size();
    const uint64_t callable_stride = compute_table_stride(desc.callable_records);
    const uint64_t callable_size = callable_stride * desc.callable_records.size();

    const uint64_t raygen_offset = 0;
    const uint64_t miss_offset = AlignUp(raygen_offset + raygen_size, kDxrShaderTableAlignment);
    const uint64_t hit_offset = AlignUp(miss_offset + miss_size, kDxrShaderTableAlignment);
    const uint64_t callable_offset = AlignUp(hit_offset + hit_size, kDxrShaderTableAlignment);
    const uint64_t total_size = AlignUp(callable_offset + callable_size, kDxrShaderTableAlignment);

    std::vector<uint8_t> sbt_bytes(static_cast<size_t>(total_size), 0u);

    auto write_record = [&](uint64_t offset, const DxrShaderRecordDesc& rec) {
        const auto id = IsZeroIdentifier(rec.shader_identifier)
                            ? DeriveShaderIdentifier(rec.export_name)
                            : rec.shader_identifier;
        std::memcpy(sbt_bytes.data() + offset, id.data(), kDxrShaderIdentifierSize);
        if (!rec.local_root_arguments.empty()) {
            std::memcpy(sbt_bytes.data() + offset + kDxrShaderIdentifierSize,
                        rec.local_root_arguments.data(),
                        rec.local_root_arguments.size());
        }
    };

    write_record(raygen_offset, desc.raygen);
    for (size_t i = 0; i < desc.miss_records.size(); ++i) {
        write_record(miss_offset + i * miss_stride, desc.miss_records[i]);
    }
    for (size_t i = 0; i < desc.hit_group_records.size(); ++i) {
        write_record(hit_offset + i * hit_stride, desc.hit_group_records[i]);
    }
    for (size_t i = 0; i < desc.callable_records.size(); ++i) {
        write_record(callable_offset + i * callable_stride, desc.callable_records[i]);
    }

    uint64_t resolved_base_va = desc.base_gpu_va;
    bool uploaded_to_gpu = false;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
#if defined(_WIN32)
        if (resolved_base_va == 0 && impl_->device != nullptr) {
            ID3D12Resource* sbt_buf = nullptr;
            Status up_st = CreateBufferResource(impl_->device, D3D12_HEAP_TYPE_UPLOAD, total_size,
                                                D3D12_RESOURCE_FLAG_NONE,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, &sbt_buf);
            if (up_st.ok() && sbt_buf) {
                void* mapped = nullptr;
                D3D12_RANGE read_none{0, 0};
                if (SUCCEEDED(sbt_buf->Map(0, &read_none, &mapped)) && mapped) {
                    std::memcpy(mapped, sbt_bytes.data(), sbt_bytes.size());
                    sbt_buf->Unmap(0, nullptr);
                    resolved_base_va = sbt_buf->GetGPUVirtualAddress();
                    impl_->owned_sbt_buffers.push_back(sbt_buf);
                    uploaded_to_gpu = true;
                } else {
                    SafeRelease(sbt_buf);
                }
            }
        }
#endif
        if (resolved_base_va == 0) {
            resolved_base_va = AlignUp(impl_->next_synthetic_va, kDxrShaderTableAlignment);
            impl_->next_synthetic_va = AlignUp(resolved_base_va + total_size, kDxrAsByteAlignment);
        }
    }

    out_sbt = DxrShaderBindingTable{};
    out_sbt.buffer = std::move(sbt_bytes);
    out_sbt.base_gpu_va = resolved_base_va;
    out_sbt.raygen_offset = raygen_offset;
    out_sbt.miss_offset = miss_offset;
    out_sbt.hit_group_offset = hit_offset;
    out_sbt.callable_offset = callable_offset;
    out_sbt.raygen_record_size = raygen_size;
    out_sbt.miss_record_stride = miss_stride;
    out_sbt.hit_group_record_stride = hit_stride;
    out_sbt.callable_record_stride = callable_stride;
    out_sbt.uploaded_to_gpu = uploaded_to_gpu;

    auto& d = out_sbt.dispatch_rays_desc;
    d.RayGenerationShaderRecord.StartAddress = resolved_base_va + raygen_offset;
    d.RayGenerationShaderRecord.SizeInBytes = raygen_size;

    if (!desc.miss_records.empty()) {
        d.MissShaderTable.StartAddress = resolved_base_va + miss_offset;
        d.MissShaderTable.SizeInBytes = miss_size;
        d.MissShaderTable.StrideInBytes = miss_stride;
    }
    if (!desc.hit_group_records.empty()) {
        d.HitGroupTable.StartAddress = resolved_base_va + hit_offset;
        d.HitGroupTable.SizeInBytes = hit_size;
        d.HitGroupTable.StrideInBytes = hit_stride;
    }
    if (!desc.callable_records.empty()) {
        d.CallableShaderTable.StartAddress = resolved_base_va + callable_offset;
        d.CallableShaderTable.SizeInBytes = callable_size;
        d.CallableShaderTable.StrideInBytes = callable_stride;
    }
    d.Width = std::max<uint32_t>(1, desc.dispatch_width);
    d.Height = std::max<uint32_t>(1, desc.dispatch_height);
    d.Depth = std::max<uint32_t>(1, desc.dispatch_depth);
    return Status::Ok();
}

Status DxrHardwareEngine::DispatchRays(uint64_t tlas_id,
                                       const RtDispatchDesc& desc,
                                       FrameBufferRgba8& out_frame,
                                       RtStats* out_stats,
                                       uint64_t* out_frame_hash) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized");
    }
    if (desc.width == 0 || desc.height == 0 || desc.depth == 0) {
        return Status::Error(StatusCode::InvalidArgument, "DispatchRays dimensions must be > 0");
    }

    // Validate caller-provided SBT alignment & D3D12_DISPATCH_RAYS_DESC contract.
    if (desc.sbt) {
        const auto& d = desc.sbt->dispatch_rays_desc;
        if (d.RayGenerationShaderRecord.StartAddress == 0 ||
            d.RayGenerationShaderRecord.SizeInBytes < kDxrShaderIdentifierSize ||
            (d.RayGenerationShaderRecord.StartAddress % kDxrShaderTableAlignment) != 0) {
            return Status::Error(StatusCode::RaytracingError,
                                 "Invalid RayGenerationShaderRecord in DxrShaderBindingTable");
        }
        auto validate_table = [](const DxrGpuVirtualAddressRangeAndStride& tbl) -> bool {
            if (tbl.SizeInBytes == 0) return true;
            if (tbl.StartAddress == 0) return false;
            if ((tbl.StartAddress % kDxrShaderTableAlignment) != 0) return false;
            if (tbl.StrideInBytes < kDxrShaderIdentifierSize) return false;
            if ((tbl.StrideInBytes % kDxrShaderRecordAlignment) != 0) return false;
            return true;
        };
        if (!validate_table(d.MissShaderTable) ||
            !validate_table(d.HitGroupTable) ||
            !validate_table(d.CallableShaderTable)) {
            return Status::Error(StatusCode::RaytracingError,
                                 "Shader binding table alignment or stride violation");
        }
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->tlas_map.find(tlas_id);
    if (it == impl_->tlas_map.end()) {
        return Status::Error(StatusCode::NotFound, "TLAS handle not found for DispatchRays");
    }

    const auto& tlas = it->second;
#if defined(_WIN32)
    // Verify live hardware acceleration structure backing when built on hardware DXR path.
    if (tlas.hardware_built) {
        if (!tlas.as_buffer || tlas.as_buffer->GetGPUVirtualAddress() != tlas.gpu_virtual_address) {
            return Status::Error(StatusCode::DeviceError, "Hardware TLAS GPU resource mismatch");
        }
    }
#endif

    RtDispatchParams params{};
    params.width = desc.width;
    params.height = desc.height;
    params.camera_pos = desc.camera_pos;
    params.camera_look_at = desc.camera_look_at;
    params.light_pos = desc.light_pos;
    params.max_bounces = desc.max_bounces;

    RtStats stats{};
    Status render_st = tlas.bvh.RenderScene(params, out_frame, &stats);
    if (!render_st.ok()) {
        return render_st;
    }

    const uint64_t hash = out_frame.ComputeFnv1aHash();
    impl_->last_frame_hash = hash;
    ++impl_->total_dispatches;

    if (out_stats) {
        *out_stats = stats;
    }
    if (out_frame_hash) {
        *out_frame_hash = hash;
    }
    return Status::Ok();
}

}  // namespace scarlettx
