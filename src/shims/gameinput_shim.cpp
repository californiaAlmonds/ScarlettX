#include "scarlettx/input/gameinput_abi.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace {

bool GameInputTraceEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("SCARLETTX_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

constexpr int32_t kS_OK = 0;
constexpr int32_t kE_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t kE_HANDLE = static_cast<int32_t>(0x80070006u);
constexpr int32_t kE_POINTER = static_cast<int32_t>(0x80004003u);
constexpr int32_t kE_NOTIMPL = static_cast<int32_t>(0x80004001u);
constexpr int32_t kGAMEINPUT_E_READING_NOT_FOUND = static_cast<int32_t>(0x838A0002u);

constexpr uint64_t kVirtualGamepadId = 1;
constexpr uint64_t kVirtualKeyboardId = 2;

#if defined(_WIN32)
#define SCARLETTX_GI_CALL __stdcall
#else
#define SCARLETTX_GI_CALL
#endif

using GiComFn = intptr_t (SCARLETTX_GI_CALL*)(void*, uintptr_t, uintptr_t, uintptr_t);

struct ReadingEntry {
    uint64_t handle = 0;
    uint64_t device_id = 0;
    uint32_t input_kind = 0;
    uint64_t timestamp_us = 0;
    GAMEINPUT_GAMEPAD_STATE gamepad{};
    std::vector<GAMEINPUT_KEY_STATE> keys;
};

struct GameInputObject {
    // First field is the COM vtable pointer so real Xbox binaries can call
    // IGameInput methods directly through the pointer returned by GameInputCreate,
    // while synthetic C tests can still treat it as an opaque uint64_t handle.
    const GiComFn* lpVtbl = nullptr;
    std::atomic<uint32_t> ref_count{1};
    std::atomic<uint64_t> next_callback_token{1};
    std::mutex mu;
    std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
    uint64_t next_reading_id = 1;
    std::vector<GAMEINPUT_DEVICE_INFO> devices;
    std::unordered_map<uint64_t, GAMEINPUT_RUMBLE_PARAMS> rumble;
    std::deque<ReadingEntry> history;

    // State behind the guest-facing readings (IGameInput::GetCurrentReading).
    bool gamepad_injected = false;
    GAMEINPUT_GAMEPAD_STATE injected_gamepad{};
    GAMEINPUT_GAMEPAD_STATE last_gamepad{};
    uint64_t gamepad_sequence = 1;

    uint64_t NowUs() const {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) + 1;
    }

    void PushReading(ReadingEntry entry) {
        entry.handle = next_reading_id++;
        entry.timestamp_us = NowUs();
        history.push_back(std::move(entry));
        if (history.size() > 256) {
            history.pop_front();
        }
    }
};

struct InputState {
    std::mutex mu;
    std::unordered_map<uint64_t, std::shared_ptr<GameInputObject>> instances;
};

InputState& State() {
    static InputState s;
    return s;
}

std::shared_ptr<GameInputObject> GetInstance(uint64_t handle) {
    std::lock_guard<std::mutex> lk(State().mu);
    auto it = State().instances.find(handle);
    return it == State().instances.end() ? nullptr : it->second;
}

intptr_t SCARLETTX_GI_CALL GiQueryInterface(void* self, uintptr_t /*riid*/, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<GameInputObject*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return kE_POINTER;
    if (!obj) {
        *out = nullptr;
        return kE_INVALIDARG;
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return kS_OK;
}

intptr_t SCARLETTX_GI_CALL GiAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<GameInputObject*>(self);
    return obj ? static_cast<intptr_t>(obj->ref_count.fetch_add(1, std::memory_order_relaxed) + 1) : 0;
}

intptr_t SCARLETTX_GI_CALL GiRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<GameInputObject*>(self);
    if (!obj) return 0;
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        std::lock_guard<std::mutex> lk(State().mu);
        State().instances.erase(reinterpret_cast<uint64_t>(obj));
    }
    return static_cast<intptr_t>(rem);
}

intptr_t SCARLETTX_GI_CALL GiGetCurrentTimestamp(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<GameInputObject*>(self);
    return obj ? static_cast<intptr_t>(obj->NowUs()) : 0;
}

// ---------------------------------------------------------------------------
// Guest-facing IGameInputDevice and IGameInputReading.
//
// Slot numbers and layouts come from the console GameInput.dll and from the
// test title's device callback, not from this repository's flat API:
//  - device: slot 3 returns a pointer to the device information, slot 4 the
//    status bits, slot 5 fills a 20-byte battery state; 18 methods in all.
//  - reading: slots 3..24 follow the public GameInput.h order (slot 22 copies
//    the 28-byte gamepad state and returns true); slots 25..28 exist on the
//    console only and are not identified.
//  - the title reads three things from the device information: the 32-byte
//    device id at +0x20, the supported input kinds at +0x68 (it tests the
//    gamepad bit, 0x40000) and at +0x130 a pointer to a string whose
//    characters are at +8.
// The console objects answer QueryInterface for IUnknown and IAgileObject.
// ---------------------------------------------------------------------------

constexpr int32_t kE_NOINTERFACE = static_cast<int32_t>(0x80004002u);
constexpr int32_t kE_OUTOFMEMORY = static_cast<int32_t>(0x8007000Eu);

struct GiGuid {
    uint32_t d1;
    uint16_t d2;
    uint16_t d3;
    uint8_t d4[8];
};
constexpr GiGuid kIidIUnknown = {0x00000000u, 0x0000u, 0x0000u, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GiGuid kIidIAgileObject = {0x94EA2B94u, 0xE9CCu, 0x49E0u, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};
constexpr GiGuid kIidIGameInputDevice = {0x31DD86FBu, 0x4C1Bu, 0x408Au, {0x86, 0x8F, 0x43, 0x9B, 0x3C, 0xD4, 0x71, 0x25}};
constexpr GiGuid kIidIGameInputReading = {0x2156947Au, 0xE1FAu, 0x4DE0u, {0xA3, 0x0B, 0xD8, 0x12, 0x93, 0x1D, 0xBD, 0x8D}};

bool GiGuidIs(uintptr_t riid, const GiGuid& guid) {
    return riid && std::memcmp(reinterpret_cast<const void*>(riid), &guid, sizeof(guid)) == 0;
}

constexpr size_t kDeviceInfoSize = 0x180;
constexpr size_t kInfoOffVendorId = 0x04;        // public header layout, not read by the test title
constexpr size_t kInfoOffProductId = 0x06;       // public header layout, not read by the test title
constexpr size_t kInfoOffDeviceId = 0x20;        // 32 bytes
constexpr size_t kInfoOffSupportedInput = 0x68;
constexpr size_t kInfoOffName = 0x130;
constexpr uint32_t kDeviceConnected = 0x1u;
constexpr uint32_t kDeviceInputEnabled = 0x2u;
constexpr uint32_t kPadStatus = kDeviceConnected | kDeviceInputEnabled;
constexpr uint32_t kPadSupportedInput =
    GAMEINPUT_KIND_GAMEPAD | GAMEINPUT_KIND_CONTROLLER_BUTTON | GAMEINPUT_KIND_CONTROLLER_AXIS;

struct GiString {
    uint32_t size_in_bytes;
    uint32_t code_point_count;
    const char* data;
};

struct GiDeviceProxy {
    const GiComFn* lpVtbl = nullptr;
    std::atomic<uint32_t> ref_count{1};
    GiString name{};
    alignas(8) uint8_t info[kDeviceInfoSize] = {};
};

struct GiReadingProxy {
    const GiComFn* lpVtbl = nullptr;
    std::atomic<uint32_t> ref_count{1};
    uint32_t input_kind = 0;
    uint64_t sequence = 0;
    uint64_t timestamp = 0;
    GAMEINPUT_GAMEPAD_STATE gamepad{};
};

static_assert(sizeof(GAMEINPUT_GAMEPAD_STATE) == 0x1c, "the console copies 28 bytes of gamepad state");

GiDeviceProxy& VirtualGamepad();

intptr_t SCARLETTX_GI_CALL DevQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return kE_POINTER;
    *out = nullptr;
    if (!GiGuidIs(riid, kIidIUnknown) && !GiGuidIs(riid, kIidIAgileObject) &&
        !GiGuidIs(riid, kIidIGameInputDevice)) {
        return kE_NOINTERFACE;
    }
    static_cast<GiDeviceProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return kS_OK;
}

intptr_t SCARLETTX_GI_CALL DevAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<GiDeviceProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

// The virtual gamepad lives as long as the process: the count is kept for
// the guest's benefit and never drops below the reference this DLL holds.
intptr_t SCARLETTX_GI_CALL DevRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto& count = static_cast<GiDeviceProxy*>(self)->ref_count;
    uint32_t current = count.load(std::memory_order_relaxed);
    while (current > 1 && !count.compare_exchange_weak(current, current - 1, std::memory_order_acq_rel)) {
    }
    return static_cast<intptr_t>(current > 1 ? current - 1 : 1);
}

intptr_t SCARLETTX_GI_CALL DevGetDeviceInfo(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return reinterpret_cast<intptr_t>(static_cast<GiDeviceProxy*>(self)->info);
}

intptr_t SCARLETTX_GI_CALL DevGetDeviceStatus(void*, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(kPadStatus);
}

// The console DLL writes the same constant answer: zero charge figures and
// a status of -1.
intptr_t SCARLETTX_GI_CALL DevGetBatteryState(void*, uintptr_t out_state, uintptr_t, uintptr_t) {
    if (out_state) {
        auto* fields = reinterpret_cast<uint32_t*>(out_state);
        fields[0] = fields[1] = fields[2] = fields[3] = 0;
        fields[4] = 0xFFFFFFFFu;
    }
    return 0;
}

// Slot 10 by the public GameInput.h order, which also has 18 device methods.
// XInput only drives the two main motors; the trigger motors are dropped.
intptr_t SCARLETTX_GI_CALL DevSetRumbleState(void*, uintptr_t params, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    struct Vibration {
        uint16_t low, high;
    };
    using XInputSetStateFn = DWORD (WINAPI*)(DWORD, Vibration*);
    static const XInputSetStateFn set_state = [] {
        const HMODULE module = LoadLibraryW(L"xinput1_4.dll");
        return module ? reinterpret_cast<XInputSetStateFn>(
                            reinterpret_cast<void*>(GetProcAddress(module, "XInputSetState")))
                      : nullptr;
    }();
    if (params != 0 && params < 0x10000) return 0;  // not a pointer
    Vibration v{};
    if (params) {
        const auto* p = reinterpret_cast<const GAMEINPUT_RUMBLE_PARAMS*>(params);
        const auto motor = [](float f) {
            return static_cast<uint16_t>(std::clamp(f, 0.0f, 1.0f) * 65535.0f + 0.5f);
        };
        v.low = motor(p->LowFrequency);
        v.high = motor(p->HighFrequency);
    }
    if (set_state) set_state(0, &v);
#else
    (void)params;
#endif
    return 0;
}

template <int Slot>
intptr_t SCARLETTX_GI_CALL DevTracedSlot(void*, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] device::vtable[" << Slot << "](a1=0x" << std::hex << a1 << ", a2=0x"
                  << a2 << ", a3=0x" << a3 << std::dec << ") -> E_NOTIMPL\n";
    }
    return kE_NOTIMPL;
}

template <size_t... Is>
constexpr std::array<GiComFn, sizeof...(Is) + 3> MakeDeviceVtable(std::index_sequence<Is...>) {
    return {&DevQueryInterface, &DevAddRef, &DevRelease, &DevTracedSlot<static_cast<int>(Is + 3)>...};
}

std::array<GiComFn, 21> BuildDeviceVtable() {
    auto vtbl = MakeDeviceVtable(std::make_index_sequence<18>{});
    vtbl[3] = &DevGetDeviceInfo;
    vtbl[4] = &DevGetDeviceStatus;
    vtbl[5] = &DevGetBatteryState;
    vtbl[10] = &DevSetRumbleState;
    return vtbl;
}

const auto kGiDeviceVtbl = BuildDeviceVtable();

GiDeviceProxy& VirtualGamepad() {
    static GiDeviceProxy* const device = [] {
        static const char kName[] = "ScarlettX Virtual Gamepad";
        static const char kId[32] = "ScarlettX.VirtualGamepad.0";
        auto* d = new GiDeviceProxy();
        d->lpVtbl = kGiDeviceVtbl.data();
        d->name.size_in_bytes = static_cast<uint32_t>(sizeof(kName) - 1);
        d->name.code_point_count = static_cast<uint32_t>(sizeof(kName) - 1);
        d->name.data = kName;
        const uint16_t vendor = 0x045Eu;
        const uint16_t product = 0x0B12u;
        const uint32_t supported = kPadSupportedInput;
        const GiString* name = &d->name;
        std::memcpy(d->info + kInfoOffVendorId, &vendor, sizeof(vendor));
        std::memcpy(d->info + kInfoOffProductId, &product, sizeof(product));
        std::memcpy(d->info + kInfoOffDeviceId, kId, sizeof(kId));
        std::memcpy(d->info + kInfoOffSupportedInput, &supported, sizeof(supported));
        std::memcpy(d->info + kInfoOffName, &name, sizeof(name));
        return d;
    }();
    return *device;
}

intptr_t SCARLETTX_GI_CALL ReadQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return kE_POINTER;
    *out = nullptr;
    if (!GiGuidIs(riid, kIidIUnknown) && !GiGuidIs(riid, kIidIAgileObject) &&
        !GiGuidIs(riid, kIidIGameInputReading)) {
        return kE_NOINTERFACE;
    }
    static_cast<GiReadingProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return kS_OK;
}

intptr_t SCARLETTX_GI_CALL ReadAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<GiReadingProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GI_CALL ReadRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* reading = static_cast<GiReadingProxy*>(self);
    const uint32_t remaining = reading->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) delete reading;
    return static_cast<intptr_t>(remaining);
}

intptr_t SCARLETTX_GI_CALL ReadGetInputKind(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(static_cast<GiReadingProxy*>(self)->input_kind);
}

intptr_t SCARLETTX_GI_CALL ReadGetSequenceNumber(void* self, uintptr_t input_kind, uintptr_t, uintptr_t) {
    auto* reading = static_cast<GiReadingProxy*>(self);
    return (static_cast<uint32_t>(input_kind) & reading->input_kind) ? static_cast<intptr_t>(reading->sequence) : 0;
}

intptr_t SCARLETTX_GI_CALL ReadGetTimestamp(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(static_cast<GiReadingProxy*>(self)->timestamp);
}

intptr_t SCARLETTX_GI_CALL ReadGetDevice(void*, uintptr_t out_device, uintptr_t, uintptr_t) {
    if (out_device) {
        GiDeviceProxy& pad = VirtualGamepad();
        pad.ref_count.fetch_add(1, std::memory_order_relaxed);
        *reinterpret_cast<void**>(out_device) = &pad;
    }
    return 0;
}

intptr_t SCARLETTX_GI_CALL ReadGetGamepadState(void* self, uintptr_t out_state, uintptr_t, uintptr_t) {
    if (!out_state) return 0;
    std::memcpy(reinterpret_cast<void*>(out_state), &static_cast<GiReadingProxy*>(self)->gamepad,
                sizeof(GAMEINPUT_GAMEPAD_STATE));
    return 1;
}

// Every other reading method returns a count or a bool: zero means "this
// reading has none of that".
template <int Slot>
intptr_t SCARLETTX_GI_CALL ReadEmptySlot(void*, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] reading::vtable[" << Slot << "](a1=0x" << std::hex << a1 << ", a2=0x"
                  << a2 << ", a3=0x" << a3 << std::dec << ") -> 0\n";
    }
    return 0;
}

template <size_t... Is>
constexpr std::array<GiComFn, sizeof...(Is) + 3> MakeReadingVtable(std::index_sequence<Is...>) {
    return {&ReadQueryInterface, &ReadAddRef, &ReadRelease, &ReadEmptySlot<static_cast<int>(Is + 3)>...};
}

std::array<GiComFn, 29> BuildReadingVtable() {
    auto vtbl = MakeReadingVtable(std::make_index_sequence<26>{});
    vtbl[3] = &ReadGetInputKind;
    vtbl[4] = &ReadGetSequenceNumber;
    vtbl[5] = &ReadGetTimestamp;
    vtbl[6] = &ReadGetDevice;
    vtbl[22] = &ReadGetGamepadState;
    return vtbl;
}

const auto kGiReadingVtbl = BuildReadingVtable();

// ---------------------------------------------------------------------------
// Host input for the virtual gamepad: the first XInput controller when one
// is connected, plus the keyboard while a window of this process has focus.
//   arrows = D-pad, WASD = left stick, IJKL = right stick,
//   Enter/Space = A, Escape/Backspace = B, X = X, Y = Y,
//   Q/E = shoulders, 1/3 = triggers, Tab = View, P = Menu.
// ---------------------------------------------------------------------------
void ReadHostGamepad(GAMEINPUT_GAMEPAD_STATE* out) {
    *out = GAMEINPUT_GAMEPAD_STATE{};
#if defined(_WIN32)
    struct XInputPad {
        uint16_t buttons;
        uint8_t left_trigger, right_trigger;
        int16_t lx, ly, rx, ry;
    };
    struct XInputStateBlock {
        uint32_t packet;
        XInputPad pad;
    };
    using XInputGetStateFn = DWORD (WINAPI*)(DWORD, XInputStateBlock*);
    static const XInputGetStateFn get_state = [] {
        const HMODULE module = LoadLibraryW(L"xinput1_4.dll");
        return module ? reinterpret_cast<XInputGetStateFn>(
                            reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState")))
                      : nullptr;
    }();
    XInputStateBlock host{};
    if (get_state && get_state(0, &host) == 0) {
        static const struct {
            uint16_t xinput;
            uint32_t gameinput;
        } kButtons[] = {
            {0x0001, GAMEINPUT_GAMEPAD_DPAD_UP},         {0x0002, GAMEINPUT_GAMEPAD_DPAD_DOWN},
            {0x0004, GAMEINPUT_GAMEPAD_DPAD_LEFT},       {0x0008, GAMEINPUT_GAMEPAD_DPAD_RIGHT},
            {0x0010, GAMEINPUT_GAMEPAD_MENU},            {0x0020, GAMEINPUT_GAMEPAD_VIEW},
            {0x0040, GAMEINPUT_GAMEPAD_LEFT_THUMBSTICK}, {0x0080, GAMEINPUT_GAMEPAD_RIGHT_THUMBSTICK},
            {0x0100, GAMEINPUT_GAMEPAD_LEFT_SHOULDER},   {0x0200, GAMEINPUT_GAMEPAD_RIGHT_SHOULDER},
            {0x1000, GAMEINPUT_GAMEPAD_A},               {0x2000, GAMEINPUT_GAMEPAD_B},
            {0x4000, GAMEINPUT_GAMEPAD_X},               {0x8000, GAMEINPUT_GAMEPAD_Y},
        };
        for (const auto& b : kButtons) {
            if (host.pad.buttons & b.xinput) out->Buttons |= b.gameinput;
        }
        const auto axis = [](int16_t v) { return std::max(-1.0f, static_cast<float>(v) / 32767.0f); };
        out->LeftTrigger = static_cast<float>(host.pad.left_trigger) / 255.0f;
        out->RightTrigger = static_cast<float>(host.pad.right_trigger) / 255.0f;
        out->LeftThumbstickX = axis(host.pad.lx);
        out->LeftThumbstickY = axis(host.pad.ly);
        out->RightThumbstickX = axis(host.pad.rx);
        out->RightThumbstickY = axis(host.pad.ry);
    }

    DWORD focused_process = 0;
    const HWND foreground = GetForegroundWindow();
    if (foreground) GetWindowThreadProcessId(foreground, &focused_process);
    if (focused_process != GetCurrentProcessId()) return;
    const auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    static const struct {
        int key;
        uint32_t gameinput;
    } kKeys[] = {
        {VK_UP, GAMEINPUT_GAMEPAD_DPAD_UP},     {VK_DOWN, GAMEINPUT_GAMEPAD_DPAD_DOWN},
        {VK_LEFT, GAMEINPUT_GAMEPAD_DPAD_LEFT}, {VK_RIGHT, GAMEINPUT_GAMEPAD_DPAD_RIGHT},
        {VK_RETURN, GAMEINPUT_GAMEPAD_A},       {VK_SPACE, GAMEINPUT_GAMEPAD_A},
        {VK_ESCAPE, GAMEINPUT_GAMEPAD_B},       {VK_BACK, GAMEINPUT_GAMEPAD_B},
        {'X', GAMEINPUT_GAMEPAD_X},             {'Y', GAMEINPUT_GAMEPAD_Y},
        {'Q', GAMEINPUT_GAMEPAD_LEFT_SHOULDER}, {'E', GAMEINPUT_GAMEPAD_RIGHT_SHOULDER},
        {VK_TAB, GAMEINPUT_GAMEPAD_VIEW},       {'P', GAMEINPUT_GAMEPAD_MENU},
    };
    for (const auto& k : kKeys) {
        if (down(k.key)) out->Buttons |= k.gameinput;
    }
    const auto stick = [&](int negative, int positive, float* value) {
        const float keys = (down(positive) ? 1.0f : 0.0f) - (down(negative) ? 1.0f : 0.0f);
        if (keys != 0.0f) *value = keys;
    };
    stick('A', 'D', &out->LeftThumbstickX);
    stick('S', 'W', &out->LeftThumbstickY);
    stick('J', 'L', &out->RightThumbstickX);
    stick('K', 'I', &out->RightThumbstickY);
    if (down('1')) out->LeftTrigger = 1.0f;
    if (down('3')) out->RightTrigger = 1.0f;
#endif
}

// IGameInput::GetCurrentReading (vtable[4]): a snapshot of the virtual
// gamepad. The sequence number advances only when the state changes.
intptr_t SCARLETTX_GI_CALL GiGetCurrentReading(
    void* self, uintptr_t input_kind, uintptr_t device, uintptr_t ppReading) {
    auto* obj = static_cast<GameInputObject*>(self);
    auto** out = reinterpret_cast<void**>(ppReading);
    if (!obj || !out) return kE_INVALIDARG;
    *out = nullptr;
    const bool wanted = (static_cast<uint32_t>(input_kind) & GAMEINPUT_KIND_GAMEPAD) != 0 &&
                        (device == 0 || device == reinterpret_cast<uintptr_t>(&VirtualGamepad()));
    if (!wanted) {
        if (GameInputTraceEnabled()) {
            std::cerr << "[ScarlettX][GameInput] IGameInput::GetCurrentReading(kind=0x" << std::hex << input_kind
                      << ", device=0x" << device << std::dec << ") -> GAMEINPUT_E_READING_NOT_FOUND\n";
        }
        return kGAMEINPUT_E_READING_NOT_FOUND;
    }
    auto* reading = new (std::nothrow) GiReadingProxy();
    if (!reading) return kE_OUTOFMEMORY;
    reading->lpVtbl = kGiReadingVtbl.data();
    reading->input_kind = GAMEINPUT_KIND_GAMEPAD;
    // Polls are frequent: trace the first one and every change of state.
    static std::atomic<bool> first_poll{true};
    bool changed = first_poll.exchange(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(obj->mu);
        GAMEINPUT_GAMEPAD_STATE now{};
        if (obj->gamepad_injected) {
            now = obj->injected_gamepad;
        } else {
            ReadHostGamepad(&now);
        }
        if (std::memcmp(&now, &obj->last_gamepad, sizeof(now)) != 0) {
            obj->last_gamepad = now;
            ++obj->gamepad_sequence;
            changed = true;
        }
        reading->gamepad = now;
        reading->sequence = obj->gamepad_sequence;
        reading->timestamp = obj->NowUs();
    }
    if (changed && GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::GetCurrentReading: gamepad buttons=0x" << std::hex
                  << reading->gamepad.Buttons << std::dec << " seq=" << reading->sequence << "\n";
    }
    *out = reading;
    return kS_OK;
}

// IGameInput::RegisterDeviceCallback (vtable[9] / +0x48):
//   HRESULT RegisterDeviceCallback(
//       IGameInputDevice* device, GameInputKind inputFilter,
//       GameInputDeviceStatus statusFilter, GameInputEnumerationKind enumKind,
//       void* context, GameInputDeviceCallback callbackFunc,
//       GameInputCallbackToken* callbackToken);
intptr_t SCARLETTX_GI_CALL GiRegisterDeviceCallback(
    void* self, uintptr_t device, uintptr_t input_filter, uintptr_t status_filter,
    uintptr_t enum_kind, uintptr_t context, uintptr_t callback_fn, uintptr_t out_token_ptr) {
    auto* obj = static_cast<GameInputObject*>(self);
    const uint64_t tok = obj ? obj->next_callback_token.fetch_add(1, std::memory_order_relaxed) : 1;
    if (out_token_ptr) {
        *reinterpret_cast<uint64_t*>(out_token_ptr) = tok;
    }
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::RegisterDeviceCallback(kind=0x"
                  << std::hex << input_filter << ", status=0x" << status_filter
                  << ", enum=" << std::dec << static_cast<uint32_t>(enum_kind) << ", context=0x" << std::hex
                  << context << ", callback=0x" << callback_fn << std::dec << ") -> S_OK (tok=" << tok << ")\n";
    }
    // The virtual gamepad is always connected, so an enumerating
    // registration reports it once, before returning (the title asks for
    // blocking enumeration). It never disconnects: nothing is kept for later.
    using DeviceCallback = void (SCARLETTX_GI_CALL*)(uint64_t token, void* context, void* device, uint64_t timestamp,
                                                     uint32_t current_status, uint32_t previous_status);
    GiDeviceProxy& pad = VirtualGamepad();
    const bool matches = (device == 0 || device == reinterpret_cast<uintptr_t>(&pad)) &&
                         (static_cast<uint32_t>(input_filter) & kPadSupportedInput) != 0 &&
                         (static_cast<uint32_t>(status_filter) & kPadStatus) != 0;
    if (callback_fn && static_cast<uint32_t>(enum_kind) != 0 && matches) {
        reinterpret_cast<DeviceCallback>(callback_fn)(tok, reinterpret_cast<void*>(context), &pad,
                                                      obj ? obj->NowUs() : 0, kPadStatus, 0);
    }
    return kS_OK;
}

intptr_t SCARLETTX_GI_CALL GiStopCallback(void* /*self*/, uintptr_t tok, uintptr_t, uintptr_t) {
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::StopCallback(tok=" << tok << ")\n";
    }
    return 0;
}

intptr_t SCARLETTX_GI_CALL GiUnregisterCallback(void* /*self*/, uintptr_t tok, uintptr_t, uintptr_t) {
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::UnregisterCallback(tok=" << tok << ") -> true\n";
    }
    return 1;
}

intptr_t SCARLETTX_GI_CALL GiSetFocusPolicy(void* /*self*/, uintptr_t policy, uintptr_t, uintptr_t) {
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::SetFocusPolicy(0x"
                  << std::hex << policy << std::dec << ")\n";
    }
    return 0;
}

template <int Slot>
intptr_t SCARLETTX_GI_CALL GiTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return kE_NOTIMPL;
}

template <size_t... Is>
constexpr std::array<GiComFn, sizeof...(Is) + 3> MakeGiVtable(std::index_sequence<Is...>) {
    return {
        &GiQueryInterface,
        &GiAddRef,
        &GiRelease,
        &GiTracedSlot<static_cast<int>(Is + 3)>...,
    };
}

std::array<GiComFn, 32> BuildGiVtable() {
    auto vtbl = MakeGiVtable(std::make_index_sequence<29>{});
    vtbl[3] = &GiGetCurrentTimestamp;
    vtbl[4] = &GiGetCurrentReading;
    vtbl[9] = reinterpret_cast<GiComFn>(&GiRegisterDeviceCallback);
    vtbl[13] = &GiStopCallback;
    vtbl[14] = &GiUnregisterCallback;
    vtbl[22] = &GiSetFocusPolicy;
    return vtbl;
}

const auto kIGameInputVtbl = BuildGiVtable();

}  // namespace

extern "C" {

int32_t GameInputCreate(uint64_t* out_game_input) {
    if (!out_game_input) return kE_INVALIDARG;
    auto obj = std::make_shared<GameInputObject>();
    obj->lpVtbl = kIGameInputVtbl.data();

    GAMEINPUT_DEVICE_INFO pad{};
    pad.DeviceId = kVirtualGamepadId;
    pad.VendorId = 0x045Eu;   // Microsoft
    pad.ProductId = 0x0B12u;  // Xbox Series X|S Wireless Controller
    pad.SupportedInput = GAMEINPUT_KIND_GAMEPAD | GAMEINPUT_KIND_CONTROLLER_BUTTON |
                         GAMEINPUT_KIND_CONTROLLER_AXIS;
    pad.SupportedRumbleMotors = 0x0Fu;  // Low, High, LeftTrigger, RightTrigger
    std::strncpy(pad.DisplayName, "ScarlettX Virtual Xbox Series Controller", sizeof(pad.DisplayName) - 1);
    obj->devices.push_back(pad);

    GAMEINPUT_DEVICE_INFO kb{};
    kb.DeviceId = kVirtualKeyboardId;
    kb.VendorId = 0x045Eu;
    kb.ProductId = 0x0001u;
    kb.SupportedInput = GAMEINPUT_KIND_KEYBOARD;
    kb.SupportedRumbleMotors = 0;
    std::strncpy(kb.DisplayName, "ScarlettX Virtual Keyboard", sizeof(kb.DisplayName) - 1);
    obj->devices.push_back(kb);

    // Seed initial neutral readings so GetCurrentReading succeeds immediately.
    ReadingEntry pad_init{};
    pad_init.device_id = kVirtualGamepadId;
    pad_init.input_kind = GAMEINPUT_KIND_GAMEPAD;
    obj->PushReading(std::move(pad_init));

    ReadingEntry kb_init{};
    kb_init.device_id = kVirtualKeyboardId;
    kb_init.input_kind = GAMEINPUT_KIND_KEYBOARD;
    obj->PushReading(std::move(kb_init));

    const uint64_t h = reinterpret_cast<uint64_t>(obj.get());
    {
        std::lock_guard<std::mutex> lk(State().mu);
        State().instances[h] = std::move(obj);
    }
    *out_game_input = h;
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] GameInputCreate() -> S_OK (IGameInput*=0x"
                  << std::hex << h << std::dec << ")\n";
    }
    return kS_OK;
}

int32_t GameInputDestroy(uint64_t game_input) {
    std::lock_guard<std::mutex> lk(State().mu);
    return State().instances.erase(game_input) ? kS_OK : kE_HANDLE;
}

uint64_t GameInputGetCurrentTimestamp(uint64_t game_input) {
    auto obj = GetInstance(game_input);
    if (!obj) return 0;
    return obj->NowUs();
}

int32_t GameInputEnumerateDevices(
    uint64_t game_input,
    uint32_t input_kind,
    uint64_t* out_device_ids,
    uint32_t capacity,
    uint32_t* out_count) {
    if (!out_count) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    uint32_t count = 0;
    for (const auto& d : obj->devices) {
        if (input_kind == GAMEINPUT_KIND_UNKNOWN || (d.SupportedInput & input_kind) != 0) {
            if (out_device_ids && count < capacity) {
                out_device_ids[count] = d.DeviceId;
            }
            ++count;
        }
    }
    *out_count = count;
    return kS_OK;
}

int32_t GameInputGetDeviceInfo(
    uint64_t game_input,
    uint64_t device_id,
    GAMEINPUT_DEVICE_INFO* out_info) {
    if (!out_info) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& d : obj->devices) {
        if (d.DeviceId == device_id) {
            *out_info = d;
            return kS_OK;
        }
    }
    return kE_INVALIDARG;
}

int32_t GameInputGetCurrentReading(
    uint64_t game_input,
    uint32_t input_kind,
    uint64_t device_id,
    uint64_t* out_reading) {
    if (!out_reading) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    for (auto it = obj->history.rbegin(); it != obj->history.rend(); ++it) {
        const bool kind_ok = (input_kind == GAMEINPUT_KIND_UNKNOWN) || ((it->input_kind & input_kind) != 0);
        const bool dev_ok = (device_id == 0) || (it->device_id == device_id);
        if (kind_ok && dev_ok) {
            *out_reading = it->handle;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

int32_t GameInputGetNextReading(
    uint64_t game_input,
    uint64_t reference_reading,
    uint32_t input_kind,
    uint64_t device_id,
    uint64_t* out_reading) {
    if (!out_reading) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& e : obj->history) {
        if (e.handle <= reference_reading) continue;
        const bool kind_ok = (input_kind == GAMEINPUT_KIND_UNKNOWN) || ((e.input_kind & input_kind) != 0);
        const bool dev_ok = (device_id == 0) || (e.device_id == device_id);
        if (kind_ok && dev_ok) {
            *out_reading = e.handle;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

int32_t GameInputGetPreviousReading(
    uint64_t game_input,
    uint64_t reference_reading,
    uint32_t input_kind,
    uint64_t device_id,
    uint64_t* out_reading) {
    if (!out_reading) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    for (auto it = obj->history.rbegin(); it != obj->history.rend(); ++it) {
        if (it->handle >= reference_reading) continue;
        const bool kind_ok = (input_kind == GAMEINPUT_KIND_UNKNOWN) || ((it->input_kind & input_kind) != 0);
        const bool dev_ok = (device_id == 0) || (it->device_id == device_id);
        if (kind_ok && dev_ok) {
            *out_reading = it->handle;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

uint64_t GameInputGetReadingTimestamp(uint64_t game_input, uint64_t reading) {
    auto obj = GetInstance(game_input);
    if (!obj) return 0;
    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& e : obj->history) {
        if (e.handle == reading) return e.timestamp_us;
    }
    return 0;
}

int32_t GameInputGetGamepadState(
    uint64_t game_input,
    uint64_t reading,
    GAMEINPUT_GAMEPAD_STATE* out_state) {
    if (!out_state) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& e : obj->history) {
        if (e.handle == reading && e.input_kind == GAMEINPUT_KIND_GAMEPAD) {
            *out_state = e.gamepad;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

uint32_t GameInputGetKeyState(
    uint64_t game_input,
    uint64_t reading,
    uint32_t max_keys,
    GAMEINPUT_KEY_STATE* out_keys) {
    auto obj = GetInstance(game_input);
    if (!obj) return 0;
    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& e : obj->history) {
        if (e.handle == reading && e.input_kind == GAMEINPUT_KIND_KEYBOARD) {
            const uint32_t n = std::min<uint32_t>(max_keys, static_cast<uint32_t>(e.keys.size()));
            if (out_keys && n > 0) {
                std::memcpy(out_keys, e.keys.data(), n * sizeof(GAMEINPUT_KEY_STATE));
            }
            return n;
        }
    }
    return 0;
}

int32_t GameInputSetRumbleState(
    uint64_t game_input,
    uint64_t device_id,
    const GAMEINPUT_RUMBLE_PARAMS* params) {
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;
    std::lock_guard<std::mutex> lk(obj->mu);
    GAMEINPUT_RUMBLE_PARAMS p{};
    if (params) {
        p.LowFrequency = std::clamp(params->LowFrequency, 0.0f, 1.0f);
        p.HighFrequency = std::clamp(params->HighFrequency, 0.0f, 1.0f);
        p.LeftTrigger = std::clamp(params->LeftTrigger, 0.0f, 1.0f);
        p.RightTrigger = std::clamp(params->RightTrigger, 0.0f, 1.0f);
    }
    obj->rumble[device_id] = p;
    return kS_OK;
}

int32_t GameInputGetRumbleState(
    uint64_t game_input,
    uint64_t device_id,
    GAMEINPUT_RUMBLE_PARAMS* out_params) {
    if (!out_params) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;
    std::lock_guard<std::mutex> lk(obj->mu);
    auto it = obj->rumble.find(device_id);
    if (it == obj->rumble.end()) {
        *out_params = {};
    } else {
        *out_params = it->second;
    }
    return kS_OK;
}

int32_t GameInputInjectGamepadState(
    uint64_t game_input,
    uint64_t device_id,
    const GAMEINPUT_GAMEPAD_STATE* state) {
    if (!state) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    ReadingEntry entry{};
    entry.device_id = device_id == 0 ? kVirtualGamepadId : device_id;
    entry.input_kind = GAMEINPUT_KIND_GAMEPAD;
    entry.gamepad = *state;
    entry.gamepad.LeftTrigger = std::clamp(entry.gamepad.LeftTrigger, 0.0f, 1.0f);
    entry.gamepad.RightTrigger = std::clamp(entry.gamepad.RightTrigger, 0.0f, 1.0f);
    entry.gamepad.LeftThumbstickX = std::clamp(entry.gamepad.LeftThumbstickX, -1.0f, 1.0f);
    entry.gamepad.LeftThumbstickY = std::clamp(entry.gamepad.LeftThumbstickY, -1.0f, 1.0f);
    entry.gamepad.RightThumbstickX = std::clamp(entry.gamepad.RightThumbstickX, -1.0f, 1.0f);
    entry.gamepad.RightThumbstickY = std::clamp(entry.gamepad.RightThumbstickY, -1.0f, 1.0f);

    std::lock_guard<std::mutex> lk(obj->mu);
    obj->injected_gamepad = entry.gamepad;
    obj->gamepad_injected = true;
    obj->PushReading(std::move(entry));
    return kS_OK;
}

int32_t GameInputInjectKeyState(
    uint64_t game_input,
    uint64_t device_id,
    const GAMEINPUT_KEY_STATE* keys,
    uint32_t key_count) {
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    ReadingEntry entry{};
    entry.device_id = device_id == 0 ? kVirtualKeyboardId : device_id;
    entry.input_kind = GAMEINPUT_KIND_KEYBOARD;
    if (keys && key_count > 0) {
        entry.keys.assign(keys, keys + key_count);
    }

    std::lock_guard<std::mutex> lk(obj->mu);
    obj->PushReading(std::move(entry));
    return kS_OK;
}

}  // extern "C"
