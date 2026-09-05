# CMake build for ioma. The hand-written Makefile still works for the command line; this file is
# what CLion and clangd want - open the folder and "Load CMake Project" on it, and code insight,
# run and debug all work. The .S context switch needs the ASM language enabled, which is why the
# project() line lists both C and ASM.
cmake_minimum_required(VERSION 3.16)
project(ioma LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)                 # gnu11: _GNU_SOURCE, typeof, statement exprs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)      # compile_commands.json for clangd / external tools

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Release, Debug, RelWithDebInfo, ..." FORCE)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2 -g")        # keep symbols, like the Makefile

find_package(Threads REQUIRED)

add_executable(ioma
        src/uring.c
        src/coro.c
        src/proactor.c
        src/main.c
        src/switch_x86_64.S)

target_include_directories(ioma PRIVATE include)
target_compile_options(ioma PRIVATE -Wall -Wextra)
target_link_libraries(ioma PRIVATE Threads::Threads)

# The tiny / trace builds from tests/stress.py are one-off gcc invocations, not targets here.
