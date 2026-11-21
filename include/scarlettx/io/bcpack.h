#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

static constexpr uint32_t kBcPackMagic = 0x4B504342u;  // 'BCPK' little-endian
static constexpr uint16_t kBcPackVersion = 1u;
static constexpr uint32_t kBcPackMaxDimension = 16384u;
static constexpr uint32_t kBcPackMaxPlanes = 8u;

enum class BcFormat : uint32_t {
    Unknown = 0,
    BC1 = 1,   // 8 bytes / 4x4 block (RGB565 endpoints + 16x2-bit indices)
    BC2 = 2,   // 16 bytes / 4x4 block (explicit 4-bit alpha + BC1 color)
    BC3 = 3,   // 16 bytes / 4x4 block (interpolated alpha + BC1 color)
    BC4 = 4,   // 8 bytes / 4x4 block (single-channel interpolated endpoints + 16x3-bit indices)
    BC5 = 5,   // 16 bytes / 4x4 block (two BC4 channels)
    BC6H = 6,  // 16 bytes / 4x4 block (HDR half-float block)
    BC7 = 7,   // 16 bytes / 4x4 block (high-precision RGBA block modes 0..7)
};

enum class BcPlaneKind : uint8_t {
    Raw = 0,
    Color0 = 1,
    Color1 = 2,
    ColorIndices = 3,
    Alpha0 = 4,
    Alpha1 = 5,
    AlphaIndices = 6,
    AlphaExplicit = 7,
    Red0 = 8,
    Red1 = 9,
    RedIndices = 10,
    Green0 = 11,
    Green1 = 12,
    GreenIndices = 13,
    ModeHeader = 14,
    Endpoints = 15,
    SecondaryEndpoints = 16,
    PrimaryIndices = 17,
};

enum class BcPlaneTransform : uint8_t {
    None = 0,
    Delta8 = 1,
    Xor8 = 2,
    Delta16 = 3,
    Rgb565Delta = 4,
    TransposeDelta8 = 5,
};

enum class BcPlaneCodec : uint8_t {
    Raw = 0,
    LzRle = 1,
    CanonicalHuffman = 2,
    LzRleHuffman = 3,
};

struct BcPackPlaneDescriptor {
    BcPlaneKind kind = BcPlaneKind::Raw;
    BcPlaneTransform transform = BcPlaneTransform::None;
    BcPlaneCodec codec = BcPlaneCodec::Raw;
    uint8_t element_bytes = 1;
    uint32_t raw_size = 0;
    uint32_t compressed_size = 0;
    uint32_t data_offset = 0;
    uint32_t plane_crc32 = 0;
};

struct BcPackHeader {
    uint32_t magic = kBcPackMagic;
    uint16_t version = kBcPackVersion;
    uint16_t flags = 0;
    BcFormat format = BcFormat::Unknown;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t block_count = 0;
    uint32_t original_size = 0;
    uint32_t plane_count = 0;
    uint32_t uncompressed_crc32 = 0;
    std::vector<BcPackPlaneDescriptor> planes;
};

struct BcPackStats {
    uint32_t original_size = 0;
    uint32_t compressed_size = 0;
    uint32_t block_count = 0;
    uint32_t plane_count = 0;
    double compression_ratio = 1.0;  // compressed_size / original_size
};

// Returns exact byte size of a single 4x4 block for the given BC format (0 if invalid).
uint32_t BcFormatBlockBytes(BcFormat format) noexcept;

// Computes expected uncompressed byte size for a BC texture surface (0 on invalid/overflow).
uint64_t BcCalculateSurfaceSize(BcFormat format, uint32_t width, uint32_t height) noexcept;

// Computes IEEE 802.3 CRC32 over a byte range.
uint32_t BcPackCrc32(const uint8_t* data, size_t size) noexcept;

// Parses and validates a BCPack container header and its plane descriptor table.
Status BcPackParseHeader(const uint8_t* src, size_t src_size, BcPackHeader& out_header);

// Compresses a BC1/BC2/BC3/BC4/BC5/BC6H/BC7 GPU texture surface losslessly using
// BC-block plane separation, spatial delta/XOR decorrelation, and LZ-RLE + Canonical Huffman coding.
Status BcPackCompress(
    BcFormat format,
    uint32_t width,
    uint32_t height,
    const uint8_t* src_blocks,
    size_t src_size,
    std::vector<uint8_t>& out_compressed,
    BcPackStats* out_stats = nullptr);

// Decompresses a BCPack stream back to the exact original BC texture surface bytes,
// verifying header consistency, plane CRCs, codec bounds, and final uncompressed CRC32.
Status BcPackDecompress(
    const uint8_t* src,
    size_t src_size,
    std::vector<uint8_t>& out_blocks,
    BcPackHeader* out_header = nullptr);

}  // namespace scarlettx
