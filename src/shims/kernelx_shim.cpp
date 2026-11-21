#define SCARLETTX_BUILDING_KERNELX 1

// Guard against Windows SDK <memoryapi.h> declaring dllimport VirtualAllocFromApp /
// VirtualProtectFromApp with ULONG while kernelx.dll exports them itself.
#if defined(_WIN32)
#  define VirtualAllocFromApp ScarlettX_Win32_VirtualAllocFromApp
#  define VirtualProtectFromApp ScarlettX_Win32_VirtualProtectFromApp
#  include <windows.h>
#  undef VirtualAllocFromApp
#  undef VirtualProtectFromApp
#endif

#include "scarlettx/kernel/kernelx_abi.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "scarlettx/kernel/gpu_tile_pool.h"
#include "scarlettx/kernel/xmemory.h"
#include "scarlettx/kernel/xruntime.h"

namespace {

using scarlettx::DeviceType;
using scarlettx::GpuTileCoord;
using scarlettx::GpuTilePool;
using scarlettx::MemoryPageSize;
using scarlettx::MemoryProtection;
using scarlettx::PhysicalAllocation;
using scarlettx::Status;
using scarlettx::UnifiedMemoryAlias;
using scarlettx::UnifiedMemoryManager;
using scarlettx::XGameRuntimeHle;

// Xbox Series X (Anaconda): 16 GiB GDDR6 total -> 13.5 GiB title + 2.5 GiB tooling/OS.
constexpr uint64_t kAnacondaTitleBudgetBytes = 27ull * 512ull * 1024ull * 1024ull;    // 13.5 GiB
constexpr uint64_t kAnacondaToolingBudgetBytes = 5ull * 512ull * 1024ull * 1024ull;   // 2.5 GiB

// Xbox Series S (Lockhart): 10 GiB GDDR6 total -> 8.0 GiB title + 2.0 GiB tooling/OS.
constexpr uint64_t kLockhartTitleBudgetBytes = 8ull * 1024ull * 1024ull * 1024ull;    // 8.0 GiB
constexpr uint64_t kLockhartToolingBudgetBytes = 2ull * 1024ull * 1024ull * 1024ull;  // 2.0 GiB

constexpr size_t kPage64KB = 65536u;
constexpr size_t kPage4MB = 4ull * 1024ull * 1024ull;

size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

void SetHostLastError(uint32_t err) {
#if defined(_WIN32)
    SetLastError(static_cast<DWORD>(err));
#else
    (void)err;
#endif
}

bool ValidateProtect(uint32_t flProtect) {
    const uint32_t base = flProtect & 0xFFu;
    switch (base) {
        case PAGE_NOACCESS:
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
            return true;
        default:
            return false;
    }
}

MemoryProtection ToMemoryProtection(uint32_t flProtect) {
    const uint32_t base = flProtect & 0xFFu;
    if (base == PAGE_NOACCESS) {
        return MemoryProtection::NoAccess;
    }
    if (base == PAGE_READONLY || base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ) {
        return MemoryProtection::ReadOnly;
    }
    if ((flProtect & PAGE_WRITECOMBINE) != 0u) {
        return MemoryProtection::WriteCombined;
    }
    return MemoryProtection::ReadWrite;
}

#if defined(_WIN32)
DWORD ToWin32PageProtect(uint32_t flProtect) {
    const uint32_t base = flProtect & 0xFFu;
    DWORD result = PAGE_READWRITE;
    switch (base) {
        case PAGE_NOACCESS: result = PAGE_NOACCESS; break;
        case PAGE_READONLY: result = PAGE_READONLY; break;
        case PAGE_READWRITE: result = PAGE_READWRITE; break;
        case PAGE_WRITECOPY: result = PAGE_WRITECOPY; break;
        case PAGE_EXECUTE: result = PAGE_EXECUTE; break;
        case PAGE_EXECUTE_READ: result = PAGE_EXECUTE_READ; break;
        case PAGE_EXECUTE_READWRITE: result = PAGE_EXECUTE_READWRITE; break;
        default: result = PAGE_READWRITE; break;
    }
    if (flProtect & PAGE_GUARD) {
        result |= PAGE_GUARD;
    }
    return result;
}
#endif

uint32_t ToUnifiedFlags(uint32_t flAllocationType, uint32_t flProtect) {
    uint32_t flags = scarlettx::kMemFlagNone;
    if (flAllocationType & MEM_GRAPHICS) {
        flags |= scarlettx::kMemFlagGraphics;
    }
    if (flAllocationType & (MEM_LARGE_PAGES | MEM_4MB_PAGES)) {
        flags |= scarlettx::kMemFlagLargePages;
    }
    if (flProtect & PAGE_WRITECOMBINE) {
        flags |= scarlettx::kMemFlagWriteCombined;
    }
    return flags;
}

uint32_t ResolveConsoleType() {
    if (const char* env = std::getenv("SCARLETTX_CONSOLE_TYPE"); env && env[0] != '\0') {
        std::string v(env);
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (v == "lockhart" || v == "3" || v == "series_s") {
            return CONSOLE_TYPE_XBOX_SCARLETT_LOCKHART;
        }
        if (v == "anaconda" || v == "4" || v == "series_x") {
            return CONSOLE_TYPE_XBOX_SCARLETT_ANACONDA;
        }
        if (v == "devkit" || v == "5") {
            return CONSOLE_TYPE_XBOX_SCARLETT_DEVKIT;
        }
    }

    switch (XGameRuntimeHle::Instance().GetDeviceType()) {
        case DeviceType::XboxScarlettLockhart:
            return CONSOLE_TYPE_XBOX_SCARLETT_LOCKHART;
        case DeviceType::XboxScarlettDevkit:
            return CONSOLE_TYPE_XBOX_SCARLETT_DEVKIT;
        case DeviceType::XboxOneX:
        case DeviceType::XboxOneXDevkit:
            return CONSOLE_TYPE_XBOX_ONE_X;
        case DeviceType::XboxOne:
        case DeviceType::XboxOneS:
            return CONSOLE_TYPE_XBOX_ONE;
        case DeviceType::XboxScarlettAnaconda:
        default:
            return CONSOLE_TYPE_XBOX_SCARLETT_ANACONDA;
    }
}

uint64_t GetTitleBudgetBytes() {
    if (ResolveConsoleType() == CONSOLE_TYPE_XBOX_SCARLETT_LOCKHART) {
        return kLockhartTitleBudgetBytes;
    }
    return kAnacondaTitleBudgetBytes;
}

uint64_t GetToolingBudgetBytes() {
    if (ResolveConsoleType() == CONSOLE_TYPE_XBOX_SCARLETT_LOCKHART) {
        return kLockhartToolingBudgetBytes;
    }
    return kAnacondaToolingBudgetBytes;
}

struct PhysicalPoolRecord {
    uint64_t physical_id = 0;
    size_t page_size_bytes = kPage64KB;
    uint32_t alloc_flags = 0;
    uint64_t live_pages = 0;
};

struct PageFrameRecord {
    uint64_t pfn = 0;
    uint64_t physical_id = 0;
    uint64_t page_index = 0;
    size_t page_size_bytes = kPage64KB;
    uint32_t alloc_flags = 0;
    uint32_t map_refcount = 0;
};

struct ActiveTitleMapping {
    uintptr_t base_va = 0;
    size_t size_bytes = 0;
    size_t page_size_bytes = kPage64KB;
    uint64_t primary_physical_id = 0;
    uint32_t alloc_flags = 0;
    uint32_t win32_protect = PAGE_READWRITE;
    bool owns_placeholder = false;
    std::vector<uint64_t> pfns;
    std::vector<void*> alias_ptrs;
};

struct AppVirtualAllocRecord {
    uintptr_t base = 0;
    size_t size = 0;
    size_t committed_bytes = 0;
    uint32_t alloc_flags = 0;
    uint32_t protection = PAGE_NOACCESS;
    bool is_placeholder = false;
};

class KernelXState {
public:
    static KernelXState& Instance() {
        static KernelXState instance;
        return instance;
    }

    UnifiedMemoryManager& Memory() { return memory_; }
    GpuTilePool& TilePool() { return tile_pool_; }

    uint64_t CurrentTitleUsedBytes() const {
        const uint64_t unified = static_cast<uint64_t>(memory_.TotalAllocatedBytes());
        const uint64_t app = app_committed_bytes_.load(std::memory_order_acquire);
        return unified + app;
    }

    uint64_t UpdateAndGetPeakTitleUsed() {
        const uint64_t current = CurrentTitleUsedBytes();
        uint64_t prev = peak_title_used_.load(std::memory_order_relaxed);
        while (current > prev &&
               !peak_title_used_.compare_exchange_weak(prev, current, std::memory_order_acq_rel)) {
        }
        return std::max(prev, current);
    }

    int32_t AllocatePhysicalPages(uint32_t flAllocationType,
                                  uint64_t* pNumberOfPages,
                                  uint64_t* pPageArray) {
        if (!pNumberOfPages || !pPageArray || *pNumberOfPages == 0) {
            SetHostLastError(87u);  // ERROR_INVALID_PARAMETER
            return 0;
        }

        const bool large_pages = (flAllocationType & (MEM_LARGE_PAGES | MEM_4MB_PAGES)) != 0u;
        const size_t page_bytes = large_pages ? kPage4MB : kPage64KB;
        const MemoryPageSize page_enum = large_pages ? MemoryPageSize::Page4MB : MemoryPageSize::Page64KB;
        const uint64_t req_pages = *pNumberOfPages;

        if (req_pages > (GetTitleBudgetBytes() / page_bytes)) {
            *pNumberOfPages = 0;
            SetHostLastError(8u);  // ERROR_NOT_ENOUGH_MEMORY
            return 0;
        }

        const uint64_t total_bytes_64 = req_pages * static_cast<uint64_t>(page_bytes);
        const uint64_t budget = GetTitleBudgetBytes();
        if (CurrentTitleUsedBytes() + total_bytes_64 > budget) {
            *pNumberOfPages = 0;
            SetHostLastError(8u);  // ERROR_NOT_ENOUGH_MEMORY
            return 0;
        }

        const uint32_t mem_flags = ToUnifiedFlags(flAllocationType, 0u);
        PhysicalAllocation phys{};
        Status st = memory_.AllocatePhysicalPages(static_cast<size_t>(total_bytes_64),
                                                  page_enum,
                                                  mem_flags,
                                                  phys);
        if (!st.ok()) {
            *pNumberOfPages = 0;
            SetHostLastError(8u);
            return 0;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            PhysicalPoolRecord pool{};
            pool.physical_id = phys.physical_id;
            pool.page_size_bytes = page_bytes;
            pool.alloc_flags = flAllocationType;
            pool.live_pages = req_pages;
            pools_.emplace(phys.physical_id, pool);

            for (uint64_t i = 0; i < req_pages; ++i) {
                const uint64_t pfn = next_pfn_++;
                PageFrameRecord rec{};
                rec.pfn = pfn;
                rec.physical_id = phys.physical_id;
                rec.page_index = i;
                rec.page_size_bytes = page_bytes;
                rec.alloc_flags = flAllocationType;
                rec.map_refcount = 0;
                pfn_table_.emplace(pfn, rec);
                pPageArray[i] = pfn;
            }
        }

        UpdateAndGetPeakTitleUsed();
        return 1;
    }

    int32_t FreePhysicalPages(uint64_t numberOfPages, const uint64_t* pPageArray) {
        if (numberOfPages == 0 || !pPageArray) {
            SetHostLastError(87u);
            return 0;
        }

        std::vector<uint64_t> pools_to_free;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Validate every PFN before mutating any state (also reject duplicates in input array).
            std::unordered_map<uint64_t, uint64_t> seen;
            seen.reserve(static_cast<size_t>(numberOfPages));
            for (uint64_t i = 0; i < numberOfPages; ++i) {
                const uint64_t pfn = pPageArray[i];
                if (seen.find(pfn) != seen.end()) {
                    SetHostLastError(87u);
                    return 0;
                }
                auto it = pfn_table_.find(pfn);
                if (it == pfn_table_.end() || it->second.map_refcount > 0) {
                    SetHostLastError(87u);
                    return 0;
                }
                seen.emplace(pfn, it->second.physical_id);
            }

            for (uint64_t i = 0; i < numberOfPages; ++i) {
                const uint64_t pfn = pPageArray[i];
                auto it = pfn_table_.find(pfn);
                const uint64_t phys_id = it->second.physical_id;
                pfn_table_.erase(it);

                auto pit = pools_.find(phys_id);
                if (pit != pools_.end()) {
                    if (pit->second.live_pages > 0) {
                        --pit->second.live_pages;
                    }
                    if (pit->second.live_pages == 0) {
                        pools_to_free.push_back(phys_id);
                        pools_.erase(pit);
                    }
                }
            }
        }

        for (uint64_t phys_id : pools_to_free) {
            Status st = memory_.FreePhysicalPages(phys_id);
            if (!st.ok()) {
                SetHostLastError(87u);
                return 0;
            }
        }
        return 1;
    }

    void* MapPhysicalPages(void* virtualAddress,
                           uint64_t numberOfPages,
                           uint32_t flAllocationType,
                           uint32_t flProtect,
                           const uint64_t* pPageArray) {
        if (numberOfPages == 0 || !pPageArray || !ValidateProtect(flProtect)) {
            SetHostLastError(87u);
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PageFrameRecord> frames;
        frames.reserve(static_cast<size_t>(numberOfPages));
        size_t page_bytes = 0;
        bool is_contiguous_single_pool = true;

        for (uint64_t i = 0; i < numberOfPages; ++i) {
            auto it = pfn_table_.find(pPageArray[i]);
            if (it == pfn_table_.end()) {
                SetHostLastError(87u);
                return nullptr;
            }
            if (i == 0) {
                page_bytes = it->second.page_size_bytes;
            } else {
                if (it->second.page_size_bytes != page_bytes) {
                    SetHostLastError(87u);
                    return nullptr;
                }
                if (it->second.physical_id != frames[0].physical_id ||
                    it->second.page_index != frames[0].page_index + i) {
                    is_contiguous_single_pool = false;
                }
            }
            frames.push_back(it->second);
        }

        const size_t total_bytes = static_cast<size_t>(numberOfPages) * page_bytes;
        const MemoryProtection prot = ToMemoryProtection(flProtect);
        const uint32_t mem_flags = ToUnifiedFlags(flAllocationType | frames[0].alloc_flags, flProtect);

        void* base_va = nullptr;
        bool owns_placeholder = false;
        std::vector<void*> mapped_ptrs;

        if (virtualAddress != nullptr) {
            // Validate that virtualAddress falls inside a caller-reserved placeholder from VirtualAllocFromApp.
            const uintptr_t req = reinterpret_cast<uintptr_t>(virtualAddress);
            bool valid_placeholder = false;
            for (const auto& [b, rec] : app_allocs_) {
                if (rec.is_placeholder && req >= b && req + total_bytes <= b + rec.size) {
                    valid_placeholder = true;
                    break;
                }
            }
            if (!valid_placeholder || !UnifiedMemoryManager::SupportsOsPlaceholders()) {
                SetHostLastError(487u);  // ERROR_INVALID_ADDRESS
                return nullptr;
            }

            if (is_contiguous_single_pool) {
                UnifiedMemoryAlias alias{};
                const size_t phys_off = static_cast<size_t>(frames[0].page_index) * page_bytes;
                Status st = memory_.MapPhysicalAlias(frames[0].physical_id,
                                                     phys_off,
                                                     total_bytes,
                                                     prot,
                                                     mem_flags,
                                                     virtualAddress,
                                                     alias);
                if (!st.ok()) {
                    SetHostLastError(487u);
                    return nullptr;
                }
                base_va = alias.host_ptr;
                mapped_ptrs.push_back(alias.host_ptr);
            } else {
                base_va = virtualAddress;
                for (uint64_t i = 0; i < numberOfPages; ++i) {
                    void* slot = static_cast<uint8_t*>(virtualAddress) + static_cast<size_t>(i) * page_bytes;
                    UnifiedMemoryAlias alias{};
                    Status st = memory_.MapPhysicalAlias(frames[i].physical_id,
                                                         static_cast<size_t>(frames[i].page_index) * page_bytes,
                                                         page_bytes,
                                                         prot,
                                                         mem_flags,
                                                         slot,
                                                         alias);
                    if (!st.ok()) {
                        for (void* p : mapped_ptrs) {
                            (void)memory_.UnmapPhysicalAlias(p);
                        }
                        SetHostLastError(487u);
                        return nullptr;
                    }
                    mapped_ptrs.push_back(alias.host_ptr);
                }
            }
        } else if (is_contiguous_single_pool) {
            // Fast path: contiguous pages from one physical pool -> single OS view alias.
            UnifiedMemoryAlias alias{};
            const size_t phys_off = static_cast<size_t>(frames[0].page_index) * page_bytes;
            Status st = memory_.MapPhysicalAlias(frames[0].physical_id,
                                                 phys_off,
                                                 total_bytes,
                                                 prot,
                                                 mem_flags,
                                                 nullptr,
                                                 alias);
            if (!st.ok()) {
                SetHostLastError(8u);
                return nullptr;
            }
            base_va = alias.host_ptr;
            mapped_ptrs.push_back(alias.host_ptr);
        } else {
            // Non-contiguous / reordered / duplicated PFNs -> contiguous OS virtual placeholder.
            if (!UnifiedMemoryManager::SupportsOsPlaceholders()) {
                SetHostLastError(50u);  // ERROR_NOT_SUPPORTED
                return nullptr;
            }
            Status ph_st = memory_.ReserveVirtualPlaceholder(total_bytes, base_va);
            if (!ph_st.ok()) {
                SetHostLastError(8u);
                return nullptr;
            }
            owns_placeholder = true;
            for (uint64_t i = 0; i < numberOfPages; ++i) {
                void* slot = static_cast<uint8_t*>(base_va) + static_cast<size_t>(i) * page_bytes;
                UnifiedMemoryAlias alias{};
                Status st = memory_.MapPhysicalAlias(frames[i].physical_id,
                                                     static_cast<size_t>(frames[i].page_index) * page_bytes,
                                                     page_bytes,
                                                     prot,
                                                     mem_flags,
                                                     slot,
                                                     alias);
                if (!st.ok()) {
                    for (void* p : mapped_ptrs) {
                        (void)memory_.UnmapPhysicalAlias(p);
                    }
                    (void)memory_.ReleaseVirtualPlaceholder(base_va);
                    SetHostLastError(8u);
                    return nullptr;
                }
                mapped_ptrs.push_back(alias.host_ptr);
            }
        }

        // Commit mapping record and bump PFN mapping refcounts.
        ActiveTitleMapping mapping{};
        mapping.base_va = reinterpret_cast<uintptr_t>(base_va);
        mapping.size_bytes = total_bytes;
        mapping.page_size_bytes = page_bytes;
        mapping.primary_physical_id = frames[0].physical_id;
        mapping.alloc_flags = flAllocationType | frames[0].alloc_flags;
        mapping.win32_protect = flProtect;
        mapping.owns_placeholder = owns_placeholder;
        mapping.pfns.assign(pPageArray, pPageArray + numberOfPages);
        mapping.alias_ptrs = std::move(mapped_ptrs);

        for (uint64_t pfn : mapping.pfns) {
            ++pfn_table_[pfn].map_refcount;
        }
        mappings_.emplace(mapping.base_va, std::move(mapping));
        return base_va;
    }

    int32_t UnmapPhysicalPages(void* virtualAddress, uint64_t numberOfPages) {
        if (!virtualAddress) {
            SetHostLastError(87u);
            return 0;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const uintptr_t key = reinterpret_cast<uintptr_t>(virtualAddress);
        auto it = mappings_.find(key);
        if (it == mappings_.end()) {
            SetHostLastError(87u);
            return 0;
        }
        if (numberOfPages != 0 && numberOfPages != it->second.pfns.size()) {
            SetHostLastError(87u);
            return 0;
        }

        for (void* alias_ptr : it->second.alias_ptrs) {
            Status st = memory_.UnmapPhysicalAlias(alias_ptr);
            if (!st.ok()) {
                SetHostLastError(87u);
                return 0;
            }
        }
        if (it->second.owns_placeholder) {
            (void)memory_.ReleaseVirtualPlaceholder(virtualAddress);
        }

        for (uint64_t pfn : it->second.pfns) {
            auto pit = pfn_table_.find(pfn);
            if (pit != pfn_table_.end() && pit->second.map_refcount > 0) {
                --pit->second.map_refcount;
            }
        }

        mappings_.erase(it);
        return 1;
    }

    int32_t QueryPhysicalPages(const void* virtualAddress,
                               uint64_t* outPhysicalId,
                               uint64_t* outSizeBytes,
                               uint32_t* outFlags) const {
        if (!virtualAddress) {
            SetHostLastError(87u);
            return 0;
        }

        const uintptr_t addr = reinterpret_cast<uintptr_t>(virtualAddress);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [base, m] : mappings_) {
                if (addr >= base && addr < base + m.size_bytes) {
                    uint64_t phys_id = m.primary_physical_id;
                    if (m.page_size_bytes > 0 && !m.pfns.empty()) {
                        const size_t page_idx = static_cast<size_t>((addr - base) / m.page_size_bytes);
                        if (page_idx < m.pfns.size()) {
                            auto pit = pfn_table_.find(m.pfns[page_idx]);
                            if (pit != pfn_table_.end()) {
                                phys_id = pit->second.physical_id;
                            }
                        }
                    }
                    if (outPhysicalId) *outPhysicalId = phys_id;
                    if (outSizeBytes) *outSizeBytes = m.size_bytes;
                    if (outFlags) *outFlags = m.alloc_flags;
                    return 1;
                }
            }
        }

        // Fallback: inspect aliases directly managed by UnifiedMemoryManager (e.g. GpuTilePool).
        UnifiedMemoryAlias alias{};
        if (memory_.LookupAliasByHostPtr(virtualAddress, alias)) {
            if (outPhysicalId) *outPhysicalId = alias.physical_id;
            if (outSizeBytes) *outSizeBytes = alias.size;
            if (outFlags) {
                uint32_t f = 0;
                if (alias.flags & scarlettx::kMemFlagGraphics) f |= MEM_GRAPHICS;
                if (alias.flags & scarlettx::kMemFlagLargePages) f |= MEM_LARGE_PAGES;
                *outFlags = f;
            }
            return 1;
        }

        SetHostLastError(87u);
        return 0;
    }

    void* VirtualAllocApp(void* baseAddress,
                          size_t size,
                          uint32_t allocationType,
                          uint32_t protection) {
        if (size == 0 || allocationType == 0 || !ValidateProtect(protection)) {
            SetHostLastError(87u);
            return nullptr;
        }

        const bool reserve_only =
            ((allocationType & MEM_RESERVE) != 0u) && ((allocationType & MEM_COMMIT) == 0u);
        const bool is_graphics = (allocationType & MEM_GRAPHICS) != 0u;

        // MEM_RESERVE | MEM_GRAPHICS creates a real OS virtual placeholder so
        // MapTitlePhysicalPages(va, ...) can bind physical pages into it directly.
        if (reserve_only && is_graphics && baseAddress == nullptr &&
            UnifiedMemoryManager::SupportsOsPlaceholders()) {
            const size_t aligned = AlignUp(size, kPage64KB);
            void* base = nullptr;
            Status st = memory_.ReserveVirtualPlaceholder(aligned, base);
            if (!st.ok()) {
                SetHostLastError(8u);
                return nullptr;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            AppVirtualAllocRecord rec{};
            rec.base = reinterpret_cast<uintptr_t>(base);
            rec.size = aligned;
            rec.committed_bytes = 0;
            rec.alloc_flags = allocationType;
            rec.protection = protection;
            rec.is_placeholder = true;
            app_allocs_.emplace(rec.base, rec);
            return base;
        }

        const size_t align = (allocationType & (MEM_LARGE_PAGES | MEM_4MB_PAGES))
                                 ? kPage4MB
                                 : (is_graphics ? kPage64KB : 4096u);
        const size_t aligned_size = AlignUp(size, align);

        if ((allocationType & MEM_COMMIT) != 0u &&
            CurrentTitleUsedBytes() + aligned_size > GetTitleBudgetBytes()) {
            SetHostLastError(8u);
            return nullptr;
        }

#if defined(_WIN32)
        // Strip Xbox-specific flags before forwarding to Win32 VirtualAlloc.
        DWORD win_alloc = 0;
        if (allocationType & MEM_COMMIT) win_alloc |= MEM_COMMIT;
        if (allocationType & MEM_RESERVE) win_alloc |= MEM_RESERVE;
        if (allocationType & MEM_RESET) win_alloc |= MEM_RESET;
        if (allocationType & MEM_TOP_DOWN) win_alloc |= MEM_TOP_DOWN;
        if (win_alloc == 0) {
            SetHostLastError(87u);
            return nullptr;
        }
        const DWORD win_prot = ToWin32PageProtect(protection);
        void* ptr = VirtualAlloc(baseAddress, aligned_size, win_alloc, win_prot);
        if (!ptr) {
            return nullptr;
        }
#else
        (void)baseAddress;
        void* ptr = std::calloc(1, aligned_size);
        if (!ptr) {
            return nullptr;
        }
#endif

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const uintptr_t key = reinterpret_cast<uintptr_t>(ptr);
            auto it = app_allocs_.find(key);
            if (it != app_allocs_.end()) {
                if ((allocationType & MEM_COMMIT) != 0u) {
                    const size_t new_committed = std::max(it->second.committed_bytes, aligned_size);
                    const size_t delta = new_committed - it->second.committed_bytes;
                    it->second.committed_bytes = new_committed;
                    app_committed_bytes_.fetch_add(delta, std::memory_order_acq_rel);
                }
                it->second.protection = protection;
            } else {
                AppVirtualAllocRecord rec{};
                rec.base = key;
                rec.size = aligned_size;
                rec.committed_bytes = (allocationType & MEM_COMMIT) ? aligned_size : 0;
                rec.alloc_flags = allocationType;
                rec.protection = protection;
                rec.is_placeholder = false;
                app_allocs_.emplace(key, rec);
                if (rec.committed_bytes > 0) {
                    app_committed_bytes_.fetch_add(rec.committed_bytes, std::memory_order_acq_rel);
                }
            }
        }

        UpdateAndGetPeakTitleUsed();
        return ptr;
    }

    int32_t VirtualFreeApp(void* address, size_t size, uint32_t freeType) {
        if (!address || freeType == 0) {
            SetHostLastError(87u);
            return 0;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const uintptr_t key = reinterpret_cast<uintptr_t>(address);
        auto it = app_allocs_.find(key);
        if (it != app_allocs_.end()) {
            if (it->second.is_placeholder) {
                Status st = memory_.ReleaseVirtualPlaceholder(address);
                if (!st.ok()) {
                    SetHostLastError(87u);
                    return 0;
                }
                app_allocs_.erase(it);
                return 1;
            }

            const bool is_release = (freeType & MEM_RELEASE) != 0u;
#if defined(_WIN32)
            if (!VirtualFree(address, is_release ? 0 : size, static_cast<DWORD>(freeType))) {
                return 0;
            }
#else
            (void)size;
            if (is_release) {
                std::free(address);
            }
#endif
            if (is_release) {
                if (it->second.committed_bytes > 0) {
                    app_committed_bytes_.fetch_sub(it->second.committed_bytes, std::memory_order_acq_rel);
                }
                app_allocs_.erase(it);
            } else if (freeType & MEM_DECOMMIT) {
                const size_t decommit = (size == 0 || size >= it->second.committed_bytes)
                                            ? it->second.committed_bytes
                                            : size;
                it->second.committed_bytes -= decommit;
                app_committed_bytes_.fetch_sub(decommit, std::memory_order_acq_rel);
            }
            return 1;
        }

#if defined(_WIN32)
        return VirtualFree(address, size, static_cast<DWORD>(freeType)) ? 1 : 0;
#else
        return 0;
#endif
    }

    int32_t VirtualProtectApp(void* address,
                              size_t size,
                              uint32_t newProtection,
                              uint32_t* oldProtection) {
        if (!address || size == 0 || !oldProtection || !ValidateProtect(newProtection)) {
            SetHostLastError(87u);
            return 0;
        }

        const uintptr_t req_start = reinterpret_cast<uintptr_t>(address);
        const uintptr_t req_end = req_start + size;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            // 1. Check active MapTitlePhysicalPages aliases first.
            for (auto& [base, m] : mappings_) {
                if (req_start >= base && req_end <= base + m.size_bytes) {
                    const uint32_t prev = m.win32_protect;
                    const MemoryProtection mem_prot = ToMemoryProtection(newProtection);

                    if (m.alias_ptrs.size() == 1 && req_start == base && size == m.size_bytes) {
                        Status st = memory_.ProtectAlias(m.alias_ptrs[0], size, mem_prot);
                        if (!st.ok()) {
                            SetHostLastError(87u);
                            return 0;
                        }
                    } else {
                        // Multi-page mapping or sub-range protection update.
                        for (void* alias_ptr : m.alias_ptrs) {
                            const uintptr_t a_start = reinterpret_cast<uintptr_t>(alias_ptr);
                            const size_t a_size =
                                (m.alias_ptrs.size() == 1) ? m.size_bytes : m.page_size_bytes;
                            const uintptr_t a_end = a_start + a_size;
                            if (a_end <= req_start || a_start >= req_end) {
                                continue;
                            }
                            if (req_start <= a_start && req_end >= a_end) {
                                Status st = memory_.ProtectAlias(alias_ptr, a_size, mem_prot);
                                if (!st.ok()) {
                                    SetHostLastError(87u);
                                    return 0;
                                }
                            } else {
#if defined(_WIN32)
                                const uintptr_t sub_start = std::max(req_start, a_start);
                                const uintptr_t sub_end = std::min(req_end, a_end);
                                DWORD host_old = 0;
                                if (!VirtualProtect(reinterpret_cast<void*>(sub_start),
                                                    sub_end - sub_start,
                                                    ToWin32PageProtect(newProtection),
                                                    &host_old)) {
                                    return 0;
                                }
#endif
                            }
                        }
                    }

                    *oldProtection = prev;
                    m.win32_protect = newProtection;
                    return 1;
                }
            }

            // 2. Check VirtualAllocFromApp allocations.
            for (auto& [base, rec] : app_allocs_) {
                if (req_start >= base && req_end <= base + rec.size) {
                    if (rec.is_placeholder) {
                        SetHostLastError(487u);  // Cannot protect unmapped placeholder
                        return 0;
                    }
#if defined(_WIN32)
                    DWORD host_old = 0;
                    if (!VirtualProtect(address, size, ToWin32PageProtect(newProtection), &host_old)) {
                        return 0;
                    }
#endif
                    *oldProtection = rec.protection;
                    rec.protection = newProtection;
                    return 1;
                }
            }
        }

        // 3. Fallback to host OS VirtualProtect for arbitrary process memory or tile aliases.
#if defined(_WIN32)
        DWORD host_old = 0;
        if (!VirtualProtect(address, size, ToWin32PageProtect(newProtection), &host_old)) {
            return 0;
        }
        *oldProtection = static_cast<uint32_t>(host_old);
        return 1;
#else
        *oldProtection = PAGE_READWRITE;
        return 1;
#endif
    }

    ~KernelXState() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [base, m] : mappings_) {
            for (void* p : m.alias_ptrs) {
                (void)memory_.UnmapPhysicalAlias(p);
            }
            if (m.owns_placeholder) {
                (void)memory_.ReleaseVirtualPlaceholder(reinterpret_cast<void*>(base));
            }
        }
        mappings_.clear();

        for (auto& [base, rec] : app_allocs_) {
            if (rec.is_placeholder) {
                (void)memory_.ReleaseVirtualPlaceholder(reinterpret_cast<void*>(base));
            } else {
#if defined(_WIN32)
                VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE);
#else
                std::free(reinterpret_cast<void*>(base));
#endif
            }
        }
        app_allocs_.clear();
        pfn_table_.clear();
        pools_.clear();
    }

private:
    KernelXState() : memory_(), tile_pool_(memory_) {}

    UnifiedMemoryManager memory_;
    GpuTilePool tile_pool_;
    mutable std::mutex mutex_;
    uint64_t next_pfn_ = 0x10000ull;
    std::atomic<uint64_t> app_committed_bytes_{0};
    std::atomic<uint64_t> peak_title_used_{0};
    std::unordered_map<uint64_t, PhysicalPoolRecord> pools_;
    std::unordered_map<uint64_t, PageFrameRecord> pfn_table_;
    std::unordered_map<uintptr_t, ActiveTitleMapping> mappings_;
    std::unordered_map<uintptr_t, AppVirtualAllocRecord> app_allocs_;
};

}  // namespace

extern "C" {

int32_t SCARLETTX_KERNELX_CALL TitleMemoryStatus(TITLEMEMORYSTATUS* lpBuffer) {
    if (!lpBuffer) {
        SetHostLastError(87u);
        return 0;
    }
    if (lpBuffer->dwLength != 0 && lpBuffer->dwLength < sizeof(TITLEMEMORYSTATUS)) {
        SetHostLastError(87u);
        return 0;
    }

    auto& state = KernelXState::Instance();
    const uint64_t total = GetTitleBudgetBytes();
    const uint64_t used = state.CurrentTitleUsedBytes();
    const uint64_t peak = state.UpdateAndGetPeakTitleUsed();
    const uint64_t avail = (total > used) ? (total - used) : 0ull;

    lpBuffer->dwLength = static_cast<uint32_t>(sizeof(TITLEMEMORYSTATUS));
    lpBuffer->dwReserved = 0;
    lpBuffer->ullTotalMem = total;
    lpBuffer->ullAvailMem = avail;
    lpBuffer->ullLegacyUsed = used;
    lpBuffer->ullLegacyPeak = peak;
    lpBuffer->ullLegacyAvail = avail;
    lpBuffer->ullTitleUsed = used;
    lpBuffer->ullTitleAvail = avail;
    return 1;
}

int32_t SCARLETTX_KERNELX_CALL ToolingMemoryStatus(TOOLINGMEMORYSTATUS* lpBuffer) {
    if (!lpBuffer) {
        SetHostLastError(87u);
        return 0;
    }
    if (lpBuffer->dwLength != 0 && lpBuffer->dwLength < sizeof(TOOLINGMEMORYSTATUS)) {
        SetHostLastError(87u);
        return 0;
    }

    const uint64_t total = GetToolingBudgetBytes();
    const uint64_t used = 0ull;
    lpBuffer->dwLength = static_cast<uint32_t>(sizeof(TOOLINGMEMORYSTATUS));
    lpBuffer->dwReserved = 0;
    lpBuffer->ullTotalMem = total;
    lpBuffer->ullAvailMem = total - used;
    lpBuffer->ullUsed = used;
    lpBuffer->ullPeak = used;
    return 1;
}

int32_t SCARLETTX_KERNELX_CALL AllocateTitlePhysicalPages(void* hProcess,
                                                          uint32_t flAllocationType,
                                                          uint64_t* pNumberOfPages,
                                                          uint64_t* pPageArray) {
    (void)hProcess;
    return KernelXState::Instance().AllocatePhysicalPages(flAllocationType, pNumberOfPages, pPageArray);
}

int32_t SCARLETTX_KERNELX_CALL FreeTitlePhysicalPages(void* hProcess,
                                                      uint64_t numberOfPages,
                                                      const uint64_t* pPageArray) {
    (void)hProcess;
    return KernelXState::Instance().FreePhysicalPages(numberOfPages, pPageArray);
}

void* SCARLETTX_KERNELX_CALL MapTitlePhysicalPages(void* virtualAddress,
                                                   uint64_t numberOfPages,
                                                   uint32_t flAllocationType,
                                                   uint32_t flProtect,
                                                   const uint64_t* pPageArray) {
    return KernelXState::Instance().MapPhysicalPages(
        virtualAddress, numberOfPages, flAllocationType, flProtect, pPageArray);
}

int32_t SCARLETTX_KERNELX_CALL UnmapTitlePhysicalPages(void* virtualAddress,
                                                       uint64_t numberOfPages) {
    return KernelXState::Instance().UnmapPhysicalPages(virtualAddress, numberOfPages);
}

int32_t SCARLETTX_KERNELX_CALL QueryTitlePhysicalPages(const void* virtualAddress,
                                                       uint64_t* outPhysicalId,
                                                       uint64_t* outSizeBytes,
                                                       uint32_t* outFlags) {
    return KernelXState::Instance().QueryPhysicalPages(
        virtualAddress, outPhysicalId, outSizeBytes, outFlags);
}

void* SCARLETTX_KERNELX_CALL VirtualAllocFromApp(void* baseAddress,
                                                 size_t size,
                                                 uint32_t allocationType,
                                                 uint32_t protection) {
    return KernelXState::Instance().VirtualAllocApp(baseAddress, size, allocationType, protection);
}

int32_t SCARLETTX_KERNELX_CALL VirtualProtectFromApp(void* address,
                                                     size_t size,
                                                     uint32_t newProtection,
                                                     uint32_t* oldProtection) {
    return KernelXState::Instance().VirtualProtectApp(address, size, newProtection, oldProtection);
}

int32_t SCARLETTX_KERNELX_CALL KernelXVirtualFree(void* address,
                                                  size_t size,
                                                  uint32_t freeType) {
    return KernelXState::Instance().VirtualFreeApp(address, size, freeType);
}

uint32_t SCARLETTX_KERNELX_CALL GetConsoleType(void) {
    return ResolveConsoleType();
}

int32_t SCARLETTX_KERNELX_CALL GetSystemOSVersion(KERNELX_OS_VERSION* outVersion) {
    if (!outVersion) {
        SetHostLastError(87u);
        return 0;
    }
    outVersion->MajorVersion = 10u;
    outVersion->MinorVersion = 0u;
    outVersion->BuildNumber = 26100u;
    outVersion->Revision = 2499u;
    return 1;
}

int32_t SCARLETTX_KERNELX_CALL KernelXCreateTilePool(uint32_t tileCount,
                                                     uint32_t flags,
                                                     uint64_t* outPoolId) {
    if (!outPoolId) {
        SetHostLastError(87u);
        return 0;
    }
    auto& state = KernelXState::Instance();
    uint64_t pool_id = 0;
    Status st = state.TilePool().CreatePool(tileCount, flags, pool_id);
    if (!st.ok()) {
        *outPoolId = 0;
        SetHostLastError(87u);
        return 0;
    }
    *outPoolId = pool_id;
    state.UpdateAndGetPeakTitleUsed();
    return 1;
}

int32_t SCARLETTX_KERNELX_CALL KernelXDestroyTilePool(uint64_t poolId) {
    Status st = KernelXState::Instance().TilePool().DestroyPool(poolId);
    if (!st.ok()) {
        SetHostLastError(87u);
        return 0;
    }
    return 1;
}

int32_t SCARLETTX_KERNELX_CALL KernelXReserveTiledResource(uint32_t widthTiles,
                                                           uint32_t heightTiles,
                                                           uint64_t* outResourceId,
                                                           void** outVirtualBase) {
    if (!outResourceId) {
        SetHostLastError(87u);
        return 0;
    }
    auto& tp = KernelXState::Instance().TilePool();
    uint64_t res_id = 0;
    Status st = tp.ReserveTiledResource(widthTiles, heightTiles, res_id);
    if (!st.ok()) {
        *outResourceId = 0;
        if (outVirtualBase) *outVirtualBase = nullptr;
        SetHostLastError(87u);
        return 0;
    }
    *outResourceId = res_id;
    if (outVirtualBase) {
        uint32_t w = 0, h = 0;
        void* base = nullptr;
        tp.GetResourceDimensions(res_id, w, h, base);
        *outVirtualBase = base;
    }
    return 1;
}

int32_t SCARLETTX_KERNELX_CALL KernelXReleaseTiledResource(uint64_t resourceId) {
    Status st = KernelXState::Instance().TilePool().ReleaseTiledResource(resourceId);
    if (!st.ok()) {
        SetHostLastError(87u);
        return 0;
    }
    return 1;
}

int32_t SCARLETTX_KERNELX_CALL KernelXMapTileRange(uint64_t resourceId,
                                                   uint32_t tileX,
                                                   uint32_t tileY,
                                                   uint64_t poolId,
                                                   uint32_t poolTileOffset,
                                                   uint32_t tileCount,
                                                   uint32_t flags) {
    GpuTileCoord coord{};
    coord.x = tileX;
    coord.y = tileY;
    coord.subresource = 0;
    Status st = KernelXState::Instance().TilePool().UpdateTileMappings(
        resourceId, coord, poolId, poolTileOffset, tileCount, flags);
    if (!st.ok()) {
        SetHostLastError(87u);
        return 0;
    }
    return 1;
}

}  // extern "C"
