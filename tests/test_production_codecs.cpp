#include "sx_check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "scarlettx/audio/xma2_decoder.h"
#include "scarlettx/io/bcpack.h"
#include "scarlettx/middleware/bink2_decoder.h"
#include "scarlettx/middleware/ffx_gpu_fsr.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

uint16_t PackRgb565(uint32_t r5, uint32_t g6, uint32_t b5) {
    return static_cast<uint16_t>(((r5 & 31u) << 11u) | ((g6 & 63u) << 5u) | (b5 & 31u));
}

void WriteLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
}

void WriteLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16u) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24u) & 0xFFu);
}

double ComputeAudioSnrDb(const std::vector<float>& ref, const std::vector<float>& dec,
                         uint16_t channels, uint16_t ch) {
    const size_t n = std::min(ref.size(), dec.size()) / channels;
    double sig_pow = 0.0;
    double err_pow = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double r = ref[i * channels + ch];
        const double d = dec[i * channels + ch];
        sig_pow += r * r;
        const double e = r - d;
        err_pow += e * e;
    }
    if (err_pow < 1e-18) return 120.0;
    return 10.0 * std::log10(sig_pow / err_pow);
}

double MeasureToneMagnitude(const std::vector<float>& pcm, uint16_t channels, uint16_t ch,
                            uint32_t sample_rate, double freq_hz) {
    const size_t n = pcm.size() / channels;
    double re = 0.0;
    double im = 0.0;
    const double step = 2.0 * kPi * freq_hz / static_cast<double>(sample_rate);
    for (size_t i = 0; i < n; ++i) {
        const double s = pcm[i * channels + ch];
        const double ang = step * static_cast<double>(i);
        re += s * std::cos(ang);
        im -= s * std::sin(ang);
    }
    return std::sqrt(re * re + im * im) * (2.0 / static_cast<double>(n));
}

double ComputeRgbPsnrDb(const std::vector<uint8_t>& ref_rgba, const std::vector<uint8_t>& dec_rgba) {
    SX_REQUIRE(ref_rgba.size() == dec_rgba.size());
    const size_t pixels = ref_rgba.size() / 4u;
    double mse = 0.0;
    for (size_t i = 0; i < pixels; ++i) {
        for (size_t c = 0; c < 3u; ++c) {
            const double diff = static_cast<double>(ref_rgba[i * 4u + c]) -
                                static_cast<double>(dec_rgba[i * 4u + c]);
            mse += diff * diff;
        }
    }
    mse /= static_cast<double>(pixels * 3u);
    if (mse < 1e-9) return 99.0;
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

double ComputeEdgeGradientEnergy(const scarlettx::FrameBufferRgba8& fb) {
    // Mean squared Sobel gradient energy (L2 edge steepness metric; unlike L1 which is
    // telescoping-invariant on monotonic steps, L2 strictly increases as edges become crisper).
    const uint32_t w = fb.width;
    const uint32_t h = fb.height;
    auto luma = [&](uint32_t x, uint32_t y) -> double {
        const size_t idx = (static_cast<size_t>(y) * w + x) * 4u;
        return 0.2126 * fb.pixels[idx + 0] +
               0.7152 * fb.pixels[idx + 1] +
               0.0722 * fb.pixels[idx + 2];
    };
    double total = 0.0;
    uint64_t count = 0;
    for (uint32_t y = 2; y + 2 < h; ++y) {
        for (uint32_t x = 2; x + 2 < w; ++x) {
            const double gx =
                -luma(x - 1, y - 1) + luma(x + 1, y - 1)
                - 2.0 * luma(x - 1, y) + 2.0 * luma(x + 1, y)
                - luma(x - 1, y + 1) + luma(x + 1, y + 1);
            const double gy =
                -luma(x - 1, y - 1) - 2.0 * luma(x, y - 1) - luma(x + 1, y - 1)
                + luma(x - 1, y + 1) + 2.0 * luma(x, y + 1) + luma(x + 1, y + 1);
            total += (gx * gx + gy * gy);
            ++count;
        }
    }
    return count > 0 ? std::sqrt(total / static_cast<double>(count)) : 0.0;
}

// ===========================================================================
// 1. Xbox BCPack GPU Texture Codec Tests
// ===========================================================================
void TestBcPackCodec() {
    using namespace scarlettx;

    SX_REQUIRE(BcFormatBlockBytes(BcFormat::BC1) == 8u);
    SX_REQUIRE(BcFormatBlockBytes(BcFormat::BC2) == 16u);
    SX_REQUIRE(BcFormatBlockBytes(BcFormat::BC3) == 16u);
    SX_REQUIRE(BcFormatBlockBytes(BcFormat::BC4) == 8u);
    SX_REQUIRE(BcFormatBlockBytes(BcFormat::BC5) == 16u);
    SX_REQUIRE(BcFormatBlockBytes(BcFormat::BC7) == 16u);
    SX_REQUIRE(BcFormatBlockBytes(BcFormat::Unknown) == 0u);

    // 1A. BC1 256x256 smooth gradient + structured index patterns (32 KiB)
    {
        constexpr uint32_t W = 256u, H = 256u;
        const uint32_t bw = W / 4u, bh = H / 4u;
        std::vector<uint8_t> bc1(static_cast<size_t>(bw) * bh * 8u);
        for (uint32_t by = 0; by < bh; ++by) {
            for (uint32_t bx = 0; bx < bw; ++bx) {
                uint8_t* blk = bc1.data() + (static_cast<size_t>(by) * bw + bx) * 8u;
                const uint32_t r0 = (bx * 31u) / bw;
                const uint32_t g0 = (by * 63u) / bh;
                const uint32_t b0 = ((bx + by) * 31u) / (bw + bh);
                const uint16_t c0 = PackRgb565(r0, g0, b0);
                const uint16_t c1 = PackRgb565(std::min(31u, r0 + 2u), std::min(63u, g0 + 2u), b0);
                WriteLe16(blk + 0, c0);
                WriteLe16(blk + 2, c1);
                const uint32_t pattern = ((bx ^ by) & 3u) == 0u ? 0x55AA55AAu : 0x00000000u;
                WriteLe32(blk + 4, pattern);
            }
        }

        std::vector<uint8_t> compressed;
        BcPackStats stats{};
        SX_REQUIRE(BcPackCompress(BcFormat::BC1, W, H, bc1.data(), bc1.size(), compressed, &stats).ok());
        SX_REQUIRE(stats.original_size == bc1.size());
        SX_REQUIRE(stats.block_count == bw * bh);
        SX_REQUIRE(stats.plane_count == 3u);
        SX_REQUIRE(stats.compression_ratio < 0.60);

        std::vector<uint8_t> decompressed;
        BcPackHeader hdr{};
        SX_REQUIRE(BcPackDecompress(compressed.data(), compressed.size(), decompressed, &hdr).ok());
        SX_REQUIRE(hdr.magic == kBcPackMagic);
        SX_REQUIRE(hdr.format == BcFormat::BC1);
        SX_REQUIRE(hdr.width == W && hdr.height == H);
        SX_REQUIRE(decompressed == bc1);

        std::cout << "[BCPack][BC1] " << bc1.size() << " -> " << compressed.size()
                  << " bytes (ratio=" << stats.compression_ratio << ") bit-exact OK\n";

        // Corruption / validation checks
        std::vector<uint8_t> bad_magic = compressed;
        bad_magic[0] = 'X';
        SX_REQUIRE(!BcPackDecompress(bad_magic.data(), bad_magic.size(), decompressed).ok());

        std::vector<uint8_t> bad_crc = compressed;
        bad_crc[32] ^= 0xFFu;  // Flip uncompressed CRC32
        SX_REQUIRE(!BcPackDecompress(bad_crc.data(), bad_crc.size(), decompressed).ok());

        std::vector<uint8_t> bad_payload = compressed;
        bad_payload.back() ^= 0x01u;  // Flip 1 bit in compressed payload (caught by plane CRC32)
        SX_REQUIRE(!BcPackDecompress(bad_payload.data(), bad_payload.size(), decompressed).ok());

        SX_REQUIRE(!BcPackDecompress(compressed.data(), compressed.size() / 2u, decompressed).ok());
        SX_REQUIRE(!BcPackDecompress(compressed.data(), 12u, decompressed).ok());
        SX_REQUIRE(!BcPackCompress(BcFormat::BC1, 0, H, bc1.data(), bc1.size(), compressed).ok());
        SX_REQUIRE(!BcPackCompress(BcFormat::BC1, W, H, bc1.data(), bc1.size() - 8u, compressed).ok());
    }

    // 1B. BC3 128x128 smooth alpha + color gradient (16 KiB)
    {
        constexpr uint32_t W = 128u, H = 128u;
        const uint32_t bw = W / 4u, bh = H / 4u;
        std::vector<uint8_t> bc3(static_cast<size_t>(bw) * bh * 16u);
        for (uint32_t by = 0; by < bh; ++by) {
            for (uint32_t bx = 0; bx < bw; ++bx) {
                uint8_t* blk = bc3.data() + (static_cast<size_t>(by) * bw + bx) * 16u;
                blk[0] = static_cast<uint8_t>(200u + (bx & 31u));
                blk[1] = static_cast<uint8_t>(180u + (by & 31u));
                for (int k = 0; k < 6; ++k) {
                    blk[2 + k] = static_cast<uint8_t>((k * 17u + ((bx >> 2u) & 3u)) & 0xFFu);
                }
                WriteLe16(blk + 8, PackRgb565(bx, by * 2u, 16u));
                WriteLe16(blk + 10, PackRgb565(std::min(31u, bx + 1u), std::min(63u, by * 2u + 1u), 16u));
                WriteLe32(blk + 12, (by & 1u) ? 0x11223344u : 0x55667788u);
            }
        }

        std::vector<uint8_t> compressed, decompressed;
        BcPackStats stats{};
        SX_REQUIRE(BcPackCompress(BcFormat::BC3, W, H, bc3.data(), bc3.size(), compressed, &stats).ok());
        SX_REQUIRE(stats.plane_count == 6u);
        SX_REQUIRE(stats.compression_ratio < 0.60);
        SX_REQUIRE(BcPackDecompress(compressed.data(), compressed.size(), decompressed).ok());
        SX_REQUIRE(decompressed == bc3);
        std::cout << "[BCPack][BC3] " << bc3.size() << " -> " << compressed.size()
                  << " bytes (ratio=" << stats.compression_ratio << ") bit-exact OK\n";
    }

    // 1C. BC5 normal-map surface (128x128) & BC4 heightmap (64x64)
    {
        constexpr uint32_t W = 128u, H = 128u;
        const uint32_t bw = W / 4u, bh = H / 4u;
        std::vector<uint8_t> bc5(static_cast<size_t>(bw) * bh * 16u);
        for (uint32_t i = 0; i < bw * bh; ++i) {
            uint8_t* blk = bc5.data() + static_cast<size_t>(i) * 16u;
            blk[0] = static_cast<uint8_t>(128 + static_cast<int>((i % bw) - 16));
            blk[1] = static_cast<uint8_t>(120 + static_cast<int>((i % bw) - 16));
            std::memset(blk + 2, 0x24, 6);
            blk[8] = static_cast<uint8_t>(128 + static_cast<int>((i / bw) - 16));
            blk[9] = static_cast<uint8_t>(124 + static_cast<int>((i / bw) - 16));
            std::memset(blk + 10, 0x49, 6);
        }
        std::vector<uint8_t> compressed, decompressed;
        BcPackStats stats{};
        SX_REQUIRE(BcPackCompress(BcFormat::BC5, W, H, bc5.data(), bc5.size(), compressed, &stats).ok());
        SX_REQUIRE(stats.compression_ratio < 0.50);
        SX_REQUIRE(BcPackDecompress(compressed.data(), compressed.size(), decompressed).ok());
        SX_REQUIRE(decompressed == bc5);
    }

    // 1D. BC7 Mode-6 structured RGBA surface (128x128 = 16 KiB)
    {
        constexpr uint32_t W = 128u, H = 128u;
        const uint32_t bw = W / 4u, bh = H / 4u;
        std::vector<uint8_t> bc7(static_cast<size_t>(bw) * bh * 16u, 0u);
        for (uint32_t by = 0; by < bh; ++by) {
            for (uint32_t bx = 0; bx < bw; ++bx) {
                uint8_t* blk = bc7.data() + (static_cast<size_t>(by) * bw + bx) * 16u;
                // Mode 6: bit 6 set (0x40), smoothly varying RGBA7777 endpoints + repeating index nibbles
                blk[0] = 0x40u;
                blk[1] = static_cast<uint8_t>((bx * 4u) & 0xFFu);
                blk[2] = static_cast<uint8_t>((by * 4u) & 0xFFu);
                blk[3] = static_cast<uint8_t>(((bx + by) * 2u) & 0xFFu);
                blk[4] = 0xFCu;
                blk[5] = static_cast<uint8_t>((bx * 4u + 2u) & 0xFFu);
                blk[6] = static_cast<uint8_t>((by * 4u + 2u) & 0xFFu);
                blk[7] = 0xFEu;
                WriteLe32(blk + 8, (bx & 1u) ? 0x12345678u : 0x12345678u);
                WriteLe32(blk + 12, (by & 3u) ? 0x22222222u : 0x44444444u);
            }
        }
        std::vector<uint8_t> compressed, decompressed;
        BcPackStats stats{};
        SX_REQUIRE(BcPackCompress(BcFormat::BC7, W, H, bc7.data(), bc7.size(), compressed, &stats).ok());
        SX_REQUIRE(stats.plane_count == 4u);
        SX_REQUIRE(stats.compression_ratio < 0.60);
        SX_REQUIRE(BcPackDecompress(compressed.data(), compressed.size(), decompressed).ok());
        SX_REQUIRE(decompressed == bc7);
        std::cout << "[BCPack][BC7] " << bc7.size() << " -> " << compressed.size()
                  << " bytes (ratio=" << stats.compression_ratio << ") bit-exact OK\n";
    }

    // 1E. Realistic varied-index BC1 texture (non-trivial spatial variation across all blocks)
    {
        constexpr uint32_t W = 128u, H = 128u;
        const uint32_t bw = W / 4u, bh = H / 4u;
        std::vector<uint8_t> bc1_varied(static_cast<size_t>(bw) * bh * 8u);
        for (uint32_t by = 0; by < bh; ++by) {
            for (uint32_t bx = 0; bx < bw; ++bx) {
                uint8_t* blk = bc1_varied.data() + (static_cast<size_t>(by) * bw + bx) * 8u;
                const uint32_t wave = static_cast<uint32_t>(
                    15.0 + 14.0 * std::sin(0.25 * bx + 0.15 * by));
                const uint32_t gwave = static_cast<uint32_t>(
                    31.0 + 28.0 * std::cos(0.18 * by - 0.12 * bx));
                WriteLe16(blk + 0, PackRgb565(wave, gwave, (bx + by) & 31u));
                WriteLe16(blk + 2, PackRgb565(std::min(31u, wave + 3u), std::min(63u, gwave + 5u), (bx + by + 2u) & 31u));
                // Varied 2-bit index selectors drawn from realistic directional gradient block patterns
                constexpr uint32_t kPatterns[8] = {
                    0x01230123u, 0x32103210u, 0x0055AAFFu, 0xFFAA5500u,
                    0x11223300u, 0x01122333u, 0x00112233u, 0x5555AAAAu
                };
                WriteLe32(blk + 4, kPatterns[(bx / 2u + by) & 7u]);
            }
        }
        std::vector<uint8_t> compressed, decompressed;
        BcPackStats stats{};
        SX_REQUIRE(BcPackCompress(BcFormat::BC1, W, H, bc1_varied.data(), bc1_varied.size(), compressed, &stats).ok());
        SX_REQUIRE(stats.compression_ratio < 0.60);
        SX_REQUIRE(BcPackDecompress(compressed.data(), compressed.size(), decompressed).ok());
        SX_REQUIRE(decompressed == bc1_varied);
        std::cout << "[BCPack][BC1-varied] " << bc1_varied.size() << " -> " << compressed.size()
                  << " bytes (ratio=" << stats.compression_ratio << ") bit-exact OK\n";
    }

    // 1F. High-entropy incompressible BC7 fallback test (must still roundtrip bit-exact)
    {
        constexpr uint32_t W = 32u, H = 32u;
        std::vector<uint8_t> noisy(static_cast<size_t>(W / 4u) * (H / 4u) * 16u);
        uint32_t rng = 0xDEADBEEFu;
        for (uint8_t& b : noisy) {
            rng = rng * 1664525u + 1013904223u;
            b = static_cast<uint8_t>(rng >> 24u);
        }
        std::vector<uint8_t> compressed, decompressed;
        SX_REQUIRE(BcPackCompress(BcFormat::BC7, W, H, noisy.data(), noisy.size(), compressed).ok());
        SX_REQUIRE(BcPackDecompress(compressed.data(), compressed.size(), decompressed).ok());
        SX_REQUIRE(decompressed == noisy);
    }
}

// ===========================================================================
// 2. Native XMA2 Packet & IMDCT Audio Decoder Tests
// ===========================================================================
void TestXma2Decoder() {
    using namespace scarlettx;

    // 2A. Standard 4-byte big-endian XMA2 packet header roundtrip check
    {
        Xma2PacketHeader orig{};
        orig.frame_count = 7u;
        orig.frame_offset_in_bits = 1234u;
        orig.packet_metadata = 1u;
        orig.packet_skip_count = 3u;

        uint8_t bytes[4] = {};
        SX_REQUIRE(PackXma2PacketHeader(orig, bytes).ok());
        // Verify exact big-endian bit layout
        const uint32_t expected_be = (7u << 26u) | (1234u << 11u) | (1u << 8u) | 3u;
        SX_REQUIRE(bytes[0] == ((expected_be >> 24u) & 0xFFu));
        SX_REQUIRE(bytes[1] == ((expected_be >> 16u) & 0xFFu));
        SX_REQUIRE(bytes[2] == ((expected_be >> 8u) & 0xFFu));
        SX_REQUIRE(bytes[3] == (expected_be & 0xFFu));

        Xma2PacketHeader parsed{};
        SX_REQUIRE(ParseXma2PacketHeader(bytes, sizeof(bytes), parsed).ok());
        SX_REQUIRE(parsed.frame_count == orig.frame_count);
        SX_REQUIRE(parsed.frame_offset_in_bits == orig.frame_offset_in_bits);
        SX_REQUIRE(parsed.packet_metadata == orig.packet_metadata);
        SX_REQUIRE(parsed.packet_skip_count == orig.packet_skip_count);
    }

    // 2B. Direct Type-IV MDCT / IMDCT TDAC exact reconstruction check
    {
        constexpr uint32_t M = 512u;
        constexpr uint32_t N = 2u * M;
        std::vector<float> win(N);
        Xma2ComputeSineWindow(win.data(), N);

        std::vector<float> sig(3u * M);
        for (size_t i = 0; i < sig.size(); ++i) {
            sig[i] = 0.6f * std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / 48000.0) +
                     0.3f * std::cos(2.0 * kPi * 1320.0 * static_cast<double>(i) / 48000.0);
        }

        std::vector<float> w0(N), w1(N), X0(M), X1(M), y0(N), y1(N);
        for (uint32_t n = 0; n < N; ++n) {
            w0[n] = sig[n] * win[n];
            w1[n] = sig[M + n] * win[n];
        }
        Xma2ForwardMdct(w0.data(), X0.data(), M);
        Xma2ForwardMdct(w1.data(), X1.data(), M);
        Xma2InverseMdct(X0.data(), y0.data(), M);
        Xma2InverseMdct(X1.data(), y1.data(), M);

        double max_err = 0.0;
        for (uint32_t n = 0; n < M; ++n) {
            const float recon = win[M + n] * y0[M + n] + win[n] * y1[n];
            max_err = std::max(max_err, std::fabs(static_cast<double>(recon - sig[M + n])));
        }
        SX_REQUIRE(max_err < 1e-4);
    }

    // 2C. Stereo 48 kHz (L = 440 Hz, R = 880 Hz) -> 2048-byte XMA2 packets -> IMDCT decode
    {
        Xma2Config cfg{};
        cfg.sample_rate = 48000u;
        cfg.channels = 2u;
        cfg.block_size = 512u;
        cfg.subframes_per_frame = 4u;
        cfg.quant_bits = 12u;

        constexpr size_t kFrames = 6144u;  // 128 ms stereo
        std::vector<float> input_pcm(kFrames * 2u);
        for (size_t i = 0; i < kFrames; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(cfg.sample_rate);
            input_pcm[i * 2u + 0u] = static_cast<float>(0.8 * std::sin(2.0 * kPi * 440.0 * t));
            input_pcm[i * 2u + 1u] = static_cast<float>(0.7 * std::sin(2.0 * kPi * 880.0 * t));
        }

        std::vector<uint8_t> packets;
        SX_REQUIRE(Xma2EncodePcmToPackets(input_pcm.data(), kFrames, cfg, packets).ok());
        SX_REQUIRE(packets.size() >= kXma2PacketSize);
        SX_REQUIRE((packets.size() % kXma2PacketSize) == 0u);

        // Verify every packet has valid big-endian XMA2 header
        Xma2PacketHeader pkt0{};
        SX_REQUIRE(ParseXma2PacketHeader(packets.data(), kXma2PacketSize, pkt0).ok());
        SX_REQUIRE(pkt0.packet_metadata == 1u);
        SX_REQUIRE(pkt0.frame_count > 0u);
        SX_REQUIRE(pkt0.frame_offset_in_bits == 0u);

        Xma2Decoder decoder;
        SX_REQUIRE(decoder.Initialize(cfg).ok());
        std::vector<float> decoded_f32;
        SX_REQUIRE(decoder.DecodePackets(packets.data(), packets.size(), decoded_f32).ok());
        SX_REQUIRE(decoded_f32.size() == input_pcm.size());
        SX_REQUIRE(decoder.Stats().packets_parsed == packets.size() / kXma2PacketSize);
        SX_REQUIRE(decoder.Stats().frames_decoded > 0u);
        SX_REQUIRE(decoder.Stats().pcm_frames_output == kFrames);

        const double snr_l = ComputeAudioSnrDb(input_pcm, decoded_f32, 2u, 0u);
        const double snr_r = ComputeAudioSnrDb(input_pcm, decoded_f32, 2u, 1u);
        SX_REQUIRE(snr_l > 40.0);
        SX_REQUIRE(snr_r > 40.0);

        // Dominant frequency verification via DFT correlation
        const double l_440 = MeasureToneMagnitude(decoded_f32, 2u, 0u, cfg.sample_rate, 440.0);
        const double l_880 = MeasureToneMagnitude(decoded_f32, 2u, 0u, cfg.sample_rate, 880.0);
        const double r_440 = MeasureToneMagnitude(decoded_f32, 2u, 1u, cfg.sample_rate, 440.0);
        const double r_880 = MeasureToneMagnitude(decoded_f32, 2u, 1u, cfg.sample_rate, 880.0);
        SX_REQUIRE(l_440 > 0.75 && l_440 > l_880 * 50.0);
        SX_REQUIRE(r_880 > 0.65 && r_880 > r_440 * 50.0);

        // Also test int16 PCM output path
        decoder.Reset();
        std::vector<int16_t> decoded_i16;
        SX_REQUIRE(decoder.DecodePacketsInt16(packets.data(), packets.size(), decoded_i16).ok());
        SX_REQUIRE(decoded_i16.size() == input_pcm.size());

        std::cout << "[XMA2][Stereo] packets=" << (packets.size() / kXma2PacketSize)
                  << " SNR_L=" << snr_l << "dB SNR_R=" << snr_r << "dB OK\n";

        // 2D. Multi-stream packet_skip_count verification (interleave dummy packet after packet 0)
        if (packets.size() >= 2u * kXma2PacketSize) {
            std::vector<uint8_t> interleaved;
            interleaved.insert(interleaved.end(), packets.begin(), packets.begin() + kXma2PacketSize);
            // Set packet_skip_count = 1 on packet 0
            Xma2PacketHeader h0{};
            SX_REQUIRE(ParseXma2PacketHeader(interleaved.data(), kXma2PacketSize, h0).ok());
            h0.packet_skip_count = 1u;
            SX_REQUIRE(PackXma2PacketHeader(h0, interleaved.data()).ok());
            // Insert dummy 2048B packet from another stream
            std::vector<uint8_t> dummy_pkt(kXma2PacketSize, 0xAAu);
            interleaved.insert(interleaved.end(), dummy_pkt.begin(), dummy_pkt.end());
            // Append remaining packets
            interleaved.insert(interleaved.end(), packets.begin() + kXma2PacketSize, packets.end());

            decoder.Reset();
            std::vector<float> skipped_pcm;
            SX_REQUIRE(decoder.DecodePackets(interleaved.data(), interleaved.size(), skipped_pcm).ok());
            SX_REQUIRE(decoder.Stats().packets_skipped == 1u);
            SX_REQUIRE(skipped_pcm == decoded_f32);
        }
    }

    // 2E. Mono 44.1 kHz dual-tone (440 Hz + 880 Hz) & error validation
    {
        Xma2Config cfg{};
        cfg.sample_rate = 44100u;
        cfg.channels = 1u;
        cfg.block_size = 512u;
        cfg.subframes_per_frame = 4u;
        cfg.quant_bits = 12u;

        constexpr size_t kFrames = 4096u;
        std::vector<float> mono_in(kFrames);
        for (size_t i = 0; i < kFrames; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(cfg.sample_rate);
            mono_in[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 440.0 * t) +
                                            0.4 * std::sin(2.0 * kPi * 880.0 * t));
        }

        std::vector<uint8_t> packets;
        SX_REQUIRE(Xma2EncodePcmToPackets(mono_in.data(), kFrames, cfg, packets).ok());

        Xma2Decoder dec;
        SX_REQUIRE(dec.Initialize(cfg).ok());
        std::vector<float> mono_out;
        SX_REQUIRE(dec.DecodePackets(packets.data(), packets.size(), mono_out).ok());
        SX_REQUIRE(mono_out.size() == kFrames);
        const double snr = ComputeAudioSnrDb(mono_in, mono_out, 1u, 0u);
        SX_REQUIRE(snr > 40.0);
        const double m440 = MeasureToneMagnitude(mono_out, 1u, 0u, cfg.sample_rate, 440.0);
        const double m880 = MeasureToneMagnitude(mono_out, 1u, 0u, cfg.sample_rate, 880.0);
        const double m660 = MeasureToneMagnitude(mono_out, 1u, 0u, cfg.sample_rate, 660.0);
        SX_REQUIRE(m440 > 0.45 && m880 > 0.35 && m660 < 0.02);

        // Invalid packet size rejection
        SX_REQUIRE(!dec.DecodePackets(packets.data(), 1000u, mono_out).ok());
        std::cout << "[XMA2][Mono] packets=" << (packets.size() / kXma2PacketSize)
                  << " SNR=" << snr << "dB (440Hz=" << m440 << ", 880Hz=" << m880 << ") OK\n";
    }
}

// ===========================================================================
// 3. Bink2 Container & Plane Frame Decoder Tests
// ===========================================================================
void TestBink2Decoder() {
    using namespace scarlettx;

    // 3A. Exact 8x8 2D DCT / IDCT invertibility check
    {
        float block[64], freq[64], recon[64];
        for (int i = 0; i < 64; ++i) {
            block[i] = static_cast<float>((i * 7) % 120) - 60.0f;
        }
        Bink2ForwardDct8x8(block, freq);
        Bink2InverseDct8x8(freq, recon);
        for (int i = 0; i < 64; ++i) {
            SX_REQUIRE(std::fabs(block[i] - recon[i]) < 1e-3f);
        }
    }

    // 3B. BT.709 RGB <-> YCbCr conversion accuracy
    {
        const std::array<std::array<uint8_t, 3>, 6> colors = {{
            {255, 0, 0}, {0, 255, 0}, {0, 0, 255},
            {200, 150, 80}, {40, 210, 190}, {128, 128, 128}
        }};
        for (const auto& c : colors) {
            uint8_t y = 0, cb = 0, cr = 0, r = 0, g = 0, b = 0;
            Bink2RgbToYCbCr709(c[0], c[1], c[2], y, cb, cr);
            Bink2YCbCr709ToRgb(y, cb, cr, r, g, b);
            SX_REQUIRE(std::abs(static_cast<int>(r) - static_cast<int>(c[0])) <= 2);
            SX_REQUIRE(std::abs(static_cast<int>(g) - static_cast<int>(c[1])) <= 2);
            SX_REQUIRE(std::abs(static_cast<int>(b) - static_cast<int>(c[2])) <= 2);
        }
    }

    // 3C. Multi-frame KB2i YCbCr 4:2:0 stream (keyframes + motion/residual P-frames + seeking)
    {
        constexpr uint32_t W = 64u, H = 64u;
        constexpr uint32_t kNumFrames = 8u;
        std::vector<std::vector<uint8_t>> src_frames(kNumFrames);

        for (uint32_t f = 0; f < kNumFrames; ++f) {
            src_frames[f].resize(static_cast<size_t>(W) * H * 4u);
            const int32_t box_x = 8 + static_cast<int32_t>(f) * 4;
            const int32_t box_y = 12 + static_cast<int32_t>(f) * 2;
            for (uint32_t y = 0; y < H; ++y) {
                for (uint32_t x = 0; x < W; ++x) {
                    uint8_t r = static_cast<uint8_t>(24u + x * 2u);
                    uint8_t g = static_cast<uint8_t>(36u + y * 2u);
                    uint8_t b = static_cast<uint8_t>(100u + (x + y));
                    // Moving 16x16 bright gold block to exercise motion vectors & residuals
                    if (static_cast<int32_t>(x) >= box_x && static_cast<int32_t>(x) < box_x + 16 &&
                        static_cast<int32_t>(y) >= box_y && static_cast<int32_t>(y) < box_y + 16) {
                        r = 235u;
                        g = 190u;
                        b = 45u;
                    }
                    const size_t idx = (static_cast<size_t>(y) * W + x) * 4u;
                    src_frames[f][idx + 0] = r;
                    src_frames[f][idx + 1] = g;
                    src_frames[f][idx + 2] = b;
                    src_frames[f][idx + 3] = 255u;
                }
            }
        }

        Bink2EncodeParams ep{};
        ep.width = W;
        ep.height = H;
        ep.fps_num = 60;
        ep.fps_den = 1;
        ep.fourcc = "KB2i";
        ep.chroma_format = Bink2ChromaFormat::YCbCr420;
        ep.keyframe_interval = 4;
        ep.quant_y = 2;
        ep.quant_c = 2;
        ep.audio_track_count = 1;

        std::vector<uint8_t> container;
        SX_REQUIRE(Bink2EncodeTestStream(src_frames, ep, container).ok());

        Bink2Decoder dec;
        SX_REQUIRE(dec.OpenFromMemory(container).ok());
        SX_REQUIRE(dec.IsOpen());
        const auto& hdr = dec.Header();
        SX_REQUIRE(std::string(hdr.fourcc) == "KB2i");
        SX_REQUIRE(hdr.is_bink2);
        SX_REQUIRE(hdr.width == W && hdr.height == H);
        SX_REQUIRE(hdr.frame_count == kNumFrames);
        SX_REQUIRE(hdr.fps_num == 60u && hdr.fps_den == 1u);
        SX_REQUIRE(hdr.audio_track_count == 1u);
        SX_REQUIRE(hdr.frame_index[0].is_keyframe);
        SX_REQUIRE(!hdr.frame_index[1].is_keyframe);
        SX_REQUIRE(hdr.frame_index[4].is_keyframe);
        SX_REQUIRE(!hdr.frame_index[5].is_keyframe);
        // P-frame should be smaller than keyframe thanks to Skip + MotionCopy/Residual
        SX_REQUIRE(hdr.frame_index[1].size < hdr.frame_index[0].size);

        std::vector<Bink2DecodedFrame> seq_frames(kNumFrames);
        double min_psnr = 999.0;
        for (uint32_t f = 0; f < kNumFrames; ++f) {
            SX_REQUIRE(dec.DecodeFrame(f, Bink2PixelFormat::Rgba8, seq_frames[f]).ok());
            SX_REQUIRE(seq_frames[f].width == W && seq_frames[f].height == H);
            SX_REQUIRE(seq_frames[f].is_keyframe == ((f % 4u) == 0u));
            const double psnr = ComputeRgbPsnrDb(src_frames[f], seq_frames[f].pixels);
            min_psnr = std::min(min_psnr, psnr);
            SX_REQUIRE(psnr > 30.0);
        }

        // BGRA8 output check on frame 3
        Bink2DecodedFrame bgra_f3{};
        SX_REQUIRE(dec.SeekToFrame(3u, Bink2PixelFormat::Bgra8, bgra_f3).ok());
        SX_REQUIRE(bgra_f3.pixels.size() == seq_frames[3].pixels.size());
        for (size_t p = 0; p < static_cast<size_t>(W) * H; ++p) {
            SX_REQUIRE(bgra_f3.pixels[p * 4u + 0u] == seq_frames[3].pixels[p * 4u + 2u]);
            SX_REQUIRE(bgra_f3.pixels[p * 4u + 1u] == seq_frames[3].pixels[p * 4u + 1u]);
            SX_REQUIRE(bgra_f3.pixels[p * 4u + 2u] == seq_frames[3].pixels[p * 4u + 0u]);
            SX_REQUIRE(bgra_f3.pixels[p * 4u + 3u] == 255u);
        }

        // Random-access seeking on a fresh decoder must match sequential decoding bit-for-bit
        Bink2Decoder seek_dec;
        SX_REQUIRE(seek_dec.OpenFromMemory(container).ok());
        Bink2DecodedFrame sought6{}, sought2{};
        SX_REQUIRE(seek_dec.SeekToFrame(6u, Bink2PixelFormat::Rgba8, sought6).ok());
        SX_REQUIRE(sought6.pixels == seq_frames[6].pixels);
        SX_REQUIRE(seek_dec.SeekToFrame(2u, Bink2PixelFormat::Rgba8, sought2).ok());
        SX_REQUIRE(sought2.pixels == seq_frames[2].pixels);

        std::cout << "[Bink2][KB2i 4:2:0] container=" << container.size()
                  << "B key0=" << hdr.frame_index[0].size
                  << "B p1=" << hdr.frame_index[1].size
                  << "B minPSNR=" << min_psnr << "dB seek-exact OK\n";
    }

    // 3D. BIKi YCbCr 4:4:4 test + invalid container rejection
    {
        constexpr uint32_t W = 32u, H = 32u;
        std::vector<std::vector<uint8_t>> frames(2, std::vector<uint8_t>(W * H * 4u));
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = (static_cast<size_t>(y) * W + x) * 4u;
                frames[0][i + 0] = static_cast<uint8_t>(x * 8u);
                frames[0][i + 1] = static_cast<uint8_t>(y * 8u);
                frames[0][i + 2] = 180u;
                frames[0][i + 3] = 255u;
                frames[1][i + 0] = static_cast<uint8_t>(x * 8u);
                frames[1][i + 1] = static_cast<uint8_t>(y * 8u);
                frames[1][i + 2] = 120u;
                frames[1][i + 3] = 255u;
            }
        }
        Bink2EncodeParams ep{};
        ep.width = W;
        ep.height = H;
        ep.fourcc = "BIKi";
        ep.chroma_format = Bink2ChromaFormat::YCbCr444;
        ep.quant_y = 1;
        ep.quant_c = 1;

        std::vector<uint8_t> container;
        SX_REQUIRE(Bink2EncodeTestStream(frames, ep, container).ok());
        Bink2Decoder dec;
        SX_REQUIRE(dec.OpenFromMemory(container).ok());
        SX_REQUIRE(std::string(dec.Header().fourcc) == "BIKi");
        SX_REQUIRE(!dec.Header().is_bink2);
        SX_REQUIRE(dec.Header().chroma_format == Bink2ChromaFormat::YCbCr444);

        Bink2DecodedFrame f0{}, f1{};
        SX_REQUIRE(dec.DecodeFrame(0u, Bink2PixelFormat::Rgba8, f0).ok());
        SX_REQUIRE(dec.DecodeFrame(1u, Bink2PixelFormat::Rgba8, f1).ok());
        SX_REQUIRE(ComputeRgbPsnrDb(frames[0], f0.pixels) > 38.0);
        SX_REQUIRE(ComputeRgbPsnrDb(frames[1], f1.pixels) > 38.0);

        // Corrupt FourCC & truncated stream must be rejected
        std::vector<uint8_t> bad = container;
        bad[0] = 'X';
        SX_REQUIRE(!dec.OpenFromMemory(bad).ok());
        SX_REQUIRE(!dec.OpenFromMemory(container.data(), 20u).ok());
    }
}

// ===========================================================================
// 4. GPU FidelityFX Super Resolution (FSR 1.0 EASU + RCAS) Compute Pipeline Tests
// ===========================================================================
void TestGpuFsrPipeline() {
    using namespace scarlettx;

    FfxGpuFsrPipeline fsr;
    Status st = fsr.Initialize(true, false);  // Live D3D12 WARP compute
    SX_REQUIRE(st.ok());
    SX_REQUIRE(fsr.IsInitialized());

    // Build 64x64 diagonal-edge + checkerboard RGBA8 input pattern
    constexpr uint32_t kInW = 64u, kInH = 64u;
    constexpr uint32_t kOutW = 128u, kOutH = 128u;
    std::vector<uint8_t> input_rgba(static_cast<size_t>(kInW) * kInH * 4u, 255u);
    for (uint32_t y = 0; y < kInH; ++y) {
        for (uint32_t x = 0; x < kInW; ++x) {
            const size_t idx = (static_cast<size_t>(y) * kInW + x) * 4u;
            bool bright = false;
            if (y < kInH / 2u) {
                // Top half: crisp 45-degree diagonal split
                bright = (x + y >= 48u);
            } else {
                // Bottom half: 4x4 checkerboard pattern
                bright = (((x / 4u) ^ (y / 4u)) & 1u) != 0u;
            }
            input_rgba[idx + 0] = bright ? 235u : 22u;
            input_rgba[idx + 1] = bright ? 220u : 30u;
            input_rgba[idx + 2] = bright ? 245u : 40u;
            input_rgba[idx + 3] = 255u;
        }
    }

    // Dispatch 1: Full FSR 1.0 (EASU + RCAS)
    Fsr1DispatchParams params{};
    params.input_rgba8 = input_rgba.data();
    params.input_width = kInW;
    params.input_height = kInH;
    params.output_width = kOutW;
    params.output_height = kOutH;
    params.sharpness = 0.85f;
    params.enable_rcas = true;

    FrameBufferRgba8 fsr_out1{};
    Fsr1DispatchStats stats1{};
    st = fsr.Dispatch(params, fsr_out1, &stats1);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(fsr_out1.width == kOutW && fsr_out1.height == kOutH);
    SX_REQUIRE(fsr_out1.pixels.size() == static_cast<size_t>(kOutW) * kOutH * 4u);
    SX_REQUIRE(stats1.used_gpu_compute);
    SX_REQUIRE(stats1.easu_dxbc_bytes > 0u);
    SX_REQUIRE(stats1.rcas_dxbc_bytes > 0u);
    SX_REQUIRE(stats1.easu_groups_x == 16u && stats1.easu_groups_y == 16u);
    SX_REQUIRE(stats1.rcas_groups_x == 16u && stats1.rcas_groups_y == 16u);
    SX_REQUIRE(stats1.output_fnv1a_hash != 0u);

    // Check dark corner (2,2) and bright interior region (100,20)
    const size_t dark_idx = (2u * kOutW + 2u) * 4u;
    const size_t bright_idx = (20u * kOutW + 100u) * 4u;
    SX_REQUIRE(fsr_out1.pixels[dark_idx + 0] < 50u && fsr_out1.pixels[dark_idx + 3] == 255u);
    SX_REQUIRE(fsr_out1.pixels[bright_idx + 0] > 200u && fsr_out1.pixels[bright_idx + 3] == 255u);

    // Dispatch 2: Identical inputs must yield the exact same deterministic FNV-1a hash
    FrameBufferRgba8 fsr_out2{};
    Fsr1DispatchStats stats2{};
    SX_REQUIRE(fsr.Dispatch(params, fsr_out2, &stats2).ok());
    SX_REQUIRE(stats2.output_fnv1a_hash == stats1.output_fnv1a_hash);
    SX_REQUIRE(fsr_out2.pixels == fsr_out1.pixels);

    // Dispatch 3: EASU-only (enable_rcas = false) to verify RCAS adds measurable edge sharpness
    Fsr1DispatchParams easu_only_params = params;
    easu_only_params.enable_rcas = false;
    FrameBufferRgba8 easu_out{};
    SX_REQUIRE(fsr.Dispatch(easu_only_params, easu_out).ok());

    // CPU bilinear reference for edge sharpness comparison
    FrameBufferRgba8 bilinear_out{};
    SX_REQUIRE(FfxGpuFsrPipeline::UpscaleBilinearCpu(
                   input_rgba.data(), kInW, kInH, kOutW, kOutH, bilinear_out).ok());

    const double grad_bilinear = ComputeEdgeGradientEnergy(bilinear_out);
    const double grad_easu = ComputeEdgeGradientEnergy(easu_out);
    const double grad_fsr_rcas = ComputeEdgeGradientEnergy(fsr_out1);

    std::cout << "[FSR1 GPU] Adapter=\"" << stats1.adapter_description
              << "\" EASU=" << stats1.easu_dxbc_bytes << "B RCAS=" << stats1.rcas_dxbc_bytes
              << "B grad(bilinear)=" << grad_bilinear
              << " grad(EASU)=" << grad_easu
              << " grad(EASU+RCAS)=" << grad_fsr_rcas
              << " hash=0x" << std::hex << stats1.output_fnv1a_hash << std::dec << "\n";

    SX_REQUIRE(grad_easu > grad_bilinear);
    SX_REQUIRE(grad_fsr_rcas > grad_easu);
    SX_REQUIRE(grad_fsr_rcas > grad_bilinear * 1.05);

    // Dynamic resolution change on the same GPU pipeline (surface cache reallocation test)
    std::vector<uint8_t> small_in(32u * 24u * 4u, 180u);
    Fsr1DispatchParams resize_params{};
    resize_params.input_rgba8 = small_in.data();
    resize_params.input_width = 32u;
    resize_params.input_height = 24u;
    resize_params.output_width = 64u;
    resize_params.output_height = 48u;
    resize_params.sharpness = 0.5f;
    resize_params.enable_rcas = true;
    FrameBufferRgba8 resized_out{};
    SX_REQUIRE(fsr.Dispatch(resize_params, resized_out).ok());
    SX_REQUIRE(resized_out.width == 64u && resized_out.height == 48u);
    SX_REQUIRE(resized_out.pixels[0] >= 175u && resized_out.pixels[0] <= 185u);

    // Parameter validation checks
    Fsr1DispatchParams bad_params = params;
    bad_params.input_rgba8 = nullptr;
    SX_REQUIRE(!fsr.Dispatch(bad_params, resized_out).ok());
    bad_params = params;
    bad_params.output_width = 0u;
    SX_REQUIRE(!fsr.Dispatch(bad_params, resized_out).ok());
    bad_params = params;
    bad_params.sharpness = std::numeric_limits<float>::quiet_NaN();
    SX_REQUIRE(!fsr.Dispatch(bad_params, resized_out).ok());

    fsr.Shutdown();
    SX_REQUIRE(!fsr.IsInitialized());

    // Verify InitializeWithDevice using an external D3D12Backend device
    {
        D3D12Backend backend;
        SX_REQUIRE(backend.Initialize(true, false).ok());
        FfxGpuFsrPipeline ext_fsr;
        SX_REQUIRE(ext_fsr.InitializeWithDevice(backend.NativeDevice()).ok());
        FrameBufferRgba8 ext_out{};
        Fsr1DispatchStats ext_stats{};
        SX_REQUIRE(ext_fsr.Dispatch(params, ext_out, &ext_stats).ok());
        SX_REQUIRE(ext_stats.output_fnv1a_hash == stats1.output_fnv1a_hash);
        ext_fsr.Shutdown();
        backend.Shutdown();
    }
}

}  // namespace

int main() {
    TestBcPackCodec();
    TestXma2Decoder();
    TestBink2Decoder();
    TestGpuFsrPipeline();
    std::cout << "[PASS] test_production_codecs (BCPack + XMA2 IMDCT + Bink2 DCT/Motion + GPU FSR1 EASU/RCAS)\n";
    return 0;
}
