#include "minifs/journal.hpp"

#include "minifs/fs_errors.hpp"

#include <algorithm>
#include <cstring>

using namespace std;

namespace minifs {

const char* crash_point_name(CrashPoint point) {
    switch (point) {
    case CrashPoint::None: return "none";
    case CrashPoint::BeforeCommit: return "before-commit";
    case CrashPoint::AfterCommit: return "after-commit";
    case CrashPoint::MidApply: return "mid-apply";
    case CrashPoint::NoJournal: return "no-journal";
    }
    return "unknown";
}

bool parse_crash_point(const string& text, CrashPoint& point) {
    for (CrashPoint candidate : {CrashPoint::None, CrashPoint::BeforeCommit,
                                 CrashPoint::AfterCommit, CrashPoint::MidApply,
                                 CrashPoint::NoJournal}) {
        if (text == crash_point_name(candidate)) {
            point = candidate;
            return true;
        }
    }
    return false;
}

Journal::Journal(VirtualDisk& disk, const Superblock& superblock)
    : disk_(disk), sb_(superblock) {}

void Journal::create() {
    JournalHeader header{};
    header.magic = kJournalMagic;
    header.state = static_cast<uint32_t>(JournalState::Empty);
    write_header(header, {});
}

size_t Journal::capacity() const {
    const size_t targets_per_header =
        (sb_.block_size - sizeof(JournalHeader)) / sizeof(uint32_t);
    return min<size_t>(sb_.journal_blocks - 1, targets_per_header);
}

JournalHeader Journal::read_header() {
    const vector<uint8_t> block = disk_.read_block(sb_.journal_start);
    JournalHeader header;
    memcpy(&header, block.data(), sizeof(header));
    return header;
}

RecoveryResult Journal::recover() {
    RecoveryResult result;
    const vector<uint8_t> header_block = disk_.read_block(sb_.journal_start);
    JournalHeader header;
    memcpy(&header, header_block.data(), sizeof(header));

    if (header.magic != kJournalMagic) {
        result.action = RecoveryResult::Action::Discarded;
        result.reason = "journal header was unreadable";
        clear(0);
        return result;
    }
    result.sequence = header.sequence;
    result.blocks = header.block_count;

    const auto state = static_cast<JournalState>(header.state);
    if (state == JournalState::Empty) {
        return result;
    }
    if (state == JournalState::Pending) {
        result.action = RecoveryResult::Action::Discarded;
        result.reason = "transaction was never committed";
        clear(header.sequence);
        return result;
    }
    if (state != JournalState::Committed || header.block_count > capacity()) {
        result.action = RecoveryResult::Action::Discarded;
        result.reason = "journal header was corrupt";
        clear(header.sequence);
        return result;
    }

    const vector<uint32_t> targets = read_targets(header_block, header.block_count);
    vector<vector<uint8_t>> images;
    images.reserve(targets.size());
    for (uint32_t i = 0; i < header.block_count; ++i) {
        images.push_back(disk_.read_block(sb_.journal_start + 1 + i));
    }
    const bool targets_valid =
        all_of(targets.begin(), targets.end(), [this](uint32_t target) {
            return target < sb_.total_blocks &&
                   (target < sb_.journal_start ||
                    target >= sb_.journal_start + sb_.journal_blocks);
        });
    if (!targets_valid || checksum(targets, images) != header.checksum) {
        result.action = RecoveryResult::Action::Discarded;
        result.reason = "journal checksum did not match";
        clear(header.sequence);
        return result;
    }

    // Replaying is idempotent: writing the same images twice is harmless, so
    // a crash during recovery just means recovery runs again next mount.
    for (size_t i = 0; i < targets.size(); ++i) {
        disk_.write_block_direct(targets[i], images[i]);
    }
    clear(header.sequence);
    result.action = RecoveryResult::Action::Replayed;
    return result;
}

void Journal::commit(const map<size_t, vector<uint8_t>>& blocks,
                     CrashPoint crash) {
    if (blocks.empty()) {
        return;
    }
    if (blocks.size() > capacity()) {
        throw FsError("Operation changes " + to_string(blocks.size()) +
                      " metadata blocks, but the journal holds only " +
                      to_string(capacity()) + ".");
    }

    vector<uint32_t> targets;
    vector<vector<uint8_t>> images;
    for (const auto& block : blocks) {
        targets.push_back(static_cast<uint32_t>(block.first));
        images.push_back(block.second);
    }

    if (crash == CrashPoint::NoJournal) {
        // What happens without journaling: the writes go straight to their
        // real blocks and power is lost halfway through.
        for (size_t i = 0; i < targets.size() / 2 + targets.size() % 2; ++i) {
            disk_.write_block_direct(targets[i], images[i]);
        }
        throw SimulatedCrash("halfway through writing metadata without the journal");
    }

    const JournalHeader previous = read_header();
    JournalHeader header{};
    header.magic = kJournalMagic;
    header.sequence = previous.magic == kJournalMagic ? previous.sequence + 1 : 1;
    header.block_count = static_cast<uint32_t>(targets.size());
    header.checksum = checksum(targets, images);

    // Step 1: record the intended changes.
    for (size_t i = 0; i < images.size(); ++i) {
        disk_.write_block_direct(sb_.journal_start + 1 + i, images[i]);
    }
    header.state = static_cast<uint32_t>(JournalState::Pending);
    write_header(header, targets);
    if (crash == CrashPoint::BeforeCommit) {
        throw SimulatedCrash("after writing the journal, before the commit record");
    }

    // Step 2: commit.
    header.state = static_cast<uint32_t>(JournalState::Committed);
    write_header(header, targets);
    if (crash == CrashPoint::AfterCommit) {
        throw SimulatedCrash("after the journal commit, before applying any change");
    }

    // Step 3: apply.
    for (size_t i = 0; i < targets.size(); ++i) {
        if (crash == CrashPoint::MidApply && i == targets.size() / 2) {
            throw SimulatedCrash("while applying the committed changes (" +
                                 to_string(i) + " of " +
                                 to_string(targets.size()) + " blocks written)");
        }
        disk_.write_block_direct(targets[i], images[i]);
    }

    // Step 4: clear.
    clear(header.sequence);
}

vector<uint32_t> Journal::read_targets(const vector<uint8_t>& block,
                                       uint32_t count) const {
    vector<uint32_t> targets(count);
    memcpy(targets.data(), block.data() + sizeof(JournalHeader),
           count * sizeof(uint32_t));
    return targets;
}

void Journal::write_header(const JournalHeader& header,
                           const vector<uint32_t>& targets) {
    vector<uint8_t> block(sb_.block_size, 0);
    memcpy(block.data(), &header, sizeof(header));
    if (!targets.empty()) {
        memcpy(block.data() + sizeof(header), targets.data(),
               targets.size() * sizeof(uint32_t));
    }
    disk_.write_block_direct(sb_.journal_start, block);
}

void Journal::clear(uint32_t sequence) {
    JournalHeader header{};
    header.magic = kJournalMagic;
    header.state = static_cast<uint32_t>(JournalState::Empty);
    header.sequence = sequence;
    write_header(header, {});
}

uint32_t Journal::checksum(const vector<uint32_t>& targets,
                           const vector<vector<uint8_t>>& images) {
    uint32_t hash = 2166136261u;
    const auto mix = [&hash](const uint8_t* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            hash ^= data[i];
            hash *= 16777619u;
        }
    };
    mix(reinterpret_cast<const uint8_t*>(targets.data()),
        targets.size() * sizeof(uint32_t));
    for (const vector<uint8_t>& image : images) {
        mix(image.data(), image.size());
    }
    return hash;
}

} // namespace minifs
