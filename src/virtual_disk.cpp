#include "minifs/virtual_disk.hpp"

#include <stdexcept>
#include <utility>

using namespace std;

namespace minifs {

VirtualDisk::VirtualDisk(string path, size_t block_size)
    : path_(move(path)), block_size_(block_size) {
    if (block_size_ == 0) {
        throw invalid_argument("Block size must be greater than zero.");
    }
}

VirtualDisk::~VirtualDisk() {
    close();
}

void VirtualDisk::format(size_t disk_size) {
    if (disk_size == 0 || disk_size % block_size_ != 0) {
        throw invalid_argument("Disk size must be a non-zero multiple of the block size.");
    }

    close();

    ofstream output(path_, ios::binary | ios::trunc);
    if (!output) {
        throw runtime_error("Unable to create virtual disk: " + path_);
    }

    output.seekp(static_cast<streamoff>(disk_size - 1));
    const char empty_byte = '\0';
    output.write(&empty_byte, 1);
    if (!output) {
        throw runtime_error("Unable to allocate virtual disk space: " + path_);
    }
    output.close();

    block_count_ = disk_size / block_size_;
    open();
}

void VirtualDisk::open() {
    close();

    file_.open(path_, ios::binary | ios::in | ios::out);
    if (!file_) {
        throw runtime_error("Unable to open virtual disk: " + path_);
    }

    file_.seekg(0, ios::end);
    const streamoff file_size = file_.tellg();
    if (file_size <= 0 ||
        file_size % static_cast<streamoff>(block_size_) != 0) {
        close();
        throw runtime_error("Virtual disk size is invalid: " + path_);
    }

    block_count_ = static_cast<size_t>(file_size) / block_size_;
    file_.seekg(0, ios::beg);
    file_.seekp(0, ios::beg);
}

void VirtualDisk::close() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
    block_count_ = 0;
}

vector<uint8_t> VirtualDisk::read_block(size_t block_number) {
    check_block_number(block_number);

    vector<uint8_t> data(block_size_);
    file_.seekg(static_cast<streamoff>(block_number * block_size_));
    file_.read(reinterpret_cast<char*>(data.data()),
               static_cast<streamsize>(data.size()));
    if (!file_) {
        throw runtime_error("Unable to read virtual disk block.");
    }
    return data;
}

void VirtualDisk::write_block(size_t block_number,
                              const vector<uint8_t>& data) {
    check_block_number(block_number);
    if (data.size() != block_size_) {
        throw invalid_argument("Block data must match the configured block size.");
    }

    file_.seekp(static_cast<streamoff>(block_number * block_size_));
    file_.write(reinterpret_cast<const char*>(data.data()),
                static_cast<streamsize>(data.size()));
    file_.flush();
    if (!file_) {
        throw runtime_error("Unable to write virtual disk block.");
    }
}

bool VirtualDisk::is_open() const {
    return file_.is_open();
}

size_t VirtualDisk::block_size() const {
    return block_size_;
}

size_t VirtualDisk::block_count() const {
    return block_count_;
}

size_t VirtualDisk::disk_size() const {
    return block_count_ * block_size_;
}

const string& VirtualDisk::path() const {
    return path_;
}

void VirtualDisk::check_block_number(size_t block_number) const {
    if (!is_open()) {
        throw runtime_error("Virtual disk is not open.");
    }
    if (block_number >= block_count_) {
        throw out_of_range("Virtual disk block number is out of range.");
    }
}

} // namespace minifs