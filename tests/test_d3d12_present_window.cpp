// Real-title proxy path: the output window of ID3D12CommandQueue::PresentX.
// The window must not be able to stop the thread that presents: while the
// window sits in a modal loop (here its system menu), PresentX still returns.
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "sx_check.h"

#include <windows.h>

#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    void** vtable = *reinterpret_cast<void***>(object);
    using Fn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[slot])(object, args...);
}

constexpr int kRelease = 2;
constexpr int kDevCreateCommandQueue = 8;
constexpr int kDevCreateCommittedResource = 27;
constexpr int kQueuePresentX = 30;

HWND FindPresentWindow() {
    HWND hwnd = nullptr;
    while ((hwnd = FindWindowExA(nullptr, hwnd, "ScarlettXPresentWindowClass", nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) return hwnd;
    }
    return nullptr;
}

bool InMenuMode(DWORD thread_id) {
    GUITHREADINFO info = {};
    info.cbSize = sizeof(info);
    return GetGUIThreadInfo(thread_id, &info) && (info.flags & GUI_INMENUMODE) != 0;
}

bool WaitForMenuMode(DWORD thread_id, bool wanted, DWORD timeout_ms) {
    for (DWORD waited = 0; waited <= timeout_ms; waited += 20) {
        if (InMenuMode(thread_id) == wanted) return true;
        Sleep(20);
    }
    return false;
}

// Ends the test if the presenting thread gets stuck, where CTest would wait
// out its own much longer timeout.
HANDLE g_presents_done = nullptr;
DWORD WINAPI Watchdog(void*) {
    if (WaitForSingleObject(g_presents_done, 120000) == WAIT_TIMEOUT) {
        std::fprintf(stderr, "test_d3d12_present_window: PresentX did not return while the window was busy\n");
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 1);
    }
    return 0;
}

}  // namespace

int main() {
    // Select the guest-facing proxies (also set by CTest for this test).
    _putenv_s("SCARLETTX_REAL_TITLE", "1");

    void* device = nullptr;
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = 1;
    const IID device_iid = __uuidof(ID3D12Device);
    SX_REQUIRE(D3D12XboxCreateDevice(nullptr, &params, &device_iid, &device) >= 0);
    SX_REQUIRE(device != nullptr);

    void* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    SX_REQUIRE(Call(device, kDevCreateCommandQueue, &qdesc, static_cast<void*>(nullptr), &queue) == 0);
    SX_REQUIRE(queue != nullptr);

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 256;
    td.Height = 144;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    void* frame = nullptr;
    SX_REQUIRE(Call(device, kDevCreateCommittedResource, &hp, uintptr_t{0}, &td,
                    static_cast<uintptr_t>(D3D12_RESOURCE_STATE_COMMON), static_cast<void*>(nullptr),
                    static_cast<void*>(nullptr), &frame) == 0);
    SX_REQUIRE(frame != nullptr);

    // Plane parameters as the title passes them: the second quadword is a
    // count, the third the resource to show.
    uint64_t plane[8] = {};
    plane[1] = 1;
    plane[2] = reinterpret_cast<uint64_t>(frame);

    SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);

    const HWND hwnd = FindPresentWindow();
    SX_REQUIRE(hwnd != nullptr);
    const DWORD window_thread = GetWindowThreadProcessId(hwnd, nullptr);
    SX_REQUIRE(window_thread != 0);
    SX_REQUIRE(window_thread != GetCurrentThreadId());

    // Alt or F10 alone does not put the window into menu mode.
    DWORD_PTR result = 0;
    SX_REQUIRE(SendMessageTimeoutA(hwnd, WM_SYSCOMMAND, SC_KEYMENU, 0, SMTO_ABORTIFHUNG, 10000, &result) != 0);
    SX_REQUIRE(!InMenuMode(window_thread));

    // Alt+Enter covers the monitor without a frame, and a second one undoes it.
    const LPARAM alt_enter = 1 << 29;
    SX_REQUIRE(SendMessageTimeoutA(hwnd, WM_SYSKEYDOWN, VK_RETURN, alt_enter, SMTO_ABORTIFHUNG, 10000, &result) != 0);
    MONITORINFO mi = {sizeof(mi)};
    RECT rc = {};
    SX_REQUIRE(GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi) && GetWindowRect(hwnd, &rc));
    SX_REQUIRE(EqualRect(&rc, &mi.rcMonitor));
    SX_REQUIRE((GetWindowLongPtrA(hwnd, GWL_STYLE) & WS_CAPTION) == 0);
    SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
    SX_REQUIRE(SendMessageTimeoutA(hwnd, WM_SYSKEYDOWN, VK_RETURN, alt_enter, SMTO_ABORTIFHUNG, 10000, &result) != 0);
    SX_REQUIRE(GetWindowRect(hwnd, &rc) && !EqualRect(&rc, &mi.rcMonitor));
    SX_REQUIRE((GetWindowLongPtrA(hwnd, GWL_STYLE) & WS_CAPTION) != 0);

    // Closing the window is not a way to take the output away from the title.
    SX_REQUIRE(SendMessageTimeoutA(hwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 10000, &result) != 0);
    SX_REQUIRE(IsWindow(hwnd));

    // Open the system menu: a modal loop inside DefWindowProc.
    PostMessageA(hwnd, WM_SYSCOMMAND, SC_KEYMENU, ' ');  // Alt+Space
    const bool modal = WaitForMenuMode(window_thread, true, 5000);
    if (!modal) {
        std::printf("test_d3d12_present_window: the system menu did not open on this desktop; "
                    "presenting during a modal loop was NOT checked\n");
    }

    g_presents_done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    SX_REQUIRE(g_presents_done != nullptr);
    const HANDLE watchdog = CreateThread(nullptr, 0, &Watchdog, nullptr, 0, nullptr);
    SX_REQUIRE(watchdog != nullptr);
    for (int i = 0; i < 3; ++i) {
        SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
    }
    SetEvent(g_presents_done);
    WaitForSingleObject(watchdog, INFINITE);
    CloseHandle(watchdog);

    if (modal) {
        // The menu was open for all three presents.
        SX_REQUIRE(InMenuMode(window_thread));
        SX_REQUIRE(SendMessageTimeoutA(hwnd, WM_CANCELMODE, 0, 0, SMTO_ABORTIFHUNG, 10000, &result) != 0);
        SX_REQUIRE(WaitForMenuMode(window_thread, false, 5000));
    }

    // The window goes away with the queue.
    SX_REQUIRE(Call(queue, kRelease) == 0);
    SX_REQUIRE(!IsWindow(hwnd));

    std::printf("test_d3d12_present_window: OK%s\n", modal ? "" : " (modal part skipped)");
    return 0;
}
