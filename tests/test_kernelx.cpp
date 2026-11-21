#include "sx_check.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "scarlettx/kernel/gpu_tile_pool.h"
#include "scarlettx/kernel/kernelx_abi.h"
#include "scarlettx/kernel/xmemory.h"

namespace {

constexpr uint64_t kExpectedAnacondaTitleMem = 27ull * 512ull * 1024ull * 1024ull;    // 13.5 GiB
constexpr uint64_t kExpectedAnacondaToolingMem = 5ull * 512ull * 1024ull * 1024ull;   // 2.5 GiB
constexpr size_t kPage64KB = 65536u;
constexpr size_t kPage4MB = 4ull * 1024ull * 1024ull;

void TestSystemQueriesAndMemoryStatus() {
    // 1. Console type & OS version
    const uint32_t console_type = GetConsoleType();
    SX_REQUIRE(console_type == CONSOLE_TYPE_XBOX_SCARLETT_ANACONDA);

    KERNELX_OS_VERSION os_ver{};
    SX_REQUIRE(GetSystemOSVersion(&os_ver) != 0);
    SX_REQUIRE(os_ver.MajorVersion == 10u);
    SX_REQUIRE(os_ver.MinorVersion == 0u);
    SX_REQUIRE(os_ver.BuildNumber == 26100u);
    SX_REQUIRE(os_ver.Revision > 0u);
    SX_REQUIRE(GetSystemOSVersion(nullptr) == 0);

    // 2. ToolingMemoryStatus (2.5 GiB Anaconda tooling pool)
    TOOLINGMEMORYSTATUS tooling{};
    tooling.dwLength = sizeof(TOOLINGMEMORYSTATUS);
    SX_REQUIRE(ToolingMemoryStatus(&tooling) != 0);
    SX_REQUIRE(tooling.dwLength == sizeof(TOOLINGMEMORYSTATUS));
    SX_REQUIRE(tooling.ullTotalMem == kExpectedAnacondaToolingMem);
    SX_REQUIRE(tooling.ullAvailMem == tooling.ullTotalMem - tooling.ullUsed);
    SX_REQUIRE(ToolingMemoryStatus(nullptr) == 0);

    // 3. TitleMemoryStatus baseline & validation
    TITLEMEMORYSTATUS before{};
    before.dwLength = sizeof(TITLEMEMORYSTATUS);
    SX_REQUIRE(TitleMemoryStatus(&before) != 0);
    SX_REQUIRE(before.dwLength == sizeof(TITLEMEMORYSTATUS));
    SX_REQUIRE(before.ullTotalMem == kExpectedAnacondaTitleMem);
    SX_REQUIRE(before.ullAvailMem == before.ullTotalMem - before.ullTitleUsed);
    SX_REQUIRE(before.ullTitleAvail == before.ullAvailMem);
    SX_REQUIRE(TitleMemoryStatus(nullptr) == 0);

    TITLEMEMORYSTATUS bad_len{};
    bad_len.dwLength = 8u;
    SX_REQUIRE(TitleMemoryStatus(&bad_len) == 0);

    // 4. Live TitleMemoryStatus tracking across 64 KiB and 4 MiB allocations
    uint64_t num_64k = 4;
    std::array<uint64_t, 4> pages_64k{};
    SX_REQUIRE(AllocateTitlePhysicalPages(nullptr, MEM_GRAPHICS, &num_64k, pages_64k.data()) != 0);
    SX_REQUIRE(num_64k == 4);
    for (uint64_t pfn : pages_64k) {
        SX_REQUIRE(pfn != 0);
    }

    TITLEMEMORYSTATUS after_64k{};
    after_64k.dwLength = sizeof(TITLEMEMORYSTATUS);
    SX_REQUIRE(TitleMemoryStatus(&after_64k) != 0);
    SX_REQUIRE(after_64k.ullTitleUsed == before.ullTitleUsed + 4ull * kPage64KB);
    SX_REQUIRE(after_64k.ullAvailMem == before.ullAvailMem - 4ull * kPage64KB);
    SX_REQUIRE(after_64k.ullLegacyPeak >= after_64k.ullTitleUsed);

    uint64_t num_4m = 1;
    uint64_t page_4m = 0;
    SX_REQUIRE(AllocateTitlePhysicalPages(nullptr, MEM_GRAPHICS | MEM_LARGE_PAGES, &num_4m, &page_4m) != 0);
    SX_REQUIRE(page_4m != 0);

    TITLEMEMORYSTATUS after_4m{};
    after_4m.dwLength = sizeof(TITLEMEMORYSTATUS);
    SX_REQUIRE(TitleMemoryStatus(&after_4m) != 0);
    SX_REQUIRE(after_4m.ullTitleUsed == after_64k.ullTitleUsed + kPage4MB);
    SX_REQUIRE(after_4m.ullAvailMem == after_64k.ullAvailMem - kPage4MB);

    SX_REQUIRE(FreeTitlePhysicalPages(nullptr, 1, &page_4m) != 0);
    SX_REQUIRE(FreeTitlePhysicalPages(nullptr, 4, pages_64k.data()) != 0);

    // Double-free must fail cleanly
    SX_REQUIRE(FreeTitlePhysicalPages(nullptr, 1, &page_4m) == 0);

    TITLEMEMORYSTATUS after_free{};
    after_free.dwLength = sizeof(TITLEMEMORYSTATUS);
    SX_REQUIRE(TitleMemoryStatus(&after_free) != 0);
    SX_REQUIRE(after_free.ullTitleUsed == before.ullTitleUsed);
    SX_REQUIRE(after_free.ullAvailMem == before.ullAvailMem);
    SX_REQUIRE(after_free.ullLegacyPeak >= after_4m.ullTitleUsed);
}

void TestDualVaAliasingAndProtection() {
    uint64_t num_pages = 2;
    std::array<uint64_t, 2> pages{};
    SX_REQUIRE(AllocateTitlePhysicalPages(nullptr, MEM_GRAPHICS, &num_pages, pages.data()) != 0);

    // Map the exact same 2 physical pages (128 KiB) at two distinct virtual addresses.
    void* va1 = MapTitlePhysicalPages(nullptr, num_pages, MEM_GRAPHICS, PAGE_READWRITE, pages.data());
    void* va2 = MapTitlePhysicalPages(nullptr, num_pages, MEM_GRAPHICS, PAGE_READWRITE, pages.data());
    SX_REQUIRE(va1 != nullptr);
    SX_REQUIRE(va2 != nullptr);
    SX_REQUIRE(va1 != va2);

    auto* p1 = static_cast<uint8_t*>(va1);
    auto* p2 = static_cast<uint8_t*>(va2);

    // Write distinct patterns across both pages via VA1 and verify exact match via VA2.
    for (size_t i = 0; i < 2 * kPage64KB; i += 257) {
        p1[i] = static_cast<uint8_t>((i * 13u + 0x5Au) & 0xFFu);
    }
    for (size_t i = 0; i < 2 * kPage64KB; i += 257) {
        SX_REQUIRE(p2[i] == static_cast<uint8_t>((i * 13u + 0x5Au) & 0xFFu));
    }

    p1[0] = 0xCAu;
    p1[kPage64KB - 1] = 0xFEu;
    p1[kPage64KB] = 0xBAu;
    p1[2 * kPage64KB - 1] = 0xBEu;

    SX_REQUIRE(p2[0] == 0xCAu);
    SX_REQUIRE(p2[kPage64KB - 1] == 0xFEu);
    SX_REQUIRE(p2[kPage64KB] == 0xBAu);
    SX_REQUIRE(p2[2 * kPage64KB - 1] == 0xBEu);

    // Modify through VA2 and verify immediately through VA1.
    const char* msg = "ScarlettX_DualVA_Aliasing_Verified!";
    std::memcpy(p2 + kPage64KB + 128, msg, std::strlen(msg) + 1);
    SX_REQUIRE(std::strcmp(reinterpret_cast<const char*>(p1 + kPage64KB + 128), msg) == 0);

    // QueryTitlePhysicalPages on both VAs (and interior offset) must report same physical pool & size.
    uint64_t phys1 = 0, phys2 = 0, size1 = 0, size2 = 0;
    uint32_t flags1 = 0, flags2 = 0;
    SX_REQUIRE(QueryTitlePhysicalPages(va1, &phys1, &size1, &flags1) != 0);
    SX_REQUIRE(QueryTitlePhysicalPages(p2 + kPage64KB + 64, &phys2, &size2, &flags2) != 0);
    SX_REQUIRE(phys1 != 0 && phys1 == phys2);
    SX_REQUIRE(size1 == 2 * kPage64KB && size2 == 2 * kPage64KB);
    SX_REQUIRE((flags1 & MEM_GRAPHICS) != 0u);
    SX_REQUIRE((flags2 & MEM_GRAPHICS) != 0u);

    // VirtualProtectFromApp on VA1 (switch to READONLY) while VA2 remains READWRITE!
    uint32_t old_prot = 0;
    SX_REQUIRE(VirtualProtectFromApp(va1, 2 * kPage64KB, PAGE_READONLY, &old_prot) != 0);
    SX_REQUIRE(old_prot == PAGE_READWRITE);

    // Write via VA2, read updated byte through READONLY VA1!
    p2[42] = 0x99u;
    SX_REQUIRE(p1[42] == 0x99u);

    // Restore VA1 back to READWRITE.
    uint32_t old_prot2 = 0;
    SX_REQUIRE(VirtualProtectFromApp(va1, 2 * kPage64KB, PAGE_READWRITE, &old_prot2) != 0);
    SX_REQUIRE(old_prot2 == PAGE_READONLY);
    p1[42] = 0x77u;
    SX_REQUIRE(p2[42] == 0x77u);

    // Cannot free physical pages while any VA alias is still mapped.
    SX_REQUIRE(FreeTitlePhysicalPages(nullptr, num_pages, pages.data()) == 0);

    // Unmap VA1; VA2 must remain completely intact and valid!
    SX_REQUIRE(UnmapTitlePhysicalPages(va1, num_pages) != 0);
    SX_REQUIRE(QueryTitlePhysicalPages(va1, nullptr, nullptr, nullptr) == 0);
    SX_REQUIRE(p2[42] == 0x77u);
    SX_REQUIRE(p2[2 * kPage64KB - 1] == 0xBEu);

    // Still mapped via VA2 -> cannot free physical pages yet.
    SX_REQUIRE(FreeTitlePhysicalPages(nullptr, num_pages, pages.data()) == 0);

    // Reordered / non-contiguous mapping (pages[1] at slot 0, pages[0] at slot 1) via placeholder!
    if (scarlettx::UnifiedMemoryManager::SupportsOsPlaceholders()) {
        std::array<uint64_t, 2> swapped = {pages[1], pages[0]};
        void* va_swap = MapTitlePhysicalPages(nullptr, 2, MEM_GRAPHICS, PAGE_READWRITE, swapped.data());
        SX_REQUIRE(va_swap != nullptr);
        auto* ps = static_cast<uint8_t*>(va_swap);
        // Page 1's first byte (0xBA) is now at slot 0; Page 0's byte 42 (0x77) is now at slot 1 + 42!
        SX_REQUIRE(ps[0] == 0xBAu);
        SX_REQUIRE(ps[kPage64KB + 42] == 0x77u);
        SX_REQUIRE(UnmapTitlePhysicalPages(va_swap, 2) != 0);

        // Also test caller-supplied MEM_RESERVE | MEM_GRAPHICS placeholder from VirtualAllocFromApp!
        void* ph = VirtualAllocFromApp(nullptr, 2 * kPage64KB, MEM_RESERVE | MEM_GRAPHICS, PAGE_NOACCESS);
        SX_REQUIRE(ph != nullptr);
        void* mapped_ph = MapTitlePhysicalPages(ph, 2, MEM_GRAPHICS, PAGE_READWRITE, pages.data());
        SX_REQUIRE(mapped_ph == ph);
        SX_REQUIRE(static_cast<uint8_t*>(mapped_ph)[42] == 0x77u);
        SX_REQUIRE(UnmapTitlePhysicalPages(mapped_ph, 2) != 0);
        SX_REQUIRE(KernelXVirtualFree(ph, 0, MEM_RELEASE) != 0);
    }

    // Unmap VA2 and free physical pages cleanly.
    SX_REQUIRE(UnmapTitlePhysicalPages(va2, num_pages) != 0);
    SX_REQUIRE(FreeTitlePhysicalPages(nullptr, num_pages, pages.data()) != 0);

    // 4 MiB MEM_LARGE_PAGES dual-VA aliasing verification
    uint64_t large_count = 1;
    uint64_t large_pfn = 0;
    SX_REQUIRE(AllocateTitlePhysicalPages(nullptr, MEM_GRAPHICS | MEM_LARGE_PAGES,
                                          &large_count, &large_pfn) != 0);
    void* lva1 = MapTitlePhysicalPages(nullptr, 1, MEM_GRAPHICS | MEM_LARGE_PAGES,
                                       PAGE_READWRITE, &large_pfn);
    void* lva2 = MapTitlePhysicalPages(nullptr, 1, MEM_GRAPHICS | MEM_LARGE_PAGES,
                                       PAGE_READWRITE, &large_pfn);
    SX_REQUIRE(lva1 != nullptr && lva2 != nullptr && lva1 != lva2);
    static_cast<uint8_t*>(lva1)[0] = 0x12u;
    static_cast<uint8_t*>(lva1)[kPage4MB - 1] = 0x34u;
    SX_REQUIRE(static_cast<const uint8_t*>(lva2)[0] == 0x12u);
    SX_REQUIRE(static_cast<const uint8_t*>(lva2)[kPage4MB - 1] == 0x34u);
    uint64_t lsize = 0;
    uint32_t lflags = 0;
    SX_REQUIRE(QueryTitlePhysicalPages(lva1, nullptr, &lsize, &lflags) != 0);
    SX_REQUIRE(lsize == kPage4MB);
    SX_REQUIRE((lflags & MEM_LARGE_PAGES) != 0u);
    SX_REQUIRE(UnmapTitlePhysicalPages(lva2, 1) != 0);
    SX_REQUIRE(UnmapTitlePhysicalPages(lva1, 1) != 0);
    SX_REQUIRE(FreeTitlePhysicalPages(nullptr, 1, &large_pfn) != 0);
}

void TestVirtualAllocFromApp() {
    TITLEMEMORYSTATUS before{};
    before.dwLength = sizeof(TITLEMEMORYSTATUS);
    SX_REQUIRE(TitleMemoryStatus(&before) != 0);

    void* mem = VirtualAllocFromApp(nullptr, 128 * 1024, MEM_RESERVE | MEM_COMMIT | MEM_GRAPHICS, PAGE_READWRITE);
    SX_REQUIRE(mem != nullptr);
    std::memset(mem, 0x3Cu, 128 * 1024);
    SX_REQUIRE(static_cast<uint8_t*>(mem)[0] == 0x3Cu);
    SX_REQUIRE(static_cast<uint8_t*>(mem)[128 * 1024 - 1] == 0x3Cu);

    TITLEMEMORYSTATUS during{};
    during.dwLength = sizeof(TITLEMEMORYSTATUS);
    SX_REQUIRE(TitleMemoryStatus(&during) != 0);
    SX_REQUIRE(during.ullTitleUsed >= before.ullTitleUsed + 128 * 1024);

    uint32_t old_prot = 0;
    SX_REQUIRE(VirtualProtectFromApp(mem, 64 * 1024, PAGE_READONLY, &old_prot) != 0);
    SX_REQUIRE(old_prot == PAGE_READWRITE);
    SX_REQUIRE(static_cast<uint8_t*>(mem)[0] == 0x3Cu);

    SX_REQUIRE(VirtualProtectFromApp(mem, 64 * 1024, PAGE_READWRITE, &old_prot) != 0);
    SX_REQUIRE(old_prot == PAGE_READONLY);

    SX_REQUIRE(KernelXVirtualFree(mem, 0, MEM_RELEASE) != 0);

    TITLEMEMORYSTATUS after{};
    after.dwLength = sizeof(TITLEMEMORYSTATUS);
    SX_REQUIRE(TitleMemoryStatus(&after) != 0);
    SX_REQUIRE(after.ullTitleUsed == before.ullTitleUsed);

    // Invalid args
    SX_REQUIRE(VirtualAllocFromApp(nullptr, 0, MEM_COMMIT, PAGE_READWRITE) == nullptr);
    SX_REQUIRE(VirtualProtectFromApp(nullptr, 4096, PAGE_READONLY, &old_prot) == 0);
}

void TestGpuTilePoolCppClass() {
    scarlettx::UnifiedMemoryManager mem;
    scarlettx::GpuTilePool pool_mgr(mem);

    // 1. Error handling on invalid inputs
    uint64_t bad_id = 0;
    SX_REQUIRE(!pool_mgr.CreatePool(0, scarlettx::kTilePoolFlagGraphics, bad_id).ok());
    SX_REQUIRE(!pool_mgr.ReserveTiledResource(0, 4, bad_id).ok());
    SX_REQUIRE(!pool_mgr.ReserveTiledResource(4, 0, bad_id).ok());

    // 2. Create a 4-tile (256 KiB) physical tile pool
    uint64_t pool_id = 0;
    SX_REQUIRE(pool_mgr.CreatePool(4, scarlettx::kTilePoolFlagGraphics, pool_id).ok());
    SX_REQUIRE(pool_id != 0);
    SX_REQUIRE(pool_mgr.TotalPhysicalTiles() == 4);
    SX_REQUIRE(pool_mgr.MappedVirtualTiles() == 0);
    SX_REQUIRE(pool_mgr.AliasedTileCount() == 0);
    SX_REQUIRE(mem.TotalAllocatedBytes() == 4 * kPage64KB);

    // 3. Reserve a 4x4 (16-tile, 1 MiB) sparse tiled resource
    uint64_t res_id = 0;
    SX_REQUIRE(pool_mgr.ReserveTiledResource(4, 4, res_id).ok());
    SX_REQUIRE(res_id != 0);

    uint32_t w = 0, h = 0;
    void* vbase = nullptr;
    SX_REQUIRE(pool_mgr.GetResourceDimensions(res_id, w, h, vbase));
    SX_REQUIRE(w == 4 && h == 4);
    if (scarlettx::UnifiedMemoryManager::SupportsOsPlaceholders()) {
        SX_REQUIRE(vbase != nullptr);
    }

    // Reading/writing an unmapped NULL tile must fail cleanly (no AV!)
    uint8_t dummy[16] = {};
    SX_REQUIRE(!pool_mgr.ReadTileBytes(res_id, {1, 1, 0}, 0, dummy, sizeof(dummy)).ok());
    SX_REQUIRE(!pool_mgr.WriteTileBytes(res_id, {1, 1, 0}, 0, dummy, sizeof(dummy)).ok());

    // 4. Sparse mapping: map middle tile (1, 1) -> physical tile 0, and (3, 2) -> physical tile 2
    SX_REQUIRE(pool_mgr.UpdateTileMappings(res_id, {1, 1, 0}, pool_id, 0, 1).ok());
    SX_REQUIRE(pool_mgr.UpdateTileMappings(res_id, {3, 2, 0}, pool_id, 2, 1).ok());
    SX_REQUIRE(pool_mgr.MappedVirtualTiles() == 2);
    SX_REQUIRE(pool_mgr.AliasedTileCount() == 0);

    const std::array<uint8_t, 8> pattern_a = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    SX_REQUIRE(pool_mgr.WriteTileBytes(res_id, {1, 1, 0}, 1024, pattern_a.data(), pattern_a.size()).ok());

    std::array<uint8_t, 8> readback{};
    SX_REQUIRE(pool_mgr.ReadTileBytes(res_id, {1, 1, 0}, 1024, readback.data(), readback.size()).ok());
    SX_REQUIRE(readback == pattern_a);

    // 5. Dual-tile aliasing: map virtual tile (2, 0) to the SAME physical tile 0!
    SX_REQUIRE(pool_mgr.UpdateTileMappings(res_id, {2, 0, 0}, pool_id, 0, 1).ok());
    SX_REQUIRE(pool_mgr.MappedVirtualTiles() == 3);
    SX_REQUIRE(pool_mgr.AliasedTileCount() == 1);

    // Verify reading through (2, 0) immediately sees pattern_a written through (1, 1)!
    readback.fill(0);
    SX_REQUIRE(pool_mgr.ReadTileBytes(res_id, {2, 0, 0}, 1024, readback.data(), readback.size()).ok());
    SX_REQUIRE(readback == pattern_a);

    // Modify via (2, 0), verify via (1, 1) and direct virtual pointer inspection!
    const uint32_t magic = 0xDEADBEEFu;
    SX_REQUIRE(pool_mgr.WriteTileBytes(res_id, {2, 0, 0}, 4096, &magic, sizeof(magic)).ok());
    uint32_t observed = 0;
    SX_REQUIRE(pool_mgr.ReadTileBytes(res_id, {1, 1, 0}, 4096, &observed, sizeof(observed)).ok());
    SX_REQUIRE(observed == magic);

    scarlettx::GpuTileMappingInfo info_a{}, info_b{};
    SX_REQUIRE(pool_mgr.QueryTileMapping(res_id, {1, 1, 0}, info_a).ok());
    SX_REQUIRE(pool_mgr.QueryTileMapping(res_id, {2, 0, 0}, info_b).ok());
    SX_REQUIRE(info_a.mapped && info_b.mapped);
    SX_REQUIRE(info_a.virtual_address != info_b.virtual_address);
    SX_REQUIRE(info_a.pool_id == pool_id && info_b.pool_id == pool_id);
    SX_REQUIRE(info_a.pool_tile_offset == 0 && info_b.pool_tile_offset == 0);
    if (scarlettx::UnifiedMemoryManager::SupportsOsPlaceholders()) {
        // Verify exact virtual tile offset inside contiguous placeholder!
        SX_REQUIRE(info_a.virtual_address == static_cast<uint8_t*>(vbase) + (1 * 4 + 1) * kPage64KB);
        SX_REQUIRE(info_b.virtual_address == static_cast<uint8_t*>(vbase) + (0 * 4 + 2) * kPage64KB);
    }

    // 6. Reuse-single-tile flag: alias 3 consecutive tiles (0, 3), (1, 3), (2, 3) to physical tile 3
    SX_REQUIRE(pool_mgr.UpdateTileMappings(res_id, {0, 3, 0}, pool_id, 3, 3,
                                           scarlettx::kTileMapFlagReuseSingleTile)
                   .ok());
    SX_REQUIRE(pool_mgr.MappedVirtualTiles() == 6);
    SX_REQUIRE(pool_mgr.AliasedTileCount() == 3);  // 1 alias on phys 0 + 2 aliases on phys 3

    const uint64_t fill_val = 0x0123456789ABCDEFull;
    SX_REQUIRE(pool_mgr.WriteTileBytes(res_id, {0, 3, 0}, 0, &fill_val, sizeof(fill_val)).ok());
    uint64_t check1 = 0, check2 = 0;
    SX_REQUIRE(pool_mgr.ReadTileBytes(res_id, {1, 3, 0}, 0, &check1, sizeof(check1)).ok());
    SX_REQUIRE(pool_mgr.ReadTileBytes(res_id, {2, 3, 0}, 0, &check2, sizeof(check2)).ok());
    SX_REQUIRE(check1 == fill_val && check2 == fill_val);

    // 7. Remap (1, 1) to physical tile 1 (should cleanly unmap old alias and reduce aliased count)
    SX_REQUIRE(pool_mgr.UpdateTileMappings(res_id, {1, 1, 0}, pool_id, 1, 1).ok());
    SX_REQUIRE(pool_mgr.MappedVirtualTiles() == 6);
    SX_REQUIRE(pool_mgr.AliasedTileCount() == 2);  // phys 0 now uniquely mapped by (2, 0)

    // Physical tile 0 data is still preserved via (2, 0), while (1, 1) now sees fresh zeroed tile 1
    uint32_t tile1_val = 0xFFFFFFFFu;
    SX_REQUIRE(pool_mgr.ReadTileBytes(res_id, {1, 1, 0}, 4096, &tile1_val, sizeof(tile1_val)).ok());
    SX_REQUIRE(tile1_val == 0u);
    uint32_t tile0_val = 0;
    SX_REQUIRE(pool_mgr.ReadTileBytes(res_id, {2, 0, 0}, 4096, &tile0_val, sizeof(tile0_val)).ok());
    SX_REQUIRE(tile0_val == magic);

    // 8. Unmapping to NULL tile
    SX_REQUIRE(pool_mgr.UpdateTileMappings(res_id, {0, 3, 0}, 0, 0, 3, scarlettx::kTileMapFlagNull).ok());
    SX_REQUIRE(pool_mgr.MappedVirtualTiles() == 3);
    SX_REQUIRE(pool_mgr.AliasedTileCount() == 0);
    SX_REQUIRE(!pool_mgr.ReadTileBytes(res_id, {0, 3, 0}, 0, &check1, sizeof(check1)).ok());

    // 9. Cross-resource aliasing & read-only tile protection check
    uint64_t res2_id = 0;
    SX_REQUIRE(pool_mgr.ReserveTiledResource(2, 2, res2_id).ok());
    // Map tile (0, 0) in res2 as READONLY aliasing physical tile 0 (already mapped in res1 at (2, 0))
    SX_REQUIRE(pool_mgr.UpdateTileMappings(res2_id, {0, 0, 0}, pool_id, 0, 1,
                                           scarlettx::kTileMapFlagReadOnly)
                   .ok());
    SX_REQUIRE(pool_mgr.AliasedTileCount() == 1);
    uint32_t cross_val = 0;
    SX_REQUIRE(pool_mgr.ReadTileBytes(res2_id, {0, 0, 0}, 4096, &cross_val, sizeof(cross_val)).ok());
    SX_REQUIRE(cross_val == magic);
    // Writing to a read-only tile must fail safely
    SX_REQUIRE(!pool_mgr.WriteTileBytes(res2_id, {0, 0, 0}, 4096, &magic, sizeof(magic)).ok());
    SX_REQUIRE(pool_mgr.ReleaseTiledResource(res2_id).ok());
    SX_REQUIRE(pool_mgr.AliasedTileCount() == 0);

    // 10. Tile-bounds & pool-bounds error handling
    SX_REQUIRE(!pool_mgr.UpdateTileMappings(res_id, {4, 0, 0}, pool_id, 0, 1).ok());    // x out of bounds
    SX_REQUIRE(!pool_mgr.UpdateTileMappings(res_id, {0, 4, 0}, pool_id, 0, 1).ok());    // y out of bounds
    SX_REQUIRE(!pool_mgr.UpdateTileMappings(res_id, {3, 3, 0}, pool_id, 0, 2).ok());    // range past resource end
    SX_REQUIRE(!pool_mgr.UpdateTileMappings(res_id, {0, 0, 0}, pool_id, 3, 2).ok());    // range past pool end
    SX_REQUIRE(!pool_mgr.UpdateTileMappings(res_id, {0, 0, 0}, 9999, 0, 1).ok());       // unknown pool
    SX_REQUIRE(!pool_mgr.UpdateTileMappings(9999, {0, 0, 0}, pool_id, 0, 1).ok());      // unknown resource
    SX_REQUIRE(!pool_mgr.WriteTileBytes(res_id, {2, 0, 0}, kPage64KB, &magic, 4).ok()); // byte range out of tile

    // Cannot destroy pool while virtual tiles are still mapped to it
    SX_REQUIRE(!pool_mgr.DestroyPool(pool_id).ok());

    // 11. Clean teardown: releasing resource unmaps all remaining tiles, then pool destroys cleanly
    SX_REQUIRE(pool_mgr.ReleaseTiledResource(res_id).ok());
    SX_REQUIRE(pool_mgr.MappedVirtualTiles() == 0);
    SX_REQUIRE(pool_mgr.AliasedTileCount() == 0);
    SX_REQUIRE(pool_mgr.DestroyPool(pool_id).ok());
    SX_REQUIRE(pool_mgr.TotalPhysicalTiles() == 0);
    SX_REQUIRE(mem.TotalAllocatedBytes() == 0);

    // 12. 4 MiB large-page backed tile pool stats check
    uint64_t lp_pool_id = 0;
    SX_REQUIRE(pool_mgr.CreatePool(8, scarlettx::kTilePoolFlagGraphics | scarlettx::kTilePoolFlagLargePages,
                                   lp_pool_id)
                   .ok());
    const auto stats = pool_mgr.GetStats();
    SX_REQUIRE(stats.total_pools == 1);
    SX_REQUIRE(stats.total_physical_tiles == 8);
    SX_REQUIRE(stats.physical_bytes == kPage4MB);
    SX_REQUIRE(pool_mgr.DestroyPool(lp_pool_id).ok());
    SX_REQUIRE(mem.TotalAllocatedBytes() == 0);
}

void TestKernelXTilePoolCAbi() {
    TITLEMEMORYSTATUS before{};
    before.dwLength = sizeof(TITLEMEMORYSTATUS);
    SX_REQUIRE(TitleMemoryStatus(&before) != 0);

    uint64_t pool_id = 0;
    SX_REQUIRE(KernelXCreateTilePool(2, KERNELX_TILE_POOL_GRAPHICS, &pool_id) != 0);
    SX_REQUIRE(pool_id != 0);

    // Tile pool physical backing must be visible in live TitleMemoryStatus!
    TITLEMEMORYSTATUS during{};
    during.dwLength = sizeof(TITLEMEMORYSTATUS);
    SX_REQUIRE(TitleMemoryStatus(&during) != 0);
    SX_REQUIRE(during.ullTitleUsed == before.ullTitleUsed + 2 * kPage64KB);

    uint64_t res_id = 0;
    void* vbase = nullptr;
    SX_REQUIRE(KernelXReserveTiledResource(2, 1, &res_id, &vbase) != 0);
    SX_REQUIRE(res_id != 0);

    // Dual-map both virtual tiles (0,0) and (1,0) to physical tile 0 via C ABI!
    SX_REQUIRE(KernelXMapTileRange(res_id, 0, 0, pool_id, 0, 2, KERNELX_TILE_MAP_REUSE_SINGLE_TILE) != 0);
    if (vbase != nullptr) {
        auto* t0 = static_cast<uint8_t*>(vbase);
        auto* t1 = t0 + kPage64KB;
        t0[0] = 0x5Au;
        t0[65535] = 0xA5u;
        SX_REQUIRE(t1[0] == 0x5Au);
        SX_REQUIRE(t1[65535] == 0xA5u);

        // QueryTitlePhysicalPages should also resolve GPU tile pool aliases!
        uint64_t phys_id = 0, sz = 0;
        uint32_t flags = 0;
        SX_REQUIRE(QueryTitlePhysicalPages(t0, &phys_id, &sz, &flags) != 0);
        SX_REQUIRE(phys_id != 0 && sz == kPage64KB);
        SX_REQUIRE((flags & MEM_GRAPHICS) != 0u);
    }

    // Cannot destroy pool while mapped
    SX_REQUIRE(KernelXDestroyTilePool(pool_id) == 0);

    // Unmap, release resource, destroy pool
    SX_REQUIRE(KernelXMapTileRange(res_id, 0, 0, 0, 0, 2, KERNELX_TILE_MAP_NULL) != 0);
    SX_REQUIRE(KernelXReleaseTiledResource(res_id) != 0);
    SX_REQUIRE(KernelXDestroyTilePool(pool_id) != 0);

    TITLEMEMORYSTATUS after{};
    after.dwLength = sizeof(TITLEMEMORYSTATUS);
    SX_REQUIRE(TitleMemoryStatus(&after) != 0);
    SX_REQUIRE(after.ullTitleUsed == before.ullTitleUsed);
}

}  // namespace

int main() {
    TestSystemQueriesAndMemoryStatus();
    TestDualVaAliasingAndProtection();
    TestVirtualAllocFromApp();
    TestGpuTilePoolCppClass();
    TestKernelXTilePoolCAbi();

    std::cout << "[test_kernelx] PASSED\n";
    return 0;
}
