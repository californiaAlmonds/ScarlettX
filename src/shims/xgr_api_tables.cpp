// ScarlettX — xgameruntime QueryApiImpl tables.
//
// Slot layouts were read from the console runtime: each table is IUnknown
// followed by the listed methods, and every method takes the table as its
// first argument.
#include "xgr_api_tables.h"

#include "scarlettx/gdk/xgameruntime_abi.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>

namespace scarlettx::xgr {
namespace {

struct Guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

constexpr Guid kIidIUnknown = {0x00000000u, 0x0000u, 0x0000u, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
// XGameGetXboxTitleId.
constexpr Guid kApiGame = {0x973A344Eu, 0x24BFu, 0x4D0Fu, {0x84, 0x57, 0x56, 0xC5, 0x34, 0x89, 0x2B, 0x29}};
// XUser.
constexpr Guid kApiUser = {0x01ACD177u, 0x91F9u, 0x4763u, {0xA3, 0x8E, 0xCC, 0xBB, 0x55, 0xCE, 0x32, 0xE0}};
// Second interface of the XUser API: XUserGetGamertag with a component.
constexpr Guid kIidUserGamertag = {0xCEF4FAC0u, 0x7676u, 0x4A94u, {0xA1, 0x19, 0x4C, 0x43, 0xF9, 0xEB, 0x5B, 0x74}};
// XGameSave.
constexpr Guid kApiGameSave = {0x704C3F58u, 0xE629u, 0x4CC2u, {0xB1, 0x97, 0x30, 0x51, 0x1B, 0x99, 0x6F, 0xE2}};
// XSystemGetConsoleId, XSystemGetXboxLiveSandboxId.
constexpr Guid kApiSystem = {0xE349BD1Au, 0xFC20u, 0x4E40u, {0xB9, 0x9C, 0x41, 0x78, 0xCC, 0x6B, 0x40, 0x9F}};

constexpr XGameResult kNoInterface = static_cast<XGameResult>(0x80004002u);
constexpr XGameResult kNotImplemented = static_cast<XGameResult>(0x80004001u);
constexpr XGameResult kInsufficientBuffer = static_cast<XGameResult>(0x8007007Au);
constexpr size_t kConsoleIdBytes = 39;      // XSystemConsoleIdBytes
constexpr size_t kSandboxIdMaxBytes = 16;   // XSystemXboxLiveSandboxIdMaxBytes

bool Equal(const void* a, const Guid& b) { return a && std::memcmp(a, &b, sizeof(Guid)) == 0; }

bool Trace() {
    const char* v = std::getenv("SCARLETTX_TRACE");
    return v && v[0] != '\0' && v[0] != '0';
}

using Slot = void (*)();

struct Table {
    const Slot* vtbl;
    std::atomic<uint32_t> refs{1};
    Guid api;
    Guid iid;
};

// The vtable for interface `riid` of API `api_id`, or nullptr.
const Slot* FindVtbl(const void* api_id, const void* riid, Guid* iid);
void* NewTable(const Slot* vtbl, const void* api_id, const Guid& iid);

XGameResult SCARLETTX_GDK_CALLBACK TableQueryInterface(void* self, const void* riid, void** out) {
    if (!out) return XGR_E_POINTER;
    *out = nullptr;
    if (!riid) return XGR_E_POINTER;
    auto* table = static_cast<Table*>(self);
    if (Equal(riid, table->iid) || Equal(riid, kIidIUnknown)) {
        table->refs.fetch_add(1, std::memory_order_relaxed);
        *out = self;
        return XGR_S_OK;
    }
    // Another interface of the same API is a separate table object.
    Guid iid{};
    const Slot* vtbl = FindVtbl(&table->api, riid, &iid);
    if (!vtbl) return kNoInterface;
    *out = NewTable(vtbl, &table->api, iid);
    return *out ? XGR_S_OK : XGR_E_OUTOFMEMORY;
}

uint32_t SCARLETTX_GDK_CALLBACK TableAddRef(void* self) {
    return static_cast<Table*>(self)->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t SCARLETTX_GDK_CALLBACK TableRelease(void* self) {
    auto* table = static_cast<Table*>(self);
    const uint32_t left = table->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) delete table;
    return left;
}

// {973A344E} slot 3.
XGameResult SCARLETTX_GDK_CALLBACK GameGetXboxTitleId(void* /*self*/, uint32_t* title_id) {
    const XGameResult hr = XGameGetXboxTitleId(title_id);
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XGameGetXboxTitleId -> 0x%08X (0x%08X)\n",
                     static_cast<unsigned>(hr), title_id ? static_cast<unsigned>(*title_id) : 0u);
    }
    return hr;
}

// {E349BD1A} slot 3. The console always answers with exactly 39 bytes.
XGameResult SCARLETTX_GDK_CALLBACK SystemGetConsoleId(void* /*self*/, size_t size, char* buffer, size_t* used) {
    if (!buffer) return XGR_E_POINTER;
    if (size < kConsoleIdBytes) return kInsufficientBuffer;
    char id[kConsoleIdBytes] = {};
    size_t id_used = 0;
    const XGameResult hr = XSystemGetConsoleId(sizeof(id), id, &id_used);
    if (hr != XGR_S_OK) return hr;
    for (size_t i = std::strlen(id); i + 1 < kConsoleIdBytes; ++i) id[i] = '0';
    id[kConsoleIdBytes - 1] = '\0';
    std::memcpy(buffer, id, kConsoleIdBytes);
    if (used) *used = kConsoleIdBytes;
    return XGR_S_OK;
}

// {E349BD1A} slot 4.
XGameResult SCARLETTX_GDK_CALLBACK SystemGetXboxLiveSandboxId(void* /*self*/, size_t size, char* buffer,
                                                              size_t* used) {
    if (!buffer) return XGR_E_POINTER;
    if (size < kSandboxIdMaxBytes) return kInsufficientBuffer;
    return XSystemGetXboxLiveSandboxId(size, buffer, used);
}

const Slot kGameVtbl[] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&GameGetXboxTitleId),
};

const Slot kSystemVtbl[] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&SystemGetConsoleId),
    reinterpret_cast<Slot>(&SystemGetXboxLiveSandboxId),
};

// {01ACD177}: XUser, in the declaration order of the public XUser.h. The
// console table was matched against that order at slots 5, 6, 9, 13, 22, 30,
// 32, 33 and 34; slots without an implementation log their name and fail.
constexpr int kUserSlots = 37;
constexpr const char* kUserSlotNames[kUserSlots] = {
    "QueryInterface", "AddRef", "Release",
    "XUserDuplicateHandle", "XUserCloseHandle", "XUserCompare", "XUserGetMaxUsers",
    "XUserAddAsync", "XUserAddResult", "XUserGetLocalId", "XUserFindUserByLocalId",
    "XUserGetId", "XUserFindUserById", "XUserGetIsGuest", "XUserGetState",
    "XUserGetGamertag", "XUserGetGamerPictureAsync", "XUserGetGamerPictureResultSize",
    "XUserGetGamerPictureResult", "XUserGetAgeGroup", "XUserCheckPrivilege",
    "XUserResolvePrivilegeWithUiAsync", "XUserResolvePrivilegeWithUiResult",
    "XUserGetTokenAndSignatureAsync", "XUserGetTokenAndSignatureResultSize",
    "XUserGetTokenAndSignatureResult", "XUserGetTokenAndSignatureUtf16Async",
    "XUserGetTokenAndSignatureUtf16ResultSize", "XUserGetTokenAndSignatureUtf16Result",
    "XUserResolveIssueWithUiAsync", "XUserResolveIssueWithUiResult",
    "XUserResolveIssueWithUiUtf16Async", "XUserResolveIssueWithUiUtf16Result",
    "XUserRegisterForChangeEvent", "XUserUnregisterForChangeEvent",
    "XUserGetSignOutDeferral", "XUserCloseSignOutDeferralHandle",
};

XGameResult LogUnimplemented(const char* name, const char* api, int slot, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    std::fprintf(stderr, "[ScarlettX][xgameruntime] %s (ApiImpl{%s} slot %d)(0x%llx, 0x%llx, 0x%llx) -> E_NOTIMPL\n",
                 name, api, slot, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                 static_cast<unsigned long long>(a3));
    std::fflush(stderr);
    return kNotImplemented;
}

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK UserUnimplemented(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return LogUnimplemented(kUserSlotNames[Index], "01ACD177", Index, a1, a2, a3);
}

XGameResult SCARLETTX_GDK_CALLBACK UserDuplicateHandle(void*, XUserHandle user, XUserHandle* duplicate) {
    return XUserDuplicateHandle(user, duplicate);
}

void SCARLETTX_GDK_CALLBACK UserCloseHandle(void*, XUserHandle user) { XUserCloseHandle(user); }

// Zero when both handles refer to the same user.
int32_t SCARLETTX_GDK_CALLBACK UserCompare(void*, XUserHandle a, XUserHandle b) { return XUserCompare(a, b); }

XGameResult SCARLETTX_GDK_CALLBACK UserGetMaxUsers(void*, uint32_t* max_users) { return XUserGetMaxUsers(max_users); }

XGameResult SCARLETTX_GDK_CALLBACK UserAddAsync(void*, XUserAddOptions options, XAsyncBlock* async) {
    const XGameResult hr = XUserAddAsync(options, async);
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XUserAddAsync(options=%u, async=%p) -> 0x%08X\n",
                     static_cast<unsigned>(options), static_cast<void*>(async), static_cast<unsigned>(hr));
    }
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK UserAddResult(void*, XAsyncBlock* async, XUserHandle* user) {
    const XGameResult hr = XUserAddResult(async, user);
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XUserAddResult(async=%p) -> 0x%08X\n",
                     static_cast<void*>(async), static_cast<unsigned>(hr));
    }
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetLocalId(void*, XUserHandle user, XUserLocalId* local_id) {
    return XUserGetLocalId(user, local_id);
}

XGameResult SCARLETTX_GDK_CALLBACK UserFindUserByLocalId(void*, XUserLocalId local_id, XUserHandle* user) {
    return XUserFindUserByLocalId(local_id, user);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetId(void*, XUserHandle user, uint64_t* id) { return XUserGetId(user, id); }

XGameResult SCARLETTX_GDK_CALLBACK UserFindUserById(void*, uint64_t id, XUserHandle* user) {
    return XUserFindUserById(id, user);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetIsGuest(void*, XUserHandle user, bool* is_guest) {
    return XUserGetIsGuest(user, is_guest);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetState(void*, XUserHandle user, XUserState* state) {
    return XUserGetState(user, state);
}

// {CEF4FAC0} slot 3. The console checks the buffer against the component's
// maximum size, whatever the actual gamertag length.
XGameResult SCARLETTX_GDK_CALLBACK UserGetGamertagComponent(void*, XUserHandle user, uint32_t component, size_t size,
                                                            char* gamertag, size_t* used) {
    constexpr size_t kMaxBytes[] = {16, 97, 15, 101};  // classic, modern, modern suffix, unique modern
    if (used) *used = 0;
    if (component > 3) return XGR_E_INVALIDARG;
    if (size < kMaxBytes[component]) return kInsufficientBuffer;
    return XUserGetGamertag(user, static_cast<XUserGamertagComponent>(component), size, gamertag, used);
}

// {01ACD177} slot 15: the original signature without a component; the
// console forwards it to the method above with the classic component.
XGameResult SCARLETTX_GDK_CALLBACK UserGetGamertag(void* self, XUserHandle user, size_t size, char* gamertag,
                                                   size_t* used) {
    return UserGetGamertagComponent(self, user, 0, size, gamertag, used);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetGamerPictureAsync(void*, XUserHandle user,
                                                            XUserGamerPictureSize picture_size, XAsyncBlock* async) {
    return XUserGetGamerPictureAsync(user, picture_size, async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetGamerPictureResultSize(void*, XAsyncBlock* async, size_t* buffer_size) {
    return XUserGetGamerPictureResultSize(async, buffer_size);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetGamerPictureResult(void*, XAsyncBlock* async, size_t buffer_size,
                                                             void* buffer, size_t* buffer_used) {
    return XUserGetGamerPictureResult(async, buffer_size, buffer, buffer_used);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetAgeGroup(void*, XUserHandle user, XUserAgeGroup* age_group) {
    return XUserGetAgeGroup(user, age_group);
}

XGameResult SCARLETTX_GDK_CALLBACK UserCheckPrivilege(void*, XUserHandle user, XUserPrivilegeOptions options,
                                                      XUserPrivilege privilege, bool* has_privilege,
                                                      XUserPrivilegeDenyReason* reason) {
    return XUserCheckPrivilege(user, options, privilege, has_privilege, reason);
}

XGameResult SCARLETTX_GDK_CALLBACK UserResolvePrivilegeWithUiAsync(void*, XUserHandle user,
                                                                   XUserPrivilegeOptions options,
                                                                   XUserPrivilege privilege, XAsyncBlock* async) {
    return XUserResolvePrivilegeWithUiAsync(user, options, privilege, async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserResolvePrivilegeWithUiResult(void*, XAsyncBlock* async) {
    return XUserResolvePrivilegeWithUiResult(async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetTokenAndSignatureAsync(
    void*, XUserHandle user, XUserGetTokenAndSignatureOptions options, const char* method, const char* url,
    size_t header_count, const XUserGetTokenAndSignatureHttpHeader* headers, size_t body_size, const void* body_buffer,
    XAsyncBlock* async) {
    return XUserGetTokenAndSignatureAsync(user, options, method, url, header_count, headers, body_size, body_buffer,
                                          async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetTokenAndSignatureResultSize(void*, XAsyncBlock* async, size_t* buffer_size) {
    return XUserGetTokenAndSignatureResultSize(async, buffer_size);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetTokenAndSignatureResult(void*, XAsyncBlock* async, size_t buffer_size,
                                                                  void* buffer,
                                                                  XUserGetTokenAndSignatureData** ptr_to_buffer,
                                                                  size_t* buffer_used) {
    return XUserGetTokenAndSignatureResult(async, buffer_size, buffer, ptr_to_buffer, buffer_used);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetTokenAndSignatureUtf16Async(
    void*, XUserHandle user, XUserGetTokenAndSignatureOptions options, const wchar_t* method, const wchar_t* url,
    size_t header_count, const XUserGetTokenAndSignatureUtf16HttpHeader* headers, size_t body_size,
    const void* body_buffer, XAsyncBlock* async) {
    return XUserGetTokenAndSignatureUtf16Async(user, options, method, url, header_count, headers, body_size,
                                               body_buffer, async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetTokenAndSignatureUtf16ResultSize(void*, XAsyncBlock* async,
                                                                           size_t* buffer_size) {
    return XUserGetTokenAndSignatureUtf16ResultSize(async, buffer_size);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetTokenAndSignatureUtf16Result(
    void*, XAsyncBlock* async, size_t buffer_size, void* buffer,
    XUserGetTokenAndSignatureUtf16Data** ptr_to_buffer, size_t* buffer_used) {
    return XUserGetTokenAndSignatureUtf16Result(async, buffer_size, buffer, ptr_to_buffer, buffer_used);
}

XGameResult SCARLETTX_GDK_CALLBACK UserResolveIssueWithUiAsync(void*, XUserHandle user, const char* url,
                                                               XAsyncBlock* async) {
    return XUserResolveIssueWithUiAsync(user, url, async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserResolveIssueWithUiResult(void*, XAsyncBlock* async) {
    return XUserResolveIssueWithUiResult(async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserResolveIssueWithUiUtf16Async(void*, XUserHandle user, const wchar_t* url,
                                                                    XAsyncBlock* async) {
    return XUserResolveIssueWithUiUtf16Async(user, url, async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserResolveIssueWithUiUtf16Result(void*, XAsyncBlock* async) {
    return XUserResolveIssueWithUiUtf16Result(async);
}

XGameResult SCARLETTX_GDK_CALLBACK UserRegisterForChangeEvent(void*, XTaskQueueHandle queue, void* context,
                                                              XUserChangeEventCallback* callback,
                                                              XTaskQueueRegistrationToken* token) {
    return XUserRegisterForChangeEvent(queue, context, callback, token);
}

bool SCARLETTX_GDK_CALLBACK UserUnregisterForChangeEvent(void*, XTaskQueueRegistrationToken token, bool wait) {
    return XUserUnregisterForChangeEvent(token, wait);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetSignOutDeferral(void*, XUserSignOutDeferralHandle* deferral) {
    return XUserGetSignOutDeferral(deferral);
}

void SCARLETTX_GDK_CALLBACK UserCloseSignOutDeferralHandle(void*, XUserSignOutDeferralHandle deferral) {
    XUserCloseSignOutDeferralHandle(deferral);
}

template <size_t... I>
std::array<Slot, kUserSlots> MakeUserVtbl(std::index_sequence<I...>) {
    std::array<Slot, kUserSlots> vtbl = {reinterpret_cast<Slot>(&UserUnimplemented<static_cast<int>(I)>)...};
    vtbl[0] = reinterpret_cast<Slot>(&TableQueryInterface);
    vtbl[1] = reinterpret_cast<Slot>(&TableAddRef);
    vtbl[2] = reinterpret_cast<Slot>(&TableRelease);
    vtbl[3] = reinterpret_cast<Slot>(&UserDuplicateHandle);
    vtbl[4] = reinterpret_cast<Slot>(&UserCloseHandle);
    vtbl[5] = reinterpret_cast<Slot>(&UserCompare);
    vtbl[6] = reinterpret_cast<Slot>(&UserGetMaxUsers);
    vtbl[7] = reinterpret_cast<Slot>(&UserAddAsync);
    vtbl[8] = reinterpret_cast<Slot>(&UserAddResult);
    vtbl[9] = reinterpret_cast<Slot>(&UserGetLocalId);
    vtbl[10] = reinterpret_cast<Slot>(&UserFindUserByLocalId);
    vtbl[11] = reinterpret_cast<Slot>(&UserGetId);
    vtbl[12] = reinterpret_cast<Slot>(&UserFindUserById);
    vtbl[13] = reinterpret_cast<Slot>(&UserGetIsGuest);
    vtbl[14] = reinterpret_cast<Slot>(&UserGetState);
    vtbl[15] = reinterpret_cast<Slot>(&UserGetGamertag);
    vtbl[16] = reinterpret_cast<Slot>(&UserGetGamerPictureAsync);
    vtbl[17] = reinterpret_cast<Slot>(&UserGetGamerPictureResultSize);
    vtbl[18] = reinterpret_cast<Slot>(&UserGetGamerPictureResult);
    vtbl[19] = reinterpret_cast<Slot>(&UserGetAgeGroup);
    vtbl[20] = reinterpret_cast<Slot>(&UserCheckPrivilege);
    vtbl[21] = reinterpret_cast<Slot>(&UserResolvePrivilegeWithUiAsync);
    vtbl[22] = reinterpret_cast<Slot>(&UserResolvePrivilegeWithUiResult);
    vtbl[23] = reinterpret_cast<Slot>(&UserGetTokenAndSignatureAsync);
    vtbl[24] = reinterpret_cast<Slot>(&UserGetTokenAndSignatureResultSize);
    vtbl[25] = reinterpret_cast<Slot>(&UserGetTokenAndSignatureResult);
    vtbl[26] = reinterpret_cast<Slot>(&UserGetTokenAndSignatureUtf16Async);
    vtbl[27] = reinterpret_cast<Slot>(&UserGetTokenAndSignatureUtf16ResultSize);
    vtbl[28] = reinterpret_cast<Slot>(&UserGetTokenAndSignatureUtf16Result);
    vtbl[29] = reinterpret_cast<Slot>(&UserResolveIssueWithUiAsync);
    vtbl[30] = reinterpret_cast<Slot>(&UserResolveIssueWithUiResult);
    vtbl[31] = reinterpret_cast<Slot>(&UserResolveIssueWithUiUtf16Async);
    vtbl[32] = reinterpret_cast<Slot>(&UserResolveIssueWithUiUtf16Result);
    vtbl[33] = reinterpret_cast<Slot>(&UserRegisterForChangeEvent);
    vtbl[34] = reinterpret_cast<Slot>(&UserUnregisterForChangeEvent);
    vtbl[35] = reinterpret_cast<Slot>(&UserGetSignOutDeferral);
    vtbl[36] = reinterpret_cast<Slot>(&UserCloseSignOutDeferralHandle);
    return vtbl;
}
const std::array<Slot, kUserSlots> kUserVtbl = MakeUserVtbl(std::make_index_sequence<kUserSlots>{});

const Slot kUserGamertagVtbl[] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&UserGetGamertagComponent),
};

// {704C3F58}: XGameSave, in the declaration order of the public XGameSave.h.
// The console code names slots 3, 7, 13, 14, 15, 18, 19 and 20 in its error
// reports, and the order of the others was matched by argument shape.
constexpr int kGameSaveSlots = 30;
constexpr const char* kGameSaveSlotNames[kGameSaveSlots] = {
    "QueryInterface", "AddRef", "Release",
    "XGameSaveInitializeProvider", "XGameSaveInitializeProviderAsync", "XGameSaveInitializeProviderResult",
    "XGameSaveCloseProvider", "XGameSaveGetRemainingQuota", "XGameSaveGetRemainingQuotaAsync",
    "XGameSaveGetRemainingQuotaResult", "XGameSaveDeleteContainer", "XGameSaveDeleteContainerAsync",
    "XGameSaveDeleteContainerResult", "XGameSaveGetContainerInfo", "XGameSaveEnumerateContainerInfo",
    "XGameSaveEnumerateContainerInfoByName", "XGameSaveCreateContainer", "XGameSaveCloseContainer",
    "XGameSaveEnumerateBlobInfo", "XGameSaveEnumerateBlobInfoByName", "XGameSaveReadBlobData",
    "XGameSaveReadBlobDataAsync", "XGameSaveReadBlobDataResult", "XGameSaveCreateUpdate",
    "XGameSaveCloseUpdate", "XGameSaveSubmitBlobWrite", "XGameSaveSubmitBlobDelete",
    "XGameSaveSubmitUpdate", "XGameSaveSubmitUpdateAsync", "XGameSaveSubmitUpdateResult",
};

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK GameSaveUnimplemented(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return LogUnimplemented(kGameSaveSlotNames[Index], "704C3F58", Index, a1, a2, a3);
}

void TraceGameSave(const char* name, XGameResult hr) {
    if (!Trace()) return;
    std::fprintf(stderr, "[ScarlettX][xgameruntime] %s -> 0x%08X\n", name, static_cast<unsigned>(hr));
}

XGameResult SCARLETTX_GDK_CALLBACK SaveInitializeProvider(void*, XUserHandle user, const char* configuration_id,
                                                          bool sync_on_demand, XGameSaveProviderHandle* provider) {
    const XGameResult hr = XGameSaveInitializeProvider(user, configuration_id, sync_on_demand, provider);
    TraceGameSave("XGameSaveInitializeProvider", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveInitializeProviderAsync(void*, XUserHandle user, const char* configuration_id,
                                                               bool sync_on_demand, XAsyncBlock* async) {
    const XGameResult hr = XGameSaveInitializeProviderAsync(user, configuration_id, sync_on_demand, async);
    TraceGameSave("XGameSaveInitializeProviderAsync", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveInitializeProviderResult(void*, XAsyncBlock* async,
                                                                XGameSaveProviderHandle* provider) {
    return XGameSaveInitializeProviderResult(async, provider);
}

void SCARLETTX_GDK_CALLBACK SaveCloseProvider(void*, XGameSaveProviderHandle provider) {
    XGameSaveCloseProvider(provider);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveGetRemainingQuota(void*, XGameSaveProviderHandle provider, int64_t* quota) {
    return XGameSaveGetRemainingQuota(provider, quota);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveDeleteContainer(void*, XGameSaveProviderHandle provider, const char* name) {
    const XGameResult hr = XGameSaveDeleteContainer(provider, name);
    TraceGameSave("XGameSaveDeleteContainer", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveEnumerateContainerInfo(void*, XGameSaveProviderHandle provider, void* context,
                                                              XGameSaveContainerInfoCallback* callback) {
    const XGameResult hr = XGameSaveEnumerateContainerInfo(provider, context, callback);
    TraceGameSave("XGameSaveEnumerateContainerInfo", hr);
    return hr;
}

// Slots 13, 15 and 19 filter the full enumeration by name or name prefix. The
// enumeration reports stored names, so the filter compares in that form.
template <typename Info, typename Callback>
struct SaveNameFilter {
    std::string name;
    bool exact;
    void* context;
    Callback* callback;

    static bool SCARLETTX_GDK_CALLBACK Thunk(const Info* info, void* filter_ptr) {
        auto* filter = static_cast<SaveNameFilter*>(filter_ptr);
        const char* name = info && info->name ? info->name : "";
        const bool match = filter->exact ? filter->name == name
                                         : std::strncmp(name, filter->name.c_str(), filter->name.size()) == 0;
        return match ? filter->callback(info, filter->context) : true;
    }
};
using SaveContainerFilter = SaveNameFilter<XGameSaveContainerInfo, XGameSaveContainerInfoCallback>;
using SaveBlobFilter = SaveNameFilter<XGameSaveBlobInfo, XGameSaveBlobInfoCallback>;

// An empty prefix matches every name.
std::string StoredSavePrefix(const char* prefix) {
    return prefix && *prefix ? StoredSaveName(prefix) : std::string();
}

XGameResult SCARLETTX_GDK_CALLBACK SaveGetContainerInfo(void*, XGameSaveProviderHandle provider, const char* name,
                                                        void* context, XGameSaveContainerInfoCallback* callback) {
    if (!name || !callback) return XGR_E_INVALIDARG;
    SaveContainerFilter filter{StoredSaveName(name), true, context, callback};
    return XGameSaveEnumerateContainerInfo(provider, &filter, &SaveContainerFilter::Thunk);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveEnumerateContainerInfoByName(void*, XGameSaveProviderHandle provider,
                                                                    const char* prefix, void* context,
                                                                    XGameSaveContainerInfoCallback* callback) {
    if (!callback) return XGR_E_INVALIDARG;
    SaveContainerFilter filter{StoredSavePrefix(prefix), false, context, callback};
    return XGameSaveEnumerateContainerInfo(provider, &filter, &SaveContainerFilter::Thunk);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveCreateContainer(void*, XGameSaveProviderHandle provider, const char* name,
                                                       XGameSaveContainerHandle* container) {
    const XGameResult hr = XGameSaveCreateContainer(provider, name, container);
    TraceGameSave("XGameSaveCreateContainer", hr);
    return hr;
}

void SCARLETTX_GDK_CALLBACK SaveCloseContainer(void*, XGameSaveContainerHandle container) {
    XGameSaveCloseContainer(container);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveEnumerateBlobInfo(void*, XGameSaveContainerHandle container, void* context,
                                                         XGameSaveBlobInfoCallback* callback) {
    return XGameSaveEnumerateBlobInfo(container, context, callback);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveEnumerateBlobInfoByName(void*, XGameSaveContainerHandle container,
                                                               const char* prefix, void* context,
                                                               XGameSaveBlobInfoCallback* callback) {
    if (!callback) return XGR_E_INVALIDARG;
    SaveBlobFilter filter{StoredSavePrefix(prefix), false, context, callback};
    return XGameSaveEnumerateBlobInfo(container, &filter, &SaveBlobFilter::Thunk);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveReadBlobData(void*, XGameSaveContainerHandle container, const char** names,
                                                    uint32_t* count, size_t blobs_size, XGameSaveBlob* blobs) {
    const XGameResult hr = XGameSaveReadBlobData(container, names, count, blobs_size, blobs);
    TraceGameSave("XGameSaveReadBlobData", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveReadBlobDataAsync(void*, XGameSaveContainerHandle container,
                                                         const char** names, uint32_t count, XAsyncBlock* async) {
    const XGameResult hr = XGameSaveReadBlobDataAsync(container, names, count, async);
    TraceGameSave("XGameSaveReadBlobDataAsync", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveReadBlobDataResult(void*, XAsyncBlock* async, size_t blobs_size,
                                                          XGameSaveBlob* blobs, uint32_t* count) {
    return XGameSaveReadBlobDataResult(async, blobs_size, blobs, count);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveCreateUpdate(void*, XGameSaveContainerHandle container,
                                                    const char* display_name, XGameSaveUpdateHandle* update) {
    return XGameSaveCreateUpdate(container, display_name, update);
}

void SCARLETTX_GDK_CALLBACK SaveCloseUpdate(void*, XGameSaveUpdateHandle update) { XGameSaveCloseUpdate(update); }

XGameResult SCARLETTX_GDK_CALLBACK SaveSubmitBlobWrite(void*, XGameSaveUpdateHandle update, const char* blob_name,
                                                       const uint8_t* data, size_t byte_count) {
    return XGameSaveSubmitBlobWrite(update, blob_name, data, byte_count);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveSubmitBlobDelete(void*, XGameSaveUpdateHandle update, const char* blob_name) {
    return XGameSaveSubmitBlobDelete(update, blob_name);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveSubmitUpdate(void*, XGameSaveUpdateHandle update) {
    const XGameResult hr = XGameSaveSubmitUpdate(update);
    TraceGameSave("XGameSaveSubmitUpdate", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveSubmitUpdateAsync(void*, XGameSaveUpdateHandle update, XAsyncBlock* async) {
    const XGameResult hr = XGameSaveSubmitUpdateAsync(update, async);
    TraceGameSave("XGameSaveSubmitUpdateAsync", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveSubmitUpdateResult(void*, XAsyncBlock* async) {
    return XGameSaveSubmitUpdateResult(async);
}

template <size_t... I>
std::array<Slot, kGameSaveSlots> MakeGameSaveVtbl(std::index_sequence<I...>) {
    std::array<Slot, kGameSaveSlots> vtbl = {reinterpret_cast<Slot>(&GameSaveUnimplemented<static_cast<int>(I)>)...};
    vtbl[0] = reinterpret_cast<Slot>(&TableQueryInterface);
    vtbl[1] = reinterpret_cast<Slot>(&TableAddRef);
    vtbl[2] = reinterpret_cast<Slot>(&TableRelease);
    vtbl[3] = reinterpret_cast<Slot>(&SaveInitializeProvider);
    vtbl[4] = reinterpret_cast<Slot>(&SaveInitializeProviderAsync);
    vtbl[5] = reinterpret_cast<Slot>(&SaveInitializeProviderResult);
    vtbl[6] = reinterpret_cast<Slot>(&SaveCloseProvider);
    vtbl[7] = reinterpret_cast<Slot>(&SaveGetRemainingQuota);
    vtbl[10] = reinterpret_cast<Slot>(&SaveDeleteContainer);
    vtbl[13] = reinterpret_cast<Slot>(&SaveGetContainerInfo);
    vtbl[14] = reinterpret_cast<Slot>(&SaveEnumerateContainerInfo);
    vtbl[15] = reinterpret_cast<Slot>(&SaveEnumerateContainerInfoByName);
    vtbl[16] = reinterpret_cast<Slot>(&SaveCreateContainer);
    vtbl[17] = reinterpret_cast<Slot>(&SaveCloseContainer);
    vtbl[18] = reinterpret_cast<Slot>(&SaveEnumerateBlobInfo);
    vtbl[19] = reinterpret_cast<Slot>(&SaveEnumerateBlobInfoByName);
    vtbl[20] = reinterpret_cast<Slot>(&SaveReadBlobData);
    vtbl[21] = reinterpret_cast<Slot>(&SaveReadBlobDataAsync);
    vtbl[22] = reinterpret_cast<Slot>(&SaveReadBlobDataResult);
    vtbl[23] = reinterpret_cast<Slot>(&SaveCreateUpdate);
    vtbl[24] = reinterpret_cast<Slot>(&SaveCloseUpdate);
    vtbl[25] = reinterpret_cast<Slot>(&SaveSubmitBlobWrite);
    vtbl[26] = reinterpret_cast<Slot>(&SaveSubmitBlobDelete);
    vtbl[27] = reinterpret_cast<Slot>(&SaveSubmitUpdate);
    vtbl[28] = reinterpret_cast<Slot>(&SaveSubmitUpdateAsync);
    vtbl[29] = reinterpret_cast<Slot>(&SaveSubmitUpdateResult);
    return vtbl;
}
const std::array<Slot, kGameSaveSlots> kGameSaveVtbl = MakeGameSaveVtbl(std::make_index_sequence<kGameSaveSlots>{});

// Discovery table for API ids nothing implements, enabled with
// SCARLETTX_XGR_DISCOVER=1: every method logs its slot and first arguments
// and fails, so a trace shows which slots a title uses.
constexpr int kDiscoverySlots = 64;

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK DiscoverySlot(void* self, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    char iid[40] = {};
    const Guid& g = static_cast<Table*>(self)->iid;
    std::snprintf(iid, sizeof(iid), "%08X-%04X-%04X", static_cast<unsigned>(g.data1), g.data2, g.data3);
    std::fprintf(stderr,
                 "[ScarlettX][xgameruntime] ApiImpl{%s}::+0x%X (slot %d)(a1=0x%llx, a2=0x%llx, a3=0x%llx) -> E_NOTIMPL\n",
                 iid, Index * 8, Index, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                 static_cast<unsigned long long>(a3));
    std::fflush(stderr);
    return kNotImplemented;
}

template <size_t... I>
constexpr std::array<Slot, kDiscoverySlots> MakeDiscoveryVtbl(std::index_sequence<I...>) {
    return {reinterpret_cast<Slot>(&TableQueryInterface), reinterpret_cast<Slot>(&TableAddRef),
            reinterpret_cast<Slot>(&TableRelease), reinterpret_cast<Slot>(&DiscoverySlot<static_cast<int>(I) + 3>)...};
}
const std::array<Slot, kDiscoverySlots> kDiscoveryVtbl = MakeDiscoveryVtbl(std::make_index_sequence<kDiscoverySlots - 3>{});

bool DiscoveryEnabled() {
    const char* v = std::getenv("SCARLETTX_XGR_DISCOVER");
    return v && v[0] != '\0' && v[0] != '0';
}

void* NewTable(const Slot* vtbl, const void* api_id, const Guid& iid) {
    auto* table = new (std::nothrow) Table();
    if (!table) return nullptr;
    table->vtbl = vtbl;
    std::memcpy(&table->api, api_id, sizeof(Guid));
    table->iid = iid;
    return table;
}

const Slot* FindVtbl(const void* api_id, const void* riid, Guid* iid) {
    // An API's main interface has the API's own id.
    const bool main = std::memcmp(api_id, riid, sizeof(Guid)) == 0 || Equal(riid, kIidIUnknown);
    std::memcpy(iid, main ? api_id : riid, sizeof(Guid));
    if (Equal(api_id, kApiGame)) return main ? kGameVtbl : nullptr;
    if (Equal(api_id, kApiSystem)) return main ? kSystemVtbl : nullptr;
    if (Equal(api_id, kApiGameSave)) return main ? kGameSaveVtbl.data() : nullptr;
    if (Equal(api_id, kApiUser)) {
        if (main) return kUserVtbl.data();
        return Equal(riid, kIidUserGamertag) ? kUserGamertagVtbl : nullptr;
    }
    return main && DiscoveryEnabled() ? kDiscoveryVtbl.data() : nullptr;
}

}  // namespace

void* CreateApiTable(const void* api_id, const void* riid) {
    if (!api_id || !riid) return nullptr;
    Guid iid{};
    const Slot* vtbl = FindVtbl(api_id, riid, &iid);
    return vtbl ? NewTable(vtbl, api_id, iid) : nullptr;
}

}  // namespace scarlettx::xgr
