// Xbox texture memory layout: struct ABI, swizzled addressing and the xg_xs
// texture computer. Expected numbers are the console library's answers for
// the same inputs.
#include "scarlettx/gpu/xg_layout.h"
#include "scarlettx/gpu/xg_xs_abi.h"
#include "sx_check.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace xg = scarlettx::gpu::xg;

namespace {

XG_TEXTURE2D_DESC Desc(uint32_t w, uint32_t h, uint32_t mips, uint32_t format, uint32_t array = 1) {
    XG_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = mips;
    d.ArraySize = array;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BindFlags = 0x8;
    d.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;
    return d;
}

uint64_t Offset(const XG_RESOURCE_LAYOUT& l, uint32_t mip, uint32_t slice, uint32_t x, uint32_t y) {
    uint64_t off = ~0ull;
    SX_REQUIRE(xg::TexelElementOffset(l, mip, slice, x, y, &off));
    return off;
}

constexpr uint32_t kRgba8 = 28;
constexpr uint32_t kBc1 = 71;
constexpr uint32_t kBc3 = 77;
constexpr uint32_t kR8 = 61;

void TestSingleMip() {
    auto l = std::make_unique<XG_RESOURCE_LAYOUT>();
    xg::ComputeTexture2DLayout(Desc(700, 284, 1, kRgba8), l.get());
    SX_REQUIRE(l->SizeBytes == 0xC6000);
    SX_REQUIRE(l->BaseAlignmentBytes == 0x1000);
    SX_REQUIRE(l->MipLevels == 1 && l->Planes == 1);
    const XG_PLANE_LAYOUT& p = l->Plane[0];
    SX_REQUIRE(p.Usage == 1 && p.SizeBytes == 0xC6000 && p.Reserved10 == 0xC6000);
    SX_REQUIRE(p.BytesPerElement == 4 && p.Reserved2C == 1 && p.Reserved30 == 0);
    const XG_MIPLEVEL_LAYOUT& m = p.MipLayout[0];
    SX_REQUIRE(m.SizeBytes == 0xC6000 && m.OffsetBytes == 0 && m.Slice2DSizeBytes == 0xC6000);
    SX_REQUIRE(m.PitchPixels == 704 && m.PitchBytes == 2816 && m.AlignmentBytes == 0x1000);
    SX_REQUIRE(m.PaddedWidthElements == 704 && m.PaddedHeightElements == 288);
    SX_REQUIRE(m.WidthElements == 700 && m.HeightElements == 284);
    SX_REQUIRE(m.ReservedWidth == 700 && m.ReservedHeight == 284);
    SX_REQUIRE(m.SwizzleMode == XG_SWIZZLE_MODE_4KB_S && m.Reserved60 == 1 && m.Reserved64 == 0);

    // 32x32 texel blocks of 4 KB, stored row by row (22 blocks per row).
    SX_REQUIRE(Offset(*l, 0, 0, 0, 0) == 0);
    SX_REQUIRE(Offset(*l, 0, 0, 1, 0) == 0x4);
    SX_REQUIRE(Offset(*l, 0, 0, 2, 0) == 0x8);
    SX_REQUIRE(Offset(*l, 0, 0, 4, 0) == 0x80);
    SX_REQUIRE(Offset(*l, 0, 0, 8, 0) == 0x200);
    SX_REQUIRE(Offset(*l, 0, 0, 16, 0) == 0x800);
    SX_REQUIRE(Offset(*l, 0, 0, 32, 0) == 0x1000);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 1) == 0x10);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 4) == 0x40);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 8) == 0x100);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 16) == 0x400);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 32) == 0x16000);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 256) == 0xB0000);

    // A single mip is never packed into a tail, however small.
    xg::ComputeTexture2DLayout(Desc(16, 16, 1, kRgba8), l.get());
    SX_REQUIRE(l->SizeBytes == 0x1000 && l->Plane[0].Reserved2C == 1 && l->Plane[0].Reserved30 == 0);
}

void TestMipChain() {
    auto l = std::make_unique<XG_RESOURCE_LAYOUT>();
    xg::ComputeTexture2DLayout(Desc(256, 256, 9, kRgba8), l.get());
    const XG_PLANE_LAYOUT& p = l->Plane[0];
    SX_REQUIRE(l->SizeBytes == 0x56000);
    SX_REQUIRE(p.Reserved2C == 4 && p.Reserved30 == 5);
    // Smallest mip first: the tail block, then mips 3, 2, 1, 0.
    const uint64_t offsets[9] = {0x16000, 0x6000, 0x2000, 0x1000, 0x800, 0x600, 0x500, 0x400, 0x300};
    const uint64_t sizes[9] = {0x40000, 0x10000, 0x4000, 0x1000, 0x800, 0x200, 0x100, 0x100, 0x100};
    for (uint32_t m = 0; m < 9; ++m) {
        SX_REQUIRE(p.MipLayout[m].OffsetBytes == offsets[m]);
        SX_REQUIRE(p.MipLayout[m].SizeBytes == sizes[m]);
        SX_REQUIRE(p.MipLayout[m].Reserved64 == (m >= 4 ? 1u : 0u));
        SX_REQUIRE(p.MipLayout[m].SwizzleMode == (m >= 4 ? 1u : 5u));
    }
    SX_REQUIRE(p.MipLayout[4].PaddedWidthElements == 16 && p.MipLayout[4].PaddedHeightElements == 32);
    SX_REQUIRE(p.MipLayout[5].PaddedWidthElements == 8 && p.MipLayout[5].PaddedHeightElements == 16);
    SX_REQUIRE(p.MipLayout[6].PaddedWidthElements == 8 && p.MipLayout[6].PaddedHeightElements == 8);
    SX_REQUIRE(Offset(*l, 4, 0, 0, 0) == 0x800);
    SX_REQUIRE(Offset(*l, 4, 0, 15, 15) == 0xBFC);
    SX_REQUIRE(Offset(*l, 5, 0, 0, 1) == 0x610);
    SX_REQUIRE(Offset(*l, 5, 0, 7, 7) == 0x6FC);
    SX_REQUIRE(Offset(*l, 6, 0, 3, 3) == 0x53C);

    // Array slices repeat the whole chain.
    xg::ComputeTexture2DLayout(Desc(256, 256, 9, kRgba8, 2), l.get());
    SX_REQUIRE(l->SizeBytes == 0xAC000 && l->Plane[0].Reserved10 == 0x56000);
    SX_REQUIRE(Offset(*l, 0, 1, 0, 0) == 0x6C000);
    SX_REQUIRE(Offset(*l, 8, 1, 0, 0) == 0x56300);

    // Other element sizes.
    xg::ComputeTexture2DLayout(Desc(256, 256, 9, kBc1), l.get());
    SX_REQUIRE(l->SizeBytes == 0xB000 && l->Plane[0].Reserved2C == 2 && l->Plane[0].Reserved30 == 7);
    SX_REQUIRE(l->Plane[0].MipLayout[0].OffsetBytes == 0x3000 && l->Plane[0].MipLayout[0].PitchPixels == 256);
    xg::ComputeTexture2DLayout(Desc(256, 256, 9, kBc3), l.get());
    SX_REQUIRE(l->SizeBytes == 0x16000 && l->Plane[0].Reserved2C == 3);
    xg::ComputeTexture2DLayout(Desc(256, 256, 9, kR8), l.get());
    SX_REQUIRE(l->SizeBytes == 0x16000 && l->Plane[0].Reserved2C == 3);

    // Odd sizes: the tail test uses the power-of-two size, padding rounds up.
    xg::ComputeTexture2DLayout(Desc(33, 17, 3, kRgba8), l.get());
    SX_REQUIRE(l->SizeBytes == 0x4000 && l->Plane[0].Reserved2C == 2 && l->Plane[0].Reserved30 == 1);
    xg::ComputeTexture2DLayout(Desc(16, 32, 3, kRgba8), l.get());
    SX_REQUIRE(l->SizeBytes == 0x1000 && l->Plane[0].Reserved2C == 0 && l->Plane[0].Reserved30 == 3);
    xg::ComputeTexture2DLayout(Desc(32, 16, 3, kRgba8), l.get());
    SX_REQUIRE(l->SizeBytes == 0x2000 && l->Plane[0].Reserved2C == 1 && l->Plane[0].Reserved30 == 2);
}

void RoundTrip(const XG_TEXTURE2D_DESC& d) {
    auto l = std::make_unique<XG_RESOURCE_LAYOUT>();
    xg::ComputeTexture2DLayout(d, l.get());
    SX_REQUIRE(xg::IsAddressable(*l));
    const uint32_t bpe = l->Plane[0].BytesPerElement;
    std::vector<uint8_t> memory(static_cast<size_t>(l->SizeBytes), 0);
    std::vector<uint32_t> owner(memory.size(), 0);  // which element wrote each byte
    uint32_t id = 0;
    std::vector<std::vector<uint8_t>> sources;
    for (uint32_t slice = 0; slice < d.ArraySize; ++slice) {
        for (uint32_t mip = 0; mip < l->MipLevels; ++mip) {
            const XG_MIPLEVEL_LAYOUT& m = l->Plane[0].MipLayout[mip];
            const uint32_t pitch = m.WidthElements * bpe + 12;  // caller-chosen row pitch
            std::vector<uint8_t> src(static_cast<size_t>(pitch) * m.HeightElements);
            for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i * 7 + mip * 31 + slice * 101 + 1);
            SX_REQUIRE(xg::CopyIntoSubresource(*l, memory.data(), mip, slice, src.data(), pitch));
            // No two elements of any subresource may share memory.
            for (uint32_t y = 0; y < m.HeightElements; ++y) {
                for (uint32_t x = 0; x < m.WidthElements; ++x) {
                    const uint64_t off = Offset(*l, mip, slice, x, y);
                    SX_REQUIRE(off + bpe <= memory.size());
                    ++id;
                    for (uint32_t b = 0; b < bpe; ++b) {
                        SX_REQUIRE(owner[off + b] == 0);
                        owner[off + b] = id;
                    }
                }
            }
            sources.push_back(std::move(src));
        }
    }
    size_t index = 0;
    for (uint32_t slice = 0; slice < d.ArraySize; ++slice) {
        for (uint32_t mip = 0; mip < l->MipLevels; ++mip, ++index) {
            const XG_MIPLEVEL_LAYOUT& m = l->Plane[0].MipLayout[mip];
            const uint32_t pitch = m.WidthElements * bpe + 12;
            std::vector<uint8_t> out(sources[index].size(), 0);
            SX_REQUIRE(xg::CopyFromSubresource(*l, memory.data(), mip, slice, out.data(), pitch));
            for (uint32_t y = 0; y < m.HeightElements; ++y) {
                SX_REQUIRE(std::memcmp(out.data() + static_cast<size_t>(y) * pitch,
                                       sources[index].data() + static_cast<size_t>(y) * pitch,
                                       m.WidthElements * bpe) == 0);
            }
        }
    }
}

void TestRoundTrip() {
    RoundTrip(Desc(700, 284, 1, kRgba8));
    RoundTrip(Desc(100, 60, 7, kRgba8));
    RoundTrip(Desc(256, 256, 9, kBc1));
    RoundTrip(Desc(64, 64, 7, kBc3, 3));
    RoundTrip(Desc(40, 24, 6, kR8, 2));
    RoundTrip(Desc(128, 64, 8, 49));  // R8G8
    RoundTrip(Desc(48, 80, 7, 10));   // R16G16B16A16_FLOAT
}

// The texture computer as a title uses it: through its vtable.
template <typename Fn>
Fn Slot(void* object, int index) {
    void** vtable = *reinterpret_cast<void***>(object);
    return reinterpret_cast<Fn>(vtable[index]);
}

void TestComputer() {
    // Sampled textures, render targets and depth buffers.
    SX_REQUIRE(XGComputeOptimalSwizzleMode(3, kRgba8, 700, 284, 1, 1, 0x8, 0) == 5);
    SX_REQUIRE(XGComputeOptimalSwizzleMode(3, kBc3, 1024, 1024, 1, 1, 0x0, 0) == 5);
    SX_REQUIRE(XGComputeOptimalSwizzleMode(3, kRgba8, 1920, 1080, 1, 1, 0x28, 0) == 27);
    SX_REQUIRE(XGComputeOptimalSwizzleMode(3, 40, 1920, 1080, 1, 1, 0x40, 0) == 24);

    // The description exactly as the title lays it out in memory.
    uint32_t raw[16] = {};
    raw[0] = 256;   // +0x00 width
    raw[1] = 256;   // +0x04 height
    raw[2] = 9;     // +0x08 mip levels
    raw[3] = 1;     // +0x0C array size
    raw[4] = kRgba8;  // +0x10 format
    raw[5] = 1;     // +0x14 sample count
    raw[8] = 0x8;   // +0x20 bind flags
    raw[11] = 5;    // +0x2C swizzle mode
    void* computer = nullptr;
    SX_REQUIRE(XGCreateTexture2DComputer(reinterpret_cast<const XG_TEXTURE2D_DESC*>(raw), &computer) == 0);
    SX_REQUIRE(computer != nullptr);

    using RefFn = uint32_t (SCARLETTX_XG_CALL*)(void*);
    using LayoutFn = int32_t (SCARLETTX_XG_CALL*)(void*, void*);
    using SizeFn = uint64_t (SCARLETTX_XG_CALL*)(void*);
    using MipFn = uint64_t (SCARLETTX_XG_CALL*)(void*, uint32_t, uint32_t);
    using TexelFn = uint64_t (SCARLETTX_XG_CALL*)(void*, uint32_t, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t);

    // Slot 2 is the layout query; the object must survive it.
    std::vector<uint8_t> out(0x2000, 0xCD);
    SX_REQUIRE(Slot<LayoutFn>(computer, 2)(computer, out.data()) == 0);
    auto rd32 = [&](size_t off) { uint32_t v; std::memcpy(&v, out.data() + off, 4); return v; };
    auto rd64 = [&](size_t off) { uint64_t v; std::memcpy(&v, out.data() + off, 8); return v; };
    SX_REQUIRE(rd64(0x00) == 0x56000);        // resource size
    SX_REQUIRE(rd64(0x08) == 0x1000);         // base alignment
    SX_REQUIRE(rd32(0x10) == 9 && rd32(0x14) == 1);
    SX_REQUIRE(rd32(0x40) == 4);              // bytes per element
    SX_REQUIRE(rd32(0x44) == 4);              // first mip of the tail
    SX_REQUIRE(rd64(0x50) == 0x40000);        // mip 0 size
    SX_REQUIRE(rd64(0x58) == 0x16000);        // mip 0 offset
    SX_REQUIRE(rd32(0x50 + 0x38) == 256 && rd32(0x50 + 0x3C) == 256);
    SX_REQUIRE(rd32(0x50 + 0x64) == 0);
    SX_REQUIRE(rd64(0x50 + 0x70 + 0x08) == 0x6000);        // mip 1 offset
    SX_REQUIRE(rd32(0x50 + 4 * 0x70 + 0x64) == 1);         // mip 4 is in the tail
    SX_REQUIRE(rd64(0x50 + 4 * 0x70 + 0x08) == 0x800);

    SX_REQUIRE(Slot<SizeFn>(computer, 3)(computer) == 0x56000);
    SX_REQUIRE(Slot<SizeFn>(computer, 4)(computer) == 0x1000);
    SX_REQUIRE(Slot<MipFn>(computer, 5)(computer, 0, 1) == 0x6000);
    SX_REQUIRE(Slot<TexelFn>(computer, 6)(computer, 0, 0, 1, 1, 0, 0) == 0x16014);
    SX_REQUIRE(Slot<TexelFn>(computer, 6)(computer, 0, 4, 15, 15, 0, 0) == 0xBFC);

    SX_REQUIRE(Slot<RefFn>(computer, 0)(computer) == 2);  // AddRef
    SX_REQUIRE(Slot<RefFn>(computer, 1)(computer) == 1);  // Release
    SX_REQUIRE(Slot<RefFn>(computer, 1)(computer) == 0);

    // A mode without texel addressing: every (mip, slice) still gets its own
    // memory inside the resource.
    raw[0] = 64;
    raw[1] = 64;
    raw[2] = 2;
    raw[3] = 2;
    raw[11] = 0;
    void* linear = nullptr;
    SX_REQUIRE(XGCreateTexture2DComputer(reinterpret_cast<const XG_TEXTURE2D_DESC*>(raw), &linear) == 0);
    const uint64_t linear_size = Slot<SizeFn>(linear, 3)(linear);
    struct Range {
        uint64_t first, last;
    } ranges[4];
    int n = 0;
    for (uint32_t slice = 0; slice < 2; ++slice) {
        for (uint32_t mip = 0; mip < 2; ++mip) {
            const uint32_t dim = 64u >> mip;
            ranges[n].first = Slot<TexelFn>(linear, 6)(linear, 0, mip, 0, 0, slice, 0);
            ranges[n].last = Slot<TexelFn>(linear, 6)(linear, 0, mip, dim - 1, dim - 1, slice, 0) + 4;
            SX_REQUIRE(ranges[n].first < ranges[n].last && ranges[n].last <= linear_size);
            ++n;
        }
    }
    for (int a = 0; a < 4; ++a) {
        for (int b = a + 1; b < 4; ++b) {
            SX_REQUIRE(ranges[a].last <= ranges[b].first || ranges[b].last <= ranges[a].first);
        }
    }
    SX_REQUIRE(Slot<RefFn>(linear, 1)(linear) == 0);
}

void TestTileModeAndNewComputers() {
    // 1. XGComputeOptimalTileMode across resource dimensions & bind flags
    SX_REQUIRE(XGComputeOptimalTileMode(1, 0, 4096, 1, 1, 1, 0) == XG_TILE_MODE_LINEAR);
    SX_REQUIRE(XGComputeOptimalTileMode(1, 0, 0, 1, 1, 1, 0) == XG_TILE_MODE_INVALID);
    SX_REQUIRE(XGComputeOptimalTileMode(2, kRgba8, 512, 1, 1, 1, 0x8) == XG_TILE_MODE_LINEAR);
    SX_REQUIRE(XGComputeOptimalTileMode(2, kRgba8, 512, 1, 1, 1, 0x20) == XG_TILE_MODE_1D_THIN);
    SX_REQUIRE(XGComputeOptimalTileMode(3, kRgba8, 1920, 1080, 1, 1, 0x8) == XG_TILE_MODE_2D_THIN);
    SX_REQUIRE(XGComputeOptimalTileMode(3, kRgba8, 1920, 1080, 1, 1, 0x20) == XG_TILE_MODE_DISPLAY);
    SX_REQUIRE(XGComputeOptimalTileMode(3, kRgba8, 1920, 1080, 1, 4, 0x20) == XG_TILE_MODE_2D_THIN);
    SX_REQUIRE(XGComputeOptimalTileMode(3, 40, 1920, 1080, 1, 1, 0x40) == XG_TILE_MODE_DEPTH_STENCIL);
    SX_REQUIRE(XGComputeOptimalTileMode(4, kRgba8, 64, 64, 16, 1, 0x8) == XG_TILE_MODE_2D_THICK);
    SX_REQUIRE(XGComputeOptimalTileMode(4, kRgba8, 64, 64, 2, 1, 0x8) == XG_TILE_MODE_1D_THICK);
    SX_REQUIRE(XGComputeOptimalTileMode(99, kRgba8, 64, 64, 1, 1, 0) == XG_TILE_MODE_INVALID);

    using RefFn = uint32_t (SCARLETTX_XG_CALL*)(void*);
    using LayoutFn = int32_t (SCARLETTX_XG_CALL*)(void*, XG_RESOURCE_LAYOUT*);
    using SizeFn = uint64_t (SCARLETTX_XG_CALL*)(void*);
    using MipFn = uint64_t (SCARLETTX_XG_CALL*)(void*, uint32_t, uint32_t);

    // 2. XGCreateTexture1DComputer (4KB + 64KB, array + mip chain + copy roundtrip)
    {
        void* bad = nullptr;
        SX_REQUIRE(XGCreateTexture1DComputer(nullptr, &bad) != 0);
        XG_TEXTURE1D_DESC d1{};
        d1.Width = 0;
        SX_REQUIRE(XGCreateTexture1DComputer(&d1, &bad) != 0);

        d1.Width = 128;
        d1.MipLevels = 4;
        d1.ArraySize = 2;
        d1.Format = kRgba8;
        d1.BindFlags = 0x8;
        d1.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;
        void* c1 = nullptr;
        SX_REQUIRE(XGCreateTexture1DComputer(&d1, &c1) == 0 && c1 != nullptr);

        auto lay = std::make_unique<XG_RESOURCE_LAYOUT>();
        SX_REQUIRE(Slot<LayoutFn>(c1, 2)(c1, lay.get()) == 0);
        SX_REQUIRE(lay->Dimension == 2 && lay->MipLevels == 4 && lay->Planes == 1);
        SX_REQUIRE(lay->BaseAlignmentBytes == 4096);
        SX_REQUIRE(Slot<SizeFn>(c1, 3)(c1) == lay->SizeBytes);
        SX_REQUIRE(Slot<SizeFn>(c1, 4)(c1) == 4096);
        SX_REQUIRE(lay->SizeBytes % 4096 == 0);
        SX_REQUIRE(lay->Plane[0].MipLayout[0].PitchBytes % 256 == 0);
        SX_REQUIRE(lay->Plane[0].MipLayout[1].WidthElements == 64);

        using Texel1DFn = uint64_t (SCARLETTX_XG_CALL*)(void*, uint32_t, uint32_t, uint64_t, uint32_t);
        using CopyIn1DFn = int32_t (SCARLETTX_XG_CALL*)(void*, void*, uint32_t, uint32_t, const void*);
        using CopyOut1DFn = int32_t (SCARLETTX_XG_CALL*)(void*, void*, const void*, uint32_t, uint32_t);

        const uint64_t off_s0 = Slot<Texel1DFn>(c1, 6)(c1, 0, 0, 5, 0);
        const uint64_t off_s1 = Slot<Texel1DFn>(c1, 6)(c1, 0, 0, 5, 1);
        SX_REQUIRE(off_s0 == 20 && off_s1 == lay->Plane[0].Reserved10 + 20);
        SX_REQUIRE(Slot<MipFn>(c1, 5)(c1, 0, 1) == lay->Plane[0].MipLayout[1].OffsetBytes);

        std::vector<uint8_t> mem(static_cast<size_t>(lay->SizeBytes), 0);
        std::vector<uint8_t> src(64 * 4);
        for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>((i * 13 + 7) & 0xFF);
        // subresource = slice * mips + mip => slice 1, mip 1 = subresource 5
        SX_REQUIRE(Slot<CopyIn1DFn>(c1, 7)(c1, mem.data(), 0, 5, src.data()) == 0);
        std::vector<uint8_t> dst(64 * 4, 0);
        SX_REQUIRE(Slot<CopyOut1DFn>(c1, 8)(c1, dst.data(), mem.data(), 0, 5) == 0);
        SX_REQUIRE(std::memcmp(src.data(), dst.data(), src.size()) == 0);

        SX_REQUIRE(Slot<RefFn>(c1, 0)(c1) == 2);
        SX_REQUIRE(Slot<RefFn>(c1, 1)(c1) == 1);
        SX_REQUIRE(Slot<RefFn>(c1, 1)(c1) == 0);
    }

    // 3. XGCreateTexture3DComputer (64KB swizzle + mip chain + 3D volume copy roundtrip)
    {
        XG_TEXTURE3D_DESC d3{};
        d3.Width = 32;
        d3.Height = 16;
        d3.Depth = 8;
        d3.MipLevels = 3;
        d3.Format = kRgba8;
        d3.BindFlags = 0x8;
        d3.SwizzleMode = XG_SWIZZLE_MODE_64KB_S;
        void* c3 = nullptr;
        SX_REQUIRE(XGCreateTexture3DComputer(&d3, &c3) == 0 && c3 != nullptr);

        auto lay = std::make_unique<XG_RESOURCE_LAYOUT>();
        SX_REQUIRE(Slot<LayoutFn>(c3, 2)(c3, lay.get()) == 0);
        SX_REQUIRE(lay->Dimension == 4 && lay->MipLevels == 3 && lay->Planes == 1);
        SX_REQUIRE(lay->BaseAlignmentBytes == 65536);
        SX_REQUIRE(Slot<SizeFn>(c3, 4)(c3) == 65536);
        SX_REQUIRE(lay->SizeBytes >= 65536 * 3 && lay->SizeBytes % 65536 == 0);
        SX_REQUIRE(lay->Plane[0].MipLayout[0].DepthOrArraySize == 8);
        SX_REQUIRE(lay->Plane[0].MipLayout[1].DepthOrArraySize == 4);
        SX_REQUIRE(lay->Plane[0].MipLayout[2].DepthOrArraySize == 2);
        SX_REQUIRE(lay->Plane[0].MipLayout[0].PitchBytes % 256 == 0);

        using Texel3DFn = uint64_t (SCARLETTX_XG_CALL*)(void*, uint32_t, uint32_t, uint64_t, uint32_t, uint32_t);
        using CopyIn3DFn = int32_t (SCARLETTX_XG_CALL*)(void*, void*, uint32_t, uint32_t, const void*, uint32_t, uint32_t);
        using CopyOut3DFn = int32_t (SCARLETTX_XG_CALL*)(void*, void*, uint32_t, uint32_t, const void*, uint32_t, uint32_t);

        const uint64_t t0 = Slot<Texel3DFn>(c3, 6)(c3, 0, 0, 3, 2, 1);
        const uint64_t t1 = Slot<Texel3DFn>(c3, 6)(c3, 0, 0, 3, 2, 2);
        SX_REQUIRE(t1 - t0 == lay->Plane[0].MipLayout[0].Slice2DSizeBytes);

        // Full 3D copy roundtrip on mip 1 (16 x 8 x 4 RGBA8)
        std::vector<uint8_t> mem(static_cast<size_t>(lay->SizeBytes), 0);
        const uint32_t row_pitch = 16 * 4;
        const uint32_t depth_pitch = row_pitch * 8;
        std::vector<uint8_t> vol_src(static_cast<size_t>(depth_pitch) * 4);
        for (size_t i = 0; i < vol_src.size(); ++i) vol_src[i] = static_cast<uint8_t>((i * 29 + 3) & 0xFF);
        SX_REQUIRE(Slot<CopyIn3DFn>(c3, 7)(c3, mem.data(), 0, 1, vol_src.data(), row_pitch, depth_pitch) == 0);
        std::vector<uint8_t> vol_dst(vol_src.size(), 0);
        SX_REQUIRE(Slot<CopyOut3DFn>(c3, 8)(c3, vol_dst.data(), row_pitch, depth_pitch, mem.data(), 0, 1) == 0);
        SX_REQUIRE(std::memcmp(vol_src.data(), vol_dst.data(), vol_src.size()) == 0);
        SX_REQUIRE(Slot<RefFn>(c3, 1)(c3) == 0);
    }

    // 4. XGCreateBufferComputer (64KB alignment, 256B pitch, copy + bounds checks)
    {
        void* bad = nullptr;
        XG_BUFFER_DESC db{};
        db.ByteWidth = 0;
        SX_REQUIRE(XGCreateBufferComputer(&db, &bad) != 0);

        db.ByteWidth = 1000;
        db.StructureByteStride = 16;
        db.BindFlags = 0x1;
        void* cb = nullptr;
        SX_REQUIRE(XGCreateBufferComputer(&db, &cb) == 0 && cb != nullptr);

        auto lay = std::make_unique<XG_RESOURCE_LAYOUT>();
        SX_REQUIRE(Slot<LayoutFn>(cb, 2)(cb, lay.get()) == 0);
        SX_REQUIRE(lay->Dimension == 1 && lay->MipLevels == 1 && lay->Planes == 1);
        SX_REQUIRE(lay->BaseAlignmentBytes == 65536);
        SX_REQUIRE(lay->SizeBytes == 65536);
        SX_REQUIRE(lay->Plane[0].BytesPerElement == 16);
        SX_REQUIRE(lay->Plane[0].MipLayout[0].PitchBytes == 1024);  // AlignUp(1000, 256)

        using BufOffFn = uint64_t (SCARLETTX_XG_CALL*)(void*);
        using BufCopyFn = int32_t (SCARLETTX_XG_CALL*)(void*, void*, const void*, uint64_t, uint64_t);
        SX_REQUIRE(Slot<BufOffFn>(cb, 5)(cb) == 0);

        std::vector<uint8_t> mem(static_cast<size_t>(lay->SizeBytes), 0);
        std::vector<uint8_t> payload(128, 0x5A);
        SX_REQUIRE(Slot<BufCopyFn>(cb, 6)(cb, mem.data(), payload.data(), 256, 128) == 0);
        // Out-of-bounds copy must fail
        SX_REQUIRE(Slot<BufCopyFn>(cb, 6)(cb, mem.data(), payload.data(), 950, 128) != 0);
        std::vector<uint8_t> back(128, 0);
        SX_REQUIRE(Slot<BufCopyFn>(cb, 7)(cb, back.data(), mem.data(), 256, 128) == 0);
        SX_REQUIRE(std::memcmp(payload.data(), back.data(), 128) == 0);
        SX_REQUIRE(Slot<RefFn>(cb, 1)(cb) == 0);
    }
}

}  // namespace

int main() {
    TestSingleMip();
    TestMipChain();
    TestRoundTrip();
    TestComputer();
    TestTileModeAndNewComputers();
    std::printf("test_xg_layout: OK\n");
    return 0;
}
