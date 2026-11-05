cmake_minimum_required(VERSION 3.16)

project(MiniFS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(minifs
    src/main.cpp
    src/virtual_disk.cpp
)

target_include_directories(minifs
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (MSVC)
    target_compile_options(minifs PRIVATE /W4 /permissive-)
else()
    target_compile_options(minifs PRIVATE -Wall -Wextra -Wpedantic)
endif()
