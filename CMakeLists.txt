cmake_minimum_required(VERSION 3.16)

project(MiniFS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# File-system core, shared by the CLI and the tests.
add_library(minifs_core STATIC
    src/virtual_disk.cpp
    src/block_bitmap.cpp
    src/inode_table.cpp
    src/file_system.cpp
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

add_executable(minifs src/main.cpp)
target_link_libraries(minifs PRIVATE minifs_core)

enable_testing()
add_executable(minifs_tests tests/fs_tests.cpp)
target_link_libraries(minifs_tests PRIVATE minifs_core)
add_test(NAME minifs_tests
         COMMAND minifs_tests
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
