#define SCARLETTX_BUILDING_XCURL 1
#include "scarlettx/net/xcurl_abi.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct XCurlEasyHandle {
    std::string url;
    std::string method = "GET";
    std::string user_agent;
    std::vector<uint8_t> post_data;
    curl_write_callback write_cb = nullptr;
    void* write_data = nullptr;
    int64_t timeout_sec = 30;
    int64_t response_code = 0;
    int64_t downloaded_bytes = 0;
    std::string content_type;
};

struct XCurlMockRule {
    std::string url_prefix;
    int64_t response_code = 200;
    std::string content_type = "application/json";
    std::vector<uint8_t> body;
};

struct XCurlMultiHandle {
    std::vector<CURL*> attached;
    std::vector<CURL*> pending;
    std::deque<CURLMsg> completed;
    CURLMsg last_returned{};
};

std::mutex g_mutex;
bool g_global_inited = false;
std::unordered_map<void*, std::unique_ptr<XCurlEasyHandle>> g_handles;
std::unordered_map<void*, std::unique_ptr<XCurlMultiHandle>> g_multi_handles;
std::vector<XCurlMockRule> g_mocks;

bool IsUnreservedUriChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

int HexDigitValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

}  // namespace

extern "C" {

CURLcode curl_global_init(int64_t flags) {
    (void)flags;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_global_inited = true;
    return CURLE_OK;
}

void curl_global_cleanup(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_multi_handles.clear();
    g_handles.clear();
    g_global_inited = false;
}

CURL* curl_easy_init(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto h = std::make_unique<XCurlEasyHandle>();
    void* raw = h.get();
    g_handles[raw] = std::move(h);
    return raw;
}

CURL* curl_easy_duphandle(CURL* curl) {
    if (!curl) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_handles.find(curl);
    if (it == g_handles.end()) return nullptr;
    auto copy = std::make_unique<XCurlEasyHandle>(*it->second);
    void* raw = copy.get();
    g_handles[raw] = std::move(copy);
    return raw;
}

void curl_easy_cleanup(CURL* curl) {
    if (!curl) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_handles.erase(curl);
}

void curl_easy_reset(CURL* curl) {
    if (!curl) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_handles.find(curl);
    if (it != g_handles.end()) {
        *it->second = XCurlEasyHandle{};
    }
}

CURLcode curl_easy_setopt(CURL* curl, CURLoption option, ...) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_handles.find(curl);
    if (it == g_handles.end()) return CURLE_BAD_FUNCTION_ARGUMENT;
    auto& h = *it->second;

    va_list args;
    va_start(args, option);
    CURLcode rc = CURLE_OK;
    switch (option) {
        case CURLOPT_URL: {
            const char* s = va_arg(args, const char*);
            h.url = s ? s : "";
            break;
        }
        case CURLOPT_CUSTOMREQUEST: {
            const char* s = va_arg(args, const char*);
            if (s) h.method = s;
            break;
        }
        case CURLOPT_USERAGENT: {
            const char* s = va_arg(args, const char*);
            h.user_agent = s ? s : "";
            break;
        }
        case CURLOPT_POST: {
            const long v = va_arg(args, long);
            if (v) h.method = "POST";
            break;
        }
        case CURLOPT_POSTFIELDS: {
            const char* p = va_arg(args, const char*);
            if (p) {
                const size_t len = std::strlen(p);
                h.post_data.assign(reinterpret_cast<const uint8_t*>(p),
                                   reinterpret_cast<const uint8_t*>(p) + len);
            }
            break;
        }
        case CURLOPT_POSTFIELDSIZE: {
            (void)va_arg(args, long);
            break;
        }
        case CURLOPT_WRITEFUNCTION: {
            h.write_cb = va_arg(args, curl_write_callback);
            break;
        }
        case CURLOPT_WRITEDATA: {
            h.write_data = va_arg(args, void*);
            break;
        }
        case CURLOPT_HTTPHEADER: {
            (void)va_arg(args, curl_slist*);
            break;
        }
        case CURLOPT_TIMEOUT: {
            h.timeout_sec = va_arg(args, long);
            break;
        }
        case CURLOPT_FOLLOWLOCATION:
        case CURLOPT_SSL_VERIFYPEER: {
            (void)va_arg(args, long);
            break;
        }
        default:
            break;
    }
    va_end(args);
    return rc;
}

CURLcode curl_easy_perform(CURL* curl) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;
    curl_write_callback cb = nullptr;
    void* ud = nullptr;
    std::vector<uint8_t> body;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handles.find(curl);
        if (it == g_handles.end()) return CURLE_BAD_FUNCTION_ARGUMENT;
        auto& h = *it->second;
        if (h.url.empty()) return CURLE_URL_MALFORMAT;

        bool matched = false;
        for (const auto& rule : g_mocks) {
            if (h.url.rfind(rule.url_prefix, 0) == 0) {
                h.response_code = rule.response_code;
                h.content_type = rule.content_type;
                body = rule.body;
                matched = true;
                break;
            }
        }
        if (!matched) {
            h.response_code = 503;
            h.content_type = "application/json";
            const std::string fallback = "{\"status\":\"offline\",\"emulator\":\"ScarlettX-XCurl\"}";
            body.assign(fallback.begin(), fallback.end());
        }
        h.downloaded_bytes = static_cast<int64_t>(body.size());
        cb = h.write_cb;
        ud = h.write_data;
    }

    if (cb && !body.empty()) {
        const size_t written = cb(reinterpret_cast<char*>(body.data()), 1, body.size(), ud);
        if (written != body.size()) {
            return CURLE_WRITE_ERROR;
        }
    }
    return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL* curl, CURLINFO info, ...) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_handles.find(curl);
    if (it == g_handles.end()) return CURLE_BAD_FUNCTION_ARGUMENT;
    const auto& h = *it->second;

    va_list args;
    va_start(args, info);
    CURLcode rc = CURLE_OK;
    switch (info) {
        case CURLINFO_RESPONSE_CODE: {
            long* out = va_arg(args, long*);
            if (out) *out = static_cast<long>(h.response_code);
            break;
        }
        case CURLINFO_SIZE_DOWNLOAD_T: {
            int64_t* out = va_arg(args, int64_t*);
            if (out) *out = h.downloaded_bytes;
            break;
        }
        case CURLINFO_CONTENT_TYPE: {
            const char** out = va_arg(args, const char**);
            if (out) *out = h.content_type.c_str();
            break;
        }
        default:
            rc = CURLE_BAD_FUNCTION_ARGUMENT;
            break;
    }
    va_end(args);
    return rc;
}

const char* curl_easy_strerror(CURLcode code) {
    switch (code) {
        case CURLE_OK: return "No error";
        case CURLE_UNSUPPORTED_PROTOCOL: return "Unsupported protocol";
        case CURLE_URL_MALFORMAT: return "URL using bad/illegal format";
        case CURLE_COULDNT_CONNECT: return "Couldn't connect to server";
        case CURLE_WRITE_ERROR: return "Failed writing received data";
        case CURLE_OPERATION_TIMEDOUT: return "Timeout was reached";
        case CURLE_BAD_FUNCTION_ARGUMENT: return "Bad function argument";
        default: return "Unknown error";
    }
}

curl_slist* curl_slist_append(curl_slist* list, const char* string) {
    if (!string) return list;
    auto* node = static_cast<curl_slist*>(std::malloc(sizeof(curl_slist)));
    if (!node) return nullptr;
    const size_t len = std::strlen(string);
    node->data = static_cast<char*>(std::malloc(len + 1));
    if (!node->data) {
        std::free(node);
        return nullptr;
    }
    std::memcpy(node->data, string, len + 1);
    node->next = nullptr;
    if (!list) return node;
    curl_slist* tail = list;
    while (tail->next) tail = tail->next;
    tail->next = node;
    return list;
}

void curl_slist_free_all(curl_slist* list) {
    while (list) {
        curl_slist* next = list->next;
        std::free(list->data);
        std::free(list);
        list = next;
    }
}

const char* curl_version(void) {
    return "ScarlettX-XCurl/8.0.0-HLE";
}

char* curl_easy_escape(CURL* /*curl*/, const char* string, int32_t length) {
    if (!string) return nullptr;
    const size_t in_len = (length > 0) ? static_cast<size_t>(length) : std::strlen(string);
    std::string out;
    out.reserve(in_len * 3u);
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < in_len; ++i) {
        const unsigned char c = static_cast<unsigned char>(string[i]);
        if (IsUnreservedUriChar(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0Fu]);
            out.push_back(kHex[c & 0x0Fu]);
        }
    }
    char* buf = static_cast<char*>(std::malloc(out.size() + 1u));
    if (!buf) return nullptr;
    std::memcpy(buf, out.data(), out.size());
    buf[out.size()] = '\0';
    return buf;
}

char* curl_easy_unescape(CURL* /*curl*/, const char* string, int32_t length, int32_t* outlength) {
    if (!string) return nullptr;
    const size_t in_len = (length > 0) ? static_cast<size_t>(length) : std::strlen(string);
    std::string out;
    out.reserve(in_len);
    for (size_t i = 0; i < in_len; ++i) {
        if (string[i] == '%' && i + 2u < in_len) {
            const int hi = HexDigitValue(string[i + 1u]);
            const int lo = HexDigitValue(string[i + 2u]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2u;
                continue;
            }
        }
        out.push_back(string[i]);
    }
    if (outlength) {
        *outlength = static_cast<int32_t>(out.size());
    }
    char* buf = static_cast<char*>(std::malloc(out.size() + 1u));
    if (!buf) return nullptr;
    std::memcpy(buf, out.data(), out.size());
    buf[out.size()] = '\0';
    return buf;
}

void curl_free(void* p) {
    std::free(p);
}

CURLM* curl_multi_init(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto mh = std::make_unique<XCurlMultiHandle>();
    void* raw = mh.get();
    g_multi_handles[raw] = std::move(mh);
    return raw;
}

CURLMcode curl_multi_cleanup(CURLM* multi_handle) {
    if (!multi_handle) return CURLM_BAD_HANDLE;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_multi_handles.erase(multi_handle) > 0 ? CURLM_OK : CURLM_BAD_HANDLE;
}

CURLMcode curl_multi_add_handle(CURLM* multi_handle, CURL* easy_handle) {
    if (!multi_handle) return CURLM_BAD_HANDLE;
    if (!easy_handle) return CURLM_BAD_EASY_HANDLE;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto mit = g_multi_handles.find(multi_handle);
    if (mit == g_multi_handles.end()) return CURLM_BAD_HANDLE;
    if (g_handles.find(easy_handle) == g_handles.end()) return CURLM_BAD_EASY_HANDLE;
    auto& mh = *mit->second;
    if (std::find(mh.attached.begin(), mh.attached.end(), easy_handle) != mh.attached.end()) {
        return CURLM_ADDED_ALREADY;
    }
    mh.attached.push_back(easy_handle);
    mh.pending.push_back(easy_handle);
    return CURLM_OK;
}

CURLMcode curl_multi_remove_handle(CURLM* multi_handle, CURL* easy_handle) {
    if (!multi_handle) return CURLM_BAD_HANDLE;
    if (!easy_handle) return CURLM_BAD_EASY_HANDLE;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto mit = g_multi_handles.find(multi_handle);
    if (mit == g_multi_handles.end()) return CURLM_BAD_HANDLE;
    auto& mh = *mit->second;
    auto it = std::find(mh.attached.begin(), mh.attached.end(), easy_handle);
    if (it == mh.attached.end()) return CURLM_BAD_EASY_HANDLE;
    mh.attached.erase(it);
    mh.pending.erase(std::remove(mh.pending.begin(), mh.pending.end(), easy_handle), mh.pending.end());
    return CURLM_OK;
}

CURLMcode curl_multi_perform(CURLM* multi_handle, int32_t* running_handles) {
    if (!multi_handle) return CURLM_BAD_HANDLE;
    std::vector<CURL*> to_run;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto mit = g_multi_handles.find(multi_handle);
        if (mit == g_multi_handles.end()) return CURLM_BAD_HANDLE;
        to_run.swap(mit->second->pending);
    }

    std::vector<CURLMsg> finished;
    finished.reserve(to_run.size());
    for (CURL* easy : to_run) {
        CURLMsg msg{};
        msg.msg = CURLMSG_DONE;
        msg.easy_handle = easy;
        msg.data.result = curl_easy_perform(easy);
        finished.push_back(msg);
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto mit = g_multi_handles.find(multi_handle);
        if (mit == g_multi_handles.end()) return CURLM_BAD_HANDLE;
        for (const auto& m : finished) {
            mit->second->completed.push_back(m);
        }
        if (running_handles) {
            *running_handles = static_cast<int32_t>(mit->second->pending.size());
        }
    }
    return CURLM_OK;
}

CURLMsg* curl_multi_info_read(CURLM* multi_handle, int32_t* msgs_in_queue) {
    if (msgs_in_queue) *msgs_in_queue = 0;
    if (!multi_handle) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto mit = g_multi_handles.find(multi_handle);
    if (mit == g_multi_handles.end() || mit->second->completed.empty()) {
        return nullptr;
    }
    auto& mh = *mit->second;
    mh.last_returned = mh.completed.front();
    mh.completed.pop_front();
    if (msgs_in_queue) {
        *msgs_in_queue = static_cast<int32_t>(mh.completed.size());
    }
    return &mh.last_returned;
}

CURLcode xcurl_register_mock_response(
    const char* url_prefix,
    int64_t response_code,
    const char* content_type,
    const void* body,
    size_t body_size) {
    if (!url_prefix) return CURLE_BAD_FUNCTION_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_mutex);
    XCurlMockRule rule{};
    rule.url_prefix = url_prefix;
    rule.response_code = response_code;
    rule.content_type = content_type ? content_type : "application/octet-stream";
    if (body && body_size > 0) {
        const auto* b = static_cast<const uint8_t*>(body);
        rule.body.assign(b, b + body_size);
    }
    g_mocks.push_back(std::move(rule));
    return CURLE_OK;
}

void xcurl_clear_mocks(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mocks.clear();
}

}  // extern "C"
