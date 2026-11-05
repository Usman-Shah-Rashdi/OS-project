#include "minifs/block_bitmap.hpp"

#include "minifs/fs_errors.hpp"

#include <algorithm>
#include <stdexcept>

using namespace std;

namespace minifs {

BlockBitmap::BlockBitmap(VirtualDisk& disk, const Superblock& superblock)
    : disk_(disk), sb_(superblock) {}

void BlockBitmap::create() {
    bits_.assign(static_cast<size_t>(sb_.bitmap_blocks) * sb_.block_size, 0);
    for (uint32_t block = 0; block < sb_.data_start; ++block) {
        set(block, true);
    }
    for (uint32_t i = 0; i < sb_.bitmap_blocks; ++i) {
        const auto first = bits_.begin() + static_cast<ptrdiff_t>(i) * sb_.block_size;
        disk_.write_block(sb_.bitmap_start + i,
                          vector<uint8_t>(first, first + sb_.block_size));
    }
}

void BlockBitmap::load() {
    bits_.clear();
    bits_.reserve(static_cast<size_t>(sb_.bitmap_blocks) * sb_.block_size);
    for (uint32_t i = 0; i < sb_.bitmap_blocks; ++i) {
        const vector<uint8_t> block = disk_.read_block(sb_.bitmap_start + i);
        bits_.insert(bits_.end(), block.begin(), block.end());
    }
}

bool BlockBitmap::is_used(uint32_t block) const {
    check_block(block);
    return (bits_[block / 8] >> (block % 8)) & 1u;
}

uint32_t BlockBitmap::allocate() {
    for (uint32_t block = sb_.data_start; block < sb_.total_blocks; ++block) {
        // Skip whole bytes that are already full.
        if (block % 8 == 0 && bits_[block / 8] == 0xFF &&
            block + 8 <= sb_.total_blocks) {
            block += 7;
            continue;
        }
        if (!is_used(block)) {
            set(block, true);
            write_back(block);
            return block;
        }
    }
    throw DiskFullError();
}

void BlockBitmap::free(uint32_t block) {
    check_block(block);
    if (block < sb_.data_start) {
        throw invalid_argument("Cannot free metadata block " + to_string(block) + ".");
    }
    if (!is_used(block)) {
        throw logic_error("Block " + to_string(block) + " is already free.");
    }
    set(block, false);
    write_back(block);
}

uint32_t BlockBitmap::count_free_data_blocks() const {
    uint32_t free_blocks = 0;
    for (uint32_t block = sb_.data_start; block < sb_.total_blocks; ++block) {
        if (!is_used(block)) {
            ++free_blocks;
        }
    }
    return free_blocks;
}

void BlockBitmap::set(uint32_t block, bool used) {
    const uint8_t mask = static_cast<uint8_t>(1u << (block % 8));
    if (used) {
        bits_[block / 8] |= mask;
    } else {
        bits_[block / 8] &= static_cast<uint8_t>(~mask);
    }
}

void BlockBitmap::write_back(uint32_t block) {
    const size_t bits_per_block = static_cast<size_t>(sb_.block_size) * 8;
    const size_t bitmap_block = block / bits_per_block;
    const auto first = bits_.begin() +
                       static_cast<ptrdiff_t>(bitmap_block * sb_.block_size);
    disk_.write_block(sb_.bitmap_start + bitmap_block,
                      vector<uint8_t>(first, first + sb_.block_size));
}

void BlockBitmap::check_block(uint32_t block) const {
    if (block >= sb_.total_blocks) {
        throw out_of_range("Block " + to_string(block) + " is out of range.");
    }
}

} // namespace minifs
