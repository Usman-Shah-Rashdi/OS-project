#include "minifs/cli.hpp"

#include "minifs/file_system.hpp"
#include "minifs/fs_errors.hpp"
#include "minifs/virtual_disk.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

namespace minifs {

namespace {

const char* const kVersion = "MiniFS 1.0.0";
constexpr size_t kMaxDiskMb = 1024;
constexpr size_t kMaxFillKb = 64 * 1024;
constexpr size_t kMapColumns = 64;

bool file_exists(const string& path) {
    return ifstream(path, ios::binary).good();
}

string format_time(int64_t seconds) {
    const time_t value = static_cast<time_t>(seconds);
    tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &value);
#else
    localtime_r(&value, &parts);
#endif
    char text[32];
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &parts);
    return text;
}

string format_bytes(uint64_t bytes) {
    ostringstream out;
    if (bytes < 1024) {
        out << bytes << " B";
    } else if (bytes < 1024 * 1024) {
        out << fixed << setprecision(1) << bytes / 1024.0 << " KB";
    } else {
        out << fixed << setprecision(1) << bytes / (1024.0 * 1024.0) << " MB";
    }
    return out.str();
}

string percent(double fraction) {
    ostringstream out;
    out << fixed << setprecision(1) << fraction * 100.0 << '%';
    return out.str();
}

string octal(uint16_t mode) {
    ostringstream out;
    out << oct << setw(3) << setfill('0') << mode;
    return out.str();
}

bool is_directory(const Inode& inode) {
    return inode.type == static_cast<uint16_t>(InodeType::Directory);
}

size_t parse_number(const string& text, const string& what, size_t low, size_t high) {
    if (text.empty() || text.size() > 9 || text.find_first_not_of("0123456789") != string::npos) {
        throw invalid_argument(what + " must be a whole number, not '" + text + "'.");
    }
    const size_t value = stoul(text);
    if (value < low || value > high) {
        throw invalid_argument(what + " must be between " + to_string(low) + " and " +
                               to_string(high) + ".");
    }
    return value;
}

// Sample content for 'fill': numbered lines, so 'read' output shows where
// one block ends and the next begins.
string sample_content(size_t bytes) {
    string content;
    content.reserve(bytes + 64);
    for (size_t line = 1; content.size() < bytes; ++line) {
        ostringstream text;
        text << "MiniFS sample line " << setw(6) << setfill('0') << line << '\n';
        content += text.str();
    }
    content.resize(bytes);
    return content;
}

// ---------------------------------------------------------------------------
// Allocation benchmark
// ---------------------------------------------------------------------------

struct BenchResult {
    AllocationMode mode;
    size_t written = 0;
    size_t failed = 0;
    uint32_t used_blocks = 0;
    FragmentationReport frag;
    double reads_per_block = 0;
    double milliseconds = 0;
    string big_file;
};

// The same workload under one allocation mode on a scratch disk: write files,
// delete every other one, grow the survivors, then write bigger files into
// the holes.
BenchResult run_benchmark(AllocationMode mode, const string& path) {
    BenchResult result;
    result.mode = mode;
    VirtualDisk disk(path);
    FileSystem fs(disk);
    fs.format(8 * 1024 * 1024, mode);

    uint32_t seed = 12345;
    const auto random = [&seed](uint32_t low, uint32_t high) {
        seed = seed * 1103515245u + 12345u;
        return low + (seed >> 16) % (high - low + 1);
    };
    const auto write = [&](const string& name, size_t bytes) {
        try {
            fs.create_file(name);
            fs.write_file(name, string(bytes, 'x'));
            ++result.written;
        } catch (const FsError&) {
            ++result.failed;
        }
    };

    const auto start = chrono::steady_clock::now();
    for (int i = 0; i < 80; ++i) {
        write("/a" + to_string(i), random(1, 16) * 4096 - random(0, 4000));
    }
    for (int i = 1; i < 80; i += 2) {
        fs.remove_file("/a" + to_string(i));
    }
    for (int i = 0; i < 80; i += 4) {
        try {
            fs.append_file("/a" + to_string(i), string(random(1, 3) * 4096, 'y'));
        } catch (const FsError&) {
            ++result.failed;
        }
    }
    for (int i = 0; i < 50; ++i) {
        write("/b" + to_string(i), random(10, 50) * 4096);
    }
    result.milliseconds =
        chrono::duration<double, milli>(chrono::steady_clock::now() - start).count();

    // Block reads needed to reach one block of a file, averaged over every
    // block: contiguous computes the address, indexed may need the indirect
    // block first, linked must follow the chain from the start.
    double reads = 0;
    size_t blocks = 0;
    for (const auto& item : fs.allocated_inodes()) {
        const size_t count = fs.data_blocks(item.second).size();
        for (size_t k = 0; k < count; ++k) {
            switch (mode) {
            case AllocationMode::Contiguous: reads += 1; break;
            case AllocationMode::Linked: reads += static_cast<double>(k + 1); break;
            case AllocationMode::Indexed: reads += k < kDirectPointers ? 1 : 2; break;
            }
        }
        blocks += count;
    }
    result.reads_per_block = blocks ? reads / blocks : 0;
    result.frag = fs.fragmentation();
    result.used_blocks = fs.usage().used_data_blocks;

    // Finally one big file using 75% of the free space. Every mode has the
    // room; contiguous also needs it to be in one piece.
    const uint32_t big_blocks = fs.usage().free_data_blocks * 3 / 4;
    try {
        fs.create_file("/big");
        fs.write_file("/big", string(size_t{big_blocks} * fs.block_payload(), 'z'));
        result.big_file = "ok";
    } catch (const NoContiguousSpaceError&) {
        result.big_file = "no run";
    } catch (const FsError&) {
        result.big_file = "failed";
    }
    result.big_file += " (" + to_string(big_blocks) + ")";
    fs.unmount();
    remove(path.c_str());
    return result;
}

// ---------------------------------------------------------------------------
// Session: one running CLI with its mounted disk
// ---------------------------------------------------------------------------

struct CommandInfo {
    string name;       // plain-English name
    string unix_name;  // Unix-style alias, may be empty
    string usage;
    string description;
    bool modifies;     // changes the disk (triggers auto-export)
};

const vector<CommandInfo>& command_table() {
    static const vector<CommandInfo> commands = {
        {"help", "", "help", "Show this help message", false},
        {"version", "", "version", "Show the MiniFS version", false},
        {"format", "", "format [MB] [--alloc=MODE]",
         "Create a fresh disk (MODE: indexed, linked, contiguous)", true},
        {"diskinfo", "", "diskinfo", "Show the disk layout", false},
        {"space", "df", "space", "Show used and free space", false},
        {"where", "pwd", "where", "Show the current directory", false},
        {"goto", "cd", "goto [path]", "Move into a directory (no path: go to /)", false},
        {"list", "ls", "list [-l] [path]", "List a directory (-l: details)", false},
        {"createdir", "mkdir", "createdir <path>", "Create a directory", true},
        {"deletedir", "rmdir", "deletedir <path>", "Delete an empty directory", true},
        {"create", "touch", "create <path>", "Create an empty file", true},
        {"write", "", "write <path> \"<text>\"", "Replace a file's text", true},
        {"append", "", "append <path> \"<text>\"", "Add text to the end of a file", true},
        {"fill", "", "fill <path> <KB>", "Fill a file with KB of sample text", true},
        {"read", "cat", "read <path>", "Show a file's contents", false},
        {"delete", "rm", "delete <path>", "Delete a file", true},
        {"rename", "", "rename <old> <new>", "Rename or move to an exact path", true},
        {"move", "mv", "move <from> <to>", "Move a file or directory (to may be a directory)", true},
        {"copy", "cp", "copy <from> <to>", "Copy a file (to may be a directory)", true},
        {"chmod", "", "chmod <mode> <path>", "Set permissions, e.g. chmod 444 notes.txt", true},
        {"info", "stat", "info <path>", "Show inode details and blocks", false},
        {"frag", "", "frag", "Show the fragmentation report", false},
        {"map", "", "map [first] [count]", "Show the ASCII block map", false},
        {"defrag", "", "defrag", "Move files into contiguous blocks", true},
        {"export", "", "export [file|on|off]", "Write disk_state.json (on: after every change)", false},
        {"crash", "", "crash [point]", "Simulate a power cut in the next change", false},
        {"check", "fsck", "check [repair]", "Check consistency (repair: fix problems)", false},
        {"bench", "", "bench", "Compare the allocation modes on one workload", false},
        {"exit", "quit", "exit", "Exit MiniFS", false},
    };
    return commands;
}

const CommandInfo* find_command(const string& name) {
    for (const CommandInfo& command : command_table()) {
        if (command.name == name || (!command.unix_name.empty() && command.unix_name == name)) {
            return &command;
        }
    }
    return nullptr;
}

class Session {
public:
    Session(const Cli::Options& options, ostream& output)
        : options_(options),
          out_(output),
          err_(options.quiet ? cerr : output),
          disk_(options.disk_path),
          fs_(disk_) {}

    void start();
    // Returns false when the session should end.
    bool execute(const string& line);
    int exit_code() const { return exit_code_; }

private:
    using Args = vector<string>;

    void dispatch(const CommandInfo& command, const Args& args);
    void require_args(const Args& args, size_t low, size_t high, const CommandInfo& command) const;
    void ensure_mounted();
    void sync_with_disk();
    void report_recovery();
    void auto_export();
    string export_path() const;

    void cmd_help();
    void cmd_format(const Args& args);
    void cmd_diskinfo();
    void cmd_space();
    void cmd_list(const Args& args);
    void cmd_info(const string& path);
    void cmd_frag();
    void cmd_map(const Args& args);
    void cmd_export(const Args& args);
    void cmd_crash(const Args& args);
    void cmd_check(const Args& args);
    void cmd_bench();

    Cli::Options options_;
    ostream& out_;
    ostream& err_;  // errors: stderr in quiet mode, otherwise the same as out_
    VirtualDisk disk_;
    FileSystem fs_;
    bool auto_export_ = false;
    int exit_code_ = 0;
};

void Session::start() {
    if (!options_.quiet) {
        out_ << kVersion << "\nType 'help' for available commands.\n";
    }
    if (!file_exists(disk_.path())) {
        if (!options_.quiet) {
            out_ << "No virtual disk found. Run 'format' to create one.\n";
        }
        return;
    }
    try {
        fs_.mount();
        if (!options_.quiet) {
            out_ << "Mounted " << disk_.path() << " ("
                 << allocation_mode_name(fs_.allocation_mode()) << " allocation).\n";
            report_recovery();
        }
    } catch (const exception& error) {
        fs_.unmount();
        err_ << "Could not mount " << disk_.path() << ": " << error.what() << '\n';
    }
}

// Another MiniFS process (such as the web GUI) may have changed the disk since
// the last command, so look at it again before running the next one.
void Session::sync_with_disk() {
    try {
        if (fs_.is_mounted()) {
            fs_.refresh();
        } else if (file_exists(disk_.path())) {
            fs_.mount();
        }
    } catch (const exception&) {
        fs_.unmount();
    }
}

void Session::report_recovery() {
    const RecoveryResult& recovery = fs_.last_recovery();
    switch (recovery.action) {
    case RecoveryResult::Action::Clean:
        return;
    case RecoveryResult::Action::Replayed:
        out_ << "Journal: found committed transaction #" << recovery.sequence << " ("
             << recovery.blocks << " blocks) from an interrupted command.\n"
             << "Journal: replayed it, so the command is now complete.\n";
        break;
    case RecoveryResult::Action::Discarded:
        out_ << "Journal: found transaction #" << recovery.sequence
             << " that was not committed (" << recovery.reason << ").\n"
             << "Journal: discarded it, so the command was rolled back.\n";
        break;
    }
    out_ << "Run 'check' (fsck) to verify the file system.\n";
}

bool Session::execute(const string& raw_line) {
    string line = raw_line;
    // A UTF-8 byte order mark (added by PowerShell pipes) or a Windows line
    // ending is not part of the command.
    if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        line.erase(0, 3);
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (options_.echo && !options_.quiet) {
        out_ << line << '\n';
    }
    const size_t first = line.find_first_not_of(" \t");
    if (first == string::npos || line[first] == '#') {
        return true;
    }

    try {
        const Args words = tokenize(line);
        if (words.empty()) {
            return true;
        }
        const CommandInfo* command = find_command(words[0]);
        if (command == nullptr) {
            err_ << "Unknown command: " << words[0] << ". Type 'help' for the list.\n";
            return true;
        }
        if (command->name == "exit") {
            return false;
        }
        sync_with_disk();
        const Args args(words.begin() + 1, words.end());
        dispatch(*command, args);
        if (command->modifies && auto_export_ && fs_.is_mounted()) {
            auto_export();
        }
    } catch (const SimulatedCrash& crash) {
        err_ << "\n*** Simulated crash " << crash.what() << ". ***\n"
             << "MiniFS stopped as if the power was cut. Start it again to see the "
                "journal recovery.\n";
        exit_code_ = 3;
        return false;
    } catch (const exception& error) {
        err_ << "Error: " << error.what() << '\n';
    }
    return true;
}

void Session::require_args(const Args& args, size_t low, size_t high,
                           const CommandInfo& command) const {
    if (args.size() < low || args.size() > high) {
        throw invalid_argument("Usage: " + command.usage);
    }
}

void Session::ensure_mounted() {
    if (!fs_.is_mounted()) {
        throw NotMountedError();
    }
}

void Session::dispatch(const CommandInfo& command, const Args& args) {
    const string& name = command.name;
    if (name == "help") {
        cmd_help();
        return;
    }
    if (name == "version") {
        out_ << kVersion << '\n';
        return;
    }
    if (name == "format") {
        cmd_format(args);
        return;
    }
    if (name == "bench") {
        cmd_bench();
        return;
    }

    ensure_mounted();
    if (name == "diskinfo") {
        cmd_diskinfo();
    } else if (name == "space") {
        cmd_space();
    } else if (name == "where") {
        require_args(args, 0, 0, command);
        out_ << fs_.working_directory_path() << '\n';
    } else if (name == "goto") {
        require_args(args, 0, 1, command);
        fs_.change_directory(args.empty() ? "/" : args[0]);
    } else if (name == "list") {
        cmd_list(args);
    } else if (name == "createdir") {
        require_args(args, 1, 1, command);
        fs_.make_directory(args[0]);
    } else if (name == "deletedir") {
        require_args(args, 1, 1, command);
        fs_.remove_directory(args[0]);
    } else if (name == "create") {
        require_args(args, 1, 1, command);
        fs_.create_file(args[0]);
    } else if (name == "write" || name == "append") {
        if (args.size() < 2) {
            throw invalid_argument("Usage: " + command.usage);
        }
        string text = args[1];
        for (size_t i = 2; i < args.size(); ++i) {
            text += " " + args[i];
        }
        if (name == "write") {
            fs_.write_file(args[0], text);
        } else {
            fs_.append_file(args[0], text);
        }
    } else if (name == "fill") {
        require_args(args, 2, 2, command);
        const size_t kb = parse_number(args[1], "Size in KB", 0, kMaxFillKb);
        bool created = false;
        try {
            fs_.resolve_path(args[0]);
        } catch (const out_of_range&) {
            fs_.create_file(args[0]);
            created = true;
        }
        try {
            fs_.write_file(args[0], sample_content(kb * 1024));
        } catch (const FsError&) {
            if (created && fs_.is_mounted()) {
                fs_.remove_file(args[0]);  // do not leave an empty file behind
            }
            throw;
        }
        const FileInfo info = fs_.stat(args[0]);
        out_ << "Wrote " << format_bytes(info.meta.size) << " to " << info.path << " ("
             << info.data_blocks.size() << " data blocks";
        if (!info.pointer_blocks.empty()) {
            out_ << " + " << info.pointer_blocks.size() << " indirect block";
        }
        out_ << ", " << info.extents << (info.extents == 1 ? " extent" : " extents") << ").\n";
    } else if (name == "read") {
        require_args(args, 1, 1, command);
        const string content = fs_.read_file(args[0]);
        out_ << content;
        // Keep the prompt on its own line, but give programs the exact bytes.
        if (!options_.quiet && (content.empty() || content.back() != '\n')) {
            out_ << '\n';
        }
    } else if (name == "delete") {
        require_args(args, 1, 1, command);
        fs_.remove_file(args[0]);
    } else if (name == "rename") {
        require_args(args, 2, 2, command);
        fs_.rename_entry(args[0], args[1]);
    } else if (name == "move") {
        require_args(args, 2, 2, command);
        fs_.move_entry(args[0], args[1]);
    } else if (name == "copy") {
        require_args(args, 2, 2, command);
        fs_.copy_file(args[0], args[1]);
    } else if (name == "chmod") {
        require_args(args, 2, 2, command);
        const string& mode = args[0];
        if (mode.empty() || mode.size() > 4 ||
            mode.find_first_not_of("01234567") != string::npos) {
            throw invalid_argument("Mode must be octal digits like 644 or 755, not '" + mode + "'.");
        }
        fs_.change_mode(args[1], static_cast<uint16_t>(stoul(mode, nullptr, 8)));
    } else if (name == "info") {
        require_args(args, 1, 1, command);
        cmd_info(args[0]);
    } else if (name == "frag") {
        cmd_frag();
    } else if (name == "map") {
        cmd_map(args);
    } else if (name == "defrag") {
        const FragmentationReport before = fs_.fragmentation();
        const DefragResult result = fs_.defrag();
        const FragmentationReport after = fs_.fragmentation();
        out_ << "Moved " << result.entries_moved << " file(s)/directories ("
             << result.blocks_moved << " blocks).\n"
             << "Fragmented files: " << before.fragmented << " -> " << after.fragmented
             << ", free extents: " << before.free_extents << " -> " << after.free_extents
             << ", external fragmentation: " << percent(before.external_fragmentation)
             << " -> " << percent(after.external_fragmentation) << ".\n";
    } else if (name == "export") {
        cmd_export(args);
    } else if (name == "crash") {
        cmd_crash(args);
    } else if (name == "check") {
        cmd_check(args);
    }
}

void Session::cmd_help() {
    out_ << "Available commands (the Unix name in brackets also works):\n";
    for (const CommandInfo& command : command_table()) {
        const string alias = command.unix_name.empty() ? "" : "[" + command.unix_name + "]";
        out_ << "  " << left << setw(28) << command.usage << setw(8) << alias
             << command.description << '\n';
    }
    out_ << right
         << "Paths may be absolute (/docs/a.txt) or relative (a.txt, ../b), and may use . and ..\n"
         << "Put text with spaces in quotes: write notes.txt \"Hello world\"\n";
}

void Session::cmd_format(const Args& args) {
    size_t megabytes = 100;
    AllocationMode mode = AllocationMode::Indexed;
    for (size_t i = 0; i < args.size(); ++i) {
        string arg = args[i];
        if (arg.rfind("--alloc=", 0) == 0) {
            arg = arg.substr(8);
        } else if (arg == "--alloc") {
            if (i + 1 == args.size()) {
                throw invalid_argument("Usage: format [MB] [--alloc=indexed|linked|contiguous]");
            }
            arg = args[++i];
        } else if (!arg.empty() && isdigit(static_cast<unsigned char>(arg[0]))) {
            megabytes = parse_number(arg, "Disk size in MB", 1, kMaxDiskMb);
            continue;
        }
        if (!parse_allocation_mode(arg, mode)) {
            throw invalid_argument("Unknown option '" + args[i] +
                                   "'. Usage: format [MB] [--alloc=indexed|linked|contiguous]");
        }
    }

    fs_.format(megabytes * 1024 * 1024, mode);
    const Superblock& sb = fs_.superblock();
    out_ << "Formatted " << disk_.path() << ": " << megabytes << " MB, " << sb.total_blocks
         << " blocks of " << sb.block_size << " bytes, " << sb.inode_count - 1
         << " inodes, " << allocation_mode_name(mode) << " allocation.\n";
}

void Session::cmd_diskinfo() {
    const Superblock& sb = fs_.superblock();
    const auto range = [](uint32_t first, uint32_t count) {
        ostringstream text;
        if (count == 1) {
            text << "block " << first << " (1 block";
        } else {
            text << "blocks " << first << "-" << first + count - 1 << " (" << count << " blocks";
        }
        return text.str();
    };
    out_ << "Path: " << disk_.path() << '\n'
         << "Disk size: " << format_bytes(disk_.disk_size()) << " (" << sb.total_blocks
         << " blocks x " << sb.block_size << " bytes)\n"
         << "Layout:\n"
         << "  Superblock    block " << kSuperblockBlock << '\n'
         << "  Free bitmap   " << range(sb.bitmap_start, sb.bitmap_blocks) << ")\n"
         << "  Inode table   " << range(sb.inode_table_start, sb.inode_table_blocks) << ", "
         << sb.inode_count << " inodes of " << sb.inode_size << " bytes)\n"
         << "  Journal       " << range(sb.journal_start, sb.journal_blocks) << ")\n"
         << "  Data blocks   " << range(sb.data_start, sb.data_block_count) << ")\n"
         << "Free data blocks: " << sb.free_block_count << " / " << sb.data_block_count << '\n'
         << "Free inodes: " << sb.free_inode_count << " / " << sb.inode_count - 1 << '\n'
         << "Root inode: " << sb.root_inode << '\n'
         << "Allocation mode: " << allocation_mode_name(fs_.allocation_mode()) << '\n';
    const uint64_t limit = fs_.max_file_size();
    out_ << "Largest file: "
         << (limit ? format_bytes(limit) + " (10 direct + 1 indirect block)" : "limited by free space")
         << '\n'
         << "Created: " << format_time(sb.created_at) << '\n';
}

void Session::cmd_space() {
    const UsageStats stats = fs_.usage();
    const double used =
        stats.data_blocks ? static_cast<double>(stats.used_data_blocks) / stats.data_blocks : 0;
    out_ << left << setw(12) << "Disk" << format_bytes(uint64_t{stats.total_blocks} * stats.block_size)
         << " (" << stats.total_blocks << " blocks of " << stats.block_size << " bytes)\n"
         << setw(12) << "Metadata" << stats.metadata_blocks
         << " blocks (superblock, bitmap, inode table, journal)\n"
         << setw(12) << "Data" << stats.data_blocks << " blocks\n"
         << setw(12) << "  Used" << stats.used_data_blocks << " blocks, "
         << format_bytes(uint64_t{stats.used_data_blocks} * stats.block_size) << " ("
         << percent(used) << ")\n"
         << setw(12) << "  Free" << stats.free_data_blocks << " blocks, "
         << format_bytes(uint64_t{stats.free_data_blocks} * stats.block_size) << '\n'
         << setw(12) << "Inodes" << stats.used_inodes << " used, " << stats.free_inodes
         << " free (of " << stats.usable_inodes << ")\n"
         << right;
}

void Session::cmd_list(const Args& args) {
    bool details = false;
    string path = ".";
    for (const string& arg : args) {
        if (arg == "-l") {
            details = true;
        } else if (path == ".") {
            path = arg;
        } else {
            throw invalid_argument("Usage: list [-l] [path]");
        }
    }
    const vector<ListEntry> entries = fs_.list(path);
    if (entries.empty()) {
        if (details) {
            out_ << "(empty)\n";
        }
        return;
    }
    for (const ListEntry& entry : entries) {
        const bool directory = is_directory(entry.meta);
        if (details) {
            out_ << (directory ? 'd' : '-') << permission_string(entry.meta.mode) << ' '
                 << setw(3) << entry.meta.link_count << ' ' << setw(10) << entry.meta.size
                 << "  " << format_time(entry.meta.modified_at) << "  ";
        }
        out_ << entry.name << (directory ? "/" : "") << '\n';
    }
}

void Session::cmd_info(const string& path) {
    const FileInfo info = fs_.stat(path);
    const Inode& inode = info.meta;
    out_ << "  Path:      " << info.path << '\n'
         << "  Type:      " << (is_directory(inode) ? "directory" : "file") << '\n'
         << "  Inode:     " << info.number << '\n'
         << "  Size:      " << inode.size << " bytes\n"
         << "  Mode:      " << octal(inode.mode) << " ("
         << (is_directory(inode) ? 'd' : '-') << permission_string(inode.mode) << ")\n"
         << "  Links:     " << inode.link_count << '\n'
         << "  Created:   " << format_time(inode.created_at) << '\n'
         << "  Modified:  " << format_time(inode.modified_at) << '\n'
         << "  Blocks:    " << info.data_blocks.size() << " data";
    if (!info.pointer_blocks.empty()) {
        out_ << " + " << info.pointer_blocks.size() << " indirect";
    }
    out_ << " (" << allocation_mode_name(fs_.allocation_mode()) << " allocation)\n"
         << "  Data at:   " << block_ranges(info.data_blocks) << '\n';
    if (!info.pointer_blocks.empty()) {
        out_ << "  Indirect:  " << block_ranges(info.pointer_blocks) << '\n';
    }
    if (!info.data_blocks.empty()) {
        out_ << "  Extents:   " << info.extents
             << (info.extents == 1 ? " (contiguous)" : " (fragmented)") << '\n';
    }
}

void Session::cmd_frag() {
    const FragmentationReport report = fs_.fragmentation();
    out_ << "Fragmentation report (" << allocation_mode_name(fs_.allocation_mode())
         << " allocation)\n"
         << "  Files and directories with data: " << report.entries << '\n'
         << "  Fragmented (more than one extent): " << report.fragmented;
    if (report.entries) {
        out_ << " (" << percent(static_cast<double>(report.fragmented) / report.entries) << ")";
    }
    out_ << '\n';
    if (report.entries) {
        out_ << "  Average extents per file: " << fixed << setprecision(2)
             << static_cast<double>(report.total_extents) / report.entries << '\n';
        out_.unsetf(ios::floatfield);
    }
    out_ << "  Internal fragmentation: " << format_bytes(report.internal_waste)
         << " unused in partly filled last blocks\n"
         << "  Free space: " << report.free_blocks << " blocks in " << report.free_extents
         << (report.free_extents == 1 ? " extent" : " extents") << ", largest "
         << report.largest_free_extent << " blocks\n"
         << "  External fragmentation: " << percent(report.external_fragmentation)
         << " (1 - largest free extent / free blocks)\n";
    if (!report.fragmented_files.empty()) {
        out_ << "Fragmented files:\n";
        for (size_t i = 0; i < report.fragmented_files.size() && i < 15; ++i) {
            const FragmentedFile& file = report.fragmented_files[i];
            out_ << "  " << file.path << ": " << file.blocks << " blocks in " << file.extents
                 << " extents\n";
        }
        if (report.fragmented_files.size() > 15) {
            out_ << "  ... and " << report.fragmented_files.size() - 15 << " more\n";
        }
    }
}

void Session::cmd_map(const Args& args) {
    const vector<BlockOwner> owners = fs_.block_map();
    const size_t total = owners.size();
    if (args.size() > 2) {
        throw invalid_argument("Usage: map [first] [count]");
    }

    size_t first = 0;
    size_t end = total;
    if (!args.empty()) {
        first = parse_number(args[0], "First block", 0, total - 1);
        end = args.size() == 2
                  ? min(total, first + parse_number(args[1], "Block count", 1, total))
                  : total;
    }
    if (args.size() < 2) {
        // Stop after the last used block so a mostly empty disk stays short.
        size_t last_used = first;
        for (size_t block = first; block < end; ++block) {
            if (owners[block].kind != BlockKind::Free) {
                last_used = block;
            }
        }
        end = min(end, (last_used / kMapColumns + 2) * kMapColumns);
    }

    out_ << "Legend: S superblock  B bitmap  I inode table  J journal  D directory\n"
         << "        F file data   P indirect block  ? leaked  . free\n";
    const size_t width = to_string(total).size();
    for (size_t row = first - first % kMapColumns; row < end; row += kMapColumns) {
        out_ << setw(static_cast<int>(width)) << row << "  ";
        for (size_t block = row; block < row + kMapColumns && block < end; ++block) {
            out_ << (block < first ? ' ' : static_cast<char>(owners[block].kind));
        }
        out_ << '\n';
    }
    if (end < total && args.size() < 2) {
        out_ << "Blocks " << end << "-" << total - 1 << " are free.\n";
    }
}

string Session::export_path() const {
    const string& disk_path = disk_.path();
    const size_t slash = disk_path.find_last_of("/\\");
    return slash == string::npos ? "disk_state.json" : disk_path.substr(0, slash + 1) + "disk_state.json";
}

void Session::auto_export() {
    ofstream file(export_path(), ios::binary | ios::trunc);
    if (file) {
        fs_.export_json(file);
    }
}

void Session::cmd_export(const Args& args) {
    if (args.size() > 1) {
        throw invalid_argument("Usage: export [file|on|off]");
    }
    if (!args.empty() && (args[0] == "on" || args[0] == "off")) {
        auto_export_ = args[0] == "on";
        out_ << "Automatic export " << (auto_export_ ? "on: " + export_path() +
                                        " is rewritten after every change." : "off.") << '\n';
        if (auto_export_) {
            auto_export();
        }
        return;
    }
    if (!args.empty() && args[0] == "-") {
        fs_.export_json(out_);  // to the screen (used by the web GUI)
        return;
    }
    const string path = args.empty() ? export_path() : args[0];
    ofstream file(path, ios::binary | ios::trunc);
    if (!file) {
        throw runtime_error("Cannot write " + path + ".");
    }
    fs_.export_json(file);
    out_ << "Exported the disk state to " << path << ".\n";
}

void Session::cmd_crash(const Args& args) {
    if (args.size() > 1) {
        throw invalid_argument("Usage: crash [before-commit|after-commit|mid-apply|no-journal|off]");
    }
    CrashPoint point = CrashPoint::AfterCommit;
    if (!args.empty()) {
        if (args[0] == "off") {
            point = CrashPoint::None;
        } else if (!parse_crash_point(args[0], point) || point == CrashPoint::None) {
            throw invalid_argument("Unknown crash point '" + args[0] +
                                   "'. Use before-commit, after-commit, mid-apply, "
                                   "no-journal or off.");
        }
    }
    fs_.arm_crash(point);
    if (point == CrashPoint::None) {
        out_ << "Crash simulation off.\n";
        return;
    }
    out_ << "Crash armed (" << crash_point_name(point)
         << "): the next command that changes the disk will stop ";
    switch (point) {
    case CrashPoint::BeforeCommit:
        out_ << "after writing the journal but before committing it.\n"
             << "Expected on restart: the transaction is discarded (command rolled back).\n";
        break;
    case CrashPoint::AfterCommit:
        out_ << "right after the journal commit, before applying it.\n"
             << "Expected on restart: the transaction is replayed (command completed).\n";
        break;
    case CrashPoint::MidApply:
        out_ << "halfway through applying the committed changes.\n"
             << "Expected on restart: the transaction is replayed (command completed).\n";
        break;
    case CrashPoint::NoJournal:
        out_ << "halfway through, with the journal bypassed.\n"
             << "Expected on restart: no recovery; 'check' finds the damage.\n";
        break;
    case CrashPoint::None:
        break;
    }
}

void Session::cmd_check(const Args& args) {
    const bool repair = !args.empty() && (args[0] == "repair" || args[0] == "--repair");
    if (args.size() > 1 || (!args.empty() && !repair)) {
        throw invalid_argument("Usage: check [repair]");
    }
    out_ << "fsck: checking " << disk_.path() << "...\n";
    const FsckReport report = fs_.fsck(repair);
    for (const string& problem : report.problems) {
        out_ << "  problem: " << problem << '\n';
    }
    for (const string& fix : report.repairs) {
        out_ << "  fixed:   " << fix << '\n';
    }
    if (report.clean()) {
        const UsageStats stats = fs_.usage();
        out_ << "fsck: file system is clean (" << stats.used_inodes << " inodes, "
             << stats.used_data_blocks << " data blocks in use).\n";
        return;
    }
    if (!repair) {
        out_ << "fsck: " << report.problems.size()
             << " problem(s) found. Run 'check repair' to fix them.\n";
        return;
    }
    const FsckReport after = fs_.fsck(false);
    if (after.clean()) {
        out_ << "fsck: repaired; the file system is now clean.\n";
    } else {
        out_ << "fsck: " << after.problems.size() << " problem(s) could not be repaired:\n";
        for (const string& problem : after.problems) {
            out_ << "  problem: " << problem << '\n';
        }
    }
}

void Session::cmd_bench() {
    const string& disk_path = disk_.path();
    const size_t slash = disk_path.find_last_of("/\\");
    const string path = (slash == string::npos ? "" : disk_path.substr(0, slash + 1)) +
                        "minifs_bench.bin";

    out_ << "Running the same workload on a scratch 8 MB disk per allocation mode...\n"
         << "(write 80 files, delete every other one, grow some, then write 50 larger "
            "files)\n\n";
    vector<BenchResult> results;
    for (AllocationMode mode :
         {AllocationMode::Contiguous, AllocationMode::Linked, AllocationMode::Indexed}) {
        results.push_back(run_benchmark(mode, path));
    }

    const auto row = [&](const string& label, const function<string(const BenchResult&)>& value) {
        out_ << "  " << left << setw(30) << label << right;
        for (const BenchResult& result : results) {
            out_ << setw(12) << value(result);
        }
        out_ << '\n';
    };
    const auto fixed2 = [](double value) {
        ostringstream text;
        text << fixed << setprecision(2) << value;
        return text.str();
    };
    row("", [](const BenchResult& r) { return string(allocation_mode_name(r.mode)); });
    row("Files written", [](const BenchResult& r) { return to_string(r.written); });
    row("Failed writes/appends", [](const BenchResult& r) { return to_string(r.failed); });
    row("Data blocks used", [](const BenchResult& r) { return to_string(r.used_blocks); });
    row("Fragmented files", [](const BenchResult& r) { return to_string(r.frag.fragmented); });
    row("Average extents per file", [&](const BenchResult& r) {
        return fixed2(r.frag.entries ? static_cast<double>(r.frag.total_extents) / r.frag.entries : 0);
    });
    row("Free extents", [](const BenchResult& r) { return to_string(r.frag.free_extents); });
    row("Largest free extent (blocks)",
        [](const BenchResult& r) { return to_string(r.frag.largest_free_extent); });
    row("External fragmentation",
        [](const BenchResult& r) { return percent(r.frag.external_fragmentation); });
    row("Internal waste", [](const BenchResult& r) { return format_bytes(r.frag.internal_waste); });
    row("Reads to reach a block (avg)", [&](const BenchResult& r) { return fixed2(r.reads_per_block); });
    row("Time (ms)", [&](const BenchResult& r) { return fixed2(r.milliseconds); });
    row("Big file (75% of free space)", [](const BenchResult& r) { return r.big_file; });
    out_ << "\nContiguous keeps every file in one piece (one read to reach any block), but\n"
            "its free space breaks into holes, so a big file can fail even when there is\n"
            "room. Linked never splits free space but must walk the chain to reach a\n"
            "block. Indexed reaches any block in at most two reads.\n";
}

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

vector<string> tokenize(const string& line) {
    vector<string> words;
    string current;
    bool in_word = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"' || c == '\'') {
            const char quote = c;
            in_word = true;
            ++i;
            for (; i < line.size() && line[i] != quote; ++i) {
                if (quote == '"' && line[i] == '\\' && i + 1 < line.size()) {
                    const char next = line[++i];
                    switch (next) {
                    case 'n': current += '\n'; break;
                    case 't': current += '\t'; break;
                    case '"': current += '"'; break;
                    case '\\': current += '\\'; break;
                    default: current += '\\'; current += next; break;
                    }
                } else {
                    current += line[i];
                }
            }
            if (i == line.size()) {
                throw invalid_argument("Missing closing quote (" + string(1, quote) + ").");
            }
        } else if (c == ' ' || c == '\t') {
            if (in_word) {
                words.push_back(current);
                current.clear();
                in_word = false;
            }
        } else {
            current += c;
            in_word = true;
        }
    }
    if (in_word) {
        words.push_back(current);
    }
    return words;
}

Cli::Cli() = default;

Cli::Cli(Options options) : options_(std::move(options)) {}

int Cli::run(istream& input, ostream& output) {
    Session session(options_, output);
    session.start();
    const char* prompt = options_.quiet ? "" : "minifs> ";
    string line;
    while (output << prompt << flush && getline(input, line)) {
        if (!session.execute(line)) {
            break;
        }
    }
    if (!options_.quiet) {
        output << '\n';
    }
    return session.exit_code();
}

} // namespace minifs
