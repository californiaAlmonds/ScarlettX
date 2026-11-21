// Real-title proxy path: GetDesc of a descriptor heap and of a heap. The
// description comes back in a buffer the caller provides, whose address is
// also the return value, and holds what the object was created with.
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
constexpr int kGetDesc = 8;
constexpr int kDevCreateDescriptorHeap = 14;
constexpr int kDevCreateHeap = 28;

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

    D3D12_DESCRIPTOR_HEAP_DESC dhd = {};
    dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    dhd.NumDescriptors = 37;
    dhd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    void* descriptor_heap = nullptr;
    SX_REQUIRE(Call(device, kDevCreateDescriptorHeap, &dhd, static_cast<void*>(nullptr), &descriptor_heap) == 0);
    SX_REQUIRE(descriptor_heap != nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC dhd_out;
    std::memset(&dhd_out, 0xCC, sizeof(dhd_out));
    SX_REQUIRE(Call(descriptor_heap, kGetDesc, &dhd_out) == reinterpret_cast<intptr_t>(&dhd_out));
    SX_REQUIRE(dhd_out.Type == dhd.Type);
    SX_REQUIRE(dhd_out.NumDescriptors == dhd.NumDescriptors);
    SX_REQUIRE(dhd_out.Flags == dhd.Flags);
    SX_REQUIRE(dhd_out.NodeMask == dhd.NodeMask);
    SX_REQUIRE(Call(descriptor_heap, kRelease) == 0);

    D3D12_HEAP_DESC hd = {};
    hd.SizeInBytes = 0xFE0000;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Properties.CreationNodeMask = 1;
    hd.Properties.VisibleNodeMask = 1;
    hd.Alignment = 0x10000;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    const IID heap_iid = __uuidof(ID3D12Heap);
    void* heap = nullptr;
    SX_REQUIRE(Call(device, kDevCreateHeap, &hd, &heap_iid, &heap) == 0);
    SX_REQUIRE(heap != nullptr);

    D3D12_HEAP_DESC hd_out;
    std::memset(&hd_out, 0xCC, sizeof(hd_out));
    SX_REQUIRE(Call(heap, kGetDesc, &hd_out) == reinterpret_cast<intptr_t>(&hd_out));
    SX_REQUIRE(hd_out.SizeInBytes == hd.SizeInBytes);
    SX_REQUIRE(hd_out.Properties.Type == hd.Properties.Type);
    SX_REQUIRE(hd_out.Properties.CPUPageProperty == hd.Properties.CPUPageProperty);
    SX_REQUIRE(hd_out.Properties.MemoryPoolPreference == hd.Properties.MemoryPoolPreference);
    SX_REQUIRE(hd_out.Properties.CreationNodeMask == hd.Properties.CreationNodeMask);
    SX_REQUIRE(hd_out.Properties.VisibleNodeMask == hd.Properties.VisibleNodeMask);
    SX_REQUIRE(hd_out.Alignment == hd.Alignment);
    SX_REQUIRE(hd_out.Flags == hd.Flags);
    SX_REQUIRE(Call(heap, kRelease) == 0);

    std::printf("test_d3d12_proxy_getdesc: OK\n");
    return 0;
}
