#pragma once

#include "minifs/fs_types.hpp"
#include "minifs/virtual_disk.hpp"

#include <cstddef>
#include <cstdint>


namespace minifs {

// The inode table: a fixed array of Inode records stored in consecutive
// blocks. An inode is free when its type is InodeType::Free. Inode 0 is never
// allocated so that 0 can mean "no inode".
class InodeTable {
public:
    InodeTable(VirtualDisk& disk, const Superblock& superblock);

    // Format: zero every inode-table block.
    void create();

    Inode read(uint32_t number);
    void write(uint32_t number, const Inode& inode);

    // Finds the lowest free inode, initialises it and writes it to disk.
    // Throws NoFreeInodesError when the table is full.
    uint32_t allocate(InodeType type, uint16_t mode);
    // Clears the inode. Releasing its data blocks is the caller's job.
    void free(uint32_t number);

private:
    void check_number(uint32_t number) const;
    uint32_t inodes_per_block() const;

    VirtualDisk& disk_;
    const Superblock& sb_;
};

} // namespace minifs
