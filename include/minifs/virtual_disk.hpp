#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace minifs {

// The disk layer: fixed-size blocks stored in one host file.
//
// While buffering is on (during a journaled transaction), write_block() keeps
// blocks in memory instead of writing them, and read_block() returns the
// buffered copy if there is one. write_block_direct() always goes straight to
// the file; it is used for blocks that no committed metadata points at yet.
class VirtualDisk {
public:
    static constexpr std::size_t DefaultDiskSize = 100ULL * 1024 * 1024;
    static constexpr std::size_t DefaultBlockSize = 4096;

    explicit VirtualDisk(std::string path,
                         std::size_t block_size = DefaultBlockSize);
    ~VirtualDisk();

    VirtualDisk(const VirtualDisk&) = delete;
    VirtualDisk& operator=(const VirtualDisk&) = delete;

    void format(std::size_t disk_size = DefaultDiskSize);
    void open();
    void close();

    std::vector<std::uint8_t> read_block(std::size_t block_number);
    void write_block(std::size_t block_number,
                     const std::vector<std::uint8_t>& data);
    void write_block_direct(std::size_t block_number,
                            const std::vector<std::uint8_t>& data);

    void begin_buffering();
    bool is_buffering() const;
    // Stops buffering and returns the buffered blocks, ordered by block number.
    std::map<std::size_t, std::vector<std::uint8_t>> take_buffer();
    // Stops buffering and forgets the buffered blocks.
    void discard_buffer();

    bool is_open() const;
    std::size_t block_size() const;
    std::size_t block_count() const;
    std::size_t disk_size() const;
    const std::string& path() const;

private:
    void check_block_number(std::size_t block_number) const;
    void check_block_data(const std::vector<std::uint8_t>& data) const;

    std::string path_;
    std::fstream file_;
    std::size_t block_size_;
    std::size_t block_count_ = 0;
    bool buffering_ = false;
    std::map<std::size_t, std::vector<std::uint8_t>> buffer_;
};

} // namespace minifs
