#include "scarlettx/gdk/xgameruntime_abi.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "[test_query_api_impl] FAILED: " #cond " at line "      \
                      << __LINE__ << "\n";                                       \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace {

struct TestGuid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

constexpr TestGuid kGuid073B7DCB = {
    0x073B7DCBu, 0x1FCFu, 0x4030u, {0x94, 0xBE, 0xE3, 0xC9, 0xEB, 0x62, 0x34, 0x28}};

constexpr TestGuid kGuidIUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

constexpr TestGuid kGuid37E56907 = {
    0x37E56907u, 0x2F10u, 0x41E8u, {0xB7, 0x2F, 0x36, 0xED, 0xB1, 0x85, 0x33, 0x1A}};

// An API id no table implements.
constexpr TestGuid kGuidUnsupported = {
    0x0BADF00Du, 0x1111u, 0x2222u, {0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA}};
// XGameGetXboxTitleId.
constexpr TestGuid kGuid973A344E = {
    0x973A344Eu, 0x24BFu, 0x4D0Fu, {0x84, 0x57, 0x56, 0xC5, 0x34, 0x89, 0x2B, 0x29}};
// XUser.
constexpr TestGuid kGuid01ACD177 = {
    0x01ACD177u, 0x91F9u, 0x4763u, {0xA3, 0x8E, 0xCC, 0xBB, 0x55, 0xCE, 0x32, 0xE0}};
// XUserGetGamertag with a component: second interface of the XUser API.
constexpr TestGuid kGuidCEF4FAC0 = {
    0xCEF4FAC0u, 0x7676u, 0x4A94u, {0xA1, 0x19, 0x4C, 0x43, 0xF9, 0xEB, 0x5B, 0x74}};
// XGameSave.
constexpr TestGuid kGuid704C3F58 = {
    0x704C3F58u, 0xE629u, 0x4CC2u, {0xB1, 0x97, 0x30, 0x51, 0x1B, 0x99, 0x6F, 0xE2}};
// XSystemGetConsoleId, XSystemGetXboxLiveSandboxId.
constexpr TestGuid kGuidE349BD1A = {
    0xE349BD1Au, 0xFC20u, 0x4E40u, {0xB9, 0x9C, 0x41, 0x78, 0xCC, 0x6B, 0x40, 0x9F}};

constexpr XGameResult kENoInterface = static_cast<XGameResult>(0x80004002u);
constexpr XGameResult kENotImpl = static_cast<XGameResult>(0x80004001u);

template <typename Fn>
Fn LoadVtableSlot(void* obj, size_t byte_offset) {
    auto* vtbl_base = *reinterpret_cast<const uint8_t* const*>(obj);
    void* fn_ptr = nullptr;
    std::memcpy(&fn_ptr, vtbl_base + byte_offset, sizeof(fn_ptr));
    return reinterpret_cast<Fn>(fn_ptr);
}

using FnQueryInterface = XGameResult(SCARLETTX_GDK_CALLBACK*)(void*, const void*, void**);
using FnAddRef = uint32_t(SCARLETTX_GDK_CALLBACK*)(void*);
using FnRelease = uint32_t(SCARLETTX_GDK_CALLBACK*)(void*);
using FnSlot06 = XGameResult(SCARLETTX_GDK_CALLBACK*)(void*, XAsyncBlock*, XAsyncWork*);
using FnSlot12 = XGameResult(SCARLETTX_GDK_CALLBACK*)(void*, uint32_t, uint32_t, XTaskQueueHandle*);
using FnSlot16 = uint8_t(SCARLETTX_GDK_CALLBACK*)(void*, XTaskQueueHandle, uint32_t, uint32_t);
using FnSlot17 = void(SCARLETTX_GDK_CALLBACK*)(void*, XTaskQueueHandle);
using FnSlot29 = void(SCARLETTX_GDK_CALLBACK*)(void*);
using FnUnimpl = XGameResult(SCARLETTX_GDK_CALLBACK*)(void*);

std::atomic<int> g_work_calls{0};
std::atomic<int> g_completion_calls{0};

XGameResult SCARLETTX_GDK_CALLBACK SampleAsyncWork(XAsyncBlock* async) {
    g_work_calls.fetch_add(1, std::memory_order_relaxed);
    auto* ctx_val = static_cast<int*>(async->context);
    if (ctx_val) {
        *ctx_val = 42;
    }
    return XGR_S_OK;
}

void SCARLETTX_GDK_CALLBACK SampleAsyncCompletion(XAsyncBlock* /*async*/) {
    g_completion_calls.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

int main() {
    // Keep save data written by this test in its own directory.
    std::error_code save_ec;
    const std::filesystem::path save_root = std::filesystem::temp_directory_path() / "scarlettx_test_query_api_impl";
    std::filesystem::remove_all(save_root, save_ec);
#if defined(_WIN32)
    _putenv_s("SCARLETTX_SAVE_ROOT", save_root.string().c_str());
#else
    setenv("SCARLETTX_SAVE_ROOT", save_root.string().c_str(), 1);
#endif

    CHECK(InitializeApiImplEx(19041, 10002, 0) == XGR_S_OK);

    // 1. Null parameter validation & out-pointer initialization.
    void* sentinel = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEADBEEFull));
    void* out = sentinel;
    CHECK(QueryApiImpl(nullptr, &kGuid073B7DCB, &out) == XGR_E_POINTER);
    CHECK(QueryApiImpl(&kGuid073B7DCB, nullptr, &out) == XGR_E_POINTER);
    CHECK(QueryApiImpl(&kGuid073B7DCB, &kGuid073B7DCB, nullptr) == XGR_E_POINTER);

    // 2. Unsupported GUIDs must fail with E_NOINTERFACE and null out *out_impl.
    out = sentinel;
    CHECK(QueryApiImpl(&kGuidUnsupported, &kGuidUnsupported, &out) == kENoInterface);
    CHECK(out == nullptr);

    out = sentinel;
    CHECK(QueryApiImpl(&kGuid073B7DCB, &kGuidUnsupported, &out) == kENoInterface);
    CHECK(out == nullptr);

    // 2b. Second observed runtime GUID {37E56907-...} returns a ScarlettX-owned instance.
    void* impl_37e5 = nullptr;
    CHECK(QueryApiImpl(&kGuid37E56907, &kGuid37E56907, &impl_37e5) == XGR_S_OK);
    CHECK(impl_37e5 != nullptr);
    auto release_37e5 = LoadVtableSlot<FnRelease>(impl_37e5, 0x10);
    CHECK(release_37e5(impl_37e5) == 0u);

    // 3. Matching API GUID with identical IID and with IID_IUnknown.
    void* impl = nullptr;
    CHECK(QueryApiImpl(&kGuid073B7DCB, &kGuid073B7DCB, &impl) == XGR_S_OK);
    CHECK(impl != nullptr);

    void* impl_unk = nullptr;
    CHECK(QueryApiImpl(&kGuid073B7DCB, &kGuidIUnknown, &impl_unk) == XGR_S_OK);
    CHECK(impl_unk != nullptr);
    CHECK(impl_unk != impl);  // ScarlettX-owned distinct guest instances.

    auto qi = LoadVtableSlot<FnQueryInterface>(impl, 0x00);
    auto add_ref = LoadVtableSlot<FnAddRef>(impl, 0x08);
    auto release = LoadVtableSlot<FnRelease>(impl, 0x10);
    auto unimpl = LoadVtableSlot<FnUnimpl>(impl, 0x40);
    auto slot_0x30 = LoadVtableSlot<FnSlot06>(impl, 0x30);
    auto slot_0x60 = LoadVtableSlot<FnSlot12>(impl, 0x60);
    auto slot_0x80 = LoadVtableSlot<FnSlot16>(impl, 0x80);
    auto slot_0x88 = LoadVtableSlot<FnSlot17>(impl, 0x88);
    auto slot_0xe8 = LoadVtableSlot<FnSlot29>(impl, 0xE8);

    CHECK(qi && add_ref && release && unimpl && slot_0x30 && slot_0x60 &&
          slot_0x80 && slot_0x88 && slot_0xe8);

    // 4. COM IUnknown lifetime checks at +0x00 / +0x08 / +0x10.
    void* qi_out = sentinel;
    CHECK(qi(impl, &kGuid073B7DCB, nullptr) == XGR_E_POINTER);
    CHECK(qi(impl, nullptr, &qi_out) == XGR_E_INVALIDARG);
    CHECK(qi_out == nullptr);

    qi_out = sentinel;
    CHECK(qi(impl, &kGuidUnsupported, &qi_out) == kENoInterface);
    CHECK(qi_out == nullptr);

    CHECK(qi(impl, &kGuidIUnknown, &qi_out) == XGR_S_OK);
    CHECK(qi_out == impl);
    CHECK(add_ref(impl) == 3u);
    CHECK(release(impl) == 2u);
    CHECK(release(qi_out) == 1u);

    // Release second instance cleanly to zero.
    auto release_unk = LoadVtableSlot<FnRelease>(impl_unk, 0x10);
    CHECK(release_unk(impl_unk) == 0u);

    // 5. Unidentified slot (+0x40) returns E_NOTIMPL.
    CHECK(unimpl(impl) == kENotImpl);

    // 6. Observed slots: +0xE8, +0x60, +0x30, +0x80 (bool/u8), +0x88.
    slot_0xe8(impl);

    XTaskQueueHandle queue = nullptr;
    // Guests pass the public-header integers: 0 is XTaskQueueDispatchMode::Manual.
    CHECK(slot_0x60(impl, 0u, 0u, &queue) == XGR_S_OK);
    CHECK(queue != nullptr);

    // Empty manual queue returns 0 (false) in low byte.
    CHECK(slot_0x80(impl, queue, static_cast<uint32_t>(XTaskQueuePort_Work), 0) == 0u);

    int ctx_value = 0;
    XAsyncBlock async{};
    async.queue = queue;
    async.context = &ctx_value;
    async.callback = SampleAsyncCompletion;

    g_work_calls.store(0);
    g_completion_calls.store(0);
    CHECK(slot_0x30(impl, &async, SampleAsyncWork) == XGR_S_OK);

    // Dispatch work port: returns 1 (true) in low byte and runs callback.
    CHECK(slot_0x80(impl, queue, static_cast<uint32_t>(XTaskQueuePort_Work), 0) == 1u);
    CHECK(g_work_calls.load() == 1);
    CHECK(ctx_value == 42);

    // Dispatch completion port: returns 1 (true) in low byte and runs completion.
    CHECK(slot_0x80(impl, queue, static_cast<uint32_t>(XTaskQueuePort_Completion), 0) == 1u);
    CHECK(g_completion_calls.load() == 1);
    CHECK(XAsyncGetStatus(&async, false) == XGR_S_OK);

    // Close queue via +0x88 and release COM wrapper via +0x10.
    slot_0x88(impl, queue);
    CHECK(release(impl) == 0u);

    // {973A344E}: IUnknown plus one method at +0x18, the title id.
    {
        using FnTitleId = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, uint32_t*);
        void* game = nullptr;
        CHECK(QueryApiImpl(&kGuid973A344E, &kGuid973A344E, &game) == XGR_S_OK);
        CHECK(game != nullptr);
        uint32_t expected = 0;
        CHECK(XGameGetXboxTitleId(&expected) == XGR_S_OK);
        uint32_t title_id = 0;
        CHECK(LoadVtableSlot<FnTitleId>(game, 0x18)(game, &title_id) == XGR_S_OK);
        CHECK(title_id == expected);
        void* again = nullptr;
        CHECK(LoadVtableSlot<FnQueryInterface>(game, 0x00)(game, &kGuid973A344E, &again) == XGR_S_OK);
        CHECK(again == game);
        CHECK(LoadVtableSlot<FnQueryInterface>(game, 0x00)(game, &kGuidE349BD1A, &again) == kENoInterface);
        CHECK(again == nullptr);
        CHECK(LoadVtableSlot<FnRelease>(game, 0x10)(game) == 1u);
        CHECK(LoadVtableSlot<FnRelease>(game, 0x10)(game) == 0u);
        game = sentinel;
        CHECK(QueryApiImpl(&kGuid973A344E, &kGuidE349BD1A, &game) == kENoInterface);
        CHECK(game == nullptr);
    }

    // {E349BD1A}: console id at +0x18 (always 39 bytes), sandbox id at +0x20.
    {
        using FnGetString = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, size_t, char*, size_t*);
        const XGameResult kInsufficientBuffer = static_cast<XGameResult>(0x8007007Au);
        void* system = nullptr;
        CHECK(QueryApiImpl(&kGuidE349BD1A, &kGuidE349BD1A, &system) == XGR_S_OK);
        CHECK(system != nullptr);
        auto console_id = LoadVtableSlot<FnGetString>(system, 0x18);
        auto sandbox_id = LoadVtableSlot<FnGetString>(system, 0x20);

        char buffer[64];
        std::memset(buffer, 'x', sizeof(buffer));
        size_t used = 777;
        CHECK(console_id(system, 38, buffer, &used) == kInsufficientBuffer);
        CHECK(used == 777 && buffer[0] == 'x');
        CHECK(console_id(system, 39, buffer, &used) == XGR_S_OK);
        CHECK(used == 39 && buffer[38] == 0 && std::strlen(buffer) == 38 && buffer[39] == 'x');
        CHECK(console_id(system, sizeof(buffer), buffer, nullptr) == XGR_S_OK);

        std::memset(buffer, 'x', sizeof(buffer));
        used = 777;
        CHECK(sandbox_id(system, 15, buffer, &used) == kInsufficientBuffer);
        CHECK(used == 777 && buffer[0] == 'x');
        CHECK(sandbox_id(system, 16, buffer, &used) == XGR_S_OK);
        CHECK(used == std::strlen(buffer) + 1 && used <= 16);
        CHECK(LoadVtableSlot<FnRelease>(system, 0x10)(system) == 0u);
    }

    // {01ACD177}: XUser in public-header order. Literal slot offsets and
    // literal enum values, as a title would use them.
    {
        using FnRegister = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, void*, void*, void*, uint64_t*);
        using FnUnregister = bool (SCARLETTX_GDK_CALLBACK*)(void*, uint64_t, bool);
        using FnAddAsync = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, uint32_t, XAsyncBlock*);
        using FnAddResult = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, XAsyncBlock*, void**);
        using FnGetU64 = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, void*, uint64_t*);
        using FnGetBool = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, void*, bool*);
        using FnGetU32 = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, uint32_t*);
        using FnGamertag = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, void*, size_t, char*, size_t*);
        using FnGamertagComponent =
            XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, void*, uint32_t, size_t, char*, size_t*);
        using FnCompare = int32_t (SCARLETTX_GDK_CALLBACK*)(void*, void*, void*);
        using FnDuplicate = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, void*, void**);
        using FnClose = void (SCARLETTX_GDK_CALLBACK*)(void*, void*);
        using FnOther = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, uintptr_t, uintptr_t, uintptr_t);

        void* xuser = nullptr;
        CHECK(QueryApiImpl(&kGuid01ACD177, &kGuid01ACD177, &xuser) == XGR_S_OK);
        CHECK(xuser != nullptr);

        uint64_t token = 0;
        int dummy_callback = 0;
        CHECK(LoadVtableSlot<FnRegister>(xuser, 0x108)(xuser, nullptr, nullptr, &dummy_callback, &token) == XGR_S_OK);
        CHECK(token != 0);

        uint32_t max_users = 0;
        CHECK(LoadVtableSlot<FnGetU32>(xuser, 0x30)(xuser, &max_users) == XGR_S_OK);
        CHECK(max_users >= 1);

        XAsyncBlock add{};
        CHECK(LoadVtableSlot<FnAddAsync>(xuser, 0x38)(xuser, 0u, &add) == XGR_S_OK);
        void* user = nullptr;
        CHECK(LoadVtableSlot<FnAddResult>(xuser, 0x40)(xuser, &add, &user) == XGR_S_OK);
        CHECK(user != nullptr);

        uint64_t xuid = 0;
        CHECK(LoadVtableSlot<FnGetU64>(xuser, 0x58)(xuser, user, &xuid) == XGR_S_OK);
        CHECK(xuid != 0);
        uint64_t local_id = 0;
        CHECK(LoadVtableSlot<FnGetU64>(xuser, 0x48)(xuser, user, &local_id) == XGR_S_OK);
        CHECK(local_id != 0);
        bool is_guest = true;
        CHECK(LoadVtableSlot<FnGetBool>(xuser, 0x68)(xuser, user, &is_guest) == XGR_S_OK);
        CHECK(!is_guest);
        char gamertag[64] = {};
        size_t gamertag_used = 0;
        // Slot 15 has no component argument and needs the classic maximum (16).
        const XGameResult kTooSmall = static_cast<XGameResult>(0x8007007Au);
        CHECK(LoadVtableSlot<FnGamertag>(xuser, 0x78)(xuser, user, 15, gamertag, &gamertag_used) == kTooSmall);
        CHECK(LoadVtableSlot<FnGamertag>(xuser, 0x78)(xuser, user, 16, gamertag, &gamertag_used) == XGR_S_OK);
        CHECK(gamertag[0] != 0 && gamertag_used == std::strlen(gamertag) + 1);

        // The component version is interface {CEF4FAC0} of the same API,
        // reachable through QueryApiImpl and through QueryInterface.
        void* xuser2 = nullptr;
        CHECK(QueryApiImpl(&kGuid01ACD177, &kGuidCEF4FAC0, &xuser2) == XGR_S_OK);
        CHECK(xuser2 != nullptr && xuser2 != xuser);
        auto gamertag_component = LoadVtableSlot<FnGamertagComponent>(xuser2, 0x18);
        char modern[128] = {};
        size_t modern_used = 0;
        CHECK(gamertag_component(xuser2, user, 4u, sizeof(modern), modern, &modern_used) == XGR_E_INVALIDARG);
        CHECK(gamertag_component(xuser2, user, 1u, 96, modern, &modern_used) == kTooSmall);
        CHECK(gamertag_component(xuser2, user, 3u, 100, modern, &modern_used) == kTooSmall);
        CHECK(gamertag_component(xuser2, user, 2u, 14, modern, &modern_used) == kTooSmall);
        CHECK(gamertag_component(xuser2, user, 0u, 16, modern, &modern_used) == XGR_S_OK);
        CHECK(std::strcmp(modern, gamertag) == 0 && modern_used == gamertag_used);
        CHECK(gamertag_component(xuser2, user, 1u, 97, modern, &modern_used) == XGR_S_OK);
        CHECK(LoadVtableSlot<FnRelease>(xuser2, 0x10)(xuser2) == 0u);
        void* via_qi = nullptr;
        CHECK(LoadVtableSlot<FnQueryInterface>(xuser, 0x00)(xuser, &kGuidCEF4FAC0, &via_qi) == XGR_S_OK);
        CHECK(via_qi != nullptr);
        CHECK(LoadVtableSlot<FnRelease>(via_qi, 0x10)(via_qi) == 0u);

        void* same = nullptr;
        CHECK(LoadVtableSlot<FnDuplicate>(xuser, 0x18)(xuser, user, &same) == XGR_S_OK);
        CHECK(LoadVtableSlot<FnCompare>(xuser, 0x28)(xuser, user, same) == 0);
        CHECK(LoadVtableSlot<FnCompare>(xuser, 0x28)(xuser, user, nullptr) != 0);
        LoadVtableSlot<FnClose>(xuser, 0x20)(xuser, same);

        // Slots 10 (0x50) and 12 (0x60): XUserFindUserByLocalId & XUserFindUserById.
        using FnFindByLocalId = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, XUserLocalId, void**);
        using FnFindById = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, uint64_t, void**);
        void* by_local = nullptr;
        CHECK(LoadVtableSlot<FnFindByLocalId>(xuser, 0x50)(xuser, XUserLocalId{local_id}, &by_local) == XGR_S_OK);
        CHECK(by_local != nullptr && LoadVtableSlot<FnCompare>(xuser, 0x28)(xuser, user, by_local) == 0);
        LoadVtableSlot<FnClose>(xuser, 0x20)(xuser, by_local);

        void* by_id = nullptr;
        CHECK(LoadVtableSlot<FnFindById>(xuser, 0x60)(xuser, xuid, &by_id) == XGR_S_OK);
        CHECK(by_id != nullptr && LoadVtableSlot<FnCompare>(xuser, 0x28)(xuser, user, by_id) == 0);
        LoadVtableSlot<FnClose>(xuser, 0x20)(xuser, by_id);
        CHECK(LoadVtableSlot<FnFindById>(xuser, 0x60)(xuser, 0xDEADBEEFull, &by_id) == XGR_E_NOTFOUND);

        // Slots 16-18 (0x80-0x90): XUserGetGamerPictureAsync / ResultSize / Result.
        using FnPicAsync = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, void*, uint32_t, XAsyncBlock*);
        using FnSizeResult = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, XAsyncBlock*, size_t*);
        using FnPicResult = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, XAsyncBlock*, size_t, void*, size_t*);
        XAsyncBlock pic{};
        CHECK(LoadVtableSlot<FnPicAsync>(xuser, 0x80)(xuser, user, 0u, &pic) == XGR_S_OK);
        CHECK(XAsyncGetStatus(&pic, true) == XGR_S_OK);
        size_t pic_sz = 0;
        CHECK(LoadVtableSlot<FnSizeResult>(xuser, 0x88)(xuser, &pic, &pic_sz) == XGR_S_OK && pic_sz > 32);
        uint8_t pic_buf[1024] = {};
        size_t pic_used = 0;
        CHECK(pic_sz <= sizeof(pic_buf));
        CHECK(LoadVtableSlot<FnPicResult>(xuser, 0x90)(xuser, &pic, sizeof(pic_buf), pic_buf, &pic_used) == XGR_S_OK);
        CHECK(pic_used == pic_sz && pic_buf[0] == 0x89 && pic_buf[1] == 'P');

        // Slots 23-25 (0xB8-0xC8): XUserGetTokenAndSignatureAsync / ResultSize / Result.
        using FnTokAsync = XGameResult (SCARLETTX_GDK_CALLBACK*)(
            void*, void*, uint32_t, const char*, const char*, size_t, const void*, size_t, const void*, XAsyncBlock*);
        using FnTokResult = XGameResult (SCARLETTX_GDK_CALLBACK*)(
            void*, XAsyncBlock*, size_t, void*, XUserGetTokenAndSignatureData**, size_t*);
        XAsyncBlock tok{};
        CHECK(LoadVtableSlot<FnTokAsync>(xuser, 0xB8)(xuser, user, 0u, "GET", "https://example.invalid", 0, nullptr, 0,
                                                      nullptr, &tok) == XGR_S_OK);
        CHECK(XAsyncGetStatus(&tok, true) == XGR_S_OK);
        size_t tok_sz = 0;
        CHECK(LoadVtableSlot<FnSizeResult>(xuser, 0xC0)(xuser, &tok, &tok_sz) == XGR_S_OK && tok_sz > 0);
        alignas(XUserGetTokenAndSignatureData) uint8_t tok_buf[512] = {};
        XUserGetTokenAndSignatureData* tok_data = nullptr;
        size_t tok_used = 0;
        CHECK(tok_sz <= sizeof(tok_buf));
        CHECK(LoadVtableSlot<FnTokResult>(xuser, 0xC8)(xuser, &tok, sizeof(tok_buf), tok_buf, &tok_data, &tok_used) ==
              XGR_S_OK);
        CHECK(tok_data != nullptr && tok_data->token != nullptr && tok_data->signature != nullptr);

        // Slots 35-36 (0x118-0x120): XUserGetSignOutDeferral / Close.
        using FnGetDeferral = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, XUserSignOutDeferralHandle*);
        using FnCloseDeferral = void (SCARLETTX_GDK_CALLBACK*)(void*, XUserSignOutDeferralHandle);
        XUserSignOutDeferralHandle deferral = nullptr;
        CHECK(LoadVtableSlot<FnGetDeferral>(xuser, 0x118)(xuser, &deferral) == XGR_S_OK && deferral != nullptr);
        LoadVtableSlot<FnCloseDeferral>(xuser, 0x120)(xuser, deferral);
        (void)sizeof(FnOther);

        CHECK(LoadVtableSlot<FnUnregister>(xuser, 0x110)(xuser, token, true));
        CHECK(!LoadVtableSlot<FnUnregister>(xuser, 0x110)(xuser, token, true));
        CHECK(!LoadVtableSlot<FnUnregister>(xuser, 0x110)(xuser, 0, true));
        LoadVtableSlot<FnClose>(xuser, 0x20)(xuser, user);
        CHECK(LoadVtableSlot<FnRelease>(xuser, 0x10)(xuser) == 0u);
    }

    // {704C3F58}: XGameSave in public-header order, driven through literal
    // slot offsets: write a blob, find it again, read it back, delete it.
    {
        using FnInit = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, void*, const char*, bool, void**);
        using FnCreate = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, void*, const char*, void**);
        using FnWrite = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, void*, const char*, const uint8_t*, size_t);
        using FnOne = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, void*);
        using FnClose = void (SCARLETTX_GDK_CALLBACK*)(void*, void*);
        using FnContainersByName = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, void*, const char*, void*,
                                                                         XGameSaveContainerInfoCallback*);
        using FnBlobsByName = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, void*, const char*, void*,
                                                                    XGameSaveBlobInfoCallback*);
        using FnRead = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, void*, const char**, uint32_t*, size_t,
                                                             XGameSaveBlob*);
        using FnDelete = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, void*, const char*);
        using FnOther = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, uintptr_t, uintptr_t, uintptr_t);
        using FnAddAsync = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, uint32_t, XAsyncBlock*);
        using FnAddResult = XGameResult (SCARLETTX_GDK_CALLBACK*)(void*, XAsyncBlock*, void**);

        void* xuser = nullptr;
        CHECK(QueryApiImpl(&kGuid01ACD177, &kGuid01ACD177, &xuser) == XGR_S_OK);
        XAsyncBlock add{};
        CHECK(LoadVtableSlot<FnAddAsync>(xuser, 0x38)(xuser, 0u, &add) == XGR_S_OK);
        void* user = nullptr;
        CHECK(LoadVtableSlot<FnAddResult>(xuser, 0x40)(xuser, &add, &user) == XGR_S_OK);

        void* save = nullptr;
        CHECK(QueryApiImpl(&kGuid704C3F58, &kGuid704C3F58, &save) == XGR_S_OK);
        CHECK(save != nullptr);
        void* provider = nullptr;
        CHECK(LoadVtableSlot<FnInit>(save, 0x18)(save, user, "00000000-0000-0000-0000-000072a634a4", false,
                                                 &provider) == XGR_S_OK);
        CHECK(provider != nullptr);
        void* container = nullptr;
        CHECK(LoadVtableSlot<FnCreate>(save, 0x80)(save, provider, "slot_a", &container) == XGR_S_OK);
        void* update = nullptr;
        CHECK(LoadVtableSlot<FnCreate>(save, 0xB8)(save, container, "Slot A", &update) == XGR_S_OK);
        const uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7};
        CHECK(LoadVtableSlot<FnWrite>(save, 0xC8)(save, update, "progress", payload, sizeof(payload)) == XGR_S_OK);
        CHECK(LoadVtableSlot<FnOne>(save, 0xD8)(save, update) == XGR_S_OK);
        LoadVtableSlot<FnClose>(save, 0xC0)(save, update);

        struct Seen {
            int containers = 0;
            int blobs = 0;
            uint32_t blob_size = 0;
        } seen;
        XGameSaveContainerInfoCallback* on_container = [](const XGameSaveContainerInfo* info, void* ctx) -> bool {
            if (std::strcmp(info->name, "slot_a") == 0) ++static_cast<Seen*>(ctx)->containers;
            return true;
        };
        XGameSaveBlobInfoCallback* on_blob = [](const XGameSaveBlobInfo* info, void* ctx) -> bool {
            ++static_cast<Seen*>(ctx)->blobs;
            static_cast<Seen*>(ctx)->blob_size = info->size;
            return true;
        };
        CHECK(LoadVtableSlot<FnContainersByName>(save, 0x68)(save, provider, "slot_a", &seen, on_container) == XGR_S_OK);
        CHECK(seen.containers == 1);
        CHECK(LoadVtableSlot<FnContainersByName>(save, 0x68)(save, provider, "slot", &seen, on_container) == XGR_S_OK);
        CHECK(seen.containers == 1);  // GetContainerInfo matches the whole name
        CHECK(LoadVtableSlot<FnContainersByName>(save, 0x78)(save, provider, "slot", &seen, on_container) == XGR_S_OK);
        CHECK(seen.containers == 2);  // the ByName enumeration matches a prefix
        CHECK(LoadVtableSlot<FnBlobsByName>(save, 0x98)(save, container, "prog", &seen, on_blob) == XGR_S_OK);
        CHECK(LoadVtableSlot<FnBlobsByName>(save, 0x98)(save, container, "other", &seen, on_blob) == XGR_S_OK);
        CHECK(seen.blobs == 1 && seen.blob_size == sizeof(payload));

        const char* names[] = {"progress"};
        uint32_t count = 1;
        alignas(XGameSaveBlob) uint8_t buffer[256] = {};
        auto* blobs = reinterpret_cast<XGameSaveBlob*>(buffer);
        CHECK(LoadVtableSlot<FnRead>(save, 0xA0)(save, container, names, &count, sizeof(buffer), blobs) == XGR_S_OK);
        CHECK(count == 1 && blobs[0].info.size == sizeof(payload));
        CHECK(std::memcmp(blobs[0].data, payload, sizeof(payload)) == 0);

        // Names are matched in the form the save store keeps them in, so a
        // name with a space finds its container and blob again.
        {
            void* spaced = nullptr;
            CHECK(LoadVtableSlot<FnCreate>(save, 0x80)(save, provider, "slot b", &spaced) == XGR_S_OK);
            void* spaced_update = nullptr;
            CHECK(LoadVtableSlot<FnCreate>(save, 0xB8)(save, spaced, "Slot B", &spaced_update) == XGR_S_OK);
            CHECK(LoadVtableSlot<FnWrite>(save, 0xC8)(save, spaced_update, "high score", payload, sizeof(payload)) ==
                  XGR_S_OK);
            CHECK(LoadVtableSlot<FnOne>(save, 0xD8)(save, spaced_update) == XGR_S_OK);
            LoadVtableSlot<FnClose>(save, 0xC0)(save, spaced_update);

            int hits = 0;
            XGameSaveContainerInfoCallback* count_container = [](const XGameSaveContainerInfo*, void* ctx) -> bool {
                ++*static_cast<int*>(ctx);
                return true;
            };
            XGameSaveBlobInfoCallback* count_blob = [](const XGameSaveBlobInfo*, void* ctx) -> bool {
                ++*static_cast<int*>(ctx);
                return true;
            };
            CHECK(LoadVtableSlot<FnContainersByName>(save, 0x68)(save, provider, "slot b", &hits, count_container) ==
                  XGR_S_OK);
            CHECK(hits == 1);
            CHECK(LoadVtableSlot<FnContainersByName>(save, 0x78)(save, provider, "slot ", &hits, count_container) ==
                  XGR_S_OK);
            CHECK(hits == 3);  // "slot a" is stored as slot_a and "slot b" as slot_b
            CHECK(LoadVtableSlot<FnBlobsByName>(save, 0x98)(save, spaced, "high s", &hits, count_blob) == XGR_S_OK);
            CHECK(hits == 4);
            LoadVtableSlot<FnClose>(save, 0x88)(save, spaced);
            CHECK(LoadVtableSlot<FnDelete>(save, 0x50)(save, provider, "slot b") == XGR_S_OK);
        }

        // A null interface id is rejected instead of being read.
        {
            void* none = &save;
            CHECK(LoadVtableSlot<FnQueryInterface>(save, 0x00)(save, nullptr, &none) == XGR_E_POINTER);
            CHECK(none == nullptr);
        }

        // XGameSaveDeleteContainerAsync is in the table but not implemented.
        CHECK(LoadVtableSlot<FnOther>(save, 0x58)(save, 0, 0, 0) == static_cast<XGameResult>(0x80004001u));

        LoadVtableSlot<FnClose>(save, 0x88)(save, container);
        CHECK(LoadVtableSlot<FnDelete>(save, 0x50)(save, provider, "slot_a") == XGR_S_OK);
        seen = Seen{};
        CHECK(LoadVtableSlot<FnContainersByName>(save, 0x68)(save, provider, "slot_a", &seen, on_container) == XGR_S_OK);
        CHECK(seen.containers == 0);
        LoadVtableSlot<FnClose>(save, 0x30)(save, provider);
        CHECK(LoadVtableSlot<FnRelease>(save, 0x10)(save) == 0u);
        LoadVtableSlot<FnClose>(xuser, 0x20)(xuser, user);
        CHECK(LoadVtableSlot<FnRelease>(xuser, 0x10)(xuser) == 0u);
    }

    UninitializeApiImpl();
    std::cout << "[test_query_api_impl] PASSED\n";
    return 0;
}
