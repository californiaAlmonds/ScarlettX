#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "scarlettx/input/gameinput_abi.h"

#define SX_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "[test_gameinput] FAILED: " #cond                      \
                      << " at line " << __LINE__ << "\n";                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#define SX_CHECK_EQ(a, b)                                                       \
    do {                                                                        \
        const auto _a = (a);                                                    \
        const auto _b = (b);                                                    \
        if (!(_a == _b)) {                                                      \
            std::cerr << "[test_gameinput] FAILED: " #a " == " #b               \
                      << " (" << _a << " != " << _b << ") at line " << __LINE__ \
                      << "\n";                                                  \
            return 1;                                                           \
        }                                                                       \
    } while (0)

// ---------------------------------------------------------------------------
// The COM surface a title uses: IGameInput*, IGameInputDevice* and
// IGameInputReading* are called through their vtables, with the slot numbers
// and structure offsets of the console GameInput.dll.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#define GI_CALL __stdcall
#else
#define GI_CALL
#endif

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    void** vtable = *reinterpret_cast<void***>(object);
    using Fn = intptr_t (GI_CALL*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[slot])(object, args...);
}

struct Guid {
    uint32_t d1;
    uint16_t d2;
    uint16_t d3;
    uint8_t d4[8];
};
const Guid kIidIUnknown = {0x00000000u, 0x0000u, 0x0000u, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const Guid kIidUnrelated = {0x12345678u, 0x1234u, 0x5678u, {1, 2, 3, 4, 5, 6, 7, 8}};

struct DeviceEvents {
    int count = 0;
    uint64_t token = 0;
    void* device = nullptr;
    uint64_t timestamp = 0;
    uint32_t current_status = 0;
    uint32_t previous_status = 0xFFFFFFFFu;
};

void GI_CALL OnDevice(uint64_t token, void* context, void* device, uint64_t timestamp, uint32_t current_status,
                      uint32_t previous_status) {
    auto* events = static_cast<DeviceEvents*>(context);
    ++events->count;
    events->token = token;
    events->device = device;
    events->timestamp = timestamp;
    events->current_status = current_status;
    events->previous_status = previous_status;
}

constexpr int kGiGetCurrentReading = 4;
constexpr int kGiRegisterDeviceCallback = 9;
constexpr int kDevGetDeviceInfo = 3;
constexpr int kDevGetDeviceStatus = 4;
constexpr int kDevSetRumbleState = 10;
constexpr int kReadGetInputKind = 3;
constexpr int kReadGetSequenceNumber = 4;
constexpr int kReadGetTimestamp = 5;
constexpr int kReadGetDevice = 6;
constexpr int kReadGetGamepadState = 22;
constexpr uint32_t kKindGamepad = 0x00040000u;  // the bit the test title checks
constexpr uint32_t kKindKeyboard = 0x00000010u;
constexpr intptr_t kReadingNotFound = static_cast<intptr_t>(static_cast<int32_t>(0x838A0002u));
constexpr intptr_t kNoInterface = static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));

int main() {
    uint64_t gi = 0;
    SX_CHECK_EQ(GameInputCreate(&gi), 0);
    SX_CHECK(gi != 0);
    SX_CHECK(GameInputGetCurrentTimestamp(gi) > 0);

    // 1. Enumerate virtual Scarlett controller
    uint64_t dev_ids[4] = {};
    uint32_t dev_count = 0;
    SX_CHECK_EQ(GameInputEnumerateDevices(gi, GAMEINPUT_KIND_GAMEPAD, dev_ids, 4, &dev_count), 0);
    SX_CHECK_EQ(dev_count, 1u);

    GAMEINPUT_DEVICE_INFO info{};
    SX_CHECK_EQ(GameInputGetDeviceInfo(gi, dev_ids[0], &info), 0);
    SX_CHECK_EQ(info.VendorId, 0x045Eu);
    SX_CHECK_EQ(info.ProductId, 0x0B12u);
    SX_CHECK_EQ(info.SupportedRumbleMotors, 0x0Fu);
    SX_CHECK(std::string(info.DisplayName).find("Xbox Series") != std::string::npos);

    // 2. Initial neutral reading
    uint64_t r0 = 0;
    SX_CHECK_EQ(GameInputGetCurrentReading(gi, GAMEINPUT_KIND_GAMEPAD, dev_ids[0], &r0), 0);
    GAMEINPUT_GAMEPAD_STATE s0{};
    SX_CHECK_EQ(GameInputGetGamepadState(gi, r0, &s0), 0);
    SX_CHECK_EQ(s0.Buttons, 0u);

    // 3. Inject two consecutive gamepad states and traverse Next/Previous history
    GAMEINPUT_GAMEPAD_STATE inj1{};
    inj1.Buttons = GAMEINPUT_GAMEPAD_A | GAMEINPUT_GAMEPAD_RIGHT_SHOULDER;
    inj1.RightTrigger = 0.75f;
    inj1.LeftThumbstickX = 0.5f;
    inj1.LeftThumbstickY = -1.5f;  // should clamp to -1.0f
    SX_CHECK_EQ(GameInputInjectGamepadState(gi, dev_ids[0], &inj1), 0);

    GAMEINPUT_GAMEPAD_STATE inj2{};
    inj2.Buttons = GAMEINPUT_GAMEPAD_X;
    inj2.LeftTrigger = 1.0f;
    SX_CHECK_EQ(GameInputInjectGamepadState(gi, dev_ids[0], &inj2), 0);

    uint64_t r1 = 0;
    SX_CHECK_EQ(GameInputGetNextReading(gi, r0, GAMEINPUT_KIND_GAMEPAD, dev_ids[0], &r1), 0);
    GAMEINPUT_GAMEPAD_STATE s1{};
    SX_CHECK_EQ(GameInputGetGamepadState(gi, r1, &s1), 0);
    SX_CHECK_EQ(s1.Buttons, static_cast<uint32_t>(GAMEINPUT_GAMEPAD_A | GAMEINPUT_GAMEPAD_RIGHT_SHOULDER));
    SX_CHECK(std::fabs(s1.RightTrigger - 0.75f) < 1e-5f);
    SX_CHECK(std::fabs(s1.LeftThumbstickY - (-1.0f)) < 1e-5f);

    uint64_t r2 = 0;
    SX_CHECK_EQ(GameInputGetCurrentReading(gi, GAMEINPUT_KIND_GAMEPAD, dev_ids[0], &r2), 0);
    SX_CHECK(r2 > r1);
    uint64_t prev = 0;
    SX_CHECK_EQ(GameInputGetPreviousReading(gi, r2, GAMEINPUT_KIND_GAMEPAD, dev_ids[0], &prev), 0);
    SX_CHECK_EQ(prev, r1);
    SX_CHECK(GameInputGetReadingTimestamp(gi, r2) >= GameInputGetReadingTimestamp(gi, r1));

    // 4. Impulse-trigger + main rumble motors
    GAMEINPUT_RUMBLE_PARAMS rumble{0.6f, 0.4f, 0.8f, 0.25f};
    SX_CHECK_EQ(GameInputSetRumbleState(gi, dev_ids[0], &rumble), 0);
    GAMEINPUT_RUMBLE_PARAMS got_rumble{};
    SX_CHECK_EQ(GameInputGetRumbleState(gi, dev_ids[0], &got_rumble), 0);
    SX_CHECK(std::fabs(got_rumble.LowFrequency - 0.6f) < 1e-5f);
    SX_CHECK(std::fabs(got_rumble.LeftTrigger - 0.8f) < 1e-5f);

    // 5. Keyboard injection & reading
    GAMEINPUT_KEY_STATE keys[2]{};
    keys[0].VirtualKey = 'W';
    keys[0].CodePoint = 'w';
    keys[1].VirtualKey = ' ';
    keys[1].CodePoint = ' ';
    SX_CHECK_EQ(GameInputInjectKeyState(gi, 0, keys, 2), 0);

    uint64_t rkb = 0;
    SX_CHECK_EQ(GameInputGetCurrentReading(gi, GAMEINPUT_KIND_KEYBOARD, 0, &rkb), 0);
    GAMEINPUT_KEY_STATE read_keys[4]{};
    const uint32_t nkeys = GameInputGetKeyState(gi, rkb, 4, read_keys);
    SX_CHECK_EQ(nkeys, 2u);
    SX_CHECK_EQ(read_keys[0].VirtualKey, 'W');

    // 5. The same object through its vtable, as a title calls it.
    {
        void* input = reinterpret_cast<void*>(gi);

        // A blocking-enumeration registration reports the gamepad before it
        // returns (values as the test title passes them).
        DeviceEvents events;
        uint64_t token = 0;
        SX_CHECK_EQ(Call(input, kGiRegisterDeviceCallback, static_cast<void*>(nullptr), uintptr_t{0x0FFFFFFF},
                         uintptr_t{0x00FFFFFF}, uintptr_t{0x100000002ull}, static_cast<void*>(&events), &OnDevice,
                         &token),
                    intptr_t{0});
        SX_CHECK_EQ(events.count, 1);
        SX_CHECK(token != 0 && events.token == token);
        SX_CHECK(events.device != nullptr);
        SX_CHECK_EQ(events.current_status & 3u, 3u);
        SX_CHECK_EQ(events.previous_status, 0u);

        // A keyboard-only registration does not report the gamepad.
        DeviceEvents keyboard_events;
        SX_CHECK_EQ(Call(input, kGiRegisterDeviceCallback, static_cast<void*>(nullptr), uintptr_t{kKindKeyboard},
                         uintptr_t{0x00FFFFFF}, uintptr_t{2}, static_cast<void*>(&keyboard_events), &OnDevice,
                         &token),
                    intptr_t{0});
        SX_CHECK_EQ(keyboard_events.count, 0);

        void* device = events.device;
        void* same = nullptr;
        SX_CHECK_EQ(Call(device, 0, &kIidUnrelated, &same), kNoInterface);
        SX_CHECK(same == nullptr);
        SX_CHECK_EQ(Call(device, 0, &kIidIUnknown, &same), intptr_t{0});
        SX_CHECK(same == device);
        Call(device, 2);

        // The three things the test title reads from the device information.
        const auto* info = reinterpret_cast<const uint8_t*>(Call(device, kDevGetDeviceInfo));
        SX_CHECK(info != nullptr);
        uint32_t supported = 0;
        std::memcpy(&supported, info + 0x68, sizeof(supported));
        SX_CHECK((supported & kKindGamepad) != 0);
        bool id_set = false;
        for (int i = 0; i < 32; ++i) id_set = id_set || info[0x20 + i] != 0;
        SX_CHECK(id_set);
        const uint8_t* name = nullptr;
        std::memcpy(&name, info + 0x130, sizeof(name));
        SX_CHECK(name != nullptr);
        const char* name_text = nullptr;
        std::memcpy(&name_text, name + 8, sizeof(name_text));
        SX_CHECK(name_text != nullptr && std::strlen(name_text) > 0);
        SX_CHECK_EQ(Call(device, kDevGetDeviceStatus) & 3, intptr_t{3});

        // Rumble is accepted, with or without a host controller; null stops it.
        const GAMEINPUT_RUMBLE_PARAMS buzz{0.25f, 0.25f, 0.0f, 0.0f};
        SX_CHECK_EQ(Call(device, kDevSetRumbleState, &buzz), intptr_t{0});
        SX_CHECK_EQ(Call(device, kDevSetRumbleState, static_cast<void*>(nullptr)), intptr_t{0});

        // Readings carry the injected state.
        GAMEINPUT_GAMEPAD_STATE pressed{};
        pressed.Buttons = GAMEINPUT_GAMEPAD_A | GAMEINPUT_GAMEPAD_DPAD_LEFT;
        pressed.RightTrigger = 0.5f;
        pressed.LeftThumbstickX = -0.25f;
        SX_CHECK_EQ(GameInputInjectGamepadState(gi, 0, &pressed), 0);

        void* reading = nullptr;
        SX_CHECK_EQ(Call(input, kGiGetCurrentReading, uintptr_t{kKindGamepad}, device, &reading), intptr_t{0});
        SX_CHECK(reading != nullptr);
        SX_CHECK_EQ(static_cast<uint32_t>(Call(reading, kReadGetInputKind)), kKindGamepad);
        SX_CHECK(Call(reading, kReadGetTimestamp) != 0);
        GAMEINPUT_GAMEPAD_STATE got{};
        SX_CHECK_EQ(Call(reading, kReadGetGamepadState, &got) & 0xFF, intptr_t{1});
        SX_CHECK_EQ(got.Buttons, pressed.Buttons);
        SX_CHECK(got.RightTrigger == 0.5f && got.LeftThumbstickX == -0.25f && got.LeftTrigger == 0.0f);
        void* reading_device = nullptr;
        Call(reading, kReadGetDevice, &reading_device);
        SX_CHECK(reading_device == device);
        Call(reading_device, 2);
        SX_CHECK_EQ(Call(reading, 0, &kIidUnrelated, &same), kNoInterface);
        // A reading has no keys, switches or raw report.
        SX_CHECK_EQ(Call(reading, 14), intptr_t{0});
        const intptr_t sequence = Call(reading, kReadGetSequenceNumber, uintptr_t{kKindGamepad});
        SX_CHECK(sequence != 0);
        SX_CHECK_EQ(Call(reading, kReadGetSequenceNumber, uintptr_t{kKindKeyboard}), intptr_t{0});
        SX_CHECK_EQ(Call(reading, 2), intptr_t{0});

        // The sequence number moves only when the state does.
        SX_CHECK_EQ(Call(input, kGiGetCurrentReading, uintptr_t{kKindGamepad}, static_cast<void*>(nullptr), &reading),
                    intptr_t{0});
        SX_CHECK_EQ(Call(reading, kReadGetSequenceNumber, uintptr_t{kKindGamepad}), sequence);
        Call(reading, 2);
        pressed.Buttons = GAMEINPUT_GAMEPAD_B;
        SX_CHECK_EQ(GameInputInjectGamepadState(gi, 0, &pressed), 0);
        SX_CHECK_EQ(Call(input, kGiGetCurrentReading, uintptr_t{kKindGamepad}, static_cast<void*>(nullptr), &reading),
                    intptr_t{0});
        SX_CHECK_EQ(Call(reading, kReadGetSequenceNumber, uintptr_t{kKindGamepad}), sequence + 1);
        SX_CHECK_EQ(Call(reading, kReadGetGamepadState, &got) & 0xFF, intptr_t{1});
        SX_CHECK_EQ(got.Buttons, static_cast<uint32_t>(GAMEINPUT_GAMEPAD_B));
        Call(reading, 2);

        // There are no keyboard readings on this path, and no readings for
        // an object that is not the gamepad.
        reading = input;
        SX_CHECK_EQ(Call(input, kGiGetCurrentReading, uintptr_t{kKindKeyboard}, static_cast<void*>(nullptr), &reading),
                    kReadingNotFound);
        SX_CHECK(reading == nullptr);
        SX_CHECK_EQ(Call(input, kGiGetCurrentReading, uintptr_t{kKindGamepad}, input, &reading), kReadingNotFound);
    }

    SX_CHECK_EQ(GameInputDestroy(gi), 0);
    std::cout << "[test_gameinput] All GameInput HLE checks passed.\n";
    return 0;
}
