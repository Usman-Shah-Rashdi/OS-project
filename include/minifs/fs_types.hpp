#pragma once

// Shared on-disk structures for MiniFS.
//
// Disk layout:
//
//   | Block 0    | Blocks 1..B | Next I blocks | Remaining blocks |
//   | Superblock | Free bitmap | Inode table   | Data blocks      |
//
// Every struct here is copied byte-for-byte to and from virtual_disk.bin, so
// field order and sizes are part of the disk format. Multi-byte values are
// stored little-endian (the native order on x86/x64 Windows). Once the team
// freezes this file, any change requires a new kFsVersion.

#include <cstddef>
#include <cstdint>
#include <type_traits>

using namespace std;

namespace minifs {

// Reads as the bytes "MNFS" at offset 0 when the disk is opened in a hex editor.
constexpr uint32_t kMagic = 0x53464E4D;
constexpr uint32_t kFsVersion = 1;

constexpr uint32_t kSuperblockBlock = 0;

// Block 0 is always the superblock, so 0 can never be a data block and is
// used as the "no block" value in block pointers.
constexpr uint32_t kNullBlock = 0;

// Inode 0 is never handed out, so 0 means "no inode" in directory entries.
constexpr uint32_t kNullInode = 0;
constexpr uint32_t kRootInode = 1;

constexpr uint32_t kDirectPointers = 10;

// Format reserves one inode for every kBlocksPerInode blocks on the disk.
constexpr uint32_t kBlocksPerInode = 4;

constexpr size_t kDirNameSize = 60;                  // including the NUL
constexpr size_t kMaxNameLength = kDirNameSize - 1;  // 59 visible characters

enum class AllocationMode : uint32_t {
    Contiguous = 0,
    Linked = 1,
    Indexed = 2,
};

enum class InodeType : uint16_t {
    Free = 0,
    File = 1,
    Directory = 2,
};

// Block 0. Describes where every other region of the disk lives.
struct Superblock {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;          // bytes per block
    uint32_t total_blocks;        // blocks on the whole disk

    uint32_t bitmap_start;        // first block of the free-space bitmap
    uint32_t bitmap_blocks;
    uint32_t inode_table_start;   // first block of the inode table
    uint32_t inode_table_blocks;

    uint32_t inode_count;         // inodes in the table, including inode 0
    uint32_t inode_size;          // sizeof(Inode), stored for validation
    uint32_t data_start;          // first data block
    uint32_t data_block_count;    // total_blocks - data_start

    uint32_t free_block_count;    // free data blocks
    uint32_t free_inode_count;    // free inodes (inode 0 is never free)
    uint32_t root_inode;
    uint32_t allocation_mode;     // AllocationMode

    uint32_t journal_start;       // 0 until the journal is implemented
    uint32_t journal_blocks;
    int64_t created_at;           // seconds since the Unix epoch
    int64_t mounted_at;
};

// One entry of the inode table: metadata for a single file or directory.
struct Inode {
    uint16_t type;                     // InodeType
    uint16_t mode;                     // permission bits, e.g. 0644
    uint32_t link_count;
    uint64_t size;                     // bytes of content
    int64_t created_at;                // seconds since the Unix epoch
    int64_t modified_at;
    int64_t accessed_at;
    uint32_t direct[kDirectPointers];  // data block numbers, 0 = unused
    uint32_t indirect;                 // block of further pointers, 0 = unused
    uint8_t reserved[44];              // pads the inode to 128 bytes
};

// A directory is a file whose content is an array of these entries.
struct DirEntry {
    uint32_t inode;                    // 0 = unused slot
    char name[kDirNameSize];           // NUL-terminated
};

static_assert(sizeof(Superblock) == 88, "Superblock layout changed");
static_assert(sizeof(Inode) == 128, "Inode layout changed");
static_assert(sizeof(DirEntry) == 64, "DirEntry layout changed");
static_assert(is_trivially_copyable<Superblock>::value &&
                  is_trivially_copyable<Inode>::value &&
                  is_trivially_copyable<DirEntry>::value,
              "On-disk structs must be trivially copyable");

} // namespace minifs
