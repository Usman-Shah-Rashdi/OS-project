#pragma once

#include "minifs/block_bitmap.hpp"
#include "minifs/fs_types.hpp"
#include "minifs/inode_table.hpp"
#include "minifs/journal.hpp"
#include "minifs/virtual_disk.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

namespace minifs {

// What a disk block is used for. The character is what 'map' prints.
enum class BlockKind : char {
    Free = '.',
    Superblock = 'S',
    Bitmap = 'B',
    InodeTable = 'I',
    Journal = 'J',
    Directory = 'D',
    File = 'F',
    Pointer = 'P',   // indirect block of an indexed file
    Orphan = '?',    // marked used in the bitmap but nothing points at it
};

struct BlockOwner {
    BlockKind kind = BlockKind::Free;
    std::uint32_t inode = kNullInode;
};

struct ListEntry {
    std::string name;
    std::uint32_t inode;
    Inode meta;
};

struct FileInfo {
    std::string path;
    std::uint32_t number;
    Inode meta;
    std::vector<std::uint32_t> data_blocks;     // in file order
    std::vector<std::uint32_t> pointer_blocks;  // indirect blocks
    std::size_t extents;                        // runs of consecutive blocks
};

struct UsageStats {
    std::uint32_t block_size;
    std::uint32_t total_blocks;
    std::uint32_t metadata_blocks;  // superblock + bitmap + inode table + journal
    std::uint32_t data_blocks;
    std::uint32_t used_data_blocks;
    std::uint32_t free_data_blocks;
    std::uint32_t usable_inodes;    // inode 0 is reserved
    std::uint32_t used_inodes;
    std::uint32_t free_inodes;
};

struct FragmentedFile {
    std::string path;
    std::size_t blocks;
    std::size_t extents;
};

struct FragmentationReport {
    std::size_t entries = 0;            // files and directories with data
    std::size_t fragmented = 0;         // entries with more than one extent
    std::size_t total_extents = 0;
    std::uint64_t internal_waste = 0;   // unused bytes in last blocks
    std::uint32_t free_blocks = 0;
    std::uint32_t free_extents = 0;
    std::uint32_t largest_free_extent = 0;
    double external_fragmentation = 0;  // 1 - largest free run / free blocks
    std::vector<FragmentedFile> fragmented_files;
};

struct FsckReport {
    std::vector<std::string> problems;
    std::vector<std::string> repairs;
    bool clean() const { return problems.empty(); }
};

struct DefragResult {
    std::size_t entries_moved = 0;
    std::size_t blocks_moved = 0;
};

// Ties the disk regions together: formats and mounts the disk, keeps the
// superblock's counters in sync, stores file contents under the disk's
// allocation mode, and implements directories, files and permissions.
//
// Every operation that changes the disk runs as one journaled transaction:
// either all of its metadata changes reach the disk, or none do.
class FileSystem {
public:
    struct ParentPath {
        std::uint32_t inode;
        std::string name;
    };

    explicit FileSystem(VirtualDisk& disk);

    FileSystem(const FileSystem&) = delete;
    FileSystem& operator=(const FileSystem&) = delete;

    // Creates a fresh disk image with superblock, bitmap, inode table,
    // journal and an empty root directory, then leaves it mounted.
    void format(std::size_t disk_size = VirtualDisk::DefaultDiskSize,
                AllocationMode mode = AllocationMode::Indexed);
    // Opens an existing disk image, validates its superblock and recovers
    // the journal. The outcome is available from last_recovery().
    void mount();
    void unmount();
    // Re-reads the superblock and bitmap so changes made by another MiniFS
    // process (for example the web GUI) are seen. Remounts if the disk was
    // reformatted or holds a journal entry to recover.
    void refresh();
    bool is_mounted() const;
    const RecoveryResult& last_recovery() const;

    const Superblock& superblock() const;
    const VirtualDisk& disk() const;
    AllocationMode allocation_mode() const;
    // Bytes of file content one data block holds (linked blocks keep 4 bytes
    // for the next-block pointer).
    std::size_t block_payload() const;
    // Largest file the allocation mode can describe, in bytes (0 = disk size).
    std::uint64_t max_file_size() const;

    // Crash simulation: the next transaction that changes the disk stops at
    // `point` and throws SimulatedCrash. The FileSystem is unmounted after.
    void arm_crash(CrashPoint point);
    CrashPoint armed_crash() const;

    // Data blocks. A newly allocated block is zero-filled.
    std::uint32_t allocate_block();
    void free_block(std::uint32_t block);
    bool is_block_used(std::uint32_t block) const;
    std::vector<std::uint8_t> read_data_block(std::uint32_t block);
    void write_data_block(std::uint32_t block, const std::vector<std::uint8_t>& data);

    // Inodes.
    std::uint32_t allocate_inode(InodeType type, std::uint16_t mode);
    void free_inode(std::uint32_t number);
    Inode read_inode(std::uint32_t number);
    void write_inode(std::uint32_t number, const Inode& inode);

    // Block lists, following the disk's allocation mode.
    std::vector<std::uint32_t> data_blocks(const Inode& inode);
    std::vector<std::uint32_t> pointer_blocks(const Inode& inode) const;

    // Directory primitives.
    std::vector<DirEntry> list_directory(std::uint32_t directory);
    std::uint32_t find_directory_entry(std::uint32_t directory,
                                       const std::string& name);
    void add_directory_entry(std::uint32_t directory, const std::string& name,
                             std::uint32_t child);
    void remove_directory_entry(std::uint32_t directory, const std::string& name);
    bool directory_empty(std::uint32_t directory);

    // Paths and the working directory.
    std::uint32_t resolve_path(const std::string& path);
    ParentPath resolve_parent(const std::string& path);
    std::uint32_t current_directory() const;
    std::string working_directory_path();
    void change_directory(const std::string& path);

    // File and directory operations (permission-checked).
    std::vector<ListEntry> list(const std::string& path);
    std::uint32_t make_directory(const std::string& path, std::uint16_t mode = 0755);
    std::uint32_t create_file(const std::string& path, std::uint16_t mode = 0644);
    void write_file(const std::string& path, const std::string& content);
    void append_file(const std::string& path, const std::string& content);
    std::string read_file(const std::string& path);
    void remove_file(const std::string& path);
    void remove_directory(const std::string& path);
    void rename_entry(const std::string& old_path, const std::string& new_path);
    // Like rename, but a destination that is an existing directory means
    // "move into that directory, keeping the name".
    void move_entry(const std::string& source, const std::string& destination);
    // Copies a file. The destination may be an existing directory.
    void copy_file(const std::string& source, const std::string& destination);
    void change_mode(const std::string& path, std::uint16_t mode);

    // Inspection (fs_inspect.cpp).
    // Every inode that is not free, read one inode-table block at a time.
    std::vector<std::pair<std::uint32_t, Inode>> allocated_inodes();
    FileInfo stat(const std::string& path);
    UsageStats usage() const;
    FragmentationReport fragmentation();
    std::vector<BlockOwner> block_map();
    std::map<std::uint32_t, std::string> inode_paths();
    void export_json(std::ostream& output);
    FsckReport fsck(bool repair);
    DefragResult defrag();

private:
    enum class Placement {
        InPlace,  // reuse the current blocks where possible (directories)
        Fresh,    // always write to newly allocated blocks (file data)
        Compact,  // newly allocated, one contiguous run if possible (defrag)
    };

    void atomic(const std::function<void()>& body);
    void reload_state();
    void apply_pending_frees();

    std::uint32_t allocate_raw_block();
    std::vector<std::uint8_t> read_content(const Inode& inode);
    void store_content(Inode& inode, const std::vector<std::uint8_t>& bytes,
                       Placement placement);
    void release_content(Inode& inode);
    // defrag: rewrites an inode's content starting exactly at `target`.
    void relocate(std::uint32_t number, std::uint32_t target);

    void create_root_directory();
    std::vector<std::string> path_components(const std::string& path) const;
    std::uint32_t lookup(std::uint32_t directory, const std::string& name,
                         const std::string& shown_path);
    std::uint32_t find_entry_or_null(std::uint32_t directory,
                                     const std::string& name);
    void require_directory(std::uint32_t inode);
    void require_permission(const Inode& inode, std::uint16_t bit,
                            const std::string& path, const char* action) const;
    void validate_entry_name(const std::string& name) const;
    void update_directory(std::uint32_t directory,
                          const std::vector<DirEntry>& entries);
    std::vector<DirEntry> read_directory_entries(std::uint32_t directory);
    std::uint32_t find_directory_entry_in(std::uint32_t directory,
                                          const std::string& name);
    std::string join_path(const std::string& directory,
                          const std::string& name) const;
    void write_superblock();
    void validate_superblock() const;
    void require_mounted() const;
    void check_data_block(std::uint32_t block) const;

    VirtualDisk& disk_;
    Superblock sb_{};
    BlockBitmap bitmap_;
    InodeTable inodes_;
    Journal journal_;
    bool mounted_ = false;
    bool in_transaction_ = false;
    std::vector<std::uint32_t> pending_frees_;
    CrashPoint armed_crash_ = CrashPoint::None;
    // Where Placement::Compact must put the next content (defrag), or 0.
    std::uint32_t compact_target_ = kNullBlock;
    RecoveryResult last_recovery_;
    std::uint32_t current_directory_ = kRootInode;
};

const char* allocation_mode_name(AllocationMode mode);
// Accepts "contiguous", "linked" or "indexed". Returns false if unknown.
bool parse_allocation_mode(const std::string& text, AllocationMode& mode);
// "rwxr-xr-x" for 0755.
std::string permission_string(std::uint16_t mode);
// Counts runs of consecutive block numbers, e.g. {5,6,7,9} has 2 extents.
std::size_t count_extents(const std::vector<std::uint32_t>& blocks);
// "5-7, 9" for {5,6,7,9}.
std::string block_ranges(const std::vector<std::uint32_t>& blocks);

} // namespace minifs
