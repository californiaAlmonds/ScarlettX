#include "scarlettx/kernel/gpu_tile_pool.h"

#include <cstring>
#include <limits>

namespace scarlettx {

namespace {

constexpr uint32_t kMaxTilesPerPool = 65536u;       // 4 GiB per single pool
constexpr uint32_t kMaxTilesPerResource = 65536u;   // 4 GiB per single tiled resource

uint32_t ToUnifiedMemoryFlags(uint32_t pool_flags) {
    uint32_t mem_flags = kMemFlagGraphics;  // GPU tile pools are inherently GPU-visible
    if (pool_flags & kTilePoolFlagLargePages) {
        mem_flags |= kMemFlagLargePages;
    }
    if (pool_flags & kTilePoolFlagWriteCombined) {
        mem_flags |= kMemFlagWriteCombined;
    }
    return mem_flags;
}

}  // namespace

GpuTilePool::GpuTilePool(UnifiedMemoryManager& memory_manager)
    : memory_(memory_manager) {}

GpuTilePool::~GpuTilePool() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [res_id, res] : resources_) {
        for (auto& slot : res.slots) {
            if (slot.mapped) {
                (void)UnmapSlotLocked(slot);
            }
        }
        if (res.use_placeholder && res.virtual_base != nullptr) {
            (void)memory_.ReleaseVirtualPlaceholder(res.virtual_base);
            res.virtual_base = nullptr;
        }
    }
    resources_.clear();

    for (auto& [pool_id, pool] : pools_) {
        (void)memory_.FreePhysicalPages(pool.physical_id);
    }
    pools_.clear();
    phys_tile_refcounts_.clear();
    total_physical_tiles_ = 0;
    total_physical_bytes_ = 0;
    mapped_virtual_tiles_ = 0;
    aliased_tile_count_ = 0;
}

void GpuTilePool::IncrementPhysicalRefLocked(uint64_t pool_id, uint32_t tile_offset) {
    const PhysicalTileKey key{pool_id, tile_offset};
    uint32_t& ref = phys_tile_refcounts_[key];
    if (ref >= 1u) {
        ++aliased_tile_count_;
    }
    ++ref;
    auto pit = pools_.find(pool_id);
    if (pit != pools_.end()) {
        ++pit->second.active_mappings;
    }
}

void GpuTilePool::DecrementPhysicalRefLocked(uint64_t pool_id, uint32_t tile_offset) {
    const PhysicalTileKey key{pool_id, tile_offset};
    auto it = phys_tile_refcounts_.find(key);
    if (it != phys_tile_refcounts_.end()) {
        if (it->second > 1u) {
            --it->second;
            if (aliased_tile_count_ > 0) {
                --aliased_tile_count_;
            }
        } else {
            phys_tile_refcounts_.erase(it);
        }
    }
    auto pit = pools_.find(pool_id);
    if (pit != pools_.end() && pit->second.active_mappings > 0) {
        --pit->second.active_mappings;
    }
}

Status GpuTilePool::UnmapSlotLocked(VirtualTileSlot& slot) {
    if (!slot.mapped) {
        return Status::Ok();
    }
    Status st = memory_.UnmapPhysicalAlias(slot.virtual_address);
    if (!st.ok()) {
        return st;
    }
    DecrementPhysicalRefLocked(slot.pool_id, slot.pool_tile_offset);
    if (mapped_virtual_tiles_ > 0) {
        --mapped_virtual_tiles_;
    }
    slot = VirtualTileSlot{};
    return Status::Ok();
}

Status GpuTilePool::CreatePool(uint32_t tile_count, uint32_t flags, uint64_t& out_pool_id) {
    out_pool_id = 0;
    if (tile_count == 0 || tile_count > kMaxTilesPerPool) {
        return Status::Error(StatusCode::InvalidArgument, "Tile pool count must be in [1, 65536]");
    }

    const size_t req_bytes = static_cast<size_t>(tile_count) * kGpuTileSizeBytes;
    const MemoryPageSize page_size =
        (flags & kTilePoolFlagLargePages) ? MemoryPageSize::Page4MB : MemoryPageSize::Page64KB;
    const uint32_t mem_flags = ToUnifiedMemoryFlags(flags);

    PhysicalAllocation phys{};
    Status st = memory_.AllocatePhysicalPages(req_bytes, page_size, mem_flags, phys);
    if (!st.ok()) {
        return st;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t pool_id = next_pool_id_++;
    PoolEntry entry{};
    entry.pool_id = pool_id;
    entry.physical_id = phys.physical_id;
    entry.tile_count = tile_count;
    entry.pool_flags = flags;
    entry.mem_flags = mem_flags;
    entry.physical_bytes = phys.size;
    entry.active_mappings = 0;

    pools_.emplace(pool_id, entry);
    total_physical_tiles_ += tile_count;
    total_physical_bytes_ += phys.size;
    out_pool_id = pool_id;
    return Status::Ok();
}

Status GpuTilePool::DestroyPool(uint64_t pool_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pools_.find(pool_id);
    if (it == pools_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown GPU tile pool id");
    }
    if (it->second.active_mappings > 0) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Cannot destroy GPU tile pool while virtual tiles are mapped to it");
    }

    Status st = memory_.FreePhysicalPages(it->second.physical_id);
    if (!st.ok()) {
        return st;
    }

    total_physical_tiles_ =
        total_physical_tiles_ >= it->second.tile_count ? (total_physical_tiles_ - it->second.tile_count) : 0;
    total_physical_bytes_ =
        total_physical_bytes_ >= it->second.physical_bytes ? (total_physical_bytes_ - it->second.physical_bytes) : 0;
    pools_.erase(it);
    return Status::Ok();
}

bool GpuTilePool::GetPoolTileCount(uint64_t pool_id, uint32_t& out_tile_count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pools_.find(pool_id);
    if (it == pools_.end()) {
        out_tile_count = 0;
        return false;
    }
    out_tile_count = it->second.tile_count;
    return true;
}

Status GpuTilePool::ReserveTiledResource(uint32_t width_tiles,
                                         uint32_t height_tiles,
                                         uint64_t& out_resource_id) {
    out_resource_id = 0;
    if (width_tiles == 0 || height_tiles == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Tiled resource dimensions must be > 0");
    }
    const uint64_t total_tiles_64 = static_cast<uint64_t>(width_tiles) * static_cast<uint64_t>(height_tiles);
    if (total_tiles_64 == 0 || total_tiles_64 > kMaxTilesPerResource) {
        return Status::Error(StatusCode::InvalidArgument, "Tiled resource tile count exceeds limit");
    }
    const size_t total_tiles = static_cast<size_t>(total_tiles_64);
    const size_t total_bytes = total_tiles * kGpuTileSizeBytes;

    void* base = nullptr;
    bool use_placeholder = false;
    if (UnifiedMemoryManager::SupportsOsPlaceholders()) {
        Status st = memory_.ReserveVirtualPlaceholder(total_bytes, base);
        if (!st.ok()) {
            return st;
        }
        use_placeholder = true;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t res_id = next_resource_id_++;
    TiledResource res{};
    res.resource_id = res_id;
    res.width_tiles = width_tiles;
    res.height_tiles = height_tiles;
    res.use_placeholder = use_placeholder;
    res.virtual_base = base;
    res.slots.resize(total_tiles);

    resources_.emplace(res_id, std::move(res));
    out_resource_id = res_id;
    return Status::Ok();
}

Status GpuTilePool::ReleaseTiledResource(uint64_t resource_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = resources_.find(resource_id);
    if (it == resources_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown tiled resource id");
    }

    TiledResource& res = it->second;
    for (auto& slot : res.slots) {
        if (slot.mapped) {
            Status st = UnmapSlotLocked(slot);
            if (!st.ok()) {
                return st;
            }
        }
    }

    if (res.use_placeholder && res.virtual_base != nullptr) {
        Status st = memory_.ReleaseVirtualPlaceholder(res.virtual_base);
        if (!st.ok()) {
            return st;
        }
        res.virtual_base = nullptr;
    }

    resources_.erase(it);
    return Status::Ok();
}

bool GpuTilePool::GetResourceDimensions(uint64_t resource_id,
                                        uint32_t& out_width_tiles,
                                        uint32_t& out_height_tiles,
                                        void*& out_virtual_base) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = resources_.find(resource_id);
    if (it == resources_.end()) {
        out_width_tiles = 0;
        out_height_tiles = 0;
        out_virtual_base = nullptr;
        return false;
    }
    out_width_tiles = it->second.width_tiles;
    out_height_tiles = it->second.height_tiles;
    out_virtual_base = it->second.virtual_base;
    return true;
}

Status GpuTilePool::UpdateTileMappings(uint64_t resource_id,
                                       GpuTileCoord tile_coord,
                                       uint64_t pool_id,
                                       uint32_t pool_tile_offset,
                                       uint32_t tile_count,
                                       uint32_t flags) {
    if (tile_count == 0) {
        return Status::Error(StatusCode::InvalidArgument, "UpdateTileMappings tile_count must be > 0");
    }
    if (tile_coord.subresource != 0) {
        return Status::Error(StatusCode::InvalidArgument, "Unsupported subresource index (only subresource 0)");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto rit = resources_.find(resource_id);
    if (rit == resources_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown tiled resource id");
    }
    TiledResource& res = rit->second;

    if (tile_coord.x >= res.width_tiles || tile_coord.y >= res.height_tiles) {
        return Status::Error(StatusCode::InvalidArgument, "Tile coordinate out of resource bounds");
    }

    const uint64_t start_idx =
        static_cast<uint64_t>(tile_coord.y) * static_cast<uint64_t>(res.width_tiles) +
        static_cast<uint64_t>(tile_coord.x);
    if (start_idx + static_cast<uint64_t>(tile_count) > res.slots.size()) {
        return Status::Error(StatusCode::InvalidArgument, "Tile mapping range exceeds tiled resource size");
    }

    const bool is_unmap = ((flags & kTileMapFlagNull) != 0u) || (pool_id == 0u);
    const bool reuse_single = (flags & kTileMapFlagReuseSingleTile) != 0u;

    PoolEntry* target_pool = nullptr;
    if (!is_unmap) {
        auto pit = pools_.find(pool_id);
        if (pit == pools_.end()) {
            return Status::Error(StatusCode::NotFound, "Unknown GPU tile pool id");
        }
        target_pool = &pit->second;

        if (reuse_single) {
            if (pool_tile_offset >= target_pool->tile_count) {
                return Status::Error(StatusCode::InvalidArgument, "Pool tile offset out of pool bounds");
            }
        } else {
            if (static_cast<uint64_t>(pool_tile_offset) + static_cast<uint64_t>(tile_count) >
                static_cast<uint64_t>(target_pool->tile_count)) {
                return Status::Error(StatusCode::InvalidArgument, "Pool tile range exceeds pool tile count");
            }
        }
    }

    const MemoryProtection prot =
        (flags & kTileMapFlagReadOnly) ? MemoryProtection::ReadOnly : MemoryProtection::ReadWrite;

    for (uint32_t i = 0; i < tile_count; ++i) {
        const size_t slot_idx = static_cast<size_t>(start_idx + i);
        VirtualTileSlot& slot = res.slots[slot_idx];

        if (slot.mapped) {
            Status unmap_st = UnmapSlotLocked(slot);
            if (!unmap_st.ok()) {
                return unmap_st;
            }
        }

        if (is_unmap) {
            continue;
        }

        const uint32_t phys_tile = reuse_single ? pool_tile_offset : (pool_tile_offset + i);
        const size_t phys_offset = static_cast<size_t>(phys_tile) * kGpuTileSizeBytes;
        void* placeholder_slot = nullptr;
        if (res.use_placeholder) {
            placeholder_slot = static_cast<uint8_t*>(res.virtual_base) + slot_idx * kGpuTileSizeBytes;
        }

        UnifiedMemoryAlias alias{};
        Status map_st = memory_.MapPhysicalAlias(target_pool->physical_id,
                                                 phys_offset,
                                                 kGpuTileSizeBytes,
                                                 prot,
                                                 target_pool->mem_flags,
                                                 placeholder_slot,
                                                 alias);
        if (!map_st.ok()) {
            return map_st;
        }

        slot.mapped = true;
        slot.pool_id = target_pool->pool_id;
        slot.pool_tile_offset = phys_tile;
        slot.virtual_address = alias.host_ptr;
        slot.gpu_va = alias.gpu_va;
        slot.flags = flags;

        ++mapped_virtual_tiles_;
        IncrementPhysicalRefLocked(target_pool->pool_id, phys_tile);
    }

    return Status::Ok();
}

Status GpuTilePool::WriteTileBytes(uint64_t resource_id,
                                   GpuTileCoord tile_coord,
                                   size_t byte_offset,
                                   const void* src,
                                   size_t byte_count) {
    if (byte_count > 0 && src == nullptr) {
        return Status::Error(StatusCode::InvalidArgument, "Null source buffer for WriteTileBytes");
    }
    if (byte_offset > kGpuTileSizeBytes || byte_count > kGpuTileSizeBytes - byte_offset) {
        return Status::Error(StatusCode::InvalidArgument, "WriteTileBytes range exceeds 64 KiB tile");
    }
    if (tile_coord.subresource != 0) {
        return Status::Error(StatusCode::InvalidArgument, "Unsupported subresource index");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto rit = resources_.find(resource_id);
    if (rit == resources_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown tiled resource id");
    }
    const TiledResource& res = rit->second;
    if (tile_coord.x >= res.width_tiles || tile_coord.y >= res.height_tiles) {
        return Status::Error(StatusCode::InvalidArgument, "Tile coordinate out of bounds");
    }

    const size_t idx =
        static_cast<size_t>(tile_coord.y) * static_cast<size_t>(res.width_tiles) +
        static_cast<size_t>(tile_coord.x);
    const VirtualTileSlot& slot = res.slots[idx];
    if (!slot.mapped || slot.virtual_address == nullptr) {
        return Status::Error(StatusCode::NotFound, "Virtual tile is not mapped (NULL tile)");
    }
    if (slot.flags & kTileMapFlagReadOnly) {
        return Status::Error(StatusCode::InvalidArgument, "Cannot write to a read-only mapped tile");
    }

    if (byte_count > 0) {
        std::memcpy(static_cast<uint8_t*>(slot.virtual_address) + byte_offset, src, byte_count);
    }
    return Status::Ok();
}

Status GpuTilePool::ReadTileBytes(uint64_t resource_id,
                                  GpuTileCoord tile_coord,
                                  size_t byte_offset,
                                  void* dst,
                                  size_t byte_count) const {
    if (byte_count > 0 && dst == nullptr) {
        return Status::Error(StatusCode::InvalidArgument, "Null destination buffer for ReadTileBytes");
    }
    if (byte_offset > kGpuTileSizeBytes || byte_count > kGpuTileSizeBytes - byte_offset) {
        return Status::Error(StatusCode::InvalidArgument, "ReadTileBytes range exceeds 64 KiB tile");
    }
    if (tile_coord.subresource != 0) {
        return Status::Error(StatusCode::InvalidArgument, "Unsupported subresource index");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto rit = resources_.find(resource_id);
    if (rit == resources_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown tiled resource id");
    }
    const TiledResource& res = rit->second;
    if (tile_coord.x >= res.width_tiles || tile_coord.y >= res.height_tiles) {
        return Status::Error(StatusCode::InvalidArgument, "Tile coordinate out of bounds");
    }

    const size_t idx =
        static_cast<size_t>(tile_coord.y) * static_cast<size_t>(res.width_tiles) +
        static_cast<size_t>(tile_coord.x);
    const VirtualTileSlot& slot = res.slots[idx];
    if (!slot.mapped || slot.virtual_address == nullptr) {
        return Status::Error(StatusCode::NotFound, "Virtual tile is not mapped (NULL tile)");
    }

    if (byte_count > 0) {
        std::memcpy(dst, static_cast<const uint8_t*>(slot.virtual_address) + byte_offset, byte_count);
    }
    return Status::Ok();
}

Status GpuTilePool::QueryTileMapping(uint64_t resource_id,
                                     GpuTileCoord tile_coord,
                                     GpuTileMappingInfo& out_info) const {
    out_info = GpuTileMappingInfo{};
    if (tile_coord.subresource != 0) {
        return Status::Error(StatusCode::InvalidArgument, "Unsupported subresource index");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto rit = resources_.find(resource_id);
    if (rit == resources_.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown tiled resource id");
    }
    const TiledResource& res = rit->second;
    if (tile_coord.x >= res.width_tiles || tile_coord.y >= res.height_tiles) {
        return Status::Error(StatusCode::InvalidArgument, "Tile coordinate out of bounds");
    }

    const size_t idx =
        static_cast<size_t>(tile_coord.y) * static_cast<size_t>(res.width_tiles) +
        static_cast<size_t>(tile_coord.x);
    const VirtualTileSlot& slot = res.slots[idx];
    out_info.mapped = slot.mapped;
    out_info.pool_id = slot.pool_id;
    out_info.pool_tile_offset = slot.pool_tile_offset;
    out_info.virtual_address = slot.virtual_address;
    out_info.gpu_va = slot.gpu_va;
    out_info.flags = slot.flags;
    return Status::Ok();
}

size_t GpuTilePool::TotalPhysicalTiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_physical_tiles_;
}

size_t GpuTilePool::MappedVirtualTiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mapped_virtual_tiles_;
}

size_t GpuTilePool::AliasedTileCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return aliased_tile_count_;
}

GpuTilePoolStats GpuTilePool::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    GpuTilePoolStats stats{};
    stats.total_pools = pools_.size();
    stats.total_resources = resources_.size();
    stats.total_physical_tiles = total_physical_tiles_;
    stats.mapped_virtual_tiles = mapped_virtual_tiles_;
    stats.aliased_tile_count = aliased_tile_count_;
    stats.physical_bytes = total_physical_bytes_;
    return stats;
}

}  // namespace scarlettx
