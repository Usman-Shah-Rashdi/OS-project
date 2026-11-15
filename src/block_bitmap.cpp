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

uint32_t BlockBitmap::find_run(uint32_t count) const {
    return find_run_from(sb_.data_start, count);
}

void BlockBitmap::allocate_range(uint32_t start, uint32_t count) {
    if (count == 0 || start < sb_.data_start || uint64_t{start} + count > sb_.total_blocks) {
        throw out_of_range("Block range is outside the data region.");
    }
    for (uint32_t block = start; block < start + count; ++block) {
        if (is_used(block)) {
            throw logic_error("Block " + to_string(block) + " is already in use.");
        }
    }
    for (uint32_t block = start; block < start + count; ++block) {
        set(block, true);
    }
    const uint32_t bits_per_block = sb_.block_size * 8;
    for (uint32_t block = start; block < start + count;
         block = (block / bits_per_block + 1) * bits_per_block) {
        write_back(block);
    }
}

uint32_t BlockBitmap::find_run_from(uint32_t min_start, uint32_t count) const {
    if (count == 0) {
        return kNullBlock;
    }
    const uint32_t first = max(min_start, sb_.data_start);
    uint32_t run_start = first;
    uint32_t run_length = 0;
    for (uint32_t block = first; block < sb_.total_blocks; ++block) {
        if (is_used(block)) {
            run_length = 0;
            run_start = block + 1;
            continue;
        }
        if (++run_length == count) {
            return run_start;
        }
    }
    return kNullBlock;
}

uint32_t BlockBitmap::allocate_run(uint32_t count) {
    if (count == 0) {
        throw invalid_argument("Cannot allocate an empty run of blocks.");
    }
    const uint32_t start = find_run(count);
    if (start == kNullBlock) {
        if (count_free_data_blocks() < count) {
            throw DiskFullError();
        }
        throw NoContiguousSpaceError(count);
    }
    allocate_range(start, count);
    return start;
}

void BlockBitmap::assign(const vector<bool>& used) {
    if (used.size() != sb_.total_blocks) {
        throw invalid_argument("Bitmap replacement has the wrong size.");
    }
    bits_.assign(static_cast<size_t>(sb_.bitmap_blocks) * sb_.block_size, 0);
    for (uint32_t block = 0; block < sb_.total_blocks; ++block) {
        set(block, used[block] || block < sb_.data_start);
    }
    for (uint32_t i = 0; i < sb_.bitmap_blocks; ++i) {
        write_back(i * sb_.block_size * 8);
    }
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
