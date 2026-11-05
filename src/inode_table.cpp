#include "minifs/inode_table.hpp"

#include "minifs/fs_errors.hpp"

#include <cstring>
#include <ctime>
#include <stdexcept>
#include <vector>

using namespace std;

namespace minifs {

InodeTable::InodeTable(VirtualDisk& disk, const Superblock& superblock)
    : disk_(disk), sb_(superblock) {}

void InodeTable::create() {
    const vector<uint8_t> zeros(sb_.block_size, 0);
    for (uint32_t i = 0; i < sb_.inode_table_blocks; ++i) {
        disk_.write_block(sb_.inode_table_start + i, zeros);
    }
}

Inode InodeTable::read(uint32_t number) {
    check_number(number);
    const vector<uint8_t> block =
        disk_.read_block(sb_.inode_table_start + number / inodes_per_block());
    Inode inode;
    memcpy(&inode, block.data() + (number % inodes_per_block()) * sizeof(Inode),
           sizeof(Inode));
    return inode;
}

void InodeTable::write(uint32_t number, const Inode& inode) {
    check_number(number);
    const uint32_t block_number =
        sb_.inode_table_start + number / inodes_per_block();
    vector<uint8_t> block = disk_.read_block(block_number);
    memcpy(block.data() + (number % inodes_per_block()) * sizeof(Inode), &inode,
           sizeof(Inode));
    disk_.write_block(block_number, block);
}

uint32_t InodeTable::allocate(InodeType type, uint16_t mode) {
    if (type == InodeType::Free) {
        throw invalid_argument("Cannot allocate an inode of type Free.");
    }

    const uint32_t per_block = inodes_per_block();
    for (uint32_t i = 0; i < sb_.inode_table_blocks; ++i) {
        const vector<uint8_t> block = disk_.read_block(sb_.inode_table_start + i);
        for (uint32_t slot = 0; slot < per_block; ++slot) {
            const uint32_t number = i * per_block + slot;
            if (number == kNullInode) {
                continue;
            }
            Inode existing;
            memcpy(&existing, block.data() + slot * sizeof(Inode), sizeof(Inode));
            if (existing.type != static_cast<uint16_t>(InodeType::Free)) {
                continue;
            }

            Inode inode{};
            inode.type = static_cast<uint16_t>(type);
            inode.mode = mode;
            inode.link_count = 1;
            const int64_t now = static_cast<int64_t>(time(nullptr));
            inode.created_at = now;
            inode.modified_at = now;
            inode.accessed_at = now;
            write(number, inode);
            return number;
        }
    }
    throw NoFreeInodesError();
}

void InodeTable::free(uint32_t number) {
    check_number(number);
    if (number == kNullInode) {
        throw invalid_argument("Inode 0 is reserved and cannot be freed.");
    }
    if (read(number).type == static_cast<uint16_t>(InodeType::Free)) {
        throw logic_error("Inode " + to_string(number) + " is already free.");
    }
    write(number, Inode{});
}

void InodeTable::check_number(uint32_t number) const {
    if (number >= sb_.inode_count) {
        throw out_of_range("Inode " + to_string(number) + " is out of range.");
    }
}

uint32_t InodeTable::inodes_per_block() const {
    return sb_.block_size / static_cast<uint32_t>(sizeof(Inode));
}

} // namespace minifs
