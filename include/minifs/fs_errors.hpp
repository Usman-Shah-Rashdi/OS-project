#pragma once

#include <stdexcept>
#include <string>

using namespace std;

namespace minifs {

// Base class for file-system errors the CLI should report to the user.
class FsError : public runtime_error {
public:
    using runtime_error::runtime_error;
};

class DiskFullError : public FsError {
public:
    DiskFullError() : FsError("Disk full: no free data blocks.") {}
};

class NoFreeInodesError : public FsError {
public:
    NoFreeInodesError() : FsError("No free inodes left.") {}
};

class NotMountedError : public FsError {
public:
    NotMountedError()
        : FsError("No file system is mounted. Run 'format' first.") {}
};

} // namespace minifs
