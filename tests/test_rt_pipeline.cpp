#include "sx_check.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <vector>

#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/gpu/rt_bvh.h"

int main() {
    auto scene = scarlettx::BuildDefaultRtTestScene();
    SX_REQUIRE(scene.size() == 12);

    scarlettx::BvhAccelerationStructure bvh;
    auto st = bvh.Build(scene);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(bvh.TriangleCount() == 12);
    SX_REQUIRE(bvh.NodeCount() > 1);

    scarlettx::RtDispatchParams params{};
    params.width = 160;
    params.height = 120;
    params.max_bounces = 2;

    scarlettx::FrameBufferRgba8 frame1;
    scarlettx::RtStats stats1{};
    st = bvh.RenderScene(params, frame1, &stats1);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(stats1.primary_rays == 160u * 120u);
    SX_REQUIRE(stats1.shadow_rays > 0);
    SX_REQUIRE(stats1.reflection_rays > 0);
    SX_REQUIRE(stats1.aabb_tests > 0);
    SX_REQUIRE(stats1.triangle_tests > 0);

    // Determinism check: second RT dispatch must produce identical pixel hash.
    scarlettx::FrameBufferRgba8 frame2;
    st = bvh.RenderScene(params, frame2, nullptr);
    SX_REQUIRE(st.ok());
    const uint64_t h1 = frame1.ComputeFnv1aHash();
    const uint64_t h2 = frame2.ComputeFnv1aHash();
    SX_REQUIRE(h1 == h2);

    const auto ppm_path = std::filesystem::path(SCARLETTX_BINARY_DIR) / "test_rt_pipeline_frame.ppm";
    SX_REQUIRE(frame1.WritePpm(ppm_path.string()).ok());
    SX_REQUIRE(std::filesystem::exists(ppm_path));

    // XboxBvhBuilder compaction & copy acceleration structure tests
    {
        std::vector<SCARLETT_BVH_TRIANGLE> abi_tris;
        abi_tris.reserve(scene.size());
        for (const auto& t : scene) {
            SCARLETT_BVH_TRIANGLE a{};
            a.v0[0] = t.v0.x; a.v0[1] = t.v0.y; a.v0[2] = t.v0.z;
            a.v1[0] = t.v1.x; a.v1[1] = t.v1.y; a.v1[2] = t.v1.z;
            a.v2[0] = t.v2.x; a.v2[1] = t.v2.y; a.v2[2] = t.v2.z;
            a.color[0] = t.color.x; a.color[1] = t.color.y; a.color[2] = t.color.z;
            a.reflectivity = t.reflectivity;
            abi_tris.push_back(a);
        }

        D3D12XBOX_RT_AS_PREBUILD_INFO prebuild{};
        SX_REQUIRE(XboxBvhGetPrebuildInfo(
                       static_cast<uint32_t>(abi_tris.size()),
                       D3D12XBOX_AS_BUILD_FLAG_ALLOW_COMPACTION | D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE,
                       &prebuild) == 0);

        SCARLETT_BVH_BUILD_DESC bdesc{};
        bdesc.Triangles = abi_tris.data();
        bdesc.TriangleCount = static_cast<uint32_t>(abi_tris.size());
        bdesc.BuildFlags = D3D12XBOX_AS_BUILD_FLAG_ALLOW_COMPACTION | D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE;
        uint64_t blas = 0;
        SX_REQUIRE(XboxBvhBuildBlas(&bdesc, &blas) == 0 && blas != 0);

        // Compacted size query & error handling
        uint64_t compact_bytes = 0;
        SX_REQUIRE(XboxBvhGetCompactedSize(blas, nullptr) != 0);
        SX_REQUIRE(XboxBvhGetCompactedSize(0xDEADBEEFull, &compact_bytes) != 0);
        SX_REQUIRE(XboxBvhGetCompactedSize(blas, &compact_bytes) == 0);
        SX_REQUIRE(compact_bytes > 0);
        SX_REQUIRE((compact_bytes % 256ull) == 0);
        SX_REQUIRE(compact_bytes <= prebuild.ResultDataMaxSizeInBytes);

        // Compact BLAS -> deep copy independent of source BLAS
        uint64_t compacted_blas = 0;
        SX_REQUIRE(XboxBvhCompactBlas(0xDEADBEEFull, &compacted_blas) != 0);
        SX_REQUIRE(XboxBvhCompactBlas(blas, nullptr) != 0);
        SX_REQUIRE(XboxBvhCompactBlas(blas, &compacted_blas) == 0);
        SX_REQUIRE(compacted_blas != 0 && compacted_blas != blas);

        // Clone BLAS via XboxBvhCopyAccelerationStructure before modifying/destroying source
        uint64_t cloned_blas = 0;
        SX_REQUIRE(XboxBvhCopyAccelerationStructure(blas, 99u, &cloned_blas) != 0);
        SX_REQUIRE(XboxBvhCopyAccelerationStructure(0xDEADBEEFull, D3D12XBOX_AS_COPY_MODE_CLONE, &cloned_blas) != 0);
        SX_REQUIRE(XboxBvhCopyAccelerationStructure(blas, D3D12XBOX_AS_COPY_MODE_CLONE, &cloned_blas) == 0);
        SX_REQUIRE(cloned_blas != 0 && cloned_blas != blas && cloned_blas != compacted_blas);

        // Destroy original BLAS; compacted & cloned BLAS must remain fully valid
        SX_REQUIRE(XboxBvhDestroy(blas) == 0);

        uint64_t tlas_from_compact = 0;
        SX_REQUIRE(XboxBvhBuildTlas(&compacted_blas, 1, &tlas_from_compact) == 0);

        SCARLETT_RT_DISPATCH_DESC ddesc{};
        ddesc.Width = params.width;
        ddesc.Height = params.height;
        ddesc.CameraPos[0] = params.camera_pos.x;
        ddesc.CameraPos[1] = params.camera_pos.y;
        ddesc.CameraPos[2] = params.camera_pos.z;
        ddesc.CameraLookAt[0] = params.camera_look_at.x;
        ddesc.CameraLookAt[1] = params.camera_look_at.y;
        ddesc.CameraLookAt[2] = params.camera_look_at.z;
        ddesc.LightPos[0] = params.light_pos.x;
        ddesc.LightPos[1] = params.light_pos.y;
        ddesc.LightPos[2] = params.light_pos.z;
        ddesc.MaxBounces = params.max_bounces;

        uint64_t compact_hash = 0;
        SX_REQUIRE(XboxBvhDispatchRays(tlas_from_compact, &ddesc, nullptr, &compact_hash) == 0);
        SX_REQUIRE(compact_hash == h1);

        // Clone & Compact TLAS via XboxBvhCopyAccelerationStructure
        uint64_t tlas_clone = 0;
        uint64_t tlas_compact = 0;
        SX_REQUIRE(XboxBvhCopyAccelerationStructure(tlas_from_compact, D3D12XBOX_AS_COPY_MODE_CLONE, &tlas_clone) == 0);
        SX_REQUIRE(XboxBvhCopyAccelerationStructure(tlas_from_compact, D3D12XBOX_AS_COPY_MODE_COMPACT, &tlas_compact) == 0);
        SX_REQUIRE(XboxBvhDestroy(tlas_from_compact) == 0);

        uint64_t clone_hash = 0, tlas_compact_hash = 0;
        SX_REQUIRE(XboxBvhDispatchRays(tlas_clone, &ddesc, nullptr, &clone_hash) == 0);
        SX_REQUIRE(XboxBvhDispatchRays(tlas_compact, &ddesc, nullptr, &tlas_compact_hash) == 0);
        SX_REQUIRE(clone_hash == h1);
        SX_REQUIRE(tlas_compact_hash == h1);

        // Ray hit check on compacted TLAS
        SCARLETT_RAY_DESC ray{};
        ray.Origin[0] = 0.0f; ray.Origin[1] = 1.0f; ray.Origin[2] = -3.5f;
        ray.Direction[0] = 0.0f; ray.Direction[1] = 0.0f; ray.Direction[2] = 1.0f;
        ray.TMin = 0.001f;
        ray.TMax = 100.0f;
        SCARLETT_RAY_HIT hit{};
        SX_REQUIRE(XboxBvhTraceSingleRay(tlas_compact, &ray, &hit) == 0);
        SX_REQUIRE(hit.Hit == 1u && hit.T > 0.0f);

        // Verify cloned BLAS is also usable to build a TLAS
        uint64_t tlas_from_clone_blas = 0;
        SX_REQUIRE(XboxBvhBuildTlas(&cloned_blas, 1, &tlas_from_clone_blas) == 0);
        uint64_t clone_blas_hash = 0;
        SX_REQUIRE(XboxBvhDispatchRays(tlas_from_clone_blas, &ddesc, nullptr, &clone_blas_hash) == 0);
        SX_REQUIRE(clone_blas_hash == h1);

        SX_REQUIRE(XboxBvhDestroy(tlas_from_clone_blas) == 0);
        SX_REQUIRE(XboxBvhDestroy(tlas_clone) == 0);
        SX_REQUIRE(XboxBvhDestroy(tlas_compact) == 0);
        SX_REQUIRE(XboxBvhDestroy(cloned_blas) == 0);
        SX_REQUIRE(XboxBvhDestroy(compacted_blas) == 0);
    }

    std::cout << "[test_rt_pipeline] PASSED (nodes=" << stats1.bvh_nodes
              << ", primary=" << stats1.primary_rays
              << ", shadow=" << stats1.shadow_rays
              << ", refl=" << stats1.reflection_rays
              << ", hash=0x" << std::hex << h1 << std::dec << ")\n";
    return 0;
}
