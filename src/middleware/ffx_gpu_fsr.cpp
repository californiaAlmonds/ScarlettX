#include "scarlettx/middleware/ffx_gpu_fsr.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#endif

namespace scarlettx {

namespace {

constexpr uint32_t kFsrMaxDimension = 8192u;
constexpr uint32_t kFsrThreadGroupSize = 8u;

// Pass 1: FSR 1.0 EASU (Edge-Adaptive Spatial Upsampling)
// 12-tap directional edge-aware Lanczos2 filter with 2x2 anti-ringing clamp.
constexpr const char* kEasuHlsl = R"(
cbuffer FsrParams : register(b0) {
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
    float rcpDstWidth;
    float rcpDstHeight;
    float sharpness;
    float enableRcas;
};

Texture2D<float4> SrcTex : register(t0);
RWTexture2D<float4> DstTex : register(u0);

float4 LoadClamped(int2 p) {
    int2 clamped = clamp(p, int2(0, 0), int2((int)srcWidth - 1, (int)srcHeight - 1));
    return SrcTex.Load(int3(clamped, 0));
}

float CalcLuma(float3 rgb) {
    return 0.5 * rgb.g + 0.25 * (rgb.r + rgb.b);
}

void AccumulateEdgeDir(
    inout float2 dir,
    inout float len,
    float w,
    float lTop,
    float lLeft,
    float lCenter,
    float lRight,
    float lBot) {
    float dc = lRight - lLeft;
    float lenX = max(abs(lRight - lCenter), abs(lCenter - lLeft));
    lenX = (lenX > 1e-5) ? saturate(abs(dc) / lenX) : 0.0;
    lenX *= lenX;

    float dr = lBot - lTop;
    float lenY = max(abs(lBot - lCenter), abs(lCenter - lTop));
    lenY = (lenY > 1e-5) ? saturate(abs(dr) / lenY) : 0.0;
    lenY *= lenY;

    dir += float2(dc, dr) * w;
    len += (lenX + lenY) * w;
}

void AccumulateLanczos2Tap(
    inout float3 aC,
    inout float aW,
    float2 off,
    float2 dir,
    float2 len2,
    float lob,
    float clp,
    float3 c) {
    float2 v;
    v.x = (off.x * dir.x) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len2;
    float d2 = min(dot(v, v), clp);

    // Fast Lanczos2 polynomial with negative lobe and exact zero crossing at d2 = 1.0
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;

    aC += c * w;
    aW += w;
}

[numthreads(8, 8, 1)]
void EasuMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= dstWidth || tid.y >= dstHeight) {
        return;
    }

    float2 scale = float2((float)srcWidth * rcpDstWidth, (float)srcHeight * rcpDstHeight);
    float2 pp = (float2(tid.xy) + 0.5) * scale - 0.5;
    float2 fp = floor(pp);
    float2 f = pp - fp;
    int2 base = int2((int)fp.x, (int)fp.y);

    // 12-tap cross neighborhood around base:
    //       b  c
    //    e  f  g  h
    //    i  j  k  l
    //       n  o
    float4 cb = LoadClamped(base + int2( 0, -1));
    float4 cc = LoadClamped(base + int2( 1, -1));
    float4 ce = LoadClamped(base + int2(-1,  0));
    float4 cf = LoadClamped(base + int2( 0,  0));
    float4 cg = LoadClamped(base + int2( 1,  0));
    float4 ch = LoadClamped(base + int2( 2,  0));
    float4 ci = LoadClamped(base + int2(-1,  1));
    float4 cj = LoadClamped(base + int2( 0,  1));
    float4 ck = LoadClamped(base + int2( 1,  1));
    float4 cl = LoadClamped(base + int2( 2,  1));
    float4 cn = LoadClamped(base + int2( 0,  2));
    float4 co = LoadClamped(base + int2( 1,  2));

    float lb = CalcLuma(cb.rgb);
    float lc = CalcLuma(cc.rgb);
    float le = CalcLuma(ce.rgb);
    float lf = CalcLuma(cf.rgb);
    float lg = CalcLuma(cg.rgb);
    float lh = CalcLuma(ch.rgb);
    float li = CalcLuma(ci.rgb);
    float lj = CalcLuma(cj.rgb);
    float lk = CalcLuma(ck.rgb);
    float ll = CalcLuma(cl.rgb);
    float ln = CalcLuma(cn.rgb);
    float lo = CalcLuma(co.rgb);

    // Bilinearly-weighted gradient direction and anisotropy estimation across the 4 inner crosses
    float2 dir = float2(0.0, 0.0);
    float len = 0.0;
    AccumulateEdgeDir(dir, len, (1.0 - f.x) * (1.0 - f.y), lb, le, lf, lg, lj);
    AccumulateEdgeDir(dir, len,        f.x  * (1.0 - f.y), lc, lf, lg, lh, lk);
    AccumulateEdgeDir(dir, len, (1.0 - f.x) *        f.y , lf, li, lj, lk, ln);
    AccumulateEdgeDir(dir, len,        f.x  *        f.y , lg, lj, lk, ll, lo);

    float dirLenSq = dot(dir, dir);
    if (dirLenSq < 1e-6) {
        dir = float2(1.0, 0.0);
        len = 0.0;
    } else {
        dir *= rsqrt(dirLenSq);
        len = saturate(len * 0.5);
        len *= len;
    }

    float stretch = 1.0 / max(abs(dir.x), abs(dir.y));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((0.25 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    float3 aC = float3(0.0, 0.0, 0.0);
    float aW = 0.0;
    AccumulateLanczos2Tap(aC, aW, float2( 0.0, -1.0) - f, dir, len2, lob, clp, cb.rgb);
    AccumulateLanczos2Tap(aC, aW, float2( 1.0, -1.0) - f, dir, len2, lob, clp, cc.rgb);
    AccumulateLanczos2Tap(aC, aW, float2(-1.0,  0.0) - f, dir, len2, lob, clp, ce.rgb);
    AccumulateLanczos2Tap(aC, aW, float2( 0.0,  0.0) - f, dir, len2, lob, clp, cf.rgb);
    AccumulateLanczos2Tap(aC, aW, float2( 1.0,  0.0) - f, dir, len2, lob, clp, cg.rgb);
    AccumulateLanczos2Tap(aC, aW, float2( 2.0,  0.0) - f, dir, len2, lob, clp, ch.rgb);
    AccumulateLanczos2Tap(aC, aW, float2(-1.0,  1.0) - f, dir, len2, lob, clp, ci.rgb);
    AccumulateLanczos2Tap(aC, aW, float2( 0.0,  1.0) - f, dir, len2, lob, clp, cj.rgb);
    AccumulateLanczos2Tap(aC, aW, float2( 1.0,  1.0) - f, dir, len2, lob, clp, ck.rgb);
    AccumulateLanczos2Tap(aC, aW, float2( 2.0,  1.0) - f, dir, len2, lob, clp, cl.rgb);
    AccumulateLanczos2Tap(aC, aW, float2( 0.0,  2.0) - f, dir, len2, lob, clp, cn.rgb);
    AccumulateLanczos2Tap(aC, aW, float2( 1.0,  2.0) - f, dir, len2, lob, clp, co.rgb);

    // 2x2 anti-ringing clamp to eliminate Lanczos halos while preserving edge sharpness
    float3 min4 = min(min(cf.rgb, cg.rgb), min(cj.rgb, ck.rgb));
    float3 max4 = max(max(cf.rgb, cg.rgb), max(cj.rgb, ck.rgb));
    float3 rgb = clamp(aC / max(aW, 1e-5), min4, max4);

    float alphaTop = lerp(cf.a, cg.a, f.x);
    float alphaBot = lerp(cj.a, ck.a, f.x);
    float alpha = lerp(alphaTop, alphaBot, f.y);

    DstTex[tid.xy] = float4(saturate(rgb), saturate(alpha));
}
)";

// Pass 2: FSR 1.0 RCAS (Robust Contrast-Adaptive Sharpening)
// 5-tap cross contrast-adaptive sharpening with exact local anti-ringing lobe bounds.
constexpr const char* kRcasHlsl = R"(
cbuffer FsrParams : register(b0) {
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
    float rcpDstWidth;
    float rcpDstHeight;
    float sharpness;
    float enableRcas;
};

Texture2D<float4> SrcTex : register(t0);
RWTexture2D<float4> DstTex : register(u0);

float4 LoadClamped(int2 p) {
    int2 clamped = clamp(p, int2(0, 0), int2((int)dstWidth - 1, (int)dstHeight - 1));
    return SrcTex.Load(int3(clamped, 0));
}

float ComputeChannelSafeLobe(float e, float avg4, float mn5, float mx5, float rcasLimit) {
    float diff = e - avg4;
    if (abs(diff) < 1e-5) {
        return 0.0;
    }
    float bound = rcasLimit;
    if (diff > 0.0) {
        float denom = 4.0 * max(mx5 - avg4, 1e-5);
        bound = min(bound, max(mx5 - e, 0.0) / denom);
    } else {
        float denom = 4.0 * max(avg4 - mn5, 1e-5);
        bound = min(bound, max(e - mn5, 0.0) / denom);
    }
    return bound;
}

[numthreads(8, 8, 1)]
void RcasMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= dstWidth || tid.y >= dstHeight) {
        return;
    }

    int2 pos = int2((int)tid.x, (int)tid.y);
    float4 e = LoadClamped(pos);
    if (enableRcas < 0.5 || sharpness <= 1e-4) {
        DstTex[tid.xy] = saturate(e);
        return;
    }

    float4 b = LoadClamped(pos + int2( 0, -1));
    float4 d = LoadClamped(pos + int2(-1,  0));
    float4 f = LoadClamped(pos + int2( 1,  0));
    float4 h = LoadClamped(pos + int2( 0,  1));

    float3 sum4 = b.rgb + d.rgb + f.rgb + h.rgb;
    float3 avg4 = sum4 * 0.25;
    float3 mn4 = min(min(b.rgb, d.rgb), min(f.rgb, h.rgb));
    float3 mx4 = max(max(b.rgb, d.rgb), max(f.rgb, h.rgb));
    float3 mn5 = min(mn4, e.rgb);
    float3 mx5 = max(mx4, e.rgb);

    // FSR 1.0 RCAS maximum negative lobe magnitude (0.25 - 1/16 = 0.1875)
    float rcasLimit = 0.1875;
    float lobeR = ComputeChannelSafeLobe(e.r, avg4.r, mn5.r, mx5.r, rcasLimit);
    float lobeG = ComputeChannelSafeLobe(e.g, avg4.g, mn5.g, mx5.g, rcasLimit);
    float lobeB = ComputeChannelSafeLobe(e.b, avg4.b, mn5.b, mx5.b, rcasLimit);

    // Shared isotropic lobe across RGB prevents color fringing on edges
    float lobeMag = min(lobeR, min(lobeG, lobeB)) * saturate(sharpness);
    float lobe = -clamp(lobeMag, 0.0, rcasLimit);

    float3 sharpened = (e.rgb + lobe * sum4) / (1.0 + 4.0 * lobe);
    DstTex[tid.xy] = float4(saturate(sharpened), saturate(e.a));
}
)";

#if defined(_WIN32)

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    void Reset(T* ptr = nullptr) noexcept {
        if (ptr_) {
            ptr_->Release();
        }
        ptr_ = ptr;
    }

    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }

private:
    T* ptr_ = nullptr;
};

std::string WideToUtf8(const wchar_t* wide) {
    if (!wide) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

uint32_t AlignUp(uint32_t value, uint32_t alignment) noexcept {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

struct FsrRootConstants {
    uint32_t srcWidth;
    uint32_t srcHeight;
    uint32_t dstWidth;
    uint32_t dstHeight;
    float rcpDstWidth;
    float rcpDstHeight;
    float sharpness;
    float enableRcas;
};

static_assert(sizeof(FsrRootConstants) == 32u, "FsrRootConstants must be 8 x 32-bit values");

Status CompileComputeShader(
    const char* hlsl_src,
    const char* source_name,
    const char* entry_point,
    ComPtr<ID3DBlob>& out_blob) {
    ComPtr<ID3DBlob> err_blob;
    HRESULT hr = D3DCompile(
        hlsl_src,
        std::strlen(hlsl_src),
        source_name,
        nullptr,
        nullptr,
        entry_point,
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL1,
        0,
        out_blob.Put(),
        err_blob.Put());
    if (FAILED(hr) || !out_blob) {
        std::string msg = std::string("D3DCompile failed for ") + entry_point;
        if (err_blob && err_blob->GetBufferPointer() && err_blob->GetBufferSize() > 0) {
            msg += ": ";
            msg.append(static_cast<const char*>(err_blob->GetBufferPointer()),
                       err_blob->GetBufferSize());
        }
        return Status::Error(StatusCode::ShaderError, msg);
    }
    return Status::Ok();
}

#endif  // _WIN32

}  // namespace

const char* FfxGpuFsrPipeline::GetEasuHlslSource() noexcept {
    return kEasuHlsl;
}

const char* FfxGpuFsrPipeline::GetRcasHlslSource() noexcept {
    return kRcasHlsl;
}

Status FfxGpuFsrPipeline::UpscaleBilinearCpu(
    const uint8_t* input_rgba8,
    uint32_t input_width,
    uint32_t input_height,
    uint32_t output_width,
    uint32_t output_height,
    FrameBufferRgba8& out_frame) {
    if (!input_rgba8 || input_width == 0 || input_height == 0 ||
        output_width == 0 || output_height == 0 ||
        input_width > kFsrMaxDimension || input_height > kFsrMaxDimension ||
        output_width > kFsrMaxDimension || output_height > kFsrMaxDimension) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid bilinear upscale parameters");
    }

    out_frame.width = output_width;
    out_frame.height = output_height;
    out_frame.pixels.resize(static_cast<size_t>(output_width) * output_height * 4u);

    const float scale_x = static_cast<float>(input_width) / static_cast<float>(output_width);
    const float scale_y = static_cast<float>(input_height) / static_cast<float>(output_height);

    for (uint32_t y = 0; y < output_height; ++y) {
        const float py = (static_cast<float>(y) + 0.5f) * scale_y - 0.5f;
        const int32_t iy0 = static_cast<int32_t>(std::floor(py));
        const float wy = py - static_cast<float>(iy0);
        const uint32_t y0 = static_cast<uint32_t>(std::clamp(iy0, 0, static_cast<int32_t>(input_height) - 1));
        const uint32_t y1 = static_cast<uint32_t>(std::clamp(iy0 + 1, 0, static_cast<int32_t>(input_height) - 1));

        for (uint32_t x = 0; x < output_width; ++x) {
            const float px = (static_cast<float>(x) + 0.5f) * scale_x - 0.5f;
            const int32_t ix0 = static_cast<int32_t>(std::floor(px));
            const float wx = px - static_cast<float>(ix0);
            const uint32_t x0 = static_cast<uint32_t>(std::clamp(ix0, 0, static_cast<int32_t>(input_width) - 1));
            const uint32_t x1 = static_cast<uint32_t>(std::clamp(ix0 + 1, 0, static_cast<int32_t>(input_width) - 1));

            const uint8_t* p00 = input_rgba8 + (static_cast<size_t>(y0) * input_width + x0) * 4u;
            const uint8_t* p10 = input_rgba8 + (static_cast<size_t>(y0) * input_width + x1) * 4u;
            const uint8_t* p01 = input_rgba8 + (static_cast<size_t>(y1) * input_width + x0) * 4u;
            const uint8_t* p11 = input_rgba8 + (static_cast<size_t>(y1) * input_width + x1) * 4u;
            uint8_t* dst = out_frame.pixels.data() + (static_cast<size_t>(y) * output_width + x) * 4u;

            for (int c = 0; c < 4; ++c) {
                const float top = static_cast<float>(p00[c]) * (1.0f - wx) + static_cast<float>(p10[c]) * wx;
                const float bot = static_cast<float>(p01[c]) * (1.0f - wx) + static_cast<float>(p11[c]) * wx;
                const float val = top * (1.0f - wy) + bot * wy;
                dst[c] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(val)), 0, 255));
            }
        }
    }

    return Status::Ok();
}

#if defined(_WIN32)

struct FfxGpuFsrPipeline::Impl {
    std::mutex mutex;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0;

    ComPtr<ID3D12RootSignature> root_sig;
    ComPtr<ID3D12PipelineState> easu_pso;
    ComPtr<ID3D12PipelineState> rcas_pso;
    ComPtr<ID3D12DescriptorHeap> srv_uav_heap;
    UINT descriptor_inc = 0;

    // Cached surface resources (rebuilt when input/output dimensions change)
    uint32_t cached_in_w = 0;
    uint32_t cached_in_h = 0;
    uint32_t cached_out_w = 0;
    uint32_t cached_out_h = 0;
    UINT upload_row_pitch = 0;
    UINT readback_row_pitch = 0;
    UINT64 upload_size = 0;
    UINT64 readback_size = 0;

    ComPtr<ID3D12Resource> input_tex;
    ComPtr<ID3D12Resource> upload_buf;
    ComPtr<ID3D12Resource> intermediate_tex;
    ComPtr<ID3D12Resource> output_tex;
    ComPtr<ID3D12Resource> readback_buf;

    size_t easu_dxbc_bytes = 0;
    size_t rcas_dxbc_bytes = 0;
    uint64_t dispatch_count = 0;

    ~Impl() { Cleanup(); }

    void ReleaseSurfaces() {
        readback_buf.Reset();
        output_tex.Reset();
        intermediate_tex.Reset();
        upload_buf.Reset();
        input_tex.Reset();
        cached_in_w = cached_in_h = cached_out_w = cached_out_h = 0u;
        upload_row_pitch = readback_row_pitch = 0u;
        upload_size = readback_size = 0u;
    }

    void Cleanup() {
        ReleaseSurfaces();
        srv_uav_heap.Reset();
        rcas_pso.Reset();
        easu_pso.Reset();
        root_sig.Reset();
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
        fence.Reset();
        cmd_list.Reset();
        allocator.Reset();
        queue.Reset();
        device.Reset();
        adapter.Reset();
        factory.Reset();
        easu_dxbc_bytes = 0;
        rcas_dxbc_bytes = 0;
        dispatch_count = 0;
        fence_value = 0;
    }

    Status WaitForGpu() {
        const uint64_t target = ++fence_value;
        HRESULT hr = queue->Signal(fence.Get(), target);
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "FSR GPU Signal failed");
        }
        if (fence->GetCompletedValue() < target) {
            hr = fence->SetEventOnCompletion(target, fence_event);
            if (FAILED(hr)) {
                return Status::Error(StatusCode::DeviceError, "FSR GPU SetEventOnCompletion failed");
            }
            const DWORD wr = WaitForSingleObject(fence_event, 15000);
            if (wr != WAIT_OBJECT_0) {
                return Status::Error(StatusCode::DeviceError, "FSR GPU fence wait timed out");
            }
        }
        return Status::Ok();
    }

    Status InitializeComputeObjects() {
        D3D12_COMMAND_QUEUE_DESC qdesc{};
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        HRESULT hr = device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(queue.Put()));
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "FSR CreateCommandQueue failed");
        }

        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.Put()));
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "FSR CreateCommandAllocator failed");
        }

        hr = device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(cmd_list.Put()));
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "FSR CreateCommandList failed");
        }
        cmd_list->Close();

        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.Put()));
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "FSR CreateFence failed");
        }
        fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event) {
            return Status::Error(StatusCode::DeviceError, "FSR CreateEventA failed");
        }

        // Compile EASU and RCAS cs_5_0 shaders live via D3DCompile
        ComPtr<ID3DBlob> easu_blob;
        Status st = CompileComputeShader(kEasuHlsl, "scarlettx_fsr1_easu.hlsl", "EasuMain", easu_blob);
        if (!st.ok()) return st;

        ComPtr<ID3DBlob> rcas_blob;
        st = CompileComputeShader(kRcasHlsl, "scarlettx_fsr1_rcas.hlsl", "RcasMain", rcas_blob);
        if (!st.ok()) return st;

        easu_dxbc_bytes = easu_blob->GetBufferSize();
        rcas_dxbc_bytes = rcas_blob->GetBufferSize();

        // Shared Compute Root Signature:
        //   [0] 8 x 32-bit Root Constants (b0)
        //   [1] Descriptor Table: 1 SRV (t0)
        //   [2] Descriptor Table: 1 UAV (u0)
        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 1;
        srv_range.BaseShaderRegister = 0;
        srv_range.RegisterSpace = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE uav_range{};
        uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav_range.NumDescriptors = 1;
        uav_range.BaseShaderRegister = 0;
        uav_range.RegisterSpace = 0;
        uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.RegisterSpace = 0;
        params[0].Constants.Num32BitValues = 8;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srv_range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &uav_range;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 3;
        rs_desc.pParameters = params;
        rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> sig_blob;
        ComPtr<ID3DBlob> err_blob;
        hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.Put(), err_blob.Put());
        if (FAILED(hr) || !sig_blob) {
            return Status::Error(StatusCode::DeviceError, "FSR D3D12SerializeRootSignature failed");
        }

        hr = device->CreateRootSignature(
            0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(), IID_PPV_ARGS(root_sig.Put()));
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "FSR CreateRootSignature failed");
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC easu_pso_desc{};
        easu_pso_desc.pRootSignature = root_sig.Get();
        easu_pso_desc.CS = {easu_blob->GetBufferPointer(), easu_blob->GetBufferSize()};
        hr = device->CreateComputePipelineState(&easu_pso_desc, IID_PPV_ARGS(easu_pso.Put()));
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "FSR CreateComputePipelineState(EASU) failed");
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC rcas_pso_desc{};
        rcas_pso_desc.pRootSignature = root_sig.Get();
        rcas_pso_desc.CS = {rcas_blob->GetBufferPointer(), rcas_blob->GetBufferSize()};
        hr = device->CreateComputePipelineState(&rcas_pso_desc, IID_PPV_ARGS(rcas_pso.Put()));
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "FSR CreateComputePipelineState(RCAS) failed");
        }

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.NumDescriptors = 4;  // [0]=In SRV, [1]=Mid UAV, [2]=Mid SRV, [3]=Out UAV
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(srv_uav_heap.Put()));
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "FSR CreateDescriptorHeap failed");
        }
        descriptor_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        return Status::Ok();
    }

    Status EnsureSurfaces(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h) {
        if (cached_in_w == in_w && cached_in_h == in_h &&
            cached_out_w == out_w && cached_out_h == out_h &&
            input_tex && upload_buf && intermediate_tex && output_tex && readback_buf) {
            return Status::Ok();
        }

        ReleaseSurfaces();

        D3D12_HEAP_PROPERTIES default_heap{};
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_HEAP_PROPERTIES upload_heap{};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_HEAP_PROPERTIES readback_heap{};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;

        // 1. Input RGBA8 texture (DEFAULT heap, starts in COPY_DEST)
        D3D12_RESOURCE_DESC in_desc{};
        in_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        in_desc.Width = in_w;
        in_desc.Height = in_h;
        in_desc.DepthOrArraySize = 1;
        in_desc.MipLevels = 1;
        in_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        in_desc.SampleDesc.Count = 1;
        in_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        in_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        HRESULT hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &in_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(input_tex.Put()));
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "FSR CreateCommittedResource(input_tex) failed");
        }

        // 2. Upload staging buffer
        upload_row_pitch = AlignUp(in_w * 4u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        upload_size = static_cast<UINT64>(upload_row_pitch) * in_h;

        D3D12_RESOURCE_DESC up_desc{};
        up_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        up_desc.Width = upload_size;
        up_desc.Height = 1;
        up_desc.DepthOrArraySize = 1;
        up_desc.MipLevels = 1;
        up_desc.Format = DXGI_FORMAT_UNKNOWN;
        up_desc.SampleDesc.Count = 1;
        up_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &up_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(upload_buf.Put()));
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "FSR CreateCommittedResource(upload_buf) failed");
        }

        // 3. Intermediate EASU -> RCAS texture (R16G16B16A16_FLOAT preserves full precision between passes)
        D3D12_RESOURCE_DESC mid_desc{};
        mid_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        mid_desc.Width = out_w;
        mid_desc.Height = out_h;
        mid_desc.DepthOrArraySize = 1;
        mid_desc.MipLevels = 1;
        mid_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        mid_desc.SampleDesc.Count = 1;
        mid_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        mid_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &mid_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(intermediate_tex.Put()));
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "FSR CreateCommittedResource(intermediate_tex) failed");
        }

        // 4. Final output RGBA8 texture (starts in UNORDERED_ACCESS)
        D3D12_RESOURCE_DESC out_desc{};
        out_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        out_desc.Width = out_w;
        out_desc.Height = out_h;
        out_desc.DepthOrArraySize = 1;
        out_desc.MipLevels = 1;
        out_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        out_desc.SampleDesc.Count = 1;
        out_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        out_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &out_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(output_tex.Put()));
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "FSR CreateCommittedResource(output_tex) failed");
        }

        // 5. Readback staging buffer
        readback_row_pitch = AlignUp(out_w * 4u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        readback_size = static_cast<UINT64>(readback_row_pitch) * out_h;

        D3D12_RESOURCE_DESC rb_desc{};
        rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rb_desc.Width = readback_size;
        rb_desc.Height = 1;
        rb_desc.DepthOrArraySize = 1;
        rb_desc.MipLevels = 1;
        rb_desc.Format = DXGI_FORMAT_UNKNOWN;
        rb_desc.SampleDesc.Count = 1;
        rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback_buf.Put()));
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "FSR CreateCommittedResource(readback_buf) failed");
        }

        // Populate descriptor heap:
        //   Slot 0: SRV input_tex
        //   Slot 1: UAV intermediate_tex
        //   Slot 2: SRV intermediate_tex
        //   Slot 3: UAV output_tex
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_base = srv_uav_heap->GetCPUDescriptorHandleForHeapStart();

        D3D12_SHADER_RESOURCE_VIEW_DESC in_srv{};
        in_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        in_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        in_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        in_srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(input_tex.Get(), &in_srv, cpu_base);

        D3D12_UNORDERED_ACCESS_VIEW_DESC mid_uav{};
        mid_uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        mid_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE slot1 = {cpu_base.ptr + static_cast<SIZE_T>(descriptor_inc) * 1u};
        device->CreateUnorderedAccessView(intermediate_tex.Get(), nullptr, &mid_uav, slot1);

        D3D12_SHADER_RESOURCE_VIEW_DESC mid_srv{};
        mid_srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        mid_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        mid_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        mid_srv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE slot2 = {cpu_base.ptr + static_cast<SIZE_T>(descriptor_inc) * 2u};
        device->CreateShaderResourceView(intermediate_tex.Get(), &mid_srv, slot2);

        D3D12_UNORDERED_ACCESS_VIEW_DESC out_uav{};
        out_uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        out_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE slot3 = {cpu_base.ptr + static_cast<SIZE_T>(descriptor_inc) * 3u};
        device->CreateUnorderedAccessView(output_tex.Get(), nullptr, &out_uav, slot3);

        cached_in_w = in_w;
        cached_in_h = in_h;
        cached_out_w = out_w;
        cached_out_h = out_h;
        return Status::Ok();
    }
};

FfxGpuFsrPipeline::FfxGpuFsrPipeline() : impl_(std::make_unique<Impl>()) {}
FfxGpuFsrPipeline::~FfxGpuFsrPipeline() = default;

bool FfxGpuFsrPipeline::IsInitialized() const noexcept {
    return impl_ && impl_->device && impl_->easu_pso && impl_->rcas_pso;
}

Status FfxGpuFsrPipeline::Initialize(bool prefer_warp, bool enable_debug_layer) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->Cleanup();
    adapter_info_ = D3D12AdapterInfo{};

    if (enable_debug_layer) {
        ComPtr<ID3D12Debug> debug_ctrl;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug_ctrl.Put()))) && debug_ctrl) {
            debug_ctrl->EnableDebugLayer();
        }
    }

    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(impl_->factory.Put()));
    if (FAILED(hr)) {
        return Status::Error(StatusCode::DeviceError, "FSR CreateDXGIFactory1 failed");
    }

    auto try_create = [&](IDXGIAdapter1* candidate) -> bool {
        if (!candidate) return false;
        ComPtr<ID3D12Device> dev;
        if (SUCCEEDED(D3D12CreateDevice(candidate, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(dev.Put()))) ||
            SUCCEEDED(D3D12CreateDevice(candidate, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(dev.Put())))) {
            impl_->device.Reset(dev.Get());
            impl_->device->AddRef();
            impl_->adapter.Reset(candidate);
            impl_->adapter->AddRef();
            return true;
        }
        return false;
    };

    if (prefer_warp) {
        ComPtr<IDXGIAdapter1> warp;
        if (SUCCEEDED(impl_->factory->EnumWarpAdapter(IID_PPV_ARGS(warp.Put()))) && warp) {
            try_create(warp.Get());
        }
    }

    if (!impl_->device) {
        for (UINT idx = 0;; ++idx) {
            ComPtr<IDXGIAdapter1> hw;
            if (impl_->factory->EnumAdapters1(idx, hw.Put()) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            hw->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && try_create(hw.Get())) {
                break;
            }
        }
    }

    if (!impl_->device) {
        ComPtr<IDXGIAdapter1> warp;
        if (SUCCEEDED(impl_->factory->EnumWarpAdapter(IID_PPV_ARGS(warp.Put()))) && warp) {
            try_create(warp.Get());
        }
    }

    if (!impl_->device) {
        impl_->Cleanup();
        return Status::Error(StatusCode::DeviceError, "FSR failed to create D3D12 device");
    }

    if (impl_->adapter) {
        DXGI_ADAPTER_DESC1 adesc{};
        if (SUCCEEDED(impl_->adapter->GetDesc1(&adesc))) {
            adapter_info_.description = WideToUtf8(adesc.Description);
            adapter_info_.is_warp = (adesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            adapter_info_.vendor_id = adesc.VendorId;
            adapter_info_.device_id = adesc.DeviceId;
        }
    }

    Status st = impl_->InitializeComputeObjects();
    if (!st.ok()) {
        impl_->Cleanup();
        adapter_info_ = D3D12AdapterInfo{};
        return st;
    }

    return Status::Ok();
}

Status FfxGpuFsrPipeline::InitializeWithDevice(void* native_d3d12_device) {
    if (!native_d3d12_device) {
        return Status::Error(StatusCode::InvalidArgument, "FSR native D3D12 device pointer is null");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->Cleanup();
    adapter_info_ = D3D12AdapterInfo{};
    adapter_info_.description = "External ID3D12Device";

    auto* dev = static_cast<ID3D12Device*>(native_d3d12_device);
    dev->AddRef();
    impl_->device.Reset(dev);

    Status st = impl_->InitializeComputeObjects();
    if (!st.ok()) {
        impl_->Cleanup();
        adapter_info_ = D3D12AdapterInfo{};
        return st;
    }
    return Status::Ok();
}

void FfxGpuFsrPipeline::Shutdown() {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->Cleanup();
    }
    adapter_info_ = D3D12AdapterInfo{};
}

Status FfxGpuFsrPipeline::Dispatch(
    const Fsr1DispatchParams& params,
    FrameBufferRgba8& out_frame,
    Fsr1DispatchStats* out_stats) {
    if (!params.input_rgba8 ||
        params.input_width == 0 || params.input_height == 0 ||
        params.output_width == 0 || params.output_height == 0 ||
        params.input_width > kFsrMaxDimension || params.input_height > kFsrMaxDimension ||
        params.output_width > kFsrMaxDimension || params.output_height > kFsrMaxDimension) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid FSR1 dispatch dimensions or input pointer");
    }
    if (!std::isfinite(params.sharpness)) {
        return Status::Error(StatusCode::InvalidArgument, "FSR1 sharpness must be finite");
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->device || !impl_->easu_pso || !impl_->rcas_pso) {
        return Status::Error(StatusCode::DeviceError, "FfxGpuFsrPipeline is not initialized");
    }

    Status st = impl_->EnsureSurfaces(
        params.input_width, params.input_height, params.output_width, params.output_height);
    if (!st.ok()) {
        return st;
    }

    // Upload RGBA8 input rows into 256-byte row-aligned upload buffer
    {
        void* mapped = nullptr;
        D3D12_RANGE read_none{0, 0};
        HRESULT hr = impl_->upload_buf->Map(0, &read_none, &mapped);
        if (FAILED(hr) || !mapped) {
            return Status::Error(StatusCode::DeviceError, "FSR upload buffer Map failed");
        }
        auto* dst_bytes = static_cast<uint8_t*>(mapped);
        const size_t row_bytes = static_cast<size_t>(params.input_width) * 4u;
        for (uint32_t y = 0; y < params.input_height; ++y) {
            std::memcpy(
                dst_bytes + static_cast<size_t>(y) * impl_->upload_row_pitch,
                params.input_rgba8 + static_cast<size_t>(y) * row_bytes,
                row_bytes);
        }
        impl_->upload_buf->Unmap(0, nullptr);
    }

    HRESULT hr = impl_->allocator->Reset();
    if (FAILED(hr)) {
        return Status::Error(StatusCode::DeviceError, "FSR CommandAllocator Reset failed");
    }
    hr = impl_->cmd_list->Reset(impl_->allocator.Get(), impl_->easu_pso.Get());
    if (FAILED(hr)) {
        return Status::Error(StatusCode::DeviceError, "FSR CommandList Reset failed");
    }

    // 1. Copy upload buffer -> input_tex
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = impl_->upload_buf.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint.Offset = 0;
    src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src_loc.PlacedFootprint.Footprint.Width = params.input_width;
    src_loc.PlacedFootprint.Footprint.Height = params.input_height;
    src_loc.PlacedFootprint.Footprint.Depth = 1;
    src_loc.PlacedFootprint.Footprint.RowPitch = impl_->upload_row_pitch;

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = impl_->input_tex.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;

    impl_->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    D3D12_RESOURCE_BARRIER in_to_srv{};
    in_to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    in_to_srv.Transition.pResource = impl_->input_tex.Get();
    in_to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    in_to_srv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    in_to_srv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    impl_->cmd_list->ResourceBarrier(1, &in_to_srv);

    // 2. Dispatch Pass 1: EASU
    FsrRootConstants constants{};
    constants.srcWidth = params.input_width;
    constants.srcHeight = params.input_height;
    constants.dstWidth = params.output_width;
    constants.dstHeight = params.output_height;
    constants.rcpDstWidth = 1.0f / static_cast<float>(params.output_width);
    constants.rcpDstHeight = 1.0f / static_cast<float>(params.output_height);
    constants.sharpness = std::clamp(params.sharpness, 0.0f, 1.0f);
    constants.enableRcas = params.enable_rcas ? 1.0f : 0.0f;

    ID3D12DescriptorHeap* heaps[] = {impl_->srv_uav_heap.Get()};
    impl_->cmd_list->SetDescriptorHeaps(1, heaps);
    impl_->cmd_list->SetComputeRootSignature(impl_->root_sig.Get());

    const D3D12_GPU_DESCRIPTOR_HANDLE gpu_base = impl_->srv_uav_heap->GetGPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot0 = gpu_base;
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot1 = {gpu_base.ptr + static_cast<UINT64>(impl_->descriptor_inc) * 1u};
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot2 = {gpu_base.ptr + static_cast<UINT64>(impl_->descriptor_inc) * 2u};
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot3 = {gpu_base.ptr + static_cast<UINT64>(impl_->descriptor_inc) * 3u};

    const uint32_t groups_x = (params.output_width + kFsrThreadGroupSize - 1u) / kFsrThreadGroupSize;
    const uint32_t groups_y = (params.output_height + kFsrThreadGroupSize - 1u) / kFsrThreadGroupSize;

    impl_->cmd_list->SetPipelineState(impl_->easu_pso.Get());
    impl_->cmd_list->SetComputeRoot32BitConstants(0, 8, &constants, 0);
    impl_->cmd_list->SetComputeRootDescriptorTable(1, gpu_slot0);
    impl_->cmd_list->SetComputeRootDescriptorTable(2, gpu_slot1);
    impl_->cmd_list->Dispatch(groups_x, groups_y, 1);

    // 3. Explicit UAV barrier + Transition intermediate_tex UAV -> SRV for Pass 2 (RCAS)
    D3D12_RESOURCE_BARRIER mid_barriers[2]{};
    mid_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    mid_barriers[0].UAV.pResource = impl_->intermediate_tex.Get();
    mid_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    mid_barriers[1].Transition.pResource = impl_->intermediate_tex.Get();
    mid_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    mid_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    mid_barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    impl_->cmd_list->ResourceBarrier(2, mid_barriers);

    // 4. Dispatch Pass 2: RCAS
    impl_->cmd_list->SetPipelineState(impl_->rcas_pso.Get());
    impl_->cmd_list->SetComputeRoot32BitConstants(0, 8, &constants, 0);
    impl_->cmd_list->SetComputeRootDescriptorTable(1, gpu_slot2);
    impl_->cmd_list->SetComputeRootDescriptorTable(2, gpu_slot3);
    impl_->cmd_list->Dispatch(groups_x, groups_y, 1);

    // 5. UAV barrier + Transition output_tex UAV -> COPY_SOURCE
    D3D12_RESOURCE_BARRIER out_barriers[2]{};
    out_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    out_barriers[0].UAV.pResource = impl_->output_tex.Get();
    out_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    out_barriers[1].Transition.pResource = impl_->output_tex.Get();
    out_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    out_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    out_barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    impl_->cmd_list->ResourceBarrier(2, out_barriers);

    // 6. Copy output_tex -> readback buffer
    D3D12_TEXTURE_COPY_LOCATION rb_src{};
    rb_src.pResource = impl_->output_tex.Get();
    rb_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    rb_src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION rb_dst{};
    rb_dst.pResource = impl_->readback_buf.Get();
    rb_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    rb_dst.PlacedFootprint.Offset = 0;
    rb_dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rb_dst.PlacedFootprint.Footprint.Width = params.output_width;
    rb_dst.PlacedFootprint.Footprint.Height = params.output_height;
    rb_dst.PlacedFootprint.Footprint.Depth = 1;
    rb_dst.PlacedFootprint.Footprint.RowPitch = impl_->readback_row_pitch;

    impl_->cmd_list->CopyTextureRegion(&rb_dst, 0, 0, 0, &rb_src, nullptr);

    // 7. Restore cached textures to initial states for subsequent dispatches
    D3D12_RESOURCE_BARRIER restore[3]{};
    restore[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restore[0].Transition.pResource = impl_->input_tex.Get();
    restore[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    restore[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    restore[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    restore[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restore[1].Transition.pResource = impl_->intermediate_tex.Get();
    restore[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    restore[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    restore[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    restore[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restore[2].Transition.pResource = impl_->output_tex.Get();
    restore[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    restore[2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    restore[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    impl_->cmd_list->ResourceBarrier(3, restore);

    hr = impl_->cmd_list->Close();
    if (FAILED(hr)) {
        return Status::Error(StatusCode::DeviceError, "FSR CommandList Close failed");
    }

    ID3D12CommandList* lists[] = {impl_->cmd_list.Get()};
    impl_->queue->ExecuteCommandLists(1, lists);

    st = impl_->WaitForGpu();
    if (!st.ok()) {
        return st;
    }

    // 8. Map readback buffer and unpack RGBA8 rows
    void* rb_mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(impl_->readback_size)};
    hr = impl_->readback_buf->Map(0, &read_range, &rb_mapped);
    if (FAILED(hr) || !rb_mapped) {
        return Status::Error(StatusCode::DeviceError, "FSR readback buffer Map failed");
    }

    out_frame.width = params.output_width;
    out_frame.height = params.output_height;
    out_frame.pixels.resize(static_cast<size_t>(params.output_width) * params.output_height * 4u);

    const auto* rb_bytes = static_cast<const uint8_t*>(rb_mapped);
    const size_t out_row_bytes = static_cast<size_t>(params.output_width) * 4u;
    for (uint32_t y = 0; y < params.output_height; ++y) {
        std::memcpy(
            out_frame.pixels.data() + static_cast<size_t>(y) * out_row_bytes,
            rb_bytes + static_cast<size_t>(y) * impl_->readback_row_pitch,
            out_row_bytes);
    }

    D3D12_RANGE write_none{0, 0};
    impl_->readback_buf->Unmap(0, &write_none);

    ++impl_->dispatch_count;

    if (out_stats) {
        out_stats->dispatch_count = impl_->dispatch_count;
        out_stats->output_fnv1a_hash = out_frame.ComputeFnv1aHash();
        out_stats->easu_groups_x = groups_x;
        out_stats->easu_groups_y = groups_y;
        out_stats->rcas_groups_x = groups_x;
        out_stats->rcas_groups_y = groups_y;
        out_stats->easu_dxbc_bytes = impl_->easu_dxbc_bytes;
        out_stats->rcas_dxbc_bytes = impl_->rcas_dxbc_bytes;
        out_stats->used_gpu_compute = true;
        out_stats->adapter_description = adapter_info_.description;
    }

    return Status::Ok();
}

#else

struct FfxGpuFsrPipeline::Impl {};
FfxGpuFsrPipeline::FfxGpuFsrPipeline() = default;
FfxGpuFsrPipeline::~FfxGpuFsrPipeline() = default;
bool FfxGpuFsrPipeline::IsInitialized() const noexcept { return false; }
Status FfxGpuFsrPipeline::Initialize(bool, bool) {
    return Status::Error(StatusCode::NotImplemented, "FfxGpuFsrPipeline requires Windows D3D12");
}
Status FfxGpuFsrPipeline::InitializeWithDevice(void*) {
    return Status::Error(StatusCode::NotImplemented, "FfxGpuFsrPipeline requires Windows D3D12");
}
void FfxGpuFsrPipeline::Shutdown() {}
Status FfxGpuFsrPipeline::Dispatch(const Fsr1DispatchParams&, FrameBufferRgba8&, Fsr1DispatchStats*) {
    return Status::Error(StatusCode::NotImplemented, "FfxGpuFsrPipeline requires Windows D3D12");
}

#endif

}  // namespace scarlettx
