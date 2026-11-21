#include "scarlettx/gdk/xgameruntime_abi.h"
#include "scarlettx/winrt/winrt_abi.h"
#include "scarlettx/xsapi/xsapi_abi.h"
#include "sx_check.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Helpers for WinRT UTF-16 HSTRING conversions in tests
// ---------------------------------------------------------------------------
std::u16string ToU16String(SX_HSTRING h) {
    uint32_t len = 0;
    const uint16_t* raw = WindowsGetStringRawBuffer(h, &len);
    SX_REQUIRE(raw != nullptr);
    std::u16string out;
    out.resize(len);
    if (len > 0) {
        std::memcpy(out.data(), raw, static_cast<size_t>(len) * sizeof(uint16_t));
    }
    return out;
}

SX_HSTRING MakeHString(const std::u16string& s) {
    SX_HSTRING h = nullptr;
    SX_REQUIRE(WindowsCreateString(
                   reinterpret_cast<const uint16_t*>(s.data()),
                   static_cast<uint32_t>(s.size()),
                   &h) == SX_WINRT_S_OK);
    return h;
}

// ---------------------------------------------------------------------------
// Custom Guest WinRT Object & Factory (for RoRegisterActivationFactories test)
// ---------------------------------------------------------------------------
std::atomic<int> g_custom_widget_live{0};
std::atomic<int> g_custom_factory_live{0};

struct CustomWidgetObj {
    const SX_IInspectableVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
};

int32_t SCARLETTX_WINRT_CALL Cw_QI(SX_IInspectable* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (std::memcmp(riid, &SX_IID_IUnknown, sizeof(SX_GUID)) == 0 ||
        std::memcmp(riid, &SX_IID_IInspectable, sizeof(SX_GUID)) == 0) {
        auto* self = reinterpret_cast<CustomWidgetObj*>(iface);
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Cw_AddRef(SX_IInspectable* iface) {
    return reinterpret_cast<CustomWidgetObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Cw_Release(SX_IInspectable* iface) {
    auto* self = reinterpret_cast<CustomWidgetObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        g_custom_widget_live.fetch_sub(1);
        delete self;
    }
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Cw_GetIids(SX_IInspectable*, uint32_t* c, SX_GUID** iids) {
    *c = 0;
    *iids = nullptr;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cw_GetRuntimeClassName(SX_IInspectable*, SX_HSTRING* name) {
    const std::u16string n = u"ScarlettX.Test.CustomWidget";
    return WindowsCreateString(reinterpret_cast<const uint16_t*>(n.data()),
                               static_cast<uint32_t>(n.size()), name);
}
int32_t SCARLETTX_WINRT_CALL Cw_GetTrustLevel(SX_IInspectable*, SX_TrustLevel* tl) {
    *tl = SX_TrustLevel_PartialTrust;
    return SX_WINRT_S_OK;
}

const SX_IInspectableVtbl kCustomWidgetVtbl = {
    &Cw_QI, &Cw_AddRef, &Cw_Release, &Cw_GetIids, &Cw_GetRuntimeClassName, &Cw_GetTrustLevel,
};

struct CustomWidgetFactoryObj {
    const SX_IActivationFactoryVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
};

int32_t SCARLETTX_WINRT_CALL Cwf_QI(SX_IActivationFactory* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    if (std::memcmp(riid, &SX_IID_IUnknown, sizeof(SX_GUID)) == 0 ||
        std::memcmp(riid, &SX_IID_IInspectable, sizeof(SX_GUID)) == 0 ||
        std::memcmp(riid, &SX_IID_IActivationFactory, sizeof(SX_GUID)) == 0) {
        auto* self = reinterpret_cast<CustomWidgetFactoryObj*>(iface);
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Cwf_AddRef(SX_IActivationFactory* iface) {
    return reinterpret_cast<CustomWidgetFactoryObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Cwf_Release(SX_IActivationFactory* iface) {
    auto* self = reinterpret_cast<CustomWidgetFactoryObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        g_custom_factory_live.fetch_sub(1);
        delete self;
    }
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Cwf_GetIids(SX_IActivationFactory*, uint32_t* c, SX_GUID** iids) {
    *c = 0;
    *iids = nullptr;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cwf_GetRuntimeClassName(SX_IActivationFactory*, SX_HSTRING* name) {
    const std::u16string n = u"ScarlettX.Test.CustomWidget";
    return WindowsCreateString(reinterpret_cast<const uint16_t*>(n.data()),
                               static_cast<uint32_t>(n.size()), name);
}
int32_t SCARLETTX_WINRT_CALL Cwf_GetTrustLevel(SX_IActivationFactory*, SX_TrustLevel* tl) {
    *tl = SX_TrustLevel_PartialTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Cwf_ActivateInstance(SX_IActivationFactory*, SX_IInspectable** inst) {
    auto* w = new CustomWidgetObj();
    w->lpVtbl = &kCustomWidgetVtbl;
    g_custom_widget_live.fetch_add(1);
    *inst = reinterpret_cast<SX_IInspectable*>(w);
    return SX_WINRT_S_OK;
}

const SX_IActivationFactoryVtbl kCustomWidgetFactoryVtbl = {
    &Cwf_QI, &Cwf_AddRef, &Cwf_Release,
    &Cwf_GetIids, &Cwf_GetRuntimeClassName, &Cwf_GetTrustLevel,
    &Cwf_ActivateInstance,
};

int32_t SCARLETTX_WINRT_CALL GetCustomWidgetFactory(
    SX_HSTRING /*classId*/,
    SX_IActivationFactory** factory) {
    auto* f = new CustomWidgetFactoryObj();
    f->lpVtbl = &kCustomWidgetFactoryVtbl;
    g_custom_factory_live.fetch_add(1);
    *factory = reinterpret_cast<SX_IActivationFactory*>(f);
    return SX_WINRT_S_OK;
}

// ---------------------------------------------------------------------------
// Test 1: Xbox Services API (Microsoft.Xbox.Services.GDK.C)
// ---------------------------------------------------------------------------
void TestXsapi() {
    SX_REQUIRE(XGameRuntimeInitialize() == XGR_S_OK);

    // Sign in default Xbox user via xgameruntime.
    XUserHandle user = nullptr;
    {
        XAsyncBlock ab{};
        SX_REQUIRE(XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &ab) == XGR_S_OK);
        SX_REQUIRE(XUserAddResult(&ab, &user) == XGR_S_OK);
        SX_REQUIRE(user != nullptr);
    }
    uint64_t user_xuid = 0;
    SX_REQUIRE(XUserGetId(user, &user_xuid) == XGR_S_OK && user_xuid != 0);

    // Clean isolated SCID storage directory before running persistence checks.
    const char* kTestScid = "00000000-0000-0000-0000-testxsapi001";
    {
        const char* save_root = std::getenv("SCARLETTX_SAVE_ROOT");
        if (save_root && *save_root) {
            std::error_code ec;
            fs::remove_all(fs::path(save_root) / "xsapi" / kTestScid, ec);
        }
    }

    // Error handling before XblInitialize.
    SX_REQUIRE(XblInitialize(nullptr) == XGR_E_INVALIDARG);
    XblContextHandle pre_ctx = nullptr;
    SX_REQUIRE(XblContextCreateHandle(user, &pre_ctx) == XBL_E_NOT_INITIALIZED);

    // Initialize XSAPI with a dedicated threadpool task queue.
    XTaskQueueHandle queue = nullptr;
    SX_REQUIRE(XTaskQueueCreate(
                   XTaskQueueDispatchMode_ThreadPool,
                   XTaskQueueDispatchMode_ThreadPool,
                   &queue) == XGR_S_OK);

    XblInitArgs init_args{};
    init_args.queue = queue;
    init_args.scid = kTestScid;
    SX_REQUIRE(XblInitialize(&init_args) == XBL_S_OK);

    // Context lifecycle: create, duplicate, get user, get XUID, close original.
    XblContextHandle ctx = nullptr;
    SX_REQUIRE(XblContextCreateHandle(user, &ctx) == XBL_S_OK && ctx != nullptr);

    XblContextHandle ctx_dup = nullptr;
    SX_REQUIRE(XblContextDuplicateHandle(ctx, &ctx_dup) == XBL_S_OK && ctx_dup == ctx);

    uint64_t ctx_xuid = 0;
    SX_REQUIRE(XblContextGetXboxUserId(ctx, &ctx_xuid) == XBL_S_OK);
    SX_REQUIRE(ctx_xuid == user_xuid);

    XUserHandle fetched_user = nullptr;
    SX_REQUIRE(XblContextGetUser(ctx, &fetched_user) == XBL_S_OK && fetched_user != nullptr);
    uint64_t fetched_xuid = 0;
    SX_REQUIRE(XUserGetId(fetched_user, &fetched_xuid) == XGR_S_OK && fetched_xuid == user_xuid);
    XUserCloseHandle(fetched_user);

    // Close first handle; duplicate must stay valid.
    XblContextCloseHandle(ctx);
    ctx = ctx_dup;
    ctx_xuid = 0;
    SX_REQUIRE(XblContextGetXboxUserId(ctx, &ctx_xuid) == XBL_S_OK && ctx_xuid == user_xuid);

    uint32_t title_id = 0;
    SX_REQUIRE(XGameGetXboxTitleId(&title_id) == XGR_S_OK && title_id != 0);

    // Achievements: invalid percent > 100 must fail.
    {
        XAsyncBlock ab{};
        ab.queue = queue;
        SX_REQUIRE(XblAchievementsUpdateAchievementAsync(ctx, user_xuid, "bad", 101, &ab) ==
                   XGR_E_INVALIDARG);
    }

    // Achievements: update "1" to 50% with completion callback verification.
    {
        std::atomic<bool> cb_fired{false};
        XAsyncBlock ab{};
        ab.queue = queue;
        ab.context = &cb_fired;
        ab.callback = [](XAsyncBlock* b) {
            static_cast<std::atomic<bool>*>(b->context)->store(true, std::memory_order_release);
        };
        SX_REQUIRE(XblAchievementsUpdateAchievementAsync(ctx, user_xuid, "1", 50, &ab) == XBL_S_OK);
        SX_REQUIRE(XAsyncGetStatus(&ab, true) == XBL_S_OK);
        // Wait briefly for completion callback on threadpool completion port.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!cb_fired.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        SX_REQUIRE(cb_fired.load(std::memory_order_acquire));
    }

    // Update more achievements ("2" at 25%, "3" at 100%, "4" at 100%).
    auto update_ach = [&](const char* id, uint32_t pct) {
        XAsyncBlock ab{};
        ab.queue = queue;
        SX_REQUIRE(XblAchievementsUpdateAchievementAsync(ctx, user_xuid, id, pct, &ab) == XBL_S_OK);
        SX_REQUIRE(XAsyncGetStatus(&ab, true) == XBL_S_OK);
    };
    update_ach("2", 25);
    update_ach("3", 100);
    update_ach("4", 100);

    // Attempt regressing "1" from 50% -> 10% (must remain monotonic at 50%).
    update_ach("1", 10);

    // Query all achievements before unlocking "1".
    {
        XAsyncBlock ab{};
        ab.queue = queue;
        SX_REQUIRE(XblAchievementsGetAchievementsForTitleIdAsync(
                       ctx, user_xuid, title_id, XblAchievementType_All, false,
                       XblAchievementOrderBy_DefaultOrder, 0, 0, &ab) == XBL_S_OK);
        XblAchievementsResultHandle res = nullptr;
        SX_REQUIRE(XblAchievementsGetAchievementsForTitleIdResult(&ab, &res) == XBL_S_OK);
        SX_REQUIRE(res != nullptr);

        const XblAchievement* list = nullptr;
        size_t count = 0;
        SX_REQUIRE(XblAchievementsResultGetAchievements(res, &list, &count) == XBL_S_OK);
        SX_REQUIRE(count == 4 && list != nullptr);
        SX_REQUIRE(std::string(list[0].id) == "1");
        SX_REQUIRE(list[0].percentComplete == 50);
        SX_REQUIRE(list[0].progressState == XblAchievementProgressState_InProgress);
        SX_REQUIRE(list[0].progression.requirementsCount == 1);
        SX_REQUIRE(std::string(list[0].progression.requirements[0].currentProgressValue) == "50");

        SX_REQUIRE(std::string(list[1].id) == "2");
        SX_REQUIRE(list[1].percentComplete == 25);

        SX_REQUIRE(std::string(list[2].id) == "3");
        SX_REQUIRE(list[2].percentComplete == 100);
        SX_REQUIRE(list[2].progressState == XblAchievementProgressState_Achieved);
        SX_REQUIRE(list[2].progression.timeUnlocked > 0);

        bool has_next = true;
        SX_REQUIRE(XblAchievementsResultHasNext(res, &has_next) == XBL_S_OK && !has_next);

        XblAchievementsResultHandle res_dup = nullptr;
        SX_REQUIRE(XblAchievementsResultDuplicateHandle(res, &res_dup) == XBL_S_OK);
        XblAchievementsResultCloseHandle(res);
        // Duplicate still valid until closed.
        size_t count2 = 0;
        SX_REQUIRE(XblAchievementsResultGetAchievements(res_dup, &list, &count2) == XBL_S_OK);
        SX_REQUIRE(count2 == 4);
        XblAchievementsResultCloseHandle(res_dup);
    }

    // Advance "1" from 50% -> 100% (now 3 unlocked achievements: "1", "3", "4").
    update_ach("1", 100);

    // Filtered unlockedOnly query.
    {
        XAsyncBlock ab{};
        ab.queue = queue;
        SX_REQUIRE(XblAchievementsGetAchievementsForTitleIdAsync(
                       ctx, user_xuid, title_id, XblAchievementType_All, true,
                       XblAchievementOrderBy_DefaultOrder, 0, 0, &ab) == XBL_S_OK);
        XblAchievementsResultHandle res = nullptr;
        SX_REQUIRE(XblAchievementsGetAchievementsForTitleIdResult(&ab, &res) == XBL_S_OK);
        const XblAchievement* list = nullptr;
        size_t count = 0;
        SX_REQUIRE(XblAchievementsResultGetAchievements(res, &list, &count) == XBL_S_OK);
        SX_REQUIRE(count == 3);
        SX_REQUIRE(std::string(list[0].id) == "1" && list[0].percentComplete == 100);
        SX_REQUIRE(std::string(list[1].id) == "3" && list[1].percentComplete == 100);
        SX_REQUIRE(std::string(list[2].id) == "4" && list[2].percentComplete == 100);
        XblAchievementsResultCloseHandle(res);
    }

    // Pagination check: skipItems=1, maxItems=2 -> returns "2", "3" with hasNext=true.
    {
        XAsyncBlock ab{};
        ab.queue = queue;
        SX_REQUIRE(XblAchievementsGetAchievementsForTitleIdAsync(
                       ctx, user_xuid, title_id, XblAchievementType_All, false,
                       XblAchievementOrderBy_DefaultOrder, 1, 2, &ab) == XBL_S_OK);
        XblAchievementsResultHandle res = nullptr;
        SX_REQUIRE(XblAchievementsGetAchievementsForTitleIdResult(&ab, &res) == XBL_S_OK);
        const XblAchievement* list = nullptr;
        size_t count = 0;
        bool has_next = false;
        SX_REQUIRE(XblAchievementsResultGetAchievements(res, &list, &count) == XBL_S_OK);
        SX_REQUIRE(XblAchievementsResultHasNext(res, &has_next) == XBL_S_OK);
        SX_REQUIRE(count == 2 && has_next);
        SX_REQUIRE(std::string(list[0].id) == "2");
        SX_REQUIRE(std::string(list[1].id) == "3");
        XblAchievementsResultCloseHandle(res);
    }

    // Different titleId returns 0 achievements.
    {
        XAsyncBlock ab{};
        ab.queue = queue;
        SX_REQUIRE(XblAchievementsGetAchievementsForTitleIdAsync(
                       ctx, user_xuid, 0xDEADBEEFu, XblAchievementType_All, false,
                       XblAchievementOrderBy_DefaultOrder, 0, 10, &ab) == XBL_S_OK);
        XblAchievementsResultHandle res = nullptr;
        SX_REQUIRE(XblAchievementsGetAchievementsForTitleIdResult(&ab, &res) == XBL_S_OK);
        const XblAchievement* list = nullptr;
        size_t count = 99;
        SX_REQUIRE(XblAchievementsResultGetAchievements(res, &list, &count) == XBL_S_OK);
        SX_REQUIRE(count == 0);
        XblAchievementsResultCloseHandle(res);
    }

    // Presence: set rich presence active -> query -> verify -> set away -> verify.
    {
        const char* tokens[] = {"Map_Neon", "Rank_S"};
        XblPresenceRichPresenceIds rp{};
        std::strncpy(rp.scid, kTestScid, sizeof(rp.scid) - 1);
        rp.presenceId = "PlayingArena";
        rp.presenceTokenIds = tokens;
        rp.presenceTokenIdsCount = 2;

        XAsyncBlock ab_set{};
        ab_set.queue = queue;
        SX_REQUIRE(XblPresenceSetPresenceAsync(ctx, true, &rp, &ab_set) == XBL_S_OK);
        SX_REQUIRE(XAsyncGetStatus(&ab_set, true) == XBL_S_OK);

        XAsyncBlock ab_get{};
        ab_get.queue = queue;
        SX_REQUIRE(XblPresenceGetPresenceAsync(ctx, user_xuid, &ab_get) == XBL_S_OK);
        XblPresenceRecordHandle rec = nullptr;
        SX_REQUIRE(XblPresenceGetPresenceResult(&ab_get, &rec) == XBL_S_OK && rec != nullptr);

        uint64_t rec_xuid = 0;
        XblPresenceUserState state = XblPresenceUserState_Unknown;
        const XblPresenceDeviceRecord* devs = nullptr;
        size_t dev_count = 0;
        SX_REQUIRE(XblPresenceRecordGetXuid(rec, &rec_xuid) == XBL_S_OK && rec_xuid == user_xuid);
        SX_REQUIRE(XblPresenceRecordGetUserState(rec, &state) == XBL_S_OK &&
                   state == XblPresenceUserState_Online);
        SX_REQUIRE(XblPresenceRecordGetDeviceRecords(rec, &devs, &dev_count) == XBL_S_OK);
        SX_REQUIRE(dev_count == 1 && devs != nullptr);
        SX_REQUIRE(devs[0].deviceType == XblPresenceDeviceType_Scarlett);
        SX_REQUIRE(devs[0].titleRecordsCount == 1 && devs[0].titleRecords != nullptr);
        SX_REQUIRE(devs[0].titleRecords[0].titleActive);
        SX_REQUIRE(std::string(devs[0].titleRecords[0].richPresenceString) ==
                   "PlayingArena(Map_Neon, Rank_S)");
        XblPresenceRecordCloseHandle(rec);

        // Mark user inactive -> Away.
        XAsyncBlock ab_away{};
        ab_away.queue = queue;
        SX_REQUIRE(XblPresenceSetPresenceAsync(ctx, false, nullptr, &ab_away) == XBL_S_OK);
        SX_REQUIRE(XAsyncGetStatus(&ab_away, true) == XBL_S_OK);

        XAsyncBlock ab_get2{};
        ab_get2.queue = queue;
        SX_REQUIRE(XblPresenceGetPresenceAsync(ctx, user_xuid, &ab_get2) == XBL_S_OK);
        XblPresenceRecordHandle rec2 = nullptr;
        SX_REQUIRE(XblPresenceGetPresenceResult(&ab_get2, &rec2) == XBL_S_OK);
        SX_REQUIRE(XblPresenceRecordGetUserState(rec2, &state) == XBL_S_OK &&
                   state == XblPresenceUserState_Away);
        XblPresenceRecordCloseHandle(rec2);
    }

    // Profile: verify gamertag & computed gamerscore (3 unlocked * 25 = 75).
    {
        XAsyncBlock ab{};
        ab.queue = queue;
        SX_REQUIRE(XblProfileGetUserProfileAsync(ctx, user_xuid, &ab) == XBL_S_OK);
        XblUserProfile prof{};
        SX_REQUIRE(XblProfileGetUserProfileResult(&ab, &prof) == XBL_S_OK);
        SX_REQUIRE(prof.xboxUserId == user_xuid);
        SX_REQUIRE(std::string(prof.gamerscore) == "75");
        SX_REQUIRE(std::strlen(prof.gamertag) > 0);
        SX_REQUIRE(std::strlen(prof.uniqueModernGamertag) > 0);
    }

    // User Statistics: default value -> write stats -> query with buffer size & too-small retry.
    {
        XAsyncBlock ab{};
        ab.queue = queue;
        SX_REQUIRE(XblUserStatisticsGetSingleUserStatisticAsync(
                       ctx, user_xuid, kTestScid, "UnwrittenStat", &ab) == XBL_S_OK);
        size_t req_size = 0;
        SX_REQUIRE(XblUserStatisticsGetSingleUserStatisticResultSize(&ab, &req_size) == XBL_S_OK);
        SX_REQUIRE(req_size > sizeof(XblUserStatisticsResult));
        std::vector<uint8_t> buf(req_size);
        XblUserStatisticsResult* res = nullptr;
        size_t used = 0;
        SX_REQUIRE(XblUserStatisticsGetSingleUserStatisticResult(
                       &ab, buf.size(), buf.data(), &res, &used) == XBL_S_OK);
        SX_REQUIRE(res != nullptr && used == req_size);
        SX_REQUIRE(res->xboxUserId == user_xuid);
        SX_REQUIRE(res->serviceConfigStatisticsCount == 1);
        SX_REQUIRE(res->serviceConfigStatistics[0].statisticsCount == 1);
        SX_REQUIRE(std::string(res->serviceConfigStatistics[0].statistics[0].value) == "0");
    }

    {
        XblTitleManagedStatistic stats[2]{};
        stats[0].statisticName = "HighScore";
        stats[0].statisticType = XblTitleManagedStatType_Number;
        stats[0].numberValue = 42000.0;
        stats[1].statisticName = "FavoriteMap";
        stats[1].statisticType = XblTitleManagedStatType_String;
        stats[1].stringValue = "ScarlettArena";

        XAsyncBlock ab_w{};
        ab_w.queue = queue;
        SX_REQUIRE(XblTitleManagedStatsWriteAsync(ctx, user_xuid, stats, 2, &ab_w) == XBL_S_OK);
        SX_REQUIRE(XAsyncGetStatus(&ab_w, true) == XBL_S_OK);

        XAsyncBlock ab_r{};
        ab_r.queue = queue;
        SX_REQUIRE(XblUserStatisticsGetSingleUserStatisticAsync(
                       ctx, user_xuid, kTestScid, "HighScore", &ab_r) == XBL_S_OK);
        size_t req_size = 0;
        SX_REQUIRE(XblUserStatisticsGetSingleUserStatisticResultSize(&ab_r, &req_size) == XBL_S_OK);

        // Verify too-small buffer fails with E_NOT_SUFFICIENT_BUFFER and allows retry.
        std::vector<uint8_t> tiny(req_size > 1 ? req_size - 1 : 0);
        XblUserStatisticsResult* res = nullptr;
        SX_REQUIRE(XblUserStatisticsGetSingleUserStatisticResult(
                       &ab_r, tiny.size(), tiny.data(), &res, nullptr) == XGR_E_NOT_SUFFICIENT_BUFFER);

        std::vector<uint8_t> buf(req_size);
        size_t used = 0;
        SX_REQUIRE(XblUserStatisticsGetSingleUserStatisticResult(
                       &ab_r, buf.size(), buf.data(), &res, &used) == XBL_S_OK);
        SX_REQUIRE(res != nullptr && used == req_size);
        SX_REQUIRE(std::string(res->serviceConfigStatistics[0].serviceConfigurationId) == kTestScid);
        SX_REQUIRE(std::string(res->serviceConfigStatistics[0].statistics[0].statisticName) == "HighScore");
        SX_REQUIRE(std::string(res->serviceConfigStatistics[0].statistics[0].value) == "42000");
    }

    // Cleanup XSAPI (clears in-memory caches) and verify persistence across re-init!
    XblContextCloseHandle(ctx);
    ctx = nullptr;
    {
        XAsyncBlock ab_clean{};
        ab_clean.queue = queue;
        SX_REQUIRE(XblCleanupAsync(&ab_clean) == XBL_S_OK);
        SX_REQUIRE(XAsyncGetStatus(&ab_clean, true) == XBL_S_OK);
    }

    // Confirm context creation rejects while uninitialized.
    SX_REQUIRE(XblContextCreateHandle(user, &ctx) == XBL_E_NOT_INITIALIZED);

    // Re-initialize XSAPI with same SCID and verify achievements & stats reload from disk!
    SX_REQUIRE(XblInitialize(&init_args) == XBL_S_OK);
    SX_REQUIRE(XblContextCreateHandle(user, &ctx) == XBL_S_OK && ctx != nullptr);

    {
        XAsyncBlock ab{};
        ab.queue = queue;
        SX_REQUIRE(XblAchievementsGetAchievementsForTitleIdAsync(
                       ctx, user_xuid, title_id, XblAchievementType_All, false,
                       XblAchievementOrderBy_DefaultOrder, 0, 0, &ab) == XBL_S_OK);
        XblAchievementsResultHandle res = nullptr;
        SX_REQUIRE(XblAchievementsGetAchievementsForTitleIdResult(&ab, &res) == XBL_S_OK);
        const XblAchievement* list = nullptr;
        size_t count = 0;
        SX_REQUIRE(XblAchievementsResultGetAchievements(res, &list, &count) == XBL_S_OK);
        SX_REQUIRE(count == 4);
        SX_REQUIRE(std::string(list[0].id) == "1" && list[0].percentComplete == 100 &&
                   list[0].progressState == XblAchievementProgressState_Achieved);
        SX_REQUIRE(std::string(list[1].id) == "2" && list[1].percentComplete == 25 &&
                   list[1].progressState == XblAchievementProgressState_InProgress);
        SX_REQUIRE(std::string(list[2].id) == "3" && list[2].percentComplete == 100);
        SX_REQUIRE(std::string(list[3].id) == "4" && list[3].percentComplete == 100);
        XblAchievementsResultCloseHandle(res);
    }

    {
        XAsyncBlock ab{};
        ab.queue = queue;
        SX_REQUIRE(XblUserStatisticsGetSingleUserStatisticAsync(
                       ctx, user_xuid, kTestScid, "FavoriteMap", &ab) == XBL_S_OK);
        size_t req_size = 0;
        SX_REQUIRE(XblUserStatisticsGetSingleUserStatisticResultSize(&ab, &req_size) == XBL_S_OK);
        std::vector<uint8_t> buf(req_size);
        XblUserStatisticsResult* res = nullptr;
        SX_REQUIRE(XblUserStatisticsGetSingleUserStatisticResult(
                       &ab, buf.size(), buf.data(), &res, nullptr) == XBL_S_OK);
        SX_REQUIRE(res != nullptr);
        SX_REQUIRE(std::string(res->serviceConfigStatistics[0].statistics[0].value) == "ScarlettArena");
    }

    XblContextCloseHandle(ctx);
    {
        XAsyncBlock ab_clean{};
        ab_clean.queue = queue;
        SX_REQUIRE(XblCleanupAsync(&ab_clean) == XBL_S_OK);
        SX_REQUIRE(XAsyncGetStatus(&ab_clean, true) == XBL_S_OK);
    }
    XUserCloseHandle(user);
    XTaskQueueCloseHandle(queue);
}

// ---------------------------------------------------------------------------
// Test 2: WinRT HSTRING & Activation Runtime (winrt_xs.dll)
// ---------------------------------------------------------------------------
void TestWinRtHString() {
    // Null / empty HSTRING semantics.
    SX_HSTRING h_empty = reinterpret_cast<SX_HSTRING>(static_cast<uintptr_t>(0x1234));
    SX_REQUIRE(WindowsCreateString(nullptr, 0, &h_empty) == SX_WINRT_S_OK);
    SX_REQUIRE(h_empty == nullptr);

    const uint16_t empty_buf[1] = {0};
    SX_REQUIRE(WindowsCreateString(empty_buf, 0, &h_empty) == SX_WINRT_S_OK);
    SX_REQUIRE(h_empty == nullptr);
    SX_REQUIRE(WindowsCreateString(nullptr, 4, &h_empty) == SX_WINRT_E_POINTER);

    uint32_t raw_len = 99;
    const uint16_t* raw_empty = WindowsGetStringRawBuffer(nullptr, &raw_len);
    SX_REQUIRE(raw_empty != nullptr && raw_empty[0] == 0 && raw_len == 0);
    SX_REQUIRE(WindowsGetStringLen(nullptr) == 0);
    SX_REQUIRE(WindowsIsStringEmpty(nullptr) == 1);
    SX_REQUIRE(WindowsDeleteString(nullptr) == SX_WINRT_S_OK);

    SX_HSTRING dup_null = reinterpret_cast<SX_HSTRING>(static_cast<uintptr_t>(0x5678));
    SX_REQUIRE(WindowsDuplicateString(nullptr, &dup_null) == SX_WINRT_S_OK);
    SX_REQUIRE(dup_null == nullptr);

    // Heap HSTRING creation, inspection, duplication, refcount lifetime.
    const std::u16string hello = u"Windows.Xbox";
    SX_HSTRING h1 = MakeHString(hello);
    SX_REQUIRE(h1 != nullptr);
    SX_REQUIRE(WindowsGetStringLen(h1) == hello.size());
    SX_REQUIRE(WindowsIsStringEmpty(h1) == 0);
    SX_REQUIRE(ToU16String(h1) == hello);

    SX_HSTRING h1_dup = nullptr;
    SX_REQUIRE(WindowsDuplicateString(h1, &h1_dup) == SX_WINRT_S_OK);
    SX_REQUIRE(h1_dup == h1);
    SX_REQUIRE(WindowsDeleteString(h1) == SX_WINRT_S_OK);
    // Duplicate remains valid after first handle deleted.
    SX_REQUIRE(ToU16String(h1_dup) == hello);
    SX_REQUIRE(WindowsDeleteString(h1_dup) == SX_WINRT_S_OK);

    // Fast-pass HSTRING reference (zero-alloc + heap promotion on Duplicate).
    const uint16_t non_terminated[3] = {'a', 'b', 'c'};
    SX_HSTRING_HEADER bad_hdr{};
    SX_HSTRING bad_ref = nullptr;
    SX_REQUIRE(WindowsCreateStringReference(non_terminated, 2, &bad_hdr, &bad_ref) ==
               SX_WINRT_E_INVALIDARG);

    uint16_t stack_text[] = {'S', 'c', 'a', 'r', 'l', 'e', 't', 't', 'X', 0};
    SX_HSTRING_HEADER hdr{};
    SX_HSTRING href = nullptr;
    SX_REQUIRE(WindowsCreateStringReference(stack_text, 9, &hdr, &href) == SX_WINRT_S_OK);
    SX_REQUIRE(href != nullptr);
    uint32_t ref_len = 0;
    SX_REQUIRE(WindowsGetStringRawBuffer(href, &ref_len) == stack_text && ref_len == 9);

    // Duplicate of fast-pass reference must produce an independent heap copy.
    SX_HSTRING h_promoted = nullptr;
    SX_REQUIRE(WindowsDuplicateString(href, &h_promoted) == SX_WINRT_S_OK);
    SX_REQUIRE(h_promoted != nullptr && h_promoted != href);
    SX_REQUIRE(WindowsDeleteString(href) == SX_WINRT_S_OK);
    std::memset(stack_text, 0, sizeof(stack_text));
    std::memset(&hdr, 0, sizeof(hdr));
    SX_REQUIRE(ToU16String(h_promoted) == u"ScarlettX");
    SX_REQUIRE(WindowsDeleteString(h_promoted) == SX_WINRT_S_OK);

    // Ordinal comparison (including null/empty cases).
    SX_HSTRING ha = MakeHString(u"Alpha");
    SX_HSTRING hb = MakeHString(u"Beta");
    SX_HSTRING ha2 = MakeHString(u"Alpha");
    SX_HSTRING ha_long = MakeHString(u"AlphaBeta");
    int32_t cmp = 42;
    SX_REQUIRE(WindowsCompareStringOrdinal(nullptr, nullptr, &cmp) == SX_WINRT_S_OK && cmp == 0);
    SX_REQUIRE(WindowsCompareStringOrdinal(nullptr, ha, &cmp) == SX_WINRT_S_OK && cmp == -1);
    SX_REQUIRE(WindowsCompareStringOrdinal(ha, nullptr, &cmp) == SX_WINRT_S_OK && cmp == 1);
    SX_REQUIRE(WindowsCompareStringOrdinal(ha, ha2, &cmp) == SX_WINRT_S_OK && cmp == 0);
    SX_REQUIRE(WindowsCompareStringOrdinal(ha, hb, &cmp) == SX_WINRT_S_OK && cmp == -1);
    SX_REQUIRE(WindowsCompareStringOrdinal(hb, ha, &cmp) == SX_WINRT_S_OK && cmp == 1);
    SX_REQUIRE(WindowsCompareStringOrdinal(ha, ha_long, &cmp) == SX_WINRT_S_OK && cmp == -1);

    // Concat, Substring, EmbeddedNull.
    SX_HSTRING h_cat = nullptr;
    SX_REQUIRE(WindowsConcatString(ha, hb, &h_cat) == SX_WINRT_S_OK);
    SX_REQUIRE(ToU16String(h_cat) == u"AlphaBeta");
    SX_HSTRING h_sub = nullptr;
    SX_REQUIRE(WindowsSubstringWithSpecifiedLength(h_cat, 5, 4, &h_sub) == SX_WINRT_S_OK);
    SX_REQUIRE(ToU16String(h_sub) == u"Beta");
    SX_HSTRING h_oob = nullptr;
    SX_REQUIRE(WindowsSubstring(h_cat, 100, &h_oob) == SX_WINRT_E_BOUNDS);

    const uint16_t emb_buf[4] = {'A', 0, 'B', 0};
    SX_HSTRING h_emb = nullptr;
    SX_REQUIRE(WindowsCreateString(emb_buf, 3, &h_emb) == SX_WINRT_S_OK);
    int32_t has_null = 0;
    SX_REQUIRE(WindowsStringHasEmbeddedNull(h_emb, &has_null) == SX_WINRT_S_OK && has_null == 1);
    SX_REQUIRE(WindowsStringHasEmbeddedNull(ha, &has_null) == SX_WINRT_S_OK && has_null == 0);

    SX_REQUIRE(WindowsDeleteString(h_emb) == SX_WINRT_S_OK);
    SX_REQUIRE(WindowsDeleteString(h_sub) == SX_WINRT_S_OK);
    SX_REQUIRE(WindowsDeleteString(h_cat) == SX_WINRT_S_OK);
    SX_REQUIRE(WindowsDeleteString(ha_long) == SX_WINRT_S_OK);
    SX_REQUIRE(WindowsDeleteString(ha2) == SX_WINRT_S_OK);
    SX_REQUIRE(WindowsDeleteString(hb) == SX_WINRT_S_OK);
    SX_REQUIRE(WindowsDeleteString(ha) == SX_WINRT_S_OK);
}

void TestWinRtActivation() {
    // Calls before RoInitialize must return CO_E_NOTINITIALIZED.
    SX_HSTRING cls_analytics = MakeHString(u"Windows.System.Profile.AnalyticsInfo");
    void* dummy = nullptr;
    SX_REQUIRE(RoActivateInstance(cls_analytics, &dummy) == SX_WINRT_CO_E_NOTINITIALIZED);
    SX_REQUIRE(RoGetActivationFactory(cls_analytics, &SX_IID_IActivationFactory, &dummy) ==
               SX_WINRT_CO_E_NOTINITIALIZED);

    // Per-thread refcounted RoInitialize / RoUninitialize & mode mismatch check.
    SX_REQUIRE(RoInitialize(99) == SX_WINRT_E_INVALIDARG);
    SX_REQUIRE(RoInitialize(SX_RO_INIT_MULTITHREADED) == SX_WINRT_S_OK);
    SX_REQUIRE(RoInitialize(SX_RO_INIT_MULTITHREADED) == SX_WINRT_S_FALSE);
    SX_REQUIRE(RoInitialize(SX_RO_INIT_SINGLETHREADED) == SX_WINRT_RPC_E_CHANGED_MODE);

    // Separate thread has independent apartment state (can init STA cleanly).
    std::atomic<bool> worker_ok{false};
    std::thread worker([&]() {
        bool ok = true;
        void* p = nullptr;
        ok = ok && (RoActivateInstance(cls_analytics, &p) == SX_WINRT_CO_E_NOTINITIALIZED);
        ok = ok && (RoInitialize(SX_RO_INIT_SINGLETHREADED) == SX_WINRT_S_OK);
        ok = ok && (RoInitialize(SX_RO_INIT_MULTITHREADED) == SX_WINRT_RPC_E_CHANGED_MODE);
        RoUninitialize();
        ok = ok && (RoActivateInstance(cls_analytics, &p) == SX_WINRT_CO_E_NOTINITIALIZED);
        worker_ok.store(ok);
    });
    worker.join();
    SX_REQUIRE(worker_ok.load());

    // Unwind nested refcount: still initialized after 1st RoUninitialize, uninit after 2nd.
    RoUninitialize();
    SX_REQUIRE(RoInitialize(SX_RO_INIT_MULTITHREADED) == SX_WINRT_S_FALSE);
    RoUninitialize();
    RoUninitialize();
    SX_REQUIRE(RoActivateInstance(cls_analytics, &dummy) == SX_WINRT_CO_E_NOTINITIALIZED);

    // Re-initialize MTA for class activation tests.
    SX_REQUIRE(RoInitialize(SX_RO_INIT_MULTITHREADED) == SX_WINRT_S_OK);

    // 1. Windows.System.Profile.AnalyticsInfo (both Statics and ActivateInstance).
    {
        SX_IAnalyticsInfoStatics* statics = nullptr;
        SX_REQUIRE(RoGetActivationFactory(
                       cls_analytics, &SX_IID_IAnalyticsInfoStatics,
                       reinterpret_cast<void**>(&statics)) == SX_WINRT_S_OK);
        SX_REQUIRE(statics != nullptr);

        SX_IAnalyticsVersionInfo* ver = nullptr;
        SX_REQUIRE(statics->lpVtbl->get_VersionInfo(statics, &ver) == SX_WINRT_S_OK && ver != nullptr);

        SX_HSTRING family = nullptr;
        SX_REQUIRE(ver->lpVtbl->get_DeviceFamily(ver, &family) == SX_WINRT_S_OK);
        SX_REQUIRE(ToU16String(family) == u"Windows.Xbox");
        WindowsDeleteString(family);

        SX_HSTRING form = nullptr;
        SX_REQUIRE(statics->lpVtbl->get_DeviceForm(statics, &form) == SX_WINRT_S_OK);
        SX_REQUIRE(ToU16String(form) == u"Xbox Series X");
        WindowsDeleteString(form);

        ver->lpVtbl->Release(ver);
        statics->lpVtbl->Release(statics);

        SX_IInspectable* inst = nullptr;
        SX_REQUIRE(RoActivateInstance(cls_analytics, reinterpret_cast<void**>(&inst)) == SX_WINRT_S_OK);
        SX_HSTRING rcn = nullptr;
        SX_REQUIRE(inst->lpVtbl->GetRuntimeClassName(inst, &rcn) == SX_WINRT_S_OK);
        SX_REQUIRE(ToU16String(rcn) == u"Windows.System.Profile.AnalyticsInfo");
        WindowsDeleteString(rcn);
        inst->lpVtbl->Release(inst);
    }

    // 2. Windows.Storage.ApplicationData (LocalFolder under SCARLETTX_SAVE_ROOT).
    {
        SX_HSTRING cls_appdata = MakeHString(u"Windows.Storage.ApplicationData");
        SX_IApplicationDataStatics* statics = nullptr;
        SX_REQUIRE(RoGetActivationFactory(
                       cls_appdata, &SX_IID_IApplicationDataStatics,
                       reinterpret_cast<void**>(&statics)) == SX_WINRT_S_OK);
        SX_IApplicationData* current = nullptr;
        SX_REQUIRE(statics->lpVtbl->get_Current(statics, &current) == SX_WINRT_S_OK && current != nullptr);

        SX_HSTRING rcn = nullptr;
        SX_REQUIRE(current->lpVtbl->GetRuntimeClassName(current, &rcn) == SX_WINRT_S_OK);
        SX_REQUIRE(ToU16String(rcn) == u"Windows.Storage.ApplicationData");
        WindowsDeleteString(rcn);

        SX_IStorageItem* local_folder = nullptr;
        SX_REQUIRE(current->lpVtbl->get_LocalFolder(current, &local_folder) == SX_WINRT_S_OK &&
                   local_folder != nullptr);

        SX_HSTRING fname = nullptr;
        SX_HSTRING fpath = nullptr;
        SX_REQUIRE(local_folder->lpVtbl->get_Name(local_folder, &fname) == SX_WINRT_S_OK);
        SX_REQUIRE(local_folder->lpVtbl->get_Path(local_folder, &fpath) == SX_WINRT_S_OK);
        SX_REQUIRE(ToU16String(fname) == u"LocalState");
        const fs::path local_fs_path(ToU16String(fpath));
        SX_REQUIRE(fs::exists(local_fs_path));

        WindowsDeleteString(fname);
        WindowsDeleteString(fpath);
        local_folder->lpVtbl->Release(local_folder);
        current->lpVtbl->Release(current);
        statics->lpVtbl->Release(statics);
        WindowsDeleteString(cls_appdata);
    }

    // 3. Windows.Gaming.Input.Gamepad.
    {
        SX_HSTRING cls_gamepad = MakeHString(u"Windows.Gaming.Input.Gamepad");
        SX_IInspectable* inst = nullptr;
        SX_REQUIRE(RoActivateInstance(cls_gamepad, reinterpret_cast<void**>(&inst)) == SX_WINRT_S_OK);
        SX_HSTRING rcn = nullptr;
        SX_REQUIRE(inst->lpVtbl->GetRuntimeClassName(inst, &rcn) == SX_WINRT_S_OK);
        SX_REQUIRE(ToU16String(rcn) == u"Windows.Gaming.Input.Gamepad");
        WindowsDeleteString(rcn);

        SX_IGamepad* gp = nullptr;
        SX_REQUIRE(inst->lpVtbl->QueryInterface(inst, &SX_IID_IGamepad, reinterpret_cast<void**>(&gp)) ==
                   SX_WINRT_S_OK);
        SX_GamepadVibration vib{0.5, 0.75, 0.25, 0.1};
        SX_REQUIRE(gp->lpVtbl->put_Vibration(gp, vib) == SX_WINRT_S_OK);
        SX_GamepadVibration vib_out{};
        SX_REQUIRE(gp->lpVtbl->get_Vibration(gp, &vib_out) == SX_WINRT_S_OK);
        SX_REQUIRE(vib_out.LeftMotor == 0.5 && vib_out.RightMotor == 0.75);
        SX_GamepadReading reading{};
        SX_REQUIRE(gp->lpVtbl->GetCurrentReading(gp, &reading) == SX_WINRT_S_OK);
        SX_REQUIRE(reading.Timestamp > 0);

        gp->lpVtbl->Release(gp);
        inst->lpVtbl->Release(inst);
        WindowsDeleteString(cls_gamepad);
    }

    // 4. Windows.Globalization.Language (default en-US + custom CreateLanguage).
    {
        SX_HSTRING cls_lang = MakeHString(u"Windows.Globalization.Language");
        SX_IInspectable* inst = nullptr;
        SX_REQUIRE(RoActivateInstance(cls_lang, reinterpret_cast<void**>(&inst)) == SX_WINRT_S_OK);
        SX_HSTRING rcn = nullptr;
        SX_REQUIRE(inst->lpVtbl->GetRuntimeClassName(inst, &rcn) == SX_WINRT_S_OK);
        SX_REQUIRE(ToU16String(rcn) == u"Windows.Globalization.Language");
        WindowsDeleteString(rcn);

        SX_ILanguage* lang = nullptr;
        SX_REQUIRE(inst->lpVtbl->QueryInterface(inst, &SX_IID_ILanguage, reinterpret_cast<void**>(&lang)) ==
                   SX_WINRT_S_OK);
        SX_HSTRING tag = nullptr;
        SX_REQUIRE(lang->lpVtbl->get_LanguageTag(lang, &tag) == SX_WINRT_S_OK);
        SX_REQUIRE(ToU16String(tag) == u"en-US");
        WindowsDeleteString(tag);
        lang->lpVtbl->Release(lang);
        inst->lpVtbl->Release(inst);

        SX_ILanguageFactory* lf = nullptr;
        SX_REQUIRE(RoGetActivationFactory(cls_lang, &SX_IID_ILanguageFactory, reinterpret_cast<void**>(&lf)) ==
                   SX_WINRT_S_OK);
        SX_HSTRING jp_in = MakeHString(u"ja-JP");
        SX_ILanguage* jp_lang = nullptr;
        SX_REQUIRE(lf->lpVtbl->CreateLanguage(lf, jp_in, &jp_lang) == SX_WINRT_S_OK && jp_lang != nullptr);
        SX_HSTRING jp_out = nullptr;
        SX_REQUIRE(jp_lang->lpVtbl->get_LanguageTag(jp_lang, &jp_out) == SX_WINRT_S_OK);
        SX_REQUIRE(ToU16String(jp_out) == u"ja-JP");
        WindowsDeleteString(jp_out);
        WindowsDeleteString(jp_in);
        jp_lang->lpVtbl->Release(jp_lang);
        lf->lpVtbl->Release(lf);
        WindowsDeleteString(cls_lang);
    }

    // 5. Unknown class must return REGDB_E_CLASSNOTREG.
    {
        SX_HSTRING unknown = MakeHString(u"ScarlettX.NonExistent.Class");
        void* p = nullptr;
        SX_REQUIRE(RoGetActivationFactory(unknown, &SX_IID_IActivationFactory, &p) ==
                   SX_WINRT_REGDB_E_CLASSNOTREG);
        SX_REQUIRE(RoActivateInstance(unknown, &p) == SX_WINRT_REGDB_E_CLASSNOTREG);
        WindowsDeleteString(unknown);
    }

    // 6. Custom runtime factory registration & revocation.
    {
        SX_HSTRING custom_cls = MakeHString(u"ScarlettX.Test.CustomWidget");
        SX_HSTRING cls_ids[1] = {custom_cls};
        SX_PFNGETACTIVATIONFACTORY cbs[1] = {&GetCustomWidgetFactory};
        uint64_t cookie = 0;

        SX_REQUIRE(RoRegisterActivationFactories(cls_ids, cbs, 1, &cookie) == SX_WINRT_S_OK);
        SX_REQUIRE(cookie != 0);

        SX_IInspectable* widget = nullptr;
        SX_REQUIRE(RoActivateInstance(custom_cls, reinterpret_cast<void**>(&widget)) == SX_WINRT_S_OK);
        SX_REQUIRE(widget != nullptr && g_custom_widget_live.load() == 1);

        SX_HSTRING rcn = nullptr;
        SX_REQUIRE(widget->lpVtbl->GetRuntimeClassName(widget, &rcn) == SX_WINRT_S_OK);
        SX_REQUIRE(ToU16String(rcn) == u"ScarlettX.Test.CustomWidget");
        WindowsDeleteString(rcn);

        SX_TrustLevel tl = SX_TrustLevel_BaseTrust;
        SX_REQUIRE(widget->lpVtbl->GetTrustLevel(widget, &tl) == SX_WINRT_S_OK);
        SX_REQUIRE(tl == SX_TrustLevel_PartialTrust);

        widget->lpVtbl->Release(widget);
        SX_REQUIRE(g_custom_widget_live.load() == 0);
        SX_REQUIRE(g_custom_factory_live.load() == 0);

        RoRevokeActivationFactories(cookie);

        void* revoked_inst = nullptr;
        SX_REQUIRE(RoActivateInstance(custom_cls, &revoked_inst) == SX_WINRT_REGDB_E_CLASSNOTREG);
        SX_REQUIRE(RoGetActivationFactory(custom_cls, &SX_IID_IActivationFactory, &revoked_inst) ==
                   SX_WINRT_REGDB_E_CLASSNOTREG);
        WindowsDeleteString(custom_cls);
    }

    WindowsDeleteString(cls_analytics);
    RoUninitialize();
}

}  // namespace

int main() {
    TestXsapi();
    TestWinRtHString();
    TestWinRtActivation();
    std::cout << "[PASS] test_xsapi_winrt (Microsoft.Xbox.Services.GDK.C + winrt_xs)\n";
    return 0;
}
