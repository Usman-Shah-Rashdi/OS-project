# MiniFS – Mini File System Simulator

Operating Systems project, Section H — Muhammad Talha, Abdul Rafiu, Muhammad Usman.

MiniFS is a block-based file system that lives inside one binary file,
`virtual_disk.bin`, which acts as a virtual disk. It has its own superblock,
free-space bitmap, inode table, write-ahead journal and data blocks, and a
Unix-style command line to work with it. Everything you create survives
restarts, and a simulated power cut is recovered from the journal.

## Features

| Area | What MiniFS does |
|---|---|
| Disk layout | Superblock, free bitmap, inode table, journal, data blocks (4 KB blocks) |
| Inodes | Type, size, permission bits, link count, timestamps, 10 direct pointers + 1 single-indirect pointer |
| Directories | Hierarchical, stored as files of 64-byte entries; absolute/relative paths, `.` and `..` |
| Allocation | `indexed` (default), `linked`, or `contiguous`, chosen at format time |
| Large files | Up to 4 MB per file in indexed mode (10 direct + 1024 indirect blocks) |
| Permissions | `rwx` owner bits, enforced on read, write, list, search and create/delete |
| Journaling | Every command is one atomic transaction; metadata goes through a write-ahead journal |
| Crash recovery | `crash` simulates a power cut at four points; mount replays or discards the journal |
| Consistency | `check` (fsck) finds leaked/cross-linked blocks, orphan inodes, bad links; `check repair` fixes them |
| Fragmentation | `frag` report, ASCII block `map`, `defrag` compaction |
| Comparison | `bench` runs one workload under all three allocation modes |
| Export | `export` writes `disk_state.json` (block map, inodes, stats) for a visualizer |

## Build

Requirements: CMake 3.16+ and a C++17 compiler (GCC 8+, Clang, or MSVC 2019+).

```bash
cmake -S . -B build
cmake --build build
```

**Windows note:** an old MinGW (for example the GCC 4.9 bundled with
Code::Blocks) cannot compile C++17. If CMake picks it up, point it at a newer
compiler, for example:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER="C:/path/to/mingw64/bin/g++.exe"
cmake --build build
```

With MinGW the runtime is linked statically, so `minifs.exe` runs from any
terminal (cmd or PowerShell) without extra DLLs on `PATH`. With a Visual Studio
generator the program is in `build/Debug/minifs.exe`.

## Run

```bash
./build/minifs                       # interactive shell on virtual_disk.bin
./build/minifs scripts/demo.txt      # run a command script
./build/minifs --disk other.bin      # use another disk image
```

On Windows: `build\minifs.exe`. The disk file is created in the folder you
run MiniFS from.

```text
minifs> format
minifs> createdir /docs
minifs> goto /docs
minifs> create notes.txt
minifs> write notes.txt "Hello MiniFS"
minifs> read notes.txt
Hello MiniFS
minifs> list -l
-rw-r--r--   1         12  2026-11-15 19:37:21  notes.txt
minifs> exit
```

## Web interface (MiniFS Explorer)

A simple file-explorer page for the same disk, in the style of Windows File
Explorer: folder tree, file list, toolbar, right-click menu, a text editor for
files, and a Properties window that shows the inode, permissions and the disk
blocks a file uses. It needs [Node.js](https://nodejs.org) 16 or newer and no
extra packages.

```bash
cmake --build build        # the page uses build/minifs.exe behind the scenes
node gui/server.js         # then open http://localhost:3000
```

The server runs `minifs --quiet` for every action, so all the file-system
work is still done by the C++ program, and changes go through the journal as
usual. The page refreshes every few seconds, so it also shows changes typed in
the MiniFS terminal; the terminal re-reads the disk before each command, so
both can be open at once.

| What you do on the page | MiniFS command it runs |
|---|---|
| New folder / New file | `createdir` / `create` |
| Double-click a file, edit, Save | `read` / `write` |
| Rename, Delete | `rename`, `delete` / `deletedir` |
| Copy + Paste, Cut + Paste | `copy`, `move` |
| Properties → Read-only | `chmod` |
| Format… | `format MB --alloc=MODE` |

Keyboard: Enter opens, Backspace goes up, F2 renames, Delete deletes,
Ctrl+C / Ctrl+X / Ctrl+V copy, cut and paste, F5 refreshes.

**Several disks.** Click **Disks** at the top of the sidebar to see every
virtual disk: `virtual_disk.bin` in the project folder and every disk in the
`disks/` folder. Each disk shows its allocation method, size and free space,
with **Open** and **Delete** buttons; **New disk…** creates a named disk in
`disks/`. Only real MiniFS disk files (they start with `MNFS`) are listed, and
only those can be deleted. A disk that is open in the MiniFS terminal cannot
be deleted until the terminal is closed. To use a GUI-made disk in the
terminal: `build\minifs.exe --disk "disks\Demo disk.bin"`.

Settings: `PORT` (default 3000), `MINIFS_EXE` (path to the program),
`MINIFS_DISK` (disk opened at start, default `virtual_disk.bin` in the project
folder), `MINIFS_DISKS_DIR` (where new disks go, default `disks/`).

## Commands

Every command has a plain-English name; the Unix name in the second column
works too. Type `help` inside MiniFS for the same list.

| Command | Unix | What it does |
|---|---|---|
| `format [MB] [--alloc=MODE]` | | Create a fresh disk (1–1024 MB, default 100). MODE: `indexed`, `linked`, `contiguous` |
| `diskinfo` | | Show the disk layout |
| `space` | `df` | Used and free blocks and inodes |
| `where` | `pwd` | Show the current directory |
| `goto [path]` | `cd` | Change directory (no path: `/`) |
| `list [-l] [path]` | `ls` | List a directory; `-l` shows permissions, links, size, time |
| `createdir <path>` | `mkdir` | Create a directory |
| `deletedir <path>` | `rmdir` | Delete an empty directory |
| `create <path>` | `touch` | Create an empty file |
| `write <path> <text>` | | Replace a file's text (quotes keep extra spaces; `\n` is a newline) |
| `append <path> <text>` | | Add text to the end of a file |
| `fill <path> <KB>` | | Write KB of sample text (for large-file and fragmentation demos) |
| `read <path>` | `cat` | Show a file's contents |
| `delete <path>` | `rm` | Delete a file |
| `rename <old> <new>` | | Rename or move to an exact path |
| `move <from> <to>` | `mv` | Move; if `<to>` is a directory the entry goes inside it |
| `copy <from> <to>` | `cp` | Copy a file; `<to>` may be a directory |
| `chmod <mode> <path>` | | Set permission bits, e.g. `chmod 444 notes.txt` |
| `info <path>` | `stat` | Inode number, size, mode, times, block list, extents |
| `frag` | | Fragmentation report |
| `map [first] [count]` | | ASCII block map |
| `defrag` | | Compact files into contiguous blocks |
| `export [file\|on\|off]` | | Write `disk_state.json`; `on` rewrites it after every change |
| `crash [point]` | | Simulate a power cut during the next change (see below) |
| `check [repair]` | `fsck` | Check consistency; `repair` fixes what it finds |
| `bench` | | Compare the three allocation modes on one workload |
| `exit` | `quit` | Leave MiniFS |

Lines starting with `#` are comments, so command scripts can be annotated.

## Disk layout

`format` lays the disk out as:

```text
| Block 0    | Blocks 1..B | Next I blocks | Next J blocks | Remaining blocks |
| Superblock | Free bitmap | Inode table   | Journal       | Data blocks      |
```

For the default 100 MB disk: 25,600 blocks of 4,096 bytes; bitmap block 1;
inode table blocks 2–201 (6,400 inodes); journal blocks 202–329; data from
block 330. `diskinfo` prints the layout of any disk.

- **Superblock** (block 0): magic `MNFS`, format version, block size, block
  count, the start and length of every region, free block and inode counts,
  root inode, allocation mode, timestamps.
- **Free bitmap:** one bit per block, 1 = used. Metadata blocks are always used.
- **Inode table:** 128-byte inodes, one per 4 blocks. Inode 0 is reserved
  (it means "no inode"); inode 1 is the root directory.
- **Journal:** a header block (state, sequence number, checksum, target block
  numbers) followed by block images. 1 block per 64 disk blocks, 16–128 blocks.
- **Data blocks:** file and directory contents. A directory is a file of
  64-byte entries (4-byte inode number + 60-byte name).

All on-disk structs are in [include/minifs/fs_types.hpp](include/minifs/fs_types.hpp)
and are checked with `static_assert`, so the layout can be inspected in a hex
editor such as HxD.

## Allocation modes

| Mode | How a file's blocks are recorded | Strength | Weakness |
|---|---|---|---|
| `indexed` (default) | 10 direct pointers + 1 indirect block of 1024 pointers | Any block in ≤ 2 reads | Pointer overhead; files can scatter |
| `linked` | First block in the inode; each block ends with a 4-byte pointer to the next | Free space never splits | Reaching block *k* needs *k* reads; 4 bytes lost per block |
| `contiguous` | First block in the inode; the file is one run | One read to any block | External fragmentation: a big file can fail even when there is room |

`bench` runs the same workload (write 80 files, delete every other one, grow
some, write 50 bigger ones, then one file using 75% of the free space) on a
scratch disk under each mode and prints the comparison, including how often
contiguous allocation fails for lack of a long enough run.

## Journaling and crash recovery

Every command that changes the disk runs as **one transaction**:

1. File data is written to newly allocated blocks (nothing points at them yet).
2. Metadata changes (superblock, bitmap, inodes, directories, indirect blocks)
   are collected in memory and written to the journal with a *Pending* header.
3. The header is rewritten as *Committed*.
4. The changes are copied to their real blocks.
5. The header is cleared.

At mount, a *Committed* transaction is replayed (the command completes) and a
*Pending* one is discarded (the command is rolled back). Blocks freed by a
transaction are not reused until it commits. If a command fails part-way (for
example "disk full"), nothing it did reaches the disk.

The `crash` command picks where the next change stops:

| `crash ...` | Stops | On the next start |
|---|---|---|
| `before-commit` | after the journal is written, before the commit | Discarded – old state kept |
| `after-commit` (default) | right after the commit | Replayed – new state |
| `mid-apply` | halfway through copying blocks | Replayed – new state |
| `no-journal` | halfway through, journal bypassed | No recovery: `check` reports the damage, `check repair` fixes it |

`check` (fsck) verifies the journal is empty, every inode's block pointers are
valid, no block belongs to two files, the bitmap matches the blocks really in
use, every inode is reachable from `/`, `.`/`..` entries and link counts are
right, and the superblock counters are correct.

## Demo and test scripts

| File | Purpose |
|---|---|
| [scripts/demo.txt](scripts/demo.txt) | The presentation demo: layout, files, indirect block, fragmentation, defrag, allocation comparison, permissions, `space` |
| [scripts/demo_crash_1.txt](scripts/demo_crash_1.txt), [scripts/demo_crash_2.txt](scripts/demo_crash_2.txt) | Crash during a write, then journal replay and `check` (run after `demo.txt`) |
| [tests/all_commands.txt](tests/all_commands.txt) | Every command and edge case: disk full, long names, deep nesting, deleting the current directory, duplicate names, permission denied |
| [tests/fs_tests.cpp](tests/fs_tests.cpp) | 30 unit tests: layout, bitmap, inodes, paths, files, all allocation modes, permissions, rollback, every crash point, fsck, defrag, export |

```bash
build/minifs scripts/demo.txt
build/minifs scripts/demo_crash_1.txt
build/minifs scripts/demo_crash_2.txt
```

Run all tests (unit tests, the all-commands script, and the two-run crash test):

```bash
ctest --test-dir build --output-on-failure
```

## Source layout

```text
include/minifs/
  fs_types.hpp      on-disk structs and constants (superblock, inode, dir entry, journal header)
  fs_errors.hpp     error types shown to the user
  virtual_disk.hpp  disk layer: read/write fixed-size blocks of virtual_disk.bin
  block_bitmap.hpp  free-space bitmap and the block allocators
  inode_table.hpp   inode allocate/free/read/write
  journal.hpp       write-ahead journal, commit, recovery, crash points
  file_system.hpp   files, directories, paths, permissions, allocation modes, fsck, defrag
  cli.hpp           command-line interface
src/
  virtual_disk.cpp  block_bitmap.cpp  inode_table.cpp  journal.cpp
  file_system.cpp   core: transactions, content storage, directories, file operations
  fs_inspect.cpp    stat, df, frag, map, export, fsck, defrag
  cli.cpp           tokenizer, commands, benchmark
  main.cpp          program entry point
tests/              unit tests and CLI scenario scripts
scripts/            demo scripts
gui/                web interface: server.js (Node) + public/ (HTML, CSS, JS)
```

## Scope notes

- Instead of the live block-grid visualizer from the project plan, the GUI is
  a simple file explorer (`gui/`). `export` still produces the full
  `disk_state.json` (block map, inodes, statistics) for a richer viewer.
- MiniFS has a single user who owns every file, so permission checks use the
  owner bits; group and other bits are stored and shown but not enforced.
- Timestamps record creation and modification; reads do not update the access
  time, so that reading never counts as a change to the disk.
