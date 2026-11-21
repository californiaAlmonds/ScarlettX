#include "scarlettx/kernel/xmemory.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <malloc.h>
#include <windows.h>
#endif

namespace scarlettx {

namespace {

size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void* AllocAligned(size_t size, size_t alignment) {
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
#endif
}

void FreeAligned(void* ptr) {
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

#if defined(_WIN32)
constexpr DWORD kMemReservePlaceholder = 0x00040000u;
constexpr DWORD kMemReplacePlaceholder = 0x00004000u;
constexpr DWORD kMemPreservePlaceholder = 0x00000002u;

using FnVirtualAlloc2 = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, PVOID, ULONG);
using FnMapViewOfFile3 = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, PVOID, ULONG);
using FnUnmapViewOfFile2 = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

struct Win32PlaceholderApi {
    FnVirtualAlloc2 virtual_alloc2 = nullptr;
    FnMapViewOfFile3 map_view_of_file3 = nullptr;
    FnUnmapViewOfFile2 unmap_view_of_file2 = nullptr;

    Win32PlaceholderApi() {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        if (!kb) {
            kb = LoadLibraryW(L"kernelbase.dll");
        }
        if (kb) {
            virtual_alloc2 = reinterpret_cast<FnVirtualAlloc2>(GetProcAddress(kb, "VirtualAlloc2"));
            map_view_of_file3 = reinterpret_cast<FnMapViewOfFile3>(GetProcAddress(kb, "MapViewOfFile3"));
            unmap_view_of_file2 = reinterpret_cast<FnUnmapViewOfFile2>(GetProcAddress(kb, "UnmapViewOfFile2"));
        }
    }

    bool Available() const {
        return virtual_alloc2 != nullptr && map_view_of_file3 != nullptr && unmap_view_of_file2 != nullptr;
    }
};

const Win32PlaceholderApi& PlaceholderApi() {
    static const Win32PlaceholderApi api;
    return api;
}

DWORD ToWin32Protect(MemoryProtection prot) {
    switch (prot) {
        case MemoryProtection::NoAccess: return PAGE_NOACCESS;
        case MemoryProtection::ReadOnly: return PAGE_READONLY;
        case MemoryProtection::ReadWrite: return PAGE_READWRITE;
        case MemoryProtection::WriteCombined: return PAGE_READWRITE | PAGE_WRITECOMBINE;
        default: return PAGE_READWRITE;
    }
}
#endif

}  // namespace

UnifiedMemoryManager::UnifiedMemoryManager() = default;

UnifiedMemoryManager::~UnifiedMemoryManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [ptr, alias] : aliases_) {
#if defined(_WIN32)
        if (alias.from_placeholder && PlaceholderApi().Available()) {
            PlaceholderApi().unmap_view_of_file2(
                GetCurrentProcess(), reinterpret_cast<void*>(ptr), kMemPreservePlaceholder);
        } else {
            UnmapViewOfFile(reinterpret_cast<void*>(ptr));
        }
#else
        (void)ptr;
        (void)alias;
#endif
    }
    aliases_.clear();

    for (auto& [base, res] : placeholders_) {
#if defined(_WIN32)
        for (const auto& span : res.spans) {
            VirtualFree(reinterpret_cast<void*>(span.base), 0, MEM_RELEASE);
        }
#else
        (void)base;
        (void)res;
#endif
    }
    placeholders_.clear();

    for (auto& [id, pool] : physical_pools_) {
#if defined(_WIN32)
        if (pool.os_handle) {
            CloseHandle(static_cast<HANDLE>(pool.os_handle));
            pool.os_handle = nullptr;
        }
#else
        (void)id;
        (void)pool;
#endif
    }
    physical_pools_.clear();

    for (auto& [va, block] : blocks_) {
        FreeAligned(block.host_ptr);
    }
    blocks_.clear();
}

Status UnifiedMemoryManager::Allocate(size_t size,
                                      MemoryPageSize page_size,
                                      bool gpu_visible,
                                      bool write_combined,
                                      UnifiedMemoryBlock& out_block) {
    if (size == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Allocation size must be > 0");
    }

    const size_t alignment = static_cast<size_t>(page_size);
    const size_t aligned_size = AlignUp(size, alignment);
    void* mem = AllocAligned(aligned_size, alignment);
    if (!mem) {
        return Status::Error(StatusCode::OutOfMemory, "Unified memory allocation failed");
    }
    std::memset(mem, 0, aligned_size);

    std::lock_guard<std::mutex> lock(mutex_);
    const GpuVirtualAddress va = AlignUp(static_cast<size_t>(next_gpu_va_), alignment);
    next_gpu_va_ = va + aligned_size;

    UnifiedMemoryBlock block{};
    block.host_ptr = mem;
    block.gpu_va = va;
    block.size = aligned_size;
    block.page_size = page_size;
    block.gpu_visible = gpu_visible;
    block.write_combined = write_combined;

    blocks_[va] = block;
    total_allocated_bytes_ += aligned_size;
    out_block = block;
    return Status::Ok();
}

Status UnifiedMemoryManager::Free(GpuVirtualAddress gpu_va) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = blocks_.find(gpu_va);
    if (it == blocks_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown GPU virtual address");
    }
    FreeAligned(it->second.host_ptr);
    total_allocated_bytes_ -= it->second.size;
    blocks_.erase(it);
    return Status::Ok();
}

bool UnifiedMemoryManager::LookupByGpuVa(GpuVirtualAddress gpu_va,
                                         UnifiedMemoryBlock& out_block) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [base_va, block] : blocks_) {
        if (gpu_va >= base_va && gpu_va < base_va + block.size) {
            out_block = block;
            return true;
        }
    }
    return false;
}

size_t UnifiedMemoryManager::TotalAllocatedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_allocated_bytes_;
}

size_t UnifiedMemoryManager::ActiveAllocationCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return blocks_.size();
}

bool UnifiedMemoryManager::SupportsOsPlaceholders() {
#if defined(_WIN32)
    return PlaceholderApi().Available();
#else
    return false;
#endif
}

Status UnifiedMemoryManager::AllocatePhysicalPages(size_t size,
                                                   MemoryPageSize page_size,
                                                   uint32_t flags,
                                                   PhysicalAllocation& out_phys) {
    if (size == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Physical page allocation size must be > 0");
    }
    const size_t min_gran = std::max<size_t>(static_cast<size_t>(page_size), 65536u);
    const size_t aligned_size = AlignUp(size, min_gran);

    PhysicalPoolEntry entry{};
    entry.info.size = aligned_size;
    entry.info.page_size = page_size;
    entry.info.flags = flags;

#if defined(_WIN32)
    const DWORD hi = static_cast<DWORD>((static_cast<uint64_t>(aligned_size) >> 32) & 0xFFFFFFFFull);
    const DWORD lo = static_cast<DWORD>(static_cast<uint64_t>(aligned_size) & 0xFFFFFFFFull);
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, hi, lo, nullptr);
    if (!section) {
        return Status::Error(StatusCode::OutOfMemory, "CreateFileMappingW failed for physical page pool");
    }
    entry.os_handle = static_cast<void*>(section);
#else
    entry.fallback_storage.assign(aligned_size, 0u);
#endif

    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t id = next_physical_id_++;
    entry.info.physical_id = id;
    out_phys = entry.info;
    physical_pools_.emplace(id, std::move(entry));
    total_allocated_bytes_ += aligned_size;
    return Status::Ok();
}

Status UnifiedMemoryManager::FreePhysicalPages(uint64_t physical_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = physical_pools_.find(physical_id);
    if (it == physical_pools_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown physical page pool id");
    }
    for (const auto& [ptr, alias] : aliases_) {
        if (alias.physical_id == physical_id) {
            return Status::Error(StatusCode::InvalidArgument, "Physical pool still has active virtual aliases");
        }
    }
#if defined(_WIN32)
    if (it->second.os_handle) {
        CloseHandle(static_cast<HANDLE>(it->second.os_handle));
        it->second.os_handle = nullptr;
    }
#endif
    total_allocated_bytes_ =
        total_allocated_bytes_ >= it->second.info.size ? (total_allocated_bytes_ - it->second.info.size) : 0;
    physical_pools_.erase(it);
    return Status::Ok();
}

Status UnifiedMemoryManager::ReserveVirtualPlaceholder(size_t size, void*& out_base) {
    out_base = nullptr;
    if (size == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Placeholder reservation size must be > 0");
    }
    const size_t aligned_size = AlignUp(size, 65536u);
#if defined(_WIN32)
    if (!PlaceholderApi().Available()) {
        return Status::Error(StatusCode::NotImplemented, "VirtualAlloc2 placeholder API unavailable");
    }
    void* base = PlaceholderApi().virtual_alloc2(
        GetCurrentProcess(), nullptr, aligned_size, MEM_RESERVE | kMemReservePlaceholder, PAGE_NOACCESS, nullptr, 0);
    if (!base) {
        return Status::Error(StatusCode::OutOfMemory, "VirtualAlloc2 placeholder reservation failed");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    PlaceholderReservation res{};
    res.base = reinterpret_cast<uintptr_t>(base);
    res.total_size = aligned_size;
    res.spans.push_back({res.base, aligned_size, false});
    placeholders_.emplace(res.base, std::move(res));
    out_base = base;
    return Status::Ok();
#else
    (void)aligned_size;
    return Status::Error(StatusCode::NotImplemented, "OS placeholders require Windows 10+");
#endif
}

Status UnifiedMemoryManager::ReleaseVirtualPlaceholder(void* base) {
    if (!base) {
        return Status::Error(StatusCode::InvalidArgument, "Null placeholder base");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const uintptr_t key = reinterpret_cast<uintptr_t>(base);
    auto it = placeholders_.find(key);
    if (it == placeholders_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown placeholder reservation");
    }
    for (const auto& span : it->second.spans) {
        if (span.mapped) {
            return Status::Error(StatusCode::InvalidArgument, "Placeholder reservation still has mapped aliases");
        }
    }
#if defined(_WIN32)
    for (const auto& span : it->second.spans) {
        VirtualFree(reinterpret_cast<void*>(span.base), 0, MEM_RELEASE);
    }
#endif
    placeholders_.erase(it);
    return Status::Ok();
}

Status UnifiedMemoryManager::MapPhysicalAlias(uint64_t physical_id,
                                              size_t physical_offset,
                                              size_t size,
                                              MemoryProtection protection,
                                              uint32_t flags,
                                              void* placeholder_addr,
                                              UnifiedMemoryAlias& out_alias) {
    if (size == 0 || (physical_offset & 0xFFFFu) != 0u) {
        return Status::Error(StatusCode::InvalidArgument, "Alias size must be > 0 and offset 64KB-aligned");
    }
    const size_t aligned_size = AlignUp(size, 65536u);

    std::lock_guard<std::mutex> lock(mutex_);
    auto pit = physical_pools_.find(physical_id);
    if (pit == physical_pools_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown physical pool id");
    }
    auto& pool = pit->second;
    if (physical_offset > pool.info.size || aligned_size > pool.info.size - physical_offset) {
        return Status::Error(StatusCode::InvalidArgument, "Physical alias range exceeds pool size");
    }

    void* mapped_ptr = nullptr;
    bool used_placeholder = false;

#if defined(_WIN32)
    HANDLE section = static_cast<HANDLE>(pool.os_handle);
    if (placeholder_addr != nullptr) {
        if (!PlaceholderApi().Available()) {
            return Status::Error(StatusCode::NotImplemented, "MapViewOfFile3 unavailable");
        }
        const uintptr_t req_start = reinterpret_cast<uintptr_t>(placeholder_addr);
        const uintptr_t req_end = req_start + aligned_size;
        if ((req_start & 0xFFFFu) != 0u) {
            return Status::Error(StatusCode::InvalidArgument, "Placeholder target must be 64KB-aligned");
        }

        // Locate enclosing unmapped placeholder span and split if larger than requested view.
        PlaceholderReservation* owner = nullptr;
        size_t span_idx = 0;
        for (auto& [rbase, res] : placeholders_) {
            if (req_start >= res.base && req_end <= res.base + res.total_size) {
                for (size_t i = 0; i < res.spans.size(); ++i) {
                    const auto& sp = res.spans[i];
                    if (!sp.mapped && req_start >= sp.base && req_end <= sp.base + sp.size) {
                        owner = &res;
                        span_idx = i;
                        break;
                    }
                }
                break;
            }
        }
        if (!owner) {
            return Status::Error(StatusCode::InvalidArgument, "Target address is not an unmapped placeholder range");
        }

        const PlaceholderSpan orig = owner->spans[span_idx];
        if (orig.base != req_start || orig.size != aligned_size) {
            if (!VirtualFree(placeholder_addr, aligned_size, MEM_RELEASE | kMemPreservePlaceholder)) {
                return Status::Error(StatusCode::OutOfMemory, "Failed to split virtual memory placeholder");
            }
            std::vector<PlaceholderSpan> replacement;
            if (req_start > orig.base) {
                replacement.push_back({orig.base, req_start - orig.base, false});
            }
            replacement.push_back({req_start, aligned_size, false});
            if (req_end < orig.base + orig.size) {
                replacement.push_back({req_end, (orig.base + orig.size) - req_end, false});
            }
            owner->spans.erase(owner->spans.begin() + static_cast<std::ptrdiff_t>(span_idx));
            owner->spans.insert(owner->spans.begin() + static_cast<std::ptrdiff_t>(span_idx),
                                replacement.begin(), replacement.end());
            // Re-find exact target span index.
            for (size_t i = 0; i < owner->spans.size(); ++i) {
                if (owner->spans[i].base == req_start && owner->spans[i].size == aligned_size) {
                    span_idx = i;
                    break;
                }
            }
        }

        const DWORD page_prot =
            (protection == MemoryProtection::ReadOnly) ? PAGE_READONLY : PAGE_READWRITE;
        mapped_ptr = PlaceholderApi().map_view_of_file3(
            section, GetCurrentProcess(), placeholder_addr, static_cast<ULONG64>(physical_offset), aligned_size,
            kMemReplacePlaceholder, page_prot, nullptr, 0);
        if (!mapped_ptr) {
            return Status::Error(StatusCode::OutOfMemory, "MapViewOfFile3 failed on placeholder");
        }
        owner->spans[span_idx].mapped = true;
        used_placeholder = true;
    } else {
        const DWORD access =
            (protection == MemoryProtection::ReadOnly) ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
        const DWORD off_hi = static_cast<DWORD>((static_cast<uint64_t>(physical_offset) >> 32) & 0xFFFFFFFFull);
        const DWORD off_lo = static_cast<DWORD>(static_cast<uint64_t>(physical_offset) & 0xFFFFFFFFull);
        mapped_ptr = MapViewOfFile(section, access, off_hi, off_lo, aligned_size);
        if (!mapped_ptr) {
            return Status::Error(StatusCode::OutOfMemory, "MapViewOfFile failed for physical alias");
        }
    }
    if (protection == MemoryProtection::NoAccess) {
        DWORD old_prot = 0;
        VirtualProtect(mapped_ptr, aligned_size, PAGE_NOACCESS, &old_prot);
    }
#else
    (void)placeholder_addr;
    mapped_ptr = pool.fallback_storage.data() + physical_offset;
#endif

    const GpuVirtualAddress gpu_va = AlignUp(static_cast<size_t>(next_gpu_va_), 65536u);
    next_gpu_va_ = gpu_va + aligned_size;

    UnifiedMemoryAlias alias{};
    alias.host_ptr = mapped_ptr;
    alias.gpu_va = gpu_va;
    alias.physical_id = physical_id;
    alias.physical_offset = physical_offset;
    alias.size = aligned_size;
    alias.protection = protection;
    alias.flags = flags;
    alias.from_placeholder = used_placeholder;

    aliases_[reinterpret_cast<uintptr_t>(mapped_ptr)] = alias;
    out_alias = alias;
    return Status::Ok();
}

Status UnifiedMemoryManager::UnmapPhysicalAlias(void* host_ptr) {
    if (!host_ptr) {
        return Status::Error(StatusCode::InvalidArgument, "Null alias pointer");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const uintptr_t key = reinterpret_cast<uintptr_t>(host_ptr);
    auto it = aliases_.find(key);
    if (it == aliases_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown physical memory alias");
    }
    const bool from_ph = it->second.from_placeholder;
#if defined(_WIN32)
    BOOL ok = FALSE;
    if (from_ph && PlaceholderApi().Available()) {
        ok = PlaceholderApi().unmap_view_of_file2(GetCurrentProcess(), host_ptr, kMemPreservePlaceholder);
    } else {
        ok = UnmapViewOfFile(host_ptr);
    }
    if (!ok) {
        return Status::Error(StatusCode::DeviceError, "Failed to unmap physical memory alias");
    }
#endif
    if (from_ph) {
        for (auto& [rbase, res] : placeholders_) {
            for (auto& sp : res.spans) {
                if (sp.base == key) {
                    sp.mapped = false;
                    break;
                }
            }
        }
    }
    aliases_.erase(it);
    return Status::Ok();
}

Status UnifiedMemoryManager::ProtectAlias(void* host_ptr, size_t size, MemoryProtection new_protection) {
    if (!host_ptr || size == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid alias protection arguments");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const uintptr_t key = reinterpret_cast<uintptr_t>(host_ptr);
    auto it = aliases_.find(key);
    if (it == aliases_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown physical memory alias");
    }
#if defined(_WIN32)
    DWORD old_prot = 0;
    const DWORD win_prot = (new_protection == MemoryProtection::WriteCombined)
                               ? PAGE_READWRITE
                               : ToWin32Protect(new_protection);
    if (!VirtualProtect(host_ptr, size, win_prot, &old_prot)) {
        return Status::Error(StatusCode::DeviceError, "VirtualProtect failed on alias");
    }
#endif
    it->second.protection = new_protection;
    return Status::Ok();
}

bool UnifiedMemoryManager::LookupAliasByHostPtr(const void* host_ptr, UnifiedMemoryAlias& out_alias) const {
    if (!host_ptr) return false;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(host_ptr);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [base, alias] : aliases_) {
        if (addr >= base && addr < base + alias.size) {
            out_alias = alias;
            return true;
        }
    }
    return false;
}

size_t UnifiedMemoryManager::ActivePhysicalPoolCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return physical_pools_.size();
}

size_t UnifiedMemoryManager::ActiveAliasCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return aliases_.size();
}

}  // namespace scarlettx
