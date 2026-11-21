#pragma once

// Clean-room Windows x64 WinRT Activation & HSTRING ABI definitions.
// Deliberately does NOT include <roapi.h> or <winstring.h> to prevent
// symbol collisions with the Windows SDK when linking winrt_xs.dll.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  if defined(SCARLETTX_BUILDING_WINRT)
#    define SCARLETTX_WINRT_API __declspec(dllexport)
#  else
#    define SCARLETTX_WINRT_API __declspec(dllimport)
#  endif
#  define SCARLETTX_WINRT_CALL __stdcall
#else
#  define SCARLETTX_WINRT_API __attribute__((visibility("default")))
#  define SCARLETTX_WINRT_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Standard WinRT / COM HRESULT values (matches Windows x64 ABI).
constexpr int32_t SX_WINRT_S_OK = 0x00000000;
constexpr int32_t SX_WINRT_S_FALSE = 0x00000001;
constexpr int32_t SX_WINRT_E_NOTIMPL = static_cast<int32_t>(0x80004001u);
constexpr int32_t SX_WINRT_E_NOINTERFACE = static_cast<int32_t>(0x80004002u);
constexpr int32_t SX_WINRT_E_POINTER = static_cast<int32_t>(0x80004003u);
constexpr int32_t SX_WINRT_E_FAIL = static_cast<int32_t>(0x80004005u);
constexpr int32_t SX_WINRT_E_OUTOFMEMORY = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t SX_WINRT_E_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t SX_WINRT_E_BOUNDS = static_cast<int32_t>(0x8000000Bu);
constexpr int32_t SX_WINRT_REGDB_E_CLASSNOTREG = static_cast<int32_t>(0x80040154u);
constexpr int32_t SX_WINRT_CO_E_NOTINITIALIZED = static_cast<int32_t>(0x800401F0u);
constexpr int32_t SX_WINRT_RPC_E_CHANGED_MODE = static_cast<int32_t>(0x80010106u);

// ---------------------------------------------------------------------------
// Fundamental WinRT ABI Types
// ---------------------------------------------------------------------------
struct SX_GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
};

static_assert(sizeof(SX_GUID) == 16, "SX_GUID must match 16-byte Windows GUID ABI");

typedef struct SX_HSTRING__* SX_HSTRING;

struct SX_HSTRING_HEADER {
    union {
        void* Reserved1;
        char Reserved2[24];
    } Reserved;
};

static_assert(sizeof(SX_HSTRING_HEADER) == 24,
              "SX_HSTRING_HEADER must match 24-byte Windows x64 HSTRING_HEADER ABI");

enum SX_RO_INIT_TYPE : uint32_t {
    SX_RO_INIT_SINGLETHREADED = 0,
    SX_RO_INIT_MULTITHREADED = 1,
};

enum SX_TrustLevel : int32_t {
    SX_TrustLevel_BaseTrust = 0,
    SX_TrustLevel_PartialTrust = 1,
    SX_TrustLevel_FullTrust = 2,
};

// ---------------------------------------------------------------------------
// Standard COM / WinRT Interface Vtable Layouts (Windows x64 ABI)
// ---------------------------------------------------------------------------
struct SX_IUnknown;
struct SX_IInspectable;
struct SX_IActivationFactory;

struct SX_IUnknownVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(
        SX_IUnknown* This,
        const SX_GUID* riid,
        void** ppvObject);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IUnknown* This);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IUnknown* This);
};

struct SX_IUnknown {
    const SX_IUnknownVtbl* lpVtbl;
};

struct SX_IInspectableVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(
        SX_IInspectable* This,
        const SX_GUID* riid,
        void** ppvObject);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IInspectable* This);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IInspectable* This);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(
        SX_IInspectable* This,
        uint32_t* iidCount,
        SX_GUID** iids);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(
        SX_IInspectable* This,
        SX_HSTRING* className);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(
        SX_IInspectable* This,
        SX_TrustLevel* trustLevel);
};

struct SX_IInspectable {
    const SX_IInspectableVtbl* lpVtbl;
};

struct SX_IActivationFactoryVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(
        SX_IActivationFactory* This,
        const SX_GUID* riid,
        void** ppvObject);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IActivationFactory* This);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IActivationFactory* This);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(
        SX_IActivationFactory* This,
        uint32_t* iidCount,
        SX_GUID** iids);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(
        SX_IActivationFactory* This,
        SX_HSTRING* className);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(
        SX_IActivationFactory* This,
        SX_TrustLevel* trustLevel);
    int32_t(SCARLETTX_WINRT_CALL* ActivateInstance)(
        SX_IActivationFactory* This,
        SX_IInspectable** instance);
};

struct SX_IActivationFactory {
    const SX_IActivationFactoryVtbl* lpVtbl;
};

// Built-in Xbox/UWP WinRT class interface vtables (public WinRT ABI).
struct SX_IAnalyticsVersionInfo;
struct SX_IAnalyticsVersionInfoVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IAnalyticsVersionInfo*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IAnalyticsVersionInfo*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IAnalyticsVersionInfo*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_IAnalyticsVersionInfo*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_IAnalyticsVersionInfo*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_IAnalyticsVersionInfo*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_DeviceFamily)(SX_IAnalyticsVersionInfo*, SX_HSTRING* value);
    int32_t(SCARLETTX_WINRT_CALL* get_DeviceFamilyVersion)(SX_IAnalyticsVersionInfo*, SX_HSTRING* value);
};
struct SX_IAnalyticsVersionInfo {
    const SX_IAnalyticsVersionInfoVtbl* lpVtbl;
};

struct SX_IAnalyticsInfoStatics;
struct SX_IAnalyticsInfoStaticsVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IAnalyticsInfoStatics*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IAnalyticsInfoStatics*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IAnalyticsInfoStatics*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_IAnalyticsInfoStatics*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_IAnalyticsInfoStatics*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_IAnalyticsInfoStatics*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_VersionInfo)(SX_IAnalyticsInfoStatics*, SX_IAnalyticsVersionInfo** value);
    int32_t(SCARLETTX_WINRT_CALL* get_DeviceForm)(SX_IAnalyticsInfoStatics*, SX_HSTRING* value);
};
struct SX_IAnalyticsInfoStatics {
    const SX_IAnalyticsInfoStaticsVtbl* lpVtbl;
};

struct SX_IStorageItem;
struct SX_IStorageItemVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IStorageItem*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IStorageItem*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IStorageItem*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_IStorageItem*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_IStorageItem*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_IStorageItem*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* RenameAsyncOverloadDefaultOptions)(SX_IStorageItem*, SX_HSTRING, void**);
    int32_t(SCARLETTX_WINRT_CALL* RenameAsync)(SX_IStorageItem*, SX_HSTRING, uint32_t, void**);
    int32_t(SCARLETTX_WINRT_CALL* DeleteAsyncOverloadDefaultOptions)(SX_IStorageItem*, void**);
    int32_t(SCARLETTX_WINRT_CALL* DeleteAsync)(SX_IStorageItem*, uint32_t, void**);
    int32_t(SCARLETTX_WINRT_CALL* GetBasicPropertiesAsync)(SX_IStorageItem*, void**);
    int32_t(SCARLETTX_WINRT_CALL* get_Name)(SX_IStorageItem*, SX_HSTRING* value);
    int32_t(SCARLETTX_WINRT_CALL* get_Path)(SX_IStorageItem*, SX_HSTRING* value);
};
struct SX_IStorageItem {
    const SX_IStorageItemVtbl* lpVtbl;
};

struct SX_IApplicationData;
struct SX_IApplicationDataVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IApplicationData*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IApplicationData*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IApplicationData*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_IApplicationData*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_IApplicationData*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_IApplicationData*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_Version)(SX_IApplicationData*, uint32_t* value);
    int32_t(SCARLETTX_WINRT_CALL* SetVersionAsync)(SX_IApplicationData*, uint32_t, void*, void**);
    int32_t(SCARLETTX_WINRT_CALL* ClearAllAsync)(SX_IApplicationData*, void**);
    int32_t(SCARLETTX_WINRT_CALL* ClearAsync)(SX_IApplicationData*, uint32_t, void**);
    int32_t(SCARLETTX_WINRT_CALL* get_LocalSettings)(SX_IApplicationData*, SX_IInspectable** value);
    int32_t(SCARLETTX_WINRT_CALL* get_RoamingSettings)(SX_IApplicationData*, SX_IInspectable** value);
    int32_t(SCARLETTX_WINRT_CALL* get_LocalFolder)(SX_IApplicationData*, SX_IStorageItem** value);
    int32_t(SCARLETTX_WINRT_CALL* get_RoamingFolder)(SX_IApplicationData*, SX_IStorageItem** value);
    int32_t(SCARLETTX_WINRT_CALL* get_TemporaryFolder)(SX_IApplicationData*, SX_IStorageItem** value);
};
struct SX_IApplicationData {
    const SX_IApplicationDataVtbl* lpVtbl;
};

struct SX_IApplicationDataStatics;
struct SX_IApplicationDataStaticsVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IApplicationDataStatics*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IApplicationDataStatics*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IApplicationDataStatics*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_IApplicationDataStatics*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_IApplicationDataStatics*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_IApplicationDataStatics*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_Current)(SX_IApplicationDataStatics*, SX_IApplicationData** value);
};
struct SX_IApplicationDataStatics {
    const SX_IApplicationDataStaticsVtbl* lpVtbl;
};

struct SX_GamepadReading {
    uint64_t Timestamp;
    uint32_t Buttons;
    double LeftTrigger;
    double RightTrigger;
    double LeftThumbstickX;
    double LeftThumbstickY;
    double RightThumbstickX;
    double RightThumbstickY;
};

struct SX_GamepadVibration {
    double LeftMotor;
    double RightMotor;
    double LeftTrigger;
    double RightTrigger;
};

struct SX_IGamepad;
struct SX_IGamepadVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_IGamepad*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_IGamepad*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_IGamepad*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_IGamepad*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_IGamepad*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_IGamepad*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_Vibration)(SX_IGamepad*, SX_GamepadVibration* value);
    int32_t(SCARLETTX_WINRT_CALL* put_Vibration)(SX_IGamepad*, SX_GamepadVibration value);
    int32_t(SCARLETTX_WINRT_CALL* GetCurrentReading)(SX_IGamepad*, SX_GamepadReading* value);
};
struct SX_IGamepad {
    const SX_IGamepadVtbl* lpVtbl;
};

struct SX_ILanguage;
struct SX_ILanguageVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_ILanguage*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_ILanguage*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_ILanguage*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_ILanguage*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_ILanguage*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_ILanguage*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_LanguageTag)(SX_ILanguage*, SX_HSTRING* value);
    int32_t(SCARLETTX_WINRT_CALL* get_DisplayName)(SX_ILanguage*, SX_HSTRING* value);
    int32_t(SCARLETTX_WINRT_CALL* get_NativeName)(SX_ILanguage*, SX_HSTRING* value);
    int32_t(SCARLETTX_WINRT_CALL* get_Script)(SX_ILanguage*, SX_HSTRING* value);
};
struct SX_ILanguage {
    const SX_ILanguageVtbl* lpVtbl;
};

struct SX_ILanguageFactory;
struct SX_ILanguageFactoryVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(SX_ILanguageFactory*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(SX_ILanguageFactory*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(SX_ILanguageFactory*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(SX_ILanguageFactory*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(SX_ILanguageFactory*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(SX_ILanguageFactory*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* CreateLanguage)(SX_ILanguageFactory*, SX_HSTRING languageTag, SX_ILanguage** result);
};
struct SX_ILanguageFactory {
    const SX_ILanguageFactoryVtbl* lpVtbl;
};

// Well-known COM / WinRT IIDs (public specifications).
constexpr SX_GUID SX_IID_IUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr SX_GUID SX_IID_IInspectable = {
    0xAF86E2E0u, 0xB12Du, 0x4C6Au, {0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90}};
constexpr SX_GUID SX_IID_IActivationFactory = {
    0x00000035u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr SX_GUID SX_IID_IAgileObject = {
    0x94EA2B94u, 0xE9CCu, 0x49E0u, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};
constexpr SX_GUID SX_IID_IAnalyticsInfoStatics = {
    0x1D5EE066u, 0x188Du, 0x5BA9u, {0x43, 0x87, 0xAC, 0xAE, 0xB0, 0xE7, 0xE3, 0x05}};
constexpr SX_GUID SX_IID_IAnalyticsVersionInfo = {
    0x926130B8u, 0x9955u, 0x4C74u, {0xBD, 0xC1, 0x7C, 0xD0, 0xDE, 0xCF, 0x9B, 0x03}};
constexpr SX_GUID SX_IID_IApplicationDataStatics = {
    0x5612147Bu, 0xE843u, 0x45E3u, {0x94, 0xD8, 0x06, 0x16, 0x9E, 0x3C, 0x8E, 0x17}};
constexpr SX_GUID SX_IID_IApplicationData = {
    0xC3DA6FB7u, 0xB744u, 0x4B45u, {0xB0, 0xB8, 0x22, 0x3A, 0x09, 0x38, 0xD0, 0xDC}};
constexpr SX_GUID SX_IID_IStorageItem = {
    0x4207A996u, 0xCA2Fu, 0x42F7u, {0xBD, 0xE8, 0x8B, 0x10, 0x45, 0x7A, 0x7F, 0x30}};
constexpr SX_GUID SX_IID_IGamepad = {
    0xBC7BB43Cu, 0x0A69u, 0x3903u, {0x9E, 0x9D, 0xA5, 0x0F, 0x86, 0xA4, 0x5D, 0xE5}};
constexpr SX_GUID SX_IID_ILanguage = {
    0xEA79A752u, 0xF7C2u, 0x4265u, {0xB1, 0xBD, 0xC4, 0xDE, 0xC4, 0xE4, 0xF0, 0x80}};
constexpr SX_GUID SX_IID_ILanguageFactory = {
    0x9B0252ACu, 0x0C27u, 0x44F8u, {0xB7, 0x92, 0x97, 0x93, 0xFB, 0x66, 0xC6, 0x3E}};

// Custom factory registration callback (matches PFNGETACTIVATIONFACTORY ABI).
typedef int32_t(SCARLETTX_WINRT_CALL* SX_PFNGETACTIVATIONFACTORY)(
    SX_HSTRING activatableClassId,
    SX_IActivationFactory** factory);

// ---------------------------------------------------------------------------
// WinRT Activation Runtime API (Ro*)
// ---------------------------------------------------------------------------
SCARLETTX_WINRT_API int32_t RoInitialize(uint32_t initType);
SCARLETTX_WINRT_API void RoUninitialize(void);
SCARLETTX_WINRT_API int32_t RoGetActivationFactory(
    SX_HSTRING activatableClassId,
    const SX_GUID* iid,
    void** factory);
SCARLETTX_WINRT_API int32_t RoActivateInstance(
    SX_HSTRING activatableClassId,
    void** instance);
SCARLETTX_WINRT_API int32_t RoRegisterActivationFactories(
    SX_HSTRING* activatableClassIds,
    SX_PFNGETACTIVATIONFACTORY* activationFactoryCallbacks,
    uint32_t count,
    uint64_t* cookie);
SCARLETTX_WINRT_API void RoRevokeActivationFactories(uint64_t cookie);

// ---------------------------------------------------------------------------
// WinRT HSTRING API (Windows*)
// ---------------------------------------------------------------------------
SCARLETTX_WINRT_API int32_t WindowsCreateString(
    const uint16_t* sourceString,
    uint32_t length,
    SX_HSTRING* string);
SCARLETTX_WINRT_API int32_t WindowsCreateStringReference(
    const uint16_t* sourceString,
    uint32_t length,
    SX_HSTRING_HEADER* hstringHeader,
    SX_HSTRING* string);
SCARLETTX_WINRT_API int32_t WindowsDeleteString(SX_HSTRING string);
SCARLETTX_WINRT_API const uint16_t* WindowsGetStringRawBuffer(
    SX_HSTRING string,
    uint32_t* length);
SCARLETTX_WINRT_API uint32_t WindowsGetStringLen(SX_HSTRING string);
SCARLETTX_WINRT_API int32_t WindowsDuplicateString(
    SX_HSTRING string,
    SX_HSTRING* newString);
SCARLETTX_WINRT_API int32_t WindowsCompareStringOrdinal(
    SX_HSTRING string1,
    SX_HSTRING string2,
    int32_t* result);
SCARLETTX_WINRT_API int32_t WindowsIsStringEmpty(SX_HSTRING string);
SCARLETTX_WINRT_API int32_t WindowsStringHasEmbeddedNull(
    SX_HSTRING string,
    int32_t* hasEmbedNull);
SCARLETTX_WINRT_API int32_t WindowsConcatString(
    SX_HSTRING string1,
    SX_HSTRING string2,
    SX_HSTRING* newString);
SCARLETTX_WINRT_API int32_t WindowsSubstring(
    SX_HSTRING string,
    uint32_t startIndex,
    SX_HSTRING* newString);
SCARLETTX_WINRT_API int32_t WindowsSubstringWithSpecifiedLength(
    SX_HSTRING string,
    uint32_t startIndex,
    uint32_t length,
    SX_HSTRING* newString);

#ifdef __cplusplus
}
#endif
