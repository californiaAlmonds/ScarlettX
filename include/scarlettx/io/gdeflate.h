#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scarlettx {

static constexpr uint8_t kGDeflateCodecId = 0x04u;
static constexpr uint8_t kGDeflateMagic = 0xFBu;  // kGDeflateCodecId ^ 0xFF
static constexpr uint32_t kGDeflateTileSize = 65536u;
static constexpr uint32_t kGDeflateNumStreams = 32u;

enum class GDeflateBlockMode : uint8_t {
    Auto = 0,          // Pick smallest of Dynamic, Fixed, and Stored per tile
    ForceStored = 1,   // Always emit btype=0 (uncompressed) blocks
    ForceFixed = 2,    // Always emit btype=1 (fixed Huffman) blocks
    ForceDynamic = 3,  // Always emit btype=2 (dynamic Huffman) blocks
};

// Returns true if `src` starts with a valid Microsoft GDeflate v1 TileStream header.
bool GDeflateIsValidStream(const uint8_t* src, size_t src_size);

// Parses the TileStream header and writes the total uncompressed byte count.
bool GDeflateGetUncompressedSize(const uint8_t* src, size_t src_size, size_t* out_uncompressed_size);

// Compresses `src` into a spec-compliant Microsoft GDeflate v1 TileStream
// (0x04 0xFB header, 64 KiB tiles, 32-stream bit-interleaved DEFLATE64).
bool GDeflateCompress(
    const uint8_t* src,
    size_t src_size,
    std::vector<uint8_t>& out_compressed,
    GDeflateBlockMode mode = GDeflateBlockMode::Auto);

// Decompresses a Microsoft GDeflate v1 TileStream into `dst` (capacity `dst_capacity`).
// Writes the decompressed size into `out_size` (also populated on buffer-too-small).
enum class GDeflateResult : int32_t {
    Ok = 0,
    InvalidArg = 1,
    InvalidStream = 2,
    BufferTooSmall = 3,
    CorruptData = 4,
};

GDeflateResult GDeflateDecompress(
    const uint8_t* src,
    size_t src_size,
    uint8_t* dst,
    size_t dst_capacity,
    size_t* out_size);

}  // namespace scarlettx
