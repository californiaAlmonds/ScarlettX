#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "scarlettx/io/dstorage_abi.h"
#include "scarlettx/loader/package_vfs.h"
#include "scarlettx/loader/xvd_container.h"

#define SX_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "[test_dstorage_xvd] FAILED: " #cond                   \
                      << " at line " << __LINE__ << "\n";                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#define SX_CHECK_EQ(a, b)                                                       \
    do {                                                                        \
        const auto _a = (a);                                                    \
        const auto _b = (b);                                                    \
        if (!(_a == _b)) {                                                      \
            std::cerr << "[test_dstorage_xvd] FAILED: " #a " == " #b            \
                      << " (" << _a << " != " << _b << ") at line " << __LINE__ \
                      << "\n";                                                  \
            return 1;                                                           \
        }                                                                       \
    } while (0)

namespace {

void OnSignal(uint64_t value, void* ctx) {
    auto* flag = static_cast<std::atomic<uint64_t>*>(ctx);
    flag->store(value, std::memory_order_release);
}

}  // namespace

int main() {
    const std::filesystem::path tmp_dir =
        std::filesystem::path(SCARLETTX_BINARY_DIR) / "test_dstorage_tmp";
    std::filesystem::create_directories(tmp_dir);

    // 1. Synthetic XVD parsing & encrypted rejection
    {
        std::vector<uint8_t> payload(2048);
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFFu);
        }

        auto unenc_bytes = scarlettx::XvdContainer::BuildSyntheticXvd(payload, 1, false);
        scarlettx::XvdContainer xvd;
        SX_CHECK(xvd.OpenBytes(unenc_bytes).ok());
        SX_CHECK(xvd.IsOpen());
        SX_CHECK_EQ(xvd.Header().magic, std::string("msft-xvd"));
        SX_CHECK(!xvd.Header().is_encrypted);
        SX_CHECK_EQ(xvd.Header().user_data_length, payload.size());

        std::vector<uint8_t> slice;
        SX_CHECK(xvd.ReadUserData(128, 256, slice).ok());
        SX_CHECK_EQ(slice.size(), 256u);
        SX_CHECK(std::memcmp(slice.data(), payload.data() + 128, 256) == 0);

        // Encrypted XVD must parse header cleanly but reject user-data reads
        auto enc_bytes = scarlettx::XvdContainer::BuildSyntheticXvd(payload, 1, true);
        scarlettx::XvdContainer enc_xvd;
        SX_CHECK(enc_xvd.OpenBytes(enc_bytes).ok());
        SX_CHECK(enc_xvd.Header().is_encrypted);
        std::vector<uint8_t> denied;
        SX_CHECK(!enc_xvd.ReadUserData(0, 64, denied).ok());

        // A user-data range that wraps or runs past the image must be rejected
        const uint64_t wrap_offset = 0xFFFFFFFFFFFFF000ull;
        const uint64_t wrap_length = 0x2000;
        auto wrap_bytes = unenc_bytes;
        std::memcpy(wrap_bytes.data() + 0x250, &wrap_offset, sizeof(wrap_offset));
        std::memcpy(wrap_bytes.data() + 0x258, &wrap_length, sizeof(wrap_length));
        scarlettx::XvdContainer wrap_xvd;
        SX_CHECK(!wrap_xvd.OpenBytes(wrap_bytes).ok());

        const uint64_t long_length = payload.size() + 1;
        auto long_bytes = unenc_bytes;
        std::memcpy(long_bytes.data() + 0x258, &long_length, sizeof(long_length));
        scarlettx::XvdContainer long_xvd;
        SX_CHECK(!long_xvd.OpenBytes(long_bytes).ok());

        const auto long_path = (tmp_dir / "long_user_data.xvd").string();
        {
            std::ofstream ofs(long_path, std::ios::binary);
            SX_CHECK(ofs.good());
            ofs.write(reinterpret_cast<const char*>(long_bytes.data()), static_cast<std::streamsize>(long_bytes.size()));
        }
        scarlettx::XvdContainer long_file_xvd;
        SX_CHECK(!long_file_xvd.OpenFile(long_path).ok());
    }

    // 2. DirectStorage configuration, factory, queue, status array
    DSTORAGE_CONFIGURATION cfg{};
    cfg.NumSubmitThreads = 1;
    cfg.NumBuiltInCpuDecompressionThreads = 1;
    SX_CHECK_EQ(DStorageSetConfiguration(&cfg), 0);

    uint64_t factory = 0;
    SX_CHECK_EQ(DStorageGetFactory(&factory), 0);
    SX_CHECK(factory != 0);

    DSTORAGE_QUEUE_DESC qdesc{};
    qdesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qdesc.Capacity = 64;
    qdesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    qdesc.Name = "ScarlettX_TestQueue";
    uint64_t queue = 0;
    SX_CHECK_EQ(DStorageCreateQueue(factory, &qdesc, &queue), 0);

    uint64_t status_arr = 0;
    SX_CHECK_EQ(DStorageCreateStatusArray(factory, 8, "TestStatus", &status_arr), 0);
    SX_CHECK_EQ(DStorageIsStatusComplete(status_arr, 0), 0u);

    // 3. DirectStorage async file read + GDeflate/LZ-RLE decompression from disk
    std::vector<uint8_t> original(4096);
    for (size_t i = 0; i < 2048; ++i) original[i] = 0x5Au;  // highly compressible run
    for (size_t i = 2048; i < 4096; ++i) original[i] = static_cast<uint8_t>(i & 0xFFu);

    std::vector<uint8_t> compressed(8192);
    uint32_t comp_size = 0;
    SX_CHECK_EQ(
        DStorageCompressBuffer(
            DSTORAGE_COMPRESSION_FORMAT_GDEFLATE,
            original.data(),
            static_cast<uint32_t>(original.size()),
            compressed.data(),
            static_cast<uint32_t>(compressed.size()),
            &comp_size),
        0);
    SX_CHECK(comp_size < original.size());

    const auto xvd_path = (tmp_dir / "assets.xvd").string();
    {
        auto xvd_img = scarlettx::XvdContainer::BuildSyntheticXvd(
            std::vector<uint8_t>(compressed.begin(), compressed.begin() + comp_size),
            1,
            false);
        std::ofstream ofs(xvd_path, std::ios::binary);
        SX_CHECK(ofs.good());
        ofs.write(reinterpret_cast<const char*>(xvd_img.data()), static_cast<std::streamsize>(xvd_img.size()));
    }

    // Verify PackageVfs::MountXvd mounts the unencrypted XVD and serves its user data
    {
        scarlettx::PackageVfs vfs;
        SX_CHECK(vfs.MountXvd("G:\\mounted_assets", xvd_path).ok());
        SX_CHECK(vfs.Exists("G:/mounted_assets"));
        std::vector<uint8_t> mounted_bytes;
        SX_CHECK(vfs.ReadFileBytes("G:\\mounted_assets", mounted_bytes).ok());
        SX_CHECK_EQ(mounted_bytes.size(), comp_size);
        SX_CHECK(std::memcmp(mounted_bytes.data(), compressed.data(), comp_size) == 0);
    }

    uint64_t xvd_file = 0;
    SX_CHECK_EQ(DStorageOpenXvdFile(factory, xvd_path.c_str(), &xvd_file), 0);
    uint64_t file_sz = 0;
    SX_CHECK_EQ(DStorageGetFileSize(xvd_file, &file_sz), 0);
    SX_CHECK_EQ(file_sz, comp_size);

    std::vector<uint8_t> dst(original.size(), 0);
    DSTORAGE_REQUEST req{};
    req.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    req.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
    req.SourceFileHandle = xvd_file;
    req.SourceFileOffset = 0;
    req.SourceSize = comp_size;
    req.DestinationMemory = dst.data();
    req.DestinationSize = static_cast<uint32_t>(dst.size());
    req.UncompressedSize = static_cast<uint32_t>(original.size());
    req.CancellationTag = 0x1001;
    req.Name = "ReadCompressedXvdAsset";

    uint64_t fence_val = 0;
    std::atomic<uint64_t> cb_val{0};
    SX_CHECK_EQ(DStorageEnqueueRequest(queue, &req), 0);
    SX_CHECK_EQ(DStorageEnqueueStatus(queue, status_arr, 0), 0);
    SX_CHECK_EQ(DStorageEnqueueSignal(queue, &fence_val, 42, OnSignal, &cb_val), 0);
    SX_CHECK_EQ(DStorageSubmit(queue), 0);
    SX_CHECK_EQ(DStorageWaitForIdle(queue, 5000), 0);

    SX_CHECK_EQ(DStorageIsStatusComplete(status_arr, 0), 1u);
    SX_CHECK_EQ(DStorageGetStatusHResult(status_arr, 0), 0);
    SX_CHECK_EQ(fence_val, 42ull);
    SX_CHECK_EQ(cb_val.load(), 42ull);
    SX_CHECK(std::memcmp(dst.data(), original.data(), original.size()) == 0);

    // 4. Cancellation by tag + error record
    std::vector<uint8_t> cancel_dst(64, 0xCC);
    DSTORAGE_REQUEST cancel_req{};
    cancel_req.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    cancel_req.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    cancel_req.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
    cancel_req.SourceMemory = original.data();
    cancel_req.SourceSize = 64;
    cancel_req.DestinationMemory = cancel_dst.data();
    cancel_req.DestinationSize = 64;
    cancel_req.CancellationTag = 0xBEEF0001ull;
    cancel_req.Name = "CancelledRead";

    SX_CHECK_EQ(DStorageEnqueueRequest(queue, &cancel_req), 0);
    SX_CHECK_EQ(DStorageCancelRequestsWithTag(queue, 0xFFFF0000ull, 0xBEEF0000ull), 0);
    SX_CHECK_EQ(DStorageEnqueueStatus(queue, status_arr, 1), 0);
    SX_CHECK_EQ(DStorageSubmit(queue), 0);
    SX_CHECK_EQ(DStorageWaitForIdle(queue, 5000), 0);

    SX_CHECK_EQ(DStorageIsStatusComplete(status_arr, 1), 1u);
    SX_CHECK(DStorageGetStatusHResult(status_arr, 1) < 0);
    SX_CHECK_EQ(cancel_dst[0], 0xCCu);

    DSTORAGE_ERROR_RECORD err{};
    SX_CHECK_EQ(DStorageRetrieveErrorRecord(queue, &err), 0);
    SX_CHECK_EQ(err.FailureCount, 1u);
    SX_CHECK(err.FirstFailureHResult < 0);
    SX_CHECK_EQ(std::string(err.FailedRequestName), std::string("CancelledRead"));

    SX_CHECK_EQ(DStorageCloseFile(xvd_file), 0);
    SX_CHECK_EQ(DStorageDestroyStatusArray(status_arr), 0);
    SX_CHECK_EQ(DStorageDestroyQueue(queue), 0);
    SX_CHECK_EQ(DStorageDestroyFactory(factory), 0);

    std::cout << "[test_dstorage_xvd] All DirectStorage + XVD HLE checks passed.\n";
    return 0;
}
