#pragma once

#include "minifs/block_bitmap.hpp"
#include "minifs/fs_types.hpp"
#include "minifs/inode_table.hpp"
#include "minifs/virtual_disk.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace std;

namespace minifs {

// Ties the disk regions together: formats and mounts the disk, keeps the
// superblock's counters in sync, and hands out data blocks and inodes.
// Higher layers (directories, file operations, CLI) should go through this
// class rather than touching VirtualDisk directly.
class FileSystem {
public:
    struct ParentPath {
        uint32_t inode;
        string name;
    };

    explicit FileSystem(VirtualDisk& disk);

    FileSystem(const FileSystem&) = delete;
    FileSystem& operator=(const FileSystem&) = delete;

    // Creates a fresh disk image with superblock, bitmap, inode table and an
    // empty root directory, then leaves it mounted.
    void format(size_t disk_size = VirtualDisk::DefaultDiskSize,
                AllocationMode mode = AllocationMode::Indexed);
    // Opens an existing disk image and validates its superblock.
    void mount();
    void unmount();
    bool is_mounted() const;

    const Superblock& superblock() const;
    const VirtualDisk& disk() const;

    // Data blocks. A newly allocated block is zero-filled.
    uint32_t allocate_block();
    void free_block(uint32_t block);
    bool is_block_used(uint32_t block) const;
    vector<uint8_t> read_data_block(uint32_t block);
    void write_data_block(uint32_t block, const vector<uint8_t>& data);

    // Inodes.
    uint32_t allocate_inode(InodeType type, uint16_t mode);
    void free_inode(uint32_t number);
    Inode read_inode(uint32_t number);
    void write_inode(uint32_t number, const Inode& inode);

    // Directory and path operations.
    vector<DirEntry> list_directory(uint32_t directory);
    uint32_t find_directory_entry(uint32_t directory, const string& name);
    void add_directory_entry(uint32_t directory,
                             const string& name,
                             uint32_t child);
    void remove_directory_entry(uint32_t directory, const string& name);
    bool directory_empty(uint32_t directory);

    uint32_t resolve_path(const string& path);
    ParentPath resolve_parent(const string& path);
    uint32_t current_directory() const;
    string working_directory_path();
    void change_directory(const string& path);
    uint32_t make_directory(const string& path, uint16_t mode = 0755);
    uint32_t create_file(const string& path, uint16_t mode = 0644);
    void write_file(const string& path, const string& content);
    string read_file(const string& path);
    void remove_file(const string& path);
    void remove_directory(const string& path);

private:
    void create_root_directory();
    vector<string> path_components(const string& path) const;
    void require_directory(uint32_t inode);
    void validate_entry_name(const string& name) const;
    void update_directory(uint32_t directory,
                          const vector<DirEntry>& entries);
    vector<DirEntry> read_directory_entries(uint32_t directory);
    uint32_t find_directory_entry_in(uint32_t directory,
                                     const string& name);
    void write_superblock();
    void validate_superblock() const;
    void require_mounted() const;
    void check_data_block(uint32_t block) const;

    VirtualDisk& disk_;
    Superblock sb_{};
    BlockBitmap bitmap_;
    InodeTable inodes_;
    bool mounted_ = false;
    uint32_t current_directory_ = kRootInode;
};

const char* allocation_mode_name(AllocationMode mode);

} // namespace minifs
