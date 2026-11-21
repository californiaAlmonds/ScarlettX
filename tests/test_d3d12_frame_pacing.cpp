// Real-title proxy path: WaitFrameEventX paces frames to the interval set
// with SetFrameIntervalX.
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "sx_check.h"

#include <windows.h>

#include <d3d12.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    void** vtable = *reinterpret_cast<void***>(object);
    using Fn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[slot])(object, args...);
}

constexpr int kDevSetFrameIntervalX = 78;
constexpr int kDevWaitFrameEventX = 80;

double WaitFrames(void* device, int frames) {
    uint64_t token = 0, last = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        SX_REQUIRE(Call(device, kDevWaitFrameEventX, uintptr_t{0}, uintptr_t{INFINITE}, static_cast<void*>(nullptr),
                        uintptr_t{0}, &token) == 0);
        SX_REQUIRE(token > last);
        last = token;
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

int main() {
    _putenv_s("SCARLETTX_REAL_TITLE", "1");

    void* device = nullptr;
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = 1;
    const IID device_iid = __uuidof(ID3D12Device);
    SX_REQUIRE(D3D12XboxCreateDevice(nullptr, &params, &device_iid, &device) >= 0);
    SX_REQUIRE(device != nullptr);

    // No interval set: no waiting.
    SX_REQUIRE(WaitFrames(device, 100) < 100.0);

    // The values the title passes: 60 Hz, two intervals of latency.
    SX_REQUIRE(Call(device, kDevSetFrameIntervalX, static_cast<void*>(nullptr), uintptr_t{16667}, uintptr_t{2},
                    uintptr_t{0}) == 0);
    WaitFrames(device, 1);
    const double ms = WaitFrames(device, 12);
    std::printf("test_d3d12_frame_pacing: 12 frames in %.1f ms\n", ms);
    SX_REQUIRE(ms > 180.0 && ms < 400.0);

    // A slow frame is not followed by a burst to catch up.
    Sleep(100);
    WaitFrames(device, 1);
    SX_REQUIRE(WaitFrames(device, 3) > 40.0);

    std::printf("test_d3d12_frame_pacing: OK\n");
    return 0;
}
