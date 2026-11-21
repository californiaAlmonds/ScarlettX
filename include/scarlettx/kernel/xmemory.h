#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "scarlettx/core/status.h"
#include "scarlettx/core/types.h"

namespace scarlettx {

enum class MemoryPageSize : uint64_t {
    Page4KB = 4096,
    Page64KB = 65536,
    Page2MB = 2ull * 1024ull * 1024ull,
    Page4MB = 4ull * 1024ull * 1024ull,
};

enum class MemoryProtection : uint32_t {
    NoAccess = 0,
    ReadOnly = 1,
    ReadWrite = 2,
    WriteCombined = 3,
};

enum UnifiedMemoryFlags : uint32_t {
    kMemFlagNone = 0u,
    kMemFlagGraphics = 1u << 0,       // Scarlett MEM_GRAPHICS (GPU-visible unified memory)
    kMemFlagLargePages = 1u << 1,     // 2 MiB / 4 MiB page backing preference
    kMemFlagWriteCombined = 1u << 2,  // CPU write-combining cache attribute
};

struct UnifiedMemoryBlock {
    void* host_ptr = nullptr;
    GpuVirtualAddress gpu_va = 0;
    size_t size = 0;
    MemoryPageSize page_size = MemoryPageSize::Page64KB;
    bool gpu_visible = true;
    bool write_combined = false;
};

struct PhysicalAllocation {
    uint64_t physical_id = 0;
    size_t size = 0;
    MemoryPageSize page_size = MemoryPageSize::Page64KB;
    uint32_t flags = kMemFlagNone;
};

struct UnifiedMemoryAlias {
    void* host_ptr = nullptr;
    GpuVirtualAddress gpu_va = 0;
    uint64_t physical_id = 0;
    size_t physical_offset = 0;
    size_t size = 0;
    MemoryProtection protection = MemoryProtection::ReadWrite;
    uint32_t flags = kMemFlagNone;
    bool from_placeholder = false;
};

// Emulates Scarlett's unified CPU/GPU virtual address space (64KB/2MB/4MB pools)
// including OS-backed physical-to-virtual page-table aliasing (MEM_GRAPHICS).
class UnifiedMemoryManager {
public:
    UnifiedMemoryManager();
    ~UnifiedMemoryManager();

    UnifiedMemoryManager(const UnifiedMemoryManager&) = delete;
    UnifiedMemoryManager& operator=(const UnifiedMemoryManager&) = delete;

    Status Allocate(size_t size,
                    MemoryPageSize page_size,
                    bool gpu_visible,
                    bool write_combined,
                    UnifiedMemoryBlock& out_block);

    Status Free(GpuVirtualAddress gpu_va);
    bool LookupByGpuVa(GpuVirtualAddress gpu_va, UnifiedMemoryBlock& out_block) const;
    size_t TotalAllocatedBytes() const;
    size_t ActiveAllocationCount() const;

    // Physical-to-virtual aliasing & Scarlett MEM_GRAPHICS placeholder support.
    static bool SupportsOsPlaceholders();

    Status AllocatePhysicalPages(size_t size,
                                 MemoryPageSize page_size,
                                 uint32_t flags,
                                 PhysicalAllocation& out_phys);
    Status FreePhysicalPages(uint64_t physical_id);

    Status ReserveVirtualPlaceholder(size_t size, void*& out_base);
    Status ReleaseVirtualPlaceholder(void* base);

    Status MapPhysicalAlias(uint64_t physical_id,
                            size_t physical_offset,
                            size_t size,
                            MemoryProtection protection,
                            uint32_t flags,
                            void* placeholder_addr,
                            UnifiedMemoryAlias& out_alias);
    Status UnmapPhysicalAlias(void* host_ptr);
    Status ProtectAlias(void* host_ptr, size_t size, MemoryProtection new_protection);
    bool LookupAliasByHostPtr(const void* host_ptr, UnifiedMemoryAlias& out_alias) const;
    size_t ActivePhysicalPoolCount() const;
    size_t ActiveAliasCount() const;

private:
    struct PhysicalPoolEntry {
        PhysicalAllocation info{};
        void* os_handle = nullptr;  // Win32 HANDLE from CreateFileMappingW
        std::vector<uint8_t> fallback_storage;
    };

    struct PlaceholderSpan {
        uintptr_t base = 0;
        size_t size = 0;
        bool mapped = false;
    };

    struct PlaceholderReservation {
        uintptr_t base = 0;
        size_t total_size = 0;
        std::vector<PlaceholderSpan> spans;
    };

    mutable std::mutex mutex_;
    GpuVirtualAddress next_gpu_va_ = 0x0000010000000000ull;
    uint64_t next_physical_id_ = 1;
    size_t total_allocated_bytes_ = 0;
    std::unordered_map<GpuVirtualAddress, UnifiedMemoryBlock> blocks_;
    std::unordered_map<uint64_t, PhysicalPoolEntry> physical_pools_;
    std::unordered_map<uintptr_t, UnifiedMemoryAlias> aliases_;
    std::unordered_map<uintptr_t, PlaceholderReservation> placeholders_;
};

}  // namespace scarlettx
