#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "scarlettx/gdk/xgameruntime_abi.h"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "[test_xgameruntime_abi] FAILED: " #cond                \
                      << " at line " << __LINE__ << "\n";                        \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace {

std::atomic<int> g_manual_work_hits{0};
std::atomic<int> g_delayed_hits{0};
std::atomic<int> g_monitor_hits{0};
std::atomic<int> g_async_work_hits{0};
std::atomic<int> g_async_comp_hits{0};
std::atomic<int> g_term_cb_hits{0};

std::atomic<int> g_custom_allocs{0};
std::atomic<int> g_custom_frees{0};

void SCARLETTX_GDK_CALLBACK ManualWorkCb(void* ctx, bool canceled) {
    if (!canceled && ctx) {
        auto* val = static_cast<int*>(ctx);
        *val += 10;
        g_manual_work_hits.fetch_add(1);
    }
}

void SCARLETTX_GDK_CALLBACK DelayedWorkCb(void* /*ctx*/, bool canceled) {
    if (!canceled) {
        g_delayed_hits.fetch_add(1);
    }
}

void SCARLETTX_GDK_CALLBACK MonitorCb(void* /*ctx*/, XTaskQueueHandle /*q*/, XTaskQueuePort /*port*/) {
    g_monitor_hits.fetch_add(1);
}

void SCARLETTX_GDK_CALLBACK TerminatedCb(void* /*ctx*/) {
    g_term_cb_hits.fetch_add(1);
}

XGameResult SCARLETTX_GDK_CALLBACK AsyncWorkFn(XAsyncBlock* block) {
    g_async_work_hits.fetch_add(1);
    if (block && block->context) {
        *static_cast<int*>(block->context) = 42;
    }
    return XGR_S_OK;
}

void SCARLETTX_GDK_CALLBACK AsyncCompletionFn(XAsyncBlock* /*block*/) {
    g_async_comp_hits.fetch_add(1);
}

bool SCARLETTX_GDK_CALLBACK EnumContainersCb(const XGameSaveContainerInfo* info, void* ctx) {
    auto* names = static_cast<std::vector<std::string>*>(ctx);
    if (info && info->name) {
        names->emplace_back(info->name);
    }
    return true;
}

bool SCARLETTX_GDK_CALLBACK EnumBlobsCb(const XGameSaveBlobInfo* info, void* ctx) {
    auto* names = static_cast<std::vector<std::string>*>(ctx);
    if (info && info->name) {
        names->emplace_back(info->name);
    }
    return true;
}

struct ReentrantEnum {
    XGameSaveProviderHandle provider = nullptr;
    int quota_calls = 0;
};

bool SCARLETTX_GDK_CALLBACK ReentrantContainersCb(const XGameSaveContainerInfo* /*info*/, void* ctx) {
    auto* reentrant = static_cast<ReentrantEnum*>(ctx);
    int64_t quota = 0;
    if (XGameSaveGetRemainingQuota(reentrant->provider, &quota) == XGR_S_OK) {
        ++reentrant->quota_calls;
    }
    return true;
}

bool SCARLETTX_GDK_CALLBACK ReentrantBlobsCb(const XGameSaveBlobInfo* /*info*/, void* ctx) {
    auto* reentrant = static_cast<ReentrantEnum*>(ctx);
    int64_t quota = 0;
    if (XGameSaveGetRemainingQuota(reentrant->provider, &quota) == XGR_S_OK) {
        ++reentrant->quota_calls;
    }
    return true;
}

void* SCARLETTX_GDK_CALLBACK CustomAlloc(size_t sz, uint64_t attr) {
    g_custom_allocs.fetch_add(1);
    return XMemAllocDefault(sz, attr);
}

void SCARLETTX_GDK_CALLBACK CustomFree(void* ptr, uint64_t attr) {
    g_custom_frees.fetch_add(1);
    XMemFreeDefault(ptr, attr);
}

}  // namespace

int main() {
    // Isolate GameSave directory for this test run.
    namespace fs = std::filesystem;
    fs::path save_root = fs::temp_directory_path() / "scarlettx_test_gamesave_abi";
    std::error_code ec;
    fs::remove_all(save_root, ec);
#if defined(_WIN32)
    _putenv_s("SCARLETTX_SAVE_ROOT", save_root.string().c_str());
#else
    setenv("SCARLETTX_SAVE_ROOT", save_root.string().c_str(), 1);
#endif

    // 1. Runtime init + feature probes + title ID
    CHECK(XGameRuntimeInitialize() == XGR_S_OK);

    bool avail = false;
    CHECK(XGameRuntimeIsFeatureAvailable(XGameRuntimeFeature_XTaskQueue, &avail) == XGR_S_OK && avail);
    CHECK(XGameRuntimeIsFeatureAvailable(XGameRuntimeFeature_XAsync, &avail) == XGR_S_OK && avail);
    CHECK(XGameRuntimeIsFeatureAvailable(XGameRuntimeFeature_XUser, &avail) == XGR_S_OK && avail);
    CHECK(XGameRuntimeIsFeatureAvailable(XGameRuntimeFeature_XGameSave, &avail) == XGR_S_OK && avail);
    CHECK(XGameRuntimeIsFeatureAvailable(XGameRuntimeFeature_XMem, &avail) == XGR_S_OK && avail);

    uint32_t title_id = 0;
    CHECK(XGameGetXboxTitleId(&title_id) == XGR_S_OK && title_id != 0);

    // 2. XSystem
    CHECK(XSystemGetDeviceType() == 7u);  // XboxScarlettAnaconda
    XSystemAnalyticsInfo analytics = XSystemGetAnalyticsInfo();
    CHECK(analytics.osVersion.major == 10);
    CHECK(std::strcmp(analytics.family, "Xbox") == 0);
    CHECK(std::strcmp(analytics.form, "Xbox Series X") == 0);

    XSystemRuntimeInfo rt_info = XSystemGetRuntimeInfo();
    CHECK(rt_info.runtimeVersion.major == 10);

    char buf[128]{};
    size_t used = 0;
    CHECK(XSystemGetAppSpecificDeviceId(4, buf, &used) == XGR_E_NOT_SUFFICIENT_BUFFER);
    CHECK(used > 4);
    CHECK(XSystemGetAppSpecificDeviceId(sizeof(buf), buf, &used) == XGR_S_OK);
    CHECK(std::strlen(buf) > 0);

    CHECK(XSystemGetConsoleId(sizeof(buf), buf, &used) == XGR_S_OK);
    CHECK(std::strlen(buf) > 0);

    CHECK(XSystemGetXboxLiveSandboxId(sizeof(buf), buf, &used) == XGR_S_OK);
    CHECK(std::strcmp(buf, "RETAIL") == 0);

    // 3. XTaskQueue (manual + monitor + delayed + duplicate + terminate)
    XTaskQueueHandle manual_q = nullptr;
    CHECK(XTaskQueueCreate(XTaskQueueDispatchMode_Manual,
                           XTaskQueueDispatchMode_Manual,
                           &manual_q) == XGR_S_OK);
    CHECK(manual_q != nullptr);
    CHECK(XSystemIsHandleValid(manual_q));

    XTaskQueueRegistrationToken mon_token{};
    CHECK(XTaskQueueRegisterMonitor(manual_q, nullptr, MonitorCb, &mon_token) == XGR_S_OK);

    int counter = 5;
    CHECK(XTaskQueueSubmitCallback(manual_q, XTaskQueuePort_Work, &counter, ManualWorkCb) == XGR_S_OK);
    CHECK(g_monitor_hits.load() == 1);
    CHECK(XTaskQueueDispatch(manual_q, XTaskQueuePort_Work, 0) == true);
    CHECK(counter == 15);
    CHECK(g_manual_work_hits.load() == 1);
    CHECK(XTaskQueueDispatch(manual_q, XTaskQueuePort_Work, 0) == false);

    CHECK(XTaskQueueSubmitDelayedCallback(manual_q, XTaskQueuePort_Work, 25, nullptr, DelayedWorkCb) == XGR_S_OK);
    CHECK(XTaskQueueDispatch(manual_q, XTaskQueuePort_Work, 500) == true);
    CHECK(g_delayed_hits.load() == 1);

    XTaskQueueUnregisterMonitor(manual_q, mon_token);

    XTaskQueueHandle dup_q = nullptr;
    CHECK(XTaskQueueDuplicateHandle(manual_q, &dup_q) == XGR_S_OK && dup_q == manual_q);
    XTaskQueueCloseHandle(dup_q);
    CHECK(XSystemIsHandleValid(manual_q));  // still alive (refcount 1)

    CHECK(XTaskQueueTerminate(manual_q, true, nullptr, TerminatedCb) == XGR_S_OK);
    CHECK(g_term_cb_hits.load() == 1);
    XTaskQueueCloseHandle(manual_q);
    CHECK(!XSystemIsHandleValid(manual_q));

    // Process default queue
    XTaskQueueHandle proc_q = nullptr;
    CHECK(XTaskQueueGetCurrentProcessTaskQueue(&proc_q) == true && proc_q != nullptr);
    XTaskQueueCloseHandle(proc_q);

    // 4. XAsyncRun on ThreadPool queue
    XTaskQueueHandle tp_q = nullptr;
    CHECK(XTaskQueueCreate(XTaskQueueDispatchMode_ThreadPool,
                           XTaskQueueDispatchMode_Manual,
                           &tp_q) == XGR_S_OK);

    int async_result_val = 0;
    XAsyncBlock async_block{};
    async_block.queue = tp_q;
    async_block.context = &async_result_val;
    async_block.callback = AsyncCompletionFn;
    CHECK(XAsyncRun(&async_block, AsyncWorkFn) == XGR_S_OK);
    CHECK(XAsyncGetStatus(&async_block, true) == XGR_S_OK);
    CHECK(async_result_val == 42);
    CHECK(g_async_work_hits.load() == 1);
    CHECK(XTaskQueueDispatch(tp_q, XTaskQueuePort_Completion, 500) == true);
    CHECK(g_async_comp_hits.load() == 1);

    // 5. XUser
    XAsyncBlock user_async{};
    user_async.queue = tp_q;
    CHECK(XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &user_async) == XGR_S_OK);
    XUserHandle user = nullptr;
    CHECK(XUserAddResult(&user_async, &user) == XGR_S_OK && user != nullptr);
    CHECK(XSystemIsHandleValid(user));

    uint64_t xuid = 0;
    CHECK(XUserGetId(user, &xuid) == XGR_S_OK && xuid != 0);
    XUserLocalId local_id{};
    CHECK(XUserGetLocalId(user, &local_id) == XGR_S_OK && local_id.value != 0);

    char gt[64]{};
    size_t gt_used = 0;
    CHECK(XUserGetGamertag(user, XUserGamertagComponent_Classic, 2, gt, &gt_used) == XGR_E_NOT_SUFFICIENT_BUFFER);
    CHECK(XUserGetGamertag(user, XUserGamertagComponent_Classic, sizeof(gt), gt, &gt_used) == XGR_S_OK);
    CHECK(std::strlen(gt) > 0);
    CHECK(XUserGetGamertag(user, XUserGamertagComponent_UniqueModern, sizeof(gt), gt, &gt_used) == XGR_S_OK);
    CHECK(std::strchr(gt, '#') != nullptr);

    XUserState ustate = XUserState_SignedOut;
    CHECK(XUserGetState(user, &ustate) == XGR_S_OK && ustate == XUserState_SignedIn);
    XUserAgeGroup age = XUserAgeGroup_Unknown;
    CHECK(XUserGetAgeGroup(user, &age) == XGR_S_OK && age == XUserAgeGroup_Adult);
    bool is_guest = true;
    CHECK(XUserGetIsGuest(user, &is_guest) == XGR_S_OK && !is_guest);
    uint32_t max_users = 0;
    CHECK(XUserGetMaxUsers(&max_users) == XGR_S_OK && max_users >= 1);
    bool has_priv = false;
    XUserPrivilegeDenyReason deny = XUserPrivilegeDenyReason_Unknown;
    CHECK(XUserCheckPrivilege(user, XUserPrivilegeOptions_None, XUserPrivilege_Multiplayer, &has_priv, &deny) == XGR_S_OK);
    CHECK(has_priv && deny == XUserPrivilegeDenyReason_None);

    // 6. XGameSave (async init, write, sync + async read, enumerate, delete)
    XAsyncBlock save_init_async{};
    save_init_async.queue = tp_q;
    CHECK(XGameSaveInitializeProviderAsync(user, "00000000-0000-0000-0000-00005ca00001", false, &save_init_async) == XGR_S_OK);
    XGameSaveProviderHandle provider = nullptr;
    CHECK(XGameSaveInitializeProviderResult(&save_init_async, &provider) == XGR_S_OK && provider != nullptr);

    int64_t quota = 0;
    CHECK(XGameSaveGetRemainingQuota(provider, &quota) == XGR_S_OK && quota > 0);

    XGameSaveContainerHandle container = nullptr;
    CHECK(XGameSaveCreateContainer(provider, "slot_0", &container) == XGR_S_OK && container != nullptr);

    XGameSaveUpdateHandle update = nullptr;
    CHECK(XGameSaveCreateUpdate(container, "Slot 0", &update) == XGR_S_OK && update != nullptr);
    const std::string payload = "ScarlettX_SaveData_v1_HP=100_LV=99";
    CHECK(XGameSaveSubmitBlobWrite(update, "profile.dat",
                                   reinterpret_cast<const uint8_t*>(payload.data()),
                                   payload.size()) == XGR_S_OK);

    XAsyncBlock submit_async{};
    submit_async.queue = tp_q;
    CHECK(XGameSaveSubmitUpdateAsync(update, &submit_async) == XGR_S_OK);
    CHECK(XGameSaveSubmitUpdateResult(&submit_async) == XGR_S_OK);
    XGameSaveCloseUpdate(update);

    // Enumerate containers & blobs
    std::vector<std::string> containers;
    CHECK(XGameSaveEnumerateContainerInfo(provider, &containers, EnumContainersCb) == XGR_S_OK);
    CHECK(containers.size() == 1 && containers[0] == "slot_0");

    std::vector<std::string> blobs;
    CHECK(XGameSaveEnumerateBlobInfo(container, &blobs, EnumBlobsCb) == XGR_S_OK);
    CHECK(blobs.size() == 1 && blobs[0] == "profile.dat");

    // Enumeration callbacks may call back into XGameSave
    ReentrantEnum reentrant{provider};
    CHECK(XGameSaveEnumerateContainerInfo(provider, &reentrant, ReentrantContainersCb) == XGR_S_OK);
    CHECK(reentrant.quota_calls == 1);
    CHECK(XGameSaveEnumerateBlobInfo(container, &reentrant, ReentrantBlobsCb) == XGR_S_OK);
    CHECK(reentrant.quota_calls == 2);

    // Sync read
    const char* read_names[] = {"profile.dat"};
    uint32_t read_count = 1;
    std::vector<uint8_t> read_buf(1024, 0);
    auto* blob_out = reinterpret_cast<XGameSaveBlob*>(read_buf.data());
    CHECK(XGameSaveReadBlobData(container, read_names, &read_count, read_buf.size(), blob_out) == XGR_S_OK);
    CHECK(read_count == 1);
    CHECK(blob_out[0].info.size == payload.size());
    CHECK(std::memcmp(blob_out[0].data, payload.data(), payload.size()) == 0);

    // Async read
    XAsyncBlock read_async{};
    read_async.queue = tp_q;
    CHECK(XGameSaveReadBlobDataAsync(container, read_names, 1, &read_async) == XGR_S_OK);
    CHECK(XAsyncGetStatus(&read_async, true) == XGR_S_OK);
    size_t req_size = 0;
    CHECK(XAsyncGetResultSize(&read_async, &req_size) == XGR_S_OK && req_size >= sizeof(XGameSaveBlob) + payload.size());
    std::vector<uint8_t> async_read_buf(req_size, 0);
    uint32_t async_read_count = 0;
    auto* async_blob_out = reinterpret_cast<XGameSaveBlob*>(async_read_buf.data());
    CHECK(XGameSaveReadBlobDataResult(&read_async, async_read_buf.size(), async_blob_out, &async_read_count) == XGR_S_OK);
    CHECK(async_read_count == 1);
    CHECK(async_blob_out[0].info.size == payload.size());
    CHECK(std::memcmp(async_blob_out[0].data, payload.data(), payload.size()) == 0);

    // Blob delete + verify empty enumeration
    XGameSaveUpdateHandle del_update = nullptr;
    CHECK(XGameSaveCreateUpdate(container, "Delete blob", &del_update) == XGR_S_OK);
    CHECK(XGameSaveSubmitBlobDelete(del_update, "profile.dat") == XGR_S_OK);
    CHECK(XGameSaveSubmitUpdate(del_update) == XGR_S_OK);
    XGameSaveCloseUpdate(del_update);

    std::vector<std::string> blobs_after_del;
    CHECK(XGameSaveEnumerateBlobInfo(container, &blobs_after_del, EnumBlobsCb) == XGR_S_OK);
    CHECK(blobs_after_del.empty());

    XGameSaveCloseContainer(container);
    CHECK(XGameSaveDeleteContainer(provider, "slot_0") == XGR_S_OK);
    XGameSaveCloseProvider(provider);

    // 7. XMem hooks & VirtualAlloc
    XMemAllocFunction* orig_alloc = nullptr;
    XMemFreeFunction* orig_free = nullptr;
    CHECK(XMemGetAllocationHooks(&orig_alloc, &orig_free) == XGR_S_OK);
    CHECK(orig_alloc != nullptr && orig_free != nullptr);

    CHECK(XMemSetAllocationHooks(CustomAlloc, CustomFree) == XGR_S_OK);
    void* mem = XMemAlloc(256, 0);
    CHECK(mem != nullptr);
    XMemFree(mem, 0);
    CHECK(g_custom_allocs.load() == 1 && g_custom_frees.load() == 1);
    CHECK(XMemSetAllocationHooks(nullptr, nullptr) == XGR_S_OK);

    void* vmem = XMemVirtualAlloc(nullptr, 65536, 0, 0);
    CHECK(vmem != nullptr);
    std::memset(vmem, 0xAB, 4096);
    CHECK(XMemVirtualFree(vmem, 0, 0) == true);

    // 8. XThread
    CHECK(XThreadIsTimeSensitive() == false);
    CHECK(XThreadSetTimeSensitive(true) == XGR_S_OK);
    CHECK(XThreadIsTimeSensitive() == true);
    CHECK(XThreadSetTimeSensitive(false) == XGR_S_OK);
    XThreadAssertNotTimeSensitive();

    // Cleanup
    XUserCloseHandle(user);
    XTaskQueueTerminate(tp_q, true, nullptr, nullptr);
    XTaskQueueCloseHandle(tp_q);
    XGameRuntimeUninitialize();

    fs::remove_all(save_root, ec);

    std::cout << "[test_xgameruntime_abi] PASSED (XTaskQueue, XAsync, XSystem, XUser, XGameSave, XMem, XThread)\n";
    return 0;
}
