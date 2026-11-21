#include "sx_check.h"

#include <cstring>
#include <iostream>
#include <vector>

#include "scarlettx/io/dstorage_abi.h"
#include "scarlettx/io/gdeflate.h"

namespace {

bool VerifyRoundTrip(const std::vector<uint8_t>& input, scarlettx::GDeflateBlockMode mode) {
    std::vector<uint8_t> comp;
    if (!scarlettx::GDeflateCompress(input.data(), input.size(), comp, mode)) {
        return false;
    }
    if (!scarlettx::GDeflateIsValidStream(comp.data(), comp.size())) {
        return false;
    }
    // Verify official Microsoft GDeflate v1 header bytes (0x04, 0xFB).
    if (comp.size() < 8 || comp[0] != 0x04u || comp[1] != 0xFBu) {
        return false;
    }
    size_t parsed_uncomp = 0;
    if (!scarlettx::GDeflateGetUncompressedSize(comp.data(), comp.size(), &parsed_uncomp)) {
        return false;
    }
    if (parsed_uncomp != input.size()) {
        return false;
    }

    std::vector<uint8_t> out(input.size(), 0xEEu);
    size_t out_size = 0;
    const auto res = scarlettx::GDeflateDecompress(
        comp.data(), comp.size(), out.empty() ? nullptr : out.data(), out.size(), &out_size);
    if (res != scarlettx::GDeflateResult::Ok || out_size != input.size()) {
        return false;
    }
    if (!input.empty() && std::memcmp(out.data(), input.data(), input.size()) != 0) {
        return false;
    }
    return true;
}

}  // namespace

int main() {
    // 1. Empty & 1-byte edge cases.
    SX_REQUIRE(VerifyRoundTrip({}, scarlettx::GDeflateBlockMode::Auto));
    SX_REQUIRE(VerifyRoundTrip({0x42u}, scarlettx::GDeflateBlockMode::ForceStored));
    SX_REQUIRE(VerifyRoundTrip({0x42u}, scarlettx::GDeflateBlockMode::ForceFixed));
    SX_REQUIRE(VerifyRoundTrip({0x42u}, scarlettx::GDeflateBlockMode::ForceDynamic));

    // 2. Highly repetitive data (tests overlapping RLE distance=1 & long DEFLATE64 matches).
    std::vector<uint8_t> rle(16384, 0x5Au);
    for (size_t i = 0; i < 512; ++i) rle[8192 + i] = static_cast<uint8_t>((i * 13) & 0xFFu);
    SX_REQUIRE(VerifyRoundTrip(rle, scarlettx::GDeflateBlockMode::ForceStored));
    SX_REQUIRE(VerifyRoundTrip(rle, scarlettx::GDeflateBlockMode::ForceFixed));
    SX_REQUIRE(VerifyRoundTrip(rle, scarlettx::GDeflateBlockMode::ForceDynamic));

    // Verify real compression ratio on repetitive payload.
    std::vector<uint8_t> comp_rle;
    SX_REQUIRE(scarlettx::GDeflateCompress(rle.data(), rle.size(), comp_rle, scarlettx::GDeflateBlockMode::Auto));
    SX_REQUIRE(comp_rle.size() < rle.size() / 4u);

    // 3. Exact 64 KiB single tile boundary (lastTileSize == 0 in TileStream header).
    std::vector<uint8_t> exact_tile(65536);
    for (size_t i = 0; i < exact_tile.size(); ++i) {
        exact_tile[i] = static_cast<uint8_t>((i / 7u + (i % 19u) * 3u) & 0xFFu);
    }
    // Inject a >32 KiB back-reference match to exercise DEFLATE64 distance codes 30-31.
    for (size_t i = 0; i < 4096; ++i) {
        exact_tile[48000 + i] = exact_tile[1000 + i];
    }
    SX_REQUIRE(VerifyRoundTrip(exact_tile, scarlettx::GDeflateBlockMode::ForceStored));
    SX_REQUIRE(VerifyRoundTrip(exact_tile, scarlettx::GDeflateBlockMode::ForceFixed));
    SX_REQUIRE(VerifyRoundTrip(exact_tile, scarlettx::GDeflateBlockMode::ForceDynamic));
    SX_REQUIRE(VerifyRoundTrip(exact_tile, scarlettx::GDeflateBlockMode::Auto));

    // 4. Multi-tile stream (200 KiB = 4 tiles: 3 full 64 KiB + 1 partial 8 KiB tile).
    std::vector<uint8_t> multi_tile(200u * 1024u);
    uint32_t rng = 0x12345678u;
    for (size_t i = 0; i < multi_tile.size(); ++i) {
        if ((i & 0x1FFu) < 320u) {
            multi_tile[i] = static_cast<uint8_t>("SCARLETTX_GDEFLATE_TILE_STREAM_"[i % 31u]);
        } else {
            rng = rng * 1664525u + 1013904223u;
            multi_tile[i] = static_cast<uint8_t>(rng >> 24);
        }
    }
    SX_REQUIRE(VerifyRoundTrip(multi_tile, scarlettx::GDeflateBlockMode::Auto));
    SX_REQUIRE(VerifyRoundTrip(multi_tile, scarlettx::GDeflateBlockMode::ForceDynamic));
    SX_REQUIRE(VerifyRoundTrip(multi_tile, scarlettx::GDeflateBlockMode::ForceFixed));

    // 5. High-entropy incompressible payload (exercises Auto fallback cleanly).
    std::vector<uint8_t> random_buf(12000);
    for (size_t i = 0; i < random_buf.size(); ++i) {
        rng = rng * 1664525u + 1013904223u;
        random_buf[i] = static_cast<uint8_t>(rng >> 24);
    }
    SX_REQUIRE(VerifyRoundTrip(random_buf, scarlettx::GDeflateBlockMode::Auto));

    // 6. Error handling: bad magic, truncated buffer, buffer-too-small.
    const uint8_t bad_magic[16] = {'S', 'X', 'C', 'R', 0, 0, 0, 0};
    SX_REQUIRE(!scarlettx::GDeflateIsValidStream(bad_magic, sizeof(bad_magic)));
    size_t out_sz = 0;
    uint8_t scratch[64]{};
    SX_REQUIRE(scarlettx::GDeflateDecompress(bad_magic, sizeof(bad_magic), scratch, sizeof(scratch), &out_sz) ==
               scarlettx::GDeflateResult::InvalidStream);
    SX_REQUIRE(scarlettx::GDeflateDecompress(comp_rle.data(), comp_rle.size(), scratch, sizeof(scratch), &out_sz) ==
               scarlettx::GDeflateResult::BufferTooSmall);
    SX_REQUIRE(out_sz == rle.size());

    // 7. DirectStorage C ABI round-trip with real GDeflate v1 stream.
    std::vector<uint8_t> ds_comp(rle.size() + 512);
    uint32_t ds_comp_sz = 0;
    SX_REQUIRE(DStorageCompressBuffer(
                   DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, rle.data(), static_cast<uint32_t>(rle.size()),
                   ds_comp.data(), static_cast<uint32_t>(ds_comp.size()), &ds_comp_sz) == 0);
    SX_REQUIRE(ds_comp_sz >= 8u && ds_comp[0] == 0x04u && ds_comp[1] == 0xFBu);
    std::vector<uint8_t> ds_out(rle.size(), 0);
    uint32_t ds_out_sz = 0;
    SX_REQUIRE(DStorageDecompressBuffer(
                   DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, ds_comp.data(), ds_comp_sz,
                   ds_out.data(), static_cast<uint32_t>(ds_out.size()), &ds_out_sz) == 0);
    SX_REQUIRE(ds_out_sz == rle.size());
    SX_REQUIRE(std::memcmp(ds_out.data(), rle.data(), rle.size()) == 0);

    std::cout << "[test_gdeflate] PASSED (GDeflate v1 0x04/0xFB Stored/Fixed/Dynamic/Multi-Tile/DEFLATE64)\n";
    return 0;
}
