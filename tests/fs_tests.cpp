// Unit checks for the superblock, free-space bitmap, inode table and data
// blocks. Run with `ctest --test-dir build` or `build/minifs_tests`.

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
//   blocks 4-255 data. Block 4 holds the root directory.
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
    CHECK(sb.data_start == 4);
    CHECK(sb.data_block_count == 252);
    CHECK(sb.free_block_count == 251);  // root directory uses one block
    CHECK(sb.free_inode_count == 62);   // inode 0 reserved, inode 1 is root
    CHECK(sb.root_inode == kRootInode);
    CHECK(sb.allocation_mode == static_cast<uint32_t>(AllocationMode::Indexed));

    for (uint32_t block = 0; block <= 4; ++block) {
        CHECK(fs.is_block_used(block));
    }
    CHECK(!fs.is_block_used(5));
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
    CHECK(root.direct[0] == 4);
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
    CHECK(a == 5);
    CHECK(b == 6);
    CHECK(fs.superblock().free_block_count == 249);

    fs.free_block(a);
    CHECK(!fs.is_block_used(a));
    CHECK(fs.superblock().free_block_count == 250);
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
    CHECK(allocated == 251);
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
    CHECK(fs.superblock().free_block_count == 250);
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
    // 3 blocks: superblock + bitmap + inode table leaves no data block.
    CHECK(throws<invalid_argument>([&] { fs.format(3 * 4096); }));
    fs.format(4 * 4096);  // smallest disk that fits the root directory block
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
    CHECK(sb.data_start == 202);
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
    CHECK(fs.directory_empty(b));

    fs.change_directory("/a/b");
    CHECK(fs.current_directory() == b);
    CHECK(fs.working_directory_path() == "/a/b");
    CHECK(fs.resolve_path(".") == b);
    CHECK(fs.resolve_path("../..") == kRootInode);
    CHECK(fs.resolve_path("../../a") == a);

    CHECK(throws<logic_error>([&] { fs.make_directory("/a"); }));
    CHECK(throws<out_of_range>([&] { fs.resolve_path("/missing"); }));
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
    fs.unmount();
    fs.mount();
    inode = fs.read_inode(file);
    CHECK(inode.size == 10);
    data = fs.read_data_block(inode.direct[0]);
    CHECK(string(data.begin(), data.begin() + 10) == "persistent");
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
        {"create file", test_create_file},
        {"write file", test_write_file},
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
