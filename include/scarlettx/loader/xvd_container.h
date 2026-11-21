#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

// Xbox Virtual Disk (XVD / XVC) header metadata.
// Retail Xbox Series X|S packages are encrypted XVDs; ScarlettX parses the
// public header layout and supports reading unencrypted XVDs (e.g., Dev Mode /
// synthetic test containers) while cleanly reporting encrypted retail images.
struct XvdHeaderInfo {
    bool valid = false;
    uint32_t format_version = 0;
    uint32_t content_type = 0;
    uint32_t volume_flags = 0;
    uint64_t drive_size = 0;
    uint64_t user_data_offset = 0;
    uint64_t user_data_length = 0;
    uint32_t embedded_xvd_length = 0;
    bool is_encrypted = false;
    bool has_data_integrity = false;
    std::array<uint8_t, 16> content_id{};
    std::string magic;
};

class XvdContainer {
public:
    static constexpr size_t kMagicOffset = 0x200;
    static constexpr size_t kMinHeaderSize = 0x1000;
    static constexpr const char* kMagic = "msft-xvd";

    // Volume flag bits (publicly documented XVD header flags).
    static constexpr uint32_t kFlagReadOnly = 1u << 0;
    static constexpr uint32_t kFlagEncryptionDisabled = 1u << 1;
    static constexpr uint32_t kFlagDataIntegrityDisabled = 1u << 2;

    Status OpenFile(const std::string& path);
    Status OpenBytes(std::vector<uint8_t> data);

    bool IsOpen() const { return header_.valid; }
    const XvdHeaderInfo& Header() const { return header_; }
    const std::string& Path() const { return path_; }

    // Reads bytes from the unencrypted user-data region of the XVD.
    Status ReadUserData(uint64_t offset, uint64_t size, std::vector<uint8_t>& out_data) const;

    // Builds an unencrypted synthetic XVD image in memory for autonomous testing.
    static std::vector<uint8_t> BuildSyntheticXvd(
        const std::vector<uint8_t>& user_data,
        uint32_t content_type = 1,
        bool mark_encrypted = false);

private:
    Status ParseHeaderFromBytes(const uint8_t* data, size_t size, uint64_t backing_size);

    std::string path_;
    std::vector<uint8_t> backing_bytes_;
    bool memory_backed_ = false;
    XvdHeaderInfo header_{};
};

}  // namespace scarlettx
