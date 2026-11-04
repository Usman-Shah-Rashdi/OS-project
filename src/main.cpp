

#include "minifs/cli.hpp"

#include <iostream>
#include <string>
using namespace std;
using namespace minifs;

namespace minifs {

namespace {

// Tier 1 implementation roadmap:
// 1. Create a VirtualDisk class that formats, opens, reads, and writes
//    fixed-size blocks in virtual_disk.bin.
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
           << "  exit       Exit MiniFS\n";
}

} // namespace

void Cli::run(istream& input, ostream& output) {
    output << "MiniFS 0.1.0\n"
           << "Type 'help' for available commands.\n";

    string command;
    while (output << "minifs> " && getline(input, command)) {
        if (command == "help") {
            print_help(output);
        } else if (command == "version") {
            output << "MiniFS 0.1.0\n";
        } else if (command == "exit" || command == "quit") {
            break;
        } else if (!command.empty()) {
            output << "Unknown command: " << command << '\n';
        }
    }
}

} // namespace minifs

int main() {
    Cli cli;
    cli.run(cin, cout);
    return 0;
}
