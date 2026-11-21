#include "scarlettx/middleware/bink2_decoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace scarlettx {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kBink2FlagChroma444 = 0x01000000u;
constexpr size_t kBinkHeaderBytes = 44u;
constexpr size_t kFrameHeaderBytes = 8u;

enum BlockMode : uint8_t {
    kBlockSkip = 0,
    kBlockIntraFill = 1,
    kBlockIntraDct = 2,
    kBlockMotionCopy = 3,
    kBlockMotionResidual = 4,
};

constexpr std::array<uint8_t, 64> kZigZag = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

// Gentle perceptual quantization weights (scaled by quant_y or quant_c)
constexpr std::array<float, 64> kQuantWeight = {
    1.0f, 1.0f, 1.2f, 1.4f, 1.6f, 2.0f, 2.4f, 2.8f,
    1.0f, 1.2f, 1.3f, 1.5f, 1.8f, 2.2f, 2.6f, 3.0f,
    1.2f, 1.3f, 1.5f, 1.7f, 2.0f, 2.5f, 2.9f, 3.3f,
    1.4f, 1.5f, 1.7f, 2.0f, 2.4f, 2.8f, 3.2f, 3.6f,
    1.6f, 1.8f, 2.0f, 2.4f, 2.8f, 3.2f, 3.6f, 4.0f,
    2.0f, 2.2f, 2.5f, 2.8f, 3.2f, 3.6f, 4.0f, 4.4f,
    2.4f, 2.6f, 2.9f, 3.2f, 3.6f, 4.0f, 4.4f, 4.8f,
    2.8f, 3.0f, 3.3f, 3.6f, 4.0f, 4.4f, 4.8f, 5.2f,
};

const std::array<std::array<float, 8>, 8>& GetDctMatrix() noexcept {
    static const std::array<std::array<float, 8>, 8> mat = []() {
        std::array<std::array<float, 8>, 8> m{};
        for (int u = 0; u < 8; ++u) {
            const double alpha = (u == 0) ? std::sqrt(1.0 / 8.0) : std::sqrt(2.0 / 8.0);
            for (int x = 0; x < 8; ++x) {
                m[u][x] = static_cast<float>(alpha * std::cos((2.0 * x + 1.0) * u * kPi / 16.0));
            }
        }
        return m;
    }();
    return mat;
}

uint16_t ReadLe16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}

uint32_t ReadLe32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

void AppendLe16(std::vector<uint8_t>& dst, uint16_t v) {
    dst.push_back(static_cast<uint8_t>(v & 0xFFu));
    dst.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
}

void AppendLe32(std::vector<uint8_t>& dst, uint32_t v) {
    dst.push_back(static_cast<uint8_t>(v & 0xFFu));
    dst.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
    dst.push_back(static_cast<uint8_t>((v >> 16u) & 0xFFu));
    dst.push_back(static_cast<uint8_t>((v >> 24u) & 0xFFu));
}

uint8_t ClampU8(float v) noexcept {
    const int iv = static_cast<int>(std::lround(v));
    return static_cast<uint8_t>(std::clamp(iv, 0, 255));
}

bool IsSupportedFourCC(const char fourcc[4], bool& out_is_bink2) noexcept {
    if (fourcc[0] == 'K' && fourcc[1] == 'B' && fourcc[2] == '2' &&
        (fourcc[3] == 'i' || fourcc[3] == 'j' || fourcc[3] == 'k' || fourcc[3] == 'n' || fourcc[3] == 'h')) {
        out_is_bink2 = true;
        return true;
    }
    if (fourcc[0] == 'B' && fourcc[1] == 'I' && fourcc[2] == 'K' &&
        (fourcc[3] == 'i' || fourcc[3] == 'b' || fourcc[3] == 'f' || fourcc[3] == 'g' || fourcc[3] == 'h')) {
        out_is_bink2 = false;
        return true;
    }
    return false;
}

void SampleBlockClamped(
    const std::vector<uint8_t>& plane,
    uint32_t plane_w,
    uint32_t plane_h,
    int32_t base_x,
    int32_t base_y,
    float out_block[64]) noexcept {
    const int32_t max_x = static_cast<int32_t>(plane_w) - 1;
    const int32_t max_y = static_cast<int32_t>(plane_h) - 1;
    for (int32_t y = 0; y < 8; ++y) {
        const int32_t sy = std::clamp(base_y + y, 0, max_y);
        const uint8_t* row = plane.data() + static_cast<size_t>(sy) * plane_w;
        for (int32_t x = 0; x < 8; ++x) {
            const int32_t sx = std::clamp(base_x + x, 0, max_x);
            out_block[y * 8 + x] = static_cast<float>(row[sx]);
        }
    }
}

void StoreBlockClamped(
    std::vector<uint8_t>& plane,
    uint32_t plane_w,
    uint32_t plane_h,
    uint32_t base_x,
    uint32_t base_y,
    const float in_block[64]) noexcept {
    const uint32_t lim_y = std::min<uint32_t>(8u, plane_h - base_y);
    const uint32_t lim_x = std::min<uint32_t>(8u, plane_w - base_x);
    for (uint32_t y = 0; y < lim_y; ++y) {
        uint8_t* row = plane.data() + static_cast<size_t>(base_y + y) * plane_w + base_x;
        for (uint32_t x = 0; x < lim_x; ++x) {
            row[x] = ClampU8(in_block[y * 8u + x]);
        }
    }
}

void WriteQuantizedDctBlock(const int16_t q[64], std::vector<uint8_t>& dst) {
    AppendLe16(dst, static_cast<uint16_t>(q[0]));
    uint8_t zero_run = 0;
    for (size_t z = 1u; z < 64u; ++z) {
        const int16_t level = q[kZigZag[z]];
        if (level == 0) {
            ++zero_run;
            continue;
        }
        if (level >= -127 && level <= 127) {
            dst.push_back(static_cast<uint8_t>(1u + zero_run));
            dst.push_back(static_cast<uint8_t>(static_cast<int8_t>(level)));
        } else {
            dst.push_back(static_cast<uint8_t>(0x40u + zero_run));
            AppendLe16(dst, static_cast<uint16_t>(level));
        }
        zero_run = 0;
    }
    dst.push_back(0x00u);  // EOB
}

Status ReadQuantizedDctBlock(const uint8_t* src, size_t src_size, size_t& pos, int16_t q_out[64]) {
    std::memset(q_out, 0, sizeof(int16_t) * 64u);
    if (pos + 2u > src_size) {
        return Status::Error(StatusCode::ParseError, "Bink2 DCT DC coefficient truncated");
    }
    q_out[0] = static_cast<int16_t>(ReadLe16(src + pos));
    pos += 2u;

    size_t z = 1u;
    while (z < 64u) {
        if (pos >= src_size) {
            return Status::Error(StatusCode::ParseError, "Bink2 DCT AC stream truncated");
        }
        const uint8_t ctrl = src[pos++];
        if (ctrl == 0x00u) {
            return Status::Ok();  // EOB
        }
        if (ctrl < 0x40u) {
            const size_t run = static_cast<size_t>(ctrl - 1u);
            if (pos >= src_size || z + run >= 64u) {
                return Status::Error(StatusCode::ParseError, "Bink2 DCT AC run out of bounds");
            }
            z += run;
            q_out[kZigZag[z++]] = static_cast<int8_t>(src[pos++]);
        } else if (ctrl < 0x80u) {
            const size_t run = static_cast<size_t>(ctrl - 0x40u);
            if (pos + 2u > src_size || z + run >= 64u) {
                return Status::Error(StatusCode::ParseError, "Bink2 DCT AC int16 run out of bounds");
            }
            z += run;
            q_out[kZigZag[z++]] = static_cast<int16_t>(ReadLe16(src + pos));
            pos += 2u;
        } else {
            return Status::Error(StatusCode::ParseError, "Bink2 invalid DCT control byte");
        }
    }

    // Consume trailing EOB if block filled all 63 AC entries
    if (pos < src_size && src[pos] == 0x00u) {
        ++pos;
    }
    return Status::Ok();
}

void QuantizeBlock(const float dct[64], float qstep, int16_t q_out[64], float dequant_out[64]) noexcept {
    for (size_t i = 0; i < 64u; ++i) {
        const float step = std::max(1.0f, kQuantWeight[i] * qstep);
        const long qv = std::lround(dct[i] / step);
        const int16_t clamped = static_cast<int16_t>(std::clamp(qv, -32767L, 32767L));
        q_out[i] = clamped;
        dequant_out[i] = static_cast<float>(clamped) * step;
    }
}

void DequantizeBlock(const int16_t q[64], float qstep, float dct_out[64]) noexcept {
    for (size_t i = 0; i < 64u; ++i) {
        const float step = std::max(1.0f, kQuantWeight[i] * qstep);
        dct_out[i] = static_cast<float>(q[i]) * step;
    }
}

int32_t ComputeBlockSad(const float a[64], const float b[64]) noexcept {
    int32_t sad = 0;
    for (int i = 0; i < 64; ++i) {
        sad += static_cast<int32_t>(std::fabs(a[i] - b[i]));
    }
    return sad;
}

void EncodePlane(
    const std::vector<uint8_t>& cur_plane,
    const std::vector<uint8_t>& ref_plane,
    std::vector<uint8_t>& recon_plane,
    uint32_t plane_w,
    uint32_t plane_h,
    bool is_keyframe,
    uint8_t quant_step,
    std::vector<uint8_t>& out_stream) {
    recon_plane.assign(static_cast<size_t>(plane_w) * plane_h, 0u);
    const uint32_t bw = (plane_w + 7u) / 8u;
    const uint32_t bh = (plane_h + 7u) / 8u;
    const float qstep = static_cast<float>(std::max<uint8_t>(1u, quant_step));

    float cur_blk[64];
    float pred_blk[64];
    float work_blk[64];
    float dct_blk[64];
    float deq_blk[64];
    float idct_blk[64];
    int16_t q_blk[64];

    int8_t prev_mv_x = 0;
    int8_t prev_mv_y = 0;

    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const int32_t px = static_cast<int32_t>(bx * 8u);
            const int32_t py = static_cast<int32_t>(by * 8u);
            SampleBlockClamped(cur_plane, plane_w, plane_h, px, py, cur_blk);

            // Check if block is constant (IntraFill candidate)
            bool is_flat = true;
            const float first_val = cur_blk[0];
            for (int i = 1; i < 64; ++i) {
                if (std::fabs(cur_blk[i] - first_val) > 0.5f) {
                    is_flat = false;
                    break;
                }
            }

            if (!is_keyframe && !ref_plane.empty()) {
                // 1. Check zero-MV Skip
                SampleBlockClamped(ref_plane, plane_w, plane_h, px, py, pred_blk);
                int32_t best_sad = ComputeBlockSad(cur_blk, pred_blk);
                int8_t best_mvx = 0;
                int8_t best_mvy = 0;

                if (best_sad <= 16) {
                    out_stream.push_back(kBlockSkip);
                    StoreBlockClamped(recon_plane, plane_w, plane_h, bx * 8u, by * 8u, pred_blk);
                    prev_mv_x = 0;
                    prev_mv_y = 0;
                    continue;
                }

                // 2. Check previous MV predictor first, then small integer search [-8..+8]
                auto test_mv = [&](int32_t mx, int32_t my) {
                    if (mx < -16 || mx > 16 || my < -16 || my > 16) return;
                    float cand[64];
                    SampleBlockClamped(ref_plane, plane_w, plane_h, px + mx, py + my, cand);
                    const int32_t sad = ComputeBlockSad(cur_blk, cand);
                    if (sad < best_sad) {
                        best_sad = sad;
                        best_mvx = static_cast<int8_t>(mx);
                        best_mvy = static_cast<int8_t>(my);
                    }
                };

                if (prev_mv_x != 0 || prev_mv_y != 0) {
                    test_mv(prev_mv_x, prev_mv_y);
                }
                if (best_sad > 16) {
                    // Coarse search step 2 in [-8, 8], then refine +-1 around best
                    for (int32_t my = -8; my <= 8; my += 2) {
                        for (int32_t mx = -8; mx <= 8; mx += 2) {
                            test_mv(mx, my);
                        }
                    }
                    const int32_t cx = best_mvx;
                    const int32_t cy = best_mvy;
                    for (int32_t dy = -1; dy <= 1; ++dy) {
                        for (int32_t dx = -1; dx <= 1; ++dx) {
                            test_mv(cx + dx, cy + dy);
                        }
                    }
                }

                SampleBlockClamped(ref_plane, plane_w, plane_h, px + best_mvx, py + best_mvy, pred_blk);
                prev_mv_x = best_mvx;
                prev_mv_y = best_mvy;

                if (best_sad <= 16) {
                    out_stream.push_back(kBlockMotionCopy);
                    out_stream.push_back(static_cast<uint8_t>(best_mvx));
                    out_stream.push_back(static_cast<uint8_t>(best_mvy));
                    StoreBlockClamped(recon_plane, plane_w, plane_h, bx * 8u, by * 8u, pred_blk);
                    continue;
                }

                // Compare MotionResidual vs Intra block energy
                int32_t intra_energy = 0;
                float mean = 0.0f;
                for (int i = 0; i < 64; ++i) mean += cur_blk[i];
                mean /= 64.0f;
                for (int i = 0; i < 64; ++i) intra_energy += static_cast<int32_t>(std::fabs(cur_blk[i] - mean));

                if (!is_flat && best_sad <= intra_energy + 128) {
                    for (int i = 0; i < 64; ++i) {
                        work_blk[i] = cur_blk[i] - pred_blk[i];
                    }
                    Bink2ForwardDct8x8(work_blk, dct_blk);
                    QuantizeBlock(dct_blk, qstep, q_blk, deq_blk);
                    Bink2InverseDct8x8(deq_blk, idct_blk);
                    for (int i = 0; i < 64; ++i) {
                        work_blk[i] = std::clamp(pred_blk[i] + idct_blk[i], 0.0f, 255.0f);
                    }
                    out_stream.push_back(kBlockMotionResidual);
                    out_stream.push_back(static_cast<uint8_t>(best_mvx));
                    out_stream.push_back(static_cast<uint8_t>(best_mvy));
                    WriteQuantizedDctBlock(q_blk, out_stream);
                    StoreBlockClamped(recon_plane, plane_w, plane_h, bx * 8u, by * 8u, work_blk);
                    continue;
                }
            }

            if (is_flat) {
                const uint8_t fill = ClampU8(first_val);
                out_stream.push_back(kBlockIntraFill);
                out_stream.push_back(fill);
                for (int i = 0; i < 64; ++i) work_blk[i] = static_cast<float>(fill);
                StoreBlockClamped(recon_plane, plane_w, plane_h, bx * 8u, by * 8u, work_blk);
            } else {
                for (int i = 0; i < 64; ++i) {
                    work_blk[i] = cur_blk[i] - 128.0f;
                }
                Bink2ForwardDct8x8(work_blk, dct_blk);
                QuantizeBlock(dct_blk, qstep, q_blk, deq_blk);
                Bink2InverseDct8x8(deq_blk, idct_blk);
                for (int i = 0; i < 64; ++i) {
                    work_blk[i] = std::clamp(idct_blk[i] + 128.0f, 0.0f, 255.0f);
                }
                out_stream.push_back(kBlockIntraDct);
                WriteQuantizedDctBlock(q_blk, out_stream);
                StoreBlockClamped(recon_plane, plane_w, plane_h, bx * 8u, by * 8u, work_blk);
            }
        }
    }
}

Status DecodePlane(
    const uint8_t* src,
    size_t src_size,
    size_t& pos,
    const std::vector<uint8_t>& ref_plane,
    std::vector<uint8_t>& dst_plane,
    uint32_t plane_w,
    uint32_t plane_h,
    bool is_keyframe,
    uint8_t quant_step) {
    dst_plane.assign(static_cast<size_t>(plane_w) * plane_h, 0u);
    const uint32_t bw = (plane_w + 7u) / 8u;
    const uint32_t bh = (plane_h + 7u) / 8u;
    const float qstep = static_cast<float>(std::max<uint8_t>(1u, quant_step));

    float pred_blk[64];
    float dct_blk[64];
    float idct_blk[64];
    float out_blk[64];
    int16_t q_blk[64];

    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            if (pos >= src_size) {
                return Status::Error(StatusCode::ParseError, "Bink2 plane bitstream truncated at block mode");
            }
            const uint8_t mode = src[pos++];
            const int32_t px = static_cast<int32_t>(bx * 8u);
            const int32_t py = static_cast<int32_t>(by * 8u);

            switch (mode) {
                case kBlockSkip: {
                    if (is_keyframe || ref_plane.empty()) {
                        return Status::Error(StatusCode::ParseError, "Bink2 Skip block invalid in keyframe");
                    }
                    SampleBlockClamped(ref_plane, plane_w, plane_h, px, py, pred_blk);
                    StoreBlockClamped(dst_plane, plane_w, plane_h, bx * 8u, by * 8u, pred_blk);
                    break;
                }
                case kBlockIntraFill: {
                    if (pos >= src_size) {
                        return Status::Error(StatusCode::ParseError, "Bink2 IntraFill value truncated");
                    }
                    const float fill = static_cast<float>(src[pos++]);
                    for (int i = 0; i < 64; ++i) out_blk[i] = fill;
                    StoreBlockClamped(dst_plane, plane_w, plane_h, bx * 8u, by * 8u, out_blk);
                    break;
                }
                case kBlockIntraDct: {
                    Status st = ReadQuantizedDctBlock(src, src_size, pos, q_blk);
                    if (!st.ok()) return st;
                    DequantizeBlock(q_blk, qstep, dct_blk);
                    Bink2InverseDct8x8(dct_blk, idct_blk);
                    for (int i = 0; i < 64; ++i) {
                        out_blk[i] = std::clamp(idct_blk[i] + 128.0f, 0.0f, 255.0f);
                    }
                    StoreBlockClamped(dst_plane, plane_w, plane_h, bx * 8u, by * 8u, out_blk);
                    break;
                }
                case kBlockMotionCopy: {
                    if (is_keyframe || ref_plane.empty() || pos + 2u > src_size) {
                        return Status::Error(StatusCode::ParseError, "Bink2 MotionCopy block invalid");
                    }
                    const int8_t mvx = static_cast<int8_t>(src[pos++]);
                    const int8_t mvy = static_cast<int8_t>(src[pos++]);
                    SampleBlockClamped(ref_plane, plane_w, plane_h, px + mvx, py + mvy, pred_blk);
                    StoreBlockClamped(dst_plane, plane_w, plane_h, bx * 8u, by * 8u, pred_blk);
                    break;
                }
                case kBlockMotionResidual: {
                    if (is_keyframe || ref_plane.empty() || pos + 2u > src_size) {
                        return Status::Error(StatusCode::ParseError, "Bink2 MotionResidual header invalid");
                    }
                    const int8_t mvx = static_cast<int8_t>(src[pos++]);
                    const int8_t mvy = static_cast<int8_t>(src[pos++]);
                    Status st = ReadQuantizedDctBlock(src, src_size, pos, q_blk);
                    if (!st.ok()) return st;
                    SampleBlockClamped(ref_plane, plane_w, plane_h, px + mvx, py + mvy, pred_blk);
                    DequantizeBlock(q_blk, qstep, dct_blk);
                    Bink2InverseDct8x8(dct_blk, idct_blk);
                    for (int i = 0; i < 64; ++i) {
                        out_blk[i] = std::clamp(pred_blk[i] + idct_blk[i], 0.0f, 255.0f);
                    }
                    StoreBlockClamped(dst_plane, plane_w, plane_h, bx * 8u, by * 8u, out_blk);
                    break;
                }
                default:
                    return Status::Error(StatusCode::ParseError, "Bink2 unknown macroblock mode");
            }
        }
    }
    return Status::Ok();
}

}  // namespace

void Bink2ForwardDct8x8(const float in[64], float out[64]) noexcept {
    const auto& C = GetDctMatrix();
    float tmp[64];
    // Row transform: tmp[y][v] = sum_x in[y][x] * C[v][x]
    for (int y = 0; y < 8; ++y) {
        for (int v = 0; v < 8; ++v) {
            float sum = 0.0f;
            for (int x = 0; x < 8; ++x) {
                sum += in[y * 8 + x] * C[v][x];
            }
            tmp[y * 8 + v] = sum;
        }
    }
    // Column transform: out[u][v] = sum_y C[u][y] * tmp[y][v]
    for (int u = 0; u < 8; ++u) {
        for (int v = 0; v < 8; ++v) {
            float sum = 0.0f;
            for (int y = 0; y < 8; ++y) {
                sum += C[u][y] * tmp[y * 8 + v];
            }
            out[u * 8 + v] = sum;
        }
    }
}

void Bink2InverseDct8x8(const float in[64], float out[64]) noexcept {
    const auto& C = GetDctMatrix();
    float tmp[64];
    // Row inverse: tmp[u][x] = sum_v in[u][v] * C[v][x]
    for (int u = 0; u < 8; ++u) {
        for (int x = 0; x < 8; ++x) {
            float sum = 0.0f;
            for (int v = 0; v < 8; ++v) {
                sum += in[u * 8 + v] * C[v][x];
            }
            tmp[u * 8 + x] = sum;
        }
    }
    // Column inverse: out[y][x] = sum_u C[u][y] * tmp[u][x]
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            float sum = 0.0f;
            for (int u = 0; u < 8; ++u) {
                sum += C[u][y] * tmp[u * 8 + x];
            }
            out[y * 8 + x] = sum;
        }
    }
}

void Bink2RgbToYCbCr709(uint8_t r, uint8_t g, uint8_t b, uint8_t& y, uint8_t& cb, uint8_t& cr) noexcept {
    const float rf = static_cast<float>(r);
    const float gf = static_cast<float>(g);
    const float bf = static_cast<float>(b);
    const float yf = 0.212600f * rf + 0.715200f * gf + 0.072200f * bf;
    const float cbf = 128.0f - 0.114572f * rf - 0.385428f * gf + 0.500000f * bf;
    const float crf = 128.0f + 0.500000f * rf - 0.454153f * gf - 0.045847f * bf;
    y = ClampU8(yf);
    cb = ClampU8(cbf);
    cr = ClampU8(crf);
}

void Bink2YCbCr709ToRgb(uint8_t y, uint8_t cb, uint8_t cr, uint8_t& r, uint8_t& g, uint8_t& b) noexcept {
    const float yf = static_cast<float>(y);
    const float cbf = static_cast<float>(cb) - 128.0f;
    const float crf = static_cast<float>(cr) - 128.0f;
    r = ClampU8(yf + 1.574800f * crf);
    g = ClampU8(yf - 0.187324f * cbf - 0.468124f * crf);
    b = ClampU8(yf + 1.855600f * cbf);
}

Status Bink2EncodeTestStream(
    const std::vector<std::vector<uint8_t>>& rgba_frames,
    const Bink2EncodeParams& params,
    std::vector<uint8_t>& out_container) {
    out_container.clear();
    if (rgba_frames.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Bink2 encoder requires at least 1 frame");
    }
    if (params.width == 0 || params.height == 0 || params.width > 7680u || params.height > 4320u) {
        return Status::Error(StatusCode::InvalidArgument, "Bink2 dimensions out of range");
    }
    if (params.fps_num == 0 || params.fps_den == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Bink2 framerate must be non-zero");
    }
    if (params.fourcc.size() != 4u) {
        return Status::Error(StatusCode::InvalidArgument, "Bink2 FourCC must be 4 characters");
    }
    bool is_bink2 = false;
    if (!IsSupportedFourCC(params.fourcc.data(), is_bink2)) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported Bink2 FourCC signature");
    }

    const size_t expected_rgba_bytes = static_cast<size_t>(params.width) * params.height * 4u;
    for (const auto& f : rgba_frames) {
        if (f.size() != expected_rgba_bytes) {
            return Status::Error(StatusCode::InvalidArgument, "Frame RGBA buffer size mismatch");
        }
    }

    const uint32_t w = params.width;
    const uint32_t h = params.height;
    const bool is_444 = (params.chroma_format == Bink2ChromaFormat::YCbCr444);
    const uint32_t cw = is_444 ? w : ((w + 1u) / 2u);
    const uint32_t ch = is_444 ? h : ((h + 1u) / 2u);
    const uint32_t key_interval = std::max<uint32_t>(1u, params.keyframe_interval);

    std::vector<uint8_t> cur_y(static_cast<size_t>(w) * h);
    std::vector<uint8_t> cur_cb(static_cast<size_t>(cw) * ch);
    std::vector<uint8_t> cur_cr(static_cast<size_t>(cw) * ch);

    std::vector<uint8_t> ref_y, ref_cb, ref_cr;
    std::vector<uint8_t> recon_y, recon_cb, recon_cr;

    struct EncodedFrame {
        bool is_keyframe = false;
        std::vector<uint8_t> payload;
    };
    std::vector<EncodedFrame> encoded_frames;
    encoded_frames.reserve(rgba_frames.size());

    uint32_t largest_frame = 0;

    for (size_t fi = 0; fi < rgba_frames.size(); ++fi) {
        const uint8_t* rgba = rgba_frames[fi].data();
        const bool is_key = (fi == 0u) || ((fi % key_interval) == 0u);

        // Convert RGBA8 -> BT.709 YCbCr planes
        if (is_444) {
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const size_t pi = static_cast<size_t>(y) * w + x;
                    Bink2RgbToYCbCr709(rgba[pi * 4u + 0u], rgba[pi * 4u + 1u], rgba[pi * 4u + 2u],
                                       cur_y[pi], cur_cb[pi], cur_cr[pi]);
                }
            }
        } else {
            std::vector<uint8_t> full_cb(static_cast<size_t>(w) * h);
            std::vector<uint8_t> full_cr(static_cast<size_t>(w) * h);
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const size_t pi = static_cast<size_t>(y) * w + x;
                    Bink2RgbToYCbCr709(rgba[pi * 4u + 0u], rgba[pi * 4u + 1u], rgba[pi * 4u + 2u],
                                       cur_y[pi], full_cb[pi], full_cr[pi]);
                }
            }
            // 2x2 box-filter downsample Cb and Cr planes for YCbCr 4:2:0
            for (uint32_t cy = 0; cy < ch; ++cy) {
                const uint32_t y0 = cy * 2u;
                const uint32_t y1 = std::min(y0 + 1u, h - 1u);
                for (uint32_t cx = 0; cx < cw; ++cx) {
                    const uint32_t x0 = cx * 2u;
                    const uint32_t x1 = std::min(x0 + 1u, w - 1u);
                    const uint32_t cb_sum =
                        static_cast<uint32_t>(full_cb[static_cast<size_t>(y0) * w + x0]) +
                        static_cast<uint32_t>(full_cb[static_cast<size_t>(y0) * w + x1]) +
                        static_cast<uint32_t>(full_cb[static_cast<size_t>(y1) * w + x0]) +
                        static_cast<uint32_t>(full_cb[static_cast<size_t>(y1) * w + x1]);
                    const uint32_t cr_sum =
                        static_cast<uint32_t>(full_cr[static_cast<size_t>(y0) * w + x0]) +
                        static_cast<uint32_t>(full_cr[static_cast<size_t>(y0) * w + x1]) +
                        static_cast<uint32_t>(full_cr[static_cast<size_t>(y1) * w + x0]) +
                        static_cast<uint32_t>(full_cr[static_cast<size_t>(y1) * w + x1]);
                    cur_cb[static_cast<size_t>(cy) * cw + cx] = static_cast<uint8_t>((cb_sum + 2u) >> 2u);
                    cur_cr[static_cast<size_t>(cy) * cw + cx] = static_cast<uint8_t>((cr_sum + 2u) >> 2u);
                }
            }
        }

        EncodedFrame ef;
        ef.is_keyframe = is_key;
        ef.payload.push_back(is_key ? 0u : 1u);
        ef.payload.push_back(static_cast<uint8_t>(params.chroma_format));
        ef.payload.push_back(std::max<uint8_t>(1u, params.quant_y));
        ef.payload.push_back(std::max<uint8_t>(1u, params.quant_c));
        AppendLe32(ef.payload, 0x324B4246u);  // 'FBK2' frame plane marker

        EncodePlane(cur_y, ref_y, recon_y, w, h, is_key, params.quant_y, ef.payload);
        EncodePlane(cur_cb, ref_cb, recon_cb, cw, ch, is_key, params.quant_c, ef.payload);
        EncodePlane(cur_cr, ref_cr, recon_cr, cw, ch, is_key, params.quant_c, ef.payload);

        // Enforce 2-byte alignment so frame offsets always have bit 0 clear for keyframe flag
        if ((ef.payload.size() & 1u) != 0u) {
            ef.payload.push_back(0u);
        }

        largest_frame = std::max<uint32_t>(largest_frame, static_cast<uint32_t>(ef.payload.size()));
        ref_y = std::move(recon_y);
        ref_cb = std::move(recon_cb);
        ref_cr = std::move(recon_cr);
        encoded_frames.push_back(std::move(ef));
    }

    const uint32_t frame_count = static_cast<uint32_t>(encoded_frames.size());
    const uint32_t audio_bytes = params.audio_track_count * 12u;
    const uint32_t index_table_bytes = (frame_count + 1u) * 4u;
    const uint32_t data_start = static_cast<uint32_t>(kBinkHeaderBytes) + audio_bytes + index_table_bytes;

    std::vector<uint32_t> index_entries;
    index_entries.reserve(frame_count + 1u);
    uint32_t cursor = data_start;
    for (const auto& ef : encoded_frames) {
        index_entries.push_back(cursor | (ef.is_keyframe ? 1u : 0u));
        cursor += static_cast<uint32_t>(ef.payload.size());
    }
    index_entries.push_back(cursor);  // End offset of final frame

    const uint32_t total_size = cursor;
    const uint32_t video_flags = is_444 ? kBink2FlagChroma444 : 0u;

    out_container.reserve(total_size);
    // 44-byte Bink / Bink2 container header
    out_container.insert(out_container.end(), params.fourcc.begin(), params.fourcc.end());
    AppendLe32(out_container, total_size - 8u);
    AppendLe32(out_container, frame_count);
    AppendLe32(out_container, largest_frame);
    AppendLe32(out_container, frame_count);
    AppendLe32(out_container, w);
    AppendLe32(out_container, h);
    AppendLe32(out_container, params.fps_num);
    AppendLe32(out_container, params.fps_den);
    AppendLe32(out_container, video_flags);
    AppendLe32(out_container, params.audio_track_count);

    // Optional audio track metadata headers (12 bytes per track)
    for (uint32_t t = 0; t < params.audio_track_count; ++t) {
        AppendLe32(out_container, 4096u);      // max_in_packet_bytes
        AppendLe32(out_container, 48000u);     // sample rate & flags
        AppendLe32(out_container, t);          // track id
    }

    // Frame offset & keyframe index table
    for (uint32_t entry : index_entries) {
        AppendLe32(out_container, entry);
    }

    // Concatenated frame payloads
    for (const auto& ef : encoded_frames) {
        out_container.insert(out_container.end(), ef.payload.begin(), ef.payload.end());
    }

    return Status::Ok();
}

Status Bink2Decoder::OpenFromMemory(const uint8_t* data, size_t size) {
    if (!data || size == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "Bink2 buffer is null or empty");
    }
    return OpenFromMemory(std::vector<uint8_t>(data, data + size));
}

Status Bink2Decoder::OpenFromMemory(std::vector<uint8_t> data) {
    Close();
    if (data.size() < kBinkHeaderBytes) {
        return Status::Error(StatusCode::ParseError, "Bink2 container too small for 44-byte header");
    }

    bool is_bink2 = false;
    if (!IsSupportedFourCC(reinterpret_cast<const char*>(data.data()), is_bink2)) {
        return Status::Error(StatusCode::UnsupportedFormat, "Invalid Bink/Bink2 FourCC signature");
    }

    Bink2ContainerHeader hdr{};
    std::memcpy(hdr.fourcc, data.data(), 4u);
    hdr.fourcc[4] = '\0';
    hdr.is_bink2 = is_bink2;
    hdr.file_size_field = ReadLe32(data.data() + 4u);
    hdr.frame_count = ReadLe32(data.data() + 8u);
    hdr.largest_frame_size = ReadLe32(data.data() + 12u);
    hdr.width = ReadLe32(data.data() + 20u);
    hdr.height = ReadLe32(data.data() + 24u);
    hdr.fps_num = ReadLe32(data.data() + 28u);
    hdr.fps_den = ReadLe32(data.data() + 32u);
    hdr.video_flags = ReadLe32(data.data() + 36u);
    hdr.audio_track_count = ReadLe32(data.data() + 40u);
    hdr.chroma_format = (hdr.video_flags & kBink2FlagChroma444)
                            ? Bink2ChromaFormat::YCbCr444
                            : Bink2ChromaFormat::YCbCr420;

    if (hdr.frame_count == 0u || hdr.frame_count > 1000000u) {
        return Status::Error(StatusCode::ParseError, "Bink2 invalid frame_count");
    }
    if (hdr.width == 0u || hdr.width > 7680u || hdr.height == 0u || hdr.height > 4320u) {
        return Status::Error(StatusCode::ParseError, "Bink2 invalid video dimensions");
    }
    if (hdr.fps_num == 0u || hdr.fps_den == 0u) {
        return Status::Error(StatusCode::ParseError, "Bink2 invalid framerate");
    }
    if (hdr.audio_track_count > 64u) {
        return Status::Error(StatusCode::ParseError, "Bink2 excessive audio track count");
    }

    const uint64_t index_offset = kBinkHeaderBytes + static_cast<uint64_t>(hdr.audio_track_count) * 12ull;
    const uint64_t index_bytes = (static_cast<uint64_t>(hdr.frame_count) + 1ull) * 4ull;
    const uint64_t data_start = index_offset + index_bytes;
    if (data_start > data.size()) {
        return Status::Error(StatusCode::ParseError, "Bink2 frame index table truncated");
    }

    hdr.frame_index.resize(hdr.frame_count);
    uint32_t prev_raw = ReadLe32(data.data() + index_offset);
    uint32_t prev_off = prev_raw & ~1u;
    bool prev_key = (prev_raw & 1u) != 0u;
    if (prev_off < data_start || prev_off > data.size()) {
        return Status::Error(StatusCode::ParseError, "Bink2 first frame offset out of bounds");
    }

    for (uint32_t i = 0; i < hdr.frame_count; ++i) {
        const uint32_t next_raw = ReadLe32(data.data() + index_offset + static_cast<size_t>(i + 1u) * 4u);
        const uint32_t next_off = next_raw & ~1u;
        if (next_off < prev_off || next_off > data.size()) {
            return Status::Error(StatusCode::ParseError, "Bink2 frame index offsets non-monotonic or out of bounds");
        }
        hdr.frame_index[i].offset = prev_off;
        hdr.frame_index[i].size = next_off - prev_off;
        hdr.frame_index[i].is_keyframe = prev_key;

        prev_off = next_off;
        prev_key = (next_raw & 1u) != 0u;
    }

    container_ = std::move(data);
    header_ = std::move(hdr);
    plane_w_y_ = header_.width;
    plane_h_y_ = header_.height;
    const bool is_444 = (header_.chroma_format == Bink2ChromaFormat::YCbCr444);
    plane_w_c_ = is_444 ? header_.width : ((header_.width + 1u) / 2u);
    plane_h_c_ = is_444 ? header_.height : ((header_.height + 1u) / 2u);

    plane_y_.assign(static_cast<size_t>(plane_w_y_) * plane_h_y_, 0u);
    plane_cb_.assign(static_cast<size_t>(plane_w_c_) * plane_h_c_, 128u);
    plane_cr_.assign(static_cast<size_t>(plane_w_c_) * plane_h_c_, 128u);
    ref_y_.clear();
    ref_cb_.clear();
    ref_cr_.clear();
    current_frame_ = 0u;
    has_decoded_frame_ = false;
    last_decoded_frame_ = 0u;
    is_open_ = true;
    return Status::Ok();
}

void Bink2Decoder::Close() {
    is_open_ = false;
    container_.clear();
    header_ = Bink2ContainerHeader{};
    plane_y_.clear();
    plane_cb_.clear();
    plane_cr_.clear();
    ref_y_.clear();
    ref_cb_.clear();
    ref_cr_.clear();
    plane_w_y_ = plane_h_y_ = plane_w_c_ = plane_h_c_ = 0u;
    current_frame_ = 0u;
    has_decoded_frame_ = false;
    last_decoded_frame_ = 0u;
}

Status Bink2Decoder::DecodeSingleFrameInternal(uint32_t frame_index) {
    const auto& entry = header_.frame_index[frame_index];
    if (entry.size < kFrameHeaderBytes ||
        static_cast<uint64_t>(entry.offset) + entry.size > container_.size()) {
        return Status::Error(StatusCode::ParseError, "Bink2 frame payload truncated");
    }

    const uint8_t* ptr = container_.data() + entry.offset;
    const uint8_t frame_type = ptr[0];
    const auto chroma = static_cast<Bink2ChromaFormat>(ptr[1]);
    const uint8_t quant_y = ptr[2];
    const uint8_t quant_c = ptr[3];
    const uint32_t marker = ReadLe32(ptr + 4u);
    if (marker != 0x324B4246u) {
        return Status::Error(StatusCode::ParseError, "Bink2 frame marker mismatch");
    }
    if (chroma != header_.chroma_format) {
        return Status::Error(StatusCode::ParseError, "Bink2 frame chroma format mismatch");
    }

    const bool is_key = (frame_type == 0u);
    if (!is_key && (ref_y_.empty() || ref_cb_.empty() || ref_cr_.empty())) {
        return Status::Error(StatusCode::ParseError, "Bink2 P-frame decoded without prior reference frame");
    }

    size_t pos = kFrameHeaderBytes;
    Status st = DecodePlane(ptr, entry.size, pos, ref_y_, plane_y_, plane_w_y_, plane_h_y_, is_key, quant_y);
    if (!st.ok()) return st;
    st = DecodePlane(ptr, entry.size, pos, ref_cb_, plane_cb_, plane_w_c_, plane_h_c_, is_key, quant_c);
    if (!st.ok()) return st;
    st = DecodePlane(ptr, entry.size, pos, ref_cr_, plane_cr_, plane_w_c_, plane_h_c_, is_key, quant_c);
    if (!st.ok()) return st;

    ref_y_ = plane_y_;
    ref_cb_ = plane_cb_;
    ref_cr_ = plane_cr_;
    current_frame_ = frame_index;
    last_decoded_frame_ = frame_index;
    has_decoded_frame_ = true;
    return Status::Ok();
}

void Bink2Decoder::ConvertPlanesToPixels(Bink2PixelFormat pixel_format, std::vector<uint8_t>& out_pixels) const {
    const uint32_t w = header_.width;
    const uint32_t h = header_.height;
    const bool is_444 = (header_.chroma_format == Bink2ChromaFormat::YCbCr444);
    const bool is_bgra = (pixel_format == Bink2PixelFormat::Bgra8);

    out_pixels.resize(static_cast<size_t>(w) * h * 4u);

    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t cy = is_444 ? y : std::min(y >> 1u, plane_h_c_ - 1u);
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t cx = is_444 ? x : std::min(x >> 1u, plane_w_c_ - 1u);
            const uint8_t yv = plane_y_[static_cast<size_t>(y) * plane_w_y_ + x];
            const uint8_t cbv = plane_cb_[static_cast<size_t>(cy) * plane_w_c_ + cx];
            const uint8_t crv = plane_cr_[static_cast<size_t>(cy) * plane_w_c_ + cx];

            uint8_t r = 0, g = 0, b = 0;
            Bink2YCbCr709ToRgb(yv, cbv, crv, r, g, b);

            const size_t dst_off = (static_cast<size_t>(y) * w + x) * 4u;
            if (is_bgra) {
                out_pixels[dst_off + 0u] = b;
                out_pixels[dst_off + 1u] = g;
                out_pixels[dst_off + 2u] = r;
                out_pixels[dst_off + 3u] = 255u;
            } else {
                out_pixels[dst_off + 0u] = r;
                out_pixels[dst_off + 1u] = g;
                out_pixels[dst_off + 2u] = b;
                out_pixels[dst_off + 3u] = 255u;
            }
        }
    }
}

Status Bink2Decoder::DecodeFrame(
    uint32_t frame_index,
    Bink2PixelFormat pixel_format,
    Bink2DecodedFrame& out_frame) {
    if (!is_open_) {
        return Status::Error(StatusCode::DeviceError, "Bink2Decoder is not open");
    }
    if (frame_index >= header_.frame_count) {
        return Status::Error(StatusCode::InvalidArgument, "Bink2 frame index out of range");
    }

    // If sequential next frame or keyframe, decode directly; otherwise seek via nearest keyframe
    if (header_.frame_index[frame_index].is_keyframe ||
        (has_decoded_frame_ && frame_index == last_decoded_frame_ + 1u)) {
        Status st = DecodeSingleFrameInternal(frame_index);
        if (!st.ok()) return st;
    } else if (!has_decoded_frame_ || frame_index != last_decoded_frame_) {
        return SeekToFrame(frame_index, pixel_format, out_frame);
    }

    out_frame.frame_index = frame_index;
    out_frame.width = header_.width;
    out_frame.height = header_.height;
    out_frame.is_keyframe = header_.frame_index[frame_index].is_keyframe;
    out_frame.pixel_format = pixel_format;
    ConvertPlanesToPixels(pixel_format, out_frame.pixels);
    return Status::Ok();
}

Status Bink2Decoder::SeekToFrame(
    uint32_t frame_index,
    Bink2PixelFormat pixel_format,
    Bink2DecodedFrame& out_frame) {
    if (!is_open_) {
        return Status::Error(StatusCode::DeviceError, "Bink2Decoder is not open");
    }
    if (frame_index >= header_.frame_count) {
        return Status::Error(StatusCode::InvalidArgument, "Bink2 seek frame index out of range");
    }

    // Find nearest preceding keyframe <= frame_index
    uint32_t key_idx = frame_index;
    while (key_idx > 0u && !header_.frame_index[key_idx].is_keyframe) {
        --key_idx;
    }
    if (!header_.frame_index[key_idx].is_keyframe) {
        return Status::Error(StatusCode::ParseError, "Bink2 stream has no preceding keyframe for seek");
    }

    // If we already decoded between key_idx and frame_index, continue forward; otherwise start at key_idx
    uint32_t start = key_idx;
    if (has_decoded_frame_ && last_decoded_frame_ >= key_idx && last_decoded_frame_ <= frame_index) {
        start = last_decoded_frame_ + 1u;
    }

    for (uint32_t fi = start; fi <= frame_index; ++fi) {
        Status st = DecodeSingleFrameInternal(fi);
        if (!st.ok()) return st;
    }

    out_frame.frame_index = frame_index;
    out_frame.width = header_.width;
    out_frame.height = header_.height;
    out_frame.is_keyframe = header_.frame_index[frame_index].is_keyframe;
    out_frame.pixel_format = pixel_format;
    ConvertPlanesToPixels(pixel_format, out_frame.pixels);
    return Status::Ok();
}

}  // namespace scarlettx
