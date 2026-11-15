#pragma once

#include "minifs/fs_types.hpp"
#include "minifs/virtual_disk.hpp"

#include <cstdint>
#include <vector>

namespace minifs {

// Free-space bitmap: one bit per disk block, 1 = used, 0 = free.
// Bits cover the whole disk, so superblock, bitmap, inode-table and journal
// blocks are marked used at format time and never handed out.
//
// The bitmap is kept in memory while mounted. Each change is written straight
// back to the bitmap block that holds the changed bit.
class BlockBitmap {
public:
    BlockBitmap(VirtualDisk& disk, const Superblock& superblock);

    // Format: mark only the metadata blocks as used and write every bitmap block.
    void create();
    // Mount: read the bitmap blocks from disk.
    void load();

    bool is_used(std::uint32_t block) const;
    // First-fit search of the data region. Throws DiskFullError when full.
    std::uint32_t allocate();
    // First-fit search for `count` consecutive free data blocks; marks them
    // used and returns the first one. Throws DiskFullError when there are not
    // enough free blocks at all, NoContiguousSpaceError when they are scattered.
    std::uint32_t allocate_run(std::uint32_t count);
    // Returns the first block of the first free run of `count` blocks, or
    // kNullBlock if there is none. Does not change anything.
    std::uint32_t find_run(std::uint32_t count) const;
    // Same, but only runs that start at or after `min_start`.
    std::uint32_t find_run_from(std::uint32_t min_start, std::uint32_t count) const;
    // Marks exactly blocks [start, start + count) used. Throws if any is used.
    void allocate_range(std::uint32_t start, std::uint32_t count);
    void free(std::uint32_t block);
    std::uint32_t count_free_data_blocks() const;

    // fsck repair: replace the whole bitmap. `used` has one entry per block.
    void assign(const std::vector<bool>& used);

private:
    void set(std::uint32_t block, bool used);
    void write_back(std::uint32_t block);
    void check_block(std::uint32_t block) const;

    VirtualDisk& disk_;
    const Superblock& sb_;
    std::vector<std::uint8_t> bits_;
};

} // namespace minifs
