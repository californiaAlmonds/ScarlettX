#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

static constexpr uint32_t kXma2PacketSize = 2048u;
static constexpr uint32_t kXma2PacketHeaderBytes = 4u;
static constexpr uint32_t kXma2PacketPayloadBytes = kXma2PacketSize - kXma2PacketHeaderBytes;
static constexpr uint32_t kXma2DefaultBlockSize = 512u;
static constexpr uint32_t kXma2BarkBandCount = 24u;
static constexpr uint32_t kXma2NoFrameOffset = 0x7FFFu;

// Standard 4-byte big-endian XMA2 packet header (bits 31..0):
//   [31:26] frame_count          (6 bits)
//   [25:11] frame_offset_in_bits (15 bits, offset from bit 32 to first frame start)
//   [10:8]  packet_metadata      (3 bits, 1 for standard XMA2 stream)
//   [7:0]   packet_skip_count    (8 bits, packets to skip for multi-stream interleaving)
struct Xma2PacketHeader {
    uint8_t frame_count = 0;
    uint16_t frame_offset_in_bits = 0;
    uint8_t packet_metadata = 1;
    uint8_t packet_skip_count = 0;
};

struct Xma2Config {
    uint32_t sample_rate = 48000u;
    uint16_t channels = 2u;             // 1 (mono) or 2 (stereo)
    uint16_t block_size = kXma2DefaultBlockSize;  // MDCT hop size: 256, 512, or 1024
    uint8_t subframes_per_frame = 4u;   // 1, 2, or 4 subframes per XMA2 frame
    uint8_t quant_bits = 12u;           // Target quantization precision (6..14 bits)
};

struct Xma2DecodeStats {
    uint64_t packets_parsed = 0;
    uint64_t packets_skipped = 0;
    uint64_t frames_decoded = 0;
    uint64_t subframes_decoded = 0;
    uint64_t pcm_frames_output = 0;
};

// Parses a 4-byte big-endian XMA2 packet header from a packet buffer (>= 4 bytes).
Status ParseXma2PacketHeader(const uint8_t* packet_data, size_t packet_bytes, Xma2PacketHeader& out_header);

// Packs a 4-byte big-endian XMA2 packet header into `out_header_bytes`.
Status PackXma2PacketHeader(const Xma2PacketHeader& header, uint8_t out_header_bytes[4]);

// Type-IV Forward MDCT (windowed block of length 2*M -> M spectral coefficients)
// and Inverse MDCT (M spectral coefficients -> 2*M time-domain samples) using sine window.
void Xma2ForwardMdct(const float* windowed_in, float* spectral_out, uint32_t block_size);
void Xma2InverseMdct(const float* spectral_in, float* time_out, uint32_t block_size);
void Xma2ComputeSineWindow(float* window_out, uint32_t window_len);

// Clean-room encoder: transforms interleaved float32 PCM (in [-1, 1]) via windowed MDCT,
// Bark-band scale factor quantization, and bitstream packing into standard 2048-byte XMA2 packets.
Status Xma2EncodePcmToPackets(
    const float* interleaved_pcm,
    size_t pcm_frames_per_channel,
    const Xma2Config& config,
    std::vector<uint8_t>& out_packets);

class Xma2Decoder {
public:
    Xma2Decoder() = default;

    Status Initialize(const Xma2Config& config);
    void Reset();
    bool IsInitialized() const noexcept { return initialized_; }
    const Xma2Config& Config() const noexcept { return config_; }
    const Xma2DecodeStats& Stats() const noexcept { return stats_; }

    // Decodes a contiguous sequence of 2048-byte XMA2 packets into interleaved float32 PCM.
    Status DecodePackets(
        const uint8_t* packet_data,
        size_t packet_bytes,
        std::vector<float>& out_interleaved_pcm);

    // Decodes a contiguous sequence of 2048-byte XMA2 packets into interleaved int16 PCM.
    Status DecodePacketsInt16(
        const uint8_t* packet_data,
        size_t packet_bytes,
        std::vector<int16_t>& out_interleaved_pcm);

private:
    Xma2Config config_{};
    Xma2DecodeStats stats_{};
    bool initialized_ = false;
    bool primed_ = false;
    std::vector<float> sine_window_;
    std::vector<float> overlap_buf_[2];
};

}  // namespace scarlettx
