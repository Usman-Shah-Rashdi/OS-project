#pragma once

#include "minifs/fs_types.hpp"
#include "minifs/virtual_disk.hpp"

#include <cstdint>
#include <vector>

using namespace std;

namespace minifs {

// Free-space bitmap: one bit per disk block, 1 = used, 0 = free.
// Bits cover the whole disk, so superblock, bitmap and inode-table blocks are
// marked used at format time and never handed out.
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

    bool is_used(uint32_t block) const;
    // First-fit search of the data region. Throws DiskFullError when full.
    uint32_t allocate();
    void free(uint32_t block);
    uint32_t count_free_data_blocks() const;

private:
    void set(uint32_t block, bool used);
    void write_back(uint32_t block);
    void check_block(uint32_t block) const;

    VirtualDisk& disk_;
    const Superblock& sb_;
    vector<uint8_t> bits_;
};

} // namespace minifs
