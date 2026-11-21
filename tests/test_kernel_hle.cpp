#include "sx_check.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "scarlettx/kernel/xmemory.h"
#include "scarlettx/kernel/xruntime.h"
#include "scarlettx/kernel/xstorage.h"
#include "scarlettx/loader/package_vfs.h"

int main() {
    // 1. Unified Memory Manager (64KB & 2MB pages)
    scarlettx::UnifiedMemoryManager mem;
    scarlettx::UnifiedMemoryBlock b1{};
    auto st = mem.Allocate(10000, scarlettx::MemoryPageSize::Page64KB, true, true, b1);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(b1.size == 65536);
    SX_REQUIRE((b1.gpu_va % 65536) == 0);
    SX_REQUIRE(b1.host_ptr != nullptr);
    std::memset(b1.host_ptr, 0xAB, 256);

    scarlettx::UnifiedMemoryBlock found{};
    SX_REQUIRE(mem.LookupByGpuVa(b1.gpu_va + 128, found));
    SX_REQUIRE(found.gpu_va == b1.gpu_va);
    SX_REQUIRE(mem.ActiveAllocationCount() == 1);

    st = mem.Free(b1.gpu_va);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(mem.ActiveAllocationCount() == 0);
    SX_REQUIRE(mem.TotalAllocatedBytes() == 0);

    // 1b. 4 MiB Scarlett large page alignment & physical-to-virtual MEM_GRAPHICS aliasing
    {
        scarlettx::UnifiedMemoryBlock b4m{};
        SX_REQUIRE(mem.Allocate(1024, scarlettx::MemoryPageSize::Page4MB, true, false, b4m).ok());
        SX_REQUIRE(b4m.size == 4ull * 1024ull * 1024ull);
        SX_REQUIRE((b4m.gpu_va % (4ull * 1024ull * 1024ull)) == 0);
        SX_REQUIRE(mem.Free(b4m.gpu_va).ok());

        scarlettx::PhysicalAllocation phys{};
        SX_REQUIRE(mem.AllocatePhysicalPages(131072, scarlettx::MemoryPageSize::Page64KB,
                                             scarlettx::kMemFlagGraphics, phys)
                       .ok());
        SX_REQUIRE(phys.physical_id != 0 && phys.size == 131072);
        SX_REQUIRE(mem.ActivePhysicalPoolCount() == 1);

        // Dual independent VA views onto the same 64 KiB physical page.
        scarlettx::UnifiedMemoryAlias alias_a{};
        scarlettx::UnifiedMemoryAlias alias_b{};
        SX_REQUIRE(mem.MapPhysicalAlias(phys.physical_id, 0, 65536, scarlettx::MemoryProtection::ReadWrite,
                                        scarlettx::kMemFlagGraphics, nullptr, alias_a)
                       .ok());
        SX_REQUIRE(mem.MapPhysicalAlias(phys.physical_id, 0, 65536, scarlettx::MemoryProtection::ReadOnly,
                                        scarlettx::kMemFlagGraphics, nullptr, alias_b)
                       .ok());
        SX_REQUIRE(alias_a.host_ptr != alias_b.host_ptr);
        SX_REQUIRE(mem.ActiveAliasCount() == 2);

        static_cast<uint8_t*>(alias_a.host_ptr)[0] = 0xDEu;
        static_cast<uint8_t*>(alias_a.host_ptr)[65535] = 0xADu;
        SX_REQUIRE(static_cast<const uint8_t*>(alias_b.host_ptr)[0] == 0xDEu);
        SX_REQUIRE(static_cast<const uint8_t*>(alias_b.host_ptr)[65535] == 0xADu);

        scarlettx::UnifiedMemoryAlias looked_up{};
        SX_REQUIRE(mem.LookupAliasByHostPtr(static_cast<uint8_t*>(alias_a.host_ptr) + 123, looked_up));
        SX_REQUIRE(looked_up.physical_id == phys.physical_id);

        // Cannot free physical pool while aliases are mapped.
        SX_REQUIRE(!mem.FreePhysicalPages(phys.physical_id).ok());

        SX_REQUIRE(mem.UnmapPhysicalAlias(alias_b.host_ptr).ok());
        SX_REQUIRE(mem.UnmapPhysicalAlias(alias_a.host_ptr).ok());

        // Contiguous placeholder reservation with out-of-order physical tile aliasing (Windows 10+).
        if (scarlettx::UnifiedMemoryManager::SupportsOsPlaceholders()) {
            void* ph_base = nullptr;
            SX_REQUIRE(mem.ReserveVirtualPlaceholder(131072, ph_base).ok());
            SX_REQUIRE(ph_base != nullptr);

            auto* slot0 = static_cast<uint8_t*>(ph_base);
            auto* slot1 = slot0 + 65536;
            scarlettx::UnifiedMemoryAlias ph_a{};
            scarlettx::UnifiedMemoryAlias ph_b{};
            // Map physical page 1 at VA slot 0, and physical page 0 at VA slot 1.
            SX_REQUIRE(mem.MapPhysicalAlias(phys.physical_id, 65536, 65536, scarlettx::MemoryProtection::ReadWrite,
                                            scarlettx::kMemFlagGraphics, slot0, ph_a)
                           .ok());
            SX_REQUIRE(mem.MapPhysicalAlias(phys.physical_id, 0, 65536, scarlettx::MemoryProtection::ReadWrite,
                                            scarlettx::kMemFlagGraphics, slot1, ph_b)
                           .ok());
            SX_REQUIRE(ph_a.host_ptr == slot0 && ph_b.host_ptr == slot1);
            // Physical page 0 still contains 0xDE / 0xAD from earlier, now visible at slot1!
            SX_REQUIRE(slot1[0] == 0xDEu && slot1[65535] == 0xADu);
            slot0[0] = 0x77u;
            SX_REQUIRE(mem.ProtectAlias(slot0, 65536, scarlettx::MemoryProtection::ReadOnly).ok());
            SX_REQUIRE(slot0[0] == 0x77u);

            SX_REQUIRE(mem.UnmapPhysicalAlias(slot1).ok());
            SX_REQUIRE(mem.UnmapPhysicalAlias(slot0).ok());
            SX_REQUIRE(mem.ReleaseVirtualPlaceholder(ph_base).ok());
        }

        SX_REQUIRE(mem.FreePhysicalPages(phys.physical_id).ok());
        SX_REQUIRE(mem.ActivePhysicalPoolCount() == 0);
        SX_REQUIRE(mem.ActiveAliasCount() == 0);
        SX_REQUIRE(mem.TotalAllocatedBytes() == 0);
    }

    // 2. XGameRuntime + XTaskQueue
    auto& rt = scarlettx::XGameRuntimeHle::Instance();
    SX_REQUIRE(rt.Initialize(scarlettx::DeviceType::XboxScarlettAnaconda).ok());
    SX_REQUIRE(rt.GetDeviceType() == scarlettx::DeviceType::XboxScarlettAnaconda);

    scarlettx::XUserProfile user;
    SX_REQUIRE(rt.GetDefaultUser(user));
    SX_REQUIRE(user.signed_in && user.xuid != 0);

    const auto qh = rt.CreateTaskQueue();
    auto* q = rt.GetTaskQueue(qh);
    SX_REQUIRE(q != nullptr);
    int counter = 0;
    q->SubmitWork([&]() { counter += 10; });
    q->SubmitCompletion([&]() { counter += 5; });
    SX_REQUIRE(q->DispatchWork() == 1);
    SX_REQUIRE(q->DispatchCompletion() == 1);
    SX_REQUIRE(counter == 15);
    rt.CloseTaskQueue(qh);
    rt.Shutdown();

    // 3. PackageVfs + XStorageQueueHle
    const auto tmp_dir = std::filesystem::path(SCARLETTX_BINARY_DIR) / "tmp_vfs_test";
    std::filesystem::create_directories(tmp_dir);
    const auto data_file = tmp_dir / "asset.bin";
    {
        std::ofstream ofs(data_file, std::ios::binary);
        ofs << "SCARLETTX_DIRECTSTORAGE_PAYLOAD";
    }

    scarlettx::PackageVfs vfs;
    SX_REQUIRE(vfs.MountDirectory("G:", tmp_dir.string()).ok());
    SX_REQUIRE(vfs.Exists("G:\\asset.bin"));

    scarlettx::XStorageQueueHle storage(vfs);
    std::vector<uint8_t> dst(13, 0);
    storage.EnqueueRead({"G:\\asset.bin", 10, 13, dst.data()});
    auto results = storage.SubmitAndWait();
    SX_REQUIRE(results.size() == 1);
    SX_REQUIRE(results[0].status.ok());
    SX_REQUIRE(results[0].bytes_read == 13);
    SX_REQUIRE(std::string(dst.begin(), dst.end()) == "DIRECTSTORAGE");

    std::cout << "[test_kernel_hle] PASSED\n";
    return 0;
}
