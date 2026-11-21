#include "scarlettx/io/bcpack.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <queue>
#include <utility>

namespace scarlettx {

namespace {

constexpr size_t kHeaderFixedBytes = 36u;
constexpr size_t kPlaneDescBytes = 20u;
constexpr uint32_t kMaxHuffBits = 15u;

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

void WriteLe16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
}

void WriteLe32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16u) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24u) & 0xFFu);
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

const std::array<uint32_t, 256>& GetCrc32Table() noexcept {
    static const std::array<uint32_t, 256> table = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256u; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1u)) : (c >> 1u);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

struct PlaneSpec {
    BcPlaneKind kind;
    uint8_t block_offset;
    uint8_t element_bytes;
    bool is_rgb565;
};

std::vector<PlaneSpec> GetFormatPlanes(BcFormat format) {
    switch (format) {
        case BcFormat::BC1:
            return {
                {BcPlaneKind::Color0, 0, 2, true},
                {BcPlaneKind::Color1, 2, 2, true},
                {BcPlaneKind::ColorIndices, 4, 4, false},
            };
        case BcFormat::BC2:
            return {
                {BcPlaneKind::AlphaExplicit, 0, 8, false},
                {BcPlaneKind::Color0, 8, 2, true},
                {BcPlaneKind::Color1, 10, 2, true},
                {BcPlaneKind::ColorIndices, 12, 4, false},
            };
        case BcFormat::BC3:
            return {
                {BcPlaneKind::Alpha0, 0, 1, false},
                {BcPlaneKind::Alpha1, 1, 1, false},
                {BcPlaneKind::AlphaIndices, 2, 6, false},
                {BcPlaneKind::Color0, 8, 2, true},
                {BcPlaneKind::Color1, 10, 2, true},
                {BcPlaneKind::ColorIndices, 12, 4, false},
            };
        case BcFormat::BC4:
            return {
                {BcPlaneKind::Red0, 0, 1, false},
                {BcPlaneKind::Red1, 1, 1, false},
                {BcPlaneKind::RedIndices, 2, 6, false},
            };
        case BcFormat::BC5:
            return {
                {BcPlaneKind::Red0, 0, 1, false},
                {BcPlaneKind::Red1, 1, 1, false},
                {BcPlaneKind::RedIndices, 2, 6, false},
                {BcPlaneKind::Green0, 8, 1, false},
                {BcPlaneKind::Green1, 9, 1, false},
                {BcPlaneKind::GreenIndices, 10, 6, false},
            };
        case BcFormat::BC6H:
        case BcFormat::BC7:
            return {
                {BcPlaneKind::ModeHeader, 0, 2, false},
                {BcPlaneKind::Endpoints, 2, 6, false},
                {BcPlaneKind::SecondaryEndpoints, 8, 4, false},
                {BcPlaneKind::PrimaryIndices, 12, 4, false},
            };
        default:
            return {};
    }
}

void ApplyTransform(
    const std::vector<uint8_t>& in,
    uint8_t elem_bytes,
    BcPlaneTransform transform,
    std::vector<uint8_t>& out) {
    const size_t n = in.size();
    out.resize(n);
    if (n == 0 || transform == BcPlaneTransform::None) {
        out = in;
        return;
    }

    switch (transform) {
        case BcPlaneTransform::Delta8: {
            const size_t stride = std::max<size_t>(1u, elem_bytes);
            for (size_t i = 0; i < n; ++i) {
                out[i] = (i < stride) ? in[i] : static_cast<uint8_t>(in[i] - in[i - stride]);
            }
            break;
        }
        case BcPlaneTransform::Xor8: {
            const size_t stride = std::max<size_t>(1u, elem_bytes);
            for (size_t i = 0; i < n; ++i) {
                out[i] = (i < stride) ? in[i] : static_cast<uint8_t>(in[i] ^ in[i - stride]);
            }
            break;
        }
        case BcPlaneTransform::Delta16: {
            if (n % 2u != 0) {
                out = in;
                return;
            }
            uint16_t prev = 0;
            for (size_t i = 0; i < n; i += 2u) {
                const uint16_t cur = ReadLe16(in.data() + i);
                const uint16_t delta = (i == 0) ? cur : static_cast<uint16_t>(cur - prev);
                WriteLe16(out.data() + i, delta);
                prev = cur;
            }
            break;
        }
        case BcPlaneTransform::Rgb565Delta: {
            if (n % 2u != 0) {
                out = in;
                return;
            }
            uint16_t pr = 0, pg = 0, pb = 0;
            for (size_t i = 0; i < n; i += 2u) {
                const uint16_t cur = ReadLe16(in.data() + i);
                const uint16_t r = (cur >> 11u) & 31u;
                const uint16_t g = (cur >> 5u) & 63u;
                const uint16_t b = cur & 31u;
                uint16_t packed = cur;
                if (i > 0) {
                    const uint16_t dr = static_cast<uint16_t>((r - pr) & 31u);
                    const uint16_t dg = static_cast<uint16_t>((g - pg) & 63u);
                    const uint16_t db = static_cast<uint16_t>((b - pb) & 31u);
                    packed = static_cast<uint16_t>((dr << 11u) | (dg << 5u) | db);
                }
                WriteLe16(out.data() + i, packed);
                pr = r;
                pg = g;
                pb = b;
            }
            break;
        }
        case BcPlaneTransform::TransposeDelta8: {
            const size_t stride = std::max<size_t>(1u, elem_bytes);
            if (n % stride != 0) {
                out = in;
                return;
            }
            const size_t count = n / stride;
            std::vector<uint8_t> transposed(n);
            for (size_t b = 0; b < count; ++b) {
                for (size_t e = 0; e < stride; ++e) {
                    transposed[e * count + b] = in[b * stride + e];
                }
            }
            out[0] = transposed[0];
            for (size_t i = 1; i < n; ++i) {
                out[i] = static_cast<uint8_t>(transposed[i] - transposed[i - 1]);
            }
            break;
        }
        default:
            out = in;
            break;
    }
}

Status InverseTransform(
    const std::vector<uint8_t>& in,
    uint8_t elem_bytes,
    BcPlaneTransform transform,
    std::vector<uint8_t>& out) {
    const size_t n = in.size();
    out.resize(n);
    if (n == 0 || transform == BcPlaneTransform::None) {
        out = in;
        return Status::Ok();
    }

    switch (transform) {
        case BcPlaneTransform::Delta8: {
            const size_t stride = std::max<size_t>(1u, elem_bytes);
            for (size_t i = 0; i < n; ++i) {
                out[i] = (i < stride) ? in[i] : static_cast<uint8_t>(in[i] + out[i - stride]);
            }
            return Status::Ok();
        }
        case BcPlaneTransform::Xor8: {
            const size_t stride = std::max<size_t>(1u, elem_bytes);
            for (size_t i = 0; i < n; ++i) {
                out[i] = (i < stride) ? in[i] : static_cast<uint8_t>(in[i] ^ out[i - stride]);
            }
            return Status::Ok();
        }
        case BcPlaneTransform::Delta16: {
            if (n % 2u != 0) {
                return Status::Error(StatusCode::ParseError, "Delta16 plane size is not even");
            }
            uint16_t acc = 0;
            for (size_t i = 0; i < n; i += 2u) {
                const uint16_t delta = ReadLe16(in.data() + i);
                acc = (i == 0) ? delta : static_cast<uint16_t>(acc + delta);
                WriteLe16(out.data() + i, acc);
            }
            return Status::Ok();
        }
        case BcPlaneTransform::Rgb565Delta: {
            if (n % 2u != 0) {
                return Status::Error(StatusCode::ParseError, "Rgb565Delta plane size is not even");
            }
            uint16_t ar = 0, ag = 0, ab = 0;
            for (size_t i = 0; i < n; i += 2u) {
                const uint16_t d = ReadLe16(in.data() + i);
                const uint16_t dr = (d >> 11u) & 31u;
                const uint16_t dg = (d >> 5u) & 63u;
                const uint16_t db = d & 31u;
                if (i == 0) {
                    ar = dr;
                    ag = dg;
                    ab = db;
                } else {
                    ar = static_cast<uint16_t>((ar + dr) & 31u);
                    ag = static_cast<uint16_t>((ag + dg) & 63u);
                    ab = static_cast<uint16_t>((ab + db) & 31u);
                }
                WriteLe16(out.data() + i, static_cast<uint16_t>((ar << 11u) | (ag << 5u) | ab));
            }
            return Status::Ok();
        }
        case BcPlaneTransform::TransposeDelta8: {
            const size_t stride = std::max<size_t>(1u, elem_bytes);
            if (n % stride != 0) {
                return Status::Error(StatusCode::ParseError, "TransposeDelta8 plane size not divisible by stride");
            }
            const size_t count = n / stride;
            std::vector<uint8_t> transposed(n);
            transposed[0] = in[0];
            for (size_t i = 1; i < n; ++i) {
                transposed[i] = static_cast<uint8_t>(transposed[i - 1] + in[i]);
            }
            for (size_t b = 0; b < count; ++b) {
                for (size_t e = 0; e < stride; ++e) {
                    out[b * stride + e] = transposed[e * count + b];
                }
            }
            return Status::Ok();
        }
        default:
            return Status::Error(StatusCode::UnsupportedFormat, "Unknown BCPack plane transform");
    }
}

// ---------------------------------------------------------------------------
// LZ77 + RLE Codec
// ---------------------------------------------------------------------------
// Tag byte:
//   0x00..0x7F : Literal run of (tag + 1) bytes (1..128)
//   0x80..0xBF : RLE run of ((tag - 0x80) + 3) bytes (3..66) of 1 literal byte
//   0xC0..0xFE : LZ77 match of ((tag - 0xC0) + 3) bytes (3..65), followed by uint16 LE distance (1..65535)
//   0xFF       : Extended LZ77 match of (66 + next_byte) bytes (66..321), followed by uint16 LE distance
void LzRleCompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    if (n == 0) {
        return;
    }

    constexpr size_t kHashBits = 13u;
    constexpr size_t kHashSize = 1u << kHashBits;
    constexpr size_t kMaxDist = 65535u;
    constexpr size_t kMaxChain = 12u;

    std::vector<int32_t> head(kHashSize, -1);
    std::vector<int32_t> prev(n, -1);

    auto hash3 = [&](size_t pos) -> size_t {
        const uint32_t v = (static_cast<uint32_t>(src[pos]) << 16u) |
                           (static_cast<uint32_t>(src[pos + 1]) << 8u) |
                           static_cast<uint32_t>(src[pos + 2]);
        return ((v * 2654435761u) >> (32u - kHashBits)) & (kHashSize - 1u);
    };

    auto insert_pos = [&](size_t pos) {
        if (pos + 2 < n) {
            const size_t h = hash3(pos);
            prev[pos] = head[h];
            head[h] = static_cast<int32_t>(pos);
        }
    };

    std::vector<uint8_t> pending_literals;
    pending_literals.reserve(128u);

    auto flush_literals = [&]() {
        size_t off = 0;
        while (off < pending_literals.size()) {
            const size_t chunk = std::min<size_t>(128u, pending_literals.size() - off);
            out.push_back(static_cast<uint8_t>(chunk - 1u));
            out.insert(out.end(), pending_literals.begin() + static_cast<ptrdiff_t>(off),
                       pending_literals.begin() + static_cast<ptrdiff_t>(off + chunk));
            off += chunk;
        }
        pending_literals.clear();
    };

    size_t pos = 0;
    while (pos < n) {
        // 1. Check RLE run
        size_t rle_len = 1;
        while (pos + rle_len < n && rle_len < 66u && src[pos + rle_len] == src[pos]) {
            ++rle_len;
        }

        // 2. Check LZ77 match
        size_t best_len = 0;
        size_t best_dist = 0;
        if (pos + 2 < n) {
            const size_t h = hash3(pos);
            int32_t cand = head[h];
            size_t chain = 0;
            const size_t max_match = std::min<size_t>(321u, n - pos);
            while (cand >= 0 && chain < kMaxChain) {
                const size_t cpos = static_cast<size_t>(cand);
                const size_t dist = pos - cpos;
                if (dist == 0 || dist > kMaxDist) {
                    break;
                }
                if (src[cpos] == src[pos] && src[cpos + best_len] == src[pos + best_len]) {
                    size_t mlen = 0;
                    while (mlen < max_match && src[cpos + mlen] == src[pos + mlen]) {
                        ++mlen;
                    }
                    if (mlen > best_len) {
                        best_len = mlen;
                        best_dist = dist;
                        if (best_len == max_match) {
                            break;
                        }
                    }
                }
                cand = prev[cpos];
                ++chain;
            }
        }

        // Prefer RLE when length >= 3 and competitive with LZ match (RLE is only 2 bytes vs 3-4 for LZ)
        if (rle_len >= 3u && (rle_len >= best_len || best_len < 4u)) {
            flush_literals();
            out.push_back(static_cast<uint8_t>(0x80u + (rle_len - 3u)));
            out.push_back(src[pos]);
            for (size_t k = 0; k < rle_len; ++k) {
                insert_pos(pos + k);
            }
            pos += rle_len;
        } else if (best_len >= 4u) {
            flush_literals();
            if (best_len <= 65u) {
                out.push_back(static_cast<uint8_t>(0xC0u + (best_len - 3u)));
            } else {
                out.push_back(0xFFu);
                out.push_back(static_cast<uint8_t>(best_len - 66u));
            }
            AppendLe16(out, static_cast<uint16_t>(best_dist));
            for (size_t k = 0; k < best_len; ++k) {
                insert_pos(pos + k);
            }
            pos += best_len;
        } else {
            insert_pos(pos);
            pending_literals.push_back(src[pos]);
            ++pos;
            if (pending_literals.size() == 128u) {
                flush_literals();
            }
        }
    }
    flush_literals();
}

Status LzRleDecompress(const uint8_t* src, size_t src_size, size_t expected_size, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(expected_size);

    size_t pos = 0;
    while (pos < src_size) {
        const uint8_t tag = src[pos++];
        if (tag < 0x80u) {
            const size_t lit_len = static_cast<size_t>(tag) + 1u;
            if (pos + lit_len > src_size || out.size() + lit_len > expected_size) {
                return Status::Error(StatusCode::ParseError, "LZ-RLE literal run out of bounds");
            }
            out.insert(out.end(), src + pos, src + pos + lit_len);
            pos += lit_len;
        } else if (tag < 0xC0u) {
            const size_t rle_len = static_cast<size_t>(tag - 0x80u) + 3u;
            if (pos >= src_size || out.size() + rle_len > expected_size) {
                return Status::Error(StatusCode::ParseError, "LZ-RLE repeat run out of bounds");
            }
            const uint8_t val = src[pos++];
            out.insert(out.end(), rle_len, val);
        } else {
            size_t match_len = 0;
            if (tag < 0xFFu) {
                match_len = static_cast<size_t>(tag - 0xC0u) + 3u;
            } else {
                if (pos >= src_size) {
                    return Status::Error(StatusCode::ParseError, "LZ-RLE extended match truncated");
                }
                match_len = 66u + static_cast<size_t>(src[pos++]);
            }
            if (pos + 2u > src_size) {
                return Status::Error(StatusCode::ParseError, "LZ-RLE distance truncated");
            }
            const size_t dist = ReadLe16(src + pos);
            pos += 2u;
            if (dist == 0 || dist > out.size() || out.size() + match_len > expected_size) {
                return Status::Error(StatusCode::ParseError, "LZ-RLE match distance or length invalid");
            }
            const size_t copy_start = out.size() - dist;
            for (size_t i = 0; i < match_len; ++i) {
                out.push_back(out[copy_start + i]);
            }
        }
    }

    if (out.size() != expected_size) {
        return Status::Error(StatusCode::ParseError, "LZ-RLE decompressed size mismatch");
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Canonical Huffman Codec (length-limited <= 15 bits)
// ---------------------------------------------------------------------------
struct HuffNode {
    uint64_t freq = 0;
    int32_t sym = -1;
    int32_t left = -1;
    int32_t right = -1;
};

void ComputeDepths(const std::vector<HuffNode>& nodes, int32_t idx, uint32_t depth, std::array<uint8_t, 256>& lengths) {
    if (idx < 0) return;
    const auto& node = nodes[static_cast<size_t>(idx)];
    if (node.sym >= 0) {
        lengths[static_cast<size_t>(node.sym)] = static_cast<uint8_t>(std::max<uint32_t>(1u, depth));
        return;
    }
    ComputeDepths(nodes, node.left, depth + 1u, lengths);
    ComputeDepths(nodes, node.right, depth + 1u, lengths);
}

void BuildLengthLimitedHuffman(const std::array<uint64_t, 256>& freq, std::array<uint8_t, 256>& lengths) {
    lengths.fill(0);

    std::vector<int32_t> active;
    active.reserve(256u);
    for (int32_t s = 0; s < 256; ++s) {
        if (freq[static_cast<size_t>(s)] > 0) {
            active.push_back(s);
        }
    }
    if (active.empty()) {
        return;
    }
    if (active.size() == 1u) {
        lengths[static_cast<size_t>(active[0])] = 1u;
        return;
    }

    std::vector<HuffNode> nodes;
    nodes.reserve(active.size() * 2u);

    struct HeapEntry {
        uint64_t freq;
        int32_t idx;
        bool operator>(const HeapEntry& o) const noexcept {
            return freq != o.freq ? (freq > o.freq) : (idx > o.idx);
        }
    };
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<HeapEntry>> pq;

    for (int32_t s : active) {
        const int32_t idx = static_cast<int32_t>(nodes.size());
        nodes.push_back({freq[static_cast<size_t>(s)], s, -1, -1});
        pq.push({freq[static_cast<size_t>(s)], idx});
    }

    while (pq.size() > 1u) {
        const auto a = pq.top(); pq.pop();
        const auto b = pq.top(); pq.pop();
        const int32_t idx = static_cast<int32_t>(nodes.size());
        nodes.push_back({a.freq + b.freq, -1, a.idx, b.idx});
        pq.push({a.freq + b.freq, idx});
    }

    ComputeDepths(nodes, pq.top().idx, 0u, lengths);

    // Clamp depths to kMaxHuffBits and rebalance Kraft sum to exactly 2^kMaxHuffBits
    bool clamped = false;
    for (int32_t s : active) {
        if (lengths[static_cast<size_t>(s)] > kMaxHuffBits) {
            lengths[static_cast<size_t>(s)] = static_cast<uint8_t>(kMaxHuffBits);
            clamped = true;
        }
    }

    if (clamped) {
        constexpr uint32_t kKraftTarget = 1u << kMaxHuffBits;
        auto compute_kraft = [&]() -> uint32_t {
            uint32_t sum = 0;
            for (int32_t s : active) {
                sum += 1u << (kMaxHuffBits - lengths[static_cast<size_t>(s)]);
            }
            return sum;
        };

        uint32_t kraft = compute_kraft();
        // If Kraft sum overflows, lengthen codes (prefer least-frequent symbols with length < 15)
        while (kraft > kKraftTarget) {
            int32_t pick = -1;
            for (int32_t s : active) {
                const uint8_t l = lengths[static_cast<size_t>(s)];
                if (l < kMaxHuffBits) {
                    if (pick < 0 || lengths[static_cast<size_t>(s)] > lengths[static_cast<size_t>(pick)] ||
                        (lengths[static_cast<size_t>(s)] == lengths[static_cast<size_t>(pick)] &&
                         freq[static_cast<size_t>(s)] < freq[static_cast<size_t>(pick)])) {
                        pick = s;
                    }
                }
            }
            if (pick < 0) break;
            const uint8_t l = lengths[static_cast<size_t>(pick)];
            lengths[static_cast<size_t>(pick)] = static_cast<uint8_t>(l + 1u);
            kraft -= 1u << (kMaxHuffBits - l - 1u);
        }
        // If Kraft sum underflows, shorten most-frequent eligible symbols without overshooting
        while (kraft < kKraftTarget) {
            int32_t pick = -1;
            for (int32_t s : active) {
                const uint8_t l = lengths[static_cast<size_t>(s)];
                if (l > 1u) {
                    const uint32_t delta = 1u << (kMaxHuffBits - l);
                    if (kraft + delta <= kKraftTarget) {
                        if (pick < 0 || freq[static_cast<size_t>(s)] > freq[static_cast<size_t>(pick)]) {
                            pick = s;
                        }
                    }
                }
            }
            if (pick < 0) break;
            const uint8_t l = lengths[static_cast<size_t>(pick)];
            lengths[static_cast<size_t>(pick)] = static_cast<uint8_t>(l - 1u);
            kraft += 1u << (kMaxHuffBits - l);
        }
    }
}

void AssignCanonicalCodes(
    const std::array<uint8_t, 256>& lengths,
    std::array<uint16_t, 256>& codes) {
    codes.fill(0);
    std::array<uint32_t, kMaxHuffBits + 1u> count{};
    for (uint8_t l : lengths) {
        if (l > 0 && l <= kMaxHuffBits) {
            ++count[l];
        }
    }
    std::array<uint32_t, kMaxHuffBits + 1u> next_code{};
    uint32_t code = 0;
    for (uint32_t bits = 1u; bits <= kMaxHuffBits; ++bits) {
        code = (code + count[bits - 1u]) << 1u;
        next_code[bits] = code;
    }
    for (size_t s = 0; s < 256u; ++s) {
        const uint8_t l = lengths[s];
        if (l > 0 && l <= kMaxHuffBits) {
            codes[s] = static_cast<uint16_t>(next_code[l]++);
        }
    }
}

void HuffmanCompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    if (n == 0) {
        return;
    }

    std::array<uint64_t, 256> freq{};
    for (size_t i = 0; i < n; ++i) {
        ++freq[src[i]];
    }

    std::array<uint8_t, 256> lengths{};
    BuildLengthLimitedHuffman(freq, lengths);

    std::array<uint16_t, 256> codes{};
    AssignCanonicalCodes(lengths, codes);

    std::vector<uint16_t> active_syms;
    active_syms.reserve(256u);
    for (uint16_t s = 0; s < 256u; ++s) {
        if (lengths[s] > 0) {
            active_syms.push_back(s);
        }
    }

    AppendLe32(out, static_cast<uint32_t>(n));
    if (active_syms.size() <= 60u) {
        // Sparse length table: mode=1, count (1 byte), [sym, len]...
        out.push_back(1u);
        out.push_back(static_cast<uint8_t>(active_syms.size()));
        for (uint16_t s : active_syms) {
            out.push_back(static_cast<uint8_t>(s));
            out.push_back(lengths[s]);
        }
    } else {
        // Dense nibble-packed 128-byte length table: mode=0
        out.push_back(0u);
        for (size_t i = 0; i < 256u; i += 2u) {
            const uint8_t packed = static_cast<uint8_t>((lengths[i] & 0x0Fu) |
                                                        ((lengths[i + 1u] & 0x0Fu) << 4u));
            out.push_back(packed);
        }
    }

    // MSB-first bit packing
    uint64_t bit_buf = 0;
    uint32_t bit_count = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t s = src[i];
        const uint32_t len = lengths[s];
        const uint32_t code = codes[s];
        bit_buf = (bit_buf << len) | code;
        bit_count += len;
        while (bit_count >= 8u) {
            bit_count -= 8u;
            out.push_back(static_cast<uint8_t>((bit_buf >> bit_count) & 0xFFu));
        }
    }
    if (bit_count > 0) {
        out.push_back(static_cast<uint8_t>((bit_buf << (8u - bit_count)) & 0xFFu));
    }
}

Status HuffmanDecompress(const uint8_t* src, size_t src_size, size_t expected_size, std::vector<uint8_t>& out) {
    out.clear();
    if (expected_size == 0) {
        return Status::Ok();
    }
    if (src_size < 5u) {
        return Status::Error(StatusCode::ParseError, "Huffman stream header truncated");
    }

    const uint32_t stored_len = ReadLe32(src);
    if (stored_len != expected_size) {
        return Status::Error(StatusCode::ParseError, "Huffman uncompressed length mismatch");
    }

    const uint8_t table_mode = src[4];
    size_t pos = 5u;
    std::array<uint8_t, 256> lengths{};

    if (table_mode == 1u) {
        if (pos >= src_size) {
            return Status::Error(StatusCode::ParseError, "Huffman sparse count truncated");
        }
        const uint8_t count = src[pos++];
        if (count == 0 || pos + static_cast<size_t>(count) * 2u > src_size) {
            return Status::Error(StatusCode::ParseError, "Huffman sparse table invalid");
        }
        for (uint8_t i = 0; i < count; ++i) {
            const uint8_t sym = src[pos++];
            const uint8_t len = src[pos++];
            if (len == 0 || len > kMaxHuffBits) {
                return Status::Error(StatusCode::ParseError, "Huffman invalid symbol bit length");
            }
            lengths[sym] = len;
        }
    } else if (table_mode == 0u) {
        if (pos + 128u > src_size) {
            return Status::Error(StatusCode::ParseError, "Huffman dense table truncated");
        }
        for (size_t i = 0; i < 256u; i += 2u) {
            const uint8_t packed = src[pos++];
            lengths[i] = packed & 0x0Fu;
            lengths[i + 1u] = (packed >> 4u) & 0x0Fu;
        }
    } else {
        return Status::Error(StatusCode::ParseError, "Unknown Huffman table mode");
    }

    // Validate Kraft inequality and build canonical decode table
    std::array<uint32_t, kMaxHuffBits + 1u> count{};
    uint32_t kraft = 0;
    uint32_t total_syms = 0;
    for (uint8_t l : lengths) {
        if (l > 0) {
            if (l > kMaxHuffBits) {
                return Status::Error(StatusCode::ParseError, "Huffman length > 15");
            }
            ++count[l];
            ++total_syms;
            kraft += 1u << (kMaxHuffBits - l);
        }
    }
    if (total_syms == 0 || kraft > (1u << kMaxHuffBits)) {
        return Status::Error(StatusCode::ParseError, "Huffman Kraft sum invalid");
    }

    std::array<uint32_t, kMaxHuffBits + 1u> first_code{};
    std::array<uint32_t, kMaxHuffBits + 1u> first_index{};
    uint32_t code = 0;
    uint32_t idx = 0;
    for (uint32_t bits = 1u; bits <= kMaxHuffBits; ++bits) {
        code = (code + count[bits - 1u]) << 1u;
        first_code[bits] = code;
        first_index[bits] = idx;
        idx += count[bits];
    }

    std::array<uint8_t, 256> sym_table{};
    std::array<uint32_t, kMaxHuffBits + 1u> offsets = first_index;
    for (uint32_t s = 0; s < 256u; ++s) {
        const uint8_t l = lengths[s];
        if (l > 0) {
            sym_table[offsets[l]++] = static_cast<uint8_t>(s);
        }
    }

    out.resize(expected_size);
    uint32_t cur_byte = 0;
    uint32_t bits_left = 0;

    for (size_t i = 0; i < expected_size; ++i) {
        uint32_t cur_code = 0;
        bool decoded = false;
        for (uint32_t len = 1u; len <= kMaxHuffBits; ++len) {
            if (bits_left == 0) {
                if (pos >= src_size) {
                    return Status::Error(StatusCode::ParseError, "Huffman bitstream truncated");
                }
                cur_byte = src[pos++];
                bits_left = 8u;
            }
            --bits_left;
            cur_code = (cur_code << 1u) | ((cur_byte >> bits_left) & 1u);
            if (count[len] > 0 && cur_code >= first_code[len] &&
                cur_code - first_code[len] < count[len]) {
                out[i] = sym_table[first_index[len] + (cur_code - first_code[len])];
                decoded = true;
                break;
            }
        }
        if (!decoded) {
            return Status::Error(StatusCode::ParseError, "Huffman invalid canonical code");
        }
    }

    return Status::Ok();
}

}  // namespace

uint32_t BcFormatBlockBytes(BcFormat format) noexcept {
    switch (format) {
        case BcFormat::BC1:
        case BcFormat::BC4:
            return 8u;
        case BcFormat::BC2:
        case BcFormat::BC3:
        case BcFormat::BC5:
        case BcFormat::BC6H:
        case BcFormat::BC7:
            return 16u;
        default:
            return 0u;
    }
}

uint64_t BcCalculateSurfaceSize(BcFormat format, uint32_t width, uint32_t height) noexcept {
    const uint32_t block_bytes = BcFormatBlockBytes(format);
    if (block_bytes == 0 || width == 0 || height == 0 ||
        width > kBcPackMaxDimension || height > kBcPackMaxDimension) {
        return 0;
    }
    const uint64_t bw = (static_cast<uint64_t>(width) + 3ull) / 4ull;
    const uint64_t bh = (static_cast<uint64_t>(height) + 3ull) / 4ull;
    return bw * bh * static_cast<uint64_t>(block_bytes);
}

uint32_t BcPackCrc32(const uint8_t* data, size_t size) noexcept {
    const auto& table = GetCrc32Table();
    uint32_t crc = 0xFFFFFFFFu;
    if (!data) {
        return 0u;
    }
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8u);
    }
    return crc ^ 0xFFFFFFFFu;
}

Status BcPackParseHeader(const uint8_t* src, size_t src_size, BcPackHeader& out_header) {
    out_header = BcPackHeader{};
    if (!src || src_size < kHeaderFixedBytes) {
        return Status::Error(StatusCode::ParseError, "BCPack stream too small for header");
    }

    out_header.magic = ReadLe32(src + 0);
    if (out_header.magic != kBcPackMagic) {
        return Status::Error(StatusCode::ParseError, "Invalid BCPack magic (expected 'BCPK')");
    }

    out_header.version = ReadLe16(src + 4);
    if (out_header.version != kBcPackVersion) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported BCPack version");
    }

    out_header.flags = ReadLe16(src + 6);
    out_header.format = static_cast<BcFormat>(ReadLe32(src + 8));
    out_header.width = ReadLe32(src + 12);
    out_header.height = ReadLe32(src + 16);
    out_header.block_count = ReadLe32(src + 20);
    out_header.original_size = ReadLe32(src + 24);
    out_header.plane_count = ReadLe32(src + 28);
    out_header.uncompressed_crc32 = ReadLe32(src + 32);

    const uint32_t block_bytes = BcFormatBlockBytes(out_header.format);
    if (block_bytes == 0) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported BC format in BCPack header");
    }
    if (out_header.width == 0 || out_header.height == 0 ||
        out_header.width > kBcPackMaxDimension || out_header.height > kBcPackMaxDimension) {
        return Status::Error(StatusCode::ParseError, "Invalid BCPack texture dimensions");
    }

    const uint64_t bw = (static_cast<uint64_t>(out_header.width) + 3ull) / 4ull;
    const uint64_t bh = (static_cast<uint64_t>(out_header.height) + 3ull) / 4ull;
    const uint64_t expected_blocks = bw * bh;
    const uint64_t expected_bytes = expected_blocks * static_cast<uint64_t>(block_bytes);

    if (expected_blocks != out_header.block_count || expected_bytes != out_header.original_size) {
        return Status::Error(StatusCode::ParseError, "BCPack block count / original size mismatch");
    }

    const auto expected_specs = GetFormatPlanes(out_header.format);
    if (out_header.plane_count == 0 || out_header.plane_count > kBcPackMaxPlanes ||
        out_header.plane_count != expected_specs.size()) {
        return Status::Error(StatusCode::ParseError, "Invalid BCPack plane count");
    }

    const size_t table_end = kHeaderFixedBytes + static_cast<size_t>(out_header.plane_count) * kPlaneDescBytes;
    if (table_end > src_size) {
        return Status::Error(StatusCode::ParseError, "BCPack plane descriptor table truncated");
    }

    out_header.planes.resize(out_header.plane_count);
    for (uint32_t i = 0; i < out_header.plane_count; ++i) {
        const uint8_t* p = src + kHeaderFixedBytes + static_cast<size_t>(i) * kPlaneDescBytes;
        BcPackPlaneDescriptor& d = out_header.planes[i];
        d.kind = static_cast<BcPlaneKind>(p[0]);
        d.transform = static_cast<BcPlaneTransform>(p[1]);
        d.codec = static_cast<BcPlaneCodec>(p[2]);
        d.element_bytes = p[3];
        d.raw_size = ReadLe32(p + 4);
        d.compressed_size = ReadLe32(p + 8);
        d.data_offset = ReadLe32(p + 12);
        d.plane_crc32 = ReadLe32(p + 16);

        if (d.element_bytes != expected_specs[i].element_bytes ||
            d.raw_size != static_cast<uint64_t>(out_header.block_count) * d.element_bytes) {
            return Status::Error(StatusCode::ParseError, "BCPack plane descriptor raw_size mismatch");
        }
        if (d.data_offset < table_end ||
            static_cast<uint64_t>(d.data_offset) + static_cast<uint64_t>(d.compressed_size) > src_size) {
            return Status::Error(StatusCode::ParseError, "BCPack plane payload out of bounds");
        }
    }

    return Status::Ok();
}

Status BcPackCompress(
    BcFormat format,
    uint32_t width,
    uint32_t height,
    const uint8_t* src_blocks,
    size_t src_size,
    std::vector<uint8_t>& out_compressed,
    BcPackStats* out_stats) {
    out_compressed.clear();

    const uint32_t block_bytes = BcFormatBlockBytes(format);
    if (block_bytes == 0) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported BC format");
    }
    if (width == 0 || height == 0 || width > kBcPackMaxDimension || height > kBcPackMaxDimension) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid BC texture dimensions");
    }
    const uint64_t expected_size = BcCalculateSurfaceSize(format, width, height);
    if (!src_blocks || src_size != expected_size || expected_size > std::numeric_limits<uint32_t>::max()) {
        return Status::Error(StatusCode::InvalidArgument, "BC surface buffer pointer or size invalid");
    }

    const uint32_t block_count = static_cast<uint32_t>(expected_size / block_bytes);
    const auto specs = GetFormatPlanes(format);
    const uint32_t plane_count = static_cast<uint32_t>(specs.size());

    std::vector<BcPackPlaneDescriptor> descs(plane_count);
    std::vector<std::vector<uint8_t>> payloads(plane_count);

    for (uint32_t p = 0; p < plane_count; ++p) {
        const PlaneSpec& spec = specs[p];
        const size_t raw_size = static_cast<size_t>(block_count) * spec.element_bytes;
        std::vector<uint8_t> raw_plane(raw_size);

        for (uint32_t b = 0; b < block_count; ++b) {
            const uint8_t* blk = src_blocks + static_cast<size_t>(b) * block_bytes + spec.block_offset;
            std::memcpy(raw_plane.data() + static_cast<size_t>(b) * spec.element_bytes,
                        blk, spec.element_bytes);
        }

        // Candidate transforms suited for each plane geometry
        std::vector<BcPlaneTransform> candidate_transforms;
        candidate_transforms.push_back(BcPlaneTransform::None);
        candidate_transforms.push_back(BcPlaneTransform::Delta8);
        candidate_transforms.push_back(BcPlaneTransform::Xor8);
        if (spec.is_rgb565) {
            candidate_transforms.push_back(BcPlaneTransform::Rgb565Delta);
            candidate_transforms.push_back(BcPlaneTransform::Delta16);
        }
        if (spec.element_bytes > 1u) {
            candidate_transforms.push_back(BcPlaneTransform::TransposeDelta8);
        }

        BcPlaneTransform best_transform = BcPlaneTransform::None;
        BcPlaneCodec best_codec = BcPlaneCodec::Raw;
        std::vector<uint8_t> best_payload = raw_plane;

        std::vector<uint8_t> transformed;
        std::vector<uint8_t> lz_buf;
        std::vector<uint8_t> huff_buf;
        std::vector<uint8_t> lz_huff_buf;

        for (BcPlaneTransform tr : candidate_transforms) {
            ApplyTransform(raw_plane, spec.element_bytes, tr, transformed);

            // Option A: LZ-RLE
            LzRleCompress(transformed.data(), transformed.size(), lz_buf);
            if (lz_buf.size() < best_payload.size()) {
                best_transform = tr;
                best_codec = BcPlaneCodec::LzRle;
                best_payload = lz_buf;
            }

            // Option B: Canonical Huffman directly on transformed plane
            HuffmanCompress(transformed.data(), transformed.size(), huff_buf);
            if (huff_buf.size() < best_payload.size()) {
                best_transform = tr;
                best_codec = BcPlaneCodec::CanonicalHuffman;
                best_payload = huff_buf;
            }

            // Option C: LZ-RLE followed by Canonical Huffman
            if (!lz_buf.empty()) {
                HuffmanCompress(lz_buf.data(), lz_buf.size(), lz_huff_buf);
                if (lz_huff_buf.size() < best_payload.size()) {
                    best_transform = tr;
                    best_codec = BcPlaneCodec::LzRleHuffman;
                    best_payload = lz_huff_buf;
                }
            }
        }

        descs[p].kind = spec.kind;
        descs[p].transform = best_transform;
        descs[p].codec = best_codec;
        descs[p].element_bytes = spec.element_bytes;
        descs[p].raw_size = static_cast<uint32_t>(raw_size);
        descs[p].compressed_size = static_cast<uint32_t>(best_payload.size());
        descs[p].plane_crc32 = BcPackCrc32(best_payload.data(), best_payload.size());
        payloads[p] = std::move(best_payload);
    }

    const size_t header_total = kHeaderFixedBytes + static_cast<size_t>(plane_count) * kPlaneDescBytes;
    size_t total_out = header_total;
    for (uint32_t p = 0; p < plane_count; ++p) {
        descs[p].data_offset = static_cast<uint32_t>(total_out);
        total_out += payloads[p].size();
    }

    out_compressed.reserve(total_out);
    AppendLe32(out_compressed, kBcPackMagic);
    AppendLe16(out_compressed, kBcPackVersion);
    AppendLe16(out_compressed, 0u);  // flags
    AppendLe32(out_compressed, static_cast<uint32_t>(format));
    AppendLe32(out_compressed, width);
    AppendLe32(out_compressed, height);
    AppendLe32(out_compressed, block_count);
    AppendLe32(out_compressed, static_cast<uint32_t>(expected_size));
    AppendLe32(out_compressed, plane_count);
    AppendLe32(out_compressed, BcPackCrc32(src_blocks, src_size));

    for (uint32_t p = 0; p < plane_count; ++p) {
        const auto& d = descs[p];
        out_compressed.push_back(static_cast<uint8_t>(d.kind));
        out_compressed.push_back(static_cast<uint8_t>(d.transform));
        out_compressed.push_back(static_cast<uint8_t>(d.codec));
        out_compressed.push_back(d.element_bytes);
        AppendLe32(out_compressed, d.raw_size);
        AppendLe32(out_compressed, d.compressed_size);
        AppendLe32(out_compressed, d.data_offset);
        AppendLe32(out_compressed, d.plane_crc32);
    }

    for (uint32_t p = 0; p < plane_count; ++p) {
        out_compressed.insert(out_compressed.end(), payloads[p].begin(), payloads[p].end());
    }

    if (out_stats) {
        out_stats->original_size = static_cast<uint32_t>(expected_size);
        out_stats->compressed_size = static_cast<uint32_t>(out_compressed.size());
        out_stats->block_count = block_count;
        out_stats->plane_count = plane_count;
        out_stats->compression_ratio =
            static_cast<double>(out_compressed.size()) / static_cast<double>(expected_size);
    }

    return Status::Ok();
}

Status BcPackDecompress(
    const uint8_t* src,
    size_t src_size,
    std::vector<uint8_t>& out_blocks,
    BcPackHeader* out_header) {
    out_blocks.clear();

    BcPackHeader hdr{};
    Status st = BcPackParseHeader(src, src_size, hdr);
    if (!st.ok()) {
        return st;
    }

    const uint32_t block_bytes = BcFormatBlockBytes(hdr.format);
    const auto specs = GetFormatPlanes(hdr.format);
    out_blocks.assign(hdr.original_size, 0u);

    std::vector<uint8_t> decoded_stage1;
    std::vector<uint8_t> decoded_transformed;
    std::vector<uint8_t> raw_plane;

    for (uint32_t p = 0; p < hdr.plane_count; ++p) {
        const auto& d = hdr.planes[p];
        const uint8_t* payload = src + d.data_offset;

        // Verify per-plane payload CRC32 before decoding
        const uint32_t actual_plane_crc = BcPackCrc32(payload, d.compressed_size);
        if (actual_plane_crc != d.plane_crc32) {
            out_blocks.clear();
            return Status::Error(StatusCode::ParseError, "BCPack plane CRC32 mismatch");
        }

        switch (d.codec) {
            case BcPlaneCodec::Raw: {
                if (d.compressed_size != d.raw_size) {
                    out_blocks.clear();
                    return Status::Error(StatusCode::ParseError, "BCPack raw plane size mismatch");
                }
                decoded_transformed.assign(payload, payload + d.compressed_size);
                break;
            }
            case BcPlaneCodec::LzRle: {
                st = LzRleDecompress(payload, d.compressed_size, d.raw_size, decoded_transformed);
                if (!st.ok()) {
                    out_blocks.clear();
                    return st;
                }
                break;
            }
            case BcPlaneCodec::CanonicalHuffman: {
                st = HuffmanDecompress(payload, d.compressed_size, d.raw_size, decoded_transformed);
                if (!st.ok()) {
                    out_blocks.clear();
                    return st;
                }
                break;
            }
            case BcPlaneCodec::LzRleHuffman: {
                if (d.compressed_size < 4u) {
                    out_blocks.clear();
                    return Status::Error(StatusCode::ParseError, "BCPack LZ-Huffman header truncated");
                }
                const uint32_t lz_len = ReadLe32(payload);
                // Sanity bound: LZ stream cannot wildly exceed raw_size + framing
                if (lz_len > d.raw_size + (d.raw_size / 64u) + 1024u) {
                    out_blocks.clear();
                    return Status::Error(StatusCode::ParseError, "BCPack LZ-Huffman intermediate size invalid");
                }
                st = HuffmanDecompress(payload, d.compressed_size, lz_len, decoded_stage1);
                if (!st.ok()) {
                    out_blocks.clear();
                    return st;
                }
                st = LzRleDecompress(decoded_stage1.data(), decoded_stage1.size(), d.raw_size, decoded_transformed);
                if (!st.ok()) {
                    out_blocks.clear();
                    return st;
                }
                break;
            }
            default:
                out_blocks.clear();
                return Status::Error(StatusCode::UnsupportedFormat, "Unknown BCPack plane codec");
        }

        st = InverseTransform(decoded_transformed, d.element_bytes, d.transform, raw_plane);
        if (!st.ok()) {
            out_blocks.clear();
            return st;
        }

        // Reassemble plane bytes back into BC blocks
        const PlaneSpec& spec = specs[p];
        for (uint32_t b = 0; b < hdr.block_count; ++b) {
            uint8_t* dst_blk = out_blocks.data() + static_cast<size_t>(b) * block_bytes + spec.block_offset;
            std::memcpy(dst_blk,
                        raw_plane.data() + static_cast<size_t>(b) * spec.element_bytes,
                        spec.element_bytes);
        }
    }

    // Final end-to-end CRC32 verification of reconstructed BC surface
    const uint32_t actual_crc = BcPackCrc32(out_blocks.data(), out_blocks.size());
    if (actual_crc != hdr.uncompressed_crc32) {
        out_blocks.clear();
        return Status::Error(StatusCode::ParseError, "BCPack uncompressed surface CRC32 mismatch");
    }

    if (out_header) {
        *out_header = std::move(hdr);
    }
    return Status::Ok();
}

}  // namespace scarlettx
