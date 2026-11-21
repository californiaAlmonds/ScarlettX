#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"
#include "scarlettx/gpu/d3d12_backend.h"

namespace scarlettx {

struct Fsr1DispatchParams {
    const uint8_t* input_rgba8 = nullptr;
    uint32_t input_width = 0;
    uint32_t input_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    float sharpness = 0.8f;      // [0.0, 1.0]: 0.0 = minimal sharpening, 1.0 = max FSR1 RCAS limit
    bool enable_rcas = true;     // Run Pass 2 (RCAS) after Pass 1 (EASU)
};

struct Fsr1DispatchStats {
    uint64_t dispatch_count = 0;
    uint64_t output_fnv1a_hash = 0;
    uint32_t easu_groups_x = 0;
    uint32_t easu_groups_y = 0;
    uint32_t rcas_groups_x = 0;
    uint32_t rcas_groups_y = 0;
    size_t easu_dxbc_bytes = 0;
    size_t rcas_dxbc_bytes = 0;
    bool used_gpu_compute = false;
    std::string adapter_description;
};

// Clean-room GPU FidelityFX Super Resolution 1.0 (EASU + RCAS) D3D12 Compute Pipeline.
// Compiles HLSL cs_5_0 shaders live via D3DCompile:
//   Pass 1 (EASU): 12-tap directional edge-adaptive Lanczos2 spatial upsampling with anti-ringing.
//   Pass 2 (RCAS): 5-tap local contrast-adaptive sharpening with non-clipping lobe limits.
class FfxGpuFsrPipeline {
public:
    FfxGpuFsrPipeline();
    ~FfxGpuFsrPipeline();

    FfxGpuFsrPipeline(const FfxGpuFsrPipeline&) = delete;
    FfxGpuFsrPipeline& operator=(const FfxGpuFsrPipeline&) = delete;

    // Initializes D3D12 device (WARP or hardware GPU), compiles EASU & RCAS cs_5_0 shaders,
    // and builds compute root signature & PSOs.
    Status Initialize(bool prefer_warp = true, bool enable_debug_layer = false);

    // Initializes using an existing ID3D12Device* (caller retains ownership; AddRef'd internally).
    Status InitializeWithDevice(void* native_d3d12_device);

    void Shutdown();
    bool IsInitialized() const noexcept;
    const D3D12AdapterInfo& AdapterInfo() const noexcept { return adapter_info_; }

    // Uploads RGBA8 input, dispatches EASU -> UAV barrier -> RCAS on GPU, reads back RGBA8 output.
    Status Dispatch(
        const Fsr1DispatchParams& params,
        FrameBufferRgba8& out_frame,
        Fsr1DispatchStats* out_stats = nullptr);

    static const char* GetEasuHlslSource() noexcept;
    static const char* GetRcasHlslSource() noexcept;

    // CPU bilinear reference upscaler for edge-sharpness & quality comparison.
    static Status UpscaleBilinearCpu(
        const uint8_t* input_rgba8,
        uint32_t input_width,
        uint32_t input_height,
        uint32_t output_width,
        uint32_t output_height,
        FrameBufferRgba8& out_frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    D3D12AdapterInfo adapter_info_{};
};

}  // namespace scarlettx
