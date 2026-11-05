#include "minifs/cli.hpp"
#include "minifs/virtual_disk.hpp"

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
//    block count, metadata locations, and the root inode number.
// 3. Add a free-space bitmap so each data block can be marked free or used.
// 4. Add an inode table for file metadata such as type, size, permissions,
//    timestamps, and references to allocated data blocks.
// 5. Create the root directory and support path lookup from that inode.
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
           << "  diskinfo   Show virtual disk information\n"
           << "  exit       Exit MiniFS\n";
}

} // namespace

void Cli::run(istream& input, ostream& output) {
    output << "MiniFS 0.1.0\n"
           << "Type 'help' for available commands.\n";

    VirtualDisk disk("virtual_disk.bin");
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

                disk.format(size_in_mb * 1024ULL * 1024ULL);
                output << "Formatted " << disk.path() << " ("
                       << size_in_mb << " MB, " << disk.block_count()
                       << " blocks).\n";
            } else if (name == "diskinfo") {
                if (!disk.is_open()) {
                    disk.open();
                }
                output << "Path: " << disk.path() << '\n'
                       << "Disk size: " << disk.disk_size() / (1024 * 1024)
                       << " MB\n"
                       << "Block size: " << disk.block_size() << " bytes\n"
                       << "Blocks: " << disk.block_count() << '\n';
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
