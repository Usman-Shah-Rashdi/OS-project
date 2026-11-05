#include "minifs/file_system.hpp"

#include "minifs/fs_errors.hpp"

#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>

using namespace std;

namespace minifs {

namespace {

uint32_t ceil_div(uint64_t value, uint64_t divisor) {
    return static_cast<uint32_t>((value + divisor - 1) / divisor);
}

DirEntry make_entry(uint32_t inode, const string& name) {
    DirEntry entry{};
    entry.inode = inode;
    memcpy(entry.name, name.data(), name.size());
    return entry;
}

} // namespace

FileSystem::FileSystem(VirtualDisk& disk)
    : disk_(disk), bitmap_(disk, sb_), inodes_(disk, sb_) {}

void FileSystem::format(size_t disk_size, AllocationMode mode) {
    const size_t block_size = disk_.block_size();
    if (block_size < sizeof(Superblock) || block_size % sizeof(Inode) != 0) {
        throw invalid_argument("Block size must hold a superblock and a whole number of inodes.");
    }
    if (disk_size / block_size > UINT32_MAX) {
        throw invalid_argument("Disk is too large for 32-bit block numbers.");
    }

    // Work out the layout before touching the file, so a disk that is too
    // small is rejected without destroying the existing image.
    const uint32_t total_blocks = static_cast<uint32_t>(disk_size / block_size);
    const uint32_t inodes_per_block = static_cast<uint32_t>(block_size / sizeof(Inode));

    Superblock sb{};
    sb.magic = kMagic;
    sb.version = kFsVersion;
    sb.block_size = static_cast<uint32_t>(block_size);
    sb.total_blocks = total_blocks;
    sb.bitmap_start = kSuperblockBlock + 1;
    sb.bitmap_blocks = ceil_div(total_blocks, uint64_t{block_size} * 8);
    sb.inode_table_start = sb.bitmap_start + sb.bitmap_blocks;
    sb.inode_table_blocks = ceil_div(total_blocks / kBlocksPerInode, inodes_per_block);
    if (sb.inode_table_blocks == 0) {
        sb.inode_table_blocks = 1;
    }
    sb.inode_count = sb.inode_table_blocks * inodes_per_block;
    sb.inode_size = static_cast<uint32_t>(sizeof(Inode));
    sb.data_start = sb.inode_table_start + sb.inode_table_blocks;
    if (static_cast<uint64_t>(sb.data_start) + 1 > total_blocks) {
        throw invalid_argument("Disk is too small for the MiniFS metadata.");
    }
    sb.data_block_count = total_blocks - sb.data_start;
    sb.free_block_count = sb.data_block_count;
    sb.free_inode_count = sb.inode_count - 1;  // inode 0 is reserved
    sb.root_inode = kRootInode;
    sb.allocation_mode = static_cast<uint32_t>(mode);
    sb.journal_start = 0;
    sb.journal_blocks = 0;
    sb.created_at = static_cast<int64_t>(time(nullptr));
    sb.mounted_at = sb.created_at;

    mounted_ = false;
    disk_.format(disk_size);
    sb_ = sb;

    bitmap_.create();
    inodes_.create();
    write_superblock();
    mounted_ = true;

    create_root_directory();
}

void FileSystem::mount() {
    mounted_ = false;
    if (!disk_.is_open()) {
        disk_.open();
    }

    const vector<uint8_t> block = disk_.read_block(kSuperblockBlock);
    Superblock sb;
    memcpy(&sb, block.data(), sizeof(Superblock));
    sb_ = sb;
    validate_superblock();

    bitmap_.load();
    mounted_ = true;

    sb_.mounted_at = static_cast<int64_t>(time(nullptr));
    write_superblock();
}

void FileSystem::unmount() {
    mounted_ = false;
    disk_.close();
}

bool FileSystem::is_mounted() const {
    return mounted_;
}

const Superblock& FileSystem::superblock() const {
    require_mounted();
    return sb_;
}

const VirtualDisk& FileSystem::disk() const {
    return disk_;
}

uint32_t FileSystem::allocate_block() {
    require_mounted();
    const uint32_t block = bitmap_.allocate();
    --sb_.free_block_count;
    write_superblock();
    disk_.write_block(block, vector<uint8_t>(sb_.block_size, 0));
    return block;
}

void FileSystem::free_block(uint32_t block) {
    require_mounted();
    bitmap_.free(block);
    ++sb_.free_block_count;
    write_superblock();
}

bool FileSystem::is_block_used(uint32_t block) const {
    require_mounted();
    return bitmap_.is_used(block);
}

vector<uint8_t> FileSystem::read_data_block(uint32_t block) {
    require_mounted();
    check_data_block(block);
    return disk_.read_block(block);
}

void FileSystem::write_data_block(uint32_t block, const vector<uint8_t>& data) {
    require_mounted();
    check_data_block(block);
    if (!bitmap_.is_used(block)) {
        throw logic_error("Cannot write to unallocated block " + to_string(block) + ".");
    }
    disk_.write_block(block, data);
}

uint32_t FileSystem::allocate_inode(InodeType type, uint16_t mode) {
    require_mounted();
    const uint32_t number = inodes_.allocate(type, mode);
    --sb_.free_inode_count;
    write_superblock();
    return number;
}

void FileSystem::free_inode(uint32_t number) {
    require_mounted();
    inodes_.free(number);
    ++sb_.free_inode_count;
    write_superblock();
}

Inode FileSystem::read_inode(uint32_t number) {
    require_mounted();
    return inodes_.read(number);
}

void FileSystem::write_inode(uint32_t number, const Inode& inode) {
    require_mounted();
    inodes_.write(number, inode);
}

void FileSystem::create_root_directory() {
    const uint32_t root = allocate_inode(InodeType::Directory, 0755);
    if (root != kRootInode) {
        throw logic_error("Root directory did not receive inode 1.");
    }

    // The root's parent is itself, so both "." and ".." point at inode 1.
    const uint32_t block = allocate_block();
    vector<uint8_t> data(sb_.block_size, 0);
    const DirEntry entries[] = {make_entry(root, "."), make_entry(root, "..")};
    memcpy(data.data(), entries, sizeof(entries));
    write_data_block(block, data);

    Inode inode = read_inode(root);
    inode.link_count = 2;  // "." plus its own ".."
    inode.size = sizeof(entries);
    inode.direct[0] = block;
    write_inode(root, inode);
}

void FileSystem::write_superblock() {
    vector<uint8_t> block(sb_.block_size, 0);
    memcpy(block.data(), &sb_, sizeof(Superblock));
    disk_.write_block(kSuperblockBlock, block);
}

void FileSystem::validate_superblock() const {
    if (sb_.magic != kMagic) {
        throw FsError("Not a MiniFS disk (bad magic number). Run 'format' to create one.");
    }
    if (sb_.version != kFsVersion) {
        throw FsError("Unsupported MiniFS version " + to_string(sb_.version) + ".");
    }
    if (sb_.block_size != disk_.block_size() ||
        sb_.total_blocks != disk_.block_count() ||
        sb_.inode_size != sizeof(Inode)) {
        throw FsError("Superblock does not match the disk image.");
    }

    const uint32_t inodes_per_block = sb_.block_size / sb_.inode_size;
    const bool layout_ok =
        sb_.bitmap_start == kSuperblockBlock + 1 &&
        sb_.inode_table_start == sb_.bitmap_start + sb_.bitmap_blocks &&
        sb_.data_start == sb_.inode_table_start + sb_.inode_table_blocks &&
        sb_.data_start < sb_.total_blocks &&
        sb_.data_block_count == sb_.total_blocks - sb_.data_start &&
        uint64_t{sb_.bitmap_blocks} * sb_.block_size * 8 >= sb_.total_blocks &&
        sb_.inode_count == sb_.inode_table_blocks * inodes_per_block &&
        sb_.free_block_count <= sb_.data_block_count &&
        sb_.free_inode_count < sb_.inode_count &&
        sb_.root_inode == kRootInode;
    if (!layout_ok) {
        throw FsError("Superblock is corrupt (inconsistent layout).");
    }
}

void FileSystem::require_mounted() const {
    if (!mounted_) {
        throw NotMountedError();
    }
}

void FileSystem::check_data_block(uint32_t block) const {
    if (block < sb_.data_start || block >= sb_.total_blocks) {
        throw out_of_range("Block " + to_string(block) + " is not a data block.");
    }
}

const char* allocation_mode_name(AllocationMode mode) {
    switch (mode) {
    case AllocationMode::Contiguous: return "contiguous";
    case AllocationMode::Linked: return "linked";
    case AllocationMode::Indexed: return "indexed";
    }
    return "unknown";
}

} // namespace minifs
