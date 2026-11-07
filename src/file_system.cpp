#include "minifs/file_system.hpp"

#include "minifs/fs_errors.hpp"

#include <algorithm>
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
    current_directory_ = kRootInode;

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
    current_directory_ = kRootInode;

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

vector<DirEntry> FileSystem::list_directory(uint32_t directory) {
    return read_directory_entries(directory);
}

uint32_t FileSystem::find_directory_entry(uint32_t directory,
                                           const string& name) {
    validate_entry_name(name);
    return find_directory_entry_in(directory, name);
}

void FileSystem::add_directory_entry(uint32_t directory,
                                     const string& name,
                                     uint32_t child) {
    require_mounted();
    require_directory(directory);
    validate_entry_name(name);
    if (child == kNullInode) {
        throw invalid_argument("Directory entries cannot reference inode 0.");
    }
    const vector<DirEntry> existing = read_directory_entries(directory);
    if (any_of(existing.begin(), existing.end(),
               [&name](const DirEntry& entry) {
                   return entry.inode != kNullInode &&
                          string(entry.name) == name;
               })) {
        throw logic_error("Entry '" + name + "' already exists.");
    }

    vector<DirEntry> entries = read_directory_entries(directory);
    entries.push_back(make_entry(child, name));
    update_directory(directory, entries);
}

void FileSystem::remove_directory_entry(uint32_t directory,
                                         const string& name) {
    require_mounted();
    require_directory(directory);
    validate_entry_name(name);
    if (name == "." || name == "..") {
        throw invalid_argument("The '" + name + "' directory entry cannot be removed.");
    }

    vector<DirEntry> entries = read_directory_entries(directory);
    const auto found = find_if(entries.begin(), entries.end(),
                               [&name](const DirEntry& entry) {
                                   return entry.inode != kNullInode &&
                                          string(entry.name) == name;
                               });
    if (found == entries.end()) {
        throw out_of_range("Entry '" + name + "' does not exist.");
    }
    entries.erase(found);
    update_directory(directory, entries);
}

bool FileSystem::directory_empty(uint32_t directory) {
    const vector<DirEntry> entries = read_directory_entries(directory);
    return all_of(entries.begin(), entries.end(), [](const DirEntry& entry) {
        return entry.inode == kNullInode || string(entry.name) == "." ||
               string(entry.name) == "..";
    });
}

uint32_t FileSystem::resolve_path(const string& path) {
    require_mounted();
    const vector<string> components = path_components(path);
    uint32_t current = !path.empty() && path.front() == '/'
                           ? kRootInode
                           : current_directory_;
    for (const string& component : components) {
        require_directory(current);
        if (component == ".") {
            continue;
        }
        current = find_directory_entry_in(current, component);
    }
    return current;
}

FileSystem::ParentPath FileSystem::resolve_parent(const string& path) {
    require_mounted();
    vector<string> components = path_components(path);
    if (components.empty()) {
        throw invalid_argument("The root directory has no parent entry.");
    }

    const string name = components.back();
    if (name == "." || name == "..") {
        throw invalid_argument("A parent path must end with a directory entry name.");
    }
    components.pop_back();

    uint32_t parent = !path.empty() && path.front() == '/'
                          ? kRootInode
                          : current_directory_;
    for (const string& component : components) {
        require_directory(parent);
        if (component == ".") {
            continue;
        }
        parent = find_directory_entry_in(parent, component);
    }
    require_directory(parent);
    return {parent, name};
}

uint32_t FileSystem::current_directory() const {
    require_mounted();
    return current_directory_;
}

string FileSystem::working_directory_path() {
    require_mounted();
    if (current_directory_ == kRootInode) {
        return "/";
    }

    vector<string> reversed;
    uint32_t current = current_directory_;
    while (current != kRootInode) {
        const uint32_t parent = find_directory_entry_in(current, "..");
        const vector<DirEntry> entries = read_directory_entries(parent);
        const auto found = find_if(entries.begin(), entries.end(),
                                   [current](const DirEntry& entry) {
                                       return entry.inode == current &&
                                              entry.name[0] != '\0' &&
                                              string(entry.name) != "." &&
                                              string(entry.name) != "..";
                                   });
        if (found == entries.end()) {
            throw logic_error("Directory has no name in its parent.");
        }
        reversed.emplace_back(found->name);
        current = parent;
    }

    string result;
    for (auto it = reversed.rbegin(); it != reversed.rend(); ++it) {
        result += "/" + *it;
    }
    return result.empty() ? "/" : result;
}

void FileSystem::change_directory(const string& path) {
    const uint32_t directory = resolve_path(path);
    require_directory(directory);
    current_directory_ = directory;
}

uint32_t FileSystem::make_directory(const string& path, uint16_t mode) {
    const ParentPath parent = resolve_parent(path);
    const vector<DirEntry> existing = read_directory_entries(parent.inode);
    if (any_of(existing.begin(), existing.end(),
               [&parent](const DirEntry& entry) {
                   return entry.inode != kNullInode &&
                          string(entry.name) == parent.name;
               })) {
        throw logic_error("Entry '" + parent.name + "' already exists.");
    }

    const uint32_t child = allocate_inode(InodeType::Directory, mode);
    uint32_t block = kNullBlock;
    try {
        block = allocate_block();
        vector<uint8_t> data(sb_.block_size, 0);
        const DirEntry entries[] = {make_entry(child, "."),
                                    make_entry(parent.inode, "..")};
        memcpy(data.data(), entries, sizeof(entries));
        write_data_block(block, data);

        Inode inode = read_inode(child);
        inode.link_count = 2;
        inode.size = sizeof(entries);
        inode.direct[0] = block;
        write_inode(child, inode);

        add_directory_entry(parent.inode, parent.name, child);
        Inode parent_inode = read_inode(parent.inode);
        ++parent_inode.link_count;
        write_inode(parent.inode, parent_inode);
        return child;
    } catch (const exception&) {
        if (block != kNullBlock && is_block_used(block)) {
            free_block(block);
        }
        free_inode(child);
        throw;
    }
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

vector<string> FileSystem::path_components(const string& path) const {
    if (path.empty()) {
        throw invalid_argument("Path cannot be empty.");
    }

    vector<string> components;
    size_t start = 0;
    while (start < path.size()) {
        while (start < path.size() && path[start] == '/') {
            ++start;
        }
        if (start == path.size()) {
            break;
        }
        const size_t end = path.find('/', start);
        const size_t length = end == string::npos ? path.size() - start
                                                   : end - start;
        const string component = path.substr(start, length);
        if (component.size() > kMaxNameLength) {
            throw invalid_argument("Directory entry name is too long.");
        }
        components.push_back(component);
        start = end == string::npos ? path.size() : end;
    }
    return components;
}

void FileSystem::require_directory(uint32_t inode) {
    const Inode value = read_inode(inode);
    if (value.type != static_cast<uint16_t>(InodeType::Directory)) {
        throw logic_error("Inode " + to_string(inode) + " is not a directory.");
    }
}

void FileSystem::validate_entry_name(const string& name) const {
    if (name.empty() || name == "/" || name.find('/') != string::npos) {
        throw invalid_argument("Invalid directory entry name.");
    }
    if (name.size() > kMaxNameLength) {
        throw invalid_argument("Directory entry name is too long.");
    }
}

vector<DirEntry> FileSystem::read_directory_entries(uint32_t directory) {
    require_mounted();
    require_directory(directory);
    const Inode inode = read_inode(directory);
    if (inode.size % sizeof(DirEntry) != 0) {
        throw FsError("Directory inode has an invalid size.");
    }

    const size_t count = static_cast<size_t>(inode.size / sizeof(DirEntry));
    vector<DirEntry> entries(count);
    size_t copied = 0;
    for (uint32_t pointer : inode.direct) {
        if (copied == count) {
            break;
        }
        if (pointer == kNullBlock) {
            throw FsError("Directory inode has a missing data block.");
        }
        const vector<uint8_t> block = read_data_block(pointer);
        const size_t available = min(count - copied,
                                     block.size() / sizeof(DirEntry));
        memcpy(entries.data() + copied, block.data(),
               available * sizeof(DirEntry));
        copied += available;
    }
    if (copied != count) {
        throw FsError("Directory inode has too many entries for its blocks.");
    }
    return entries;
}

uint32_t FileSystem::find_directory_entry_in(uint32_t directory,
                                             const string& name) {
    const vector<DirEntry> entries = read_directory_entries(directory);
    for (const DirEntry& entry : entries) {
        if (entry.inode != kNullInode && string(entry.name) == name) {
            return entry.inode;
        }
    }
    throw out_of_range("Entry '" + name + "' does not exist.");
}

void FileSystem::update_directory(uint32_t directory,
                                  const vector<DirEntry>& entries) {
    Inode inode = read_inode(directory);
    const size_t bytes = entries.size() * sizeof(DirEntry);
    const size_t entries_per_block = sb_.block_size / sizeof(DirEntry);
    const size_t required_blocks = (entries.size() + entries_per_block - 1) /
                                   entries_per_block;
    if (required_blocks > kDirectPointers) {
        throw invalid_argument("Directory is too large for its direct pointers.");
    }

    vector<uint32_t> allocated;
    try {
        for (size_t i = 0; i < required_blocks; ++i) {
            if (inode.direct[i] == kNullBlock) {
                inode.direct[i] = allocate_block();
                allocated.push_back(inode.direct[i]);
            }
            vector<uint8_t> block(sb_.block_size, 0);
            const size_t first = i * entries_per_block;
            const size_t count = min(entries.size() - first, entries_per_block);
            memcpy(block.data(), entries.data() + first,
                   count * sizeof(DirEntry));
            write_data_block(inode.direct[i], block);
        }
    } catch (const exception&) {
        for (uint32_t block : allocated) {
            free_block(block);
        }
        throw;
    }

    for (size_t i = required_blocks; i < kDirectPointers; ++i) {
        if (inode.direct[i] != kNullBlock) {
            free_block(inode.direct[i]);
            inode.direct[i] = kNullBlock;
        }
    }
    inode.size = bytes;
    inode.modified_at = static_cast<int64_t>(time(nullptr));
    write_inode(directory, inode);
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
