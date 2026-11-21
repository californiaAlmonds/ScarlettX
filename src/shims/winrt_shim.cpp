#define SCARLETTX_BUILDING_WINRT 1
#include "scarlettx/winrt/winrt_abi.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
// Minimal declaration for CoTaskMemAlloc (in ole32.lib) without pulling WinRT SDK headers.
extern "C" __declspec(dllimport) void* __stdcall CoTaskMemAlloc(size_t cb);
#endif

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// HSTRING Internal Representation (exact 24-byte x64 ABI header)
// ---------------------------------------------------------------------------
constexpr uint32_t kHStringMagicMask = 0xFFFF0000u;
constexpr uint32_t kHStringMagic = 0x53580000u;
constexpr uint32_t kHStringFlagRef = 0x00000001u;

struct SxHStringImpl {
    uint32_t flags;
    uint32_t length;
    std::atomic<int32_t> ref_count;
    uint32_t reserved;
    const uint16_t* buffer;
};

static_assert(sizeof(SxHStringImpl) == sizeof(SX_HSTRING_HEADER),
              "SxHStringImpl must fit in 24-byte SX_HSTRING_HEADER");
static_assert(alignof(SxHStringImpl) <= alignof(SX_HSTRING_HEADER),
              "SxHStringImpl alignment must fit SX_HSTRING_HEADER");

const uint16_t kEmptyUtf16[1] = {0};

const SxHStringImpl* AsImpl(SX_HSTRING h) {
    return reinterpret_cast<const SxHStringImpl*>(h);
}

SxHStringImpl* AsImplMut(SX_HSTRING h) {
    return reinterpret_cast<SxHStringImpl*>(h);
}

std::u16string HStringToU16(SX_HSTRING h) {
    uint32_t len = 0;
    const uint16_t* raw = WindowsGetStringRawBuffer(h, &len);
    std::u16string out;
    out.resize(len);
    if (len > 0) {
        std::memcpy(out.data(), raw, static_cast<size_t>(len) * sizeof(uint16_t));
    }
    return out;
}

int32_t CreateHStringFromU16(const std::u16string& text, SX_HSTRING* out) {
    return WindowsCreateString(
        reinterpret_cast<const uint16_t*>(text.data()),
        static_cast<uint32_t>(text.size()),
        out);
}

std::u16string PathToU16(const fs::path& p) {
    const std::u16string u16 = p.u16string();
    return u16;
}

// ---------------------------------------------------------------------------
// COM / GUID Helpers
// ---------------------------------------------------------------------------
bool GuidEquals(const SX_GUID* a, const SX_GUID& b) {
    if (!a) return false;
    return std::memcmp(a, &b, sizeof(SX_GUID)) == 0;
}

void* AllocComMemory(size_t bytes) {
#if defined(_WIN32)
    return CoTaskMemAlloc(bytes);
#else
    return std::malloc(bytes);
#endif
}

int32_t CopyIids(const SX_GUID* list, uint32_t count, uint32_t* outCount, SX_GUID** outIids) {
    if (!outCount || !outIids) {
        return SX_WINRT_E_POINTER;
    }
    *outCount = 0;
    *outIids = nullptr;
    if (count == 0) {
        return SX_WINRT_S_OK;
    }
    auto* mem = static_cast<SX_GUID*>(AllocComMemory(sizeof(SX_GUID) * count));
    if (!mem) {
        return SX_WINRT_E_OUTOFMEMORY;
    }
    std::memcpy(mem, list, sizeof(SX_GUID) * count);
    *outCount = count;
    *outIids = mem;
    return SX_WINRT_S_OK;
}

// ---------------------------------------------------------------------------
// Per-Thread WinRT Apartment State (RoInitialize / RoUninitialize)
// ---------------------------------------------------------------------------
struct ThreadRoState {
    uint32_t init_count = 0;
    uint32_t init_type = SX_RO_INIT_MULTITHREADED;
};

thread_local ThreadRoState g_tls_ro_state;

// ---------------------------------------------------------------------------
// Custom Activation Factory Registry
// ---------------------------------------------------------------------------
struct RegisteredFactoryEntry {
    uint64_t cookie = 0;
    std::u16string class_id;
    SX_PFNGETACTIVATIONFACTORY callback = nullptr;
};

struct WinRtRegistry {
    std::mutex mutex;
    uint64_t next_cookie = 1;
    std::vector<RegisteredFactoryEntry> custom_factories;
};

WinRtRegistry& GetRegistry() {
    static WinRtRegistry r;
    return r;
}

// ---------------------------------------------------------------------------
// Built-in Class 1: Windows.System.Profile.AnalyticsInfo & AnalyticsVersionInfo
// ---------------------------------------------------------------------------
struct AnalyticsVersionInfoObj {
    const SX_IAnalyticsVersionInfoVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    std::u16string runtime_class_name;
};

int32_t SCARLETTX_WINRT_CALL Avi_QueryInterface(
    SX_IAnalyticsVersionInfo* self_iface,
    const SX_GUID* riid,
    void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!self_iface || !riid) return SX_WINRT_E_INVALIDARG;
    auto* self = reinterpret_cast<AnalyticsVersionInfoObj*>(self_iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IAnalyticsVersionInfo)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}

uint32_t SCARLETTX_WINRT_CALL Avi_AddRef(SX_IAnalyticsVersionInfo* self_iface) {
    if (!self_iface) return 0;
    auto* self = reinterpret_cast<AnalyticsVersionInfoObj*>(self_iface);
    return self->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t SCARLETTX_WINRT_CALL Avi_Release(SX_IAnalyticsVersionInfo* self_iface) {
    if (!self_iface) return 0;
    auto* self = reinterpret_cast<AnalyticsVersionInfoObj*>(self_iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        delete self;
    }
    return rem;
}

int32_t SCARLETTX_WINRT_CALL Avi_GetIids(
    SX_IAnalyticsVersionInfo*,
    uint32_t* count,
    SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IAnalyticsVersionInfo};
    return CopyIids(kIids, 1, count, iids);
}

int32_t SCARLETTX_WINRT_CALL Avi_GetRuntimeClassName(
    SX_IAnalyticsVersionInfo* self_iface,
    SX_HSTRING* className) {
    if (!self_iface || !className) return SX_WINRT_E_POINTER;
    auto* self = reinterpret_cast<AnalyticsVersionInfoObj*>(self_iface);
    return CreateHStringFromU16(self->runtime_class_name, className);
}

int32_t SCARLETTX_WINRT_CALL Avi_GetTrustLevel(
    SX_IAnalyticsVersionInfo*,
    SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}

int32_t SCARLETTX_WINRT_CALL Avi_GetDeviceFamily(
    SX_IAnalyticsVersionInfo*,
    SX_HSTRING* value) {
    if (!value) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Xbox", value);
}

int32_t SCARLETTX_WINRT_CALL Avi_GetDeviceFamilyVersion(
    SX_IAnalyticsVersionInfo*,
    SX_HSTRING* value) {
    if (!value) return SX_WINRT_E_POINTER;
    // Encodes Xbox OS 10.0.26100.2499 as ((10<<48)|(0<<32)|(26100<<16)|2499).
    return CreateHStringFromU16(u"2814751477203968", value);
}

const SX_IAnalyticsVersionInfoVtbl kAviVtbl = {
    &Avi_QueryInterface,
    &Avi_AddRef,
    &Avi_Release,
    &Avi_GetIids,
    &Avi_GetRuntimeClassName,
    &Avi_GetTrustLevel,
    &Avi_GetDeviceFamily,
    &Avi_GetDeviceFamilyVersion,
};

AnalyticsVersionInfoObj* CreateAnalyticsObj(std::u16string class_name) {
    auto* obj = new (std::nothrow) AnalyticsVersionInfoObj();
    if (!obj) return nullptr;
    obj->lpVtbl = &kAviVtbl;
    obj->runtime_class_name = std::move(class_name);
    return obj;
}

struct AnalyticsInfoFactoryObj {
    const SX_IActivationFactoryVtbl* activation_vtbl;
    const SX_IAnalyticsInfoStaticsVtbl* statics_vtbl;
    std::atomic<uint32_t> ref_count{1};
};

int32_t AnalyticsFactory_QI_Common(AnalyticsInfoFactoryObj* self, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!self || !riid) return SX_WINRT_E_INVALIDARG;
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IActivationFactory)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->activation_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, SX_IID_IAnalyticsInfoStatics)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->statics_vtbl;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}

uint32_t AnalyticsFactory_AddRef_Common(AnalyticsInfoFactoryObj* self) {
    return self->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t AnalyticsFactory_Release_Common(AnalyticsInfoFactoryObj* self) {
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}

int32_t SCARLETTX_WINRT_CALL Af_QueryInterface(SX_IActivationFactory* iface, const SX_GUID* riid, void** ppv) {
    return AnalyticsFactory_QI_Common(reinterpret_cast<AnalyticsInfoFactoryObj*>(iface), riid, ppv);
}
uint32_t SCARLETTX_WINRT_CALL Af_AddRef(SX_IActivationFactory* iface) {
    return AnalyticsFactory_AddRef_Common(reinterpret_cast<AnalyticsInfoFactoryObj*>(iface));
}
uint32_t SCARLETTX_WINRT_CALL Af_Release(SX_IActivationFactory* iface) {
    return AnalyticsFactory_Release_Common(reinterpret_cast<AnalyticsInfoFactoryObj*>(iface));
}
int32_t SCARLETTX_WINRT_CALL Af_GetIids(SX_IActivationFactory*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, SX_IID_IAnalyticsInfoStatics};
    return CopyIids(kIids, 2, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Af_GetRuntimeClassName(SX_IActivationFactory*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.System.Profile.AnalyticsInfo", name);
}
int32_t SCARLETTX_WINRT_CALL Af_GetTrustLevel(SX_IActivationFactory*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Af_ActivateInstance(SX_IActivationFactory*, SX_IInspectable** instance) {
    if (!instance) return SX_WINRT_E_POINTER;
    auto* obj = CreateAnalyticsObj(u"Windows.System.Profile.AnalyticsInfo");
    if (!obj) {
        *instance = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *instance = reinterpret_cast<SX_IInspectable*>(obj);
    return SX_WINRT_S_OK;
}

AnalyticsInfoFactoryObj* FromAnalyticsStatics(SX_IAnalyticsInfoStatics* iface) {
    return reinterpret_cast<AnalyticsInfoFactoryObj*>(
        reinterpret_cast<uint8_t*>(iface) - offsetof(AnalyticsInfoFactoryObj, statics_vtbl));
}
int32_t SCARLETTX_WINRT_CALL As_QueryInterface(SX_IAnalyticsInfoStatics* iface, const SX_GUID* riid, void** ppv) {
    return AnalyticsFactory_QI_Common(FromAnalyticsStatics(iface), riid, ppv);
}
uint32_t SCARLETTX_WINRT_CALL As_AddRef(SX_IAnalyticsInfoStatics* iface) {
    return AnalyticsFactory_AddRef_Common(FromAnalyticsStatics(iface));
}
uint32_t SCARLETTX_WINRT_CALL As_Release(SX_IAnalyticsInfoStatics* iface) {
    return AnalyticsFactory_Release_Common(FromAnalyticsStatics(iface));
}
int32_t SCARLETTX_WINRT_CALL As_GetIids(SX_IAnalyticsInfoStatics*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, SX_IID_IAnalyticsInfoStatics};
    return CopyIids(kIids, 2, count, iids);
}
int32_t SCARLETTX_WINRT_CALL As_GetRuntimeClassName(SX_IAnalyticsInfoStatics*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.System.Profile.AnalyticsInfo", name);
}
int32_t SCARLETTX_WINRT_CALL As_GetTrustLevel(SX_IAnalyticsInfoStatics*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL As_GetVersionInfo(SX_IAnalyticsInfoStatics*, SX_IAnalyticsVersionInfo** value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto* obj = CreateAnalyticsObj(u"Windows.System.Profile.AnalyticsVersionInfo");
    if (!obj) {
        *value = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *value = reinterpret_cast<SX_IAnalyticsVersionInfo*>(obj);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL As_GetDeviceForm(SX_IAnalyticsInfoStatics*, SX_HSTRING* value) {
    if (!value) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Xbox Series X", value);
}

const SX_IActivationFactoryVtbl kAnalyticsFactoryVtbl = {
    &Af_QueryInterface, &Af_AddRef, &Af_Release,
    &Af_GetIids, &Af_GetRuntimeClassName, &Af_GetTrustLevel,
    &Af_ActivateInstance,
};

const SX_IAnalyticsInfoStaticsVtbl kAnalyticsStaticsVtbl = {
    &As_QueryInterface, &As_AddRef, &As_Release,
    &As_GetIids, &As_GetRuntimeClassName, &As_GetTrustLevel,
    &As_GetVersionInfo, &As_GetDeviceForm,
};

// ---------------------------------------------------------------------------
// Built-in Class 2: Windows.Storage.ApplicationData & StorageFolder
// ---------------------------------------------------------------------------
fs::path ResolveWinRtAppDataFolder(const char* leaf) {
    const char* env_root = std::getenv("SCARLETTX_SAVE_ROOT");
    fs::path base = (env_root && *env_root)
                        ? fs::path(env_root)
                        : (fs::temp_directory_path() / "scarlettx_saves");
    fs::path dir = base / "winrt_appdata" / leaf;
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

struct StorageFolderObj {
    const SX_IStorageItemVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    std::u16string name;
    std::u16string path;
};

int32_t SCARLETTX_WINRT_CALL Sf_QueryInterface(SX_IStorageItem* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    auto* self = reinterpret_cast<StorageFolderObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IStorageItem)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Sf_AddRef(SX_IStorageItem* iface) {
    return reinterpret_cast<StorageFolderObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Sf_Release(SX_IStorageItem* iface) {
    auto* self = reinterpret_cast<StorageFolderObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Sf_GetIids(SX_IStorageItem*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IStorageItem};
    return CopyIids(kIids, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Sf_GetRuntimeClassName(SX_IStorageItem*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Storage.StorageFolder", name);
}
int32_t SCARLETTX_WINRT_CALL Sf_GetTrustLevel(SX_IStorageItem*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Sf_NotImpl2(SX_IStorageItem*, SX_HSTRING, void**) { return SX_WINRT_E_NOTIMPL; }
int32_t SCARLETTX_WINRT_CALL Sf_NotImpl3(SX_IStorageItem*, SX_HSTRING, uint32_t, void**) { return SX_WINRT_E_NOTIMPL; }
int32_t SCARLETTX_WINRT_CALL Sf_NotImpl1(SX_IStorageItem*, void**) { return SX_WINRT_E_NOTIMPL; }
int32_t SCARLETTX_WINRT_CALL Sf_NotImpl1u(SX_IStorageItem*, uint32_t, void**) { return SX_WINRT_E_NOTIMPL; }
int32_t SCARLETTX_WINRT_CALL Sf_GetName(SX_IStorageItem* iface, SX_HSTRING* value) {
    if (!value) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(reinterpret_cast<StorageFolderObj*>(iface)->name, value);
}
int32_t SCARLETTX_WINRT_CALL Sf_GetPath(SX_IStorageItem* iface, SX_HSTRING* value) {
    if (!value) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(reinterpret_cast<StorageFolderObj*>(iface)->path, value);
}

const SX_IStorageItemVtbl kStorageFolderVtbl = {
    &Sf_QueryInterface, &Sf_AddRef, &Sf_Release,
    &Sf_GetIids, &Sf_GetRuntimeClassName, &Sf_GetTrustLevel,
    &Sf_NotImpl2, &Sf_NotImpl3, &Sf_NotImpl1, &Sf_NotImpl1u, &Sf_NotImpl1,
    &Sf_GetName, &Sf_GetPath,
};

StorageFolderObj* CreateStorageFolder(const char* leaf) {
    auto* obj = new (std::nothrow) StorageFolderObj();
    if (!obj) return nullptr;
    obj->lpVtbl = &kStorageFolderVtbl;
    const fs::path p = ResolveWinRtAppDataFolder(leaf);
    obj->name = PathToU16(fs::path(leaf));
    obj->path = PathToU16(p);
    return obj;
}

struct GenericInspectableObj {
    const SX_IInspectableVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    std::u16string class_name;
};

int32_t SCARLETTX_WINRT_CALL Gi_QueryInterface(SX_IInspectable* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    auto* self = reinterpret_cast<GenericInspectableObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Gi_AddRef(SX_IInspectable* iface) {
    return reinterpret_cast<GenericInspectableObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Gi_Release(SX_IInspectable* iface) {
    auto* self = reinterpret_cast<GenericInspectableObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Gi_GetIids(SX_IInspectable*, uint32_t* count, SX_GUID** iids) {
    return CopyIids(nullptr, 0, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Gi_GetRuntimeClassName(SX_IInspectable* iface, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(reinterpret_cast<GenericInspectableObj*>(iface)->class_name, name);
}
int32_t SCARLETTX_WINRT_CALL Gi_GetTrustLevel(SX_IInspectable*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
const SX_IInspectableVtbl kGenericInspectableVtbl = {
    &Gi_QueryInterface, &Gi_AddRef, &Gi_Release,
    &Gi_GetIids, &Gi_GetRuntimeClassName, &Gi_GetTrustLevel,
};

struct ApplicationDataObj {
    const SX_IApplicationDataVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
};

int32_t SCARLETTX_WINRT_CALL Ad_QueryInterface(SX_IApplicationData* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    auto* self = reinterpret_cast<ApplicationDataObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IApplicationData)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Ad_AddRef(SX_IApplicationData* iface) {
    return reinterpret_cast<ApplicationDataObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Ad_Release(SX_IApplicationData* iface) {
    auto* self = reinterpret_cast<ApplicationDataObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Ad_GetIids(SX_IApplicationData*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IApplicationData};
    return CopyIids(kIids, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Ad_GetRuntimeClassName(SX_IApplicationData*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Storage.ApplicationData", name);
}
int32_t SCARLETTX_WINRT_CALL Ad_GetTrustLevel(SX_IApplicationData*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Ad_GetVersion(SX_IApplicationData*, uint32_t* value) {
    if (!value) return SX_WINRT_E_POINTER;
    *value = 1;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Ad_SetVersionAsync(SX_IApplicationData*, uint32_t, void*, void**) {
    return SX_WINRT_E_NOTIMPL;
}
int32_t SCARLETTX_WINRT_CALL Ad_ClearAllAsync(SX_IApplicationData*, void**) {
    return SX_WINRT_E_NOTIMPL;
}
int32_t SCARLETTX_WINRT_CALL Ad_ClearAsync(SX_IApplicationData*, uint32_t, void**) {
    return SX_WINRT_E_NOTIMPL;
}
int32_t SCARLETTX_WINRT_CALL Ad_GetSettings(SX_IApplicationData*, SX_IInspectable** value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto* obj = new (std::nothrow) GenericInspectableObj();
    if (!obj) {
        *value = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    obj->lpVtbl = &kGenericInspectableVtbl;
    obj->class_name = u"Windows.Storage.ApplicationDataContainer";
    *value = reinterpret_cast<SX_IInspectable*>(obj);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Ad_GetLocalFolder(SX_IApplicationData*, SX_IStorageItem** value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto* f = CreateStorageFolder("LocalState");
    if (!f) {
        *value = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *value = reinterpret_cast<SX_IStorageItem*>(f);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Ad_GetRoamingFolder(SX_IApplicationData*, SX_IStorageItem** value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto* f = CreateStorageFolder("RoamingState");
    if (!f) {
        *value = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *value = reinterpret_cast<SX_IStorageItem*>(f);
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Ad_GetTempFolder(SX_IApplicationData*, SX_IStorageItem** value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto* f = CreateStorageFolder("TempState");
    if (!f) {
        *value = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *value = reinterpret_cast<SX_IStorageItem*>(f);
    return SX_WINRT_S_OK;
}

const SX_IApplicationDataVtbl kApplicationDataVtbl = {
    &Ad_QueryInterface, &Ad_AddRef, &Ad_Release,
    &Ad_GetIids, &Ad_GetRuntimeClassName, &Ad_GetTrustLevel,
    &Ad_GetVersion, &Ad_SetVersionAsync, &Ad_ClearAllAsync, &Ad_ClearAsync,
    &Ad_GetSettings, &Ad_GetSettings,
    &Ad_GetLocalFolder, &Ad_GetRoamingFolder, &Ad_GetTempFolder,
};

ApplicationDataObj* CreateApplicationData() {
    auto* obj = new (std::nothrow) ApplicationDataObj();
    if (!obj) return nullptr;
    obj->lpVtbl = &kApplicationDataVtbl;
    return obj;
}

struct ApplicationDataFactoryObj {
    const SX_IActivationFactoryVtbl* activation_vtbl;
    const SX_IApplicationDataStaticsVtbl* statics_vtbl;
    std::atomic<uint32_t> ref_count{1};
};

int32_t AdFactory_QI_Common(ApplicationDataFactoryObj* self, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!self || !riid) return SX_WINRT_E_INVALIDARG;
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IActivationFactory)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->activation_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, SX_IID_IApplicationDataStatics)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->statics_vtbl;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t AdFactory_AddRef_Common(ApplicationDataFactoryObj* self) {
    return self->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t AdFactory_Release_Common(ApplicationDataFactoryObj* self) {
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}

int32_t SCARLETTX_WINRT_CALL Adf_QueryInterface(SX_IActivationFactory* iface, const SX_GUID* riid, void** ppv) {
    return AdFactory_QI_Common(reinterpret_cast<ApplicationDataFactoryObj*>(iface), riid, ppv);
}
uint32_t SCARLETTX_WINRT_CALL Adf_AddRef(SX_IActivationFactory* iface) {
    return AdFactory_AddRef_Common(reinterpret_cast<ApplicationDataFactoryObj*>(iface));
}
uint32_t SCARLETTX_WINRT_CALL Adf_Release(SX_IActivationFactory* iface) {
    return AdFactory_Release_Common(reinterpret_cast<ApplicationDataFactoryObj*>(iface));
}
int32_t SCARLETTX_WINRT_CALL Adf_GetIids(SX_IActivationFactory*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, SX_IID_IApplicationDataStatics};
    return CopyIids(kIids, 2, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Adf_GetRuntimeClassName(SX_IActivationFactory*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Storage.ApplicationData", name);
}
int32_t SCARLETTX_WINRT_CALL Adf_GetTrustLevel(SX_IActivationFactory*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Adf_ActivateInstance(SX_IActivationFactory*, SX_IInspectable** instance) {
    if (!instance) return SX_WINRT_E_POINTER;
    auto* obj = CreateApplicationData();
    if (!obj) {
        *instance = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *instance = reinterpret_cast<SX_IInspectable*>(obj);
    return SX_WINRT_S_OK;
}

ApplicationDataFactoryObj* FromAdStatics(SX_IApplicationDataStatics* iface) {
    return reinterpret_cast<ApplicationDataFactoryObj*>(
        reinterpret_cast<uint8_t*>(iface) - offsetof(ApplicationDataFactoryObj, statics_vtbl));
}
int32_t SCARLETTX_WINRT_CALL Ads_QueryInterface(SX_IApplicationDataStatics* iface, const SX_GUID* riid, void** ppv) {
    return AdFactory_QI_Common(FromAdStatics(iface), riid, ppv);
}
uint32_t SCARLETTX_WINRT_CALL Ads_AddRef(SX_IApplicationDataStatics* iface) {
    return AdFactory_AddRef_Common(FromAdStatics(iface));
}
uint32_t SCARLETTX_WINRT_CALL Ads_Release(SX_IApplicationDataStatics* iface) {
    return AdFactory_Release_Common(FromAdStatics(iface));
}
int32_t SCARLETTX_WINRT_CALL Ads_GetIids(SX_IApplicationDataStatics*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, SX_IID_IApplicationDataStatics};
    return CopyIids(kIids, 2, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Ads_GetRuntimeClassName(SX_IApplicationDataStatics*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Storage.ApplicationData", name);
}
int32_t SCARLETTX_WINRT_CALL Ads_GetTrustLevel(SX_IApplicationDataStatics*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Ads_GetCurrent(SX_IApplicationDataStatics*, SX_IApplicationData** value) {
    if (!value) return SX_WINRT_E_POINTER;
    auto* obj = CreateApplicationData();
    if (!obj) {
        *value = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *value = reinterpret_cast<SX_IApplicationData*>(obj);
    return SX_WINRT_S_OK;
}

const SX_IActivationFactoryVtbl kAppDataFactoryVtbl = {
    &Adf_QueryInterface, &Adf_AddRef, &Adf_Release,
    &Adf_GetIids, &Adf_GetRuntimeClassName, &Adf_GetTrustLevel,
    &Adf_ActivateInstance,
};
const SX_IApplicationDataStaticsVtbl kAppDataStaticsVtbl = {
    &Ads_QueryInterface, &Ads_AddRef, &Ads_Release,
    &Ads_GetIids, &Ads_GetRuntimeClassName, &Ads_GetTrustLevel,
    &Ads_GetCurrent,
};

// ---------------------------------------------------------------------------
// Built-in Class 3: Windows.Gaming.Input.Gamepad
// ---------------------------------------------------------------------------
struct GamepadObj {
    const SX_IGamepadVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    std::mutex mutex;
    SX_GamepadVibration vibration{};
};

int32_t SCARLETTX_WINRT_CALL Gp_QueryInterface(SX_IGamepad* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    auto* self = reinterpret_cast<GamepadObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IGamepad)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Gp_AddRef(SX_IGamepad* iface) {
    return reinterpret_cast<GamepadObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Gp_Release(SX_IGamepad* iface) {
    auto* self = reinterpret_cast<GamepadObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Gp_GetIids(SX_IGamepad*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IGamepad};
    return CopyIids(kIids, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Gp_GetRuntimeClassName(SX_IGamepad*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Gaming.Input.Gamepad", name);
}
int32_t SCARLETTX_WINRT_CALL Gp_GetTrustLevel(SX_IGamepad*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Gp_GetVibration(SX_IGamepad* iface, SX_GamepadVibration* value) {
    if (!iface || !value) return SX_WINRT_E_POINTER;
    auto* self = reinterpret_cast<GamepadObj*>(iface);
    std::lock_guard<std::mutex> lk(self->mutex);
    *value = self->vibration;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Gp_PutVibration(SX_IGamepad* iface, SX_GamepadVibration value) {
    if (!iface) return SX_WINRT_E_INVALIDARG;
    auto* self = reinterpret_cast<GamepadObj*>(iface);
    std::lock_guard<std::mutex> lk(self->mutex);
    self->vibration = value;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Gp_GetCurrentReading(SX_IGamepad*, SX_GamepadReading* value) {
    if (!value) return SX_WINRT_E_POINTER;
    SX_GamepadReading r{};
    r.Timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    *value = r;
    return SX_WINRT_S_OK;
}

const SX_IGamepadVtbl kGamepadVtbl = {
    &Gp_QueryInterface, &Gp_AddRef, &Gp_Release,
    &Gp_GetIids, &Gp_GetRuntimeClassName, &Gp_GetTrustLevel,
    &Gp_GetVibration, &Gp_PutVibration, &Gp_GetCurrentReading,
};

struct GamepadFactoryObj {
    const SX_IActivationFactoryVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
};

int32_t SCARLETTX_WINRT_CALL Gpf_QueryInterface(SX_IActivationFactory* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    auto* self = reinterpret_cast<GamepadFactoryObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IActivationFactory)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Gpf_AddRef(SX_IActivationFactory* iface) {
    return reinterpret_cast<GamepadFactoryObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Gpf_Release(SX_IActivationFactory* iface) {
    auto* self = reinterpret_cast<GamepadFactoryObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Gpf_GetIids(SX_IActivationFactory*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory};
    return CopyIids(kIids, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Gpf_GetRuntimeClassName(SX_IActivationFactory*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Gaming.Input.Gamepad", name);
}
int32_t SCARLETTX_WINRT_CALL Gpf_GetTrustLevel(SX_IActivationFactory*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Gpf_ActivateInstance(SX_IActivationFactory*, SX_IInspectable** instance) {
    if (!instance) return SX_WINRT_E_POINTER;
    auto* obj = new (std::nothrow) GamepadObj();
    if (!obj) {
        *instance = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    obj->lpVtbl = &kGamepadVtbl;
    *instance = reinterpret_cast<SX_IInspectable*>(obj);
    return SX_WINRT_S_OK;
}

const SX_IActivationFactoryVtbl kGamepadFactoryVtbl = {
    &Gpf_QueryInterface, &Gpf_AddRef, &Gpf_Release,
    &Gpf_GetIids, &Gpf_GetRuntimeClassName, &Gpf_GetTrustLevel,
    &Gpf_ActivateInstance,
};

// ---------------------------------------------------------------------------
// Built-in Class 4: Windows.Globalization.Language
// ---------------------------------------------------------------------------
struct LanguageObj {
    const SX_ILanguageVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    std::u16string tag = u"en-US";
};

int32_t SCARLETTX_WINRT_CALL Lang_QueryInterface(SX_ILanguage* iface, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!iface || !riid) return SX_WINRT_E_INVALIDARG;
    auto* self = reinterpret_cast<LanguageObj*>(iface);
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_ILanguage)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Lang_AddRef(SX_ILanguage* iface) {
    return reinterpret_cast<LanguageObj*>(iface)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Lang_Release(SX_ILanguage* iface) {
    auto* self = reinterpret_cast<LanguageObj*>(iface);
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}
int32_t SCARLETTX_WINRT_CALL Lang_GetIids(SX_ILanguage*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_ILanguage};
    return CopyIids(kIids, 1, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Lang_GetRuntimeClassName(SX_ILanguage*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Globalization.Language", name);
}
int32_t SCARLETTX_WINRT_CALL Lang_GetTrustLevel(SX_ILanguage*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Lang_GetLanguageTag(SX_ILanguage* iface, SX_HSTRING* value) {
    if (!iface || !value) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(reinterpret_cast<LanguageObj*>(iface)->tag, value);
}
int32_t SCARLETTX_WINRT_CALL Lang_GetDisplayName(SX_ILanguage* iface, SX_HSTRING* value) {
    if (!iface || !value) return SX_WINRT_E_POINTER;
    auto* self = reinterpret_cast<LanguageObj*>(iface);
    if (self->tag == u"en-US") {
        return CreateHStringFromU16(u"English (United States)", value);
    }
    return CreateHStringFromU16(self->tag, value);
}
int32_t SCARLETTX_WINRT_CALL Lang_GetNativeName(SX_ILanguage* iface, SX_HSTRING* value) {
    return Lang_GetDisplayName(iface, value);
}
int32_t SCARLETTX_WINRT_CALL Lang_GetScript(SX_ILanguage*, SX_HSTRING* value) {
    if (!value) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Latn", value);
}

const SX_ILanguageVtbl kLanguageVtbl = {
    &Lang_QueryInterface, &Lang_AddRef, &Lang_Release,
    &Lang_GetIids, &Lang_GetRuntimeClassName, &Lang_GetTrustLevel,
    &Lang_GetLanguageTag, &Lang_GetDisplayName, &Lang_GetNativeName, &Lang_GetScript,
};

LanguageObj* CreateLanguageObj(std::u16string tag) {
    auto* obj = new (std::nothrow) LanguageObj();
    if (!obj) return nullptr;
    obj->lpVtbl = &kLanguageVtbl;
    obj->tag = tag.empty() ? u"en-US" : std::move(tag);
    return obj;
}

struct LanguageFactoryObj {
    const SX_IActivationFactoryVtbl* activation_vtbl;
    const SX_ILanguageFactoryVtbl* lang_factory_vtbl;
    std::atomic<uint32_t> ref_count{1};
};

int32_t LangFactory_QI_Common(LanguageFactoryObj* self, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (!self || !riid) return SX_WINRT_E_INVALIDARG;
    if (GuidEquals(riid, SX_IID_IUnknown) ||
        GuidEquals(riid, SX_IID_IInspectable) ||
        GuidEquals(riid, SX_IID_IAgileObject) ||
        GuidEquals(riid, SX_IID_IActivationFactory)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->activation_vtbl;
        return SX_WINRT_S_OK;
    }
    if (GuidEquals(riid, SX_IID_ILanguageFactory)) {
        self->ref_count.fetch_add(1, std::memory_order_relaxed);
        *ppv = &self->lang_factory_vtbl;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t LangFactory_AddRef_Common(LanguageFactoryObj* self) {
    return self->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint32_t LangFactory_Release_Common(LanguageFactoryObj* self) {
    const uint32_t rem = self->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete self;
    return rem;
}

int32_t SCARLETTX_WINRT_CALL Lf_QueryInterface(SX_IActivationFactory* iface, const SX_GUID* riid, void** ppv) {
    return LangFactory_QI_Common(reinterpret_cast<LanguageFactoryObj*>(iface), riid, ppv);
}
uint32_t SCARLETTX_WINRT_CALL Lf_AddRef(SX_IActivationFactory* iface) {
    return LangFactory_AddRef_Common(reinterpret_cast<LanguageFactoryObj*>(iface));
}
uint32_t SCARLETTX_WINRT_CALL Lf_Release(SX_IActivationFactory* iface) {
    return LangFactory_Release_Common(reinterpret_cast<LanguageFactoryObj*>(iface));
}
int32_t SCARLETTX_WINRT_CALL Lf_GetIids(SX_IActivationFactory*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, SX_IID_ILanguageFactory};
    return CopyIids(kIids, 2, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Lf_GetRuntimeClassName(SX_IActivationFactory*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Globalization.Language", name);
}
int32_t SCARLETTX_WINRT_CALL Lf_GetTrustLevel(SX_IActivationFactory*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Lf_ActivateInstance(SX_IActivationFactory*, SX_IInspectable** instance) {
    if (!instance) return SX_WINRT_E_POINTER;
    auto* obj = CreateLanguageObj(u"en-US");
    if (!obj) {
        *instance = nullptr;
        return SX_WINRT_E_OUTOFMEMORY;
    }
    *instance = reinterpret_cast<SX_IInspectable*>(obj);
    return SX_WINRT_S_OK;
}

LanguageFactoryObj* FromLangFactory(SX_ILanguageFactory* iface) {
    return reinterpret_cast<LanguageFactoryObj*>(
        reinterpret_cast<uint8_t*>(iface) - offsetof(LanguageFactoryObj, lang_factory_vtbl));
}
int32_t SCARLETTX_WINRT_CALL Lfc_QueryInterface(SX_ILanguageFactory* iface, const SX_GUID* riid, void** ppv) {
    return LangFactory_QI_Common(FromLangFactory(iface), riid, ppv);
}
uint32_t SCARLETTX_WINRT_CALL Lfc_AddRef(SX_ILanguageFactory* iface) {
    return LangFactory_AddRef_Common(FromLangFactory(iface));
}
uint32_t SCARLETTX_WINRT_CALL Lfc_Release(SX_ILanguageFactory* iface) {
    return LangFactory_Release_Common(FromLangFactory(iface));
}
int32_t SCARLETTX_WINRT_CALL Lfc_GetIids(SX_ILanguageFactory*, uint32_t* count, SX_GUID** iids) {
    static const SX_GUID kIids[] = {SX_IID_IActivationFactory, SX_IID_ILanguageFactory};
    return CopyIids(kIids, 2, count, iids);
}
int32_t SCARLETTX_WINRT_CALL Lfc_GetRuntimeClassName(SX_ILanguageFactory*, SX_HSTRING* name) {
    if (!name) return SX_WINRT_E_POINTER;
    return CreateHStringFromU16(u"Windows.Globalization.Language", name);
}
int32_t SCARLETTX_WINRT_CALL Lfc_GetTrustLevel(SX_ILanguageFactory*, SX_TrustLevel* level) {
    if (!level) return SX_WINRT_E_POINTER;
    *level = SX_TrustLevel_BaseTrust;
    return SX_WINRT_S_OK;
}
int32_t SCARLETTX_WINRT_CALL Lfc_CreateLanguage(
    SX_ILanguageFactory*,
    SX_HSTRING languageTag,
    SX_ILanguage** result) {
    if (!result) return SX_WINRT_E_POINTER;
    *result = nullptr;
    if (WindowsIsStringEmpty(languageTag)) {
        return SX_WINRT_E_INVALIDARG;
    }
    auto* obj = CreateLanguageObj(HStringToU16(languageTag));
    if (!obj) return SX_WINRT_E_OUTOFMEMORY;
    *result = reinterpret_cast<SX_ILanguage*>(obj);
    return SX_WINRT_S_OK;
}

const SX_IActivationFactoryVtbl kLanguageFactoryVtbl = {
    &Lf_QueryInterface, &Lf_AddRef, &Lf_Release,
    &Lf_GetIids, &Lf_GetRuntimeClassName, &Lf_GetTrustLevel,
    &Lf_ActivateInstance,
};
const SX_ILanguageFactoryVtbl kLanguageCustomFactoryVtbl = {
    &Lfc_QueryInterface, &Lfc_AddRef, &Lfc_Release,
    &Lfc_GetIids, &Lfc_GetRuntimeClassName, &Lfc_GetTrustLevel,
    &Lfc_CreateLanguage,
};

// Built-in factory resolver
int32_t CreateBuiltInFactory(const std::u16string& class_id, const SX_GUID* iid, void** out_factory) {
    if (class_id == u"Windows.System.Profile.AnalyticsInfo") {
        auto* f = new (std::nothrow) AnalyticsInfoFactoryObj();
        if (!f) return SX_WINRT_E_OUTOFMEMORY;
        f->activation_vtbl = &kAnalyticsFactoryVtbl;
        f->statics_vtbl = &kAnalyticsStaticsVtbl;
        int32_t hr = AnalyticsFactory_QI_Common(f, iid, out_factory);
        AnalyticsFactory_Release_Common(f);
        return hr;
    }
    if (class_id == u"Windows.Storage.ApplicationData") {
        auto* f = new (std::nothrow) ApplicationDataFactoryObj();
        if (!f) return SX_WINRT_E_OUTOFMEMORY;
        f->activation_vtbl = &kAppDataFactoryVtbl;
        f->statics_vtbl = &kAppDataStaticsVtbl;
        int32_t hr = AdFactory_QI_Common(f, iid, out_factory);
        AdFactory_Release_Common(f);
        return hr;
    }
    if (class_id == u"Windows.Gaming.Input.Gamepad") {
        auto* f = new (std::nothrow) GamepadFactoryObj();
        if (!f) return SX_WINRT_E_OUTOFMEMORY;
        f->lpVtbl = &kGamepadFactoryVtbl;
        int32_t hr = Gpf_QueryInterface(reinterpret_cast<SX_IActivationFactory*>(f), iid, out_factory);
        Gpf_Release(reinterpret_cast<SX_IActivationFactory*>(f));
        return hr;
    }
    if (class_id == u"Windows.Globalization.Language") {
        auto* f = new (std::nothrow) LanguageFactoryObj();
        if (!f) return SX_WINRT_E_OUTOFMEMORY;
        f->activation_vtbl = &kLanguageFactoryVtbl;
        f->lang_factory_vtbl = &kLanguageCustomFactoryVtbl;
        int32_t hr = LangFactory_QI_Common(f, iid, out_factory);
        LangFactory_Release_Common(f);
        return hr;
    }
    return SX_WINRT_REGDB_E_CLASSNOTREG;
}

}  // namespace

extern "C" {

// ===========================================================================
// WinRT Activation Runtime API (Ro*)
// ===========================================================================
int32_t RoInitialize(uint32_t initType) {
    if (initType != SX_RO_INIT_SINGLETHREADED && initType != SX_RO_INIT_MULTITHREADED) {
        return SX_WINRT_E_INVALIDARG;
    }
    if (g_tls_ro_state.init_count == 0) {
        g_tls_ro_state.init_count = 1;
        g_tls_ro_state.init_type = initType;
        return SX_WINRT_S_OK;
    }
    if (g_tls_ro_state.init_type != initType) {
        return SX_WINRT_RPC_E_CHANGED_MODE;
    }
    ++g_tls_ro_state.init_count;
    return SX_WINRT_S_FALSE;
}

void RoUninitialize(void) {
    if (g_tls_ro_state.init_count > 0) {
        --g_tls_ro_state.init_count;
    }
}

int32_t RoGetActivationFactory(
    SX_HSTRING activatableClassId,
    const SX_GUID* iid,
    void** factory) {
    if (!factory) {
        return SX_WINRT_E_POINTER;
    }
    *factory = nullptr;
    if (g_tls_ro_state.init_count == 0) {
        return SX_WINRT_CO_E_NOTINITIALIZED;
    }
    if (!iid || WindowsIsStringEmpty(activatableClassId)) {
        return SX_WINRT_E_INVALIDARG;
    }

    const std::u16string class_id = HStringToU16(activatableClassId);

    // 1. Check custom runtime-registered activation factories (LIFO precedence).
    SX_PFNGETACTIVATIONFACTORY custom_cb = nullptr;
    {
        WinRtRegistry& reg = GetRegistry();
        std::lock_guard<std::mutex> lk(reg.mutex);
        for (auto it = reg.custom_factories.rbegin(); it != reg.custom_factories.rend(); ++it) {
            if (it->class_id == class_id) {
                custom_cb = it->callback;
                break;
            }
        }
    }

    if (custom_cb) {
        SX_IActivationFactory* raw_factory = nullptr;
        const int32_t hr = custom_cb(activatableClassId, &raw_factory);
        if (hr < 0) {
            return hr;
        }
        if (!raw_factory) {
            return SX_WINRT_E_FAIL;
        }
        const int32_t qi_hr = raw_factory->lpVtbl->QueryInterface(raw_factory, iid, factory);
        raw_factory->lpVtbl->Release(raw_factory);
        return qi_hr;
    }

    // 2. Check built-in Xbox/UWP classes.
    return CreateBuiltInFactory(class_id, iid, factory);
}

int32_t RoActivateInstance(SX_HSTRING activatableClassId, void** instance) {
    if (!instance) {
        return SX_WINRT_E_POINTER;
    }
    *instance = nullptr;
    if (g_tls_ro_state.init_count == 0) {
        return SX_WINRT_CO_E_NOTINITIALIZED;
    }
    if (WindowsIsStringEmpty(activatableClassId)) {
        return SX_WINRT_E_INVALIDARG;
    }

    SX_IActivationFactory* factory = nullptr;
    int32_t hr = RoGetActivationFactory(
        activatableClassId, &SX_IID_IActivationFactory, reinterpret_cast<void**>(&factory));
    if (hr < 0 || !factory) {
        return hr < 0 ? hr : SX_WINRT_E_FAIL;
    }

    SX_IInspectable* insp = nullptr;
    hr = factory->lpVtbl->ActivateInstance(factory, &insp);
    factory->lpVtbl->Release(factory);
    if (hr < 0) {
        return hr;
    }
    *instance = insp;
    return SX_WINRT_S_OK;
}

int32_t RoRegisterActivationFactories(
    SX_HSTRING* activatableClassIds,
    SX_PFNGETACTIVATIONFACTORY* activationFactoryCallbacks,
    uint32_t count,
    uint64_t* cookie) {
    if (!cookie) {
        return SX_WINRT_E_POINTER;
    }
    *cookie = 0;
    if (g_tls_ro_state.init_count == 0) {
        return SX_WINRT_CO_E_NOTINITIALIZED;
    }
    if (!activatableClassIds || !activationFactoryCallbacks || count == 0) {
        return SX_WINRT_E_INVALIDARG;
    }

    std::vector<std::pair<std::u16string, SX_PFNGETACTIVATIONFACTORY>> validated;
    validated.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (WindowsIsStringEmpty(activatableClassIds[i]) || !activationFactoryCallbacks[i]) {
            return SX_WINRT_E_INVALIDARG;
        }
        validated.emplace_back(HStringToU16(activatableClassIds[i]), activationFactoryCallbacks[i]);
    }

    WinRtRegistry& reg = GetRegistry();
    std::lock_guard<std::mutex> lk(reg.mutex);
    const uint64_t assigned = reg.next_cookie++;
    for (auto& [cid, cb] : validated) {
        reg.custom_factories.push_back(RegisteredFactoryEntry{assigned, std::move(cid), cb});
    }
    *cookie = assigned;
    return SX_WINRT_S_OK;
}

void RoRevokeActivationFactories(uint64_t cookie) {
    if (cookie == 0) {
        return;
    }
    WinRtRegistry& reg = GetRegistry();
    std::lock_guard<std::mutex> lk(reg.mutex);
    reg.custom_factories.erase(
        std::remove_if(reg.custom_factories.begin(), reg.custom_factories.end(),
                       [cookie](const RegisteredFactoryEntry& e) { return e.cookie == cookie; }),
        reg.custom_factories.end());
}

// ===========================================================================
// WinRT HSTRING API (Windows*)
// ===========================================================================
int32_t WindowsCreateString(
    const uint16_t* sourceString,
    uint32_t length,
    SX_HSTRING* string) {
    if (!string) {
        return SX_WINRT_E_INVALIDARG;
    }
    *string = nullptr;
    if (length == 0) {
        // Per WinRT specification, empty string is represented by NULL HSTRING.
        return SX_WINRT_S_OK;
    }
    if (!sourceString) {
        return SX_WINRT_E_POINTER;
    }

    const size_t chars_bytes = (static_cast<size_t>(length) + 1) * sizeof(uint16_t);
    const size_t total_bytes = sizeof(SxHStringImpl) + chars_bytes;
    void* mem = ::operator new(total_bytes, std::nothrow);
    if (!mem) {
        return SX_WINRT_E_OUTOFMEMORY;
    }
    auto* buf = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(mem) + sizeof(SxHStringImpl));
    std::memcpy(buf, sourceString, static_cast<size_t>(length) * sizeof(uint16_t));
    buf[length] = 0;

    auto* impl = new (mem) SxHStringImpl{
        kHStringMagic,
        length,
        1,
        0,
        buf,
    };
    *string = reinterpret_cast<SX_HSTRING>(impl);
    return SX_WINRT_S_OK;
}

int32_t WindowsCreateStringReference(
    const uint16_t* sourceString,
    uint32_t length,
    SX_HSTRING_HEADER* hstringHeader,
    SX_HSTRING* string) {
    if (!hstringHeader || !string) {
        return SX_WINRT_E_INVALIDARG;
    }
    *string = nullptr;
    if (!sourceString) {
        if (length != 0) {
            return SX_WINRT_E_POINTER;
        }
        return SX_WINRT_S_OK;
    }
    // Fast-pass HSTRING requires null-termination at sourceString[length].
    if (sourceString[length] != 0) {
        return SX_WINRT_E_INVALIDARG;
    }
    if (length == 0) {
        return SX_WINRT_S_OK;
    }

    auto* impl = new (hstringHeader->Reserved.Reserved2) SxHStringImpl{
        kHStringMagic | kHStringFlagRef,
        length,
        0,
        0,
        sourceString,
    };
    *string = reinterpret_cast<SX_HSTRING>(impl);
    return SX_WINRT_S_OK;
}

int32_t WindowsDeleteString(SX_HSTRING string) {
    if (!string) {
        return SX_WINRT_S_OK;
    }
    SxHStringImpl* impl = AsImplMut(string);
    if ((impl->flags & kHStringMagicMask) != kHStringMagic) {
        return SX_WINRT_E_INVALIDARG;
    }
    if ((impl->flags & kHStringFlagRef) != 0) {
        // Per WinRT spec, deleting a fast-pass string reference is a safe no-op.
        return SX_WINRT_S_OK;
    }
    if (impl->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        impl->~SxHStringImpl();
        ::operator delete(impl);
    }
    return SX_WINRT_S_OK;
}

const uint16_t* WindowsGetStringRawBuffer(SX_HSTRING string, uint32_t* length) {
    if (!string) {
        if (length) *length = 0;
        return kEmptyUtf16;
    }
    const SxHStringImpl* impl = AsImpl(string);
    if (length) *length = impl->length;
    return impl->buffer ? impl->buffer : kEmptyUtf16;
}

uint32_t WindowsGetStringLen(SX_HSTRING string) {
    if (!string) {
        return 0;
    }
    return AsImpl(string)->length;
}

int32_t WindowsDuplicateString(SX_HSTRING string, SX_HSTRING* newString) {
    if (!newString) {
        return SX_WINRT_E_INVALIDARG;
    }
    *newString = nullptr;
    if (!string) {
        return SX_WINRT_S_OK;
    }
    SxHStringImpl* impl = AsImplMut(string);
    if ((impl->flags & kHStringMagicMask) != kHStringMagic) {
        return SX_WINRT_E_INVALIDARG;
    }
    if ((impl->flags & kHStringFlagRef) != 0) {
        // Fast-pass references must be promoted to heap-allocated copies on duplicate.
        return WindowsCreateString(impl->buffer, impl->length, newString);
    }
    impl->ref_count.fetch_add(1, std::memory_order_relaxed);
    *newString = string;
    return SX_WINRT_S_OK;
}

int32_t WindowsCompareStringOrdinal(
    SX_HSTRING string1,
    SX_HSTRING string2,
    int32_t* result) {
    if (!result) {
        return SX_WINRT_E_INVALIDARG;
    }
    uint32_t len1 = 0;
    uint32_t len2 = 0;
    const uint16_t* buf1 = WindowsGetStringRawBuffer(string1, &len1);
    const uint16_t* buf2 = WindowsGetStringRawBuffer(string2, &len2);

    const uint32_t common = std::min(len1, len2);
    for (uint32_t i = 0; i < common; ++i) {
        if (buf1[i] < buf2[i]) {
            *result = -1;
            return SX_WINRT_S_OK;
        }
        if (buf1[i] > buf2[i]) {
            *result = 1;
            return SX_WINRT_S_OK;
        }
    }
    if (len1 < len2) {
        *result = -1;
    } else if (len1 > len2) {
        *result = 1;
    } else {
        *result = 0;
    }
    return SX_WINRT_S_OK;
}

int32_t WindowsIsStringEmpty(SX_HSTRING string) {
    return (string == nullptr || AsImpl(string)->length == 0) ? 1 : 0;
}

int32_t WindowsStringHasEmbeddedNull(SX_HSTRING string, int32_t* hasEmbedNull) {
    if (!hasEmbedNull) {
        return SX_WINRT_E_INVALIDARG;
    }
    *hasEmbedNull = 0;
    if (!string) {
        return SX_WINRT_S_OK;
    }
    uint32_t len = 0;
    const uint16_t* buf = WindowsGetStringRawBuffer(string, &len);
    for (uint32_t i = 0; i < len; ++i) {
        if (buf[i] == 0) {
            *hasEmbedNull = 1;
            break;
        }
    }
    return SX_WINRT_S_OK;
}

int32_t WindowsConcatString(
    SX_HSTRING string1,
    SX_HSTRING string2,
    SX_HSTRING* newString) {
    if (!newString) {
        return SX_WINRT_E_INVALIDARG;
    }
    *newString = nullptr;
    uint32_t len1 = 0;
    uint32_t len2 = 0;
    const uint16_t* buf1 = WindowsGetStringRawBuffer(string1, &len1);
    const uint16_t* buf2 = WindowsGetStringRawBuffer(string2, &len2);
    if (len1 == 0 && len2 == 0) {
        return SX_WINRT_S_OK;
    }
    std::vector<uint16_t> combined(static_cast<size_t>(len1) + len2);
    if (len1 > 0) std::memcpy(combined.data(), buf1, static_cast<size_t>(len1) * sizeof(uint16_t));
    if (len2 > 0) std::memcpy(combined.data() + len1, buf2, static_cast<size_t>(len2) * sizeof(uint16_t));
    return WindowsCreateString(combined.data(), static_cast<uint32_t>(combined.size()), newString);
}

int32_t WindowsSubstring(
    SX_HSTRING string,
    uint32_t startIndex,
    SX_HSTRING* newString) {
    if (!newString) {
        return SX_WINRT_E_INVALIDARG;
    }
    *newString = nullptr;
    uint32_t len = 0;
    const uint16_t* buf = WindowsGetStringRawBuffer(string, &len);
    if (startIndex > len) {
        return SX_WINRT_E_BOUNDS;
    }
    return WindowsCreateString(buf + startIndex, len - startIndex, newString);
}

int32_t WindowsSubstringWithSpecifiedLength(
    SX_HSTRING string,
    uint32_t startIndex,
    uint32_t length,
    SX_HSTRING* newString) {
    if (!newString) {
        return SX_WINRT_E_INVALIDARG;
    }
    *newString = nullptr;
    uint32_t total = 0;
    const uint16_t* buf = WindowsGetStringRawBuffer(string, &total);
    if (startIndex > total || length > total - startIndex) {
        return SX_WINRT_E_BOUNDS;
    }
    return WindowsCreateString(buf + startIndex, length, newString);
}

}  // extern "C"
