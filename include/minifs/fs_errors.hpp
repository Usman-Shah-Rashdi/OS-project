#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace minifs {

// Base class for file-system errors the CLI should report to the user.
class FsError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class DiskFullError : public FsError {
public:
    DiskFullError() : FsError("Disk full: no free data blocks.") {}
};

// Enough free blocks exist, but not in one run (contiguous allocation).
class NoContiguousSpaceError : public FsError {
public:
    explicit NoContiguousSpaceError(std::size_t blocks)
        : FsError("No run of " + std::to_string(blocks) +
                  " free blocks in a row (external fragmentation). "
                  "Try 'defrag'.") {}
};

class NoFreeInodesError : public FsError {
public:
    NoFreeInodesError() : FsError("No free inodes left.") {}
};

class FileTooLargeError : public FsError {
public:
    using FsError::FsError;
};

class PermissionDeniedError : public FsError {
public:
    using FsError::FsError;
};

class NotMountedError : public FsError {
public:
    NotMountedError()
        : FsError("No file system is mounted. Run 'format' first.") {}
};

// Thrown by the 'crash' simulation. It is deliberately not an FsError: it
// means "the machine lost power here", and the program must stop without
// writing anything else to the disk.
class SimulatedCrash : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace minifs
