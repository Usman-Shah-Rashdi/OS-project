#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using namespace std;

namespace minifs {

class VirtualDisk {
public:
    static constexpr size_t DefaultDiskSize = 100ULL * 1024 * 1024;
    static constexpr size_t DefaultBlockSize = 4096;

    explicit VirtualDisk(string path,
                         size_t block_size = DefaultBlockSize);
    ~VirtualDisk();

    VirtualDisk(const VirtualDisk&) = delete;
    VirtualDisk& operator=(const VirtualDisk&) = delete;

    void format(size_t disk_size = DefaultDiskSize);
    void open();
    void close();

    vector<uint8_t> read_block(size_t block_number);
    void write_block(size_t block_number, const vector<uint8_t>& data);

    bool is_open() const;
    size_t block_size() const;
    size_t block_count() const;
    size_t disk_size() const;
    const string& path() const;

private:
    void check_block_number(size_t block_number) const;

    string path_;
    fstream file_;
    size_t block_size_;
    size_t block_count_ = 0;
};

} // namespace minifs