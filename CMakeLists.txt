cmake_minimum_required(VERSION 3.16)

project(MiniFS VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# File-system core and CLI, shared by the program and the tests.
add_library(minifs_core STATIC
    src/virtual_disk.cpp
    src/block_bitmap.cpp
    src/inode_table.cpp
    src/journal.cpp
    src/file_system.cpp
    src/fs_inspect.cpp
    src/cli.cpp
)

target_include_directories(minifs_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (MSVC)
    target_compile_options(minifs_core PUBLIC /W4 /permissive-)
else()
    target_compile_options(minifs_core PUBLIC -Wall -Wextra -Wpedantic)
endif()

# MinGW: link the GCC runtime statically so minifs.exe runs on machines (and
# terminals) without the compiler's DLLs on PATH.
if (MINGW)
    target_link_options(minifs_core PUBLIC -static -static-libgcc -static-libstdc++)
endif()

add_executable(minifs src/main.cpp)
target_link_libraries(minifs PRIVATE minifs_core)

# ---------------------------------------------------------------------------
# Tests: `ctest --test-dir build --output-on-failure`
# ---------------------------------------------------------------------------
enable_testing()

add_executable(minifs_tests tests/fs_tests.cpp)
target_link_libraries(minifs_tests PRIVATE minifs_core)
add_test(NAME unit_tests
         COMMAND minifs_tests
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Every command and edge case through the real CLI; must end with a clean fsck.
add_test(NAME cli_all_commands
         COMMAND minifs --disk cli_test.bin ${CMAKE_CURRENT_SOURCE_DIR}/tests/all_commands.txt
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(cli_all_commands PROPERTIES
    PASS_REGULAR_EXPRESSION "ALL-COMMANDS-DONE"
    FAIL_REGULAR_EXPRESSION "Unknown command;problem\\(s\\) found;could not be repaired")

# Crash demo in two runs: crash after the journal commit, then recover.
add_test(NAME cli_crash_1_crash
         COMMAND minifs --disk crash_test.bin ${CMAKE_CURRENT_SOURCE_DIR}/tests/crash_part1.txt
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(cli_crash_1_crash PROPERTIES
    PASS_REGULAR_EXPRESSION "Simulated crash after the journal commit")
add_test(NAME cli_crash_2_recover
         COMMAND minifs --disk crash_test.bin ${CMAKE_CURRENT_SOURCE_DIR}/tests/crash_part2.txt
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(cli_crash_2_recover PROPERTIES
    DEPENDS cli_crash_1_crash
    PASS_REGULAR_EXPRESSION "replayed it.*survived the crash.*file system is clean"
    FAIL_REGULAR_EXPRESSION "problem\\(s\\) found")
