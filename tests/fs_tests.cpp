// Unit checks for the disk layer, bitmap, inode table, directories, file
// operations, allocation modes, permissions, journal, fsck and defrag.
// Run with `ctest --test-dir build` or `build/minifs_tests`.

#include "minifs/cli.hpp"
#include "minifs/file_system.hpp"
#include "minifs/fs_errors.hpp"
#include "minifs/virtual_disk.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace std;
using namespace minifs;

namespace {

int failures = 0;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            cerr << "  FAILED line " << __LINE__ << ": " #condition "\n";   \
            ++failures;                                                     \
        }                                                                   \
    } while (false)

template <typename Error>
bool throws(const function<void()>& action) {
    try {
        action();
    } catch (const Error&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

const string kDiskPath = "test_disk.bin";
constexpr size_t kOneMb = 1024 * 1024;

// 1 MB with 4 KB blocks = 256 blocks:
//   block 0 superblock, block 1 bitmap, blocks 2-3 inode table (64 inodes),
//   blocks 4-19 journal, blocks 20-255 data. Block 20 holds the root directory.
constexpr uint32_t kDataStart = 20;
constexpr uint32_t kDataBlocks = 236;

void test_format_layout() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);

    const Superblock& sb = fs.superblock();
    CHECK(sb.magic == kMagic);
    CHECK(sb.version == kFsVersion);
    CHECK(sb.block_size == 4096);
    CHECK(sb.total_blocks == 256);
    CHECK(sb.bitmap_start == 1);
    CHECK(sb.bitmap_blocks == 1);
    CHECK(sb.inode_table_start == 2);
    CHECK(sb.inode_table_blocks == 2);
    CHECK(sb.inode_count == 64);
    CHECK(sb.journal_start == 4);
    CHECK(sb.journal_blocks == 16);
    CHECK(sb.data_start == kDataStart);
    CHECK(sb.data_block_count == kDataBlocks);
    CHECK(sb.free_block_count == kDataBlocks - 1);  // root directory uses one block
    CHECK(sb.free_inode_count == 62);   // inode 0 reserved, inode 1 is root
    CHECK(sb.root_inode == kRootInode);
    CHECK(sb.allocation_mode == static_cast<uint32_t>(AllocationMode::Indexed));

    for (uint32_t block = 0; block <= kDataStart; ++block) {
        CHECK(fs.is_block_used(block));
    }
    CHECK(!fs.is_block_used(kDataStart + 1));
    CHECK(!fs.is_block_used(255));
}

void test_root_directory() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);

    const Inode root = fs.read_inode(kRootInode);
    CHECK(root.type == static_cast<uint16_t>(InodeType::Directory));
    CHECK(root.mode == 0755);
    CHECK(root.link_count == 2);
    CHECK(root.size == 2 * sizeof(DirEntry));
    CHECK(root.direct[0] == kDataStart);
    CHECK(root.direct[1] == kNullBlock);

    const vector<uint8_t> data = fs.read_data_block(root.direct[0]);
    DirEntry entries[2];
    memcpy(entries, data.data(), sizeof(entries));
    CHECK(entries[0].inode == kRootInode);
    CHECK(string(entries[0].name) == ".");
    CHECK(entries[1].inode == kRootInode);
    CHECK(string(entries[1].name) == "..");

    CHECK(fs.read_inode(kNullInode).type == static_cast<uint16_t>(InodeType::Free));
}

void test_block_allocation() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);

    const uint32_t a = fs.allocate_block();
    const uint32_t b = fs.allocate_block();
    CHECK(a == kDataStart + 1);
    CHECK(b == kDataStart + 2);
    CHECK(fs.superblock().free_block_count == kDataBlocks - 3);

    fs.free_block(a);
    CHECK(!fs.is_block_used(a));
    CHECK(fs.superblock().free_block_count == kDataBlocks - 2);
    CHECK(fs.allocate_block() == a);  // first fit reuses the hole

    CHECK(throws<logic_error>([&] { fs.free_block(200); }));      // already free
    CHECK(throws<invalid_argument>([&] { fs.free_block(2); }));   // metadata
    CHECK(throws<out_of_range>([&] { fs.free_block(256); }));
    CHECK(throws<out_of_range>([&] { fs.read_data_block(3); }));  // inode table
    CHECK(throws<logic_error>(
        [&] { fs.write_data_block(200, vector<uint8_t>(4096, 1)); }));

    // Allocated blocks come back zero-filled even if old data was there.
    vector<uint8_t> junk(4096, 0xAB);
    fs.write_data_block(b, junk);
    fs.free_block(b);
    const uint32_t reused = fs.allocate_block();
    CHECK(reused == b);
    CHECK(fs.read_data_block(reused) == vector<uint8_t>(4096, 0));
}

void test_disk_full() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);

    uint32_t allocated = 0;
    while (fs.superblock().free_block_count > 0) {
        fs.allocate_block();
        ++allocated;
    }
    CHECK(allocated == kDataBlocks - 1);
    CHECK(throws<DiskFullError>([&] { fs.allocate_block(); }));
}

void test_inode_allocation() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);

    const uint32_t file = fs.allocate_inode(InodeType::File, 0644);
    CHECK(file == 2);
    const Inode inode = fs.read_inode(file);
    CHECK(inode.type == static_cast<uint16_t>(InodeType::File));
    CHECK(inode.mode == 0644);
    CHECK(inode.size == 0);
    CHECK(inode.created_at > 0);
    CHECK(fs.superblock().free_inode_count == 61);

    fs.free_inode(file);
    CHECK(fs.read_inode(file).type == static_cast<uint16_t>(InodeType::Free));
    CHECK(fs.superblock().free_inode_count == 62);
    CHECK(throws<logic_error>([&] { fs.free_inode(file); }));
    CHECK(throws<invalid_argument>([&] { fs.free_inode(kNullInode); }));
    CHECK(throws<out_of_range>([&] { fs.read_inode(64); }));

    // Fill the table: 62 free inodes, then the next allocation fails.
    for (int i = 0; i < 62; ++i) {
        fs.allocate_inode(InodeType::File, 0644);
    }
    CHECK(fs.superblock().free_inode_count == 0);
    CHECK(throws<NoFreeInodesError>(
        [&] { fs.allocate_inode(InodeType::File, 0644); }));
}

void test_persistence() {
    uint32_t block = 0;
    uint32_t inode_number = 0;
    {
        VirtualDisk disk(kDiskPath);
        FileSystem fs(disk);
        fs.format(kOneMb);
        block = fs.allocate_block();
        vector<uint8_t> data(4096, 0);
        memcpy(data.data(), "hello", 5);
        fs.write_data_block(block, data);

        inode_number = fs.allocate_inode(InodeType::File, 0600);
        Inode inode = fs.read_inode(inode_number);
        inode.size = 5;
        inode.direct[0] = block;
        fs.write_inode(inode_number, inode);
        fs.unmount();
    }

    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.mount();
    CHECK(fs.superblock().free_block_count == kDataBlocks - 2);
    CHECK(fs.superblock().free_inode_count == 61);
    CHECK(fs.is_block_used(block));
    CHECK(!fs.is_block_used(block + 1));

    const Inode inode = fs.read_inode(inode_number);
    CHECK(inode.type == static_cast<uint16_t>(InodeType::File));
    CHECK(inode.mode == 0600);
    CHECK(inode.size == 5);
    CHECK(inode.direct[0] == block);
    const vector<uint8_t> data = fs.read_data_block(block);
    CHECK(memcmp(data.data(), "hello", 5) == 0);

    // The next allocation continues where the previous session left off.
    CHECK(fs.allocate_block() == block + 1);
}

void test_mount_rejects_bad_disks() {
    {
        VirtualDisk disk(kDiskPath);
        disk.format(kOneMb);  // raw zeros, no superblock
    }
    {
        VirtualDisk disk(kDiskPath);
        FileSystem fs(disk);
        CHECK(throws<FsError>([&] { fs.mount(); }));
        CHECK(!fs.is_mounted());
        CHECK(throws<NotMountedError>([&] { fs.allocate_block(); }));
    }

    // Corrupt the layout after a valid format.
    {
        VirtualDisk disk(kDiskPath);
        FileSystem fs(disk);
        fs.format(kOneMb);
        fs.unmount();
    }
    {
        fstream file(kDiskPath, ios::binary | ios::in | ios::out);
        const uint32_t bad_data_start = 1;
        file.seekp(static_cast<streamoff>(offsetof(Superblock, data_start)));
        file.write(reinterpret_cast<const char*>(&bad_data_start), sizeof(bad_data_start));
    }
    {
        VirtualDisk disk(kDiskPath);
        FileSystem fs(disk);
        CHECK(throws<FsError>([&] { fs.mount(); }));
    }
}

void test_format_rejects_tiny_disk() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    // Superblock + bitmap + inode table + 16-block journal = 19 blocks, so a
    // 19-block disk leaves no data block.
    CHECK(throws<invalid_argument>([&] { fs.format(3 * 4096); }));
    CHECK(throws<invalid_argument>([&] { fs.format(19 * 4096); }));
    fs.format(20 * 4096);  // smallest disk that fits the root directory block
    CHECK(fs.superblock().data_block_count == 1);
    CHECK(fs.superblock().free_block_count == 0);
}

void test_large_disk_layout() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(VirtualDisk::DefaultDiskSize);  // 100 MB
    const Superblock& sb = fs.superblock();
    CHECK(sb.total_blocks == 25600);
    CHECK(sb.bitmap_blocks == 1);
    CHECK(sb.inode_count == 6400);
    CHECK(sb.inode_table_blocks == 200);
    CHECK(sb.journal_start == 202);
    CHECK(sb.journal_blocks == 128);
    CHECK(sb.data_start == 330);
    fs.unmount();
    fs.mount();
    CHECK(fs.superblock().free_block_count == sb.data_block_count - 1);
}

void test_directories_and_paths() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);

    CHECK(fs.working_directory_path() == "/");
    const uint32_t a = fs.make_directory("/a");
    const uint32_t b = fs.make_directory("/a/b");
    CHECK(fs.resolve_path("/a") == a);
    CHECK(fs.resolve_path("/a/b") == b);
    CHECK(fs.resolve_path("/a/./b") == b);
    CHECK(fs.resolve_path("/a/b/..") == a);
    CHECK(fs.resolve_path("//a///b/") == b);
    CHECK(fs.directory_empty(b));

    fs.change_directory("/a/b");
    CHECK(fs.current_directory() == b);
    CHECK(fs.working_directory_path() == "/a/b");
    CHECK(fs.resolve_path(".") == b);
    CHECK(fs.resolve_path("../..") == kRootInode);
    CHECK(fs.resolve_path("../../a") == a);
    CHECK(fs.resolve_path("/..") == kRootInode);  // root's parent is root

    CHECK(throws<logic_error>([&] { fs.make_directory("/a"); }));
    CHECK(throws<out_of_range>([&] { fs.resolve_path("/missing"); }));
    CHECK(throws<invalid_argument>([&] { fs.make_directory("/a/" + string(60, 'x')); }));
    CHECK(throws<invalid_argument>([&] {
        fs.add_directory_entry(b, string(60, 'x'), a);
    }));

    const uint32_t file = fs.allocate_inode(InodeType::File, 0644);
    fs.add_directory_entry(b, "file", file);
    CHECK(!fs.directory_empty(a));
    CHECK(fs.resolve_path("/a/b/file") == file);
    CHECK(throws<logic_error>([&] { fs.resolve_path("/a/b/file/child"); }));
    fs.remove_directory_entry(b, "file");
    CHECK(fs.directory_empty(b));

    fs.unmount();
    fs.mount();
    CHECK(fs.resolve_path("/a/b") == b);
}

void test_large_directory() {
    // More entries than fit in one block (64 per block) spill into more blocks.
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(4 * kOneMb);
    for (int i = 0; i < 150; ++i) {
        fs.create_file("/f" + to_string(i));
    }
    CHECK(fs.list("/").size() == 150);
    CHECK(fs.data_blocks(fs.read_inode(kRootInode)).size() == 3);
    for (int i = 0; i < 150; i += 2) {
        fs.remove_file("/f" + to_string(i));
    }
    CHECK(fs.list("/").size() == 75);
    CHECK(fs.data_blocks(fs.read_inode(kRootInode)).size() == 2);
    CHECK(fs.fsck(false).clean());
}

void test_create_file() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);

    const uint32_t directory = fs.make_directory("/a");
    const uint32_t file = fs.create_file("/a/file");
    const Inode inode = fs.read_inode(file);
    CHECK(inode.type == static_cast<uint16_t>(InodeType::File));
    CHECK(inode.mode == 0644);
    CHECK(inode.size == 0);
    CHECK(fs.resolve_path("/a/file") == file);
    CHECK(!fs.directory_empty(directory));

    CHECK(throws<logic_error>([&] { fs.create_file("/a/file"); }));
    CHECK(throws<out_of_range>([&] { fs.create_file("/missing/file"); }));
    CHECK(throws<logic_error>([&] { fs.create_file("/a/file/child"); }));
    CHECK(throws<invalid_argument>([&] { fs.create_file("/a/.."); }));

    fs.unmount();
    fs.mount();
    CHECK(fs.resolve_path("/a/file") == file);
    CHECK(fs.read_inode(file).size == 0);
}

void test_write_file() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);

    const uint32_t file = fs.create_file("/file");
    const string content(4096, 'x');
    fs.write_file("/file", content);
    Inode inode = fs.read_inode(file);
    CHECK(inode.size == content.size());
    CHECK(inode.direct[0] != kNullBlock);
    CHECK(inode.direct[1] == kNullBlock);

    vector<uint8_t> data = fs.read_data_block(inode.direct[0]);
    CHECK(data == vector<uint8_t>(content.begin(), content.end()));

    fs.write_file("/file", "hello");
    inode = fs.read_inode(file);
    CHECK(inode.size == 5);
    CHECK(inode.direct[1] == kNullBlock);
    data = fs.read_data_block(inode.direct[0]);
    CHECK(string(data.begin(), data.begin() + 5) == "hello");

    fs.write_file("/file", "");
    inode = fs.read_inode(file);
    CHECK(inode.size == 0);
    CHECK(inode.direct[0] == kNullBlock);
    CHECK(fs.superblock().free_block_count == fs.superblock().data_block_count - 1);

    CHECK(throws<logic_error>([&] { fs.write_file("/", "data"); }));
    CHECK(throws<out_of_range>([&] { fs.write_file("/missing", "data"); }));

    fs.write_file("/file", "persistent");
    fs.append_file("/file", " data");
    fs.unmount();
    fs.mount();
    CHECK(fs.read_file("/file") == "persistent data");
}

void test_read_file() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);
    fs.create_file("/file");

    CHECK(fs.read_file("/file").empty());
    fs.write_file("/file", string(4097, 'x'));
    CHECK(fs.read_file("/file") == string(4097, 'x'));
    CHECK(throws<logic_error>([&] { fs.read_file("/"); }));
    CHECK(throws<out_of_range>([&] { fs.read_file("/missing"); }));
}

void test_indirect_block() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(8 * kOneMb);
    const uint32_t file = fs.create_file("/big");

    // 50 blocks: 10 direct + 40 through the indirect block.
    string content;
    for (int i = 0; i < 50 * 4096; ++i) {
        content += static_cast<char>('a' + i % 26);
    }
    fs.write_file("/big", content);
    Inode inode = fs.read_inode(file);
    CHECK(inode.indirect != kNullBlock);
    CHECK(fs.data_blocks(inode).size() == 50);
    CHECK(fs.pointer_blocks(inode).size() == 1);
    CHECK(fs.read_file("/big") == content);
    const uint32_t indirect = inode.indirect;

    // Shrinking below the direct limit frees the indirect block.
    fs.write_file("/big", "small");
    inode = fs.read_inode(file);
    CHECK(inode.indirect == kNullBlock);
    CHECK(!fs.is_block_used(indirect));

    // The largest file is 10 direct + 1024 indirect blocks.
    CHECK(fs.max_file_size() == (10 + 1024) * 4096ULL);
    CHECK(throws<FileTooLargeError>(
        [&] { fs.write_file("/big", string(fs.max_file_size() + 1, 'x')); }));
    fs.write_file("/big", string(fs.max_file_size(), 'y'));
    CHECK(fs.read_file("/big").size() == fs.max_file_size());
    CHECK(fs.fsck(false).clean());
}

void test_allocation_modes() {
    for (AllocationMode mode :
         {AllocationMode::Contiguous, AllocationMode::Linked, AllocationMode::Indexed}) {
        VirtualDisk disk(kDiskPath);
        FileSystem fs(disk);
        fs.format(kOneMb, mode);
        CHECK(fs.allocation_mode() == mode);

        fs.make_directory("/d");
        for (int i = 0; i < 6; ++i) {
            fs.create_file("/d/f" + to_string(i));
            fs.write_file("/d/f" + to_string(i), string(static_cast<size_t>(i) * 5000 + 7, 'a' + i));
        }
        fs.remove_file("/d/f1");
        fs.remove_file("/d/f3");
        fs.create_file("/d/g");
        fs.write_file("/d/g", string(30000, 'g'));

        const vector<uint32_t> blocks = fs.data_blocks(fs.read_inode(fs.resolve_path("/d/g")));
        if (mode == AllocationMode::Contiguous) {
            CHECK(count_extents(blocks) == 1);
        }
        if (mode == AllocationMode::Linked) {
            CHECK(fs.block_payload() == 4092);
        }

        fs.unmount();
        fs.mount();
        CHECK(fs.read_file("/d/g") == string(30000, 'g'));
        CHECK(fs.read_file("/d/f4") == string(20007, 'e'));
        CHECK(fs.fsck(false).clean());
    }
}

void test_contiguous_external_fragmentation() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb, AllocationMode::Contiguous);  // 235 free data blocks
    const auto blocks = [](size_t count) { return string(count * 4096, 'x'); };
    fs.create_file("/a");
    fs.write_file("/a", blocks(100));
    fs.create_file("/b");
    fs.write_file("/b", blocks(30));
    fs.create_file("/c");
    fs.write_file("/c", blocks(100));
    fs.remove_file("/b");  // free: a hole of 30 and 5 at the end

    fs.create_file("/d");
    CHECK(throws<NoContiguousSpaceError>([&] { fs.write_file("/d", blocks(33)); }));
    CHECK(fs.fsck(false).clean());
    fs.write_file("/d", blocks(30));  // fits the hole exactly
    CHECK(count_extents(fs.data_blocks(fs.read_inode(fs.resolve_path("/d")))) == 1);
}

void test_remove_file() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);
    const uint32_t file = fs.create_file("/file");
    fs.write_file("/file", string(4097, 'x'));
    const uint32_t first_block = fs.read_inode(file).direct[0];
    const uint32_t free_blocks = fs.superblock().free_block_count;

    fs.remove_file("/file");
    CHECK(throws<out_of_range>([&] { fs.resolve_path("/file"); }));
    CHECK(fs.read_inode(file).type == static_cast<uint16_t>(InodeType::Free));
    CHECK(!fs.is_block_used(first_block));
    CHECK(fs.superblock().free_block_count == free_blocks + 2);
    CHECK(throws<logic_error>([&] { fs.remove_file("/"); }));
    CHECK(throws<out_of_range>([&] { fs.remove_file("/missing"); }));
}

void test_remove_directory() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);
    const uint32_t parent = fs.make_directory("/parent");
    const uint32_t directory = fs.make_directory("/parent/child");
    const uint32_t block = fs.read_inode(directory).direct[0];
    const uint32_t free_blocks = fs.superblock().free_block_count;

    fs.remove_directory("/parent/child");
    CHECK(throws<out_of_range>([&] { fs.resolve_path("/parent/child"); }));
    CHECK(fs.read_inode(directory).type == static_cast<uint16_t>(InodeType::Free));
    CHECK(!fs.is_block_used(block));
    CHECK(fs.superblock().free_block_count == free_blocks + 1);
    CHECK(fs.read_inode(parent).link_count == 2);

    fs.create_file("/parent/file");
    CHECK(throws<logic_error>([&] { fs.remove_directory("/parent"); }));
    CHECK(throws<logic_error>([&] { fs.remove_directory("/parent/file"); }));
    CHECK(throws<invalid_argument>([&] { fs.remove_directory("/"); }));
    fs.change_directory("/parent");
    CHECK(throws<invalid_argument>([&] { fs.remove_directory("/parent"); }));
}

void test_rename_entry() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);
    const uint32_t source_dir = fs.make_directory("/source");
    const uint32_t destination_dir = fs.make_directory("/destination");
    const uint32_t file = fs.create_file("/source/file");
    fs.write_file("/source/file", "data");

    fs.rename_entry("/source/file", "/destination/renamed");
    CHECK(throws<out_of_range>([&] { fs.resolve_path("/source/file"); }));
    CHECK(fs.resolve_path("/destination/renamed") == file);
    CHECK(fs.read_file("/destination/renamed") == "data");

    fs.rename_entry("/destination/renamed", "/destination/final");
    CHECK(fs.resolve_path("/destination/final") == file);
    CHECK(throws<logic_error>(
        [&] { fs.rename_entry("/destination/final", "/source"); }));

    const uint32_t child = fs.make_directory("/source/child");
    fs.rename_entry("/source/child", "/destination/moved");
    CHECK(fs.resolve_path("/destination/moved") == child);
    CHECK(fs.resolve_path("/destination/moved/..") == destination_dir);
    CHECK(fs.read_inode(source_dir).link_count == 2);
    CHECK(fs.read_inode(destination_dir).link_count == 3);
    CHECK(throws<invalid_argument>(
        [&] { fs.rename_entry("/destination", "/destination/nested"); }));
    CHECK(fs.fsck(false).clean());
}

void test_copy_and_move() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);
    fs.make_directory("/docs");
    fs.make_directory("/backup");
    fs.create_file("/docs/a.txt");
    fs.write_file("/docs/a.txt", "original");

    fs.copy_file("/docs/a.txt", "/backup");            // into a directory
    CHECK(fs.read_file("/backup/a.txt") == "original");
    fs.write_file("/docs/a.txt", "changed");           // copies are independent
    CHECK(fs.read_file("/backup/a.txt") == "original");
    fs.copy_file("/docs/a.txt", "/backup/a.txt");      // overwrite existing
    CHECK(fs.read_file("/backup/a.txt") == "changed");
    fs.copy_file("/docs/a.txt", "/docs/b.txt");         // new name
    CHECK(fs.read_file("/docs/b.txt") == "changed");
    CHECK(throws<logic_error>([&] { fs.copy_file("/docs", "/backup"); }));
    CHECK(throws<invalid_argument>([&] { fs.copy_file("/docs/a.txt", "/docs/a.txt"); }));

    fs.move_entry("/docs/b.txt", "/backup");            // into a directory
    CHECK(fs.read_file("/backup/b.txt") == "changed");
    CHECK(throws<out_of_range>([&] { fs.resolve_path("/docs/b.txt"); }));
    fs.move_entry("/backup", "/docs");                  // directory into directory
    CHECK(fs.read_file("/docs/backup/a.txt") == "changed");
    CHECK(fs.fsck(false).clean());
}

void test_permissions() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);
    fs.make_directory("/d");
    fs.create_file("/d/f");
    fs.write_file("/d/f", "secret");

    fs.change_mode("/d/f", 0444);  // read only
    CHECK(throws<PermissionDeniedError>([&] { fs.write_file("/d/f", "x"); }));
    CHECK(throws<PermissionDeniedError>([&] { fs.append_file("/d/f", "x"); }));
    CHECK(fs.read_file("/d/f") == "secret");

    fs.change_mode("/d/f", 0200);  // write only
    CHECK(throws<PermissionDeniedError>([&] { fs.read_file("/d/f"); }));
    CHECK(throws<PermissionDeniedError>([&] { fs.copy_file("/d/f", "/g"); }));
    fs.write_file("/d/f", "new");

    fs.change_mode("/d", 0555);    // directory without write: no new entries
    CHECK(throws<PermissionDeniedError>([&] { fs.create_file("/d/g"); }));
    CHECK(throws<PermissionDeniedError>([&] { fs.remove_file("/d/f"); }));
    fs.change_mode("/d", 0311);    // no read: cannot list, can still reach files
    CHECK(throws<PermissionDeniedError>([&] { fs.list("/d"); }));
    CHECK(fs.resolve_path("/d/f") != kNullInode);
    fs.change_mode("/d", 0644);    // no execute: cannot enter or reach files
    CHECK(throws<PermissionDeniedError>([&] { fs.resolve_path("/d/f"); }));
    CHECK(throws<PermissionDeniedError>([&] { fs.change_directory("/d"); }));
    CHECK(throws<invalid_argument>([&] { fs.change_mode("/d", 01000); }));

    fs.change_mode("/d", 0755);
    fs.unmount();
    fs.mount();
    CHECK(fs.read_inode(fs.resolve_path("/d/f")).mode == 0200);
}

void test_failed_operation_rolls_back() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);
    fs.create_file("/f");
    fs.write_file("/f", "keep me");
    const uint32_t free_blocks = fs.superblock().free_block_count;
    const uint32_t free_inodes = fs.superblock().free_inode_count;

    // Needs more blocks than the disk has: fails part-way through allocating.
    CHECK(throws<DiskFullError>([&] { fs.write_file("/f", string(300 * 4096, 'x')); }));
    CHECK(fs.superblock().free_block_count == free_blocks);
    CHECK(fs.superblock().free_inode_count == free_inodes);
    CHECK(fs.read_file("/f") == "keep me");
    CHECK(fs.fsck(false).clean());

    fs.unmount();
    fs.mount();
    CHECK(fs.superblock().free_block_count == free_blocks);
    CHECK(fs.read_file("/f") == "keep me");
}

// Arms a crash, runs `change`, checks it crashed, then remounts.
RecoveryResult crash_and_remount(FileSystem& fs, CrashPoint point,
                                 const function<void()>& change) {
    fs.arm_crash(point);
    CHECK(throws<SimulatedCrash>(change));
    CHECK(!fs.is_mounted());
    fs.mount();
    return fs.last_recovery();
}

void test_journal_recovery() {
    struct Case {
        CrashPoint point;
        RecoveryResult::Action action;
        bool change_survives;
    };
    const Case cases[] = {
        {CrashPoint::BeforeCommit, RecoveryResult::Action::Discarded, false},
        {CrashPoint::AfterCommit, RecoveryResult::Action::Replayed, true},
        {CrashPoint::MidApply, RecoveryResult::Action::Replayed, true},
    };
    for (const Case& test : cases) {
        VirtualDisk disk(kDiskPath);
        FileSystem fs(disk);
        fs.format(kOneMb);
        fs.make_directory("/d");
        fs.create_file("/d/f");
        fs.write_file("/d/f", "old");

        const RecoveryResult result = crash_and_remount(fs, test.point, [&] {
            fs.write_file("/d/f", string(20000, 'n'));
        });
        CHECK(result.action == test.action);
        CHECK(fs.read_file("/d/f") == (test.change_survives ? string(20000, 'n') : "old"));
        CHECK(fs.fsck(false).clean());

        // Creating a directory also recovers cleanly.
        crash_and_remount(fs, test.point, [&] { fs.make_directory("/d/sub"); });
        bool exists = true;
        try {
            fs.resolve_path("/d/sub");
        } catch (const out_of_range&) {
            exists = false;
        }
        CHECK(exists == test.change_survives);
        CHECK(fs.fsck(false).clean());
    }
}

void test_crash_without_journal_and_fsck() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);
    fs.create_file("/f");
    fs.write_file("/f", "old");

    const RecoveryResult result = crash_and_remount(fs, CrashPoint::NoJournal, [&] {
        fs.write_file("/f", string(20000, 'n'));
    });
    CHECK(result.action == RecoveryResult::Action::Clean);  // nothing to replay
    const FsckReport damaged = fs.fsck(false);
    CHECK(!damaged.clean());

    const FsckReport repaired = fs.fsck(true);
    CHECK(!repaired.repairs.empty());
    CHECK(fs.fsck(false).clean());
    CHECK(fs.read_file("/f") == "old");
}

void test_fsck_finds_orphans_and_bad_links() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);
    fs.make_directory("/d");
    CHECK(fs.fsck(false).clean());

    // An inode that no directory points at, holding one block.
    const uint32_t orphan = fs.allocate_inode(InodeType::File, 0644);
    Inode inode = fs.read_inode(orphan);
    inode.size = 10;
    inode.direct[0] = fs.allocate_block();
    fs.write_inode(orphan, inode);
    // A wrong link count.
    Inode directory = fs.read_inode(fs.resolve_path("/d"));
    directory.link_count = 7;
    fs.write_inode(fs.resolve_path("/d"), directory);
    // A leaked block.
    fs.allocate_block();

    const FsckReport report = fs.fsck(false);
    CHECK(report.problems.size() >= 3);
    fs.fsck(true);
    CHECK(fs.fsck(false).clean());
    CHECK(fs.read_inode(orphan).type == static_cast<uint16_t>(InodeType::Free));
    CHECK(fs.read_inode(fs.resolve_path("/d")).link_count == 2);
    CHECK(fs.superblock().free_block_count == kDataBlocks - 2);
}

void test_defrag() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);
    // Interleave two files, then grow one so it lands in pieces.
    for (int i = 0; i < 8; ++i) {
        fs.create_file("/f" + to_string(i));
        fs.write_file("/f" + to_string(i), string(3 * 4096, 'a' + i));
    }
    for (int i = 0; i < 8; i += 2) {
        fs.remove_file("/f" + to_string(i));
    }
    fs.create_file("/big");
    fs.write_file("/big", string(20 * 4096, 'B'));
    CHECK(fs.fragmentation().fragmented > 0);

    fs.defrag();
    const FragmentationReport after = fs.fragmentation();
    CHECK(after.fragmented == 0);
    CHECK(after.free_extents == 1);
    CHECK(fs.read_file("/big") == string(20 * 4096, 'B'));
    CHECK(fs.read_file("/f3") == string(3 * 4096, 'd'));
    CHECK(fs.fsck(false).clean());
}

void test_defrag_closes_holes() {
    // A fragmented file filling holes between other files, plus a file with
    // an indirect block: after defrag every file is one extent and all free
    // space is one run at the end, under each allocation mode.
    for (AllocationMode mode :
         {AllocationMode::Indexed, AllocationMode::Linked, AllocationMode::Contiguous}) {
        VirtualDisk disk(kDiskPath);
        FileSystem fs(disk);
        fs.format(2 * kOneMb, mode);
        fs.make_directory("/p");
        for (const char* name : {"/p/a", "/p/b", "/p/c", "/p/d"}) {
            fs.create_file(name);
            fs.write_file(name, string(3 * 4096, name[3]));
        }
        fs.create_file("/report");
        fs.write_file("/report", string(30 * 4096, 'r'));
        fs.remove_file("/p/a");
        fs.remove_file("/p/c");
        fs.create_file("/p/big");
        fs.write_file("/p/big", string(10 * 4096, 'B'));

        fs.defrag();
        const FragmentationReport after = fs.fragmentation();
        CHECK(after.fragmented == 0);
        CHECK(after.free_extents == 1);
        CHECK(fs.read_file("/p/big") == string(10 * 4096, 'B'));
        CHECK(fs.read_file("/report") == string(30 * 4096, 'r'));
        CHECK(fs.read_file("/p/d") == string(3 * 4096, 'd'));
        CHECK(fs.fsck(false).clean());
    }
}

void test_export_json() {
    VirtualDisk disk(kDiskPath);
    FileSystem fs(disk);
    fs.format(kOneMb);
    fs.create_file("/a \"quoted\".txt");
    fs.write_file("/a \"quoted\".txt", "hi");
    ostringstream json;
    fs.export_json(json);
    const string text = json.str();
    CHECK(text.find("\"blocks\": \"SBIIJJJJJJJJJJJJJJJJDF") != string::npos);
    CHECK(text.find("\"path\": \"/a \\\"quoted\\\".txt\"") != string::npos);
    CHECK(text.front() == '{');
}

void test_tokenize() {
    CHECK(tokenize("write a.txt hello world") ==
          (vector<string>{"write", "a.txt", "hello", "world"}));
    CHECK(tokenize("write \"my file\" \"line 1\\nline 2\"") ==
          (vector<string>{"write", "my file", "line 1\nline 2"}));
    CHECK(tokenize("  ls   -l  ") == (vector<string>{"ls", "-l"}));
    CHECK(tokenize("write f ''") == (vector<string>{"write", "f", ""}));
    CHECK(throws<invalid_argument>([] { tokenize("write \"unterminated"); }));
}

} // namespace

int main() {
    const pair<const char*, void (*)()> tests[] = {
        {"format layout", test_format_layout},
        {"root directory", test_root_directory},
        {"block allocation", test_block_allocation},
        {"disk full", test_disk_full},
        {"inode allocation", test_inode_allocation},
        {"persistence across mounts", test_persistence},
        {"mount rejects bad disks", test_mount_rejects_bad_disks},
        {"format rejects tiny disk", test_format_rejects_tiny_disk},
        {"100 MB layout", test_large_disk_layout},
        {"directories and paths", test_directories_and_paths},
        {"large directory", test_large_directory},
        {"create file", test_create_file},
        {"write file", test_write_file},
        {"read file", test_read_file},
        {"indirect block", test_indirect_block},
        {"allocation modes", test_allocation_modes},
        {"contiguous external fragmentation", test_contiguous_external_fragmentation},
        {"remove file", test_remove_file},
        {"remove directory", test_remove_directory},
        {"rename entry", test_rename_entry},
        {"copy and move", test_copy_and_move},
        {"permissions", test_permissions},
        {"failed operation rolls back", test_failed_operation_rolls_back},
        {"journal recovery", test_journal_recovery},
        {"crash without journal + fsck", test_crash_without_journal_and_fsck},
        {"fsck orphans and link counts", test_fsck_finds_orphans_and_bad_links},
        {"defrag", test_defrag},
        {"defrag closes holes", test_defrag_closes_holes},
        {"export json", test_export_json},
        {"tokenize", test_tokenize},
    };

    for (const auto& test : tests) {
        const int before = failures;
        try {
            test.second();
        } catch (const exception& error) {
            cerr << "  FAILED with exception: " << error.what() << '\n';
            ++failures;
        }
        cout << (failures == before ? "[pass] " : "[FAIL] ") << test.first << '\n';
    }

    remove(kDiskPath.c_str());
    cout << (failures == 0 ? "All tests passed.\n" : "Some tests failed.\n");
    return failures == 0 ? 0 : 1;
}
