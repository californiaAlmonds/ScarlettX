#include "scarlettx/loader/xvd_container.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace scarlettx {

namespace {

uint32_t ReadU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64LE(const uint8_t* p) {
    return static_cast<uint64_t>(ReadU32LE(p)) |
           (static_cast<uint64_t>(ReadU32LE(p + 4)) << 32);
}

void WriteU32LE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

void WriteU64LE(uint8_t* p, uint64_t v) {
    WriteU32LE(p, static_cast<uint32_t>(v & 0xFFFFFFFFull));
    WriteU32LE(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFull));
}

// Public XVD header field offsets (relative to start of file; magic at 0x200).
constexpr size_t kOffVolumeFlags = 0x208;
constexpr size_t kOffFormatVersion = 0x20C;
constexpr size_t kOffDriveSize = 0x218;
constexpr size_t kOffContentId = 0x220;
constexpr size_t kOffContentType = 0x240;
constexpr size_t kOffEmbeddedXvdLen = 0x244;
constexpr size_t kOffUserDataOffset = 0x250;
constexpr size_t kOffUserDataLength = 0x258;

}  // namespace

Status XvdContainer::ParseHeaderFromBytes(const uint8_t* data, size_t size, uint64_t backing_size) {
    header_ = {};
    if (!data || size < kMinHeaderSize) {
        return Status::Error(StatusCode::UnsupportedFormat, "XVD image smaller than 4 KiB header");
    }

    if (std::memcmp(data + kMagicOffset, kMagic, 8) != 0) {
        return Status::Error(StatusCode::UnsupportedFormat, "Missing 'msft-xvd' magic at offset 0x200");
    }

    header_.magic.assign(reinterpret_cast<const char*>(data + kMagicOffset), 8);
    header_.volume_flags = ReadU32LE(data + kOffVolumeFlags);
    header_.format_version = ReadU32LE(data + kOffFormatVersion);
    header_.drive_size = ReadU64LE(data + kOffDriveSize);
    std::memcpy(header_.content_id.data(), data + kOffContentId, 16);
    header_.content_type = ReadU32LE(data + kOffContentType);
    header_.embedded_xvd_length = ReadU32LE(data + kOffEmbeddedXvdLen);
    header_.user_data_offset = ReadU64LE(data + kOffUserDataOffset);
    header_.user_data_length = ReadU64LE(data + kOffUserDataLength);

    if (header_.user_data_offset < kMinHeaderSize) {
        header_.user_data_offset = kMinHeaderSize;
    }
    if (header_.user_data_length == 0 && header_.drive_size > header_.user_data_offset) {
        header_.user_data_length = header_.drive_size - header_.user_data_offset;
    }
    if (header_.user_data_offset > backing_size ||
        header_.user_data_length > backing_size - header_.user_data_offset) {
        header_ = {};
        return Status::Error(StatusCode::UnsupportedFormat, "XVD user-data range exceeds image size");
    }

    header_.is_encrypted = (header_.volume_flags & kFlagEncryptionDisabled) == 0;
    header_.has_data_integrity = (header_.volume_flags & kFlagDataIntegrityDisabled) == 0;
    header_.valid = true;
    return Status::Ok();
}

Status XvdContainer::OpenFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::NotFound, "Failed to open XVD file: " + path);
    }

    std::vector<uint8_t> hdr(kMinHeaderSize);
    ifs.read(reinterpret_cast<char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
    if (static_cast<size_t>(ifs.gcount()) < kMinHeaderSize) {
        return Status::Error(StatusCode::UnsupportedFormat, "Truncated XVD header: " + path);
    }
    ifs.seekg(0, std::ios::end);
    const auto end_pos = ifs.tellg();
    if (end_pos < 0) {
        return Status::Error(StatusCode::IoError, "Failed to query XVD file size: " + path);
    }

    Status st = ParseHeaderFromBytes(hdr.data(), hdr.size(), static_cast<uint64_t>(end_pos));
    if (!st.ok()) {
        return st;
    }

    path_ = path;
    backing_bytes_.clear();
    memory_backed_ = false;
    return Status::Ok();
}

Status XvdContainer::OpenBytes(std::vector<uint8_t> data) {
    Status st = ParseHeaderFromBytes(data.data(), data.size(), data.size());
    if (!st.ok()) {
        return st;
    }
    path_.clear();
    backing_bytes_ = std::move(data);
    memory_backed_ = true;
    return Status::Ok();
}

Status XvdContainer::ReadUserData(
    uint64_t offset,
    uint64_t size,
    std::vector<uint8_t>& out_data) const {
    if (!header_.valid) {
        return Status::Error(StatusCode::InvalidArgument, "XVD container is not open");
    }
    if (header_.is_encrypted) {
        return Status::Error(
            StatusCode::NotImplemented,
            "Encrypted retail XVD images are not supported; supply an unencrypted Dev Mode or synthetic XVD");
    }
    if (offset > header_.user_data_length || size > header_.user_data_length - offset) {
        return Status::Error(StatusCode::InvalidArgument, "XVD read out of user-data bounds");
    }

    const uint64_t abs_offset = header_.user_data_offset + offset;
    out_data.resize(static_cast<size_t>(size));
    if (size == 0) {
        return Status::Ok();
    }

    if (memory_backed_) {
        if (abs_offset > backing_bytes_.size() || size > backing_bytes_.size() - abs_offset) {
            return Status::Error(StatusCode::IoError, "In-memory XVD buffer truncated");
        }
        std::memcpy(out_data.data(), backing_bytes_.data() + abs_offset, static_cast<size_t>(size));
        return Status::Ok();
    }

    std::ifstream ifs(path_, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::IoError, "Failed to reopen XVD file: " + path_);
    }
    ifs.seekg(static_cast<std::streamoff>(abs_offset), std::ios::beg);
    ifs.read(reinterpret_cast<char*>(out_data.data()), static_cast<std::streamsize>(size));
    if (static_cast<uint64_t>(ifs.gcount()) != size) {
        return Status::Error(StatusCode::IoError, "Short read from XVD file: " + path_);
    }
    return Status::Ok();
}

std::vector<uint8_t> XvdContainer::BuildSyntheticXvd(
    const std::vector<uint8_t>& user_data,
    uint32_t content_type,
    bool mark_encrypted) {
    const uint64_t total_size = kMinHeaderSize + user_data.size();
    std::vector<uint8_t> img(static_cast<size_t>(total_size), 0);

    std::memcpy(img.data() + kMagicOffset, kMagic, 8);
    uint32_t flags = kFlagReadOnly | kFlagDataIntegrityDisabled;
    if (!mark_encrypted) {
        flags |= kFlagEncryptionDisabled;
    }
    WriteU32LE(img.data() + kOffVolumeFlags, flags);
    WriteU32LE(img.data() + kOffFormatVersion, 3);
    WriteU64LE(img.data() + kOffDriveSize, total_size);
    for (size_t i = 0; i < 16; ++i) {
        img[kOffContentId + i] = static_cast<uint8_t>(0xA0u + i);
    }
    WriteU32LE(img.data() + kOffContentType, content_type);
    WriteU32LE(img.data() + kOffEmbeddedXvdLen, 0);
    WriteU64LE(img.data() + kOffUserDataOffset, kMinHeaderSize);
    WriteU64LE(img.data() + kOffUserDataLength, user_data.size());

    if (!user_data.empty()) {
        std::memcpy(img.data() + kMinHeaderSize, user_data.data(), user_data.size());
    }
    return img;
}

}  // namespace scarlettx
