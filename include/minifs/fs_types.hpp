#pragma once

// Shared on-disk structures for MiniFS.
//
// Disk layout:
//
//   | Block 0    | Blocks 1..B | Next I blocks | Next J blocks | Remaining blocks |
//   | Superblock | Free bitmap | Inode table   | Journal       | Data blocks      |
//
// Every struct here is copied byte-for-byte to and from virtual_disk.bin, so
// field order and sizes are part of the disk format. Multi-byte values are
// stored little-endian (the native order on x86/x64 Windows). Any change to
// these structs requires a new kFsVersion.

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace minifs {

// Reads as the bytes "MNFS" at offset 0 when the disk is opened in a hex editor.
constexpr std::uint32_t kMagic = 0x53464E4D;
// Version 2 added the journal region.
constexpr std::uint32_t kFsVersion = 2;

constexpr std::uint32_t kSuperblockBlock = 0;

// Block 0 is always the superblock, so 0 can never be a data block and is
// used as the "no block" value in block pointers.
constexpr std::uint32_t kNullBlock = 0;

// Inode 0 is never handed out, so 0 means "no inode" in directory entries.
constexpr std::uint32_t kNullInode = 0;
constexpr std::uint32_t kRootInode = 1;

constexpr std::uint32_t kDirectPointers = 10;

// Format reserves one inode for every kBlocksPerInode blocks on the disk.
constexpr std::uint32_t kBlocksPerInode = 4;

// The journal gets one block for every kBlocksPerJournalBlock disk blocks,
// clamped to [kMinJournalBlocks, kMaxJournalBlocks].
constexpr std::uint32_t kBlocksPerJournalBlock = 64;
constexpr std::uint32_t kMinJournalBlocks = 16;
constexpr std::uint32_t kMaxJournalBlocks = 128;

constexpr std::size_t kDirNameSize = 60;                  // including the NUL
constexpr std::size_t kMaxNameLength = kDirNameSize - 1;  // 59 visible characters

// How a file's data blocks are chosen and recorded. Fixed per disk at format.
//   Contiguous: one run of consecutive blocks; direct[0] holds the first block.
//   Linked:     each block ends with a 4-byte pointer to the next block;
//               direct[0] holds the first block.
//   Indexed:    direct[0..9] plus one single-indirect block of pointers.
enum class AllocationMode : std::uint32_t {
    Contiguous = 0,
    Linked = 1,
    Indexed = 2,
};

enum class InodeType : std::uint16_t {
    Free = 0,
    File = 1,
    Directory = 2,
};

// Block 0. Describes where every other region of the disk lives.
struct Superblock {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t block_size;          // bytes per block
    std::uint32_t total_blocks;        // blocks on the whole disk

    std::uint32_t bitmap_start;        // first block of the free-space bitmap
    std::uint32_t bitmap_blocks;
    std::uint32_t inode_table_start;   // first block of the inode table
    std::uint32_t inode_table_blocks;

    std::uint32_t inode_count;         // inodes in the table, including inode 0
    std::uint32_t inode_size;          // sizeof(Inode), stored for validation
    std::uint32_t data_start;          // first data block
    std::uint32_t data_block_count;    // total_blocks - data_start

    std::uint32_t free_block_count;    // free data blocks
    std::uint32_t free_inode_count;    // free inodes (inode 0 is never free)
    std::uint32_t root_inode;
    std::uint32_t allocation_mode;     // AllocationMode

    std::uint32_t journal_start;       // first block of the journal
    std::uint32_t journal_blocks;
    std::int64_t created_at;           // seconds since the Unix epoch
    std::int64_t mounted_at;
};

// One entry of the inode table: metadata for a single file or directory.
struct Inode {
    std::uint16_t type;                     // InodeType
    std::uint16_t mode;                     // permission bits, e.g. 0644
    std::uint32_t link_count;
    std::uint64_t size;                     // bytes of content
    std::int64_t created_at;                // seconds since the Unix epoch
    std::int64_t modified_at;
    std::int64_t accessed_at;
    std::uint32_t direct[kDirectPointers];  // see AllocationMode, 0 = unused
    std::uint32_t indirect;                 // block of further pointers, 0 = unused
    std::uint8_t reserved[44];              // pads the inode to 128 bytes
};

// A directory is a file whose content is an array of these entries.
struct DirEntry {
    std::uint32_t inode;                    // 0 = unused slot
    char name[kDirNameSize];                // NUL-terminated
};

// Reads as "JRNL" in a hex editor.
constexpr std::uint32_t kJournalMagic = 0x4C4E524A;

enum class JournalState : std::uint32_t {
    Empty = 0,      // nothing to recover
    Pending = 1,    // block images written, transaction not committed yet
    Committed = 2,  // transaction committed, may not be applied yet
};

// First block of the journal region. The array of target block numbers
// follows this header in the same block; the block images follow in the next
// journal blocks, one image per target.
struct JournalHeader {
    std::uint32_t magic;
    std::uint32_t state;          // JournalState
    std::uint32_t sequence;       // increases with every transaction
    std::uint32_t block_count;    // number of block images
    std::uint32_t checksum;       // FNV-1a over targets and images
    std::uint32_t reserved[3];
};

static_assert(sizeof(Superblock) == 88, "Superblock layout changed");
static_assert(sizeof(Inode) == 128, "Inode layout changed");
static_assert(sizeof(DirEntry) == 64, "DirEntry layout changed");
static_assert(sizeof(JournalHeader) == 32, "JournalHeader layout changed");
static_assert(std::is_trivially_copyable<Superblock>::value &&
                  std::is_trivially_copyable<Inode>::value &&
                  std::is_trivially_copyable<DirEntry>::value &&
                  std::is_trivially_copyable<JournalHeader>::value,
              "On-disk structs must be trivially copyable");

} // namespace minifs
