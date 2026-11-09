# MiniFS

MiniFS is a block-based file-system simulator for the Operating Systems project.
It will store its state in a single virtual disk file (`virtual_disk.bin`) and
provide a Unix-style command-line interface. A browser visualizer will be added
after the file-system core is in place.

## Project layout

```text
.
├── CMakeLists.txt
├── include/minifs/     # Public project headers
├── src/                # C++ implementation
└── virtual_disk.bin    # Generated at runtime (not tracked)
```

## Requirements

- CMake 3.16 or newer
- A C++17 compiler (GCC, Clang, or MSVC)

## Build

From the project root:

```bash
cmake -S . -B build
cmake --build build
```

Run the CLI:

```bash
./build/minifs
```

On Windows with a Visual Studio generator, run the generated executable from
the corresponding configuration directory, for example
`build/Debug/minifs.exe`.

Run the unit tests:

```bash
ctest --test-dir build --output-on-failure
```

## Disk layout

`format` creates a 100 MB disk with 4 KB blocks (or a size given in MB) and
lays it out as:

```text
| Block 0    | Blocks 1..B | Next I blocks | Remaining blocks |
| Superblock | Free bitmap | Inode table   | Data blocks      |
```

- **Superblock**: magic `MNFS`, version, block size, block count, the start
  and length of every region, free block/inode counts, root inode, and
  allocation mode.
- **Free bitmap**: one bit per block, 1 = used. Metadata blocks are marked
  used at format time.
- **Inode table**: 128-byte inodes (type, permissions, size, timestamps,
  10 direct pointers, 1 indirect pointer). One inode for every 4 blocks.
  Inode 0 is reserved; inode 1 is the root directory.
- **Data blocks**: file and directory contents. Directories are arrays of
  64-byte entries (inode number + 60-byte name).

The structs live in `include/minifs/fs_types.hpp`.

```text
minifs> format
minifs> diskinfo
minifs> pwd
minifs> mkdir /a
minifs> cd /a
minifs> ls /
minifs> touch /a/file
minifs> write /a/file "Hello MiniFS"
minifs> cat /a/file
minifs> rm /a/file
minifs> rmdir /a
minifs> rename /a/old /a/new
```

The disk is mounted automatically when MiniFS starts. Path resolution supports
absolute and relative paths, `.`, and `..`. The `touch` command creates empty
files, `write` replaces a file's contents, `cat` prints file contents, `rm`
removes files, `rmdir` removes empty directories, and `rename` renames or moves
entries. Recovery, allocation strategies, and the visualizer will be
allocation strategies, and the visualizer will be implemented incrementally.
