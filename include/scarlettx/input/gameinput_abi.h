#pragma once

#include <cstdint>

#if defined(_WIN32)
#  if defined(SCARLETTX_BUILDING_GAMEINPUT)
#    define SCARLETTX_GAMEINPUT_API __declspec(dllexport)
#  else
#    define SCARLETTX_GAMEINPUT_API __declspec(dllimport)
#  endif
#else
#  define SCARLETTX_GAMEINPUT_API __attribute__((visibility("default")))
#endif

extern "C" {

enum GAMEINPUT_KIND : uint32_t {
    GAMEINPUT_KIND_UNKNOWN = 0x00000000,
    GAMEINPUT_KIND_RAW_DEVICE_REPORT = 0x00000001,
    GAMEINPUT_KIND_CONTROLLER_AXIS = 0x00000002,
    GAMEINPUT_KIND_CONTROLLER_BUTTON = 0x00000004,
    GAMEINPUT_KIND_CONTROLLER_SWITCH = 0x00000008,
    GAMEINPUT_KIND_KEYBOARD = 0x00000010,
    GAMEINPUT_KIND_MOUSE = 0x00000020,
    GAMEINPUT_KIND_GAMEPAD = 0x00040000,
};

enum GAMEINPUT_GAMEPAD_BUTTONS : uint32_t {
    GAMEINPUT_GAMEPAD_NONE = 0x00000000,
    GAMEINPUT_GAMEPAD_MENU = 0x00000001,
    GAMEINPUT_GAMEPAD_VIEW = 0x00000002,
    GAMEINPUT_GAMEPAD_A = 0x00000004,
    GAMEINPUT_GAMEPAD_B = 0x00000008,
    GAMEINPUT_GAMEPAD_X = 0x00000010,
    GAMEINPUT_GAMEPAD_Y = 0x00000020,
    GAMEINPUT_GAMEPAD_DPAD_UP = 0x00000040,
    GAMEINPUT_GAMEPAD_DPAD_DOWN = 0x00000080,
    GAMEINPUT_GAMEPAD_DPAD_LEFT = 0x00000100,
    GAMEINPUT_GAMEPAD_DPAD_RIGHT = 0x00000200,
    GAMEINPUT_GAMEPAD_LEFT_SHOULDER = 0x00000400,
    GAMEINPUT_GAMEPAD_RIGHT_SHOULDER = 0x00000800,
    GAMEINPUT_GAMEPAD_LEFT_THUMBSTICK = 0x00001000,
    GAMEINPUT_GAMEPAD_RIGHT_THUMBSTICK = 0x00002000,
};

struct GAMEINPUT_DEVICE_INFO {
    uint64_t DeviceId;
    uint16_t VendorId;
    uint16_t ProductId;
    uint32_t SupportedInput;
    uint32_t SupportedRumbleMotors;
    char DisplayName[64];
};

struct GAMEINPUT_GAMEPAD_STATE {
    uint32_t Buttons;
    float LeftTrigger;
    float RightTrigger;
    float LeftThumbstickX;
    float LeftThumbstickY;
    float RightThumbstickX;
    float RightThumbstickY;
};

struct GAMEINPUT_KEY_STATE {
    uint32_t ScanCode;
    uint32_t CodePoint;
    uint8_t VirtualKey;
    uint8_t IsDeadKey;
    uint16_t Reserved;
};

struct GAMEINPUT_RUMBLE_PARAMS {
    float LowFrequency;
    float HighFrequency;
    float LeftTrigger;
    float RightTrigger;
};

SCARLETTX_GAMEINPUT_API int32_t GameInputCreate(uint64_t* out_game_input);
SCARLETTX_GAMEINPUT_API int32_t GameInputDestroy(uint64_t game_input);
SCARLETTX_GAMEINPUT_API uint64_t GameInputGetCurrentTimestamp(uint64_t game_input);

SCARLETTX_GAMEINPUT_API int32_t GameInputEnumerateDevices(
    uint64_t game_input,
    uint32_t input_kind,
    uint64_t* out_device_ids,
    uint32_t capacity,
    uint32_t* out_count);
SCARLETTX_GAMEINPUT_API int32_t GameInputGetDeviceInfo(
    uint64_t game_input,
    uint64_t device_id,
    GAMEINPUT_DEVICE_INFO* out_info);

SCARLETTX_GAMEINPUT_API int32_t GameInputGetCurrentReading(
    uint64_t game_input,
    uint32_t input_kind,
    uint64_t device_id,
    uint64_t* out_reading);
SCARLETTX_GAMEINPUT_API int32_t GameInputGetNextReading(
    uint64_t game_input,
    uint64_t reference_reading,
    uint32_t input_kind,
    uint64_t device_id,
    uint64_t* out_reading);
SCARLETTX_GAMEINPUT_API int32_t GameInputGetPreviousReading(
    uint64_t game_input,
    uint64_t reference_reading,
    uint32_t input_kind,
    uint64_t device_id,
    uint64_t* out_reading);
SCARLETTX_GAMEINPUT_API uint64_t GameInputGetReadingTimestamp(
    uint64_t game_input,
    uint64_t reading);
SCARLETTX_GAMEINPUT_API int32_t GameInputGetGamepadState(
    uint64_t game_input,
    uint64_t reading,
    GAMEINPUT_GAMEPAD_STATE* out_state);
SCARLETTX_GAMEINPUT_API uint32_t GameInputGetKeyState(
    uint64_t game_input,
    uint64_t reading,
    uint32_t max_keys,
    GAMEINPUT_KEY_STATE* out_keys);

SCARLETTX_GAMEINPUT_API int32_t GameInputSetRumbleState(
    uint64_t game_input,
    uint64_t device_id,
    const GAMEINPUT_RUMBLE_PARAMS* params);
SCARLETTX_GAMEINPUT_API int32_t GameInputGetRumbleState(
    uint64_t game_input,
    uint64_t device_id,
    GAMEINPUT_RUMBLE_PARAMS* out_params);

// Synthetic input injection for autonomous testing & headless automation
SCARLETTX_GAMEINPUT_API int32_t GameInputInjectGamepadState(
    uint64_t game_input,
    uint64_t device_id,
    const GAMEINPUT_GAMEPAD_STATE* state);
SCARLETTX_GAMEINPUT_API int32_t GameInputInjectKeyState(
    uint64_t game_input,
    uint64_t device_id,
    const GAMEINPUT_KEY_STATE* keys,
    uint32_t key_count);

enum GAMEINPUT_DEVICE_STATUS : uint32_t {
    GAMEINPUT_DEVICE_NO_STATUS = 0x00000000,
    GAMEINPUT_DEVICE_CONNECTED = 0x00000001,
    GAMEINPUT_DEVICE_INPUT_ENABLED = 0x00000002,
    GAMEINPUT_DEVICE_OUTPUT_ENABLED = 0x00000004,
    GAMEINPUT_DEVICE_ANY_STATUS = 0x00FFFFFF,
};

enum GAMEINPUT_ENUMERATION_KIND : uint32_t {
    GAMEINPUT_NO_ENUMERATION = 0,
    GAMEINPUT_ASYNC_ENUMERATION = 1,
    GAMEINPUT_BLOCKING_ENUMERATION = 2,
};

typedef void (*GAMEINPUT_DEVICE_CALLBACK)(
    uint64_t callback_token,
    void* context,
    uint64_t device_id,
    uint64_t timestamp_us,
    uint32_t current_status,
    uint32_t previous_status);

SCARLETTX_GAMEINPUT_API int32_t GameInputRegisterDeviceCallback(
    uint64_t game_input,
    uint64_t device_id,
    uint32_t input_filter,
    uint32_t status_filter,
    uint32_t enumeration_kind,
    void* context,
    GAMEINPUT_DEVICE_CALLBACK callback,
    uint64_t* out_callback_token);

SCARLETTX_GAMEINPUT_API int32_t GameInputUnregisterCallback(
    uint64_t game_input,
    uint64_t callback_token,
    uint64_t timeout_us);

}  // extern "C"
