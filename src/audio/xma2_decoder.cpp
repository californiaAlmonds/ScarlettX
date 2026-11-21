#include "scarlettx/audio/xma2_decoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace scarlettx {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kPacketPayloadBits = kXma2PacketPayloadBytes * 8u;  // 16352 bits
constexpr uint32_t kEndOfFramesSentinel = 0x7FFFu;

constexpr std::array<uint32_t, 6> kSampleRateTable = {
    22050u, 24000u, 32000u, 44100u, 48000u, 16000u
};

constexpr std::array<float, kXma2BarkBandCount + 1u> kBarkEdgesHz = {
    0.0f,    100.0f,  200.0f,  300.0f,  400.0f,  510.0f,
    630.0f,  770.0f,  920.0f,  1080.0f, 1270.0f, 1480.0f,
    1720.0f, 2000.0f, 2320.0f, 2700.0f, 3150.0f, 3700.0f,
    4400.0f, 5300.0f, 6400.0f, 7700.0f, 9500.0f, 12000.0f,
    24000.0f
};

bool IsValidBlockSize(uint32_t block_size) noexcept {
    return block_size == 256u || block_size == 512u || block_size == 1024u;
}

uint32_t BlockSizeToCode(uint32_t block_size) noexcept {
    if (block_size == 256u) return 0u;
    if (block_size == 512u) return 1u;
    return 2u;  // 1024
}

uint32_t CodeToBlockSize(uint32_t code) noexcept {
    if (code == 0u) return 256u;
    if (code == 1u) return 512u;
    if (code == 2u) return 1024u;
    return 0u;
}

uint32_t SampleRateToCode(uint32_t sr) noexcept {
    for (uint32_t i = 0; i < kSampleRateTable.size(); ++i) {
        if (kSampleRateTable[i] == sr) {
            return i;
        }
    }
    return 4u;  // default 48000
}

uint32_t CodeToSampleRate(uint32_t code) noexcept {
    if (code < kSampleRateTable.size()) {
        return kSampleRateTable[code];
    }
    return 0u;
}

uint32_t CeilLog2(uint32_t v) noexcept {
    if (v <= 1u) return 1u;
    uint32_t bits = 0;
    uint32_t x = v - 1u;
    while (x > 0u) {
        ++bits;
        x >>= 1u;
    }
    return bits;
}

void ComputeBarkBandBins(
    uint32_t block_size,
    uint32_t sample_rate,
    std::array<uint32_t, kXma2BarkBandCount + 1u>& out_edges) noexcept {
    const float nyquist = 0.5f * static_cast<float>(sample_rate);
    out_edges[0] = 0u;
    for (uint32_t b = 1u; b < kXma2BarkBandCount; ++b) {
        const float frac = std::clamp(kBarkEdgesHz[b] / nyquist, 0.0f, 1.0f);
        uint32_t bin = static_cast<uint32_t>(std::round(frac * static_cast<float>(block_size)));
        // Ensure every band has at least 1 bin while staying inside [0, block_size]
        const uint32_t min_bin = out_edges[b - 1u] + 1u;
        const uint32_t max_bin = block_size - (kXma2BarkBandCount - b);
        if (min_bin <= max_bin) {
            bin = std::clamp(bin, min_bin, max_bin);
        } else {
            bin = std::min(out_edges[b - 1u], block_size);
        }
        out_edges[b] = bin;
    }
    out_edges[kXma2BarkBandCount] = block_size;
}

// ---------------------------------------------------------------------------
// Radix-2 Complex FFT & Exact Type-IV DCT (used for O(M log M) MDCT / IMDCT)
// ---------------------------------------------------------------------------
struct ComplexD {
    double re = 0.0;
    double im = 0.0;
};

void Radix2FftPosExp(std::vector<ComplexD>& data) {
    const size_t n = data.size();
    // Bit-reversal permutation
    size_t j = 0;
    for (size_t i = 1; i < n; ++i) {
        size_t bit = n >> 1u;
        while (j & bit) {
            j ^= bit;
            bit >>= 1u;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }

    // Cooley-Tukey butterflies with exp(+i * 2*pi*k / len)
    for (size_t len = 2; len <= n; len <<= 1u) {
        const double ang = 2.0 * kPi / static_cast<double>(len);
        const double wlen_re = std::cos(ang);
        const double wlen_im = std::sin(ang);
        const size_t half = len >> 1u;
        for (size_t i = 0; i < n; i += len) {
            double w_re = 1.0;
            double w_im = 0.0;
            for (size_t k = 0; k < half; ++k) {
                const ComplexD u = data[i + k];
                const ComplexD v = data[i + k + half];
                const double t_re = v.re * w_re - v.im * w_im;
                const double t_im = v.re * w_im + v.im * w_re;
                data[i + k] = {u.re + t_re, u.im + t_im};
                data[i + k + half] = {u.re - t_re, u.im - t_im};
                const double nw_re = w_re * wlen_re - w_im * wlen_im;
                const double nw_im = w_re * wlen_im + w_im * wlen_re;
                w_re = nw_re;
                w_im = nw_im;
            }
        }
    }
}

// Orthonormal Type-IV DCT of length M (power of 2 >= 4):
//   G[k] = sqrt(2/M) * sum_{n=0}^{M-1} f[n] * cos(pi/M * (n + 0.5) * (k + 0.5))
void ComputeDct4(const double* f, double* g, uint32_t M) {
    const uint32_t H = M / 2u;
    std::vector<ComplexD> z(H);
    const double inv_M = 1.0 / static_cast<double>(M);

    for (uint32_t m = 0; m < H; ++m) {
        const double a = f[2u * m];
        const double b = f[M - 1u - 2u * m];
        const double theta = kPi * static_cast<double>(m) * inv_M;
        const double c = std::cos(theta);
        const double s = std::sin(theta);
        // (a - i*b) * (c + i*s) = (a*c + b*s) + i*(a*s - b*c)
        z[m].re = a * c + b * s;
        z[m].im = a * s - b * c;
    }

    Radix2FftPosExp(z);

    const double scale = std::sqrt(2.0 * inv_M);
    for (uint32_t p = 0; p < H; ++p) {
        const double psi = kPi * (static_cast<double>(p) + 0.25) * inv_M;
        const double c = std::cos(psi);
        const double s = std::sin(psi);
        const double w_re = z[p].re * c - z[p].im * s;
        const double w_im = z[p].re * s + z[p].im * c;
        g[2u * p] = scale * w_re;
        g[M - 1u - 2u * p] = scale * w_im;
    }
}

// ---------------------------------------------------------------------------
// MSB-first BitWriter / BitReader
// ---------------------------------------------------------------------------
class BitWriter {
public:
    void WriteBits(uint32_t value, uint32_t num_bits) {
        for (uint32_t i = num_bits; i > 0u; --i) {
            bits_.push_back(static_cast<uint8_t>((value >> (i - 1u)) & 1u));
        }
    }

    void PatchBits(size_t bit_pos, uint32_t value, uint32_t num_bits) {
        for (uint32_t i = 0; i < num_bits; ++i) {
            bits_[bit_pos + i] = static_cast<uint8_t>((value >> (num_bits - 1u - i)) & 1u);
        }
    }

    size_t BitSize() const noexcept { return bits_.size(); }
    const std::vector<uint8_t>& Bits() const noexcept { return bits_; }

private:
    std::vector<uint8_t> bits_;
};

class BitReader {
public:
    explicit BitReader(const std::vector<uint8_t>& bits, size_t start_bit = 0)
        : bits_(bits), pos_(start_bit) {}

    bool CanRead(size_t n) const noexcept { return pos_ + n <= bits_.size(); }
    size_t Position() const noexcept { return pos_; }
    size_t Remaining() const noexcept { return (pos_ < bits_.size()) ? (bits_.size() - pos_) : 0u; }

    bool ReadBits(uint32_t num_bits, uint32_t& out_val) noexcept {
        if (num_bits > 32u || !CanRead(num_bits)) {
            return false;
        }
        uint32_t v = 0;
        for (uint32_t i = 0; i < num_bits; ++i) {
            v = (v << 1u) | (bits_[pos_++] & 1u);
        }
        out_val = v;
        return true;
    }

    void Seek(size_t bit_pos) noexcept {
        pos_ = std::min(bit_pos, bits_.size());
    }

private:
    const std::vector<uint8_t>& bits_;
    size_t pos_ = 0;
};

void EncodeChannelSpectrum(
    const float* spectral,
    uint32_t block_size,
    const std::array<uint32_t, kXma2BarkBandCount + 1u>& band_edges,
    uint8_t target_qbits,
    BitWriter& bw) {
    float peak = 0.0f;
    for (uint32_t k = 0; k < block_size; ++k) {
        peak = std::max(peak, std::fabs(spectral[k]));
    }

    if (peak < 1e-7f) {
        bw.WriteBits(1u, 1u);  // is_silent = 1
        return;
    }
    bw.WriteBits(0u, 1u);  // is_silent = 0

    const int global_exp = std::clamp(static_cast<int>(std::ceil(std::log2(peak))), -24, 23);
    const uint32_t global_exp_u = static_cast<uint32_t>(global_exp + 24);
    bw.WriteBits(global_exp_u, 6u);
    const float global_scale = std::ldexp(1.0f, global_exp);
    const float silence_thresh = global_scale * 1e-5f;

    const uint8_t base_qbits = std::clamp<uint8_t>(target_qbits, 6u, 14u);

    std::vector<int32_t> qvals;
    std::vector<uint32_t> nz_indices;

    for (uint32_t b = 0; b < kXma2BarkBandCount; ++b) {
        const uint32_t lo = band_edges[b];
        const uint32_t hi = band_edges[b + 1u];
        if (lo >= hi) {
            bw.WriteBits(0u, 1u);
            continue;
        }

        float band_peak = 0.0f;
        for (uint32_t k = lo; k < hi; ++k) {
            band_peak = std::max(band_peak, std::fabs(spectral[k]));
        }

        if (band_peak <= silence_thresh) {
            bw.WriteBits(0u, 1u);  // band_active = 0
            continue;
        }

        const int band_exp = std::clamp(static_cast<int>(std::ceil(std::log2(band_peak))), global_exp - 31, global_exp);
        const uint32_t exp_delta = static_cast<uint32_t>(std::clamp(global_exp - band_exp, 0, 31));
        const float band_scale = std::ldexp(1.0f, global_exp - static_cast<int>(exp_delta));

        // Adaptive bit allocation: slightly trim bits only for very faint bands (> 90 dB below peak)
        uint8_t qbits = base_qbits;
        if (exp_delta >= 20u && qbits > 8u) {
            qbits = static_cast<uint8_t>(qbits - 2u);
        }
        const int32_t qmax = (1 << (qbits - 1u)) - 1;
        const uint32_t band_len = hi - lo;

        qvals.resize(band_len);
        nz_indices.clear();
        for (uint32_t i = 0; i < band_len; ++i) {
            const float norm = spectral[lo + i] / band_scale;
            const int32_t q = std::clamp(static_cast<int32_t>(std::lround(norm * static_cast<float>(qmax))),
                                         -qmax, qmax);
            qvals[i] = q;
            if (q != 0) {
                nz_indices.push_back(i);
            }
        }

        if (nz_indices.empty()) {
            bw.WriteBits(0u, 1u);  // all quantized to zero
            continue;
        }

        bw.WriteBits(1u, 1u);  // band_active = 1
        bw.WriteBits(exp_delta, 5u);
        bw.WriteBits(static_cast<uint32_t>(qbits - 6u), 4u);

        const uint32_t idx_bits = CeilLog2(band_len);
        const uint32_t cnt_bits = CeilLog2(band_len + 1u);
        const size_t sparse_cost = 1u + cnt_bits + nz_indices.size() * (idx_bits + qbits);
        const size_t dense_cost = 1u + static_cast<size_t>(band_len) * qbits;

        if (sparse_cost < dense_cost) {
            bw.WriteBits(1u, 1u);  // sparse
            bw.WriteBits(static_cast<uint32_t>(nz_indices.size()), cnt_bits);
            for (uint32_t idx : nz_indices) {
                bw.WriteBits(idx, idx_bits);
                bw.WriteBits(static_cast<uint32_t>(qvals[idx] + qmax), qbits);
            }
        } else {
            bw.WriteBits(0u, 1u);  // dense
            for (uint32_t i = 0; i < band_len; ++i) {
                bw.WriteBits(static_cast<uint32_t>(qvals[i] + qmax), qbits);
            }
        }
    }
}

Status DecodeChannelSpectrum(
    BitReader& br,
    uint32_t block_size,
    const std::array<uint32_t, kXma2BarkBandCount + 1u>& band_edges,
    std::vector<float>& out_spectral) {
    out_spectral.assign(block_size, 0.0f);

    uint32_t is_silent = 0;
    if (!br.ReadBits(1u, is_silent)) {
        return Status::Error(StatusCode::ParseError, "XMA2 subframe truncated at silence flag");
    }
    if (is_silent) {
        return Status::Ok();
    }

    uint32_t global_exp_u = 0;
    if (!br.ReadBits(6u, global_exp_u)) {
        return Status::Error(StatusCode::ParseError, "XMA2 subframe truncated at global scale");
    }
    const int global_exp = static_cast<int>(global_exp_u) - 24;

    for (uint32_t b = 0; b < kXma2BarkBandCount; ++b) {
        const uint32_t lo = band_edges[b];
        const uint32_t hi = band_edges[b + 1u];

        uint32_t band_active = 0;
        if (!br.ReadBits(1u, band_active)) {
            return Status::Error(StatusCode::ParseError, "XMA2 subframe truncated at Bark band flag");
        }
        if (!band_active) {
            continue;
        }
        if (lo >= hi) {
            return Status::Error(StatusCode::ParseError, "XMA2 active Bark band has zero width");
        }

        uint32_t exp_delta = 0;
        uint32_t qbits_code = 0;
        uint32_t is_sparse = 0;
        if (!br.ReadBits(5u, exp_delta) || !br.ReadBits(4u, qbits_code) || !br.ReadBits(1u, is_sparse)) {
            return Status::Error(StatusCode::ParseError, "XMA2 Bark band header truncated");
        }
        const uint32_t qbits = qbits_code + 6u;
        if (qbits > 16u) {
            return Status::Error(StatusCode::ParseError, "XMA2 invalid quantization bit width");
        }

        const int32_t qmax = (1 << (qbits - 1u)) - 1;
        const float band_scale = std::ldexp(1.0f, global_exp - static_cast<int>(exp_delta));
        const float inv_qmax = band_scale / static_cast<float>(qmax);
        const uint32_t band_len = hi - lo;

        if (is_sparse) {
            const uint32_t idx_bits = CeilLog2(band_len);
            const uint32_t cnt_bits = CeilLog2(band_len + 1u);
            uint32_t nz_count = 0;
            if (!br.ReadBits(cnt_bits, nz_count) || nz_count > band_len) {
                return Status::Error(StatusCode::ParseError, "XMA2 sparse Bark band count invalid");
            }
            for (uint32_t i = 0; i < nz_count; ++i) {
                uint32_t local_idx = 0;
                uint32_t q_off = 0;
                if (!br.ReadBits(idx_bits, local_idx) || !br.ReadBits(qbits, q_off)) {
                    return Status::Error(StatusCode::ParseError, "XMA2 sparse Bark band entry truncated");
                }
                if (local_idx >= band_len) {
                    return Status::Error(StatusCode::ParseError, "XMA2 sparse Bark band index out of range");
                }
                const int32_t q = static_cast<int32_t>(q_off) - qmax;
                out_spectral[lo + local_idx] = static_cast<float>(q) * inv_qmax;
            }
        } else {
            for (uint32_t i = 0; i < band_len; ++i) {
                uint32_t q_off = 0;
                if (!br.ReadBits(qbits, q_off)) {
                    return Status::Error(StatusCode::ParseError, "XMA2 dense Bark band entry truncated");
                }
                const int32_t q = static_cast<int32_t>(q_off) - qmax;
                out_spectral[lo + i] = static_cast<float>(q) * inv_qmax;
            }
        }
    }

    return Status::Ok();
}

}  // namespace

Status ParseXma2PacketHeader(const uint8_t* packet_data, size_t packet_bytes, Xma2PacketHeader& out_header) {
    out_header = Xma2PacketHeader{};
    if (!packet_data || packet_bytes < kXma2PacketHeaderBytes) {
        return Status::Error(StatusCode::InvalidArgument, "XMA2 packet buffer too small for header");
    }

    const uint32_t be = (static_cast<uint32_t>(packet_data[0]) << 24u) |
                        (static_cast<uint32_t>(packet_data[1]) << 16u) |
                        (static_cast<uint32_t>(packet_data[2]) << 8u) |
                        static_cast<uint32_t>(packet_data[3]);

    out_header.frame_count = static_cast<uint8_t>((be >> 26u) & 0x3Fu);
    out_header.frame_offset_in_bits = static_cast<uint16_t>((be >> 11u) & 0x7FFFu);
    out_header.packet_metadata = static_cast<uint8_t>((be >> 8u) & 0x07u);
    out_header.packet_skip_count = static_cast<uint8_t>(be & 0xFFu);

    if (out_header.frame_offset_in_bits != kXma2NoFrameOffset &&
        out_header.frame_offset_in_bits >= kPacketPayloadBits) {
        return Status::Error(StatusCode::ParseError, "XMA2 packet frame_offset_in_bits exceeds packet payload");
    }
    return Status::Ok();
}

Status PackXma2PacketHeader(const Xma2PacketHeader& header, uint8_t out_header_bytes[4]) {
    if (!out_header_bytes) {
        return Status::Error(StatusCode::InvalidArgument, "Null output header pointer");
    }
    if (header.frame_count > 63u || header.packet_metadata > 7u ||
        (header.frame_offset_in_bits != kXma2NoFrameOffset &&
         header.frame_offset_in_bits >= kPacketPayloadBits)) {
        return Status::Error(StatusCode::InvalidArgument, "XMA2 packet header field out of range");
    }

    const uint32_t be = (static_cast<uint32_t>(header.frame_count) << 26u) |
                        (static_cast<uint32_t>(header.frame_offset_in_bits) << 11u) |
                        (static_cast<uint32_t>(header.packet_metadata) << 8u) |
                        static_cast<uint32_t>(header.packet_skip_count);

    out_header_bytes[0] = static_cast<uint8_t>((be >> 24u) & 0xFFu);
    out_header_bytes[1] = static_cast<uint8_t>((be >> 16u) & 0xFFu);
    out_header_bytes[2] = static_cast<uint8_t>((be >> 8u) & 0xFFu);
    out_header_bytes[3] = static_cast<uint8_t>(be & 0xFFu);
    return Status::Ok();
}

void Xma2ComputeSineWindow(float* window_out, uint32_t window_len) {
    if (!window_out || window_len == 0u) return;
    const double inv_n = 1.0 / static_cast<double>(window_len);
    for (uint32_t n = 0; n < window_len; ++n) {
        window_out[n] = static_cast<float>(std::sin(kPi * (static_cast<double>(n) + 0.5) * inv_n));
    }
}

void Xma2ForwardMdct(const float* windowed_in, float* spectral_out, uint32_t block_size) {
    if (!windowed_in || !spectral_out || block_size < 4u || (block_size & (block_size - 1u)) != 0u) {
        return;
    }
    const uint32_t M = block_size;
    const uint32_t half_M = M / 2u;

    std::vector<double> folded(M);
    std::vector<double> dct(M);
    for (uint32_t j = 0; j < half_M; ++j) {
        folded[j] = -static_cast<double>(windowed_in[M + half_M - 1u - j]) -
                    static_cast<double>(windowed_in[M + half_M + j]);
        folded[half_M + j] = static_cast<double>(windowed_in[j]) -
                             static_cast<double>(windowed_in[M - 1u - j]);
    }

    ComputeDct4(folded.data(), dct.data(), M);
    for (uint32_t k = 0; k < M; ++k) {
        spectral_out[k] = static_cast<float>(dct[k]);
    }
}

void Xma2InverseMdct(const float* spectral_in, float* time_out, uint32_t block_size) {
    if (!spectral_in || !time_out || block_size < 4u || (block_size & (block_size - 1u)) != 0u) {
        return;
    }
    const uint32_t M = block_size;
    const uint32_t half_M = M / 2u;

    std::vector<double> spec(M);
    std::vector<double> g(M);
    for (uint32_t k = 0; k < M; ++k) {
        spec[k] = static_cast<double>(spectral_in[k]);
    }

    ComputeDct4(spec.data(), g.data(), M);

    for (uint32_t r = 0; r < half_M; ++r) {
        const float a = static_cast<float>(g[half_M + r]);
        const float b = static_cast<float>(g[r]);
        time_out[r] = a;
        time_out[M - 1u - r] = -a;
        time_out[M + half_M - 1u - r] = -b;
        time_out[M + half_M + r] = -b;
    }
}

Status Xma2EncodePcmToPackets(
    const float* interleaved_pcm,
    size_t pcm_frames_per_channel,
    const Xma2Config& config,
    std::vector<uint8_t>& out_packets) {
    out_packets.clear();
    if (!interleaved_pcm || pcm_frames_per_channel == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "XMA2 encoder requires non-empty PCM input");
    }
    if (config.channels != 1u && config.channels != 2u) {
        return Status::Error(StatusCode::InvalidArgument, "XMA2 supports 1 (mono) or 2 (stereo) channels");
    }
    if (!IsValidBlockSize(config.block_size)) {
        return Status::Error(StatusCode::InvalidArgument, "XMA2 block_size must be 256, 512, or 1024");
    }
    if (config.subframes_per_frame != 1u && config.subframes_per_frame != 2u && config.subframes_per_frame != 4u) {
        return Status::Error(StatusCode::InvalidArgument, "XMA2 subframes_per_frame must be 1, 2, or 4");
    }

    const uint32_t M = config.block_size;
    const uint32_t N = 2u * M;
    const uint16_t ch_count = config.channels;

    std::vector<float> window(N);
    Xma2ComputeSineWindow(window.data(), N);

    std::array<uint32_t, kXma2BarkBandCount + 1u> band_edges{};
    ComputeBarkBandBins(M, config.sample_rate, band_edges);

    // Total subframes needed for zero-shift TDAC overlap-add:
    //   Subframe 0 = priming [zeros(M), pcm[0..M-1]]
    //   Subframe s (1..num_hops-1) = [pcm[(s-1)M..sM-1], pcm[sM..(s+1)M-1]]
    //   Subframe num_hops = flushing [pcm[(num_hops-1)M..], zeros(M)]
    const size_t num_hops = (pcm_frames_per_channel + M - 1u) / M;
    const size_t total_subframes = num_hops + 1u;

    auto get_sample = [&](size_t frame_idx, uint16_t ch) -> float {
        if (frame_idx >= pcm_frames_per_channel) {
            return 0.0f;
        }
        return interleaved_pcm[frame_idx * ch_count + ch];
    };

    // Encode each subframe independently first, then group up to `subframes_per_frame`
    // subframes per XMA2 frame while guaranteeing 15-bit frame_size_bits < 0x7FFF.
    std::vector<std::vector<uint8_t>> subframe_bits(total_subframes);
    std::vector<float> windowed(N);
    std::vector<float> spectral(M);

    for (size_t global_sf = 0; global_sf < total_subframes; ++global_sf) {
        BitWriter sfbw;
        for (uint16_t ch = 0; ch < ch_count; ++ch) {
            for (uint32_t n = 0; n < M; ++n) {
                const float first_half = (global_sf == 0u)
                                             ? 0.0f
                                             : get_sample((global_sf - 1u) * M + n, ch);
                const float second_half = get_sample(global_sf * M + n, ch);
                windowed[n] = first_half * window[n];
                windowed[M + n] = second_half * window[M + n];
            }
            Xma2ForwardMdct(windowed.data(), spectral.data(), M);
            EncodeChannelSpectrum(spectral.data(), M, band_edges, config.quant_bits, sfbw);
        }
        subframe_bits[global_sf] = sfbw.Bits();
    }

    constexpr size_t kFrameHeaderBits = 38u;
    std::vector<std::vector<uint8_t>> encoded_frames;
    size_t sf_cursor = 0;
    size_t samples_accounted = 0;

    while (sf_cursor < total_subframes) {
        uint8_t sf_in_frame = 0;
        size_t payload_bits = kFrameHeaderBits;
        const size_t max_sf = std::min<size_t>(config.subframes_per_frame, total_subframes - sf_cursor);
        while (sf_in_frame < max_sf) {
            const size_t next_bits = subframe_bits[sf_cursor + sf_in_frame].size();
            if (sf_in_frame > 0u && payload_bits + next_bits >= kEndOfFramesSentinel) {
                break;
            }
            payload_bits += next_bits;
            ++sf_in_frame;
        }
        if (payload_bits >= kEndOfFramesSentinel) {
            return Status::Error(StatusCode::OutOfMemory, "Single XMA2 subframe exceeds 15-bit frame length limit");
        }

        // Calculate how many valid output PCM samples this frame yields (excluding subframe 0 priming)
        size_t output_hops = 0;
        for (uint8_t s = 0; s < sf_in_frame; ++s) {
            if (sf_cursor + s > 0u) {
                ++output_hops;
            }
        }
        const size_t remaining_pcm = (samples_accounted < pcm_frames_per_channel)
                                         ? (pcm_frames_per_channel - samples_accounted)
                                         : 0u;
        const uint32_t valid_pcm_in_frame = static_cast<uint32_t>(
            std::min<size_t>(output_hops * M, remaining_pcm));
        samples_accounted += valid_pcm_in_frame;

        BitWriter bw;
        bw.WriteBits(static_cast<uint32_t>(payload_bits), 15u);
        bw.WriteBits(1u, 1u);   // has_subframe_config
        bw.WriteBits(static_cast<uint32_t>(ch_count - 1u), 2u);
        bw.WriteBits(SampleRateToCode(config.sample_rate), 3u);
        bw.WriteBits(BlockSizeToCode(M), 2u);
        bw.WriteBits(static_cast<uint32_t>(sf_in_frame - 1u), 2u);  // 0..3 -> 1..4 subframes
        bw.WriteBits(valid_pcm_in_frame, 13u);  // up to 4096 samples per frame

        std::vector<uint8_t> frame_vec = bw.Bits();
        frame_vec.reserve(payload_bits);
        for (uint8_t s = 0; s < sf_in_frame; ++s) {
            const auto& sb = subframe_bits[sf_cursor + s];
            frame_vec.insert(frame_vec.end(), sb.begin(), sb.end());
        }
        encoded_frames.push_back(std::move(frame_vec));
        sf_cursor += sf_in_frame;
    }

    // Pack all frames continuously into standard 2048-byte XMA2 packets
    struct PacketBuilder {
        Xma2PacketHeader hdr{};
        std::array<uint8_t, kXma2PacketSize> bytes{};
        uint32_t bit_cursor = 0;
        bool has_first_frame = false;

        PacketBuilder() {
            hdr.frame_count = 0;
            hdr.frame_offset_in_bits = kXma2NoFrameOffset;
            hdr.packet_metadata = 1u;
            hdr.packet_skip_count = 0u;
            bytes.fill(0xFFu);  // Unused trailing payload bits are 1s (0x7FFF sentinel)
        }

        void WritePayloadBit(uint8_t bit) {
            const uint32_t byte_idx = kXma2PacketHeaderBytes + (bit_cursor >> 3u);
            const uint32_t bit_in_byte = 7u - (bit_cursor & 7u);
            if (bit) {
                bytes[byte_idx] |= static_cast<uint8_t>(1u << bit_in_byte);
            } else {
                bytes[byte_idx] &= static_cast<uint8_t>(~(1u << bit_in_byte));
            }
            ++bit_cursor;
        }
    };

    std::vector<PacketBuilder> packets;
    packets.emplace_back();

    for (const auto& fbits : encoded_frames) {
        // Ensure we don't overflow 6-bit frame_count (max 63 frames starting in one packet)
        if (packets.back().bit_cursor >= kPacketPayloadBits || packets.back().hdr.frame_count >= 63u) {
            packets.emplace_back();
        }
        auto& start_pkt = packets.back();
        if (!start_pkt.has_first_frame) {
            start_pkt.hdr.frame_offset_in_bits = static_cast<uint16_t>(start_pkt.bit_cursor);
            start_pkt.has_first_frame = true;
        }
        ++start_pkt.hdr.frame_count;

        for (uint8_t bit : fbits) {
            if (packets.back().bit_cursor >= kPacketPayloadBits) {
                packets.emplace_back();
            }
            packets.back().WritePayloadBit(bit);
        }
    }

    out_packets.reserve(packets.size() * kXma2PacketSize);
    for (auto& pkt : packets) {
        Status st = PackXma2PacketHeader(pkt.hdr, pkt.bytes.data());
        if (!st.ok()) {
            out_packets.clear();
            return st;
        }
        out_packets.insert(out_packets.end(), pkt.bytes.begin(), pkt.bytes.end());
    }

    return Status::Ok();
}

Status Xma2Decoder::Initialize(const Xma2Config& config) {
    if (config.channels != 1u && config.channels != 2u) {
        return Status::Error(StatusCode::InvalidArgument, "XMA2 decoder supports 1 or 2 channels");
    }
    if (!IsValidBlockSize(config.block_size)) {
        return Status::Error(StatusCode::InvalidArgument, "XMA2 block_size must be 256, 512, or 1024");
    }
    config_ = config;
    sine_window_.resize(static_cast<size_t>(config_.block_size) * 2u);
    Xma2ComputeSineWindow(sine_window_.data(), static_cast<uint32_t>(sine_window_.size()));
    initialized_ = true;
    Reset();
    return Status::Ok();
}

void Xma2Decoder::Reset() {
    stats_ = Xma2DecodeStats{};
    primed_ = false;
    for (int ch = 0; ch < 2; ++ch) {
        overlap_buf_[ch].assign(config_.block_size, 0.0f);
    }
}

Status Xma2Decoder::DecodePackets(
    const uint8_t* packet_data,
    size_t packet_bytes,
    std::vector<float>& out_interleaved_pcm) {
    out_interleaved_pcm.clear();
    if (!initialized_) {
        return Status::Error(StatusCode::DeviceError, "Xma2Decoder is not initialized");
    }
    if (!packet_data || packet_bytes < kXma2PacketSize || (packet_bytes % kXma2PacketSize) != 0u) {
        return Status::Error(StatusCode::InvalidArgument, "XMA2 input must be a non-empty multiple of 2048 bytes");
    }

    const size_t num_packets = packet_bytes / kXma2PacketSize;
    std::vector<uint8_t> stream_bits;
    stream_bits.reserve(num_packets * kPacketPayloadBits);

    bool found_first_offset = false;
    size_t start_bit = 0;
    size_t pkt_idx = 0;

    while (pkt_idx < num_packets) {
        const uint8_t* pkt = packet_data + pkt_idx * kXma2PacketSize;
        Xma2PacketHeader hdr{};
        Status st = ParseXma2PacketHeader(pkt, kXma2PacketSize, hdr);
        if (!st.ok()) {
            return st;
        }
        if (hdr.packet_metadata != 1u) {
            return Status::Error(StatusCode::UnsupportedFormat, "Unsupported XMA2 packet metadata (expected 1)");
        }

        const size_t base_bit = stream_bits.size();
        if (!found_first_offset && hdr.frame_offset_in_bits != kXma2NoFrameOffset) {
            start_bit = base_bit + hdr.frame_offset_in_bits;
            found_first_offset = true;
        }

        for (uint32_t b = kXma2PacketHeaderBytes; b < kXma2PacketSize; ++b) {
            const uint8_t byte_val = pkt[b];
            for (int bit = 7; bit >= 0; --bit) {
                stream_bits.push_back(static_cast<uint8_t>((byte_val >> bit) & 1u));
            }
        }

        ++stats_.packets_parsed;
        const size_t skip = hdr.packet_skip_count;
        if (skip > 0u) {
            const size_t actual_skip = std::min(skip, (num_packets - 1u) - pkt_idx);
            stats_.packets_skipped += actual_skip;
            pkt_idx += actual_skip;
        }
        ++pkt_idx;
    }

    if (!found_first_offset) {
        return Status::Error(StatusCode::ParseError, "No XMA2 frame start found in packet stream");
    }

    BitReader br(stream_bits, start_bit);
    std::vector<float> spectral;
    std::vector<float> imdct_out;

    while (br.Remaining() >= 15u) {
        const size_t frame_start = br.Position();
        uint32_t frame_size_bits = 0;
        if (!br.ReadBits(15u, frame_size_bits)) {
            break;
        }
        if (frame_size_bits == kEndOfFramesSentinel || frame_size_bits == 0u) {
            break;
        }
        if (frame_size_bits < 38u || frame_start + frame_size_bits > stream_bits.size()) {
            return Status::Error(StatusCode::ParseError, "XMA2 frame_size_bits invalid or truncated");
        }

        uint32_t has_cfg = 0;
        uint32_t ch_minus1 = 0;
        uint32_t sr_code = 0;
        uint32_t bs_code = 0;
        uint32_t sf_minus1 = 0;
        uint32_t valid_pcm_in_frame = 0;
        if (!br.ReadBits(1u, has_cfg) ||
            !br.ReadBits(2u, ch_minus1) ||
            !br.ReadBits(3u, sr_code) ||
            !br.ReadBits(2u, bs_code) ||
            !br.ReadBits(2u, sf_minus1) ||
            !br.ReadBits(13u, valid_pcm_in_frame)) {
            return Status::Error(StatusCode::ParseError, "XMA2 frame header truncated");
        }

        const uint16_t frame_channels = static_cast<uint16_t>(ch_minus1 + 1u);
        const uint32_t frame_sr = CodeToSampleRate(sr_code);
        const uint32_t frame_M = CodeToBlockSize(bs_code);
        const uint8_t sf_count = static_cast<uint8_t>(sf_minus1 + 1u);

        if (frame_channels != config_.channels || frame_sr == 0u || frame_M == 0u) {
            return Status::Error(StatusCode::UnsupportedFormat, "XMA2 frame parameters incompatible with decoder config");
        }
        if (frame_M != config_.block_size) {
            config_.block_size = static_cast<uint16_t>(frame_M);
            sine_window_.resize(static_cast<size_t>(frame_M) * 2u);
            Xma2ComputeSineWindow(sine_window_.data(), static_cast<uint32_t>(sine_window_.size()));
            for (int c = 0; c < 2; ++c) {
                overlap_buf_[c].assign(frame_M, 0.0f);
            }
            primed_ = false;
        }

        std::array<uint32_t, kXma2BarkBandCount + 1u> band_edges{};
        ComputeBarkBandBins(frame_M, frame_sr, band_edges);

        imdct_out.resize(static_cast<size_t>(frame_M) * 2u);
        std::vector<float> frame_pcm;
        frame_pcm.reserve(static_cast<size_t>(sf_count) * frame_M * frame_channels);

        for (uint8_t sf = 0; sf < sf_count; ++sf) {
            std::vector<float> ch_out[2];
            for (uint16_t ch = 0; ch < frame_channels; ++ch) {
                Status st = DecodeChannelSpectrum(br, frame_M, band_edges, spectral);
                if (!st.ok()) {
                    return st;
                }
                Xma2InverseMdct(spectral.data(), imdct_out.data(), frame_M);

                if (primed_) {
                    ch_out[ch].resize(frame_M);
                    for (uint32_t n = 0; n < frame_M; ++n) {
                        ch_out[ch][n] = overlap_buf_[ch][n] + sine_window_[n] * imdct_out[n];
                    }
                }
                for (uint32_t n = 0; n < frame_M; ++n) {
                    overlap_buf_[ch][n] = sine_window_[frame_M + n] * imdct_out[frame_M + n];
                }
            }

            if (primed_) {
                for (uint32_t n = 0; n < frame_M; ++n) {
                    for (uint16_t ch = 0; ch < frame_channels; ++ch) {
                        frame_pcm.push_back(ch_out[ch][n]);
                    }
                }
            } else {
                primed_ = true;
            }
            ++stats_.subframes_decoded;
        }

        const size_t produced_frames = frame_pcm.size() / frame_channels;
        const size_t keep_frames = std::min<size_t>(produced_frames, valid_pcm_in_frame);
        out_interleaved_pcm.insert(
            out_interleaved_pcm.end(),
            frame_pcm.begin(),
            frame_pcm.begin() + static_cast<ptrdiff_t>(keep_frames * frame_channels));

        stats_.pcm_frames_output += keep_frames;
        ++stats_.frames_decoded;

        // Advance cleanly to the exact frame boundary
        br.Seek(frame_start + frame_size_bits);
    }

    return Status::Ok();
}

Status Xma2Decoder::DecodePacketsInt16(
    const uint8_t* packet_data,
    size_t packet_bytes,
    std::vector<int16_t>& out_interleaved_pcm) {
    out_interleaved_pcm.clear();
    std::vector<float> f32;
    Status st = DecodePackets(packet_data, packet_bytes, f32);
    if (!st.ok()) {
        return st;
    }

    out_interleaved_pcm.resize(f32.size());
    for (size_t i = 0; i < f32.size(); ++i) {
        const float clamped = std::clamp(f32[i], -1.0f, 1.0f);
        const long scaled = std::lround(clamped * 32767.0f);
        out_interleaved_pcm[i] = static_cast<int16_t>(std::clamp(scaled, -32768L, 32767L));
    }
    return Status::Ok();
}

}  // namespace scarlettx
