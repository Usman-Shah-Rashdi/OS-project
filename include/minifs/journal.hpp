#pragma once

#include "minifs/fs_types.hpp"
#include "minifs/virtual_disk.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace minifs {

// Where the 'crash' command stops a transaction.
enum class CrashPoint {
    None,
    BeforeCommit,  // block images written to the journal, not committed
    AfterCommit,   // journal committed, nothing applied to the real blocks
    MidApply,      // committed, about half of the blocks applied
    NoJournal,     // journal bypassed, half of the blocks written directly
};

const char* crash_point_name(CrashPoint point);
// Accepts the names printed by crash_point_name(). Returns false if unknown.
bool parse_crash_point(const std::string& text, CrashPoint& point);

struct RecoveryResult {
    enum class Action { Clean, Replayed, Discarded };
    Action action = Action::Clean;
    std::uint32_t sequence = 0;
    std::uint32_t blocks = 0;
    std::string reason;  // why a transaction was discarded
};

// Write-ahead journal for metadata blocks.
//
// A transaction is a set of whole-block images. commit() follows the four
// steps from the design:
//   1. write the block images and a Pending header to the journal,
//   2. mark the header Committed,
//   3. copy each image to its real location,
//   4. mark the header Empty.
// recover() runs at mount: a Committed transaction is replayed (step 3 again),
// a Pending one is discarded because it was never committed.
class Journal {
public:
    Journal(VirtualDisk& disk, const Superblock& superblock);

    // Format: write an empty header.
    void create();
    RecoveryResult recover();
    void commit(const std::map<std::size_t, std::vector<std::uint8_t>>& blocks,
                CrashPoint crash = CrashPoint::None);

    // Largest number of blocks one transaction can hold.
    std::size_t capacity() const;
    JournalHeader read_header();

private:
    std::vector<std::uint32_t> read_targets(const std::vector<std::uint8_t>& block,
                                            std::uint32_t count) const;
    void write_header(const JournalHeader& header,
                      const std::vector<std::uint32_t>& targets);
    void clear(std::uint32_t sequence);
    static std::uint32_t checksum(
        const std::vector<std::uint32_t>& targets,
        const std::vector<std::vector<std::uint8_t>>& images);

    VirtualDisk& disk_;
    const Superblock& sb_;
};

} // namespace minifs
