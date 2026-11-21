#include "scarlettx/io/gdeflate.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <queue>
#include <vector>

namespace scarlettx {
namespace {

constexpr uint32_t kNumLanes = kGDeflateNumStreams;  // 32
constexpr uint32_t kTileStreamHeaderBytes = 8u;

constexpr uint32_t kCodeLenOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
};

// DEFLATE64 length/distance tables matching Microsoft DirectStorage GDeflate.hlsl.
constexpr uint32_t kBaseLength[30] = {
    0,   3,   4,   5,   6,   7,   8,   9,
    10,  11,  13,  15,  17,  19,  23,  27,
    31,  35,  43,  51,  59,  67,  83,  99,
    115, 131, 163, 195, 227, 3,
};

constexpr uint32_t kExtraLength[30] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4,
    4, 5, 5, 5, 5, 16,
};

constexpr uint32_t kBaseDist[32] = {
    1,     2,     3,     4,     5,     7,     9,     13,
    17,    25,    33,    49,    65,    97,    129,   193,
    257,   385,   513,   769,   1025,  1537,  2049,  3073,
    4097,  6145,  8193,  12289, 16385, 24577, 32769, 49153,
};

constexpr uint32_t kExtraDist[32] = {
    0,  0,  0,  0,  1,  1,  2,  2,
    3,  3,  4,  4,  5,  5,  6,  6,
    7,  7,  8,  8,  9,  9,  10, 10,
    11, 11, 12, 12, 13, 13, 14, 14,
};

inline uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline void WriteLe16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

inline void WriteLe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

inline uint32_t BitMask(uint32_t n) {
    return n >= 32u ? 0xFFFFFFFFu : ((1u << n) - 1u);
}

inline uint32_t ReverseBits(uint32_t v, uint32_t bits) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < bits; ++i) {
        r = (r << 1) | (v & 1u);
        v >>= 1;
    }
    return r;
}

inline uint32_t ReverseBits32(uint32_t x) {
    x = ((x & 0x55555555u) << 1) | ((x >> 1) & 0x55555555u);
    x = ((x & 0x33333333u) << 2) | ((x >> 2) & 0x33333333u);
    x = ((x & 0x0F0F0F0Fu) << 4) | ((x >> 4) & 0x0F0F0F0Fu);
    x = ((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu);
    return (x << 16) | (x >> 16);
}

struct ParsedTileStream {
    uint16_t num_tiles = 0;
    uint32_t last_tile_size = 0;
    size_t uncompressed_size = 0;
};

bool ParseHeader(const uint8_t* src, size_t src_size, ParsedTileStream& out) {
    if (!src || src_size < kTileStreamHeaderBytes) {
        return false;
    }
    const uint8_t id = src[0];
    const uint8_t magic = src[1];
    if (id != kGDeflateCodecId || magic != static_cast<uint8_t>(id ^ 0xFFu)) {
        return false;
    }
    const uint16_t num_tiles = ReadLe16(src + 2);
    const uint32_t word2 = ReadLe32(src + 4);
    const uint32_t tile_size_idx = word2 & 0x3u;
    const uint32_t last_tile_size = (word2 >> 2) & 0x3FFFFu;
    const uint32_t reserved = word2 >> 20;
    if (tile_size_idx != 1u || reserved != 0u || last_tile_size > kGDeflateTileSize) {
        return false;
    }
    out.num_tiles = num_tiles;
    out.last_tile_size = last_tile_size;
    if (num_tiles == 0) {
        if (last_tile_size != 0) return false;
        out.uncompressed_size = 0;
    } else {
        const size_t full_bytes = static_cast<size_t>(num_tiles) * kGDeflateTileSize;
        out.uncompressed_size =
            full_bytes - (last_tile_size == 0 ? 0u : (kGDeflateTileSize - last_tile_size));
    }
    return true;
}

// ---------------------------------------------------------------------------
// 32-stream lockstep BitReader (exact semantics of GDeflate.hlsl BitReader)
// ---------------------------------------------------------------------------
struct GDeflateBitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t base = 0;
    uint64_t buf[kNumLanes]{};
    uint32_t cnt[kNumLanes]{};

    bool Init(const uint8_t* tile_data, size_t tile_size) {
        if (!tile_data || tile_size < kNumLanes * 4u) {
            return false;
        }
        data = tile_data;
        size = tile_size;
        for (uint32_t t = 0; t < kNumLanes; ++t) {
            buf[t] = static_cast<uint64_t>(ReadLe32(tile_data + t * 4u));
            cnt[t] = 32u;
        }
        base = kNumLanes * 4u;
        return true;
    }

    uint32_t Peek32(uint32_t lane) const {
        return static_cast<uint32_t>(buf[lane]);
    }

    uint32_t Peek(uint32_t lane, uint32_t n) const {
        return static_cast<uint32_t>(buf[lane]) & BitMask(n);
    }

    void EatLockstep(const uint32_t bits_to_eat[kNumLanes], const bool active[kNumLanes]) {
        for (uint32_t t = 0; t < kNumLanes; ++t) {
            if (active[t]) {
                const uint32_t n = bits_to_eat[t];
                buf[t] >>= n;
                cnt[t] = (cnt[t] >= n) ? (cnt[t] - n) : 0u;
            }
        }
        for (uint32_t t = 0; t < kNumLanes; ++t) {
            if (active[t] && cnt[t] < 32u) {
                uint32_t word = 0;
                if (base + 4u <= size) {
                    word = ReadLe32(data + base);
                } else if (base < size) {
                    for (size_t k = 0; base + k < size; ++k) {
                        word |= static_cast<uint32_t>(data[base + k]) << (k * 8u);
                    }
                }
                base += 4u;
                buf[t] |= static_cast<uint64_t>(word) << cnt[t];
                cnt[t] += 32u;
            }
        }
    }

    uint32_t ReadLane0(uint32_t n) {
        const uint32_t val = Peek(0, n);
        uint32_t eat_n[kNumLanes]{};
        bool active[kNumLanes]{};
        eat_n[0] = n;
        active[0] = true;
        EatLockstep(eat_n, active);
        return val;
    }
};

// ---------------------------------------------------------------------------
// 32-stream lockstep BitWriter (exact inverse of GDeflateBitReader)
// ---------------------------------------------------------------------------
struct GDeflateBitWriter {
    struct Step {
        std::array<uint8_t, kNumLanes> n{};
        std::array<bool, kNumLanes> active{};
    };

    std::array<std::vector<uint32_t>, kNumLanes> lane_words;
    std::array<uint64_t, kNumLanes> lane_accum{};
    std::array<uint32_t, kNumLanes> lane_bits{};
    std::vector<Step> steps;

    void PushLaneBits(uint32_t lane, uint32_t value, uint32_t nbits) {
        if (nbits == 0) return;
        lane_accum[lane] |= (static_cast<uint64_t>(value & BitMask(nbits)) << lane_bits[lane]);
        lane_bits[lane] += nbits;
        while (lane_bits[lane] >= 32u) {
            lane_words[lane].push_back(static_cast<uint32_t>(lane_accum[lane] & 0xFFFFFFFFull));
            lane_accum[lane] >>= 32u;
            lane_bits[lane] -= 32u;
        }
    }

    void WriteStep(const uint32_t values[kNumLanes],
                   const uint32_t nbits[kNumLanes],
                   const bool active[kNumLanes]) {
        Step s{};
        for (uint32_t t = 0; t < kNumLanes; ++t) {
            s.active[t] = active[t];
            s.n[t] = active[t] ? static_cast<uint8_t>(nbits[t]) : 0u;
            if (active[t]) {
                PushLaneBits(t, values[t], nbits[t]);
            }
        }
        steps.push_back(s);
    }

    void WriteLane0(uint32_t value, uint32_t nbits) {
        uint32_t vals[kNumLanes]{};
        uint32_t ns[kNumLanes]{};
        bool act[kNumLanes]{};
        vals[0] = value;
        ns[0] = nbits;
        act[0] = true;
        WriteStep(vals, ns, act);
    }

    std::vector<uint8_t> Finalize() {
        // Flush any remaining partial bits per lane into words.
        for (uint32_t t = 0; t < kNumLanes; ++t) {
            if (lane_bits[t] > 0 || lane_words[t].empty()) {
                lane_words[t].push_back(static_cast<uint32_t>(lane_accum[t] & 0xFFFFFFFFull));
                lane_accum[t] = 0;
                lane_bits[t] = 0;
            }
        }

        std::vector<uint32_t> serialized;
        serialized.reserve(kNumLanes + steps.size() * 4u);

        std::array<size_t, kNumLanes> word_idx{};
        std::array<uint32_t, kNumLanes> cnt{};

        // Initial 32 words (one per lane).
        for (uint32_t t = 0; t < kNumLanes; ++t) {
            serialized.push_back(lane_words[t][0]);
            word_idx[t] = 1;
            cnt[t] = 32u;
        }

        // Replay exact refill order of GDeflateBitReader::EatLockstep.
        for (const auto& s : steps) {
            for (uint32_t t = 0; t < kNumLanes; ++t) {
                if (s.active[t]) {
                    cnt[t] -= s.n[t];
                }
            }
            for (uint32_t t = 0; t < kNumLanes; ++t) {
                if (s.active[t] && cnt[t] < 32u) {
                    const uint32_t w = (word_idx[t] < lane_words[t].size()) ? lane_words[t][word_idx[t]++] : 0u;
                    serialized.push_back(w);
                    cnt[t] += 32u;
                }
            }
        }

        // Append any unconsumed trailing words (for lanes whose last bits fit in the final 32-bit buffer)
        // plus 32 zero-padded words (128 bytes) per the GDeflate stream specification.
        for (uint32_t t = 0; t < kNumLanes; ++t) {
            while (word_idx[t] < lane_words[t].size()) {
                serialized.push_back(lane_words[t][word_idx[t]++]);
            }
        }
        for (uint32_t i = 0; i < kNumLanes; ++i) {
            serialized.push_back(0u);
        }

        std::vector<uint8_t> bytes;
        bytes.reserve(serialized.size() * 4u);
        for (uint32_t w : serialized) {
            WriteLe32(bytes, w);
        }
        return bytes;
    }
};

// ---------------------------------------------------------------------------
// Canonical Huffman Decoder (RFC 1951, up to 15-bit codes)
// ---------------------------------------------------------------------------
struct HuffmanDecoder {
    static constexpr uint32_t kMaxBits = 15u;
    uint32_t max_len = 0;
    std::array<uint32_t, kMaxBits + 1> count{};
    std::array<uint32_t, kMaxBits + 1> first_code{};
    std::array<uint32_t, kMaxBits + 1> first_symbol{};
    std::vector<uint16_t> symbols;

    bool Build(const uint8_t* lengths, uint32_t num_symbols, uint32_t max_allowed_bits) {
        count.fill(0);
        first_code.fill(0);
        first_symbol.fill(0);
        symbols.clear();
        max_len = 0;

        uint32_t total = 0;
        for (uint32_t i = 0; i < num_symbols; ++i) {
            const uint32_t l = lengths[i];
            if (l > max_allowed_bits || l > kMaxBits) {
                return false;
            }
            if (l > 0) {
                ++count[l];
                ++total;
                if (l > max_len) max_len = l;
            }
        }
        if (total == 0) {
            return true;  // Empty tree (valid for unused distance tree in all-literal blocks)
        }

        uint32_t code = 0;
        uint32_t sym_off = 0;
        for (uint32_t l = 1; l <= max_allowed_bits; ++l) {
            first_code[l] = code;
            first_symbol[l] = sym_off;
            code += count[l];
            if (code > (1u << l)) {
                return false;  // Over-subscribed Huffman tree
            }
            code <<= 1;
            sym_off += count[l];
        }

        symbols.resize(total);
        std::array<uint32_t, kMaxBits + 1> next_off = first_symbol;
        for (uint32_t i = 0; i < num_symbols; ++i) {
            const uint32_t l = lengths[i];
            if (l > 0) {
                symbols[next_off[l]++] = static_cast<uint16_t>(i);
            }
        }
        return true;
    }

    bool Decode(uint32_t lsb_bits, uint32_t& out_sym, uint32_t& out_len) const {
        if (symbols.empty()) {
            return false;
        }
        const uint32_t rev = ReverseBits32(lsb_bits);
        for (uint32_t l = 1; l <= max_len; ++l) {
            if (count[l] == 0) continue;
            const uint32_t c = rev >> (32u - l);
            if (c >= first_code[l] && (c - first_code[l]) < count[l]) {
                out_sym = symbols[first_symbol[l] + (c - first_code[l])];
                out_len = l;
                return true;
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Canonical Huffman Encoder (builds bit-reversed LSB codes from lengths)
// ---------------------------------------------------------------------------
struct HuffmanEncoder {
    std::vector<uint16_t> code;
    std::vector<uint8_t> len;

    void BuildFromLengths(const uint8_t* lengths, uint32_t num_symbols, uint32_t max_bits) {
        code.assign(num_symbols, 0);
        len.assign(lengths, lengths + num_symbols);

        std::array<uint32_t, 16> bl_count{};
        for (uint32_t i = 0; i < num_symbols; ++i) {
            if (lengths[i] > 0 && lengths[i] <= max_bits) {
                ++bl_count[lengths[i]];
            }
        }
        std::array<uint32_t, 16> next_code{};
        uint32_t c = 0;
        for (uint32_t b = 1; b <= max_bits; ++b) {
            c = (c + bl_count[b - 1]) << 1;
            next_code[b] = c;
        }
        for (uint32_t i = 0; i < num_symbols; ++i) {
            const uint32_t l = lengths[i];
            if (l > 0) {
                code[i] = static_cast<uint16_t>(ReverseBits(next_code[l]++, l));
            }
        }
    }
};

// Computes length-limited canonical Huffman code lengths (guaranteed <= max_bits).
void ComputeCodeLengths(const uint32_t* freqs, uint32_t num_symbols, uint32_t max_bits, std::vector<uint8_t>& out_lens) {
    out_lens.assign(num_symbols, 0);
    struct Node {
        uint64_t freq = 0;
        int32_t sym = -1;
        int32_t left = -1;
        int32_t right = -1;
    };
    std::vector<Node> nodes;
    nodes.reserve(num_symbols * 2u);

    auto cmp = [&nodes](int32_t a, int32_t b) {
        if (nodes[a].freq != nodes[b].freq) return nodes[a].freq > nodes[b].freq;
        return a > b;
    };
    std::priority_queue<int32_t, std::vector<int32_t>, decltype(cmp)> pq(cmp);

    std::vector<uint32_t> active_syms;
    for (uint32_t i = 0; i < num_symbols; ++i) {
        if (freqs[i] > 0) {
            active_syms.push_back(i);
            const int32_t idx = static_cast<int32_t>(nodes.size());
            nodes.push_back({freqs[i], static_cast<int32_t>(i), -1, -1});
            pq.push(idx);
        }
    }
    if (active_syms.empty()) {
        return;
    }
    if (active_syms.size() == 1) {
        out_lens[active_syms[0]] = 1;
        return;
    }

    while (pq.size() > 1) {
        const int32_t a = pq.top();
        pq.pop();
        const int32_t b = pq.top();
        pq.pop();
        const int32_t p = static_cast<int32_t>(nodes.size());
        nodes.push_back({nodes[a].freq + nodes[b].freq, -1, a, b});
        pq.push(p);
    }

    // BFS/DFS depths from root.
    std::vector<std::pair<int32_t, uint32_t>> stack;
    stack.push_back({pq.top(), 0u});
    bool overflow = false;
    while (!stack.empty()) {
        auto [idx, depth] = stack.back();
        stack.pop_back();
        if (nodes[idx].sym >= 0) {
            uint32_t d = depth == 0 ? 1u : depth;
            if (d > max_bits) {
                d = max_bits;
                overflow = true;
            }
            out_lens[static_cast<uint32_t>(nodes[idx].sym)] = static_cast<uint8_t>(d);
        } else {
            stack.push_back({nodes[idx].left, depth + 1u});
            stack.push_back({nodes[idx].right, depth + 1u});
        }
    }

    // If clamping to max_bits over-subscribed the Kraft sum, lengthen shortest codes
    // until sum(2^(max_bits - len)) <= 2^max_bits.
    if (overflow) {
        const uint32_t limit = 1u << max_bits;
        auto kraft_sum = [&]() {
            uint32_t s = 0;
            for (uint32_t sym : active_syms) {
                s += (1u << (max_bits - out_lens[sym]));
            }
            return s;
        };
        std::sort(active_syms.begin(), active_syms.end(), [&](uint32_t a, uint32_t b) {
            return freqs[a] < freqs[b];
        });
        while (kraft_sum() > limit) {
            for (uint32_t sym : active_syms) {
                if (out_lens[sym] < max_bits) {
                    ++out_lens[sym];
                    if (kraft_sum() <= limit) break;
                }
            }
        }
    }
}

void BuildFixedLengths(std::array<uint8_t, 288>& lit_lens, std::array<uint8_t, 32>& dist_lens) {
    for (uint32_t i = 0; i <= 143; ++i) lit_lens[i] = 8;
    for (uint32_t i = 144; i <= 255; ++i) lit_lens[i] = 9;
    for (uint32_t i = 256; i <= 279; ++i) lit_lens[i] = 7;
    for (uint32_t i = 280; i <= 287; ++i) lit_lens[i] = 8;
    dist_lens.fill(5);
}

// ---------------------------------------------------------------------------
// Tile Decompressor (matches GDeflate.hlsl DecompressTile bit-for-bit)
// ---------------------------------------------------------------------------
bool DecodeCompressedBlock(
    GDeflateBitReader& br,
    const HuffmanDecoder& lit_dec,
    const HuffmanDecoder& dist_dec,
    uint8_t* dst,
    size_t dst_cap,
    size_t& dst_pos) {
    // Round 0: decode initial lit/len symbol in all 32 lanes.
    uint32_t sym[kNumLanes]{};
    uint32_t code_len[kNumLanes]{};
    uint32_t val[kNumLanes]{};
    uint32_t eat_n[kNumLanes]{};
    bool active[kNumLanes]{};
    uint32_t length[kNumLanes]{};
    uint32_t offset[kNumLanes]{};
    bool iscopy[kNumLanes]{};
    uint32_t byte_val[kNumLanes]{};

    uint32_t eob_mask = 0;
    for (uint32_t t = 0; t < kNumLanes; ++t) {
        if (eob_mask != 0u) {
            sym[t] = 0;
            code_len[t] = 0;
            continue;
        }
        const uint32_t bits = br.Peek32(t);
        if (!lit_dec.Decode(bits, sym[t], code_len[t])) {
            return false;
        }
        if (sym[t] == 256u) {
            eob_mask |= (1u << t);
        }
    }

    bool oob[kNumLanes]{};
    for (uint32_t t = 0; t < kNumLanes; ++t) {
        oob[t] = (eob_mask & BitMask(t)) != 0;
        const uint32_t bits = br.Peek32(t);
        if (!oob[t]) {
            if (sym[t] < 256u) {
                val[t] = 1u;
                eat_n[t] = code_len[t];
            } else {
                const uint32_t idx = sym[t] - 256u;
                if (idx >= 30u) return false;
                const uint32_t extra = kExtraLength[idx];
                val[t] = kBaseLength[idx] + ((bits >> code_len[t]) & BitMask(extra));
                eat_n[t] = code_len[t] + extra;
            }
            active[t] = true;
        } else {
            val[t] = 0;
            eat_n[t] = 0;
            active[t] = false;
        }
    }
    br.EatLockstep(eat_n, active);

    uint32_t running = 0;
    for (uint32_t t = 0; t < kNumLanes; ++t) {
        length[t] = oob[t] ? 0u : val[t];
        offset[t] = running;
        running += length[t];
        iscopy[t] = (!oob[t] && sym[t] > 256u);
        byte_val[t] = sym[t];
    }

    auto emit_round = [&](const uint32_t dist_val[kNumLanes], const bool round_iscopy[kNumLanes]) -> bool {
        const uint32_t round_total = offset[kNumLanes - 1] + length[kNumLanes - 1];
        if (dst_pos + round_total > dst_cap) {
            return false;
        }
        // Emit literals first, then copies in lane order (matching GDeflate.hlsl WriteOutput).
        for (uint32_t t = 0; t < kNumLanes; ++t) {
            if (!round_iscopy[t] && length[t] != 0u) {
                dst[dst_pos + offset[t]] = static_cast<uint8_t>(byte_val[t] & 0xFFu);
            }
        }
        for (uint32_t t = 0; t < kNumLanes; ++t) {
            if (round_iscopy[t]) {
                const size_t out_off = dst_pos + offset[t];
                const uint32_t d = dist_val[t];
                const uint32_t len = length[t];
                if (d == 0u || d > out_off || out_off + len > dst_cap) {
                    return false;
                }
                for (uint32_t i = 0; i < len; ++i) {
                    dst[out_off + i] = dst[out_off + i - d];
                }
            }
        }
        dst_pos += round_total;
        return true;
    };

    // Subsequent rounds until EOB is seen.
    while (eob_mask == 0u) {
        uint32_t next_sym[kNumLanes]{};
        uint32_t next_len[kNumLanes]{};
        eob_mask = 0u;

        for (uint32_t t = 0; t < kNumLanes; ++t) {
            if (!iscopy[t] && eob_mask != 0u) {
                next_sym[t] = 0;
                next_len[t] = 0;
                continue;
            }
            const uint32_t bits = br.Peek32(t);
            const bool ok = iscopy[t] ? dist_dec.Decode(bits, next_sym[t], next_len[t])
                                      : lit_dec.Decode(bits, next_sym[t], next_len[t]);
            if (!ok) {
                return false;
            }
            if (!iscopy[t] && next_sym[t] == 256u) {
                eob_mask |= (1u << t);
            }
        }

        uint32_t next_val[kNumLanes]{};
        for (uint32_t t = 0; t < kNumLanes; ++t) {
            oob[t] = (eob_mask & BitMask(t)) != 0;
            const uint32_t bits = br.Peek32(t);
            if (iscopy[t]) {
                if (next_sym[t] >= 32u) return false;
                const uint32_t extra = kExtraDist[next_sym[t]];
                next_val[t] = kBaseDist[next_sym[t]] + ((bits >> next_len[t]) & BitMask(extra));
                eat_n[t] = next_len[t] + extra;
                active[t] = true;
            } else if (!oob[t]) {
                if (next_sym[t] < 256u) {
                    next_val[t] = 1u;
                    eat_n[t] = next_len[t];
                } else {
                    const uint32_t idx = next_sym[t] - 256u;
                    if (idx >= 30u) return false;
                    const uint32_t extra = kExtraLength[idx];
                    next_val[t] = kBaseLength[idx] + ((bits >> next_len[t]) & BitMask(extra));
                    eat_n[t] = next_len[t] + extra;
                }
                active[t] = true;
            } else {
                next_val[t] = 0;
                eat_n[t] = 0;
                active[t] = false;
            }
        }
        br.EatLockstep(eat_n, active);

        if (!emit_round(next_val, iscopy)) {
            return false;
        }

        running = 0;
        for (uint32_t t = 0; t < kNumLanes; ++t) {
            const bool prev_copy = iscopy[t];
            length[t] = (prev_copy || oob[t]) ? 0u : next_val[t];
            offset[t] = running;
            running += length[t];
            iscopy[t] = (!prev_copy && !oob[t] && next_sym[t] > 256u);
            byte_val[t] = next_sym[t];
        }
    }

    // Final flush round for any remaining pending distance codes from the EOB round.
    uint32_t final_dist[kNumLanes]{};
    bool final_copy[kNumLanes]{};
    for (uint32_t t = 0; t < kNumLanes; ++t) {
        final_copy[t] = iscopy[t] && !oob[t];
        if (final_copy[t]) {
            const uint32_t bits = br.Peek32(t);
            uint32_t dsym = 0;
            uint32_t dlen = 0;
            if (!dist_dec.Decode(bits, dsym, dlen) || dsym >= 32u) {
                return false;
            }
            const uint32_t extra = kExtraDist[dsym];
            final_dist[t] = kBaseDist[dsym] + ((bits >> dlen) & BitMask(extra));
            eat_n[t] = dlen + extra;
            active[t] = true;
        } else {
            final_dist[t] = 0;
            eat_n[t] = 0;
            active[t] = false;
        }
    }
    br.EatLockstep(eat_n, active);
    return emit_round(final_dist, final_copy);
}

bool DecodeDynamicTrees(
    GDeflateBitReader& br,
    uint32_t hlit,
    uint32_t hdist,
    uint32_t hclen,
    HuffmanDecoder& lit_dec,
    HuffmanDecoder& dist_dec) {
    // 1. Read code-length code lengths across lanes 0..hclen-1.
    uint8_t cl_lens[19]{};
    uint32_t eat_n[kNumLanes]{};
    bool active[kNumLanes]{};
    for (uint32_t t = 0; t < kNumLanes; ++t) {
        if (t < hclen) {
            cl_lens[kCodeLenOrder[t]] = static_cast<uint8_t>(br.Peek(t, 3u));
            eat_n[t] = 3u;
            active[t] = true;
        }
    }
    br.EatLockstep(eat_n, active);

    HuffmanDecoder cl_dec;
    if (!cl_dec.Build(cl_lens, 19u, 7u)) {
        return false;
    }

    // 2. Unpack (hlit + hdist) code lengths across 32 lanes per round.
    const uint32_t total_count = hlit + hdist;
    std::vector<uint8_t> all_lens(total_count, 0u);
    uint32_t base_offset = 0;
    uint32_t last_len = 0;

    static constexpr uint32_t kRepBase[4] = {1u, 3u, 3u, 11u};
    static constexpr uint32_t kRepExtra[4] = {0u, 2u, 3u, 7u};

    while (base_offset < total_count) {
        uint32_t syms[kNumLanes]{};
        uint32_t slens[kNumLanes]{};
        uint32_t rep_n[kNumLanes]{};
        uint32_t extra_bits[kNumLanes]{};
        uint32_t codelens[kNumLanes]{};

        uint32_t running = base_offset;
        for (uint32_t t = 0; t < kNumLanes; ++t) {
            if (running >= total_count) {
                eat_n[t] = 0;
                active[t] = false;
                continue;
            }
            const uint32_t bits = br.Peek32(t);
            if (!cl_dec.Decode(bits, syms[t], slens[t]) || syms[t] > 18u) {
                return false;
            }
            const uint32_t idx = (syms[t] <= 15u) ? 0u : (syms[t] - 15u);
            extra_bits[t] = kRepExtra[idx];
            rep_n[t] = kRepBase[idx] + ((bits >> slens[t]) & BitMask(extra_bits[t]));

            if (syms[t] <= 15u) {
                codelens[t] = syms[t];
            } else if (syms[t] > 16u) {
                codelens[t] = 0u;
            } else {
                uint32_t prev = last_len;
                for (int32_t p = static_cast<int32_t>(t) - 1; p >= 0; --p) {
                    if (syms[p] != 16u) {
                        prev = codelens[p];
                        break;
                    }
                }
                codelens[t] = prev;
            }

            const uint32_t write_n = std::min(rep_n[t], total_count - running);
            for (uint32_t k = 0; k < write_n; ++k) {
                all_lens[running + k] = static_cast<uint8_t>(codelens[t]);
            }
            running += rep_n[t];
            eat_n[t] = slens[t] + extra_bits[t];
            active[t] = true;
            last_len = codelens[t];
        }
        br.EatLockstep(eat_n, active);
        base_offset = running;
    }

    return lit_dec.Build(all_lens.data(), hlit, 15u) &&
           dist_dec.Build(all_lens.data() + hlit, hdist, 15u);
}

bool DecompressTile(const uint8_t* tile_data, size_t tile_size, uint8_t* dst, size_t expected_out_size) {
    GDeflateBitReader br;
    if (!br.Init(tile_data, tile_size)) {
        return false;
    }

    size_t dst_pos = 0;
    bool done = false;
    while (!done) {
        const uint32_t header = br.Peek32(0);
        done = (header & 1u) != 0;
        const uint32_t btype = (header >> 1) & 3u;
        br.ReadLane0(3u);

        if (btype == 0u) {
            const uint32_t block_size = br.ReadLane0(16u);
            if (dst_pos + block_size > expected_out_size) {
                return false;
            }
            const uint32_t full_rounds = block_size / kNumLanes;
            uint32_t eat8[kNumLanes];
            bool all_act[kNumLanes];
            for (uint32_t t = 0; t < kNumLanes; ++t) {
                eat8[t] = 8u;
                all_act[t] = true;
            }
            for (uint32_t r = 0; r < full_rounds; ++r) {
                for (uint32_t t = 0; t < kNumLanes; ++t) {
                    dst[dst_pos + t] = static_cast<uint8_t>(br.Peek(t, 8u));
                }
                br.EatLockstep(eat8, all_act);
                dst_pos += kNumLanes;
            }
            const uint32_t rem = block_size % kNumLanes;
            if (rem > 0) {
                uint32_t eat_rem[kNumLanes]{};
                bool act_rem[kNumLanes]{};
                for (uint32_t t = 0; t < rem; ++t) {
                    dst[dst_pos + t] = static_cast<uint8_t>(br.Peek(t, 8u));
                    eat_rem[t] = 8u;
                    act_rem[t] = true;
                }
                br.EatLockstep(eat_rem, act_rem);
                dst_pos += rem;
            }
        } else if (btype == 1u) {
            std::array<uint8_t, 288> lit_lens{};
            std::array<uint8_t, 32> dist_lens{};
            BuildFixedLengths(lit_lens, dist_lens);
            HuffmanDecoder lit_dec;
            HuffmanDecoder dist_dec;
            if (!lit_dec.Build(lit_lens.data(), 288u, 15u) ||
                !dist_dec.Build(dist_lens.data(), 32u, 15u)) {
                return false;
            }
            if (!DecodeCompressedBlock(br, lit_dec, dist_dec, dst, expected_out_size, dst_pos)) {
                return false;
            }
        } else if (btype == 2u) {
            const uint32_t hlit = ((header >> 3) & 0x1Fu) + 257u;
            const uint32_t hdist = ((header >> 8) & 0x1Fu) + 1u;
            const uint32_t hclen = ((header >> 13) & 0x0Fu) + 4u;
            br.ReadLane0(14u);
            HuffmanDecoder lit_dec;
            HuffmanDecoder dist_dec;
            if (!DecodeDynamicTrees(br, hlit, hdist, hclen, lit_dec, dist_dec)) {
                return false;
            }
            if (!DecodeCompressedBlock(br, lit_dec, dist_dec, dst, expected_out_size, dst_pos)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return dst_pos == expected_out_size;
}

// ---------------------------------------------------------------------------
// Tile Compressor (LZ77 + Stored / Fixed / Dynamic GDeflate v1 encoding)
// ---------------------------------------------------------------------------
struct LzToken {
    uint16_t lit_or_len = 0;  // 0..255 literal, 256 EOB, or 3..65535 length (when is_match=true)
    uint32_t dist = 0;        // 1..65536 (when is_match=true)
    bool is_match = false;
    bool is_eob = false;
};

void EncodeLengthSym(uint32_t length, uint32_t& out_sym, uint32_t& out_extra_val, uint32_t& out_extra_bits) {
    if (length >= 259u) {
        // DEFLATE64 symbol 285: base=3, 16 extra bits (covers 3..65538).
        out_sym = 285u;
        out_extra_val = length - 3u;
        out_extra_bits = 16u;
        return;
    }
    for (uint32_t idx = 28u; idx >= 1u; --idx) {
        if (length >= kBaseLength[idx]) {
            out_sym = 256u + idx;
            out_extra_val = length - kBaseLength[idx];
            out_extra_bits = kExtraLength[idx];
            return;
        }
    }
    out_sym = 257u;
    out_extra_val = 0;
    out_extra_bits = 0;
}

void EncodeDistSym(uint32_t dist, uint32_t& out_sym, uint32_t& out_extra_val, uint32_t& out_extra_bits) {
    for (int32_t idx = 31; idx >= 0; --idx) {
        if (dist >= kBaseDist[idx]) {
            out_sym = static_cast<uint32_t>(idx);
            out_extra_val = dist - kBaseDist[idx];
            out_extra_bits = kExtraDist[idx];
            return;
        }
    }
    out_sym = 0;
    out_extra_val = 0;
    out_extra_bits = 0;
}

std::vector<LzToken> TokenizeLz77(const uint8_t* src, size_t size, bool allow_deflate64_ext) {
    std::vector<LzToken> tokens;
    tokens.reserve(size / 2u + 16u);
    if (size == 0) {
        tokens.push_back({256u, 0u, false, true});
        return tokens;
    }

    constexpr uint32_t kHashBits = 15u;
    constexpr uint32_t kHashSize = 1u << kHashBits;
    constexpr uint32_t kMaxChain = 32u;
    const uint32_t max_dist = allow_deflate64_ext ? 65536u : 32768u;
    const uint32_t max_len = allow_deflate64_ext ? 65535u : 258u;

    std::vector<int32_t> head(kHashSize, -1);
    std::vector<int32_t> prev(size, -1);

    auto hash3 = [&](size_t pos) -> uint32_t {
        const uint32_t v = (static_cast<uint32_t>(src[pos]) << 16) |
                           (static_cast<uint32_t>(src[pos + 1]) << 8) |
                           static_cast<uint32_t>(src[pos + 2]);
        return ((v * 2654435761u) >> (32u - kHashBits)) & (kHashSize - 1u);
    };

    size_t pos = 0;
    while (pos < size) {
        uint32_t best_len = 0;
        uint32_t best_dist = 0;

        if (pos + 2u < size) {
            const uint32_t h = hash3(pos);
            int32_t cur = head[h];
            prev[pos] = cur;
            head[h] = static_cast<int32_t>(pos);

            uint32_t steps = 0;
            const uint32_t limit_len = static_cast<uint32_t>(std::min<size_t>(max_len, size - pos));
            while (cur >= 0 && steps < kMaxChain) {
                const uint32_t dist = static_cast<uint32_t>(pos - static_cast<size_t>(cur));
                if (dist > max_dist) break;
                if (src[cur + best_len] == src[pos + best_len]) {
                    uint32_t l = 0;
                    while (l < limit_len && src[cur + l] == src[pos + l]) {
                        ++l;
                    }
                    if (l >= 3u && l > best_len) {
                        best_len = l;
                        best_dist = dist;
                        if (best_len >= 128u || best_len == limit_len) break;
                    }
                }
                cur = prev[static_cast<size_t>(cur)];
                ++steps;
            }
        }

        if (best_len >= 3u) {
            tokens.push_back({static_cast<uint16_t>(best_len), best_dist, true, false});
            const size_t end_insert = std::min(pos + best_len, size > 2u ? size - 2u : 0u);
            for (size_t k = pos + 1u; k < end_insert; ++k) {
                const uint32_t hk = hash3(k);
                prev[k] = head[hk];
                head[hk] = static_cast<int32_t>(k);
            }
            pos += best_len;
        } else {
            tokens.push_back({src[pos], 0u, false, false});
            ++pos;
        }
    }

    tokens.push_back({256u, 0u, false, true});
    return tokens;
}

std::vector<uint8_t> EncodeTileStored(const uint8_t* src, size_t size) {
    GDeflateBitWriter bw;
    size_t offset = 0;
    do {
        const size_t chunk = std::min<size_t>(65535u, size - offset);
        const bool is_final = (offset + chunk >= size);
        bw.WriteLane0((is_final ? 1u : 0u) | (0u << 1), 3u);
        bw.WriteLane0(static_cast<uint32_t>(chunk), 16u);

        const size_t full_rounds = chunk / kNumLanes;
        uint32_t vals[kNumLanes]{};
        uint32_t ns[kNumLanes]{};
        bool act[kNumLanes]{};
        for (uint32_t t = 0; t < kNumLanes; ++t) {
            ns[t] = 8u;
            act[t] = true;
        }
        for (size_t r = 0; r < full_rounds; ++r) {
            for (uint32_t t = 0; t < kNumLanes; ++t) {
                vals[t] = src[offset + t];
            }
            bw.WriteStep(vals, ns, act);
            offset += kNumLanes;
        }
        const size_t rem = chunk % kNumLanes;
        if (rem > 0) {
            std::memset(vals, 0, sizeof(vals));
            std::memset(ns, 0, sizeof(ns));
            std::memset(act, 0, sizeof(act));
            for (size_t t = 0; t < rem; ++t) {
                vals[t] = src[offset + t];
                ns[t] = 8u;
                act[t] = true;
            }
            bw.WriteStep(vals, ns, act);
            offset += rem;
        }
    } while (offset < size);
    return bw.Finalize();
}

void EmitCompressedTokens(
    GDeflateBitWriter& bw,
    const std::vector<LzToken>& tokens,
    const HuffmanEncoder& lit_enc,
    const HuffmanEncoder& dist_enc) {
    size_t tok_idx = 0;
    bool pending_dist[kNumLanes]{};
    uint32_t pending_dist_val[kNumLanes]{};
    bool eob_emitted = false;

    while (!eob_emitted) {
        uint32_t vals[kNumLanes]{};
        uint32_t ns[kNumLanes]{};
        bool act[kNumLanes]{};
        bool next_pending[kNumLanes]{};
        uint32_t next_dist_val[kNumLanes]{};

        for (uint32_t t = 0; t < kNumLanes; ++t) {
            if (pending_dist[t]) {
                uint32_t dsym = 0, dextra = 0, dbits = 0;
                EncodeDistSym(pending_dist_val[t], dsym, dextra, dbits);
                vals[t] = static_cast<uint32_t>(dist_enc.code[dsym]) | (dextra << dist_enc.len[dsym]);
                ns[t] = static_cast<uint32_t>(dist_enc.len[dsym]) + dbits;
                act[t] = true;
            } else if (!eob_emitted && tok_idx < tokens.size()) {
                const LzToken& tok = tokens[tok_idx++];
                if (tok.is_eob) {
                    vals[t] = lit_enc.code[256];
                    ns[t] = lit_enc.len[256];
                    act[t] = true;
                    eob_emitted = true;
                } else if (!tok.is_match) {
                    const uint32_t sym = tok.lit_or_len;
                    vals[t] = lit_enc.code[sym];
                    ns[t] = lit_enc.len[sym];
                    act[t] = true;
                } else {
                    uint32_t lsym = 0, lextra = 0, lbits = 0;
                    EncodeLengthSym(tok.lit_or_len, lsym, lextra, lbits);
                    vals[t] = static_cast<uint32_t>(lit_enc.code[lsym]) | (lextra << lit_enc.len[lsym]);
                    ns[t] = static_cast<uint32_t>(lit_enc.len[lsym]) + lbits;
                    act[t] = true;
                    next_pending[t] = true;
                    next_dist_val[t] = tok.dist;
                }
            }
        }

        bw.WriteStep(vals, ns, act);
        std::memcpy(pending_dist, next_pending, sizeof(pending_dist));
        std::memcpy(pending_dist_val, next_dist_val, sizeof(pending_dist_val));
    }

    // Final distance flush round (always emitted per GDeflate.hlsl lines 732-735).
    uint32_t vals[kNumLanes]{};
    uint32_t ns[kNumLanes]{};
    bool act[kNumLanes]{};
    for (uint32_t t = 0; t < kNumLanes; ++t) {
        if (pending_dist[t]) {
            uint32_t dsym = 0, dextra = 0, dbits = 0;
            EncodeDistSym(pending_dist_val[t], dsym, dextra, dbits);
            vals[t] = static_cast<uint32_t>(dist_enc.code[dsym]) | (dextra << dist_enc.len[dsym]);
            ns[t] = static_cast<uint32_t>(dist_enc.len[dsym]) + dbits;
            act[t] = true;
        }
    }
    bw.WriteStep(vals, ns, act);
}

std::vector<uint8_t> EncodeTileFixed(const std::vector<LzToken>& tokens) {
    std::array<uint8_t, 288> lit_lens{};
    std::array<uint8_t, 32> dist_lens{};
    BuildFixedLengths(lit_lens, dist_lens);
    HuffmanEncoder lit_enc;
    HuffmanEncoder dist_enc;
    lit_enc.BuildFromLengths(lit_lens.data(), 288u, 15u);
    dist_enc.BuildFromLengths(dist_lens.data(), 32u, 15u);

    GDeflateBitWriter bw;
    bw.WriteLane0(1u | (1u << 1), 3u);  // bfinal=1, btype=1 (Fixed Huffman)
    EmitCompressedTokens(bw, tokens, lit_enc, dist_enc);
    return bw.Finalize();
}

std::vector<uint8_t> EncodeTileDynamic(const std::vector<LzToken>& tokens) {
    std::array<uint32_t, 286> lit_freq{};
    std::array<uint32_t, 32> dist_freq{};
    for (const auto& tok : tokens) {
        if (tok.is_eob) {
            ++lit_freq[256];
        } else if (!tok.is_match) {
            ++lit_freq[tok.lit_or_len];
        } else {
            uint32_t lsym = 0, lextra = 0, lbits = 0;
            EncodeLengthSym(tok.lit_or_len, lsym, lextra, lbits);
            ++lit_freq[lsym];
            uint32_t dsym = 0, dextra = 0, dbits = 0;
            EncodeDistSym(tok.dist, dsym, dextra, dbits);
            ++dist_freq[dsym];
        }
    }
    bool any_dist = false;
    for (uint32_t f : dist_freq) {
        if (f > 0) {
            any_dist = true;
            break;
        }
    }
    if (!any_dist) {
        dist_freq[0] = 1;  // Ensure valid non-empty distance tree
    }

    std::vector<uint8_t> lit_lens;
    std::vector<uint8_t> dist_lens;
    ComputeCodeLengths(lit_freq.data(), 286u, 15u, lit_lens);
    ComputeCodeLengths(dist_freq.data(), 32u, 15u, dist_lens);

    uint32_t hlit = 286u;
    while (hlit > 257u && lit_lens[hlit - 1u] == 0u) --hlit;
    uint32_t hdist = 32u;
    while (hdist > 1u && dist_lens[hdist - 1u] == 0u) --hdist;

    // RLE-encode combined code lengths using RFC 1951 code-length alphabet (0..18).
    std::vector<uint8_t> combined;
    combined.reserve(hlit + hdist);
    combined.insert(combined.end(), lit_lens.begin(), lit_lens.begin() + hlit);
    combined.insert(combined.end(), dist_lens.begin(), dist_lens.begin() + hdist);

    struct ClSym {
        uint8_t sym = 0;
        uint8_t extra_val = 0;
        uint8_t extra_bits = 0;
    };
    std::vector<ClSym> cl_syms;
    cl_syms.reserve(combined.size());
    std::array<uint32_t, 19> cl_freq{};

    size_t i = 0;
    while (i < combined.size()) {
        const uint8_t cur = combined[i];
        size_t run = 1;
        while (i + run < combined.size() && combined[i + run] == cur) {
            ++run;
        }
        if (cur == 0u) {
            while (run >= 11u) {
                const uint32_t c = static_cast<uint32_t>(std::min<size_t>(run, 138u));
                cl_syms.push_back({18u, static_cast<uint8_t>(c - 11u), 7u});
                ++cl_freq[18];
                run -= c;
            }
            if (run >= 3u) {
                cl_syms.push_back({17u, static_cast<uint8_t>(run - 3u), 3u});
                ++cl_freq[17];
                run = 0;
            }
            while (run > 0) {
                cl_syms.push_back({0u, 0u, 0u});
                ++cl_freq[0];
                --run;
            }
        } else {
            cl_syms.push_back({cur, 0u, 0u});
            ++cl_freq[cur];
            --run;
            while (run >= 3u) {
                const uint32_t c = static_cast<uint32_t>(std::min<size_t>(run, 6u));
                cl_syms.push_back({16u, static_cast<uint8_t>(c - 3u), 2u});
                ++cl_freq[16];
                run -= c;
            }
            while (run > 0) {
                cl_syms.push_back({cur, 0u, 0u});
                ++cl_freq[cur];
                --run;
            }
        }
        i += (combined[i] == cur) ? 0u : 0u;  // advance handled below
        size_t adv = 1;
        while (i + adv < combined.size() && combined[i + adv] == cur) ++adv;
        i += adv;
    }

    std::vector<uint8_t> cl_lens;
    ComputeCodeLengths(cl_freq.data(), 19u, 7u, cl_lens);
    uint32_t hclen = 19u;
    while (hclen > 4u && cl_lens[kCodeLenOrder[hclen - 1u]] == 0u) --hclen;

    HuffmanEncoder cl_enc;
    cl_enc.BuildFromLengths(cl_lens.data(), 19u, 7u);
    HuffmanEncoder lit_enc;
    HuffmanEncoder dist_enc;
    lit_enc.BuildFromLengths(lit_lens.data(), hlit, 15u);
    dist_enc.BuildFromLengths(dist_lens.data(), hdist, 15u);

    GDeflateBitWriter bw;
    // Block header (3 bits: bfinal=1, btype=2) + dynamic header (14 bits) on lane 0.
    bw.WriteLane0(1u | (2u << 1), 3u);
    const uint32_t dyn_hdr = ((hlit - 257u) & 0x1Fu) |
                             (((hdist - 1u) & 0x1Fu) << 5) |
                             (((hclen - 4u) & 0x0Fu) << 10);
    bw.WriteLane0(dyn_hdr, 14u);

    // Code-length code lengths: 3 bits each across lanes 0..hclen-1.
    {
        uint32_t vals[kNumLanes]{};
        uint32_t ns[kNumLanes]{};
        bool act[kNumLanes]{};
        for (uint32_t t = 0; t < hclen; ++t) {
            vals[t] = cl_lens[kCodeLenOrder[t]];
            ns[t] = 3u;
            act[t] = true;
        }
        bw.WriteStep(vals, ns, act);
    }

    // Emit code-length symbols in 32-lane rounds.
    for (size_t pos = 0; pos < cl_syms.size(); pos += kNumLanes) {
        uint32_t vals[kNumLanes]{};
        uint32_t ns[kNumLanes]{};
        bool act[kNumLanes]{};
        const size_t round_n = std::min<size_t>(kNumLanes, cl_syms.size() - pos);
        for (size_t t = 0; t < round_n; ++t) {
            const ClSym& cs = cl_syms[pos + t];
            vals[t] = static_cast<uint32_t>(cl_enc.code[cs.sym]) |
                      (static_cast<uint32_t>(cs.extra_val) << cl_enc.len[cs.sym]);
            ns[t] = static_cast<uint32_t>(cl_enc.len[cs.sym]) + cs.extra_bits;
            act[t] = true;
        }
        bw.WriteStep(vals, ns, act);
    }

    EmitCompressedTokens(bw, tokens, lit_enc, dist_enc);
    return bw.Finalize();
}

std::vector<uint8_t> CompressTile(const uint8_t* src, size_t size, GDeflateBlockMode mode) {
    if (mode == GDeflateBlockMode::ForceStored) {
        return EncodeTileStored(src, size);
    }
    if (mode == GDeflateBlockMode::ForceFixed) {
        auto tokens = TokenizeLz77(src, size, false);
        return EncodeTileFixed(tokens);
    }
    if (mode == GDeflateBlockMode::ForceDynamic) {
        auto tokens = TokenizeLz77(src, size, true);
        return EncodeTileDynamic(tokens);
    }

    // Auto mode: pick smallest of Dynamic, Fixed, and Stored.
    auto tokens_dyn = TokenizeLz77(src, size, true);
    std::vector<uint8_t> best = EncodeTileDynamic(tokens_dyn);

    auto tokens_fix = TokenizeLz77(src, size, false);
    std::vector<uint8_t> fixed = EncodeTileFixed(tokens_fix);
    if (fixed.size() < best.size()) {
        best = std::move(fixed);
    }

    if (best.size() >= size + 128u) {
        std::vector<uint8_t> stored = EncodeTileStored(src, size);
        if (stored.size() < best.size()) {
            best = std::move(stored);
        }
    }
    return best;
}

}  // namespace

bool GDeflateIsValidStream(const uint8_t* src, size_t src_size) {
    ParsedTileStream parsed{};
    return ParseHeader(src, src_size, parsed);
}

bool GDeflateGetUncompressedSize(const uint8_t* src, size_t src_size, size_t* out_uncompressed_size) {
    if (!out_uncompressed_size) return false;
    ParsedTileStream parsed{};
    if (!ParseHeader(src, src_size, parsed)) {
        return false;
    }
    *out_uncompressed_size = parsed.uncompressed_size;
    return true;
}

bool GDeflateCompress(
    const uint8_t* src,
    size_t src_size,
    std::vector<uint8_t>& out_compressed,
    GDeflateBlockMode mode) {
    out_compressed.clear();
    if (src_size > 0 && !src) {
        return false;
    }
    const size_t full_tiles = src_size / kGDeflateTileSize;
    const uint32_t last_tile_size = static_cast<uint32_t>(src_size % kGDeflateTileSize);
    const size_t num_tiles_sz = full_tiles + (last_tile_size != 0 ? 1u : 0u);
    if (num_tiles_sz > 65535u) {
        return false;
    }
    const uint16_t num_tiles = static_cast<uint16_t>(num_tiles_sz);

    // 8-byte TileStream header.
    out_compressed.push_back(kGDeflateCodecId);
    out_compressed.push_back(kGDeflateMagic);
    WriteLe16(out_compressed, num_tiles);
    const uint32_t word2 = 1u | ((last_tile_size & 0x3FFFFu) << 2);
    WriteLe32(out_compressed, word2);

    if (num_tiles == 0) {
        return true;
    }

    std::vector<std::vector<uint8_t>> tiles(num_tiles);
    for (uint32_t i = 0; i < num_tiles; ++i) {
        const size_t tile_off = static_cast<size_t>(i) * kGDeflateTileSize;
        const size_t tile_len = (i + 1u < num_tiles || last_tile_size == 0u)
                                    ? static_cast<size_t>(kGDeflateTileSize)
                                    : static_cast<size_t>(last_tile_size);
        tiles[i] = CompressTile(src + tile_off, tile_len, mode);
    }

    // Tile offset table: tileOffsets[0] stores the compressed size of the LAST tile;
    // tileOffsets[i] (i > 0) stores the byte offset of tile i relative to data start.
    std::vector<uint32_t> offsets(num_tiles, 0u);
    uint64_t cursor = 0;
    for (uint32_t i = 0; i < num_tiles; ++i) {
        if (i > 0) {
            if (cursor > std::numeric_limits<uint32_t>::max()) {
                out_compressed.clear();
                return false;
            }
            offsets[i] = static_cast<uint32_t>(cursor);
        }
        cursor += tiles[i].size();
    }
    offsets[0] = static_cast<uint32_t>(tiles[num_tiles - 1u].size());

    out_compressed.reserve(kTileStreamHeaderBytes + static_cast<size_t>(num_tiles) * 4u + static_cast<size_t>(cursor));
    for (uint32_t off : offsets) {
        WriteLe32(out_compressed, off);
    }
    for (const auto& t : tiles) {
        out_compressed.insert(out_compressed.end(), t.begin(), t.end());
    }
    return true;
}

GDeflateResult GDeflateDecompress(
    const uint8_t* src,
    size_t src_size,
    uint8_t* dst,
    size_t dst_capacity,
    size_t* out_size) {
    if (!src || !out_size) {
        return GDeflateResult::InvalidArg;
    }
    ParsedTileStream parsed{};
    if (!ParseHeader(src, src_size, parsed)) {
        return GDeflateResult::InvalidStream;
    }
    *out_size = parsed.uncompressed_size;
    if (parsed.uncompressed_size == 0) {
        return GDeflateResult::Ok;
    }
    if (!dst || dst_capacity < parsed.uncompressed_size) {
        return GDeflateResult::BufferTooSmall;
    }

    const size_t table_bytes = static_cast<size_t>(parsed.num_tiles) * 4u;
    if (src_size < kTileStreamHeaderBytes + table_bytes) {
        return GDeflateResult::InvalidStream;
    }
    const uint8_t* table_ptr = src + kTileStreamHeaderBytes;
    const uint8_t* data_start = table_ptr + table_bytes;
    const size_t data_bytes = src_size - (kTileStreamHeaderBytes + table_bytes);

    for (uint32_t i = 0; i < parsed.num_tiles; ++i) {
        const uint32_t tile_off = (i > 0) ? ReadLe32(table_ptr + static_cast<size_t>(i) * 4u) : 0u;
        uint32_t tile_comp_size = 0;
        if (i + 1u < parsed.num_tiles) {
            const uint32_t next_off = ReadLe32(table_ptr + static_cast<size_t>(i + 1u) * 4u);
            if (next_off < tile_off) {
                return GDeflateResult::CorruptData;
            }
            tile_comp_size = next_off - tile_off;
        } else {
            tile_comp_size = ReadLe32(table_ptr);  // tileOffsets[0] = last tile size
        }

        if (tile_off > data_bytes || tile_comp_size > data_bytes - tile_off) {
            return GDeflateResult::CorruptData;
        }

        const size_t out_off = static_cast<size_t>(i) * kGDeflateTileSize;
        const size_t expected_tile_out =
            (i + 1u < parsed.num_tiles || parsed.last_tile_size == 0u)
                ? static_cast<size_t>(kGDeflateTileSize)
                : static_cast<size_t>(parsed.last_tile_size);

        if (!DecompressTile(data_start + tile_off, tile_comp_size, dst + out_off, expected_tile_out)) {
            return GDeflateResult::CorruptData;
        }
    }

    return GDeflateResult::Ok;
}

}  // namespace scarlettx
