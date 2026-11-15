#include "minifs/file_system.hpp"

#include "minifs/fs_errors.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <set>
#include <stdexcept>
#include <string>

using namespace std;

namespace minifs {

namespace {

uint32_t ceil_div(uint64_t value, uint64_t divisor) {
    return static_cast<uint32_t>((value + divisor - 1) / divisor);
}

int64_t now() {
    return static_cast<int64_t>(time(nullptr));
}

DirEntry make_entry(uint32_t inode, const string& name) {
    DirEntry entry{};
    entry.inode = inode;
    memcpy(entry.name, name.data(), min(name.size(), kMaxNameLength));
    return entry;
}

// Directory names are NUL-terminated, but never read past the field even if
// the disk is corrupt.
string entry_name(const DirEntry& entry) {
    return string(entry.name, strnlen(entry.name, kDirNameSize));
}

bool is_dot_entry(const DirEntry& entry) {
    const string name = entry_name(entry);
    return name == "." || name == "..";
}

vector<uint8_t> entries_to_bytes(const vector<DirEntry>& entries) {
    vector<uint8_t> bytes(entries.size() * sizeof(DirEntry));
    if (!entries.empty()) {
        memcpy(bytes.data(), entries.data(), bytes.size());
    }
    return bytes;
}

bool is_type(const Inode& inode, InodeType type) {
    return inode.type == static_cast<uint16_t>(type);
}

} // namespace

// ---------------------------------------------------------------------------
// Format, mount and transactions
// ---------------------------------------------------------------------------

FileSystem::FileSystem(VirtualDisk& disk)
    : disk_(disk), bitmap_(disk, sb_), inodes_(disk, sb_), journal_(disk, sb_) {}

void FileSystem::format(size_t disk_size, AllocationMode mode) {
    const size_t block_size = disk_.block_size();
    if (block_size < sizeof(Superblock) || block_size % sizeof(Inode) != 0) {
        throw invalid_argument("Block size must hold a superblock and a whole number of inodes.");
    }
    if (disk_size / block_size > UINT32_MAX) {
        throw invalid_argument("Disk is too large for 32-bit block numbers.");
    }
    if (static_cast<uint32_t>(mode) > static_cast<uint32_t>(AllocationMode::Indexed)) {
        throw invalid_argument("Unknown allocation mode.");
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
    sb.inode_table_blocks =
        max<uint32_t>(1, ceil_div(total_blocks / kBlocksPerInode, inodes_per_block));
    sb.inode_count = sb.inode_table_blocks * inodes_per_block;
    sb.inode_size = static_cast<uint32_t>(sizeof(Inode));
    sb.journal_start = sb.inode_table_start + sb.inode_table_blocks;
    sb.journal_blocks = min(kMaxJournalBlocks,
                            max(kMinJournalBlocks, total_blocks / kBlocksPerJournalBlock));
    sb.data_start = sb.journal_start + sb.journal_blocks;
    if (static_cast<uint64_t>(sb.data_start) + 1 > total_blocks) {
        throw invalid_argument("Disk is too small for the MiniFS metadata.");
    }
    sb.data_block_count = total_blocks - sb.data_start;
    sb.free_block_count = sb.data_block_count;
    sb.free_inode_count = sb.inode_count - 1;  // inode 0 is reserved
    sb.root_inode = kRootInode;
    sb.allocation_mode = static_cast<uint32_t>(mode);
    sb.created_at = now();
    sb.mounted_at = sb.created_at;

    mounted_ = false;
    in_transaction_ = false;
    pending_frees_.clear();
    armed_crash_ = CrashPoint::None;
    last_recovery_ = RecoveryResult{};
    disk_.format(disk_size);
    sb_ = sb;

    bitmap_.create();
    inodes_.create();
    journal_.create();
    write_superblock();
    mounted_ = true;
    current_directory_ = kRootInode;

    create_root_directory();
}

void FileSystem::mount() {
    mounted_ = false;
    in_transaction_ = false;
    pending_frees_.clear();
    armed_crash_ = CrashPoint::None;
    if (!disk_.is_open()) {
        disk_.open();
    }

    const auto load_superblock = [this] {
        const vector<uint8_t> block = disk_.read_block(kSuperblockBlock);
        memcpy(&sb_, block.data(), sizeof(Superblock));
        validate_superblock();
    };
    load_superblock();

    // A replayed transaction may have rewritten the superblock itself.
    last_recovery_ = journal_.recover();
    if (last_recovery_.action == RecoveryResult::Action::Replayed) {
        load_superblock();
    }

    bitmap_.load();
    mounted_ = true;
    current_directory_ = kRootInode;

    sb_.mounted_at = now();
    write_superblock();
}

void FileSystem::unmount() {
    mounted_ = false;
    in_transaction_ = false;
    pending_frees_.clear();
    disk_.close();
}

void FileSystem::refresh() {
    require_mounted();
    if (in_transaction_) {
        return;
    }
    Superblock on_disk;
    const vector<uint8_t> block = disk_.read_block(kSuperblockBlock);
    memcpy(&on_disk, block.data(), sizeof(Superblock));
    const JournalHeader journal = journal_.read_header();
    if (on_disk.magic != kMagic || on_disk.created_at != sb_.created_at ||
        journal.state != static_cast<uint32_t>(JournalState::Empty)) {
        disk_.close();  // reformatted (maybe resized) or needs recovery
        mount();
        return;
    }
    sb_ = on_disk;
    bitmap_.load();
    try {
        if (inodes_.read(current_directory_).type != static_cast<uint16_t>(InodeType::Directory)) {
            current_directory_ = kRootInode;
        }
    } catch (const exception&) {
        current_directory_ = kRootInode;
    }
}

bool FileSystem::is_mounted() const {
    return mounted_;
}

const RecoveryResult& FileSystem::last_recovery() const {
    return last_recovery_;
}

const Superblock& FileSystem::superblock() const {
    require_mounted();
    return sb_;
}

const VirtualDisk& FileSystem::disk() const {
    return disk_;
}

AllocationMode FileSystem::allocation_mode() const {
    return static_cast<AllocationMode>(sb_.allocation_mode);
}

size_t FileSystem::block_payload() const {
    return allocation_mode() == AllocationMode::Linked
               ? sb_.block_size - sizeof(uint32_t)
               : sb_.block_size;
}

uint64_t FileSystem::max_file_size() const {
    if (allocation_mode() != AllocationMode::Indexed) {
        return 0;
    }
    const uint64_t pointers_per_block = sb_.block_size / sizeof(uint32_t);
    return (kDirectPointers + pointers_per_block) * sb_.block_size;
}

void FileSystem::arm_crash(CrashPoint point) {
    require_mounted();
    armed_crash_ = point;
}

CrashPoint FileSystem::armed_crash() const {
    return armed_crash_;
}

// Runs `body` as one transaction. Metadata writes are buffered in memory and
// committed through the journal at the end; file data goes straight to newly
// allocated blocks, which nothing on disk points at until the commit.
// If `body` throws, the buffered writes are dropped and the in-memory state is
// reloaded, so a failed command leaves the disk exactly as it was.
void FileSystem::atomic(const function<void()>& body) {
    require_mounted();
    if (in_transaction_) {
        body();  // part of the enclosing transaction
        return;
    }

    in_transaction_ = true;
    pending_frees_.clear();
    disk_.begin_buffering();
    try {
        body();
        apply_pending_frees();
        in_transaction_ = false;
        const auto blocks = disk_.take_buffer();
        if (blocks.empty()) {
            return;
        }
        const CrashPoint crash = armed_crash_;
        armed_crash_ = CrashPoint::None;
        journal_.commit(blocks, crash);
    } catch (const SimulatedCrash&) {
        // Power is gone: write nothing more.
        in_transaction_ = false;
        pending_frees_.clear();
        mounted_ = false;
        disk_.close();
        throw;
    } catch (...) {
        in_transaction_ = false;
        pending_frees_.clear();
        disk_.discard_buffer();
        try {
            reload_state();
        } catch (...) {
            mounted_ = false;
        }
        throw;
    }
}

void FileSystem::reload_state() {
    const vector<uint8_t> block = disk_.read_block(kSuperblockBlock);
    memcpy(&sb_, block.data(), sizeof(Superblock));
    bitmap_.load();
}

// Blocks freed during a transaction are released only at its end, so they
// cannot be handed out again (and overwritten) before the commit makes the
// free permanent.
void FileSystem::apply_pending_frees() {
    if (pending_frees_.empty()) {
        return;
    }
    for (uint32_t block : pending_frees_) {
        bitmap_.free(block);
        ++sb_.free_block_count;
    }
    pending_frees_.clear();
    write_superblock();
}

// ---------------------------------------------------------------------------
// Blocks and inodes
// ---------------------------------------------------------------------------

uint32_t FileSystem::allocate_block() {
    const uint32_t block = allocate_raw_block();
    disk_.write_block_direct(block, vector<uint8_t>(sb_.block_size, 0));
    return block;
}

uint32_t FileSystem::allocate_raw_block() {
    require_mounted();
    const uint32_t block = bitmap_.allocate();
    --sb_.free_block_count;
    write_superblock();
    return block;
}

void FileSystem::free_block(uint32_t block) {
    require_mounted();
    if (!in_transaction_) {
        bitmap_.free(block);
        ++sb_.free_block_count;
        write_superblock();
        return;
    }
    if (block < sb_.data_start) {
        throw invalid_argument("Cannot free metadata block " + to_string(block) + ".");
    }
    if (!bitmap_.is_used(block) ||
        find(pending_frees_.begin(), pending_frees_.end(), block) != pending_frees_.end()) {
        throw logic_error("Block " + to_string(block) + " is already free.");
    }
    pending_frees_.push_back(block);
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

// ---------------------------------------------------------------------------
// File contents under the three allocation modes
// ---------------------------------------------------------------------------

vector<uint32_t> FileSystem::data_blocks(const Inode& inode) {
    const size_t payload = block_payload();
    const size_t count = static_cast<size_t>((inode.size + payload - 1) / payload);
    vector<uint32_t> blocks;
    if (count == 0) {
        return blocks;
    }
    if (count > sb_.data_block_count) {
        throw FsError("Inode size is larger than the disk.");
    }
    const auto valid = [this](uint32_t block) {
        return block >= sb_.data_start && block < sb_.total_blocks;
    };
    blocks.reserve(count);

    switch (allocation_mode()) {
    case AllocationMode::Contiguous: {
        const uint32_t start = inode.direct[0];
        if (!valid(start) || uint64_t{start} + count > sb_.total_blocks) {
            throw FsError("Inode has a bad contiguous block range.");
        }
        for (uint32_t i = 0; i < count; ++i) {
            blocks.push_back(start + i);
        }
        break;
    }
    case AllocationMode::Linked: {
        uint32_t block = inode.direct[0];
        for (size_t i = 0; i < count; ++i) {
            if (!valid(block)) {
                throw FsError("Inode has a broken block chain.");
            }
            blocks.push_back(block);
            if (i + 1 < count) {
                const vector<uint8_t> data = disk_.read_block(block);
                memcpy(&block, data.data() + block_payload(), sizeof(block));
            }
        }
        break;
    }
    case AllocationMode::Indexed: {
        const uint64_t limit = max_file_size() / sb_.block_size;
        if (count > limit) {
            throw FsError("Inode size is larger than its pointers allow.");
        }
        for (size_t i = 0; i < count && i < kDirectPointers; ++i) {
            if (!valid(inode.direct[i])) {
                throw FsError("Inode has a bad direct block pointer.");
            }
            blocks.push_back(inode.direct[i]);
        }
        if (count > kDirectPointers) {
            if (!valid(inode.indirect)) {
                throw FsError("Inode has a bad indirect block pointer.");
            }
            const vector<uint8_t> data = disk_.read_block(inode.indirect);
            const auto* pointers = reinterpret_cast<const uint32_t*>(data.data());
            for (size_t i = kDirectPointers; i < count; ++i) {
                const uint32_t block = pointers[i - kDirectPointers];
                if (!valid(block)) {
                    throw FsError("Indirect block holds a bad pointer.");
                }
                blocks.push_back(block);
            }
        }
        break;
    }
    }
    return blocks;
}

vector<uint32_t> FileSystem::pointer_blocks(const Inode& inode) const {
    if (allocation_mode() == AllocationMode::Indexed && inode.indirect != kNullBlock) {
        return {inode.indirect};
    }
    return {};
}

vector<uint8_t> FileSystem::read_content(const Inode& inode) {
    const size_t payload = block_payload();
    vector<uint8_t> bytes;
    bytes.reserve(static_cast<size_t>(inode.size));
    size_t remaining = static_cast<size_t>(inode.size);
    for (uint32_t block : data_blocks(inode)) {
        const vector<uint8_t> data = disk_.read_block(block);
        const size_t count = min(remaining, payload);
        bytes.insert(bytes.end(), data.begin(), data.begin() + static_cast<ptrdiff_t>(count));
        remaining -= count;
    }
    return bytes;
}

// Replaces the content of `inode` (the caller writes the inode afterwards).
// New blocks are chosen by the allocation mode; old blocks that are no longer
// needed are freed.
void FileSystem::store_content(Inode& inode, const vector<uint8_t>& bytes,
                               Placement placement) {
    const AllocationMode mode = allocation_mode();
    const size_t block_size = sb_.block_size;
    const size_t payload = block_payload();
    const size_t needed = (bytes.size() + payload - 1) / payload;

    const uint64_t limit = max_file_size();
    if (limit != 0 && bytes.size() > limit) {
        throw FileTooLargeError(
            "File too large: indexed allocation holds at most " +
            to_string(limit) + " bytes (" + to_string(kDirectPointers) +
            " direct + " + to_string(block_size / sizeof(uint32_t)) +
            " indirect blocks).");
    }
    const vector<uint32_t> old_blocks = data_blocks(inode);
    const uint32_t old_indirect =
        mode == AllocationMode::Indexed ? inode.indirect : kNullBlock;

    // Keep a prefix of the current blocks when updating in place. A
    // contiguous file that grows needs a new run instead.
    vector<uint32_t> blocks;
    if (placement == Placement::InPlace &&
        (mode != AllocationMode::Contiguous || needed <= old_blocks.size())) {
        blocks.assign(old_blocks.begin(),
                      old_blocks.begin() + static_cast<ptrdiff_t>(min(needed, old_blocks.size())));
    }
    const size_t reused = blocks.size();
    const size_t missing = needed - reused;
    const bool needs_pointer = mode == AllocationMode::Indexed && needed > kDirectPointers;
    uint32_t reserved_pointer = kNullBlock;

    if (missing > 0) {
        uint32_t run_start = kNullBlock;
        if (placement == Placement::Compact) {
            // Data run plus the indirect block right after it, at the
            // requested place (defrag) or the first run that fits.
            const uint32_t span = static_cast<uint32_t>(missing) + (needs_pointer ? 1 : 0);
            const uint32_t start =
                compact_target_ != kNullBlock ? compact_target_ : bitmap_.find_run(span);
            if (start != kNullBlock) {
                bitmap_.allocate_range(start, span);
                sb_.free_block_count -= span;
                write_superblock();
                run_start = start;
                if (needs_pointer) {
                    reserved_pointer = start + static_cast<uint32_t>(missing);
                }
            }
        }
        if (run_start == kNullBlock && mode == AllocationMode::Contiguous) {
            run_start = bitmap_.allocate_run(static_cast<uint32_t>(missing));
            sb_.free_block_count -= static_cast<uint32_t>(missing);
            write_superblock();
        }
        if (run_start != kNullBlock) {
            for (uint32_t i = 0; i < missing; ++i) {
                blocks.push_back(run_start + i);
            }
        } else {
            for (size_t i = 0; i < missing; ++i) {
                blocks.push_back(allocate_raw_block());
            }
        }
    }

    for (size_t i = 0; i < needed; ++i) {
        vector<uint8_t> data(block_size, 0);
        const size_t offset = i * payload;
        memcpy(data.data(), bytes.data() + offset, min(payload, bytes.size() - offset));
        if (mode == AllocationMode::Linked) {
            const uint32_t next = i + 1 < needed ? blocks[i + 1] : kNullBlock;
            memcpy(data.data() + payload, &next, sizeof(next));
        }
        // Reused blocks are live metadata, so they go through the journal.
        // New blocks are unreachable until the commit and can be written now.
        if (i < reused) {
            disk_.write_block(blocks[i], data);
        } else {
            disk_.write_block_direct(blocks[i], data);
        }
    }

    memset(inode.direct, 0, sizeof(inode.direct));
    uint32_t new_indirect = kNullBlock;
    if (mode == AllocationMode::Indexed) {
        for (size_t i = 0; i < needed && i < kDirectPointers; ++i) {
            inode.direct[i] = blocks[i];
        }
        if (needed > kDirectPointers) {
            vector<uint8_t> pointers(block_size, 0);
            memcpy(pointers.data(), blocks.data() + kDirectPointers,
                   (needed - kDirectPointers) * sizeof(uint32_t));
            if (placement == Placement::InPlace && old_indirect != kNullBlock) {
                new_indirect = old_indirect;
                disk_.write_block(new_indirect, pointers);
            } else if (reserved_pointer != kNullBlock) {
                new_indirect = reserved_pointer;
                disk_.write_block_direct(new_indirect, pointers);
            } else {
                new_indirect = allocate_raw_block();
                disk_.write_block_direct(new_indirect, pointers);
            }
        }
    } else if (needed > 0) {
        inode.direct[0] = blocks[0];
    }
    inode.indirect = new_indirect;
    inode.size = bytes.size();

    const set<uint32_t> keep(blocks.begin(), blocks.end());
    for (uint32_t block : old_blocks) {
        if (keep.count(block) == 0) {
            free_block(block);
        }
    }
    if (old_indirect != kNullBlock && old_indirect != new_indirect) {
        free_block(old_indirect);
    }
}

void FileSystem::release_content(Inode& inode) {
    for (uint32_t block : data_blocks(inode)) {
        free_block(block);
    }
    for (uint32_t block : pointer_blocks(inode)) {
        free_block(block);
    }
    memset(inode.direct, 0, sizeof(inode.direct));
    inode.indirect = kNullBlock;
    inode.size = 0;
}

// ---------------------------------------------------------------------------
// Directories
// ---------------------------------------------------------------------------

vector<DirEntry> FileSystem::list_directory(uint32_t directory) {
    return read_directory_entries(directory);
}

uint32_t FileSystem::find_directory_entry(uint32_t directory, const string& name) {
    validate_entry_name(name);
    return find_directory_entry_in(directory, name);
}

void FileSystem::add_directory_entry(uint32_t directory, const string& name,
                                     uint32_t child) {
    atomic([&] {
        require_directory(directory);
        validate_entry_name(name);
        if (child == kNullInode) {
            throw invalid_argument("Directory entries cannot reference inode 0.");
        }
        vector<DirEntry> entries = read_directory_entries(directory);
        for (const DirEntry& entry : entries) {
            if (entry.inode != kNullInode && entry_name(entry) == name) {
                throw logic_error("'" + name + "' already exists.");
            }
        }
        entries.push_back(make_entry(child, name));
        update_directory(directory, entries);
    });
}

void FileSystem::remove_directory_entry(uint32_t directory, const string& name) {
    atomic([&] {
        require_directory(directory);
        validate_entry_name(name);
        if (name == "." || name == "..") {
            throw invalid_argument("The '" + name + "' directory entry cannot be removed.");
        }
        vector<DirEntry> entries = read_directory_entries(directory);
        const auto found = find_if(entries.begin(), entries.end(), [&name](const DirEntry& entry) {
            return entry.inode != kNullInode && entry_name(entry) == name;
        });
        if (found == entries.end()) {
            throw out_of_range("No such file or directory: '" + name + "'.");
        }
        entries.erase(found);
        update_directory(directory, entries);
    });
}

bool FileSystem::directory_empty(uint32_t directory) {
    const vector<DirEntry> entries = read_directory_entries(directory);
    return all_of(entries.begin(), entries.end(), [](const DirEntry& entry) {
        return entry.inode == kNullInode || is_dot_entry(entry);
    });
}

vector<DirEntry> FileSystem::read_directory_entries(uint32_t directory) {
    require_mounted();
    const Inode inode = read_inode(directory);
    if (!is_type(inode, InodeType::Directory)) {
        throw logic_error("Inode " + to_string(directory) + " is not a directory.");
    }
    if (inode.size % sizeof(DirEntry) != 0) {
        throw FsError("Directory inode " + to_string(directory) + " has an invalid size.");
    }
    const vector<uint8_t> bytes = read_content(inode);
    vector<DirEntry> entries(bytes.size() / sizeof(DirEntry));
    if (!entries.empty()) {
        memcpy(entries.data(), bytes.data(), entries.size() * sizeof(DirEntry));
    }
    return entries;
}

void FileSystem::update_directory(uint32_t directory, const vector<DirEntry>& entries) {
    Inode inode = read_inode(directory);
    store_content(inode, entries_to_bytes(entries), Placement::InPlace);
    inode.modified_at = now();
    write_inode(directory, inode);
}

uint32_t FileSystem::find_entry_or_null(uint32_t directory, const string& name) {
    for (const DirEntry& entry : read_directory_entries(directory)) {
        if (entry.inode != kNullInode && entry_name(entry) == name) {
            return entry.inode;
        }
    }
    return kNullInode;
}

uint32_t FileSystem::find_directory_entry_in(uint32_t directory, const string& name) {
    const uint32_t found = find_entry_or_null(directory, name);
    if (found == kNullInode) {
        throw out_of_range("No such file or directory: '" + name + "'.");
    }
    return found;
}

void FileSystem::create_root_directory() {
    const uint32_t root = allocate_inode(InodeType::Directory, 0755);
    if (root != kRootInode) {
        throw logic_error("Root directory did not receive inode 1.");
    }
    // The root's parent is itself, so both "." and ".." point at inode 1.
    Inode inode = read_inode(root);
    store_content(inode, entries_to_bytes({make_entry(root, "."), make_entry(root, "..")}),
                  Placement::InPlace);
    inode.link_count = 2;  // "." plus its own ".."
    write_inode(root, inode);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

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
        const size_t length = end == string::npos ? path.size() - start : end - start;
        const string component = path.substr(start, length);
        if (component.size() > kMaxNameLength) {
            throw invalid_argument("Name '" + component.substr(0, 20) +
                                   "...' is too long (at most " +
                                   to_string(kMaxNameLength) + " characters).");
        }
        components.push_back(component);
        start = end == string::npos ? path.size() : end;
    }
    return components;
}

// Looks `name` up in `directory`. Searching a directory needs its execute bit.
uint32_t FileSystem::lookup(uint32_t directory, const string& name,
                            const string& shown_path) {
    const Inode inode = read_inode(directory);
    const size_t slash = shown_path.find_last_of('/');
    const string shown_parent = slash == string::npos ? "."
                                : slash == 0          ? "/"
                                                      : shown_path.substr(0, slash);
    if (!is_type(inode, InodeType::Directory)) {
        throw logic_error("Not a directory: '" + shown_parent + "'.");
    }
    require_permission(inode, 01, shown_parent, "search");
    const uint32_t found = find_entry_or_null(directory, name);
    if (found == kNullInode) {
        throw out_of_range("No such file or directory: '" + shown_path + "'.");
    }
    return found;
}

uint32_t FileSystem::resolve_path(const string& path) {
    require_mounted();
    const vector<string> components = path_components(path);
    const bool absolute = path.front() == '/';
    uint32_t current = absolute ? kRootInode : current_directory_;
    string shown = absolute ? "" : ".";
    for (const string& component : components) {
        shown += "/" + component;
        if (component == ".") {
            if (!is_type(read_inode(current), InodeType::Directory)) {
                throw logic_error("Not a directory: '" + shown.substr(0, shown.size() - 2) + "'.");
            }
            continue;
        }
        current = lookup(current, component, absolute ? shown : shown.substr(2));
    }
    return current;
}

FileSystem::ParentPath FileSystem::resolve_parent(const string& path) {
    require_mounted();
    const vector<string> components = path_components(path);
    if (components.empty()) {
        throw invalid_argument("The root directory has no parent entry.");
    }
    const string name = components.back();
    if (name == "." || name == "..") {
        throw invalid_argument("'" + path + "' must end with a name, not '" + name + "'.");
    }

    string trimmed = path;
    while (trimmed.size() > 1 && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    const size_t last_slash = trimmed.find_last_of('/');
    string parent_path;
    if (last_slash == string::npos) {
        parent_path = ".";
    } else {
        parent_path = trimmed.substr(0, last_slash);
        while (!parent_path.empty() && parent_path.back() == '/') {
            parent_path.pop_back();
        }
        if (parent_path.empty()) {
            parent_path = "/";
        }
    }
    const uint32_t parent = resolve_path(parent_path);
    const Inode inode = read_inode(parent);
    if (!is_type(inode, InodeType::Directory)) {
        throw logic_error("Not a directory: '" + parent_path + "'.");
    }
    require_permission(inode, 01, parent_path, "search");
    return {parent, name};
}

uint32_t FileSystem::current_directory() const {
    require_mounted();
    return current_directory_;
}

string FileSystem::working_directory_path() {
    require_mounted();
    vector<string> reversed;
    uint32_t current = current_directory_;
    for (uint32_t depth = 0; current != kRootInode; ++depth) {
        if (depth > sb_.inode_count) {
            throw FsError("Directory tree has a loop.");
        }
        const uint32_t parent = find_directory_entry_in(current, "..");
        const vector<DirEntry> entries = read_directory_entries(parent);
        const auto found = find_if(entries.begin(), entries.end(), [current](const DirEntry& entry) {
            return entry.inode == current && !is_dot_entry(entry) && entry.name[0] != '\0';
        });
        if (found == entries.end()) {
            throw FsError("Directory has no name in its parent.");
        }
        reversed.push_back(entry_name(*found));
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
    const Inode inode = read_inode(directory);
    if (!is_type(inode, InodeType::Directory)) {
        throw logic_error("Not a directory: '" + path + "'.");
    }
    require_permission(inode, 01, path, "enter");
    current_directory_ = directory;
}

string FileSystem::join_path(const string& directory, const string& name) const {
    if (directory.empty() || directory.back() == '/') {
        return directory + name;
    }
    return directory + "/" + name;
}

// ---------------------------------------------------------------------------
// File and directory operations
// ---------------------------------------------------------------------------

vector<ListEntry> FileSystem::list(const string& path) {
    const uint32_t number = resolve_path(path);
    const Inode inode = read_inode(number);
    if (is_type(inode, InodeType::File)) {
        const size_t slash = path.find_last_of('/');
        return {{slash == string::npos ? path : path.substr(slash + 1), number, inode}};
    }
    require_permission(inode, 04, path, "list");

    vector<ListEntry> result;
    for (const DirEntry& entry : read_directory_entries(number)) {
        if (entry.inode != kNullInode && !is_dot_entry(entry)) {
            result.push_back({entry_name(entry), entry.inode, read_inode(entry.inode)});
        }
    }
    sort(result.begin(), result.end(),
         [](const ListEntry& a, const ListEntry& b) { return a.name < b.name; });
    return result;
}

uint32_t FileSystem::make_directory(const string& path, uint16_t mode) {
    uint32_t child = kNullInode;
    atomic([&] {
        const ParentPath parent = resolve_parent(path);
        validate_entry_name(parent.name);
        require_permission(read_inode(parent.inode), 02, path, "create");
        if (find_entry_or_null(parent.inode, parent.name) != kNullInode) {
            throw logic_error("'" + path + "' already exists.");
        }

        child = allocate_inode(InodeType::Directory, mode);
        Inode inode = read_inode(child);
        store_content(inode,
                      entries_to_bytes({make_entry(child, "."), make_entry(parent.inode, "..")}),
                      Placement::Fresh);
        inode.link_count = 2;
        write_inode(child, inode);

        add_directory_entry(parent.inode, parent.name, child);
        Inode parent_inode = read_inode(parent.inode);
        ++parent_inode.link_count;  // the child's ".."
        write_inode(parent.inode, parent_inode);
    });
    return child;
}

uint32_t FileSystem::create_file(const string& path, uint16_t mode) {
    uint32_t file = kNullInode;
    atomic([&] {
        const ParentPath parent = resolve_parent(path);
        validate_entry_name(parent.name);
        require_permission(read_inode(parent.inode), 02, path, "create");
        if (find_entry_or_null(parent.inode, parent.name) != kNullInode) {
            throw logic_error("'" + path + "' already exists.");
        }
        file = allocate_inode(InodeType::File, mode);
        add_directory_entry(parent.inode, parent.name, file);
    });
    return file;
}

void FileSystem::write_file(const string& path, const string& content) {
    atomic([&] {
        const uint32_t number = resolve_path(path);
        Inode inode = read_inode(number);
        if (!is_type(inode, InodeType::File)) {
            throw logic_error("'" + path + "' is a directory, not a file.");
        }
        require_permission(inode, 02, path, "write");
        store_content(inode, vector<uint8_t>(content.begin(), content.end()), Placement::Fresh);
        inode.modified_at = now();
        write_inode(number, inode);
    });
}

void FileSystem::append_file(const string& path, const string& content) {
    atomic([&] {
        const uint32_t number = resolve_path(path);
        Inode inode = read_inode(number);
        if (!is_type(inode, InodeType::File)) {
            throw logic_error("'" + path + "' is a directory, not a file.");
        }
        require_permission(inode, 02, path, "write");
        vector<uint8_t> bytes = read_content(inode);
        bytes.insert(bytes.end(), content.begin(), content.end());
        store_content(inode, bytes, Placement::Fresh);
        inode.modified_at = now();
        write_inode(number, inode);
    });
}

string FileSystem::read_file(const string& path) {
    const Inode inode = read_inode(resolve_path(path));
    if (!is_type(inode, InodeType::File)) {
        throw logic_error("'" + path + "' is a directory, not a file.");
    }
    require_permission(inode, 04, path, "read");
    const vector<uint8_t> bytes = read_content(inode);
    return string(bytes.begin(), bytes.end());
}

void FileSystem::remove_file(const string& path) {
    atomic([&] {
        const uint32_t file = resolve_path(path);
        Inode inode = read_inode(file);
        if (!is_type(inode, InodeType::File)) {
            throw logic_error("'" + path + "' is a directory. Use 'deletedir' (rmdir) instead.");
        }
        const ParentPath parent = resolve_parent(path);
        require_permission(read_inode(parent.inode), 02, path, "delete");
        remove_directory_entry(parent.inode, parent.name);
        release_content(inode);
        free_inode(file);
    });
}

void FileSystem::remove_directory(const string& path) {
    atomic([&] {
        const uint32_t directory = resolve_path(path);
        if (directory == kRootInode) {
            throw invalid_argument("Cannot remove the root directory.");
        }
        if (directory == current_directory_) {
            throw invalid_argument("Cannot remove the current directory. 'goto ..' first.");
        }
        Inode inode = read_inode(directory);
        if (!is_type(inode, InodeType::Directory)) {
            throw logic_error("'" + path + "' is not a directory. Use 'delete' (rm) instead.");
        }
        if (!directory_empty(directory)) {
            throw logic_error("Directory '" + path + "' is not empty.");
        }

        const ParentPath parent = resolve_parent(path);
        require_permission(read_inode(parent.inode), 02, path, "delete");
        remove_directory_entry(parent.inode, parent.name);
        Inode parent_inode = read_inode(parent.inode);
        if (parent_inode.link_count > 0) {
            --parent_inode.link_count;
            write_inode(parent.inode, parent_inode);
        }
        release_content(inode);
        free_inode(directory);
    });
}

void FileSystem::rename_entry(const string& old_path, const string& new_path) {
    atomic([&] {
        const uint32_t entry_inode = resolve_path(old_path);
        if (entry_inode == kRootInode) {
            throw invalid_argument("Cannot rename the root directory.");
        }

        const ParentPath source = resolve_parent(old_path);
        const ParentPath destination = resolve_parent(new_path);
        validate_entry_name(destination.name);
        if (source.inode == destination.inode && source.name == destination.name) {
            return;
        }
        require_permission(read_inode(source.inode), 02, old_path, "move");
        require_permission(read_inode(destination.inode), 02, new_path, "create");
        if (find_entry_or_null(destination.inode, destination.name) != kNullInode) {
            throw logic_error("'" + new_path + "' already exists.");
        }

        const Inode inode = read_inode(entry_inode);
        const bool is_directory = is_type(inode, InodeType::Directory);
        if (is_directory) {
            for (uint32_t current = destination.inode, depth = 0;; ++depth) {
                if (current == entry_inode) {
                    throw invalid_argument("Cannot move a directory into itself.");
                }
                if (current == kRootInode || depth > sb_.inode_count) {
                    break;
                }
                current = find_directory_entry_in(current, "..");
            }
        }

        if (source.inode == destination.inode) {
            vector<DirEntry> entries = read_directory_entries(source.inode);
            for (DirEntry& entry : entries) {
                if (entry.inode == entry_inode && entry_name(entry) == source.name) {
                    entry = make_entry(entry_inode, destination.name);
                    update_directory(source.inode, entries);
                    return;
                }
            }
            throw FsError("Source directory entry is missing.");
        }

        add_directory_entry(destination.inode, destination.name, entry_inode);
        remove_directory_entry(source.inode, source.name);

        if (is_directory) {
            vector<DirEntry> entries = read_directory_entries(entry_inode);
            for (DirEntry& entry : entries) {
                if (entry_name(entry) == "..") {
                    entry.inode = destination.inode;
                    update_directory(entry_inode, entries);
                    break;
                }
            }
            Inode source_inode = read_inode(source.inode);
            if (source_inode.link_count > 0) {
                --source_inode.link_count;
            }
            write_inode(source.inode, source_inode);
            Inode destination_inode = read_inode(destination.inode);
            ++destination_inode.link_count;
            write_inode(destination.inode, destination_inode);
        }
    });
}

void FileSystem::move_entry(const string& source, const string& destination) {
    atomic([&] {
        string target = destination;
        uint32_t existing = kNullInode;
        try {
            existing = resolve_path(destination);
        } catch (const out_of_range&) {
        }
        if (existing != kNullInode &&
            is_type(read_inode(existing), InodeType::Directory)) {
            target = join_path(destination, resolve_parent(source).name);
        }
        rename_entry(source, target);
    });
}

void FileSystem::copy_file(const string& source, const string& destination) {
    atomic([&] {
        const uint32_t number = resolve_path(source);
        const Inode inode = read_inode(number);
        if (!is_type(inode, InodeType::File)) {
            throw logic_error("'" + source + "' is a directory; only files can be copied.");
        }
        require_permission(inode, 04, source, "read");
        const vector<uint8_t> content = read_content(inode);

        string target = destination;
        uint32_t existing = kNullInode;
        try {
            existing = resolve_path(destination);
        } catch (const out_of_range&) {
        }
        if (existing != kNullInode &&
            is_type(read_inode(existing), InodeType::Directory)) {
            target = join_path(destination, resolve_parent(source).name);
            existing = kNullInode;
            try {
                existing = resolve_path(target);
            } catch (const out_of_range&) {
            }
        }
        if (existing == number) {
            throw invalid_argument("'" + source + "' and '" + target + "' are the same file.");
        }
        if (existing != kNullInode) {
            write_file(target, string(content.begin(), content.end()));
            return;
        }

        const uint32_t copy = create_file(target, inode.mode);
        Inode copy_inode = read_inode(copy);
        store_content(copy_inode, content, Placement::Fresh);
        write_inode(copy, copy_inode);
    });
}

void FileSystem::change_mode(const string& path, uint16_t mode) {
    if (mode > 0777) {
        throw invalid_argument("Permission bits must be between 000 and 777.");
    }
    atomic([&] {
        const uint32_t number = resolve_path(path);
        Inode inode = read_inode(number);
        inode.mode = mode;
        write_inode(number, inode);
    });
}

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

void FileSystem::require_directory(uint32_t inode) {
    if (!is_type(read_inode(inode), InodeType::Directory)) {
        throw logic_error("Inode " + to_string(inode) + " is not a directory.");
    }
}

// MiniFS has a single user who owns everything, so only the owner bits count.
void FileSystem::require_permission(const Inode& inode, uint16_t bit,
                                    const string& path, const char* action) const {
    if (((inode.mode >> 6) & bit) != 0) {
        return;
    }
    const char* needed = bit == 04 ? "read" : bit == 02 ? "write" : "execute";
    throw PermissionDeniedError("Permission denied: cannot " + string(action) + " '" +
                                path + "' (no " + needed + " permission; mode is " +
                                permission_string(inode.mode) + ").");
}

void FileSystem::validate_entry_name(const string& name) const {
    if (name.empty() || name.find('/') != string::npos) {
        throw invalid_argument("Invalid name '" + name + "'.");
    }
    if (any_of(name.begin(), name.end(),
               [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; })) {
        throw invalid_argument("Names cannot contain control characters such as line breaks or tabs.");
    }
    if (name.size() > kMaxNameLength) {
        throw invalid_argument("Name is too long (at most " + to_string(kMaxNameLength) +
                               " characters).");
    }
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
        throw FsError("Disk uses MiniFS format version " + to_string(sb_.version) +
                      ", this program needs version " + to_string(kFsVersion) +
                      ". Run 'format' to create a new disk.");
    }
    if (sb_.block_size != disk_.block_size() || sb_.total_blocks != disk_.block_count() ||
        sb_.inode_size != sizeof(Inode)) {
        throw FsError("Superblock does not match the disk image.");
    }

    const uint32_t inodes_per_block = sb_.block_size / sb_.inode_size;
    const bool layout_ok =
        sb_.bitmap_start == kSuperblockBlock + 1 &&
        sb_.inode_table_start == sb_.bitmap_start + sb_.bitmap_blocks &&
        sb_.journal_start == sb_.inode_table_start + sb_.inode_table_blocks &&
        sb_.journal_blocks >= 2 &&
        sb_.data_start == sb_.journal_start + sb_.journal_blocks &&
        sb_.data_start < sb_.total_blocks &&
        sb_.data_block_count == sb_.total_blocks - sb_.data_start &&
        uint64_t{sb_.bitmap_blocks} * sb_.block_size * 8 >= sb_.total_blocks &&
        sb_.inode_count == sb_.inode_table_blocks * inodes_per_block &&
        sb_.free_block_count <= sb_.data_block_count &&
        sb_.free_inode_count < sb_.inode_count &&
        sb_.root_inode == kRootInode &&
        sb_.allocation_mode <= static_cast<uint32_t>(AllocationMode::Indexed);
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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const char* allocation_mode_name(AllocationMode mode) {
    switch (mode) {
    case AllocationMode::Contiguous: return "contiguous";
    case AllocationMode::Linked: return "linked";
    case AllocationMode::Indexed: return "indexed";
    }
    return "unknown";
}

bool parse_allocation_mode(const string& text, AllocationMode& mode) {
    for (AllocationMode candidate :
         {AllocationMode::Contiguous, AllocationMode::Linked, AllocationMode::Indexed}) {
        if (text == allocation_mode_name(candidate)) {
            mode = candidate;
            return true;
        }
    }
    return false;
}

string permission_string(uint16_t mode) {
    string result;
    for (int shift = 6; shift >= 0; shift -= 3) {
        result += (mode >> shift) & 04 ? 'r' : '-';
        result += (mode >> shift) & 02 ? 'w' : '-';
        result += (mode >> shift) & 01 ? 'x' : '-';
    }
    return result;
}

size_t count_extents(const vector<uint32_t>& blocks) {
    size_t extents = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i == 0 || blocks[i] != blocks[i - 1] + 1) {
            ++extents;
        }
    }
    return extents;
}

string block_ranges(const vector<uint32_t>& blocks) {
    string result;
    for (size_t i = 0; i < blocks.size();) {
        size_t j = i;
        while (j + 1 < blocks.size() && blocks[j + 1] == blocks[j] + 1) {
            ++j;
        }
        if (!result.empty()) {
            result += ", ";
        }
        result += to_string(blocks[i]);
        if (j > i) {
            result += "-" + to_string(blocks[j]);
        }
        i = j + 1;
    }
    return result.empty() ? "none" : result;
}

} // namespace minifs
