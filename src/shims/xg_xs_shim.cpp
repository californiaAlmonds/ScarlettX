#define SCARLETTX_BUILDING_XG_XS 1
#include "scarlettx/gpu/xg_xs_abi.h"
#include "scarlettx/gpu/xg_layout.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

bool TraceEnabled() {
    const char* v = std::getenv("SCARLETTX_TRACE");
    return v && v[0] != '\0' && v[0] != '0';
}

uint64_t AlignUp(uint64_t value, uint64_t align) {
    if (align <= 1) return value;
    return (value + align - 1) & ~(align - 1);
}

void ComputeLayout(const XG_TEXTURE2D_DESC& desc, XG_RESOURCE_LAYOUT& out) {
    scarlettx::gpu::xg::ComputeTexture2DLayout(desc, &out);
}

// Vtable of the texture computer. It is reference counted but is not an
// IUnknown: there is no QueryInterface, so the layout query is slot 2. The
// title calls slot 2 with a layout out-pointer right after creating the object.
struct IXGTexture2DComputerVtbl {
    uint32_t (SCARLETTX_XG_CALL* AddRef)(void* self);
    uint32_t (SCARLETTX_XG_CALL* Release)(void* self);
    int32_t (SCARLETTX_XG_CALL* GetResourceLayout)(void* self, XG_RESOURCE_LAYOUT* layout);
    uint64_t (SCARLETTX_XG_CALL* GetResourceSizeBytes)(void* self);
    uint64_t (SCARLETTX_XG_CALL* GetResourceBaseAlignmentBytes)(void* self);
    uint64_t (SCARLETTX_XG_CALL* GetMipLevelOffsetBytes)(void* self, uint32_t plane, uint32_t miplevel);
    uint64_t (SCARLETTX_XG_CALL* GetTexelElementOffsetBytes)(
        void* self, uint32_t plane, uint32_t miplevel, uint64_t x, uint32_t y, uint32_t zOrSlice, uint32_t sample);
    int32_t (SCARLETTX_XG_CALL* CopyIntoSubresource)(
        void* self, void* dst, uint32_t plane, uint32_t subresource, const void* src, uint32_t srcRowPitch, uint32_t srcDepthPitch);
    int32_t (SCARLETTX_XG_CALL* CopyFromSubresource)(
        void* self, void* dst, uint32_t dstRowPitch, uint32_t dstDepthPitch, const void* src, uint32_t plane, uint32_t subresource);
};

struct XGTexture2DComputerImpl {
    const IXGTexture2DComputerVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    XG_TEXTURE2D_DESC desc{};
    XG_RESOURCE_LAYOUT layout{};
};

uint32_t SCARLETTX_XG_CALL XgAddRef(void* self) {
    return static_cast<XGTexture2DComputerImpl*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t SCARLETTX_XG_CALL XgRelease(void* self) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    const uint32_t remaining = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) {
        delete obj;
    }
    return remaining;
}

int32_t SCARLETTX_XG_CALL XgGetResourceLayout(void* self, XG_RESOURCE_LAYOUT* layout) {
    if (!self || !layout) return static_cast<int32_t>(0x80070057u);
    *layout = static_cast<XGTexture2DComputerImpl*>(self)->layout;
    return 0;
}

uint64_t SCARLETTX_XG_CALL XgGetResourceSizeBytes(void* self) {
    return self ? static_cast<XGTexture2DComputerImpl*>(self)->layout.SizeBytes : 0;
}

uint64_t SCARLETTX_XG_CALL XgGetResourceBaseAlignmentBytes(void* self) {
    return self ? static_cast<XGTexture2DComputerImpl*>(self)->layout.BaseAlignmentBytes : 65536;
}

uint64_t SCARLETTX_XG_CALL XgGetMipLevelOffsetBytes(void* self, uint32_t plane, uint32_t miplevel) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    if (!obj || plane >= obj->layout.Planes || miplevel >= obj->layout.MipLevels) return 0;
    return obj->layout.Plane[plane].MipLayout[miplevel].OffsetBytes;
}

uint64_t SCARLETTX_XG_CALL XgGetTexelElementOffsetBytes(
    void* self, uint32_t plane, uint32_t miplevel, uint64_t x, uint32_t y, uint32_t zOrSlice, uint32_t /*sample*/) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    if (!obj || plane >= obj->layout.Planes || miplevel >= obj->layout.MipLevels) return 0;
    uint64_t offset = 0;
    if (scarlettx::gpu::xg::TexelElementOffset(obj->layout, miplevel, zOrSlice, static_cast<uint32_t>(x), y, &offset)) {
        return offset;
    }
    // Modes without texel addressing: rows inside the mip, slices one mip chain apart.
    const auto& pl = obj->layout.Plane[plane];
    const auto& ml = pl.MipLayout[miplevel];
    return ml.OffsetBytes + static_cast<uint64_t>(zOrSlice) * pl.Reserved10 +
           static_cast<uint64_t>(y) * ml.PitchBytes + x * pl.BytesPerElement;
}

int32_t SCARLETTX_XG_CALL XgCopyIntoSubresource(
    void* self, void* dst, uint32_t plane, uint32_t subresource, const void* src, uint32_t srcRowPitch, uint32_t /*srcDepthPitch*/) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    if (!obj || !dst || !src || plane >= obj->layout.Planes) return static_cast<int32_t>(0x80070057u);
    if (scarlettx::gpu::xg::IsAddressable(obj->layout)) {
        const uint32_t levels = std::max<uint32_t>(1, obj->layout.MipLevels);
        return scarlettx::gpu::xg::CopyIntoSubresource(obj->layout, dst, subresource % levels, subresource / levels, src, srcRowPitch)
                   ? 0
                   : static_cast<int32_t>(0x80070057u);
    }
    const uint32_t mips = std::max<uint32_t>(1, obj->layout.MipLevels);
    const uint32_t mip = subresource % mips;
    const uint32_t slice = subresource / mips;
    const auto& ml = obj->layout.Plane[plane].MipLayout[mip];
    auto* dst_base = static_cast<uint8_t*>(dst) + ml.OffsetBytes +
                     static_cast<uint64_t>(slice) * obj->layout.Plane[plane].Reserved10;
    const auto* src_base = static_cast<const uint8_t*>(src);
    const uint32_t copy_bytes = std::min(srcRowPitch, ml.PitchBytes);
    for (uint32_t row = 0; row < ml.HeightElements; ++row) {
        std::memcpy(dst_base + static_cast<uint64_t>(row) * ml.PitchBytes,
                    src_base + static_cast<uint64_t>(row) * srcRowPitch,
                    copy_bytes);
    }
    return 0;
}

int32_t SCARLETTX_XG_CALL XgCopyFromSubresource(
    void* self, void* dst, uint32_t dstRowPitch, uint32_t /*dstDepthPitch*/, const void* src, uint32_t plane, uint32_t subresource) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    if (!obj || !dst || !src || plane >= obj->layout.Planes) return static_cast<int32_t>(0x80070057u);
    if (scarlettx::gpu::xg::IsAddressable(obj->layout)) {
        const uint32_t levels = std::max<uint32_t>(1, obj->layout.MipLevels);
        return scarlettx::gpu::xg::CopyFromSubresource(obj->layout, src, subresource % levels, subresource / levels, dst, dstRowPitch)
                   ? 0
                   : static_cast<int32_t>(0x80070057u);
    }
    const uint32_t mips = std::max<uint32_t>(1, obj->layout.MipLevels);
    const uint32_t mip = subresource % mips;
    const uint32_t slice = subresource / mips;
    const auto& ml = obj->layout.Plane[plane].MipLayout[mip];
    const auto* src_base = static_cast<const uint8_t*>(src) + ml.OffsetBytes +
                           static_cast<uint64_t>(slice) * obj->layout.Plane[plane].Reserved10;
    auto* dst_base = static_cast<uint8_t*>(dst);
    const uint32_t copy_bytes = std::min(dstRowPitch, ml.PitchBytes);
    for (uint32_t row = 0; row < ml.HeightElements; ++row) {
        std::memcpy(dst_base + static_cast<uint64_t>(row) * dstRowPitch,
                    src_base + static_cast<uint64_t>(row) * ml.PitchBytes,
                    copy_bytes);
    }
    return 0;
}

const IXGTexture2DComputerVtbl kVtbl = {
    &XgAddRef,
    &XgRelease,
    &XgGetResourceLayout,
    &XgGetResourceSizeBytes,
    &XgGetResourceBaseAlignmentBytes,
    &XgGetMipLevelOffsetBytes,
    &XgGetTexelElementOffsetBytes,
    &XgCopyIntoSubresource,
    &XgCopyFromSubresource,
};

uint32_t AlignUp32(uint32_t value, uint32_t align) {
    if (align <= 1) return value;
    return (value + align - 1u) / align * align;
}

uint32_t ComputeFullMipCount(uint32_t max_dim) {
    uint32_t count = 1;
    uint32_t d = std::max<uint32_t>(1u, max_dim);
    while (d > 1u && count < 15u) {
        d >>= 1u;
        ++count;
    }
    return count;
}

bool Is64KbSwizzleMode(uint32_t mode) {
    return (mode >= XG_SWIZZLE_MODE_64KB_Z && mode <= XG_SWIZZLE_MODE_64KB_R_T) ||
           (mode >= XG_SWIZZLE_MODE_64KB_Z_X && mode <= XG_SWIZZLE_MODE_64KB_R_X);
}

bool Is4KbSwizzleMode(uint32_t mode) {
    return (mode >= XG_SWIZZLE_MODE_4KB_Z && mode <= XG_SWIZZLE_MODE_4KB_R) ||
           (mode >= XG_SWIZZLE_MODE_4KB_Z_X && mode <= XG_SWIZZLE_MODE_4KB_R_X);
}

// ---------------------------------------------------------------------------
// XGTexture1DComputer
// ---------------------------------------------------------------------------
struct IXGTexture1DComputerVtbl {
    uint32_t (SCARLETTX_XG_CALL* AddRef)(void* self);
    uint32_t (SCARLETTX_XG_CALL* Release)(void* self);
    int32_t (SCARLETTX_XG_CALL* GetResourceLayout)(void* self, XG_RESOURCE_LAYOUT* layout);
    uint64_t (SCARLETTX_XG_CALL* GetResourceSizeBytes)(void* self);
    uint64_t (SCARLETTX_XG_CALL* GetResourceBaseAlignmentBytes)(void* self);
    uint64_t (SCARLETTX_XG_CALL* GetMipLevelOffsetBytes)(void* self, uint32_t plane, uint32_t miplevel);
    uint64_t (SCARLETTX_XG_CALL* GetTexelElementOffsetBytes)(
        void* self, uint32_t plane, uint32_t miplevel, uint64_t x, uint32_t arraySlice);
    int32_t (SCARLETTX_XG_CALL* CopyIntoSubresource)(
        void* self, void* dst, uint32_t plane, uint32_t subresource, const void* src);
    int32_t (SCARLETTX_XG_CALL* CopyFromSubresource)(
        void* self, void* dst, const void* src, uint32_t plane, uint32_t subresource);
};

struct XGTexture1DComputerImpl {
    const IXGTexture1DComputerVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    XG_TEXTURE1D_DESC desc{};
    XG_RESOURCE_LAYOUT layout{};
};

void ComputeTexture1DLayout(const XG_TEXTURE1D_DESC& desc, XG_RESOURCE_LAYOUT& out) {
    std::memset(&out, 0, sizeof(out));
    const uint32_t width = std::max<uint32_t>(1u, desc.Width);
    const uint32_t array_size = std::max<uint32_t>(1u, desc.ArraySize);
    const uint32_t mips = desc.MipLevels == 0
                              ? ComputeFullMipCount(width)
                              : std::clamp<uint32_t>(desc.MipLevels, 1u, 15u);
    const uint32_t bpe = scarlettx::gpu::xg::FormatBytesPerElement(desc.Format);
    const bool bc = scarlettx::gpu::xg::IsBlockCompressed(desc.Format);

    const uint64_t base_align = Is64KbSwizzleMode(desc.SwizzleMode) ? 65536ull : 4096ull;
    const uint64_t mip_align =
        Is64KbSwizzleMode(desc.SwizzleMode) ? 65536ull
                                            : (Is4KbSwizzleMode(desc.SwizzleMode) ? 4096ull : 256ull);

    out.Dimension = 2;  // D3D12_RESOURCE_DIMENSION_TEXTURE1D
    out.MipLevels = mips;
    out.Planes = 1;
    out.BaseAlignmentBytes = base_align;

    XG_PLANE_LAYOUT& plane = out.Plane[0];
    plane.Usage = 1;
    plane.BaseAlignmentBytes = base_align;
    plane.BytesPerElement = bpe;
    plane.Reserved2C = mips;
    plane.Reserved30 = 0;

    uint64_t offset = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = std::max<uint32_t>(1u, width >> m);
        const uint32_t we = bc ? std::max<uint32_t>(1u, (mw + 3u) / 4u) : mw;
        uint32_t pitch_bytes = AlignUp32(we * bpe, 256u);
        if (m == 0 && desc.Pitch > pitch_bytes) {
            pitch_bytes = AlignUp32(desc.Pitch, 256u);
        }
        const uint32_t padded_we = pitch_bytes / std::max<uint32_t>(1u, bpe);
        const uint64_t mip_size = AlignUp(pitch_bytes, mip_align);

        XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[m];
        ml.WidthElements = we;
        ml.HeightElements = 1u;
        ml.ReservedWidth = mw;
        ml.ReservedHeight = 1u;
        ml.PaddedWidthElements = padded_we;
        ml.PaddedHeightElements = 1u;
        ml.DepthOrArraySize = array_size;
        ml.PaddedDepthOrArraySize = array_size;
        ml.SampleCount = 1u;
        ml.PitchBytes = pitch_bytes;
        ml.PitchPixels = padded_we * (bc ? 4u : 1u);
        ml.AlignmentBytes = static_cast<uint32_t>(mip_align);
        ml.SwizzleMode = desc.SwizzleMode;
        ml.SizeBytes = mip_size;
        ml.Slice2DSizeBytes = mip_size;
        ml.OffsetBytes = offset;
        ml.Reserved60 = 1u;
        offset += mip_size;
    }

    const uint64_t slice_size = AlignUp(offset, base_align);
    plane.Reserved10 = slice_size;
    plane.SizeBytes = slice_size * array_size;
    out.SizeBytes = AlignUp(plane.SizeBytes, base_align);
}

uint32_t SCARLETTX_XG_CALL Xg1DAddRef(void* self) {
    return static_cast<XGTexture1DComputerImpl*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t SCARLETTX_XG_CALL Xg1DRelease(void* self) {
    auto* obj = static_cast<XGTexture1DComputerImpl*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete obj;
    return rem;
}

int32_t SCARLETTX_XG_CALL Xg1DGetResourceLayout(void* self, XG_RESOURCE_LAYOUT* layout) {
    if (!self || !layout) return static_cast<int32_t>(0x80070057u);
    *layout = static_cast<XGTexture1DComputerImpl*>(self)->layout;
    return 0;
}

uint64_t SCARLETTX_XG_CALL Xg1DGetResourceSizeBytes(void* self) {
    return self ? static_cast<XGTexture1DComputerImpl*>(self)->layout.SizeBytes : 0;
}

uint64_t SCARLETTX_XG_CALL Xg1DGetResourceBaseAlignmentBytes(void* self) {
    return self ? static_cast<XGTexture1DComputerImpl*>(self)->layout.BaseAlignmentBytes : 4096;
}

uint64_t SCARLETTX_XG_CALL Xg1DGetMipLevelOffsetBytes(void* self, uint32_t plane, uint32_t miplevel) {
    auto* obj = static_cast<XGTexture1DComputerImpl*>(self);
    if (!obj || plane >= obj->layout.Planes || miplevel >= obj->layout.MipLevels) return 0;
    return obj->layout.Plane[plane].MipLayout[miplevel].OffsetBytes;
}

uint64_t SCARLETTX_XG_CALL Xg1DGetTexelElementOffsetBytes(
    void* self, uint32_t plane, uint32_t miplevel, uint64_t x, uint32_t arraySlice) {
    auto* obj = static_cast<XGTexture1DComputerImpl*>(self);
    if (!obj || plane >= obj->layout.Planes || miplevel >= obj->layout.MipLevels) return 0;
    const auto& pl = obj->layout.Plane[plane];
    const auto& ml = pl.MipLayout[miplevel];
    if (arraySlice >= std::max<uint32_t>(1u, ml.DepthOrArraySize) || x >= ml.WidthElements) return 0;
    return ml.OffsetBytes + static_cast<uint64_t>(arraySlice) * pl.Reserved10 +
           x * static_cast<uint64_t>(pl.BytesPerElement);
}

int32_t SCARLETTX_XG_CALL Xg1DCopyIntoSubresource(
    void* self, void* dst, uint32_t plane, uint32_t subresource, const void* src) {
    auto* obj = static_cast<XGTexture1DComputerImpl*>(self);
    if (!obj || !dst || !src || plane >= obj->layout.Planes) return static_cast<int32_t>(0x80070057u);
    const uint32_t mips = std::max<uint32_t>(1u, obj->layout.MipLevels);
    const uint32_t mip = subresource % mips;
    const uint32_t slice = subresource / mips;
    const auto& pl = obj->layout.Plane[plane];
    const auto& ml = pl.MipLayout[mip];
    if (slice >= std::max<uint32_t>(1u, ml.DepthOrArraySize)) return static_cast<int32_t>(0x80070057u);
    auto* dst_base = static_cast<uint8_t*>(dst) + ml.OffsetBytes +
                     static_cast<uint64_t>(slice) * pl.Reserved10;
    std::memcpy(dst_base, src, static_cast<size_t>(ml.WidthElements) * pl.BytesPerElement);
    return 0;
}

int32_t SCARLETTX_XG_CALL Xg1DCopyFromSubresource(
    void* self, void* dst, const void* src, uint32_t plane, uint32_t subresource) {
    auto* obj = static_cast<XGTexture1DComputerImpl*>(self);
    if (!obj || !dst || !src || plane >= obj->layout.Planes) return static_cast<int32_t>(0x80070057u);
    const uint32_t mips = std::max<uint32_t>(1u, obj->layout.MipLevels);
    const uint32_t mip = subresource % mips;
    const uint32_t slice = subresource / mips;
    const auto& pl = obj->layout.Plane[plane];
    const auto& ml = pl.MipLayout[mip];
    if (slice >= std::max<uint32_t>(1u, ml.DepthOrArraySize)) return static_cast<int32_t>(0x80070057u);
    const auto* src_base = static_cast<const uint8_t*>(src) + ml.OffsetBytes +
                           static_cast<uint64_t>(slice) * pl.Reserved10;
    std::memcpy(dst, src_base, static_cast<size_t>(ml.WidthElements) * pl.BytesPerElement);
    return 0;
}

const IXGTexture1DComputerVtbl kVtbl1D = {
    &Xg1DAddRef,
    &Xg1DRelease,
    &Xg1DGetResourceLayout,
    &Xg1DGetResourceSizeBytes,
    &Xg1DGetResourceBaseAlignmentBytes,
    &Xg1DGetMipLevelOffsetBytes,
    &Xg1DGetTexelElementOffsetBytes,
    &Xg1DCopyIntoSubresource,
    &Xg1DCopyFromSubresource,
};

// ---------------------------------------------------------------------------
// XGTexture3DComputer
// ---------------------------------------------------------------------------
struct IXGTexture3DComputerVtbl {
    uint32_t (SCARLETTX_XG_CALL* AddRef)(void* self);
    uint32_t (SCARLETTX_XG_CALL* Release)(void* self);
    int32_t (SCARLETTX_XG_CALL* GetResourceLayout)(void* self, XG_RESOURCE_LAYOUT* layout);
    uint64_t (SCARLETTX_XG_CALL* GetResourceSizeBytes)(void* self);
    uint64_t (SCARLETTX_XG_CALL* GetResourceBaseAlignmentBytes)(void* self);
    uint64_t (SCARLETTX_XG_CALL* GetMipLevelOffsetBytes)(void* self, uint32_t plane, uint32_t miplevel);
    uint64_t (SCARLETTX_XG_CALL* GetTexelElementOffsetBytes)(
        void* self, uint32_t plane, uint32_t miplevel, uint64_t x, uint32_t y, uint32_t z);
    int32_t (SCARLETTX_XG_CALL* CopyIntoSubresource)(
        void* self, void* dst, uint32_t plane, uint32_t subresource, const void* src, uint32_t srcRowPitch, uint32_t srcDepthPitch);
    int32_t (SCARLETTX_XG_CALL* CopyFromSubresource)(
        void* self, void* dst, uint32_t dstRowPitch, uint32_t dstDepthPitch, const void* src, uint32_t plane, uint32_t subresource);
};

struct XGTexture3DComputerImpl {
    const IXGTexture3DComputerVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    XG_TEXTURE3D_DESC desc{};
    XG_RESOURCE_LAYOUT layout{};
};

void Compute3DTileShape(uint32_t swizzle_mode, uint32_t bpe,
                        uint32_t& tile_w, uint32_t& tile_h, uint32_t& tile_d,
                        uint64_t& base_align) {
    if (Is64KbSwizzleMode(swizzle_mode)) {
        base_align = 65536ull;
        switch (bpe) {
            case 1:  tile_w = 64; tile_h = 32; tile_d = 32; break;
            case 2:  tile_w = 32; tile_h = 32; tile_d = 32; break;
            case 4:  tile_w = 32; tile_h = 32; tile_d = 16; break;
            case 8:  tile_w = 32; tile_h = 16; tile_d = 16; break;
            case 16: tile_w = 16; tile_h = 16; tile_d = 16; break;
            default: tile_w = 16; tile_h = 16; tile_d = 16; break;
        }
    } else if (Is4KbSwizzleMode(swizzle_mode)) {
        base_align = 4096ull;
        switch (bpe) {
            case 1:  tile_w = 16; tile_h = 16; tile_d = 16; break;
            case 2:  tile_w = 16; tile_h = 16; tile_d = 8;  break;
            case 4:  tile_w = 16; tile_h = 8;  tile_d = 8;  break;
            case 8:  tile_w = 8;  tile_h = 8;  tile_d = 8;  break;
            case 16: tile_w = 8;  tile_h = 8;  tile_d = 4;  break;
            default: tile_w = 8;  tile_h = 8;  tile_d = 4;  break;
        }
    } else {
        base_align = 4096ull;
        tile_w = std::max<uint32_t>(1u, 256u / std::max<uint32_t>(1u, bpe));
        tile_h = 1u;
        tile_d = 1u;
    }
}

void ComputeTexture3DLayout(const XG_TEXTURE3D_DESC& desc, XG_RESOURCE_LAYOUT& out) {
    std::memset(&out, 0, sizeof(out));
    const uint32_t width = std::max<uint32_t>(1u, desc.Width);
    const uint32_t height = std::max<uint32_t>(1u, desc.Height);
    const uint32_t depth = std::max<uint32_t>(1u, desc.Depth);
    const uint32_t max_dim = std::max(width, std::max(height, depth));
    const uint32_t mips = desc.MipLevels == 0
                              ? ComputeFullMipCount(max_dim)
                              : std::clamp<uint32_t>(desc.MipLevels, 1u, 15u);
    const uint32_t bpe = scarlettx::gpu::xg::FormatBytesPerElement(desc.Format);
    const bool bc = scarlettx::gpu::xg::IsBlockCompressed(desc.Format);

    uint32_t tile_w = 1, tile_h = 1, tile_d = 1;
    uint64_t base_align = 65536ull;
    Compute3DTileShape(desc.SwizzleMode, bpe, tile_w, tile_h, tile_d, base_align);

    out.Dimension = 4;  // D3D12_RESOURCE_DIMENSION_TEXTURE3D
    out.MipLevels = mips;
    out.Planes = 1;
    out.BaseAlignmentBytes = base_align;

    XG_PLANE_LAYOUT& plane = out.Plane[0];
    plane.Usage = 1;
    plane.BaseAlignmentBytes = base_align;
    plane.BytesPerElement = bpe;
    plane.Reserved2C = mips;
    plane.Reserved30 = 0;

    uint64_t offset = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = std::max<uint32_t>(1u, width >> m);
        const uint32_t mh = std::max<uint32_t>(1u, height >> m);
        const uint32_t md = std::max<uint32_t>(1u, depth >> m);
        const uint32_t we = bc ? std::max<uint32_t>(1u, (mw + 3u) / 4u) : mw;
        const uint32_t he = bc ? std::max<uint32_t>(1u, (mh + 3u) / 4u) : mh;

        const uint32_t padded_w = AlignUp32(we, tile_w);
        const uint32_t padded_h = AlignUp32(he, tile_h);
        const uint32_t padded_d = AlignUp32(md, tile_d);
        uint32_t pitch_bytes = AlignUp32(padded_w * bpe, 256u);
        if (m == 0 && desc.Pitch > pitch_bytes) {
            pitch_bytes = AlignUp32(desc.Pitch, 256u);
        }
        const uint32_t eff_padded_w = pitch_bytes / std::max<uint32_t>(1u, bpe);
        const uint64_t slice2d_bytes = static_cast<uint64_t>(pitch_bytes) * padded_h;
        const uint64_t mip_size = AlignUp(slice2d_bytes * padded_d, base_align);

        XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[m];
        ml.WidthElements = we;
        ml.HeightElements = he;
        ml.ReservedWidth = mw;
        ml.ReservedHeight = mh;
        ml.DepthOrArraySize = md;
        ml.PaddedWidthElements = eff_padded_w;
        ml.PaddedHeightElements = padded_h;
        ml.PaddedDepthOrArraySize = padded_d;
        ml.SampleCount = 1u;
        ml.PitchBytes = pitch_bytes;
        ml.PitchPixels = eff_padded_w * (bc ? 4u : 1u);
        ml.AlignmentBytes = static_cast<uint32_t>(base_align);
        ml.SwizzleMode = desc.SwizzleMode;
        ml.Slice2DSizeBytes = slice2d_bytes;
        ml.SizeBytes = mip_size;
        ml.OffsetBytes = offset;
        ml.Reserved60 = 1u;
        offset += mip_size;
    }

    plane.Reserved10 = offset;
    plane.SizeBytes = offset;
    out.SizeBytes = AlignUp(offset, base_align);
}

uint32_t SCARLETTX_XG_CALL Xg3DAddRef(void* self) {
    return static_cast<XGTexture3DComputerImpl*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t SCARLETTX_XG_CALL Xg3DRelease(void* self) {
    auto* obj = static_cast<XGTexture3DComputerImpl*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete obj;
    return rem;
}

int32_t SCARLETTX_XG_CALL Xg3DGetResourceLayout(void* self, XG_RESOURCE_LAYOUT* layout) {
    if (!self || !layout) return static_cast<int32_t>(0x80070057u);
    *layout = static_cast<XGTexture3DComputerImpl*>(self)->layout;
    return 0;
}

uint64_t SCARLETTX_XG_CALL Xg3DGetResourceSizeBytes(void* self) {
    return self ? static_cast<XGTexture3DComputerImpl*>(self)->layout.SizeBytes : 0;
}

uint64_t SCARLETTX_XG_CALL Xg3DGetResourceBaseAlignmentBytes(void* self) {
    return self ? static_cast<XGTexture3DComputerImpl*>(self)->layout.BaseAlignmentBytes : 65536;
}

uint64_t SCARLETTX_XG_CALL Xg3DGetMipLevelOffsetBytes(void* self, uint32_t plane, uint32_t miplevel) {
    auto* obj = static_cast<XGTexture3DComputerImpl*>(self);
    if (!obj || plane >= obj->layout.Planes || miplevel >= obj->layout.MipLevels) return 0;
    return obj->layout.Plane[plane].MipLayout[miplevel].OffsetBytes;
}

uint64_t SCARLETTX_XG_CALL Xg3DGetTexelElementOffsetBytes(
    void* self, uint32_t plane, uint32_t miplevel, uint64_t x, uint32_t y, uint32_t z) {
    auto* obj = static_cast<XGTexture3DComputerImpl*>(self);
    if (!obj || plane >= obj->layout.Planes || miplevel >= obj->layout.MipLevels) return 0;
    const auto& pl = obj->layout.Plane[plane];
    const auto& ml = pl.MipLayout[miplevel];
    if (x >= ml.WidthElements || y >= ml.HeightElements || z >= ml.DepthOrArraySize) return 0;
    return ml.OffsetBytes + static_cast<uint64_t>(z) * ml.Slice2DSizeBytes +
           static_cast<uint64_t>(y) * ml.PitchBytes + x * static_cast<uint64_t>(pl.BytesPerElement);
}

int32_t SCARLETTX_XG_CALL Xg3DCopyIntoSubresource(
    void* self, void* dst, uint32_t plane, uint32_t subresource, const void* src, uint32_t srcRowPitch, uint32_t srcDepthPitch) {
    auto* obj = static_cast<XGTexture3DComputerImpl*>(self);
    if (!obj || !dst || !src || plane >= obj->layout.Planes || subresource >= obj->layout.MipLevels) {
        return static_cast<int32_t>(0x80070057u);
    }
    const auto& pl = obj->layout.Plane[plane];
    const auto& ml = pl.MipLayout[subresource];
    const uint32_t row_bytes = ml.WidthElements * pl.BytesPerElement;
    const uint32_t copy_bytes = std::min(srcRowPitch, std::min(row_bytes, ml.PitchBytes));
    auto* dst_base = static_cast<uint8_t*>(dst) + ml.OffsetBytes;
    const auto* src_base = static_cast<const uint8_t*>(src);
    for (uint32_t z = 0; z < ml.DepthOrArraySize; ++z) {
        auto* dst_slice = dst_base + static_cast<uint64_t>(z) * ml.Slice2DSizeBytes;
        const auto* src_slice = src_base + static_cast<uint64_t>(z) * srcDepthPitch;
        for (uint32_t y = 0; y < ml.HeightElements; ++y) {
            std::memcpy(dst_slice + static_cast<uint64_t>(y) * ml.PitchBytes,
                        src_slice + static_cast<uint64_t>(y) * srcRowPitch,
                        copy_bytes);
        }
    }
    return 0;
}

int32_t SCARLETTX_XG_CALL Xg3DCopyFromSubresource(
    void* self, void* dst, uint32_t dstRowPitch, uint32_t dstDepthPitch, const void* src, uint32_t plane, uint32_t subresource) {
    auto* obj = static_cast<XGTexture3DComputerImpl*>(self);
    if (!obj || !dst || !src || plane >= obj->layout.Planes || subresource >= obj->layout.MipLevels) {
        return static_cast<int32_t>(0x80070057u);
    }
    const auto& pl = obj->layout.Plane[plane];
    const auto& ml = pl.MipLayout[subresource];
    const uint32_t row_bytes = ml.WidthElements * pl.BytesPerElement;
    const uint32_t copy_bytes = std::min(dstRowPitch, std::min(row_bytes, ml.PitchBytes));
    const auto* src_base = static_cast<const uint8_t*>(src) + ml.OffsetBytes;
    auto* dst_base = static_cast<uint8_t*>(dst);
    for (uint32_t z = 0; z < ml.DepthOrArraySize; ++z) {
        const auto* src_slice = src_base + static_cast<uint64_t>(z) * ml.Slice2DSizeBytes;
        auto* dst_slice = dst_base + static_cast<uint64_t>(z) * dstDepthPitch;
        for (uint32_t y = 0; y < ml.HeightElements; ++y) {
            std::memcpy(dst_slice + static_cast<uint64_t>(y) * dstRowPitch,
                        src_slice + static_cast<uint64_t>(y) * ml.PitchBytes,
                        copy_bytes);
        }
    }
    return 0;
}

const IXGTexture3DComputerVtbl kVtbl3D = {
    &Xg3DAddRef,
    &Xg3DRelease,
    &Xg3DGetResourceLayout,
    &Xg3DGetResourceSizeBytes,
    &Xg3DGetResourceBaseAlignmentBytes,
    &Xg3DGetMipLevelOffsetBytes,
    &Xg3DGetTexelElementOffsetBytes,
    &Xg3DCopyIntoSubresource,
    &Xg3DCopyFromSubresource,
};

// ---------------------------------------------------------------------------
// XGBufferComputer
// ---------------------------------------------------------------------------
struct IXGBufferComputerVtbl {
    uint32_t (SCARLETTX_XG_CALL* AddRef)(void* self);
    uint32_t (SCARLETTX_XG_CALL* Release)(void* self);
    int32_t (SCARLETTX_XG_CALL* GetResourceLayout)(void* self, XG_RESOURCE_LAYOUT* layout);
    uint64_t (SCARLETTX_XG_CALL* GetResourceSizeBytes)(void* self);
    uint64_t (SCARLETTX_XG_CALL* GetResourceBaseAlignmentBytes)(void* self);
    uint64_t (SCARLETTX_XG_CALL* GetBufferOffsetBytes)(void* self);
    int32_t (SCARLETTX_XG_CALL* CopyIntoSubresource)(
        void* self, void* dst, const void* src, uint64_t byteOffset, uint64_t byteCount);
    int32_t (SCARLETTX_XG_CALL* CopyFromSubresource)(
        void* self, void* dst, const void* src, uint64_t byteOffset, uint64_t byteCount);
};

struct XGBufferComputerImpl {
    const IXGBufferComputerVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    XG_BUFFER_DESC desc{};
    XG_RESOURCE_LAYOUT layout{};
};

void ComputeBufferLayout(const XG_BUFFER_DESC& desc, XG_RESOURCE_LAYOUT& out) {
    std::memset(&out, 0, sizeof(out));
    const uint64_t byte_width = desc.ByteWidth;
    const uint64_t base_align = Is4KbSwizzleMode(desc.SwizzleMode) ? 4096ull : 65536ull;
    const uint64_t pitch_bytes = AlignUp(byte_width, 256ull);
    const uint64_t aligned_size = AlignUp(byte_width, base_align);
    const uint32_t bpe = desc.StructureByteStride > 0 ? desc.StructureByteStride : 1u;

    out.Dimension = 1;  // D3D12_RESOURCE_DIMENSION_BUFFER
    out.MipLevels = 1;
    out.Planes = 1;
    out.BaseAlignmentBytes = base_align;
    out.SizeBytes = aligned_size;

    XG_PLANE_LAYOUT& plane = out.Plane[0];
    plane.Usage = 1;
    plane.SizeBytes = aligned_size;
    plane.Reserved10 = aligned_size;
    plane.BaseOffsetBytes = 0;
    plane.BaseAlignmentBytes = base_align;
    plane.BytesPerElement = bpe;
    plane.Reserved2C = 1;
    plane.Reserved30 = 0;

    XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[0];
    ml.SizeBytes = aligned_size;
    ml.OffsetBytes = 0;
    ml.Slice2DSizeBytes = aligned_size;
    ml.PitchBytes = static_cast<uint32_t>(std::min<uint64_t>(pitch_bytes, 0xFFFFFFFFull));
    ml.PitchPixels = ml.PitchBytes / bpe;
    ml.AlignmentBytes = static_cast<uint32_t>(base_align);
    ml.WidthElements = static_cast<uint32_t>(byte_width / bpe);
    ml.HeightElements = 1u;
    ml.PaddedWidthElements = static_cast<uint32_t>(pitch_bytes / bpe);
    ml.PaddedHeightElements = 1u;
    ml.DepthOrArraySize = 1u;
    ml.PaddedDepthOrArraySize = 1u;
    ml.ReservedWidth = desc.ByteWidth;
    ml.ReservedHeight = 1u;
    ml.SampleCount = 1u;
    ml.SwizzleMode = XG_SWIZZLE_MODE_LINEAR;
    ml.Reserved60 = 1u;
}

uint32_t SCARLETTX_XG_CALL XgBufAddRef(void* self) {
    return static_cast<XGBufferComputerImpl*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t SCARLETTX_XG_CALL XgBufRelease(void* self) {
    auto* obj = static_cast<XGBufferComputerImpl*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete obj;
    return rem;
}

int32_t SCARLETTX_XG_CALL XgBufGetResourceLayout(void* self, XG_RESOURCE_LAYOUT* layout) {
    if (!self || !layout) return static_cast<int32_t>(0x80070057u);
    *layout = static_cast<XGBufferComputerImpl*>(self)->layout;
    return 0;
}

uint64_t SCARLETTX_XG_CALL XgBufGetResourceSizeBytes(void* self) {
    return self ? static_cast<XGBufferComputerImpl*>(self)->layout.SizeBytes : 0;
}

uint64_t SCARLETTX_XG_CALL XgBufGetResourceBaseAlignmentBytes(void* self) {
    return self ? static_cast<XGBufferComputerImpl*>(self)->layout.BaseAlignmentBytes : 65536;
}

uint64_t SCARLETTX_XG_CALL XgBufGetBufferOffsetBytes(void* /*self*/) {
    return 0;
}

int32_t SCARLETTX_XG_CALL XgBufCopyIntoSubresource(
    void* self, void* dst, const void* src, uint64_t byteOffset, uint64_t byteCount) {
    auto* obj = static_cast<XGBufferComputerImpl*>(self);
    if (!obj || !dst || !src) return static_cast<int32_t>(0x80070057u);
    if (byteOffset > obj->desc.ByteWidth || byteCount > obj->desc.ByteWidth - byteOffset) {
        return static_cast<int32_t>(0x80070057u);
    }
    if (byteCount > 0) {
        std::memcpy(static_cast<uint8_t*>(dst) + byteOffset, src, static_cast<size_t>(byteCount));
    }
    return 0;
}

int32_t SCARLETTX_XG_CALL XgBufCopyFromSubresource(
    void* self, void* dst, const void* src, uint64_t byteOffset, uint64_t byteCount) {
    auto* obj = static_cast<XGBufferComputerImpl*>(self);
    if (!obj || !dst || !src) return static_cast<int32_t>(0x80070057u);
    if (byteOffset > obj->desc.ByteWidth || byteCount > obj->desc.ByteWidth - byteOffset) {
        return static_cast<int32_t>(0x80070057u);
    }
    if (byteCount > 0) {
        std::memcpy(dst, static_cast<const uint8_t*>(src) + byteOffset, static_cast<size_t>(byteCount));
    }
    return 0;
}

const IXGBufferComputerVtbl kVtblBuffer = {
    &XgBufAddRef,
    &XgBufRelease,
    &XgBufGetResourceLayout,
    &XgBufGetResourceSizeBytes,
    &XgBufGetResourceBaseAlignmentBytes,
    &XgBufGetBufferOffsetBytes,
    &XgBufCopyIntoSubresource,
    &XgBufCopyFromSubresource,
};

}  // namespace

extern "C" {

uint32_t SCARLETTX_XG_CALL XGComputeOptimalSwizzleMode(
    uint32_t resourceDimension,
    uint32_t format,
    uint32_t width,
    uint32_t height,
    uint32_t depthOrArraySize,
    uint32_t sampleCount,
    uint32_t bindFlags,
    uint32_t reserved) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][xg_xs] XGComputeOptimalSwizzleMode(dim=" << resourceDimension
                  << ", fmt=" << format << ", " << width << "x" << height
                  << "x" << depthOrArraySize << ", samples=" << sampleCount
                  << ", bind=0x" << std::hex << bindFlags << std::dec << ")\n";
    }
    (void)reserved;  // 0 at every call seen
    // Matches what the console library answers for these bind flags.
    if (bindFlags & 0x40u) return XG_SWIZZLE_MODE_64KB_Z_X;  // depth stencil
    if (bindFlags & 0x20u) return XG_SWIZZLE_MODE_64KB_R_X;  // render target
    return XG_SWIZZLE_MODE_4KB_S;
}

int32_t SCARLETTX_XG_CALL XGCreateTexture2DComputer(
    const XG_TEXTURE2D_DESC* desc,
    void** computer) {
    if (!desc || !computer) {
        return static_cast<int32_t>(0x80070057u);  // E_INVALIDARG
    }
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][xg_xs] XGCreateTexture2DComputer(" << desc->Width << "x"
                  << desc->Height << ", mips=" << desc->MipLevels << ", fmt=" << desc->Format
                  << ", array=" << desc->ArraySize << ")\n";
    }
    auto* obj = new XGTexture2DComputerImpl();
    obj->lpVtbl = &kVtbl;
    obj->desc = *desc;
    ComputeLayout(obj->desc, obj->layout);
    *computer = obj;
    return 0;  // S_OK
}

uint32_t SCARLETTX_XG_CALL XGComputeOptimalTileMode(
    uint32_t resourceDimension,
    uint32_t format,
    uint32_t width,
    uint32_t height,
    uint32_t depthOrArraySize,
    uint32_t sampleCount,
    uint32_t bindFlags) {
    (void)format;
    switch (resourceDimension) {
        case 1:  // Buffer
            return width > 0 ? XG_TILE_MODE_LINEAR : XG_TILE_MODE_INVALID;
        case 2:  // Texture1D
            if (width == 0) return XG_TILE_MODE_INVALID;
            return (bindFlags & 0x20u) ? XG_TILE_MODE_1D_THIN : XG_TILE_MODE_LINEAR;
        case 3:  // Texture2D
            if (width == 0 || height == 0) return XG_TILE_MODE_INVALID;
            if (bindFlags & 0x40u) return XG_TILE_MODE_DEPTH_STENCIL;
            if (bindFlags & 0x20u) {
                return sampleCount > 1u ? XG_TILE_MODE_2D_THIN : XG_TILE_MODE_DISPLAY;
            }
            return (width < 64u && height < 64u) ? XG_TILE_MODE_1D_THIN : XG_TILE_MODE_2D_THIN;
        case 4:  // Texture3D
            if (width == 0 || height == 0 || depthOrArraySize == 0) return XG_TILE_MODE_INVALID;
            return depthOrArraySize >= 4u ? XG_TILE_MODE_2D_THICK : XG_TILE_MODE_1D_THICK;
        default:
            return XG_TILE_MODE_INVALID;
    }
}

int32_t SCARLETTX_XG_CALL XGCreateTexture1DComputer(
    const XG_TEXTURE1D_DESC* desc,
    void** computer) {
    if (!desc || !computer || desc->Width == 0) {
        return static_cast<int32_t>(0x80070057u);
    }
    auto* obj = new XGTexture1DComputerImpl();
    obj->lpVtbl = &kVtbl1D;
    obj->desc = *desc;
    ComputeTexture1DLayout(obj->desc, obj->layout);
    *computer = obj;
    return 0;
}

int32_t SCARLETTX_XG_CALL XGCreateTexture3DComputer(
    const XG_TEXTURE3D_DESC* desc,
    void** computer) {
    if (!desc || !computer || desc->Width == 0 || desc->Height == 0 || desc->Depth == 0) {
        return static_cast<int32_t>(0x80070057u);
    }
    auto* obj = new XGTexture3DComputerImpl();
    obj->lpVtbl = &kVtbl3D;
    obj->desc = *desc;
    ComputeTexture3DLayout(obj->desc, obj->layout);
    *computer = obj;
    return 0;
}

int32_t SCARLETTX_XG_CALL XGCreateBufferComputer(
    const XG_BUFFER_DESC* desc,
    void** computer) {
    if (!desc || !computer || desc->ByteWidth == 0) {
        return static_cast<int32_t>(0x80070057u);
    }
    auto* obj = new XGBufferComputerImpl();
    obj->lpVtbl = &kVtblBuffer;
    obj->desc = *desc;
    ComputeBufferLayout(obj->desc, obj->layout);
    *computer = obj;
    return 0;
}

}  // extern "C"
