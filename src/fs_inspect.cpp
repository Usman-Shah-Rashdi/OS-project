// Read-only inspection (stat, df, frag, map, export) plus the two whole-disk
// maintenance tools, fsck and defrag.

#include "minifs/file_system.hpp"

#include "minifs/fs_errors.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <iomanip>
#include <map>
#include <ostream>
#include <set>
#include <sstream>

using namespace std;

namespace minifs {

namespace {

string entry_name(const DirEntry& entry) {
    return string(entry.name, strnlen(entry.name, kDirNameSize));
}

bool is_type(const Inode& inode, InodeType type) {
    return inode.type == static_cast<uint16_t>(type);
}

const char* type_name(const Inode& inode) {
    return is_type(inode, InodeType::Directory) ? "directory" : "file";
}

string json_string(const string& text) {
    ostringstream out;
    out << '"';
    for (unsigned char c : text) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                out << "\\u" << hex << setw(4) << setfill('0') << static_cast<int>(c)
                    << dec << setfill(' ');
            } else {
                out << c;
            }
        }
    }
    out << '"';
    return out.str();
}

void json_numbers(ostream& output, const vector<uint32_t>& numbers) {
    output << '[';
    for (size_t i = 0; i < numbers.size(); ++i) {
        output << (i ? "," : "") << numbers[i];
    }
    output << ']';
}

string octal(uint16_t mode) {
    ostringstream out;
    out << oct << setw(4) << setfill('0') << mode;
    return out.str();
}

} // namespace

vector<pair<uint32_t, Inode>> FileSystem::allocated_inodes() {
    require_mounted();
    vector<pair<uint32_t, Inode>> result;
    const uint32_t per_block = sb_.block_size / sizeof(Inode);
    for (uint32_t i = 0; i < sb_.inode_table_blocks; ++i) {
        const vector<uint8_t> block = disk_.read_block(sb_.inode_table_start + i);
        for (uint32_t slot = 0; slot < per_block; ++slot) {
            const uint32_t number = i * per_block + slot;
            Inode inode;
            memcpy(&inode, block.data() + slot * sizeof(Inode), sizeof(Inode));
            if (number != kNullInode && !is_type(inode, InodeType::Free)) {
                result.emplace_back(number, inode);
            }
        }
    }
    return result;
}

map<uint32_t, string> FileSystem::inode_paths() {
    map<uint32_t, string> paths;
    paths[kRootInode] = "/";
    deque<uint32_t> queue{kRootInode};
    while (!queue.empty()) {
        const uint32_t directory = queue.front();
        queue.pop_front();
        vector<DirEntry> entries;
        try {
            entries = read_directory_entries(directory);
        } catch (const exception&) {
            continue;
        }
        for (const DirEntry& entry : entries) {
            const string name = entry_name(entry);
            if (entry.inode == kNullInode || entry.inode >= sb_.inode_count ||
                name == "." || name == ".." || paths.count(entry.inode)) {
                continue;
            }
            paths[entry.inode] = join_path(paths[directory], name);
            if (is_type(read_inode(entry.inode), InodeType::Directory)) {
                queue.push_back(entry.inode);
            }
        }
    }
    return paths;
}

FileInfo FileSystem::stat(const string& path) {
    const uint32_t number = resolve_path(path);
    FileInfo info;
    info.number = number;
    info.meta = read_inode(number);
    const map<uint32_t, string> paths = inode_paths();
    const auto found = paths.find(number);
    info.path = found != paths.end() ? found->second : path;
    info.data_blocks = data_blocks(info.meta);
    info.pointer_blocks = pointer_blocks(info.meta);
    info.extents = count_extents(info.data_blocks);
    return info;
}

UsageStats FileSystem::usage() const {
    require_mounted();
    UsageStats stats;
    stats.block_size = sb_.block_size;
    stats.total_blocks = sb_.total_blocks;
    stats.metadata_blocks = sb_.data_start;
    stats.data_blocks = sb_.data_block_count;
    stats.free_data_blocks = sb_.free_block_count;
    stats.used_data_blocks = sb_.data_block_count - sb_.free_block_count;
    stats.usable_inodes = sb_.inode_count - 1;
    stats.free_inodes = sb_.free_inode_count;
    stats.used_inodes = stats.usable_inodes - stats.free_inodes;
    return stats;
}

FragmentationReport FileSystem::fragmentation() {
    require_mounted();
    FragmentationReport report;
    const map<uint32_t, string> paths = inode_paths();
    const size_t payload = block_payload();

    for (const auto& item : allocated_inodes()) {
        vector<uint32_t> blocks;
        try {
            blocks = data_blocks(item.second);
        } catch (const FsError&) {
            continue;
        }
        if (blocks.empty()) {
            continue;
        }
        ++report.entries;
        const size_t extents = count_extents(blocks);
        report.total_extents += extents;
        report.internal_waste += blocks.size() * payload - item.second.size;
        if (extents > 1) {
            ++report.fragmented;
            const auto found = paths.find(item.first);
            report.fragmented_files.push_back(
                {found != paths.end() ? found->second : "inode " + to_string(item.first),
                 blocks.size(), extents});
        }
    }
    sort(report.fragmented_files.begin(), report.fragmented_files.end(),
         [](const FragmentedFile& a, const FragmentedFile& b) { return a.extents > b.extents; });

    uint32_t run = 0;
    for (uint32_t block = sb_.data_start; block <= sb_.total_blocks; ++block) {
        if (block < sb_.total_blocks && !bitmap_.is_used(block)) {
            ++report.free_blocks;
            ++run;
            continue;
        }
        if (run > 0) {
            ++report.free_extents;
            report.largest_free_extent = max(report.largest_free_extent, run);
            run = 0;
        }
    }
    if (report.free_blocks > 0) {
        report.external_fragmentation =
            1.0 - static_cast<double>(report.largest_free_extent) / report.free_blocks;
    }
    return report;
}

vector<BlockOwner> FileSystem::block_map() {
    require_mounted();
    vector<BlockOwner> owners(sb_.total_blocks);
    const auto mark = [&owners](uint32_t first, uint32_t count, BlockKind kind) {
        for (uint32_t block = first; block < first + count; ++block) {
            owners[block].kind = kind;
        }
    };
    mark(kSuperblockBlock, 1, BlockKind::Superblock);
    mark(sb_.bitmap_start, sb_.bitmap_blocks, BlockKind::Bitmap);
    mark(sb_.inode_table_start, sb_.inode_table_blocks, BlockKind::InodeTable);
    mark(sb_.journal_start, sb_.journal_blocks, BlockKind::Journal);

    for (const auto& item : allocated_inodes()) {
        const BlockKind kind = is_type(item.second, InodeType::Directory) ? BlockKind::Directory
                                                                          : BlockKind::File;
        try {
            for (uint32_t block : data_blocks(item.second)) {
                owners[block] = {kind, item.first};
            }
            for (uint32_t block : pointer_blocks(item.second)) {
                if (block >= sb_.data_start && block < sb_.total_blocks) {
                    owners[block] = {BlockKind::Pointer, item.first};
                }
            }
        } catch (const FsError&) {
        }
    }
    for (uint32_t block = sb_.data_start; block < sb_.total_blocks; ++block) {
        if (owners[block].kind == BlockKind::Free && bitmap_.is_used(block)) {
            owners[block].kind = BlockKind::Orphan;
        }
    }
    return owners;
}

void FileSystem::export_json(ostream& output) {
    require_mounted();
    const UsageStats stats = usage();
    const FragmentationReport frag = fragmentation();
    const vector<BlockOwner> owners = block_map();
    const map<uint32_t, string> paths = inode_paths();

    output << "{\n  \"superblock\": {"
           << "\"magic\": \"MNFS\", \"version\": " << sb_.version
           << ", \"block_size\": " << sb_.block_size
           << ", \"total_blocks\": " << sb_.total_blocks
           << ", \"bitmap_start\": " << sb_.bitmap_start
           << ", \"bitmap_blocks\": " << sb_.bitmap_blocks
           << ", \"inode_table_start\": " << sb_.inode_table_start
           << ", \"inode_table_blocks\": " << sb_.inode_table_blocks
           << ", \"inode_count\": " << sb_.inode_count
           << ", \"journal_start\": " << sb_.journal_start
           << ", \"journal_blocks\": " << sb_.journal_blocks
           << ", \"data_start\": " << sb_.data_start
           << ", \"allocation_mode\": " << json_string(allocation_mode_name(allocation_mode()))
           << ", \"created_at\": " << sb_.created_at
           << ", \"mounted_at\": " << sb_.mounted_at << "},\n";

    output << "  \"stats\": {"
           << "\"used_blocks\": " << stats.used_data_blocks
           << ", \"free_blocks\": " << stats.free_data_blocks
           << ", \"data_blocks\": " << stats.data_blocks
           << ", \"usage_percent\": " << fixed << setprecision(2)
           << (stats.data_blocks ? 100.0 * stats.used_data_blocks / stats.data_blocks : 0.0)
           << ", \"used_inodes\": " << stats.used_inodes
           << ", \"free_inodes\": " << stats.free_inodes
           << ", \"fragmented_files\": " << frag.fragmented
           << ", \"free_extents\": " << frag.free_extents
           << ", \"largest_free_extent\": " << frag.largest_free_extent
           << ", \"external_fragmentation_percent\": " << 100.0 * frag.external_fragmentation
           << ", \"internal_waste_bytes\": " << frag.internal_waste << "},\n";
    output.unsetf(ios::floatfield);

    // One character per block (same letters as 'map') and the owning inode.
    output << "  \"blocks\": \"";
    for (const BlockOwner& owner : owners) {
        output << static_cast<char>(owner.kind);
    }
    output << "\",\n  \"owners\": [";
    for (size_t i = 0; i < owners.size(); ++i) {
        output << (i ? "," : "") << owners[i].inode;
    }
    output << "],\n  \"inodes\": [\n";

    bool first = true;
    for (const auto& item : allocated_inodes()) {
        const Inode& inode = item.second;
        vector<uint32_t> blocks;
        try {
            blocks = data_blocks(inode);
        } catch (const FsError&) {
        }
        const auto found = paths.find(item.first);
        output << (first ? "" : ",\n") << "    {\"inode\": " << item.first
               << ", \"path\": "
               << (found != paths.end() ? json_string(found->second) : string("null"))
               << ", \"type\": " << json_string(type_name(inode))
               << ", \"size\": " << inode.size
               << ", \"mode\": " << json_string(octal(inode.mode))
               << ", \"permissions\": " << json_string(permission_string(inode.mode))
               << ", \"links\": " << inode.link_count
               << ", \"created_at\": " << inode.created_at
               << ", \"modified_at\": " << inode.modified_at
               << ", \"blocks\": ";
        json_numbers(output, blocks);
        output << ", \"pointer_blocks\": ";
        json_numbers(output, pointer_blocks(inode));
        output << ", \"extents\": " << count_extents(blocks) << "}";
        first = false;
    }
    output << "\n  ]\n}\n";
}

// ---------------------------------------------------------------------------
// fsck
// ---------------------------------------------------------------------------

FsckReport FileSystem::fsck(bool repair) {
    require_mounted();
    FsckReport report;
    const auto problem = [&report](const string& text) { report.problems.push_back(text); };

    // 1. Journal: after a clean mount it must be empty.
    const JournalHeader header = journal_.read_header();
    if (header.magic != kJournalMagic ||
        header.state != static_cast<uint32_t>(JournalState::Empty)) {
        problem("Journal is not empty.");
        if (repair) {
            journal_.create();
            report.repairs.push_back("Cleared the journal.");
        }
    }

    // 2. Inodes and the blocks they point at.
    map<uint32_t, Inode> live;
    set<uint32_t> broken;
    vector<uint32_t> owner(sb_.total_blocks, kNullInode);
    for (const auto& item : allocated_inodes()) {
        const uint32_t number = item.first;
        const Inode& inode = item.second;
        if (!is_type(inode, InodeType::File) && !is_type(inode, InodeType::Directory)) {
            problem("Inode " + to_string(number) + " has an invalid type (" +
                    to_string(inode.type) + ").");
            broken.insert(number);
            continue;
        }
        live[number] = inode;
        vector<uint32_t> blocks;
        try {
            blocks = data_blocks(inode);
        } catch (const FsError& error) {
            problem("Inode " + to_string(number) + ": " + error.what());
            broken.insert(number);
            continue;
        }
        for (uint32_t block : pointer_blocks(inode)) {
            blocks.push_back(block);
        }
        for (uint32_t block : blocks) {
            if (block < sb_.data_start || block >= sb_.total_blocks) {
                problem("Inode " + to_string(number) + " points outside the data region.");
                broken.insert(number);
                break;
            }
            if (owner[block] != kNullInode) {
                problem("Block " + to_string(block) + " is used by both inode " +
                        to_string(owner[block]) + " and inode " + to_string(number) +
                        " (cross-linked).");
                continue;
            }
            owner[block] = number;
        }
        if (is_type(inode, InodeType::Directory) && inode.size % sizeof(DirEntry) != 0) {
            problem("Directory inode " + to_string(number) + " has an invalid size.");
            broken.insert(number);
        }
    }

    // 3. Walk the directory tree from the root.
    map<uint32_t, uint32_t> references;
    set<uint32_t> reachable{kRootInode};
    if (!live.count(kRootInode) || !is_type(live[kRootInode], InodeType::Directory) ||
        broken.count(kRootInode)) {
        problem("Root directory (inode 1) is missing or damaged. Reformat the disk.");
        return report;
    }
    struct Pending {
        uint32_t inode;
        uint32_t parent;
        string path;
    };
    deque<Pending> queue{{kRootInode, kRootInode, "/"}};
    while (!queue.empty()) {
        const Pending current = queue.front();
        queue.pop_front();
        vector<DirEntry> entries;
        try {
            entries = read_directory_entries(current.inode);
        } catch (const exception& error) {
            problem("Cannot read directory '" + current.path + "': " + error.what());
            continue;
        }

        bool changed = false;
        set<string> names;
        vector<DirEntry> kept;
        for (DirEntry entry : entries) {
            const string name = entry_name(entry);
            if (entry.inode == kNullInode) {
                continue;
            }
            if (name == "." && entry.inode != current.inode) {
                problem("Directory '" + current.path + "' has a wrong '.' entry.");
                entry.inode = current.inode;
                changed = true;
            }
            if (name == ".." && entry.inode != current.parent) {
                problem("Directory '" + current.path + "' has a wrong '..' entry.");
                entry.inode = current.parent;
                changed = true;
            }
            if (entry.inode >= sb_.inode_count || !live.count(entry.inode) ||
                broken.count(entry.inode)) {
                problem("Directory '" + current.path + "' has entry '" + name +
                        "' pointing to a free or damaged inode " + to_string(entry.inode) + ".");
                changed = true;
                continue;  // drop the entry
            }
            if (!names.insert(name).second) {
                problem("Directory '" + current.path + "' contains '" + name + "' twice.");
            }
            ++references[entry.inode];
            kept.push_back(entry);
            if (name == "." || name == "..") {
                continue;
            }
            if (!reachable.insert(entry.inode).second) {
                if (is_type(live[entry.inode], InodeType::Directory)) {
                    problem("Directory inode " + to_string(entry.inode) +
                            " appears in more than one place.");
                }
                continue;
            }
            if (is_type(live[entry.inode], InodeType::Directory)) {
                queue.push_back({entry.inode, current.inode, join_path(current.path, name)});
            }
        }
        if (changed && repair) {
            try {
                update_directory(current.inode, kept);
                report.repairs.push_back("Fixed the entries of directory '" + current.path + "'.");
            } catch (const exception& error) {
                report.repairs.push_back("Could not fix directory '" + current.path +
                                         "': " + error.what());
            }
        }
    }

    // 4. Inodes that no directory points at.
    for (const auto& item : live) {
        if (reachable.count(item.first) || broken.count(item.first)) {
            continue;
        }
        problem("Inode " + to_string(item.first) + " (" + type_name(item.second) + ", " +
                to_string(item.second.size) + " bytes) is not in any directory (orphan).");
        if (repair) {
            free_inode(item.first);
            report.repairs.push_back("Freed orphan inode " + to_string(item.first) + ".");
        }
    }
    for (uint32_t number : broken) {
        if (repair && number != kRootInode) {
            free_inode(number);
            report.repairs.push_back("Freed damaged inode " + to_string(number) + ".");
        }
    }

    // 5. Link counts must equal the number of entries pointing at the inode.
    for (const auto& item : live) {
        if (!reachable.count(item.first) || broken.count(item.first)) {
            continue;
        }
        const uint32_t expected = references[item.first];
        if (item.second.link_count != expected) {
            problem("Inode " + to_string(item.first) + " has link count " +
                    to_string(item.second.link_count) + ", expected " + to_string(expected) + ".");
            if (repair) {
                Inode fixed = read_inode(item.first);
                fixed.link_count = expected;
                write_inode(item.first, fixed);
                report.repairs.push_back("Set the link count of inode " +
                                         to_string(item.first) + " to " +
                                         to_string(expected) + ".");
            }
        }
    }

    // 6. Bitmap against the blocks that are really in use.
    if (repair) {
        // Directory repairs and freed inodes changed ownership; recount.
        fill(owner.begin(), owner.end(), kNullInode);
        for (const auto& item : allocated_inodes()) {
            try {
                vector<uint32_t> blocks = data_blocks(item.second);
                for (uint32_t block : pointer_blocks(item.second)) {
                    blocks.push_back(block);
                }
                for (uint32_t block : blocks) {
                    if (block >= sb_.data_start && block < sb_.total_blocks) {
                        owner[block] = item.first;
                    }
                }
            } catch (const FsError&) {
            }
        }
    }
    vector<uint32_t> leaked;
    vector<uint32_t> unmarked;
    for (uint32_t block = 0; block < sb_.total_blocks; ++block) {
        const bool used = bitmap_.is_used(block);
        if (block < sb_.data_start) {
            if (!used) {
                unmarked.push_back(block);
            }
            continue;
        }
        const bool referenced = owner[block] != kNullInode;
        if (used && !referenced) {
            leaked.push_back(block);
        } else if (!used && referenced) {
            unmarked.push_back(block);
        }
    }
    if (!leaked.empty()) {
        problem(to_string(leaked.size()) +
                " block(s) marked used but not owned by any file (leaked): " +
                block_ranges(leaked) + ".");
    }
    if (!unmarked.empty()) {
        problem(to_string(unmarked.size()) + " block(s) in use but marked free: " +
                block_ranges(unmarked) + ".");
    }

    // 7. Superblock counters.
    const uint32_t free_blocks = bitmap_.count_free_data_blocks();
    const uint32_t free_inodes =
        sb_.inode_count - 1 - static_cast<uint32_t>(allocated_inodes().size());
    if (sb_.free_block_count != free_blocks) {
        problem("Superblock says " + to_string(sb_.free_block_count) +
                " free blocks, bitmap has " + to_string(free_blocks) + ".");
    }
    if (sb_.free_inode_count != free_inodes) {
        problem("Superblock says " + to_string(sb_.free_inode_count) +
                " free inodes, inode table has " + to_string(free_inodes) + ".");
    }

    if (repair && !report.problems.empty()) {
        vector<bool> used(sb_.total_blocks, false);
        for (uint32_t block = 0; block < sb_.total_blocks; ++block) {
            used[block] = block < sb_.data_start || owner[block] != kNullInode;
        }
        bitmap_.assign(used);
        sb_.free_block_count = bitmap_.count_free_data_blocks();
        sb_.free_inode_count =
            sb_.inode_count - 1 - static_cast<uint32_t>(allocated_inodes().size());
        write_superblock();
        report.repairs.push_back("Rebuilt the free-space bitmap and superblock counters.");
    }
    return report;
}

// ---------------------------------------------------------------------------
// defrag
// ---------------------------------------------------------------------------

void FileSystem::relocate(uint32_t number, uint32_t target) {
    compact_target_ = target;
    try {
        atomic([&] {
            Inode inode = read_inode(number);
            store_content(inode, read_content(inode), Placement::Compact);
            write_inode(number, inode);
        });
    } catch (...) {
        compact_target_ = kNullBlock;
        throw;
    }
    compact_target_ = kNullBlock;
}

// Compaction: visit files in disk order and slide each one down to `cursor`,
// the end of the packed area, so the data region ends up as one solid run of
// files followed by one run of free space. A file in the way is moved above
// the target first; a file whose target overlaps its own blocks moves out and
// back. Every move is its own journaled transaction, so a crash during defrag
// loses nothing.
DefragResult FileSystem::defrag() {
    require_mounted();
    DefragResult result;

    // Blocks an inode occupies: data blocks, then its indirect block.
    const auto footprint = [this](const Inode& inode) {
        vector<uint32_t> blocks = data_blocks(inode);
        for (uint32_t block : pointer_blocks(inode)) {
            blocks.push_back(block);
        }
        return blocks;
    };
    const auto packed_at = [](const vector<uint32_t>& blocks, uint32_t start) {
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (blocks[i] != start + i) {
                return false;
            }
        }
        return true;
    };
    const auto move = [&](uint32_t number, uint32_t target, size_t blocks) {
        relocate(number, target);
        ++result.entries_moved;
        result.blocks_moved += blocks;
    };

    set<uint32_t> done;
    uint32_t cursor = sb_.data_start;
    for (;;) {
        // Next file in disk order that is not packed yet.
        uint32_t number = kNullInode;
        uint32_t first = UINT32_MAX;
        for (const auto& item : allocated_inodes()) {
            if (done.count(item.first)) {
                continue;
            }
            vector<uint32_t> blocks;
            try {
                blocks = data_blocks(item.second);
            } catch (const FsError&) {
            }
            if (blocks.empty()) {
                done.insert(item.first);
            } else if (blocks.front() < first) {
                first = blocks.front();
                number = item.first;
            }
        }
        if (number == kNullInode) {
            break;
        }
        done.insert(number);

        const vector<uint32_t> blocks = footprint(read_inode(number));
        const uint32_t span = static_cast<uint32_t>(blocks.size());
        if (packed_at(blocks, cursor)) {
            cursor += span;
            continue;
        }
        if (uint64_t{cursor} + span > sb_.total_blocks) {
            break;
        }

        // Move other files out of [cursor, cursor + span).
        const vector<BlockOwner> owners = block_map();
        set<uint32_t> in_the_way;
        bool blocked = false;
        for (uint32_t block = cursor; block < cursor + span; ++block) {
            if (owners[block].kind == BlockKind::Orphan) {
                blocked = true;  // leaked block: run 'check repair' first
            } else if (owners[block].kind != BlockKind::Free && owners[block].inode != number) {
                in_the_way.insert(owners[block].inode);
            }
        }
        for (uint32_t other : in_the_way) {
            if (blocked) {
                break;
            }
            const uint32_t other_span = static_cast<uint32_t>(footprint(read_inode(other)).size());
            const uint32_t spot = bitmap_.find_run_from(cursor + span, other_span);
            if (spot == kNullBlock) {
                blocked = true;
            } else {
                move(other, spot, other_span);
            }
        }

        // Move this file down, through a temporary spot if it overlaps itself.
        if (!blocked) {
            const set<uint32_t> own(blocks.begin(), blocks.end());
            bool overlaps = false;
            for (uint32_t block = cursor; block < cursor + span; ++block) {
                overlaps = overlaps || own.count(block) > 0;
            }
            if (overlaps) {
                const uint32_t spot = bitmap_.find_run_from(cursor + span, span);
                if (spot == kNullBlock) {
                    blocked = true;
                } else {
                    move(number, spot, span);
                }
            }
        }
        if (blocked) {
            // Not enough free space to pack this file; leave it where it is.
            cursor = max(cursor, *max_element(blocks.begin(), blocks.end()) + 1);
            continue;
        }
        move(number, cursor, span);
        cursor += span;
    }
    return result;
}

} // namespace minifs
