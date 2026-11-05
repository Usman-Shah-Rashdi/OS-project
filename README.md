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

The first storage layer is implemented. Use `format` to create a 100 MB disk
with 4 KB blocks, or provide a size in megabytes:

```text
minifs> format
minifs> format 50
minifs> diskinfo
```

The CLI can now create, open, and inspect `virtual_disk.bin`. File-system
metadata, directories, allocation strategies, persistence rules, recovery, and
the visualizer will be implemented incrementally.
