#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "scarlettx/gpu/d3d12_xs_abi.h"

#define SX_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "[test_d3d12_xs_abi] FAILED: " #cond                   \
                      << " at line " << __LINE__ << "\n";                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#define SX_CHECK_EQ(a, b)                                                       \
    do {                                                                        \
        const auto _a = (a);                                                    \
        const auto _b = (b);                                                    \
        if (!(_a == _b)) {                                                      \
            std::cerr << "[test_d3d12_xs_abi] FAILED: " #a " == " #b            \
                      << " (" << _a << " != " << _b << ") at line " << __LINE__ \
                      << "\n";                                                  \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#endif

namespace {

SCARLETT_BVH_TRIANGLE MakeTri(float z, float r, float g, float b) {
    SCARLETT_BVH_TRIANGLE t{};
    t.v0[0] = -1.0f; t.v0[1] = -1.0f; t.v0[2] = z;
    t.v1[0] =  1.0f; t.v1[1] = -1.0f; t.v1[2] = z;
    t.v2[0] =  0.0f; t.v2[1] =  1.0f; t.v2[2] = z;
    t.color[0] = r; t.color[1] = g; t.color[2] = b;
    t.reflectivity = 0.25f;
    return t;
}

}  // namespace

int main() {
    // 1. Direct PC D3D12 creation via D3D12XboxCreateDevice + root signature serialization
#if defined(_WIN32)
    {
        D3D12XboxSetProcessDebugFlags(D3D12XBOX_PROCESS_DEBUG_FLAG_NONE);
        SX_CHECK_EQ(D3D12XboxGetProcessDebugFlags(), 0u);
        D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
        params.Version = 1;
        params.PreferWarp = 1;
        ID3D12Device* raw_dev = nullptr;
        const int32_t hr = D3D12XboxCreateDevice(nullptr, &params, &__uuidof(ID3D12Device),
                                                 reinterpret_cast<void**>(&raw_dev));
        SX_CHECK(hr >= 0);
        SX_CHECK(raw_dev != nullptr);

        // Verify we can create a real ID3D12CommandQueue on the returned PC D3D12 device
        D3D12_COMMAND_QUEUE_DESC qdesc{};
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* queue = nullptr;
        SX_CHECK(SUCCEEDED(raw_dev->CreateCommandQueue(&qdesc, __uuidof(ID3D12CommandQueue),
                                                       reinterpret_cast<void**>(&queue))));
        SX_CHECK(queue != nullptr);

        // Serialize + deserialize a root signature through d3d12_xs.dll
        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob* blob = nullptr;
        ID3DBlob* err_blob = nullptr;
        SX_CHECK(D3D12XsSerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                               reinterpret_cast<void**>(&blob),
                                               reinterpret_cast<void**>(&err_blob)) >= 0);
        SX_CHECK(blob != nullptr);
        SX_CHECK(blob->GetBufferSize() > 0);

        ID3D12RootSignatureDeserializer* deser = nullptr;
        SX_CHECK(D3D12XsCreateRootSignatureDeserializer(
                     blob->GetBufferPointer(), blob->GetBufferSize(),
                     &__uuidof(ID3D12RootSignatureDeserializer),
                     reinterpret_cast<void**>(&deser)) >= 0);
        SX_CHECK(deser != nullptr);
        SX_CHECK(deser->GetRootSignatureDesc() != nullptr);

        deser->Release();
        blob->Release();
        if (err_blob) err_blob->Release();
        queue->Release();
        raw_dev->Release();
    }
#endif

    // 2. Scarlett handle-based device, native device extraction, resource allocation, frame pipeline
    uint64_t dev = 0;
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = 1;
    SX_CHECK_EQ(D3D12XboxCreateSyntheticDevice(&params, &dev), 0);
    SX_CHECK(dev != 0);

    // Null & invalid handle checks
    SX_CHECK(D3D12XboxQueryCapabilities(dev, nullptr) != 0);
    SX_CHECK(D3D12XboxQueryRaytracingCapabilities(0xDEADBEEFull, nullptr) != 0);

    D3D12XBOX_RAYTRACING_CAPABILITIES rt_caps{};
    SX_CHECK_EQ(D3D12XboxQueryRaytracingCapabilities(dev, &rt_caps), 0);
    SX_CHECK(rt_caps.ActiveBackend == D3D12XBOX_RT_BACKEND_HARDWARE_DXR ||
             rt_caps.ActiveBackend == D3D12XBOX_RT_BACKEND_SOFTWARE_BVH);
    SX_CHECK(rt_caps.MaxRecursionDepth == 31);
    SX_CHECK(std::strlen(rt_caps.AdapterName) > 0);

    // Resource allocation info via real ID3D12Device::GetResourceAllocationInfo
    uint64_t alloc_size = 0;
    uint64_t alloc_align = 0;
    SX_CHECK_EQ(D3D12XboxGetResourceAllocationInfo(dev, 1920, 1080, 28 /* R8G8B8A8_UNORM */,
                                                   &alloc_size, &alloc_align), 0);
    SX_CHECK(alloc_size >= 1920ull * 1080ull * 4ull);
    SX_CHECK(alloc_align >= 65536ull);

    uint64_t bc_size = 0;
    uint64_t bc_align = 0;
    SX_CHECK_EQ(D3D12XboxGetResourceAllocationInfo(dev, 1920, 1080, 71 /* BC1_UNORM */,
                                                   &bc_size, &bc_align), 0);
    SX_CHECK(bc_size >= 480ull * 270ull * 8ull);
    SX_CHECK(bc_size < alloc_size);
    SX_CHECK(D3D12XboxGetResourceAllocationInfo(dev, 1920, 1080, 0 /* UNKNOWN */,
                                                &bc_size, &bc_align) != 0);

    // Frame interval, schedule frame event, WaitFrameEventXEx, PresentX, stats, Suspend/Resume
    SX_CHECK_EQ(D3D12XboxSetFrameIntervalX(dev, D3D12XBOX_FRAME_INTERVAL_120_HZ, 0), 0);

#if defined(_WIN32)
    HANDLE frame_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SX_CHECK(frame_ev != nullptr);
    SX_CHECK_EQ(D3D12XboxScheduleFrameEventX(dev, D3D12XBOX_FRAME_EVENT_ORIGIN, 0, frame_ev), 0);
    SX_CHECK_EQ(WaitForSingleObject(frame_ev, 1000), WAIT_OBJECT_0);
    CloseHandle(frame_ev);

    ID3D12Device* native_dev = nullptr;
    SX_CHECK_EQ(D3D12XboxGetNativeDevice(dev, reinterpret_cast<void**>(&native_dev)), 0);
    SX_CHECK(native_dev != nullptr);

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    SX_CHECK(SUCCEEDED(native_dev->CreateCommandQueue(&qdesc, __uuidof(ID3D12CommandQueue),
                                                      reinterpret_cast<void**>(&queue))));
#endif

    D3D12XBOX_FRAME_PIPELINE_TOKEN tok1 = 0;
    SX_CHECK_EQ(D3D12XboxWaitFrameEventXEx(dev, D3D12XBOX_FRAME_EVENT_ORIGIN, 100, &tok1), 0);
    SX_CHECK_EQ(tok1, 1ull);

    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.Token = tok1;
    plane.Width = 1280;
    plane.Height = 720;

    D3D12XBOX_PRESENT_PARAMETERS present_params{};
    present_params.Token = tok1;
    present_params.PlaneCount = 1;
    present_params.Flags = D3D12XBOX_PRESENT_FLAG_IMMEDIATE;
    present_params.Planes = &plane;
#if defined(_WIN32)
    present_params.CommandQueue = queue;
#endif
    SX_CHECK_EQ(D3D12XboxPresentXParams(dev, &present_params), 0);

    D3D12XBOX_FRAME_STATISTICS stats{};
    SX_CHECK_EQ(D3D12XboxGetFrameStatisticsX(dev, &stats), 0);
    SX_CHECK_EQ(stats.PresentedFrameCount, 1ull);
    SX_CHECK_EQ(stats.LastPresentedToken, 1ull);
    SX_CHECK_EQ(stats.LastIssuedToken, 1ull);
    SX_CHECK_EQ(stats.IntervalHz, 120u);
    SX_CHECK_EQ(stats.Suspended, 0u);

    // Suspend rejects PresentX until resumed
    SX_CHECK_EQ(D3D12XboxSuspendX(dev, 0), 0);
    SX_CHECK(D3D12XboxPresentXParams(dev, &present_params) != 0);
    SX_CHECK_EQ(D3D12XboxResumeX(dev), 0);
    SX_CHECK_EQ(D3D12XboxPresentXParams(dev, &present_params), 0);

#if defined(_WIN32)
    queue->Release();
    native_dev->Release();
#endif

    // 3. DXR Acceleration Structure translation via d3d12_xs.dll
    D3D12XBOX_RT_AS_INPUTS pre_in{};
    pre_in.Type = D3D12XBOX_AS_TYPE_BOTTOM_LEVEL;
    pre_in.Flags = D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE | D3D12XBOX_AS_BUILD_FLAG_PREFER_FAST_TRACE;
    pre_in.NumDescs = 2;
    D3D12XBOX_RT_AS_PREBUILD_INFO pre_info{};
    SX_CHECK_EQ(D3D12XboxGetRaytracingAccelerationStructurePrebuildInfo(dev, &pre_in, &pre_info), 0);
    SX_CHECK(pre_info.ResultDataMaxSizeInBytes > 0);
    SX_CHECK(pre_info.ScratchDataSizeInBytes > 0);
    SX_CHECK(pre_info.UpdateScratchDataSizeInBytes > 0);

    SCARLETT_BVH_TRIANGLE tris[2] = {
        MakeTri(2.0f, 1.0f, 0.2f, 0.2f),
        MakeTri(4.0f, 0.2f, 0.4f, 1.0f),
    };
    D3D12XBOX_RT_AS_BUILD_DESC blas_desc{};
    blas_desc.Inputs = pre_in;
    blas_desc.Triangles = tris;
    uint64_t dxr_blas = 0;
    SX_CHECK_EQ(D3D12XboxBuildRaytracingAccelerationStructure(dev, &blas_desc, &dxr_blas), 0);
    SX_CHECK(dxr_blas != 0);

    D3D12XBOX_RT_AS_BUILD_DESC tlas_desc{};
    tlas_desc.Inputs.Type = D3D12XBOX_AS_TYPE_TOP_LEVEL;
    tlas_desc.Inputs.Flags = D3D12XBOX_AS_BUILD_FLAG_PREFER_FAST_TRACE;
    tlas_desc.Inputs.NumDescs = 1;
    tlas_desc.BlasHandles = &dxr_blas;
    uint64_t dxr_tlas = 0;
    SX_CHECK_EQ(D3D12XboxBuildRaytracingAccelerationStructure(dev, &tlas_desc, &dxr_tlas), 0);
    SX_CHECK(dxr_tlas != 0);

    SX_CHECK_EQ(D3D12XboxDestroyRaytracingAccelerationStructure(dev, dxr_tlas), 0);
    SX_CHECK_EQ(D3D12XboxDestroyRaytracingAccelerationStructure(dev, dxr_blas), 0);
    SX_CHECK(D3D12XboxDestroyRaytracingAccelerationStructure(dev, dxr_blas) != 0);

    SX_CHECK_EQ(D3D12XboxDestroyDevice(dev), 0);

    // 4. XboxBvhBuilder.dll extended APIs (prebuild, build, update, stats, single-ray trace)
    D3D12XBOX_RT_AS_PREBUILD_INFO bvh_pre{};
    SX_CHECK_EQ(XboxBvhGetPrebuildInfo(2, D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE, &bvh_pre), 0);
    SX_CHECK(bvh_pre.ResultDataMaxSizeInBytes >= 256);
    SX_CHECK(bvh_pre.UpdateScratchDataSizeInBytes >= 256);

    SCARLETT_BVH_BUILD_DESC bdesc{};
    bdesc.Triangles = tris;
    bdesc.TriangleCount = 2;
    bdesc.BuildFlags = D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE;
    uint64_t blas = 0;
    SX_CHECK_EQ(XboxBvhBuildBlas(&bdesc, &blas), 0);

    SCARLETT_BVH_STATS bstats{};
    SX_CHECK_EQ(XboxBvhGetStats(blas, &bstats), 0);
    SX_CHECK_EQ(bstats.TriangleCount, 2u);
    SX_CHECK(bstats.NodeCount >= 1u);

    // Update BLAS geometry (move first triangle closer to z = 1.5)
    tris[0] = MakeTri(1.5f, 0.2f, 1.0f, 0.3f);
    SX_CHECK_EQ(XboxBvhUpdateBlas(blas, &bdesc), 0);

    uint64_t tlas = 0;
    SX_CHECK_EQ(XboxBvhBuildTlas(&blas, 1, &tlas), 0);

    SCARLETT_RAY_DESC ray{};
    ray.Origin[0] = 0.0f; ray.Origin[1] = 0.0f; ray.Origin[2] = 0.0f;
    ray.Direction[0] = 0.0f; ray.Direction[1] = 0.0f; ray.Direction[2] = 1.0f;
    ray.TMin = 0.001f;
    ray.TMax = 100.0f;
    SCARLETT_RAY_HIT hit{};
    SX_CHECK_EQ(XboxBvhTraceSingleRay(tlas, &ray, &hit), 0);
    SX_CHECK_EQ(hit.Hit, 1u);
    SX_CHECK(std::fabs(hit.T - 1.5f) < 0.01f);

    // Ray pointing away should miss
    ray.Direction[2] = -1.0f;
    SCARLETT_RAY_HIT miss{};
    SX_CHECK_EQ(XboxBvhTraceSingleRay(tlas, &ray, &miss), 0);
    SX_CHECK_EQ(miss.Hit, 0u);

    SX_CHECK_EQ(XboxBvhDestroy(tlas), 0);
    SX_CHECK_EQ(XboxBvhDestroy(blas), 0);

    std::cout << "[PASS] test_d3d12_xs_abi (adapter=" << rt_caps.AdapterName
              << ", hw_dxr_tier=" << rt_caps.HardwareRaytracingTier
              << ", active_rt_backend="
              << (rt_caps.ActiveBackend == D3D12XBOX_RT_BACKEND_HARDWARE_DXR ? "DXR" : "SoftwareBVH")
              << ")\n";
    return 0;
}
