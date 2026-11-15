# MiniFS – Mini File System Simulator

Operating Systems project, Section H — Muhammad Talha, Usman Shah, Abdul Rafiu.

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

## How to run the project

The steps below are for Windows (PowerShell or cmd). On Linux or macOS the
commands are the same except that the program is `./build/minifs` and paths
use `/` instead of `\`. Run every command from the project folder.

### Step 1 – Install the tools (once)

| Tool | Needed for | Where to get it |
|---|---|---|
| CMake 3.16 or newer | Building | <https://cmake.org/download/> |
| A C++17 compiler: GCC 8 or newer (MinGW-w64), or Visual Studio 2019+ with "Desktop development with C++" | Building | <https://winlibs.com/> or <https://visualstudio.microsoft.com/> |
| Node.js 16 or newer | Web interface only | <https://nodejs.org/> |
| Git (or GitHub Desktop) | Getting the code | <https://git-scm.com/> |

Check that they work (the GCC version must be 8 or higher):

```powershell
cmake --version
g++ --version
node --version
```

### Step 2 – Get the code

```powershell
git clone https://github.com/Usman-Shah-Rashdi/OS-project.git
cd OS-project
```

### Step 3 – Build

```powershell
cmake -S . -B build
cmake --build build
```

The build is done when it finishes without `error` lines and
`build\minifs.exe` exists. After changing any C++ code, run only
`cmake --build build` again.

- **Old compiler:** the GCC 4.9 that comes with Code::Blocks cannot compile
  C++17. If CMake picks it up, delete the `build` folder and point CMake at a
  newer compiler:
  ```powershell
  cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER="C:/path/to/mingw64/bin/g++.exe"
  cmake --build build
  ```
- **Visual Studio:** the program is built as `build\Debug\minifs.exe`; use
  that path in the steps below.
- With MinGW the C++ runtime is linked in, so `minifs.exe` runs from any
  terminal without extra DLLs.

### Step 4 – Run MiniFS (command line)

```powershell
build\minifs.exe
```

The first time, there is no disk yet, so create one with `format`. The disk is
saved as `virtual_disk.bin` in the folder you started MiniFS from, and it is
loaded again automatically next time.

```text
minifs> format
Formatted virtual_disk.bin: 100 MB, 25600 blocks of 4096 bytes, 6399 inodes, indexed allocation.
minifs> createdir /docs
minifs> goto /docs
minifs> create notes.txt
minifs> write notes.txt "Hello MiniFS"
minifs> read notes.txt
Hello MiniFS
minifs> list -l
-rw-r--r--   1         12  2026-11-15 19:37:21  notes.txt
minifs> help
minifs> exit
```

Type `help` for every command (also listed in [Commands](#commands) below).
Other ways to start it:

```powershell
build\minifs.exe scripts\demo.txt        # run the commands in a script file
build\minifs.exe --disk other.bin        # use (or create) a different disk file
```

### Step 5 – Run the demo

The presentation demo formats a 16 MB disk and shows files, a large file with
an indirect block, fragmentation and `defrag`, the allocation comparison,
permissions and disk usage. The crash demo then interrupts a write and shows
the journal recovering it on the next start.

```powershell
build\minifs.exe scripts\demo.txt
build\minifs.exe scripts\demo_crash_1.txt
build\minifs.exe scripts\demo_crash_2.txt
```

Note: `demo.txt` formats `virtual_disk.bin`, which erases what was on it.

### Step 6 – Run the tests

```powershell
ctest --test-dir build --output-on-failure
```

Expected result: `100% tests passed out of 4` (the 30 unit tests, the
all-commands script, and the two-part crash test).

### Step 7 – Run the web interface

Build MiniFS first (Step 3), then start the server:

```powershell
node gui\server.js
```

Open <http://localhost:3000> in Edge or Chrome. Keep the terminal open while
you use the page, and press **Ctrl+C** in it to stop the server. If the page
says the disk needs formatting, click **Format this disk…** or **New disk…**.
More details are in [Web interface](#web-interface-minifs-explorer).

### Troubleshooting

| Problem | Solution |
|---|---|
| `cannot open output file minifs.exe: Permission denied` while building | MiniFS is still running. Type `exit` in it (or close that terminal), then build again. In PowerShell, `Stop-Process -Name minifs` also works. |
| Errors about C++17 or `-std=c++17` while building | The compiler is too old. Delete the `build` folder and configure with a GCC 8+ compiler (see Step 3). |
| `Disk uses MiniFS format version 1 ... Run 'format'` | The disk was made by an older MiniFS. Run `format` (this erases it), or delete `virtual_disk.bin`. |
| `No file system is mounted. Run 'format' first.` | There is no disk in this folder yet. Run `format`, or start MiniFS from the project folder. |
| The web page says it cannot reach the server | Start it with `node gui\server.js` and keep that terminal open. |
| The server prints `MiniFS program not found` | Build MiniFS first (Step 3). |
| Port 3000 is already in use | Use another port. PowerShell: `$env:PORT=3001; node gui\server.js`. cmd: `set PORT=3001` then `node gui\server.js`. Open `http://localhost:3001`. |
| `$env:...` gives an error | That is PowerShell syntax; you are in cmd. Use the cmd form or open PowerShell. |

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
