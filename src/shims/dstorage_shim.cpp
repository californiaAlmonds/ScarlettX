#include "scarlettx/io/dstorage_abi.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "scarlettx/io/gdeflate.h"
#include "scarlettx/loader/xvd_container.h"

namespace {

constexpr int32_t kS_OK = 0;
constexpr int32_t kE_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t kE_FAIL = static_cast<int32_t>(0x80004005u);
constexpr int32_t kE_NOTIMPL = static_cast<int32_t>(0x80004001u);
constexpr int32_t kE_HANDLE = static_cast<int32_t>(0x80070006u);
constexpr int32_t kE_OUTOFMEMORY = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t kE_ABORT = static_cast<int32_t>(0x80004004u);
constexpr int32_t kE_DSTORAGE_FILE_NOT_FOUND = static_cast<int32_t>(0x80070002u);
constexpr int32_t kE_DSTORAGE_BUFFER_TOO_SMALL = static_cast<int32_t>(0x8007007Au);

// Simple lossless packet-RLE codec framed with 'SXCR' magic so both GDeflate-compat
// and LZ_RLE requests round-trip deterministically in headless tests.
// Packet format:
//   [0..3]  magic 'SXCR'
//   [4..7]  uncompressed_size (u32 LE)
//   stream of control bytes:
//     if (ctrl & 0x80): repeat next byte ((ctrl & 0x7F) + 3) times
//     else:             copy (ctrl + 1) literal bytes
constexpr uint8_t kCodecMagic[4] = {'S', 'X', 'C', 'R'};

int32_t CompressLzRle(
    const uint8_t* src,
    uint32_t src_size,
    uint8_t* dst,
    uint32_t dst_cap,
    uint32_t* out_size) {
    if (!out_size) return kE_INVALIDARG;
    if (src_size > 0 && !src) return kE_INVALIDARG;

    std::vector<uint8_t> tmp;
    tmp.reserve(src_size + 16);
    tmp.insert(tmp.end(), kCodecMagic, kCodecMagic + 4);
    tmp.push_back(static_cast<uint8_t>(src_size & 0xFFu));
    tmp.push_back(static_cast<uint8_t>((src_size >> 8) & 0xFFu));
    tmp.push_back(static_cast<uint8_t>((src_size >> 16) & 0xFFu));
    tmp.push_back(static_cast<uint8_t>((src_size >> 24) & 0xFFu));

    uint32_t i = 0;
    while (i < src_size) {
        // Check for run of >= 3 identical bytes
        uint32_t run = 1;
        while (i + run < src_size && src[i + run] == src[i] && run < 130) {
            ++run;
        }
        if (run >= 3) {
            tmp.push_back(static_cast<uint8_t>(0x80u | (run - 3u)));
            tmp.push_back(src[i]);
            i += run;
            continue;
        }

        // Accumulate literals until a run of >= 3 or 128 literals
        uint32_t lit_start = i;
        while (i < src_size && (i - lit_start) < 128) {
            if (i + 2 < src_size && src[i] == src[i + 1] && src[i] == src[i + 2]) {
                break;
            }
            ++i;
        }
        uint32_t lit_len = i - lit_start;
        tmp.push_back(static_cast<uint8_t>(lit_len - 1u));
        tmp.insert(tmp.end(), src + lit_start, src + lit_start + lit_len);
    }

    *out_size = static_cast<uint32_t>(tmp.size());
    if (!dst || dst_cap < tmp.size()) {
        return kE_DSTORAGE_BUFFER_TOO_SMALL;
    }
    std::memcpy(dst, tmp.data(), tmp.size());
    return kS_OK;
}

int32_t DecompressLzRle(
    const uint8_t* src,
    uint32_t src_size,
    uint8_t* dst,
    uint32_t dst_cap,
    uint32_t* out_size) {
    if (!src || src_size < 8 || !out_size) return kE_INVALIDARG;
    if (std::memcmp(src, kCodecMagic, 4) != 0) {
        return kE_FAIL;
    }
    const uint32_t expected =
        static_cast<uint32_t>(src[4]) |
        (static_cast<uint32_t>(src[5]) << 8) |
        (static_cast<uint32_t>(src[6]) << 16) |
        (static_cast<uint32_t>(src[7]) << 24);
    if (!dst || dst_cap < expected) {
        *out_size = expected;
        return kE_DSTORAGE_BUFFER_TOO_SMALL;
    }

    uint32_t in_pos = 8;
    uint32_t out_pos = 0;
    while (in_pos < src_size && out_pos < expected) {
        const uint8_t ctrl = src[in_pos++];
        if (ctrl & 0x80u) {
            if (in_pos >= src_size) return kE_FAIL;
            const uint32_t run = static_cast<uint32_t>(ctrl & 0x7Fu) + 3u;
            const uint8_t val = src[in_pos++];
            if (out_pos + run > expected) return kE_FAIL;
            std::memset(dst + out_pos, val, run);
            out_pos += run;
        } else {
            const uint32_t lit = static_cast<uint32_t>(ctrl) + 1u;
            if (in_pos + lit > src_size || out_pos + lit > expected) return kE_FAIL;
            std::memcpy(dst + out_pos, src + in_pos, lit);
            in_pos += lit;
            out_pos += lit;
        }
    }

    if (out_pos != expected) {
        return kE_FAIL;
    }
    *out_size = out_pos;
    return kS_OK;
}

struct FileObject {
    bool is_xvd = false;
    std::string path;
    uint64_t size = 0;
    scarlettx::XvdContainer xvd;
};

struct StatusArrayObject {
    std::mutex mu;
    std::string name;
    std::vector<uint8_t> completed;
    std::vector<int32_t> hresults;
};

enum class QueueItemKind {
    kRequest,
    kSignal,
    kStatus,
};

struct QueueItem {
    QueueItemKind kind = QueueItemKind::kRequest;
    DSTORAGE_REQUEST req{};
    std::string req_name;
    uint64_t* fence_ptr = nullptr;
    uint64_t fence_val = 0;
    DSTORAGE_SIGNAL_CALLBACK callback = nullptr;
    void* callback_ctx = nullptr;
    uint64_t status_array = 0;
    uint32_t status_index = 0;
    bool cancelled = false;
};

struct QueueObject {
    DSTORAGE_QUEUE_DESC desc{};
    std::string name;
    std::mutex mu;
    std::condition_variable cv;
    std::condition_variable idle_cv;
    std::deque<QueueItem> staged;
    std::deque<QueueItem> submitted;
    bool stop = false;
    bool busy = false;
    DSTORAGE_ERROR_RECORD error_record{};
    int32_t last_batch_hr = kS_OK;
    std::thread worker;
};

struct DStorageState {
    std::mutex mu;
    DSTORAGE_CONFIGURATION config{};
    uint64_t next_handle = 1;
    std::unordered_map<uint64_t, bool> factories;
    std::unordered_map<uint64_t, std::shared_ptr<FileObject>> files;
    std::unordered_map<uint64_t, std::shared_ptr<StatusArrayObject>> status_arrays;
    std::unordered_map<uint64_t, std::shared_ptr<QueueObject>> queues;
};

DStorageState& State() {
    static DStorageState s;
    return s;
}

int32_t ExecuteRequest(const DSTORAGE_REQUEST& req) {
    if (!req.DestinationMemory || req.DestinationSize == 0) {
        return kE_INVALIDARG;
    }

    std::vector<uint8_t> raw;
    if (req.SourceType == DSTORAGE_REQUEST_SOURCE_FILE) {
        std::shared_ptr<FileObject> file;
        {
            std::lock_guard<std::mutex> lk(State().mu);
            auto it = State().files.find(req.SourceFileHandle);
            if (it == State().files.end()) return kE_HANDLE;
            file = it->second;
        }
        if (req.SourceFileOffset > file->size ||
            req.SourceSize > file->size - req.SourceFileOffset) {
            return kE_INVALIDARG;
        }
        if (file->is_xvd) {
            auto st = file->xvd.ReadUserData(req.SourceFileOffset, req.SourceSize, raw);
            if (!st.ok()) return kE_FAIL;
        } else {
            raw.resize(req.SourceSize);
            std::ifstream ifs(file->path, std::ios::binary);
            if (!ifs) return kE_DSTORAGE_FILE_NOT_FOUND;
            ifs.seekg(static_cast<std::streamoff>(req.SourceFileOffset), std::ios::beg);
            ifs.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(req.SourceSize));
            if (static_cast<uint32_t>(ifs.gcount()) != req.SourceSize) return kE_FAIL;
        }
    } else if (req.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY) {
        if (!req.SourceMemory) return kE_INVALIDARG;
        raw.resize(req.SourceSize);
        if (req.SourceSize > 0) {
            std::memcpy(raw.data(), req.SourceMemory, req.SourceSize);
        }
    } else {
        return kE_INVALIDARG;
    }

    if (req.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_NONE) {
        if (raw.size() > req.DestinationSize) {
            return kE_DSTORAGE_BUFFER_TOO_SMALL;
        }
        if (!raw.empty()) {
            std::memcpy(req.DestinationMemory, raw.data(), raw.size());
        }
        return kS_OK;
    }

    if (req.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_GDEFLATE) {
        size_t out_sz = 0;
        const auto res = scarlettx::GDeflateDecompress(
            raw.data(),
            raw.size(),
            static_cast<uint8_t*>(req.DestinationMemory),
            req.DestinationSize,
            &out_sz);
        if (res == scarlettx::GDeflateResult::Ok) return kS_OK;
        if (res == scarlettx::GDeflateResult::BufferTooSmall) return kE_DSTORAGE_BUFFER_TOO_SMALL;
        return kE_FAIL;
    }

    if (req.CompressionFormat == DSTORAGE_COMPRESSION_FORMAT_LZ_RLE) {
        uint32_t out_sz = 0;
        return DecompressLzRle(
            raw.data(),
            static_cast<uint32_t>(raw.size()),
            static_cast<uint8_t*>(req.DestinationMemory),
            req.DestinationSize,
            &out_sz);
    }

    return kE_NOTIMPL;
}

void QueueWorkerLoop(std::shared_ptr<QueueObject> q) {
    for (;;) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lk(q->mu);
            q->cv.wait(lk, [&]() { return q->stop || !q->submitted.empty(); });
            if (q->stop && q->submitted.empty()) {
                return;
            }
            item = std::move(q->submitted.front());
            q->submitted.pop_front();
            q->busy = true;
        }

        if (item.kind == QueueItemKind::kRequest) {
            int32_t hr = item.cancelled ? kE_ABORT : ExecuteRequest(item.req);
            if (hr < 0) {
                std::lock_guard<std::mutex> lk(q->mu);
                if (q->error_record.FailureCount == 0) {
                    q->error_record.FirstFailureHResult = hr;
                    q->error_record.CommandType = 1;
                    std::memset(q->error_record.FailedRequestName, 0, sizeof(q->error_record.FailedRequestName));
                    std::strncpy(
                        q->error_record.FailedRequestName,
                        item.req_name.c_str(),
                        sizeof(q->error_record.FailedRequestName) - 1);
                }
                q->error_record.FailureCount++;
                q->last_batch_hr = hr;
            }
        } else if (item.kind == QueueItemKind::kSignal) {
            if (item.fence_ptr) {
                *item.fence_ptr = item.fence_val;
            }
            if (item.callback) {
                item.callback(item.fence_val, item.callback_ctx);
            }
        } else if (item.kind == QueueItemKind::kStatus) {
            std::shared_ptr<StatusArrayObject> arr;
            {
                std::lock_guard<std::mutex> lk(State().mu);
                auto it = State().status_arrays.find(item.status_array);
                if (it != State().status_arrays.end()) arr = it->second;
            }
            int32_t batch_hr = kS_OK;
            {
                std::lock_guard<std::mutex> lk(q->mu);
                batch_hr = q->last_batch_hr;
                q->last_batch_hr = kS_OK;
            }
            if (arr) {
                std::lock_guard<std::mutex> lk(arr->mu);
                if (item.status_index < arr->completed.size()) {
                    arr->hresults[item.status_index] = batch_hr;
                    arr->completed[item.status_index] = 1;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(q->mu);
            q->busy = false;
            if (q->submitted.empty()) {
                q->idle_cv.notify_all();
            }
        }
    }
}

}  // namespace

extern "C" {

int32_t DStorageSetConfiguration(const DSTORAGE_CONFIGURATION* config) {
    if (!config) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    State().config = *config;
    return kS_OK;
}

int32_t DStorageGetFactory(uint64_t* out_factory) {
    if (!out_factory) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    const uint64_t h = State().next_handle++;
    State().factories[h] = true;
    *out_factory = h;
    return kS_OK;
}

int32_t DStorageDestroyFactory(uint64_t factory) {
    std::lock_guard<std::mutex> lk(State().mu);
    if (State().factories.erase(factory) == 0) return kE_HANDLE;
    return kS_OK;
}

int32_t DStorageOpenFile(uint64_t factory, const char* path, uint64_t* out_file) {
    if (!path || !out_file) return kE_INVALIDARG;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        if (State().factories.find(factory) == State().factories.end()) return kE_HANDLE;
    }

    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return kE_DSTORAGE_FILE_NOT_FOUND;
    const auto end_pos = ifs.tellg();
    if (end_pos < 0) return kE_FAIL;

    auto obj = std::make_shared<FileObject>();
    obj->is_xvd = false;
    obj->path = path;
    obj->size = static_cast<uint64_t>(end_pos);

    std::lock_guard<std::mutex> lk(State().mu);
    const uint64_t h = State().next_handle++;
    State().files[h] = std::move(obj);
    *out_file = h;
    return kS_OK;
}

int32_t DStorageOpenXvdFile(uint64_t factory, const char* xvd_path, uint64_t* out_file) {
    if (!xvd_path || !out_file) return kE_INVALIDARG;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        if (State().factories.find(factory) == State().factories.end()) return kE_HANDLE;
    }

    auto obj = std::make_shared<FileObject>();
    obj->is_xvd = true;
    obj->path = xvd_path;
    auto st = obj->xvd.OpenFile(xvd_path);
    if (!st.ok()) return kE_FAIL;
    if (obj->xvd.Header().is_encrypted) return kE_NOTIMPL;
    obj->size = obj->xvd.Header().user_data_length;

    std::lock_guard<std::mutex> lk(State().mu);
    const uint64_t h = State().next_handle++;
    State().files[h] = std::move(obj);
    *out_file = h;
    return kS_OK;
}

int32_t DStorageCloseFile(uint64_t file) {
    std::lock_guard<std::mutex> lk(State().mu);
    return State().files.erase(file) ? kS_OK : kE_HANDLE;
}

int32_t DStorageGetFileSize(uint64_t file, uint64_t* out_size) {
    if (!out_size) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lk(State().mu);
    auto it = State().files.find(file);
    if (it == State().files.end()) return kE_HANDLE;
    *out_size = it->second->size;
    return kS_OK;
}

int32_t DStorageCreateQueue(
    uint64_t factory,
    const DSTORAGE_QUEUE_DESC* desc,
    uint64_t* out_queue) {
    if (!desc || !out_queue) return kE_INVALIDARG;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        if (State().factories.find(factory) == State().factories.end()) return kE_HANDLE;
    }

    auto q = std::make_shared<QueueObject>();
    q->desc = *desc;
    if (desc->Name) q->name = desc->Name;
    q->worker = std::thread(QueueWorkerLoop, q);

    std::lock_guard<std::mutex> lk(State().mu);
    const uint64_t h = State().next_handle++;
    State().queues[h] = std::move(q);
    *out_queue = h;
    return kS_OK;
}

int32_t DStorageDestroyQueue(uint64_t queue) {
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        q = std::move(it->second);
        State().queues.erase(it);
    }
    {
        std::lock_guard<std::mutex> lk(q->mu);
        q->stop = true;
    }
    q->cv.notify_all();
    if (q->worker.joinable()) {
        q->worker.join();
    }
    return kS_OK;
}

int32_t DStorageEnqueueRequest(uint64_t queue, const DSTORAGE_REQUEST* request) {
    if (!request) return kE_INVALIDARG;
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        q = it->second;
    }
    QueueItem item;
    item.kind = QueueItemKind::kRequest;
    item.req = *request;
    if (request->Name) item.req_name = request->Name;
    std::lock_guard<std::mutex> lk(q->mu);
    q->staged.push_back(std::move(item));
    return kS_OK;
}

int32_t DStorageEnqueueSignal(
    uint64_t queue,
    uint64_t* fence_value_ptr,
    uint64_t value,
    DSTORAGE_SIGNAL_CALLBACK callback,
    void* context) {
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        q = it->second;
    }
    QueueItem item;
    item.kind = QueueItemKind::kSignal;
    item.fence_ptr = fence_value_ptr;
    item.fence_val = value;
    item.callback = callback;
    item.callback_ctx = context;
    std::lock_guard<std::mutex> lk(q->mu);
    q->staged.push_back(std::move(item));
    return kS_OK;
}

int32_t DStorageEnqueueStatus(uint64_t queue, uint64_t status_array, uint32_t index) {
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        if (State().status_arrays.find(status_array) == State().status_arrays.end()) return kE_HANDLE;
        q = it->second;
    }
    QueueItem item;
    item.kind = QueueItemKind::kStatus;
    item.status_array = status_array;
    item.status_index = index;
    std::lock_guard<std::mutex> lk(q->mu);
    q->staged.push_back(std::move(item));
    return kS_OK;
}

int32_t DStorageSubmit(uint64_t queue) {
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        q = it->second;
    }
    {
        std::lock_guard<std::mutex> lk(q->mu);
        while (!q->staged.empty()) {
            q->submitted.push_back(std::move(q->staged.front()));
            q->staged.pop_front();
        }
    }
    q->cv.notify_one();
    return kS_OK;
}

int32_t DStorageWaitForIdle(uint64_t queue, uint32_t timeout_ms) {
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        q = it->second;
    }
    std::unique_lock<std::mutex> lk(q->mu);
    auto pred = [&]() { return !q->busy && q->submitted.empty(); };
    if (timeout_ms == 0xFFFFFFFFu) {
        q->idle_cv.wait(lk, pred);
        return kS_OK;
    }
    const bool done = q->idle_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), pred);
    return done ? kS_OK : kE_FAIL;
}

int32_t DStorageCancelRequestsWithTag(uint64_t queue, uint64_t mask, uint64_t value) {
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        q = it->second;
    }
    std::lock_guard<std::mutex> lk(q->mu);
    for (auto& item : q->staged) {
        if (item.kind == QueueItemKind::kRequest && (item.req.CancellationTag & mask) == value) {
            item.cancelled = true;
        }
    }
    for (auto& item : q->submitted) {
        if (item.kind == QueueItemKind::kRequest && (item.req.CancellationTag & mask) == value) {
            item.cancelled = true;
        }
    }
    return kS_OK;
}

int32_t DStorageCreateStatusArray(
    uint64_t factory,
    uint32_t capacity,
    const char* name,
    uint64_t* out_status_array) {
    if (capacity == 0 || !out_status_array) return kE_INVALIDARG;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        if (State().factories.find(factory) == State().factories.end()) return kE_HANDLE;
    }
    auto arr = std::make_shared<StatusArrayObject>();
    if (name) arr->name = name;
    arr->completed.assign(capacity, 0);
    arr->hresults.assign(capacity, kS_OK);

    std::lock_guard<std::mutex> lk(State().mu);
    const uint64_t h = State().next_handle++;
    State().status_arrays[h] = std::move(arr);
    *out_status_array = h;
    return kS_OK;
}

int32_t DStorageDestroyStatusArray(uint64_t status_array) {
    std::lock_guard<std::mutex> lk(State().mu);
    return State().status_arrays.erase(status_array) ? kS_OK : kE_HANDLE;
}

uint32_t DStorageIsStatusComplete(uint64_t status_array, uint32_t index) {
    std::shared_ptr<StatusArrayObject> arr;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().status_arrays.find(status_array);
        if (it == State().status_arrays.end()) return 0;
        arr = it->second;
    }
    std::lock_guard<std::mutex> lk(arr->mu);
    if (index >= arr->completed.size()) return 0;
    return arr->completed[index] ? 1u : 0u;
}

int32_t DStorageGetStatusHResult(uint64_t status_array, uint32_t index) {
    std::shared_ptr<StatusArrayObject> arr;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().status_arrays.find(status_array);
        if (it == State().status_arrays.end()) return kE_HANDLE;
        arr = it->second;
    }
    std::lock_guard<std::mutex> lk(arr->mu);
    if (index >= arr->hresults.size()) return kE_INVALIDARG;
    return arr->hresults[index];
}

int32_t DStorageRetrieveErrorRecord(uint64_t queue, DSTORAGE_ERROR_RECORD* out_record) {
    if (!out_record) return kE_INVALIDARG;
    std::shared_ptr<QueueObject> q;
    {
        std::lock_guard<std::mutex> lk(State().mu);
        auto it = State().queues.find(queue);
        if (it == State().queues.end()) return kE_HANDLE;
        q = it->second;
    }
    std::lock_guard<std::mutex> lk(q->mu);
    *out_record = q->error_record;
    q->error_record = {};
    return kS_OK;
}

int32_t DStorageCompressBuffer(
    uint8_t format,
    const void* src,
    uint32_t src_size,
    void* dst,
    uint32_t dst_capacity,
    uint32_t* out_compressed_size) {
    if (format == DSTORAGE_COMPRESSION_FORMAT_NONE) {
        if (!out_compressed_size) return kE_INVALIDARG;
        *out_compressed_size = src_size;
        if (!dst || dst_capacity < src_size) return kE_DSTORAGE_BUFFER_TOO_SMALL;
        if (src_size > 0 && src) std::memcpy(dst, src, src_size);
        return kS_OK;
    }
    if (format == DSTORAGE_COMPRESSION_FORMAT_GDEFLATE) {
        if (!out_compressed_size) return kE_INVALIDARG;
        if (src_size > 0 && !src) return kE_INVALIDARG;
        std::vector<uint8_t> comp;
        if (!scarlettx::GDeflateCompress(static_cast<const uint8_t*>(src), src_size, comp)) {
            return kE_FAIL;
        }
        *out_compressed_size = static_cast<uint32_t>(comp.size());
        if (!dst || dst_capacity < comp.size()) {
            return kE_DSTORAGE_BUFFER_TOO_SMALL;
        }
        if (!comp.empty()) {
            std::memcpy(dst, comp.data(), comp.size());
        }
        return kS_OK;
    }
    if (format == DSTORAGE_COMPRESSION_FORMAT_LZ_RLE) {
        return CompressLzRle(
            static_cast<const uint8_t*>(src),
            src_size,
            static_cast<uint8_t*>(dst),
            dst_capacity,
            out_compressed_size);
    }
    return kE_NOTIMPL;
}

int32_t DStorageDecompressBuffer(
    uint8_t format,
    const void* src,
    uint32_t src_size,
    void* dst,
    uint32_t dst_capacity,
    uint32_t* out_decompressed_size) {
    if (format == DSTORAGE_COMPRESSION_FORMAT_NONE) {
        if (!out_decompressed_size) return kE_INVALIDARG;
        *out_decompressed_size = src_size;
        if (!dst || dst_capacity < src_size) return kE_DSTORAGE_BUFFER_TOO_SMALL;
        if (src_size > 0 && src) std::memcpy(dst, src, src_size);
        return kS_OK;
    }
    if (format == DSTORAGE_COMPRESSION_FORMAT_GDEFLATE) {
        if (!out_decompressed_size) return kE_INVALIDARG;
        size_t out_sz = 0;
        const auto res = scarlettx::GDeflateDecompress(
            static_cast<const uint8_t*>(src),
            src_size,
            static_cast<uint8_t*>(dst),
            dst_capacity,
            &out_sz);
        *out_decompressed_size = static_cast<uint32_t>(out_sz);
        if (res == scarlettx::GDeflateResult::Ok) return kS_OK;
        if (res == scarlettx::GDeflateResult::BufferTooSmall) return kE_DSTORAGE_BUFFER_TOO_SMALL;
        if (res == scarlettx::GDeflateResult::InvalidArg) return kE_INVALIDARG;
        return kE_FAIL;
    }
    if (format == DSTORAGE_COMPRESSION_FORMAT_LZ_RLE) {
        return DecompressLzRle(
            static_cast<const uint8_t*>(src),
            src_size,
            static_cast<uint8_t*>(dst),
            dst_capacity,
            out_decompressed_size);
    }
    return kE_NOTIMPL;
}

}  // extern "C"
