#define SCARLETTX_BUILDING_BINK2 1
#include "scarlettx/middleware/bink2_abi.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using SteadyClock = std::chrono::steady_clock;

uint32_t AlignUp32(uint32_t v, uint32_t align) {
    return (v + align - 1u) & ~(align - 1u);
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void SynthesizeRgbaPixels(uint32_t w, uint32_t h, uint32_t frame_num, uint8_t* dst_rgba) {
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4ull;
            dst_rgba[idx + 0] = static_cast<uint8_t>((x * 4u + frame_num * 17u) & 0xFFu);
            dst_rgba[idx + 1] = static_cast<uint8_t>((y * 4u + frame_num * 9u) & 0xFFu);
            dst_rgba[idx + 2] = static_cast<uint8_t>(((x ^ y) + frame_num * 23u) & 0xFFu);
            dst_rgba[idx + 3] = 255u;
        }
    }
}

void WritePlanarYCbCrFromRgba(const uint8_t* rgba,
                              uint32_t w,
                              uint32_t h,
                              uint32_t frame_num,
                              BINKFRAMEBUFFERS& fb) {
    const int32_t total = std::clamp(fb.TotalFrames, 1, 2);
    const uint32_t slot = (frame_num > 0 ? (frame_num - 1u) : 0u) % static_cast<uint32_t>(total);
    BINKFRAMEPLANESET& ps = fb.Frames[slot];

    if (ps.YPlane && ps.YPitch > 0) {
        auto* y_dst = static_cast<uint8_t*>(ps.YPlane);
        const uint32_t copy_w = std::min(w, ps.YPitch);
        const uint32_t copy_h = fb.YABufferHeight > 0 ? std::min(h, fb.YABufferHeight) : h;
        for (uint32_t y = 0; y < copy_h; ++y) {
            uint8_t* row = y_dst + static_cast<size_t>(y) * ps.YPitch;
            for (uint32_t x = 0; x < copy_w; ++x) {
                const size_t idx = (static_cast<size_t>(y) * w + x) * 4ull;
                const int r = rgba[idx + 0];
                const int g = rgba[idx + 1];
                const int b = rgba[idx + 2];
                const int luma = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                row[x] = static_cast<uint8_t>(std::clamp(luma, 16, 235));
            }
        }
    }

    if (ps.APlane && ps.APitch > 0) {
        auto* a_dst = static_cast<uint8_t*>(ps.APlane);
        const uint32_t copy_w = std::min(w, ps.APitch);
        const uint32_t copy_h = fb.YABufferHeight > 0 ? std::min(h, fb.YABufferHeight) : h;
        for (uint32_t y = 0; y < copy_h; ++y) {
            uint8_t* row = a_dst + static_cast<size_t>(y) * ps.APitch;
            for (uint32_t x = 0; x < copy_w; ++x) {
                const size_t idx = (static_cast<size_t>(y) * w + x) * 4ull;
                row[x] = rgba[idx + 3];
            }
        }
    }

    const uint32_t cw = (w + 1u) / 2u;
    const uint32_t ch = (h + 1u) / 2u;
    if (ps.cBPlane && ps.cBPitch > 0) {
        auto* cb_dst = static_cast<uint8_t*>(ps.cBPlane);
        const uint32_t copy_w = std::min(cw, ps.cBPitch);
        const uint32_t copy_h = fb.cRcBBufferHeight > 0 ? std::min(ch, fb.cRcBBufferHeight) : ch;
        for (uint32_t y = 0; y < copy_h; ++y) {
            uint8_t* row = cb_dst + static_cast<size_t>(y) * ps.cBPitch;
            const uint32_t sy = std::min(y * 2u, h - 1u);
            for (uint32_t x = 0; x < copy_w; ++x) {
                const uint32_t sx = std::min(x * 2u, w - 1u);
                const size_t idx = (static_cast<size_t>(sy) * w + sx) * 4ull;
                const int r = rgba[idx + 0];
                const int g = rgba[idx + 1];
                const int b = rgba[idx + 2];
                const int cb = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                row[x] = static_cast<uint8_t>(std::clamp(cb, 16, 240));
            }
        }
    }

    if (ps.cRPlane && ps.cRPitch > 0) {
        auto* cr_dst = static_cast<uint8_t*>(ps.cRPlane);
        const uint32_t copy_w = std::min(cw, ps.cRPitch);
        const uint32_t copy_h = fb.cRcBBufferHeight > 0 ? std::min(ch, fb.cRcBBufferHeight) : ch;
        for (uint32_t y = 0; y < copy_h; ++y) {
            uint8_t* row = cr_dst + static_cast<size_t>(y) * ps.cRPitch;
            const uint32_t sy = std::min(y * 2u, h - 1u);
            for (uint32_t x = 0; x < copy_w; ++x) {
                const uint32_t sx = std::min(x * 2u, w - 1u);
                const size_t idx = (static_cast<size_t>(sy) * w + sx) * 4ull;
                const int r = rgba[idx + 0];
                const int g = rgba[idx + 1];
                const int b = rgba[idx + 2];
                const int cr = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                row[x] = static_cast<uint8_t>(std::clamp(cr, 16, 240));
            }
        }
    }

    fb.FrameNum = slot;
}

struct AsyncDecodeJob {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frame_num = 1;
    bool paused = false;
    bool has_fb = false;
    BINKFRAMEBUFFERS fb{};

    std::vector<uint8_t> decoded_rgba;
    uint64_t decomp_us = 0;
};

struct BinkStreamState {
    BINK pub{};
    std::string path;
    uint32_t file_fps = 30;
    uint32_t file_fps_div = 1;
    uint32_t played_frames = 0;
    uint32_t skipped_frames = 0;
    uint32_t bg_io_suspended = 0;
    uint32_t read_buffer_size = 1024u * 1024u;
    uint32_t read_buffer_used = 256u * 1024u;
    uint64_t total_video_decomp_us = 0;
    uint64_t total_blit_us = 0;
    uint64_t open_time_us = 0;
    uint64_t service_calls = 0;
    SteadyClock::time_point open_tp = SteadyClock::now();
    SteadyClock::time_point last_frame_tp = SteadyClock::now();

    // Custom-allocator-backed RGBA pixel buffer
    uint8_t* frame_rgba_ptr = nullptr;
    size_t frame_rgba_bytes = 0;
    BINKMEMFREE free_fn = nullptr;

    // Registered YCbCr frame buffers
    bool has_frame_buffers = false;
    BINKFRAMEBUFFERS registered_fb{};
    BINKFRAMEBUFFERS* registered_fb_ptr = nullptr;

    // Async decode worker
    std::shared_ptr<AsyncDecodeJob> async_job;
    std::thread async_thread;

    ~BinkStreamState() {
        if (async_thread.joinable()) {
            async_thread.join();
        }
        if (frame_rgba_ptr) {
            if (free_fn) {
                free_fn(frame_rgba_ptr);
            } else {
                std::free(frame_rgba_ptr);
            }
            frame_rgba_ptr = nullptr;
            frame_rgba_bytes = 0;
        }
    }
};

std::mutex g_mutex;
uint64_t g_next_id = 1;
std::unordered_map<uint64_t, std::unique_ptr<BinkStreamState>> g_streams;
std::string g_last_error;
BINKMEMALLOC g_alloc_fn = nullptr;
BINKMEMFREE g_free_fn = nullptr;
uint32_t g_force_fps = 0;
uint32_t g_force_fps_div = 0;

bool AllocateFrameBufferLocked(BinkStreamState& s) {
    const size_t bytes = static_cast<size_t>(s.pub.Width) * s.pub.Height * 4ull;
    if (s.frame_rgba_ptr && s.frame_rgba_bytes == bytes) {
        return true;
    }
    if (s.frame_rgba_ptr) {
        if (s.free_fn) {
            s.free_fn(s.frame_rgba_ptr);
        } else {
            std::free(s.frame_rgba_ptr);
        }
        s.frame_rgba_ptr = nullptr;
        s.frame_rgba_bytes = 0;
        s.free_fn = nullptr;
    }
    void* mem = g_alloc_fn ? g_alloc_fn(bytes) : std::malloc(bytes);
    if (!mem) {
        return false;
    }
    s.frame_rgba_ptr = static_cast<uint8_t*>(mem);
    s.frame_rgba_bytes = bytes;
    s.free_fn = g_alloc_fn ? g_free_fn : nullptr;
    std::memset(s.frame_rgba_ptr, 0, bytes);
    return true;
}

void SynthesizeFrame(BinkStreamState& s) {
    if (!s.frame_rgba_ptr) return;
    const auto t0 = SteadyClock::now();
    SynthesizeRgbaPixels(s.pub.Width, s.pub.Height, s.pub.FrameNum, s.frame_rgba_ptr);
    if (s.has_frame_buffers) {
        WritePlanarYCbCrFromRgba(s.frame_rgba_ptr, s.pub.Width, s.pub.Height, s.pub.FrameNum, s.registered_fb);
        if (s.registered_fb_ptr) {
            s.registered_fb_ptr->FrameNum = s.registered_fb.FrameNum;
        }
    }
    const auto dt = std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - t0).count();
    s.total_video_decomp_us += static_cast<uint64_t>(std::max<int64_t>(dt, 1));
    s.last_frame_tp = SteadyClock::now();
}

BINK* OpenInternal(const char* name, const BINK_OPEN_OPTIONS* opts, uint32_t flags) {
    const auto t0 = SteadyClock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!name || name[0] == '\0') {
        g_last_error = "BinkOpen: empty filename";
        return nullptr;
    }

    auto state = std::make_unique<BinkStreamState>();
    state->path = name;
    state->pub.Width = 320;
    state->pub.Height = 180;
    state->pub.Frames = 60;
    state->pub.FrameNum = 1;
    state->pub.LastFrameNum = 0;
    state->pub.FrameRate = 30;
    state->pub.FrameRateDiv = 1;
    state->pub.OpenFlags = flags;
    state->pub.Paused = 0;
    state->pub.SoundOn = 1;
    state->pub.Volume = 32768;

    if (opts && opts->MaxBufferSize > 0) {
        state->read_buffer_size = opts->MaxBufferSize;
        state->read_buffer_used = std::min(state->read_buffer_used, state->read_buffer_size);
    }

    // Parse real BIK2 / KB2 / BIKi header if a file exists on disk
    std::ifstream file(name, std::ios::binary);
    if (file.is_open()) {
        if (opts && opts->FileOffset > 0) {
            file.seekg(static_cast<std::streamoff>(opts->FileOffset), std::ios::beg);
        }
        uint8_t hdr[44] = {};
        file.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
        if (file.gcount() >= 40) {
            const bool is_bink1 = (hdr[0] == 'B' && hdr[1] == 'I' && hdr[2] == 'K');
            const bool is_bink2 = (hdr[0] == 'K' && hdr[1] == 'B' && hdr[2] == '2');
            if (is_bink1 || is_bink2) {
                const uint32_t frames = ReadLe32(hdr + 8);
                const uint32_t width = ReadLe32(hdr + 20);
                const uint32_t height = ReadLe32(hdr + 24);
                const uint32_t fps = ReadLe32(hdr + 28);
                const uint32_t fps_div = ReadLe32(hdr + 32);
                if (width > 0 && width <= 7680 && height > 0 && height <= 4320) {
                    state->pub.Width = width;
                    state->pub.Height = height;
                }
                if (frames > 0 && frames <= 1000000) {
                    state->pub.Frames = frames;
                }
                if (fps > 0 && fps_div > 0) {
                    state->pub.FrameRate = fps;
                    state->pub.FrameRateDiv = fps_div;
                }
            }
        }
    }

    state->file_fps = state->pub.FrameRate;
    state->file_fps_div = state->pub.FrameRateDiv;
    if (g_force_fps > 0 && g_force_fps_div > 0) {
        state->pub.FrameRate = g_force_fps;
        state->pub.FrameRateDiv = g_force_fps_div;
    }

    if (!AllocateFrameBufferLocked(*state)) {
        g_last_error = "BinkOpen: failed to allocate frame buffer";
        return nullptr;
    }

    state->pub.InternalId = g_next_id++;
    SynthesizeFrame(*state);
    state->open_tp = SteadyClock::now();
    state->last_frame_tp = state->open_tp;
    state->open_time_us = static_cast<uint64_t>(
        std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::microseconds>(state->open_tp - t0).count()));

    BINK* ret = &state->pub;
    g_streams[state->pub.InternalId] = std::move(state);
    g_last_error.clear();
    return ret;
}

void CommitFinishedAsyncLocked(BinkStreamState& s, const std::shared_ptr<AsyncDecodeJob>& job) {
    if (!job->paused && !job->decoded_rgba.empty() && s.frame_rgba_ptr &&
        s.frame_rgba_bytes == job->decoded_rgba.size()) {
        std::memcpy(s.frame_rgba_ptr, job->decoded_rgba.data(), s.frame_rgba_bytes);
        s.pub.LastFrameNum = job->frame_num;
        ++s.played_frames;
        s.total_video_decomp_us += job->decomp_us;
        s.last_frame_tp = SteadyClock::now();
        if (s.has_frame_buffers) {
            s.registered_fb.FrameNum = job->fb.FrameNum;
            if (s.registered_fb_ptr) {
                s.registered_fb_ptr->FrameNum = job->fb.FrameNum;
            }
        }
    }
    s.async_job.reset();
}

}  // namespace

extern "C" {

BINK* BinkOpen(const char* name, uint32_t flags) {
    return OpenInternal(name, nullptr, flags);
}

BINK* BinkOpenWithOptions(const char* name, const BINK_OPEN_OPTIONS* opts, uint32_t flags) {
    return OpenInternal(name, opts, flags);
}

void BinkClose(BINK* bnk) {
    if (!bnk) return;
    std::unique_ptr<BinkStreamState> owned;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_streams.find(bnk->InternalId);
        if (it != g_streams.end()) {
            owned = std::move(it->second);
            g_streams.erase(it);
        }
    }
    // ~BinkStreamState joins async_thread and invokes free_fn outside g_mutex.
}

int32_t BinkDoFrame(BINK* bnk) {
    if (!bnk) return -1;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return -1;
    auto& s = *it->second;
    if (!s.pub.Paused) {
        SynthesizeFrame(s);
        s.pub.LastFrameNum = s.pub.FrameNum;
        ++s.played_frames;
    }
    return 0;  // 0 = frame decoded (no skip)
}

void BinkNextFrame(BINK* bnk) {
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    auto& s = *it->second;
    if (!s.pub.Paused) {
        s.pub.FrameNum = (s.pub.FrameNum % std::max(1u, s.pub.Frames)) + 1u;
    }
}

int32_t BinkWait(BINK* bnk) {
    if (!bnk) return 0;
    return bnk->Paused ? 1 : 0;  // 0 = ready for next frame immediately
}

int32_t BinkCopyToBuffer(
    BINK* bnk,
    void* dest,
    int32_t dest_pitch,
    uint32_t dest_height,
    uint32_t dest_x,
    uint32_t dest_y,
    uint32_t flags) {
    if (!bnk || !dest || dest_pitch <= 0 || dest_height == 0) {
        return -1;
    }
    const auto t0 = SteadyClock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return -1;
    auto& s = *it->second;
    if (!s.frame_rgba_ptr) return -1;

    const uint32_t fmt = flags & 0x0Fu;
    const bool is_bgra = (fmt == BINKSURFACE32BGRA);
    const uint32_t copy_h = std::min(s.pub.Height, dest_height > dest_y ? dest_height - dest_y : 0u);
    auto* dst_base = static_cast<uint8_t*>(dest);

    for (uint32_t y = 0; y < copy_h; ++y) {
        uint8_t* dst_row = dst_base + static_cast<size_t>(dest_y + y) * static_cast<size_t>(dest_pitch) +
                           static_cast<size_t>(dest_x) * 4ull;
        const uint8_t* src_row = s.frame_rgba_ptr + static_cast<size_t>(y) * s.pub.Width * 4ull;
        if (!is_bgra) {
            std::memcpy(dst_row, src_row, static_cast<size_t>(s.pub.Width) * 4ull);
        } else {
            for (uint32_t x = 0; x < s.pub.Width; ++x) {
                dst_row[x * 4 + 0] = src_row[x * 4 + 2];
                dst_row[x * 4 + 1] = src_row[x * 4 + 1];
                dst_row[x * 4 + 2] = src_row[x * 4 + 0];
                dst_row[x * 4 + 3] = src_row[x * 4 + 3];
            }
        }
    }
    const auto dt = std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - t0).count();
    s.total_blit_us += static_cast<uint64_t>(std::max<int64_t>(dt, 1));
    return 0;
}

int32_t BinkPause(BINK* bnk, int32_t pause) {
    if (!bnk) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return 0;
    it->second->pub.Paused = pause ? 1u : 0u;
    it->second->last_frame_tp = SteadyClock::now();
    return static_cast<int32_t>(it->second->pub.Paused);
}

void BinkGoto(BINK* bnk, uint32_t frame, int32_t flags) {
    (void)flags;
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    auto& s = *it->second;
    const uint32_t clamped = std::clamp(frame, 1u, std::max(1u, s.pub.Frames));
    s.pub.FrameNum = clamped;
    SynthesizeFrame(s);
}

void BinkSetSoundOnOff(BINK* bnk, int32_t onoff) {
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it != g_streams.end()) {
        it->second->pub.SoundOn = onoff ? 1u : 0u;
    }
}

void BinkSetVolume(BINK* bnk, uint32_t track_id, int32_t volume) {
    (void)track_id;
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it != g_streams.end()) {
        it->second->pub.Volume = std::clamp(volume, 0, 65536);
    }
}

void BinkGetSummary(BINK* bnk, BINKSUMMARY* sum) {
    if (!bnk || !sum) return;
    std::memset(sum, 0, sizeof(*sum));
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    const auto& s = *it->second;
    sum->Width = s.pub.Width;
    sum->Height = s.pub.Height;
    sum->FileFrameRate = s.file_fps;
    sum->FileFrameRateDiv = s.file_fps_div;
    sum->FrameRate = s.pub.FrameRate;
    sum->FrameRateDiv = s.pub.FrameRateDiv;
    sum->TotalOpenTime = static_cast<uint32_t>(std::max<uint64_t>(1ull, s.open_time_us / 1000ull));
    sum->TotalFrames = s.pub.Frames;
    sum->TotalPlayedFrames = s.played_frames;
    sum->SkippedFrames = s.skipped_frames;
    sum->TotalTime = (s.pub.Frames * 1000u * std::max(1u, s.pub.FrameRateDiv)) /
                     std::max(1u, s.pub.FrameRate);
}

const char* BinkGetError(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_last_error.c_str();
}

void BinkSetMemory(BINKMEMALLOC alloc_fn, BINKMEMFREE free_fn) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (alloc_fn && free_fn) {
        g_alloc_fn = alloc_fn;
        g_free_fn = free_fn;
    } else {
        g_alloc_fn = nullptr;
        g_free_fn = nullptr;
    }
}

int32_t BinkControlBackgroundIO(BINK* bnk, uint32_t control) {
    if (!bnk) return -1;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return -1;
    auto& s = *it->second;
    const uint32_t cmd = control & ~BINKBGIOFORCE;
    if (cmd == BINKBGIOSUSPEND) {
        s.bg_io_suspended = 1u;
    } else if (cmd == BINKBGIORESUME) {
        s.bg_io_suspended = 0u;
    }
    return static_cast<int32_t>(s.bg_io_suspended);
}

int32_t BinkDoFrameAsync(BINK* bnk, uint32_t thread_one, uint32_t thread_two) {
    (void)thread_one;
    (void)thread_two;
    if (!bnk) return 0;

    std::thread prev_thread;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_streams.find(bnk->InternalId);
        if (it == g_streams.end()) return 0;
        auto& s = *it->second;

        if (s.async_job) {
            std::lock_guard<std::mutex> jlk(s.async_job->mu);
            if (!s.async_job->done) {
                return 0;  // Async decode already in progress
            }
            prev_thread = std::move(s.async_thread);
            CommitFinishedAsyncLocked(s, s.async_job);
        } else if (s.async_thread.joinable()) {
            prev_thread = std::move(s.async_thread);
        }

        auto job = std::make_shared<AsyncDecodeJob>();
        job->width = s.pub.Width;
        job->height = s.pub.Height;
        job->frame_num = s.pub.FrameNum;
        job->paused = (s.pub.Paused != 0);
        job->has_fb = s.has_frame_buffers;
        job->fb = s.registered_fb;

        s.async_job = job;
        s.async_thread = std::thread([job]() {
            const auto t0 = SteadyClock::now();
            if (!job->paused) {
                const size_t bytes = static_cast<size_t>(job->width) * job->height * 4ull;
                job->decoded_rgba.resize(bytes);
                SynthesizeRgbaPixels(job->width, job->height, job->frame_num, job->decoded_rgba.data());
                if (job->has_fb) {
                    WritePlanarYCbCrFromRgba(
                        job->decoded_rgba.data(), job->width, job->height, job->frame_num, job->fb);
                }
            }
            const auto dt = std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - t0).count();
            job->decomp_us = static_cast<uint64_t>(std::max<int64_t>(dt, 1));
            {
                std::lock_guard<std::mutex> jlk(job->mu);
                job->done = true;
            }
            job->cv.notify_all();
        });
    }

    if (prev_thread.joinable()) {
        prev_thread.join();
    }
    return 1;
}

int32_t BinkDoFrameAsyncWait(BINK* bnk, int32_t us) {
    if (!bnk) return 0;

    std::shared_ptr<AsyncDecodeJob> job;
    uint64_t stream_id = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_streams.find(bnk->InternalId);
        if (it == g_streams.end()) return 0;
        stream_id = bnk->InternalId;
        job = it->second->async_job;
    }

    if (!job) {
        return 1;  // No pending async work
    }

    {
        std::unique_lock<std::mutex> jlk(job->mu);
        if (!job->done) {
            if (us < 0) {
                job->cv.wait(jlk, [&] { return job->done; });
            } else if (us > 0) {
                job->cv.wait_for(jlk, std::chrono::microseconds(us), [&] { return job->done; });
            }
        }
        if (!job->done) {
            return 0;  // Still running
        }
    }

    std::thread done_thread;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_streams.find(stream_id);
        if (it != g_streams.end() && it->second->async_job == job) {
            done_thread = std::move(it->second->async_thread);
            CommitFinishedAsyncLocked(*it->second, job);
        }
    }
    if (done_thread.joinable()) {
        done_thread.join();
    }
    return 1;
}

void BinkRegisterFrameBuffers(BINK* bnk, BINKFRAMEBUFFERS* set) {
    if (!bnk || !set) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    auto& s = *it->second;
    s.has_frame_buffers = true;
    s.registered_fb = *set;
    s.registered_fb_ptr = set;
}

void BinkGetFrameBuffersInfo(BINK* bnk, BINKFRAMEBUFFERS* set) {
    if (!bnk || !set) return;
    std::memset(set, 0, sizeof(*set));
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    const auto& s = *it->second;

    const uint32_t ya_w = AlignUp32(s.pub.Width, 16u);
    const uint32_t ya_h = AlignUp32(s.pub.Height, 16u);
    const uint32_t c_w = AlignUp32((s.pub.Width + 1u) / 2u, 16u);
    const uint32_t c_h = AlignUp32((s.pub.Height + 1u) / 2u, 16u);

    set->TotalFrames = 2;
    set->YABufferWidth = ya_w;
    set->YABufferHeight = ya_h;
    set->cRcBBufferWidth = c_w;
    set->cRcBBufferHeight = c_h;
    set->FrameNum = s.has_frame_buffers ? s.registered_fb.FrameNum : 0u;
    for (int i = 0; i < 2; ++i) {
        set->Frames[i].YPitch = ya_w;
        set->Frames[i].APitch = ya_w;
        set->Frames[i].cRPitch = c_w;
        set->Frames[i].cBPitch = c_w;
    }
}

void BinkSetFrameRate(uint32_t forcerate, uint32_t forceratediv) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (forcerate > 0 && forceratediv > 0) {
        g_force_fps = forcerate;
        g_force_fps_div = forceratediv;
    } else {
        g_force_fps = 0;
        g_force_fps_div = 0;
    }
}

void BinkGetRealtime(BINK* bnk, BINKREALTIME* run, uint32_t frames) {
    if (!bnk || !run) return;
    std::memset(run, 0, sizeof(*run));
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    const auto& s = *it->second;

    const uint32_t window = frames > 0 ? std::min(frames, std::max(1u, s.played_frames)) : std::max(1u, s.played_frames);
    const uint64_t elapsed_ms = static_cast<uint64_t>(
        std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - s.open_tp).count()));
    const uint32_t fps = std::max(1u, s.pub.FrameRate);
    const uint32_t fps_div = std::max(1u, s.pub.FrameRateDiv);

    run->FrameNum = s.pub.FrameNum;
    run->FrameRate = s.pub.FrameRate;
    run->FrameRateDiv = s.pub.FrameRateDiv;
    run->Frames = window;
    run->FramesTime = static_cast<uint32_t>(elapsed_ms);
    run->FramesVideoDecompTime = static_cast<uint32_t>(
        s.total_video_decomp_us > 0 ? std::max<uint64_t>(1ull, s.total_video_decomp_us / 1000ull) : 0ull);
    run->FramesAudioDecompTime = 0;
    run->FramesReadTime = 0;
    run->FramesIdleReadTime = 0;
    run->FramesThreadReadTime = 0;
    run->FramesBlitTime = static_cast<uint32_t>(
        s.total_blit_us > 0 ? std::max<uint64_t>(1ull, s.total_blit_us / 1000ull) : 0ull);
    run->ReadBufferSize = s.read_buffer_size;
    run->ReadBufferUsed = s.read_buffer_used;
    const uint64_t raw_frame_bytes = static_cast<uint64_t>(s.pub.Width) * s.pub.Height * 3ull / 2ull;
    run->FramesDataRate = static_cast<uint32_t>((raw_frame_bytes * fps) / fps_div);
}

int32_t BinkShouldSkip(BINK* bnk) {
    if (!bnk) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return 0;
    auto& s = *it->second;
    if (s.pub.Paused) return 0;

    const uint64_t frame_interval_us =
        (1000000ull * std::max(1u, s.pub.FrameRateDiv)) / std::max(1u, s.pub.FrameRate);
    const uint64_t since_last_us = static_cast<uint64_t>(
        std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - s.last_frame_tp).count()));
    // Suggest skipping only when more than 2 frame intervals behind schedule.
    if (since_last_us > frame_interval_us * 2ull) {
        ++s.skipped_frames;
        s.last_frame_tp = SteadyClock::now();
        return 1;
    }
    return 0;
}

void BinkService(BINK* bnk) {
    if (!bnk) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(bnk->InternalId);
    if (it == g_streams.end()) return;
    auto& s = *it->second;
    ++s.service_calls;
    if (!s.bg_io_suspended && s.read_buffer_used < s.read_buffer_size) {
        const uint32_t step = std::max<uint32_t>(4096u, s.read_buffer_size / 8u);
        s.read_buffer_used = std::min(s.read_buffer_size, s.read_buffer_used + step);
    }
}

}  // extern "C"
