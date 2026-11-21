#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

enum class Bink2ChromaFormat : uint8_t {
    YCbCr420 = 0,
    YCbCr444 = 1,
};

enum class Bink2PixelFormat : uint8_t {
    Rgba8 = 0,
    Bgra8 = 1,
};

struct Bink2FrameIndexEntry {
    uint32_t offset = 0;
    uint32_t size = 0;
    bool is_keyframe = false;
};

struct Bink2ContainerHeader {
    char fourcc[5] = {};  // Null-terminated FourCC: "KB2i", "KB2j", "BIKi", etc.
    bool is_bink2 = false;
    uint32_t file_size_field = 0;     // File size minus 8 bytes per Bink spec
    uint32_t frame_count = 0;
    uint32_t largest_frame_size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_num = 30;
    uint32_t fps_den = 1;
    uint32_t video_flags = 0;
    uint32_t audio_track_count = 0;
    Bink2ChromaFormat chroma_format = Bink2ChromaFormat::YCbCr420;
    std::vector<Bink2FrameIndexEntry> frame_index;
};

struct Bink2DecodedFrame {
    uint32_t frame_index = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool is_keyframe = false;
    Bink2PixelFormat pixel_format = Bink2PixelFormat::Rgba8;
    std::vector<uint8_t> pixels;  // width * height * 4
};

struct Bink2EncodeParams {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_num = 30;
    uint32_t fps_den = 1;
    std::string fourcc = "KB2i";
    Bink2ChromaFormat chroma_format = Bink2ChromaFormat::YCbCr420;
    uint32_t keyframe_interval = 10;
    uint8_t quant_y = 2;
    uint8_t quant_c = 3;
    uint32_t audio_track_count = 0;
};

// 8x8 2D Forward & Inverse DCT (exact orthonormal Type-II / Type-III DCT).
void Bink2ForwardDct8x8(const float in[64], float out[64]) noexcept;
void Bink2InverseDct8x8(const float in[64], float out[64]) noexcept;

// BT.709 full-range RGB <-> YCbCr conversion helpers.
void Bink2RgbToYCbCr709(uint8_t r, uint8_t g, uint8_t b, uint8_t& y, uint8_t& cb, uint8_t& cr) noexcept;
void Bink2YCbCr709ToRgb(uint8_t y, uint8_t cb, uint8_t cr, uint8_t& r, uint8_t& g, uint8_t& b) noexcept;

// Clean-room encoder: generates a valid Bink2 (KB2i/KB2j/BIKi) container from RGBA8 frames
// using BT.709 YCbCr 4:2:0 / 4:4:4 planes, 8x8 2D DCT keyframes, and motion/residual P-frames.
Status Bink2EncodeTestStream(
    const std::vector<std::vector<uint8_t>>& rgba_frames,
    const Bink2EncodeParams& params,
    std::vector<uint8_t>& out_container);

class Bink2Decoder {
public:
    Bink2Decoder() = default;

    // Parses container header and frame index table from an in-memory Bink/Bink2 byte buffer.
    Status OpenFromMemory(const uint8_t* data, size_t size);
    Status OpenFromMemory(std::vector<uint8_t> data);
    void Close();

    bool IsOpen() const noexcept { return is_open_; }
    const Bink2ContainerHeader& Header() const noexcept { return header_; }
    uint32_t CurrentFrameIndex() const noexcept { return current_frame_; }

    // Decodes the current frame (or next sequential frame) into RGBA8 or BGRA8.
    Status DecodeFrame(
        uint32_t frame_index,
        Bink2PixelFormat pixel_format,
        Bink2DecodedFrame& out_frame);

    // Seeks to `frame_index` by locating the nearest preceding keyframe and decoding forward.
    Status SeekToFrame(
        uint32_t frame_index,
        Bink2PixelFormat pixel_format,
        Bink2DecodedFrame& out_frame);

private:
    Status DecodeSingleFrameInternal(uint32_t frame_index);
    void ConvertPlanesToPixels(Bink2PixelFormat pixel_format, std::vector<uint8_t>& out_pixels) const;

    bool is_open_ = false;
    std::vector<uint8_t> container_;
    Bink2ContainerHeader header_{};

    uint32_t plane_w_y_ = 0;
    uint32_t plane_h_y_ = 0;
    uint32_t plane_w_c_ = 0;
    uint32_t plane_h_c_ = 0;

    std::vector<uint8_t> plane_y_;
    std::vector<uint8_t> plane_cb_;
    std::vector<uint8_t> plane_cr_;

    std::vector<uint8_t> ref_y_;
    std::vector<uint8_t> ref_cb_;
    std::vector<uint8_t> ref_cr_;

    uint32_t current_frame_ = 0;
    bool has_decoded_frame_ = false;
    uint32_t last_decoded_frame_ = 0;
};

}  // namespace scarlettx
