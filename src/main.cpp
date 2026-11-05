#include "minifs/cli.hpp"
#include "minifs/file_system.hpp"
#include "minifs/fs_errors.hpp"
#include "minifs/virtual_disk.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
using namespace std;
using namespace minifs;

namespace minifs {

namespace {

// Tier 1 implementation roadmap:
// 1. Create a VirtualDisk class that formats, opens, reads, and writes
//    fixed-size blocks in virtual_disk.bin. [implemented]
// 2. Add a Superblock containing the disk signature, block size, total
//    block count, metadata locations, and the root inode number. [implemented]
// 3. Add a free-space bitmap so each data block can be marked free or used.
//    [implemented]
// 4. Add an inode table for file metadata such as type, size, permissions,
//    timestamps, and references to allocated data blocks. [implemented]
// 5. Create the root directory [implemented] and support path lookup from
//    that inode.
// 6. Connect the first commands to the file-system layer:
//    format, mkdir, ls, touch, write, cat, and rm.
//
// Keep storage, metadata, and command parsing in separate classes. The CLI
// should translate user input into file-system operations instead of managing
// disk blocks directly.

void print_help(ostream& output) {
    output << "Available commands:\n"
           << "  help       Show this help message\n"
           << "  version    Show the MiniFS version\n"
           << "  format [MB] Create or reset virtual_disk.bin (default: 100 MB)\n"
           << "  diskinfo   Show the disk layout and free space\n"
           << "  exit       Exit MiniFS\n";
}

bool file_exists(const string& path) {
    return ifstream(path, ios::binary).good();
}

void print_disk_info(const FileSystem& fs, ostream& output) {
    const Superblock& sb = fs.superblock();
    // e.g. "blocks 2-21 (20 blocks" or "block 1 (1 block"
    const auto range = [](uint32_t first, uint32_t count) {
        if (count == 1) {
            return "block " + to_string(first) + " (1 block";
        }
        return "blocks " + to_string(first) + "-" +
               to_string(first + count - 1) + " (" + to_string(count) +
               " blocks";
    };

    output << "Path: " << fs.disk().path() << '\n'
           << "Disk size: " << fs.disk().disk_size() / (1024 * 1024) << " MB ("
           << sb.total_blocks << " blocks x " << sb.block_size << " bytes)\n"
           << "Layout:\n"
           << "  Superblock    block " << kSuperblockBlock << '\n'
           << "  Free bitmap   " << range(sb.bitmap_start, sb.bitmap_blocks)
           << ")\n"
           << "  Inode table   "
           << range(sb.inode_table_start, sb.inode_table_blocks) << ", "
           << sb.inode_count << " inodes)\n"
           << "  Data blocks   " << range(sb.data_start, sb.data_block_count)
           << ")\n"
           << "Free data blocks: " << sb.free_block_count << " / "
           << sb.data_block_count << '\n'
           << "Free inodes: " << sb.free_inode_count << " / "
           << sb.inode_count - 1 << '\n'
           << "Root inode: " << sb.root_inode << '\n'
           << "Allocation mode: "
           << allocation_mode_name(static_cast<AllocationMode>(sb.allocation_mode))
           << '\n';
}

} // namespace

void Cli::run(istream& input, ostream& output) {
    output << "MiniFS 0.1.0\n"
           << "Type 'help' for available commands.\n";

    VirtualDisk disk("virtual_disk.bin");
    FileSystem fs(disk);
    if (file_exists(disk.path())) {
        try {
            fs.mount();
            output << "Mounted " << disk.path() << ".\n";
        } catch (const exception& error) {
            fs.unmount();
            output << "Could not mount " << disk.path() << ": " << error.what()
                   << '\n';
        }
    } else {
        output << "No virtual disk found. Run 'format' to create one.\n";
    }

    string command;
    while (output << "minifs> " && getline(input, command)) {
        istringstream command_stream(command);
        string name;
        command_stream >> name;

        try {
            if (name == "help") {
                print_help(output);
            } else if (name == "version") {
                output << "MiniFS 0.1.0\n";
            } else if (name == "format") {
                size_t size_in_mb = 100;
                string extra_argument;
                if (command_stream >> extra_argument) {
                    size_in_mb = stoull(extra_argument);
                }
                if (size_in_mb == 0) {
                    throw invalid_argument("Disk size must be greater than zero.");
                }

                fs.format(size_in_mb * 1024ULL * 1024ULL);
                output << "Formatted " << disk.path() << " ("
                       << size_in_mb << " MB, " << fs.superblock().total_blocks
                       << " blocks, " << fs.superblock().inode_count
                       << " inodes).\n";
            } else if (name == "diskinfo") {
                if (!fs.is_mounted()) {
                    if (!file_exists(disk.path())) {
                        throw NotMountedError();
                    }
                    fs.mount();
                }
                print_disk_info(fs, output);
            } else if (name == "exit" || name == "quit") {
                break;
            } else if (!name.empty()) {
                output << "Unknown command: " << name << '\n';
            }
        }
        catch (const exception& error) {
            output << "Error: " << error.what() << '\n';
        }
    }
}

} // namespace minifs

int main() {
    Cli cli;
    cli.run(cin, cout);
    return 0;
}
