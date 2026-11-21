#define SCARLETTX_BUILDING_XSAPI 1
#include "scarlettx/xsapi/xsapi_abi.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Handle Backing Types (opaque in xsapi_abi.h)
// ---------------------------------------------------------------------------
struct XblContext {
    std::atomic<int32_t> ref_count{1};
    XUserHandle user = nullptr;
    uint64_t xuid = 0;
    std::string scid;
};

struct XblAchievementsResult {
    struct EntryStorage {
        std::string id;
        std::string scid;
        std::string name;
        std::string unlocked_desc;
        std::string locked_desc;
        std::string current_progress_str;
        std::string target_progress_str;
    };

    std::atomic<int32_t> ref_count{1};
    std::vector<EntryStorage> storage;
    std::vector<XblAchievementRequirement> requirements;
    std::vector<XblAchievement> items;
    bool has_next = false;
};

struct XblPresenceRecord {
    std::atomic<int32_t> ref_count{1};
    uint64_t xuid = 0;
    XblPresenceUserState user_state = XblPresenceUserState_Offline;
    std::string title_name;
    std::string rich_presence_string;
    std::vector<XblPresenceTitleRecord> title_records;
    std::vector<XblPresenceDeviceRecord> device_records;
};

namespace {

// ---------------------------------------------------------------------------
// Internal Persistent Data Models
// ---------------------------------------------------------------------------
struct StoredAchievement {
    std::string id;
    std::string scid;
    std::string name;
    std::string unlocked_desc;
    std::string locked_desc;
    uint32_t title_id = 0;
    uint32_t percent_complete = 0;
    int64_t time_unlocked = 0;
    uint32_t gamerscore = 25;
    XblAchievementType type = XblAchievementType_Persistent;
    bool is_secret = false;
};

struct StoredStat {
    std::string name;
    std::string type;
    std::string value;
};

struct StoredPresence {
    uint64_t xuid = 0;
    XblPresenceUserState user_state = XblPresenceUserState_Online;
    uint32_t title_id = 0;
    std::string scid;
    std::string title_name = "ScarlettX Title";
    std::string rich_presence;
    int64_t last_modified = 0;
    bool title_active = false;
};

struct StoreKey {
    std::string scid;
    uint64_t xuid = 0;

    bool operator<(const StoreKey& o) const {
        if (scid != o.scid) return scid < o.scid;
        return xuid < o.xuid;
    }
};

// ---------------------------------------------------------------------------
// Global XSAPI Runtime State
// ---------------------------------------------------------------------------
struct XsapiState {
    std::mutex mutex;
    bool initialized = false;
    std::string scid;
    XTaskQueueHandle default_queue = nullptr;

    std::unordered_map<XblContext*, std::shared_ptr<XblContext>> live_contexts;
    std::unordered_map<XblAchievementsResult*, std::shared_ptr<XblAchievementsResult>> live_ach_results;
    std::unordered_map<XblPresenceRecord*, std::shared_ptr<XblPresenceRecord>> live_presence_records;

    // In-memory caches loaded from & flushed to disk under SCARLETTX_SAVE_ROOT
    std::map<StoreKey, std::map<std::string, StoredAchievement>> achievements_cache;
    std::map<StoreKey, bool> achievements_loaded;

    std::map<StoreKey, std::map<std::string, StoredStat>> stats_cache;
    std::map<StoreKey, bool> stats_loaded;

    std::unordered_map<uint64_t, StoredPresence> presence_by_xuid;
};

XsapiState& GetState() {
    static XsapiState s;
    return s;
}

// ---------------------------------------------------------------------------
// Utilities: Time, Strings, Save Root, Minimal Strict JSON Codec
// ---------------------------------------------------------------------------
int64_t NowUnixSeconds() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

std::string SanitizePathToken(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "default" : out;
}

std::string FormatXuidHex(uint64_t xuid) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(xuid));
    return std::string(buf);
}

fs::path ResolveXsapiDir(const std::string& scid) {
    const char* env_root = std::getenv("SCARLETTX_SAVE_ROOT");
    fs::path base = (env_root && *env_root)
                        ? fs::path(env_root)
                        : (fs::temp_directory_path() / "scarlettx_saves");
    return base / "xsapi" / SanitizePathToken(scid);
}

fs::path AchievementsFilePath(const std::string& scid, uint64_t xuid) {
    return ResolveXsapiDir(scid) / ("achievements_" + FormatXuidHex(xuid) + ".json");
}

fs::path StatsFilePath(const std::string& scid, uint64_t xuid) {
    return ResolveXsapiDir(scid) / ("stats_" + FormatXuidHex(xuid) + ".json");
}

uint32_t CurrentTitleId() {
    uint32_t tid = 0;
    if (XGameGetXboxTitleId(&tid) == XGR_S_OK && tid != 0) {
        return tid;
    }
    return 0x5CA00001u;
}

void CopyCString(char* dst, size_t dst_size, const std::string& src) {
    if (!dst || dst_size == 0) {
        return;
    }
    const size_t n = std::min(src.size(), dst_size - 1);
    if (n > 0) {
        std::memcpy(dst, src.data(), n);
    }
    dst[n] = '\0';
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

// Lightweight recursive-descent JSON reader for local persistence.
struct JsonValue {
    enum class Kind { Null, Bool, Int, String, Array, Object };
    Kind kind = Kind::Null;
    bool bool_val = false;
    int64_t int_val = 0;
    std::string str_val;
    std::vector<JsonValue> arr_val;
    std::map<std::string, JsonValue> obj_val;

    const JsonValue* Find(const std::string& key) const {
        if (kind != Kind::Object) return nullptr;
        auto it = obj_val.find(key);
        return it == obj_val.end() ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}

    bool Parse(JsonValue& out) {
        SkipWs();
        if (!ParseValue(out)) return false;
        SkipWs();
        return pos_ == s_.size();
    }

private:
    void SkipWs() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) {
            ++pos_;
        }
    }

    bool ParseValue(JsonValue& out) {
        SkipWs();
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '{') return ParseObject(out);
        if (c == '[') return ParseArray(out);
        if (c == '"') {
            out = JsonValue{};
            out.kind = JsonValue::Kind::String;
            return ParseString(out.str_val);
        }
        if (c == 't' && s_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out = JsonValue{};
            out.kind = JsonValue::Kind::Bool;
            out.bool_val = true;
            return true;
        }
        if (c == 'f' && s_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out = JsonValue{};
            out.kind = JsonValue::Kind::Bool;
            out.bool_val = false;
            return true;
        }
        if (c == 'n' && s_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            out = JsonValue{};
            out.kind = JsonValue::Kind::Null;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return ParseInt(out);
        }
        return false;
    }

    bool ParseString(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) return false;
                char esc = s_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > s_.size()) return false;
                        unsigned code = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = s_[pos_++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(10 + h - 'a');
                            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(10 + h - 'A');
                            else return false;
                        }
                        if (code < 0x80) {
                            out.push_back(static_cast<char>(code));
                        } else {
                            out.push_back('?');
                        }
                        break;
                    }
                    default:
                        return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool ParseInt(JsonValue& out) {
        const size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return false;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        // Skip optional fractional/exponent part if any
        if (pos_ < s_.size() && (s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E')) {
            while (pos_ < s_.size() &&
                   (std::isdigit(static_cast<unsigned char>(s_[pos_])) ||
                    s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E' ||
                    s_[pos_] == '+' || s_[pos_] == '-')) {
                ++pos_;
            }
        }
        out = JsonValue{};
        out.kind = JsonValue::Kind::Int;
        try {
            out.int_val = std::stoll(s_.substr(start, pos_ - start));
        } catch (...) {
            out.int_val = 0;
        }
        return true;
    }

    bool ParseArray(JsonValue& out) {
        ++pos_;  // '['
        out = JsonValue{};
        out.kind = JsonValue::Kind::Array;
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (pos_ < s_.size()) {
            JsonValue elem;
            if (!ParseValue(elem)) return false;
            out.arr_val.push_back(std::move(elem));
            SkipWs();
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            if (s_[pos_] != ',') return false;
            ++pos_;
        }
        return false;
    }

    bool ParseObject(JsonValue& out) {
        ++pos_;  // '{'
        out = JsonValue{};
        out.kind = JsonValue::Kind::Object;
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (pos_ < s_.size()) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return false;
            ++pos_;
            JsonValue val;
            if (!ParseValue(val)) return false;
            out.obj_val[std::move(key)] = std::move(val);
            SkipWs();
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (s_[pos_] != ',') return false;
            ++pos_;
        }
        return false;
    }

    const std::string& s_;
    size_t pos_ = 0;
};

bool WriteFileAtomically(const fs::path& path, const std::string& contents) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }
    fs::path tmp_path = path;
    tmp_path += ".tmp";
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return false;
        }
        ofs.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        ofs.flush();
        if (!ofs.good()) {
            return false;
        }
    }
    fs::rename(tmp_path, path, ec);
    if (ec) {
        // Fallback direct write if rename across filesystem failed.
        fs::remove(tmp_path, ec);
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        ofs.flush();
        return ofs.good();
    }
    return true;
}

bool ReadFileToString(const fs::path& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    out = oss.str();
    return true;
}

// ---------------------------------------------------------------------------
// Persistent Achievement & Statistic Storage (called with state.mutex held)
// ---------------------------------------------------------------------------
void EnsureAchievementsLoadedLocked(XsapiState& s, const std::string& scid, uint64_t xuid) {
    const StoreKey key{scid, xuid};
    if (s.achievements_loaded[key]) {
        return;
    }
    s.achievements_loaded[key] = true;
    auto& map = s.achievements_cache[key];

    std::string raw;
    if (!ReadFileToString(AchievementsFilePath(scid, xuid), raw)) {
        return;
    }
    JsonValue root;
    JsonParser parser(raw);
    if (!parser.Parse(root)) {
        return;
    }
    const JsonValue* arr = root.Find("achievements");
    if (!arr || arr->kind != JsonValue::Kind::Array) {
        return;
    }
    for (const auto& item : arr->arr_val) {
        if (item.kind != JsonValue::Kind::Object) continue;
        const JsonValue* id_v = item.Find("id");
        if (!id_v || id_v->kind != JsonValue::Kind::String || id_v->str_val.empty()) continue;

        StoredAchievement ach{};
        ach.id = id_v->str_val;
        ach.scid = scid;
        if (const auto* v = item.Find("scid"); v && v->kind == JsonValue::Kind::String && !v->str_val.empty()) {
            ach.scid = v->str_val;
        }
        if (const auto* v = item.Find("name"); v && v->kind == JsonValue::Kind::String) {
            ach.name = v->str_val;
        } else {
            ach.name = "Achievement " + ach.id;
        }
        if (const auto* v = item.Find("unlockedDescription"); v && v->kind == JsonValue::Kind::String) {
            ach.unlocked_desc = v->str_val;
        } else {
            ach.unlocked_desc = "Unlocked achievement " + ach.id;
        }
        if (const auto* v = item.Find("lockedDescription"); v && v->kind == JsonValue::Kind::String) {
            ach.locked_desc = v->str_val;
        } else {
            ach.locked_desc = "Complete achievement " + ach.id;
        }
        if (const auto* v = item.Find("titleId"); v && v->kind == JsonValue::Kind::Int) {
            ach.title_id = static_cast<uint32_t>(v->int_val);
        }
        if (const auto* v = item.Find("percentComplete"); v && v->kind == JsonValue::Kind::Int) {
            ach.percent_complete = static_cast<uint32_t>(std::clamp<int64_t>(v->int_val, 0, 100));
        }
        if (const auto* v = item.Find("timeUnlocked"); v && v->kind == JsonValue::Kind::Int) {
            ach.time_unlocked = v->int_val;
        }
        if (const auto* v = item.Find("gamerscore"); v && v->kind == JsonValue::Kind::Int) {
            ach.gamerscore = static_cast<uint32_t>(std::max<int64_t>(0, v->int_val));
        }
        if (const auto* v = item.Find("type"); v && v->kind == JsonValue::Kind::Int) {
            ach.type = static_cast<XblAchievementType>(v->int_val);
        }
        if (const auto* v = item.Find("isSecret"); v && v->kind == JsonValue::Kind::Bool) {
            ach.is_secret = v->bool_val;
        }
        map[ach.id] = std::move(ach);
    }
}

bool SaveAchievementsLocked(const XsapiState& s, const std::string& scid, uint64_t xuid) {
    const StoreKey key{scid, xuid};
    auto it = s.achievements_cache.find(key);
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"xuid\": " << xuid << ",\n";
    oss << "  \"scid\": " << JsonEscape(scid) << ",\n";
    oss << "  \"achievements\": [";
    if (it != s.achievements_cache.end() && !it->second.empty()) {
        oss << "\n";
        bool first = true;
        for (const auto& [id, ach] : it->second) {
            if (!first) oss << ",\n";
            first = false;
            oss << "    {\n";
            oss << "      \"id\": " << JsonEscape(ach.id) << ",\n";
            oss << "      \"scid\": " << JsonEscape(ach.scid) << ",\n";
            oss << "      \"name\": " << JsonEscape(ach.name) << ",\n";
            oss << "      \"unlockedDescription\": " << JsonEscape(ach.unlocked_desc) << ",\n";
            oss << "      \"lockedDescription\": " << JsonEscape(ach.locked_desc) << ",\n";
            oss << "      \"titleId\": " << ach.title_id << ",\n";
            oss << "      \"percentComplete\": " << ach.percent_complete << ",\n";
            oss << "      \"timeUnlocked\": " << ach.time_unlocked << ",\n";
            oss << "      \"gamerscore\": " << ach.gamerscore << ",\n";
            oss << "      \"type\": " << static_cast<uint32_t>(ach.type) << ",\n";
            oss << "      \"isSecret\": " << (ach.is_secret ? "true" : "false") << "\n";
            oss << "    }";
        }
        oss << "\n  ";
    }
    oss << "]\n}\n";
    return WriteFileAtomically(AchievementsFilePath(scid, xuid), oss.str());
}

void EnsureStatsLoadedLocked(XsapiState& s, const std::string& scid, uint64_t xuid) {
    const StoreKey key{scid, xuid};
    if (s.stats_loaded[key]) {
        return;
    }
    s.stats_loaded[key] = true;
    auto& map = s.stats_cache[key];

    std::string raw;
    if (!ReadFileToString(StatsFilePath(scid, xuid), raw)) {
        return;
    }
    JsonValue root;
    JsonParser parser(raw);
    if (!parser.Parse(root)) {
        return;
    }
    const JsonValue* arr = root.Find("statistics");
    if (!arr || arr->kind != JsonValue::Kind::Array) {
        return;
    }
    for (const auto& item : arr->arr_val) {
        if (item.kind != JsonValue::Kind::Object) continue;
        const JsonValue* name_v = item.Find("name");
        if (!name_v || name_v->kind != JsonValue::Kind::String || name_v->str_val.empty()) continue;

        StoredStat st{};
        st.name = name_v->str_val;
        if (const auto* v = item.Find("type"); v && v->kind == JsonValue::Kind::String) {
            st.type = v->str_val;
        } else {
            st.type = "String";
        }
        if (const auto* v = item.Find("value"); v && v->kind == JsonValue::Kind::String) {
            st.value = v->str_val;
        }
        map[st.name] = std::move(st);
    }
}

bool SaveStatsLocked(const XsapiState& s, const std::string& scid, uint64_t xuid) {
    const StoreKey key{scid, xuid};
    auto it = s.stats_cache.find(key);
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"xuid\": " << xuid << ",\n";
    oss << "  \"scid\": " << JsonEscape(scid) << ",\n";
    oss << "  \"statistics\": [";
    if (it != s.stats_cache.end() && !it->second.empty()) {
        oss << "\n";
        bool first = true;
        for (const auto& [name, st] : it->second) {
            if (!first) oss << ",\n";
            first = false;
            oss << "    {\n";
            oss << "      \"name\": " << JsonEscape(st.name) << ",\n";
            oss << "      \"type\": " << JsonEscape(st.type) << ",\n";
            oss << "      \"value\": " << JsonEscape(st.value) << "\n";
            oss << "    }";
        }
        oss << "\n  ";
    }
    oss << "]\n}\n";
    return WriteFileAtomically(StatsFilePath(scid, xuid), oss.str());
}

// ---------------------------------------------------------------------------
// Handle Lookup Helpers
// ---------------------------------------------------------------------------
std::shared_ptr<XblContext> LookupContext(XblContextHandle handle) {
    if (!handle) return nullptr;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.live_contexts.find(handle);
    return it == s.live_contexts.end() ? nullptr : it->second;
}

std::shared_ptr<XblAchievementsResult> LookupAchResult(XblAchievementsResultHandle handle) {
    if (!handle) return nullptr;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.live_ach_results.find(handle);
    return it == s.live_ach_results.end() ? nullptr : it->second;
}

std::shared_ptr<XblPresenceRecord> LookupPresenceRecord(XblPresenceRecordHandle handle) {
    if (!handle) return nullptr;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.live_presence_records.find(handle);
    return it == s.live_presence_records.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// XAsyncProvider Bridge (uses real xgameruntime XAsyncBegin/Schedule/Complete)
// ---------------------------------------------------------------------------
const char kIdCleanup[] = "XblCleanupAsync";
const char kIdAchGet[] = "XblAchievementsGetAchievementsForTitleIdAsync";
const char kIdAchUpdate[] = "XblAchievementsUpdateAchievementAsync";
const char kIdPresenceSet[] = "XblPresenceSetPresenceAsync";
const char kIdPresenceGet[] = "XblPresenceGetPresenceAsync";
const char kIdProfileGet[] = "XblProfileGetUserProfileAsync";
const char kIdStatsGet[] = "XblUserStatisticsGetSingleUserStatisticAsync";
const char kIdStatsWrite[] = "XblTitleManagedStatsWriteAsync";

struct XsapiAsyncOp {
    std::function<XGameResult(XsapiAsyncOp&)> work_fn;
    std::function<XGameResult(void* buffer, size_t buffer_size)> get_result_fn;
    std::function<void()> cleanup_unclaimed_fn;
    size_t required_buffer_size = 0;
    std::atomic<bool> canceled{false};
};

XGameResult SCARLETTX_GDK_CALLBACK XsapiAsyncProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<XsapiAsyncOp*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            if (ctx->canceled.load(std::memory_order_acquire)) {
                XAsyncComplete(data->async, XGR_E_ABORT, 0);
                return XGR_E_PENDING;
            }
            XGameResult hr = ctx->work_fn ? ctx->work_fn(*ctx) : XGR_E_FAIL;
            if (ctx->canceled.load(std::memory_order_acquire) && hr >= 0) {
                hr = XGR_E_ABORT;
            }
            XAsyncComplete(data->async, hr, hr >= 0 ? ctx->required_buffer_size : 0);
            return XGR_E_PENDING;
        }
        case XAsyncOp_GetResult:
            if (!ctx->get_result_fn) {
                return XGR_E_NOT_SUPPORTED;
            }
            return ctx->get_result_fn(data->buffer, data->bufferSize);
        case XAsyncOp_Cancel:
            ctx->canceled.store(true, std::memory_order_release);
            return XGR_S_OK;
        case XAsyncOp_Cleanup:
            if (ctx->cleanup_unclaimed_fn) {
                ctx->cleanup_unclaimed_fn();
            }
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

XGameResult StartXsapiAsync(
    XAsyncBlock* async,
    const char* identity,
    std::unique_ptr<XsapiAsyncOp> op) {
    if (!async) {
        return XGR_E_INVALIDARG;
    }
    XsapiAsyncOp* raw = op.release();
    XGameResult hr = XAsyncBegin(async, raw, identity, identity, &XsapiAsyncProvider);
    if (hr < 0) {
        delete raw;
        return hr;
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
        return hr;
    }
    return XGR_S_OK;
}

bool CompareAchievementIds(const std::string& a, const std::string& b) {
    auto is_digits = [](const std::string& str) {
        if (str.empty()) return false;
        for (char c : str) {
            if (c < '0' || c > '9') return false;
        }
        return true;
    };
    if (is_digits(a) && is_digits(b)) {
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    }
    return a < b;
}

std::string FormatDoubleStat(double val) {
    if (std::isfinite(val)) {
        const double rounded = std::round(val);
        if (std::fabs(val - rounded) < 1e-9 &&
            rounded >= -9007199254740992.0 && rounded <= 9007199254740992.0) {
            return std::to_string(static_cast<int64_t>(rounded));
        }
    }
    std::ostringstream oss;
    oss << std::setprecision(15) << val;
    return oss.str();
}

}  // namespace

extern "C" {

// ===========================================================================
// Lifecycle & Context
// ===========================================================================
XGameResult XblInitialize(const XblInitArgs* args) {
    if (!args) {
        return XGR_E_INVALIDARG;
    }
    XsapiState& s = GetState();
    XTaskQueueHandle dup_queue = nullptr;
    if (args->queue) {
        XGameResult hr = XTaskQueueDuplicateHandle(args->queue, &dup_queue);
        if (hr < 0) {
            return hr;
        }
    }

    XTaskQueueHandle old_queue = nullptr;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        old_queue = s.default_queue;
        s.default_queue = dup_queue;
        s.scid = (args->scid && *args->scid)
                     ? std::string(args->scid)
                     : std::string("00000000-0000-0000-0000-00005ca00001");
        s.initialized = true;
    }
    if (old_queue) {
        XTaskQueueCloseHandle(old_queue);
    }
    return XBL_S_OK;
}

XGameResult XblCleanupAsync(XAsyncBlock* async) {
    if (!async) {
        return XGR_E_INVALIDARG;
    }
    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [](XsapiAsyncOp&) -> XGameResult {
        XTaskQueueHandle old_queue = nullptr;
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            // Flush all loaded caches to disk before clearing in-memory state.
            for (const auto& [key, _] : s.achievements_cache) {
                SaveAchievementsLocked(s, key.scid, key.xuid);
            }
            for (const auto& [key, _] : s.stats_cache) {
                SaveStatsLocked(s, key.scid, key.xuid);
            }
            s.achievements_cache.clear();
            s.achievements_loaded.clear();
            s.stats_cache.clear();
            s.stats_loaded.clear();
            s.presence_by_xuid.clear();
            old_queue = s.default_queue;
            s.default_queue = nullptr;
            s.scid.clear();
            s.initialized = false;
        }
        if (old_queue) {
            XTaskQueueCloseHandle(old_queue);
        }
        return XBL_S_OK;
    };
    return StartXsapiAsync(async, kIdCleanup, std::move(op));
}

XGameResult XblContextCreateHandle(XUserHandle user, XblContextHandle* context) {
    if (!user || !context) {
        return XGR_E_INVALIDARG;
    }
    *context = nullptr;

    std::string active_scid;
    {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) {
            return XBL_E_NOT_INITIALIZED;
        }
        active_scid = s.scid;
    }

    uint64_t xuid = 0;
    XGameResult hr = XUserGetId(user, &xuid);
    if (hr < 0) {
        return hr;
    }

    XUserHandle dup_user = nullptr;
    hr = XUserDuplicateHandle(user, &dup_user);
    if (hr < 0) {
        return hr;
    }

    auto ctx = std::make_shared<XblContext>();
    ctx->user = dup_user;
    ctx->xuid = xuid;
    ctx->scid = std::move(active_scid);

    XblContextHandle raw = ctx.get();
    {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        s.live_contexts[raw] = std::move(ctx);
    }
    *context = raw;
    return XBL_S_OK;
}

XGameResult XblContextDuplicateHandle(
    XblContextHandle handle,
    XblContextHandle* duplicatedHandle) {
    if (!handle || !duplicatedHandle) {
        return XGR_E_INVALIDARG;
    }
    *duplicatedHandle = nullptr;
    auto ctx = LookupContext(handle);
    if (!ctx) {
        return XGR_E_HANDLE;
    }
    ctx->ref_count.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = handle;
    return XBL_S_OK;
}

void XblContextCloseHandle(XblContextHandle handle) {
    if (!handle) {
        return;
    }
    std::shared_ptr<XblContext> dropped;
    {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        auto it = s.live_contexts.find(handle);
        if (it == s.live_contexts.end()) {
            return;
        }
        if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            dropped = std::move(it->second);
            s.live_contexts.erase(it);
        }
    }
    if (dropped && dropped->user) {
        XUserCloseHandle(dropped->user);
        dropped->user = nullptr;
    }
}

XGameResult XblContextGetUser(XblContextHandle context, XUserHandle* user) {
    if (!context || !user) {
        return XGR_E_INVALIDARG;
    }
    *user = nullptr;
    auto ctx = LookupContext(context);
    if (!ctx || !ctx->user) {
        return XGR_E_HANDLE;
    }
    return XUserDuplicateHandle(ctx->user, user);
}

XGameResult XblContextGetXboxUserId(XblContextHandle context, uint64_t* xboxUserId) {
    if (!context || !xboxUserId) {
        return XGR_E_INVALIDARG;
    }
    auto ctx = LookupContext(context);
    if (!ctx) {
        return XGR_E_HANDLE;
    }
    *xboxUserId = ctx->xuid;
    return XBL_S_OK;
}

// ===========================================================================
// Achievements
// ===========================================================================
XGameResult XblAchievementsUpdateAchievementForTitleIdAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    uint32_t titleId,
    const char* serviceConfigurationId,
    const char* achievementId,
    uint32_t percentComplete,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !achievementId || !*achievementId || !async) {
        return XGR_E_INVALIDARG;
    }
    if (percentComplete > 100u) {
        return XGR_E_INVALIDARG;
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) {
        return XGR_E_HANDLE;
    }

    const uint64_t target_xuid = xboxUserId != 0 ? xboxUserId : ctx->xuid;
    const uint32_t target_tid = titleId != 0 ? titleId : CurrentTitleId();
    const std::string target_scid = (serviceConfigurationId && *serviceConfigurationId)
                                        ? std::string(serviceConfigurationId)
                                        : ctx->scid;
    const std::string ach_id = achievementId;

    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [ctx, target_xuid, target_tid, target_scid, ach_id, percentComplete](
                      XsapiAsyncOp&) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) {
            return XBL_E_NOT_INITIALIZED;
        }
        EnsureAchievementsLoadedLocked(s, target_scid, target_xuid);
        auto& map = s.achievements_cache[StoreKey{target_scid, target_xuid}];
        auto& ach = map[ach_id];
        if (ach.id.empty()) {
            ach.id = ach_id;
            ach.scid = target_scid;
            ach.title_id = target_tid;
            ach.name = "Achievement " + ach_id;
            ach.unlocked_desc = "Unlocked achievement " + ach_id;
            ach.locked_desc = "Complete achievement " + ach_id;
            ach.gamerscore = 25;
            ach.type = XblAchievementType_Persistent;
            ach.is_secret = false;
        } else if (ach.title_id == 0) {
            ach.title_id = target_tid;
        }

        // Xbox Live achievement progress is monotonic (0..100).
        if (percentComplete > ach.percent_complete) {
            ach.percent_complete = percentComplete;
        }
        if (ach.percent_complete >= 100u) {
            ach.percent_complete = 100u;
            if (ach.time_unlocked == 0) {
                ach.time_unlocked = NowUnixSeconds();
            }
        }
        if (!SaveAchievementsLocked(s, target_scid, target_xuid)) {
            return XGR_E_FAIL;
        }
        return XBL_S_OK;
    };
    return StartXsapiAsync(async, kIdAchUpdate, std::move(op));
}

XGameResult XblAchievementsUpdateAchievementAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* achievementId,
    uint32_t percentComplete,
    XAsyncBlock* async) {
    return XblAchievementsUpdateAchievementForTitleIdAsync(
        xboxLiveContext,
        xboxUserId,
        CurrentTitleId(),
        nullptr,
        achievementId,
        percentComplete,
        async);
}

XGameResult XblAchievementsGetAchievementsForTitleIdAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    uint32_t titleId,
    uint32_t type,
    bool unlockedOnly,
    uint32_t orderBy,
    uint32_t skipItems,
    uint32_t maxItems,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return XGR_E_INVALIDARG;
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) {
        return XGR_E_HANDLE;
    }

    const uint64_t target_xuid = xboxUserId != 0 ? xboxUserId : ctx->xuid;
    const std::string target_scid = ctx->scid;

    auto result_holder = std::make_shared<XblAchievementsResultHandle>(nullptr);
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, target_xuid, target_scid, titleId, type, unlockedOnly, orderBy,
                   skipItems, maxItems, result_holder](XsapiAsyncOp& self) -> XGameResult {
        std::vector<StoredAchievement> filtered;
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) {
                return XBL_E_NOT_INITIALIZED;
            }
            EnsureAchievementsLoadedLocked(s, target_scid, target_xuid);
            const auto it = s.achievements_cache.find(StoreKey{target_scid, target_xuid});
            if (it != s.achievements_cache.end()) {
                for (const auto& [id, ach] : it->second) {
                    if (titleId != 0 && ach.title_id != titleId) {
                        continue;
                    }
                    if (type == XblAchievementType_Persistent &&
                        ach.type != XblAchievementType_Persistent) {
                        continue;
                    }
                    if (type == XblAchievementType_Challenge &&
                        ach.type != XblAchievementType_Challenge) {
                        continue;
                    }
                    if (unlockedOnly && ach.percent_complete < 100u) {
                        continue;
                    }
                    filtered.push_back(ach);
                }
            }
        }

        // Apply requested ordering.
        if (orderBy == XblAchievementOrderBy_TitleId) {
            std::sort(filtered.begin(), filtered.end(),
                      [](const StoredAchievement& a, const StoredAchievement& b) {
                          if (a.title_id != b.title_id) return a.title_id < b.title_id;
                          return CompareAchievementIds(a.id, b.id);
                      });
        } else if (orderBy == XblAchievementOrderBy_UnlockTime) {
            std::sort(filtered.begin(), filtered.end(),
                      [](const StoredAchievement& a, const StoredAchievement& b) {
                          if (a.time_unlocked != b.time_unlocked) {
                              return a.time_unlocked > b.time_unlocked;
                          }
                          return CompareAchievementIds(a.id, b.id);
                      });
        } else {
            std::sort(filtered.begin(), filtered.end(),
                      [](const StoredAchievement& a, const StoredAchievement& b) {
                          return CompareAchievementIds(a.id, b.id);
                      });
        }

        // Apply pagination (skipItems / maxItems).
        const size_t total = filtered.size();
        const size_t start = std::min<size_t>(skipItems, total);
        size_t count = total - start;
        if (maxItems > 0 && count > maxItems) {
            count = maxItems;
        }
        const bool has_next = (start + count) < total;

        auto res = std::make_shared<XblAchievementsResult>();
        res->has_next = has_next;
        res->storage.reserve(count);
        res->requirements.reserve(count);
        res->items.reserve(count);

        for (size_t i = 0; i < count; ++i) {
            const auto& src = filtered[start + i];
            XblAchievementsResult::EntryStorage st{};
            st.id = src.id;
            st.scid = src.scid;
            st.name = src.name;
            st.unlocked_desc = src.unlocked_desc;
            st.locked_desc = src.locked_desc;
            st.current_progress_str = std::to_string(src.percent_complete);
            st.target_progress_str = "100";
            res->storage.push_back(std::move(st));
        }

        for (size_t i = 0; i < count; ++i) {
            const auto& src = filtered[start + i];
            const auto& st = res->storage[i];

            XblAchievementRequirement req{};
            req.id = st.id.c_str();
            req.currentProgressValue = st.current_progress_str.c_str();
            req.targetProgressValue = st.target_progress_str.c_str();
            res->requirements.push_back(req);
        }

        for (size_t i = 0; i < count; ++i) {
            const auto& src = filtered[start + i];
            const auto& st = res->storage[i];

            XblAchievement item{};
            item.id = st.id.c_str();
            item.serviceConfigurationId = st.scid.c_str();
            item.name = st.name.c_str();
            item.titleId = src.title_id;
            if (src.percent_complete >= 100u) {
                item.progressState = XblAchievementProgressState_Achieved;
            } else if (src.percent_complete > 0u) {
                item.progressState = XblAchievementProgressState_InProgress;
            } else {
                item.progressState = XblAchievementProgressState_NotStarted;
            }
            item.progression.requirements = &res->requirements[i];
            item.progression.requirementsCount = 1;
            item.progression.timeUnlocked = src.time_unlocked;
            item.unlockedDescription = st.unlocked_desc.c_str();
            item.lockedDescription = st.locked_desc.c_str();
            item.type = src.type;
            item.participationType = XblAchievementParticipationType_Individual;
            item.percentComplete = src.percent_complete;
            item.gamerscore = src.gamerscore;
            item.isSecret = src.is_secret;
            item.isRevoked = false;
            res->items.push_back(item);
        }

        XblAchievementsResultHandle raw = res.get();
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            s.live_ach_results[raw] = std::move(res);
        }
        *result_holder = raw;
        self.required_buffer_size = sizeof(XblAchievementsResultHandle);
        return XBL_S_OK;
    };

    op->get_result_fn = [result_holder](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblAchievementsResultHandle)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        XblAchievementsResultHandle h = *result_holder;
        *result_holder = nullptr;
        std::memcpy(buffer, &h, sizeof(h));
        return XBL_S_OK;
    };

    op->cleanup_unclaimed_fn = [result_holder]() {
        if (*result_holder) {
            XblAchievementsResultCloseHandle(*result_holder);
            *result_holder = nullptr;
        }
    };

    return StartXsapiAsync(async, kIdAchGet, std::move(op));
}

XGameResult XblAchievementsGetAchievementsForTitleIdResult(
    XAsyncBlock* async,
    XblAchievementsResultHandle* result) {
    if (!async || !result) {
        return XGR_E_INVALIDARG;
    }
    *result = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return hr;
    }
    return XAsyncGetResult(async, kIdAchGet, sizeof(XblAchievementsResultHandle), result, nullptr);
}

XGameResult XblAchievementsResultGetAchievements(
    XblAchievementsResultHandle resultHandle,
    const XblAchievement** achievements,
    size_t* achievementsCount) {
    if (!resultHandle || !achievements || !achievementsCount) {
        return XGR_E_INVALIDARG;
    }
    auto res = LookupAchResult(resultHandle);
    if (!res) {
        return XGR_E_HANDLE;
    }
    *achievements = res->items.empty() ? nullptr : res->items.data();
    *achievementsCount = res->items.size();
    return XBL_S_OK;
}

XGameResult XblAchievementsResultHasNext(
    XblAchievementsResultHandle resultHandle,
    bool* hasNext) {
    if (!resultHandle || !hasNext) {
        return XGR_E_INVALIDARG;
    }
    auto res = LookupAchResult(resultHandle);
    if (!res) {
        return XGR_E_HANDLE;
    }
    *hasNext = res->has_next;
    return XBL_S_OK;
}

XGameResult XblAchievementsResultDuplicateHandle(
    XblAchievementsResultHandle handle,
    XblAchievementsResultHandle* duplicatedHandle) {
    if (!handle || !duplicatedHandle) {
        return XGR_E_INVALIDARG;
    }
    *duplicatedHandle = nullptr;
    auto res = LookupAchResult(handle);
    if (!res) {
        return XGR_E_HANDLE;
    }
    res->ref_count.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = handle;
    return XBL_S_OK;
}

void XblAchievementsResultCloseHandle(XblAchievementsResultHandle resultHandle) {
    if (!resultHandle) {
        return;
    }
    std::shared_ptr<XblAchievementsResult> dropped;
    {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        auto it = s.live_ach_results.find(resultHandle);
        if (it == s.live_ach_results.end()) {
            return;
        }
        if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            dropped = std::move(it->second);
            s.live_ach_results.erase(it);
        }
    }
}

// ===========================================================================
// Presence
// ===========================================================================
XGameResult XblPresenceSetPresenceAsync(
    XblContextHandle xboxLiveContext,
    bool isUserActiveInTitle,
    const XblPresenceRichPresenceIds* richPresenceIds,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return XGR_E_INVALIDARG;
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) {
        return XGR_E_HANDLE;
    }

    std::string scid = ctx->scid;
    std::string rich_str;
    if (richPresenceIds) {
        if (richPresenceIds->scid[0] != '\0') {
            scid = richPresenceIds->scid;
        }
        if (richPresenceIds->presenceId && *richPresenceIds->presenceId) {
            rich_str = richPresenceIds->presenceId;
            if (richPresenceIds->presenceTokenIds && richPresenceIds->presenceTokenIdsCount > 0) {
                rich_str += "(";
                for (size_t i = 0; i < richPresenceIds->presenceTokenIdsCount; ++i) {
                    if (i > 0) rich_str += ", ";
                    rich_str += richPresenceIds->presenceTokenIds[i]
                                    ? richPresenceIds->presenceTokenIds[i]
                                    : "";
                }
                rich_str += ")";
            }
        }
    }
    const uint32_t tid = CurrentTitleId();

    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [ctx, isUserActiveInTitle, scid, rich_str, tid](XsapiAsyncOp&) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) {
            return XBL_E_NOT_INITIALIZED;
        }
        StoredPresence p{};
        p.xuid = ctx->xuid;
        p.user_state = isUserActiveInTitle ? XblPresenceUserState_Online : XblPresenceUserState_Away;
        p.title_id = tid;
        p.scid = scid;
        p.title_name = "ScarlettX Title";
        p.rich_presence = rich_str;
        p.last_modified = NowUnixSeconds();
        p.title_active = isUserActiveInTitle;
        s.presence_by_xuid[ctx->xuid] = std::move(p);
        return XBL_S_OK;
    };
    return StartXsapiAsync(async, kIdPresenceSet, std::move(op));
}

XGameResult XblPresenceGetPresenceAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xuid,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return XGR_E_INVALIDARG;
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) {
        return XGR_E_HANDLE;
    }
    const uint64_t target_xuid = xuid != 0 ? xuid : ctx->xuid;

    auto result_holder = std::make_shared<XblPresenceRecordHandle>(nullptr);
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, target_xuid, result_holder](XsapiAsyncOp& self) -> XGameResult {
        StoredPresence stored{};
        bool found = false;
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) {
                return XBL_E_NOT_INITIALIZED;
            }
            auto it = s.presence_by_xuid.find(target_xuid);
            if (it != s.presence_by_xuid.end()) {
                stored = it->second;
                found = true;
            }
        }

        auto rec = std::make_shared<XblPresenceRecord>();
        rec->xuid = target_xuid;
        if (found) {
            rec->user_state = stored.user_state;
            rec->title_name = stored.title_name;
            rec->rich_presence_string = stored.rich_presence;

            XblPresenceTitleRecord tr{};
            tr.titleId = stored.title_id;
            tr.titleName = rec->title_name.c_str();
            tr.lastModified = stored.last_modified;
            tr.titleActive = stored.title_active;
            tr.richPresenceString = rec->rich_presence_string.c_str();
            tr.viewState = stored.title_active ? 1u : 0u;
            rec->title_records.push_back(tr);

            XblPresenceDeviceRecord dr{};
            dr.deviceType = XblPresenceDeviceType_Scarlett;
            dr.titleRecords = rec->title_records.data();
            dr.titleRecordsCount = rec->title_records.size();
            rec->device_records.push_back(dr);
        } else {
            rec->user_state = (target_xuid == ctx->xuid)
                                  ? XblPresenceUserState_Online
                                  : XblPresenceUserState_Offline;
        }

        XblPresenceRecordHandle raw = rec.get();
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            s.live_presence_records[raw] = std::move(rec);
        }
        *result_holder = raw;
        self.required_buffer_size = sizeof(XblPresenceRecordHandle);
        return XBL_S_OK;
    };

    op->get_result_fn = [result_holder](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblPresenceRecordHandle)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        XblPresenceRecordHandle h = *result_holder;
        *result_holder = nullptr;
        std::memcpy(buffer, &h, sizeof(h));
        return XBL_S_OK;
    };

    op->cleanup_unclaimed_fn = [result_holder]() {
        if (*result_holder) {
            XblPresenceRecordCloseHandle(*result_holder);
            *result_holder = nullptr;
        }
    };

    return StartXsapiAsync(async, kIdPresenceGet, std::move(op));
}

XGameResult XblPresenceGetPresenceResult(
    XAsyncBlock* async,
    XblPresenceRecordHandle* presenceRecordHandle) {
    if (!async || !presenceRecordHandle) {
        return XGR_E_INVALIDARG;
    }
    *presenceRecordHandle = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return hr;
    }
    return XAsyncGetResult(
        async, kIdPresenceGet, sizeof(XblPresenceRecordHandle), presenceRecordHandle, nullptr);
}

XGameResult XblPresenceRecordGetXuid(XblPresenceRecordHandle handle, uint64_t* xuid) {
    if (!handle || !xuid) {
        return XGR_E_INVALIDARG;
    }
    auto rec = LookupPresenceRecord(handle);
    if (!rec) {
        return XGR_E_HANDLE;
    }
    *xuid = rec->xuid;
    return XBL_S_OK;
}

XGameResult XblPresenceRecordGetUserState(
    XblPresenceRecordHandle handle,
    XblPresenceUserState* userState) {
    if (!handle || !userState) {
        return XGR_E_INVALIDARG;
    }
    auto rec = LookupPresenceRecord(handle);
    if (!rec) {
        return XGR_E_HANDLE;
    }
    *userState = rec->user_state;
    return XBL_S_OK;
}

XGameResult XblPresenceRecordGetDeviceRecords(
    XblPresenceRecordHandle handle,
    const XblPresenceDeviceRecord** deviceRecords,
    size_t* deviceRecordsCount) {
    if (!handle || !deviceRecords || !deviceRecordsCount) {
        return XGR_E_INVALIDARG;
    }
    auto rec = LookupPresenceRecord(handle);
    if (!rec) {
        return XGR_E_HANDLE;
    }
    *deviceRecords = rec->device_records.empty() ? nullptr : rec->device_records.data();
    *deviceRecordsCount = rec->device_records.size();
    return XBL_S_OK;
}

XGameResult XblPresenceRecordDuplicateHandle(
    XblPresenceRecordHandle handle,
    XblPresenceRecordHandle* duplicatedHandle) {
    if (!handle || !duplicatedHandle) {
        return XGR_E_INVALIDARG;
    }
    *duplicatedHandle = nullptr;
    auto rec = LookupPresenceRecord(handle);
    if (!rec) {
        return XGR_E_HANDLE;
    }
    rec->ref_count.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = handle;
    return XBL_S_OK;
}

void XblPresenceRecordCloseHandle(XblPresenceRecordHandle handle) {
    if (!handle) {
        return;
    }
    std::shared_ptr<XblPresenceRecord> dropped;
    {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        auto it = s.live_presence_records.find(handle);
        if (it == s.live_presence_records.end()) {
            return;
        }
        if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            dropped = std::move(it->second);
            s.live_presence_records.erase(it);
        }
    }
}

// ===========================================================================
// Profile
// ===========================================================================
XGameResult XblProfileGetUserProfileAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return XGR_E_INVALIDARG;
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) {
        return XGR_E_HANDLE;
    }
    const uint64_t target_xuid = xboxUserId != 0 ? xboxUserId : ctx->xuid;

    auto profile_holder = std::make_shared<XblUserProfile>();
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, target_xuid, profile_holder](XsapiAsyncOp& self) -> XGameResult {
        uint64_t total_gamerscore = 0;
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) {
                return XBL_E_NOT_INITIALIZED;
            }
            EnsureAchievementsLoadedLocked(s, ctx->scid, target_xuid);
            auto it = s.achievements_cache.find(StoreKey{ctx->scid, target_xuid});
            if (it != s.achievements_cache.end()) {
                for (const auto& [id, ach] : it->second) {
                    if (ach.percent_complete >= 100u) {
                        total_gamerscore += ach.gamerscore;
                    }
                }
            }
        }

        XblUserProfile p{};
        p.xboxUserId = target_xuid;

        std::string gt_classic = "ScarlettXUser";
        std::string gt_modern = "ScarlettX";
        std::string gt_suffix = "1001";
        std::string gt_unique = "ScarlettX#1001";

        if (target_xuid == ctx->xuid && ctx->user) {
            char buf[128]{};
            size_t used = 0;
            if (XUserGetGamertag(ctx->user, XUserGamertagComponent_Classic, sizeof(buf), buf, &used) == XGR_S_OK) {
                gt_classic = buf;
            }
            if (XUserGetGamertag(ctx->user, XUserGamertagComponent_Modern, sizeof(buf), buf, &used) == XGR_S_OK) {
                gt_modern = buf;
            }
            if (XUserGetGamertag(ctx->user, XUserGamertagComponent_ModernSuffix, sizeof(buf), buf, &used) == XGR_S_OK) {
                gt_suffix = buf;
            }
            if (XUserGetGamertag(ctx->user, XUserGamertagComponent_UniqueModern, sizeof(buf), buf, &used) == XGR_S_OK) {
                gt_unique = buf;
            }
        } else {
            const std::string tag_id = std::to_string(target_xuid % 10000ull);
            gt_classic = "OfflineUser" + tag_id;
            gt_modern = "OfflineUser";
            gt_suffix = tag_id;
            gt_unique = gt_modern + "#" + gt_suffix;
        }

        CopyCString(p.gamertag, sizeof(p.gamertag), gt_classic);
        CopyCString(p.modernGamertag, sizeof(p.modernGamertag), gt_modern);
        CopyCString(p.modernGamertagSuffix, sizeof(p.modernGamertagSuffix), gt_suffix);
        CopyCString(p.uniqueModernGamertag, sizeof(p.uniqueModernGamertag), gt_unique);
        CopyCString(p.appDisplayName, sizeof(p.appDisplayName), gt_classic);
        CopyCString(p.gameDisplayName, sizeof(p.gameDisplayName), gt_classic);
        CopyCString(p.gamerscore, sizeof(p.gamerscore), std::to_string(total_gamerscore));
        CopyCString(p.appDisplayPictureResizeUri, sizeof(p.appDisplayPictureResizeUri),
                    "https://scarlettx.local/gamerpic/default.png");
        CopyCString(p.gameDisplayPictureResizeUri, sizeof(p.gameDisplayPictureResizeUri),
                    "https://scarlettx.local/gamerpic/default.png");

        *profile_holder = p;
        self.required_buffer_size = sizeof(XblUserProfile);
        return XBL_S_OK;
    };

    op->get_result_fn = [profile_holder](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblUserProfile)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        std::memcpy(buffer, profile_holder.get(), sizeof(XblUserProfile));
        return XBL_S_OK;
    };

    return StartXsapiAsync(async, kIdProfileGet, std::move(op));
}

XGameResult XblProfileGetUserProfileResult(XAsyncBlock* async, XblUserProfile* profile) {
    if (!async || !profile) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return hr;
    }
    return XAsyncGetResult(async, kIdProfileGet, sizeof(XblUserProfile), profile, nullptr);
}

// ===========================================================================
// User Statistics & Title-Managed Statistics
// ===========================================================================
XGameResult XblTitleManagedStatsWriteAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const XblTitleManagedStatistic* statistics,
    size_t statisticsCount,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async || (!statistics && statisticsCount > 0)) {
        return XGR_E_INVALIDARG;
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) {
        return XGR_E_HANDLE;
    }

    std::vector<StoredStat> entries;
    entries.reserve(statisticsCount);
    for (size_t i = 0; i < statisticsCount; ++i) {
        if (!statistics[i].statisticName || !*statistics[i].statisticName) {
            return XGR_E_INVALIDARG;
        }
        StoredStat st{};
        st.name = statistics[i].statisticName;
        if (statistics[i].statisticType == XblTitleManagedStatType_Number) {
            st.type = "Double";
            st.value = FormatDoubleStat(statistics[i].numberValue);
        } else if (statistics[i].statisticType == XblTitleManagedStatType_String) {
            st.type = "String";
            st.value = statistics[i].stringValue ? statistics[i].stringValue : "";
        } else {
            return XGR_E_INVALIDARG;
        }
        entries.push_back(std::move(st));
    }

    const uint64_t target_xuid = xboxUserId != 0 ? xboxUserId : ctx->xuid;
    const std::string target_scid = ctx->scid;

    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [ctx, target_xuid, target_scid, entries = std::move(entries)](
                      XsapiAsyncOp&) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) {
            return XBL_E_NOT_INITIALIZED;
        }
        EnsureStatsLoadedLocked(s, target_scid, target_xuid);
        auto& map = s.stats_cache[StoreKey{target_scid, target_xuid}];
        for (const auto& entry : entries) {
            map[entry.name] = entry;
        }
        if (!SaveStatsLocked(s, target_scid, target_xuid)) {
            return XGR_E_FAIL;
        }
        return XBL_S_OK;
    };
    return StartXsapiAsync(async, kIdStatsWrite, std::move(op));
}

XGameResult XblUserStatisticsGetSingleUserStatisticAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* serviceConfigurationId,
    const char* statisticName,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !statisticName || !*statisticName || !async) {
        return XGR_E_INVALIDARG;
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) {
        return XGR_E_HANDLE;
    }

    const uint64_t target_xuid = xboxUserId != 0 ? xboxUserId : ctx->xuid;
    const std::string target_scid = (serviceConfigurationId && *serviceConfigurationId)
                                        ? std::string(serviceConfigurationId)
                                        : ctx->scid;
    const std::string stat_name = statisticName;

    struct StatPayload {
        uint64_t xuid = 0;
        std::string scid;
        StoredStat stat;
        size_t packed_size = 0;
    };
    auto payload = std::make_shared<StatPayload>();
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, target_xuid, target_scid, stat_name, payload](
                      XsapiAsyncOp& self) -> XGameResult {
        StoredStat result_stat{};
        result_stat.name = stat_name;
        result_stat.type = "Int64";
        result_stat.value = "0";
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) {
                return XBL_E_NOT_INITIALIZED;
            }
            EnsureStatsLoadedLocked(s, target_scid, target_xuid);
            auto it = s.stats_cache.find(StoreKey{target_scid, target_xuid});
            if (it != s.stats_cache.end()) {
                auto sit = it->second.find(stat_name);
                if (sit != it->second.end()) {
                    result_stat = sit->second;
                }
            }
        }

        payload->xuid = target_xuid;
        payload->scid = target_scid;
        payload->stat = std::move(result_stat);
        payload->packed_size = sizeof(XblUserStatisticsResult) +
                               sizeof(XblServiceConfigurationStatistic) +
                               sizeof(XblStatistic) +
                               (payload->stat.name.size() + 1) +
                               (payload->stat.type.size() + 1) +
                               (payload->stat.value.size() + 1);
        self.required_buffer_size = payload->packed_size;
        return XBL_S_OK;
    };

    op->get_result_fn = [payload](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer) {
            return XGR_E_INVALIDARG;
        }
        if (buffer_size < payload->packed_size) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        std::memset(buffer, 0, payload->packed_size);

        auto* base = static_cast<uint8_t*>(buffer);
        size_t offset = 0;

        auto* res = reinterpret_cast<XblUserStatisticsResult*>(base + offset);
        offset += sizeof(XblUserStatisticsResult);

        auto* sc_stat = reinterpret_cast<XblServiceConfigurationStatistic*>(base + offset);
        offset += sizeof(XblServiceConfigurationStatistic);

        auto* stat = reinterpret_cast<XblStatistic*>(base + offset);
        offset += sizeof(XblStatistic);

        auto append_str = [&](const std::string& str) -> const char* {
            char* dst = reinterpret_cast<char*>(base + offset);
            std::memcpy(dst, str.c_str(), str.size() + 1);
            offset += str.size() + 1;
            return dst;
        };

        stat->statisticName = append_str(payload->stat.name);
        stat->statisticType = append_str(payload->stat.type);
        stat->value = append_str(payload->stat.value);

        CopyCString(sc_stat->serviceConfigurationId, sizeof(sc_stat->serviceConfigurationId),
                    payload->scid);
        sc_stat->statistics = stat;
        sc_stat->statisticsCount = 1;

        res->xboxUserId = payload->xuid;
        res->serviceConfigStatistics = sc_stat;
        res->serviceConfigStatisticsCount = 1;
        return XBL_S_OK;
    };

    return StartXsapiAsync(async, kIdStatsGet, std::move(op));
}

XGameResult XblUserStatisticsGetSingleUserStatisticResultSize(
    XAsyncBlock* async,
    size_t* resultSizeInBytes) {
    if (!async || !resultSizeInBytes) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return hr;
    }
    return XAsyncGetResultSize(async, resultSizeInBytes);
}

XGameResult XblUserStatisticsGetSingleUserStatisticResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblUserStatisticsResult** ptrToBuffer,
    size_t* bufferUsed) {
    if (!async || !buffer || !ptrToBuffer) {
        return XGR_E_INVALIDARG;
    }
    *ptrToBuffer = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return hr;
    }
    size_t used = 0;
    hr = XAsyncGetResult(async, kIdStatsGet, bufferSize, buffer, &used);
    if (hr < 0) {
        return hr;
    }
    *ptrToBuffer = static_cast<XblUserStatisticsResult*>(buffer);
    if (bufferUsed) {
        *bufferUsed = used;
    }
    return XBL_S_OK;
}

}  // extern "C"
