# CMake build for the ioxd library plus the playground examples.
#
# Consume it from another CMake project either as a subdirectory:
#     add_subdirectory(ioxd)
#     target_link_libraries(myapp PRIVATE ioxd::ioxd)
# or, after `cmake --install`, via find_package:
#     find_package(ioxd REQUIRED)
#     target_link_libraries(myapp PRIVATE ioxd::ioxd)
cmake_minimum_required(VERSION 3.21)   # PROJECT_IS_TOP_LEVEL, find_program(NO_CACHE)

# The compiler: unless one is given (-DCMAKE_C_COMPILER, CC in the environment, a toolchain file),
# the newest gcc on the PATH - a distro's `gcc` is often older than a `gcc-NN` installed beside it,
# and the code is C23, which needs gcc 14. Decided before project(), where CMake fixes the
# compiler; the make build does the same.
if(NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC} AND NOT CMAKE_TOOLCHAIN_FILE)
    set(ioxd_newest_gcc "")
    set(ioxd_newest_gcc_version 0)
    foreach(candidate gcc gcc-14 gcc-15 gcc-16)
        unset(candidate_path)
        find_program(candidate_path ${candidate} NO_CACHE)
        if(candidate_path)
            execute_process(COMMAND ${candidate_path} -dumpfullversion
                            OUTPUT_VARIABLE candidate_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(candidate_version VERSION_GREATER ioxd_newest_gcc_version)
                set(ioxd_newest_gcc ${candidate_path})
                set(ioxd_newest_gcc_version ${candidate_version})
            endif()
        endif()
    endforeach()
    if(ioxd_newest_gcc)
        set(CMAKE_C_COMPILER ${ioxd_newest_gcc})
    endif()
endif()

project(ioxd VERSION 0.1.0 LANGUAGES C ASM)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)                 # gnu23: _GNU_SOURCE, statement exprs
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "libioxd is C23 and needs gcc 14 or newer; this is gcc ${CMAKE_C_COMPILER_VERSION}. "
                        "Configure with -DCMAKE_C_COMPILER=gcc-14 (in CLion: Settings > Build > Toolchains).")
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Release, Debug, RelWithDebInfo, ..." FORCE)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O3 -g")

find_package(Threads REQUIRED)
include(GNUInstallDirs)

# --- the library (static by default; -DBUILD_SHARED_LIBS=ON for a .so) ---
add_library(ioxd
        lib/io/uring.c
        lib/io/coro.c
        lib/io/switch_x86_64.S
        lib/io/bufring.c
        lib/io/conn.c
        lib/io/proactor.c
        lib/io/pipe.c
        lib/http/engine.c
        lib/http/api.c
        lib/http/router.c
        lib/http/run.c
        lib/json/json.c
        third_party/picohttpparser/picohttpparser.c)
add_library(ioxd::ioxd ALIAS ioxd)

target_include_directories(ioxd
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            lib
            third_party/picohttpparser)
target_compile_definitions(ioxd PRIVATE _GNU_SOURCE)
target_compile_options(ioxd PRIVATE -Wall -Wextra)
# Fat LTO objects when supported: the archive stays plain-linkable, and consumers that link with
# -flto get cross-file inlining (the demo below does).
include(CheckCCompilerFlag)
check_c_compiler_flag("-flto -ffat-lto-objects" IOXD_HAVE_FAT_LTO)
if(IOXD_HAVE_FAT_LTO)
    target_compile_options(ioxd PRIVATE -flto -ffat-lto-objects)
    target_link_options(ioxd PRIVATE -flto)
endif()
target_link_libraries(ioxd PUBLIC Threads::Threads)
set_target_properties(ioxd PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)

# Vendored parser: silence its warnings, they are not ours to fix.
set_source_files_properties(third_party/picohttpparser/picohttpparser.c
        PROPERTIES COMPILE_OPTIONS "-w")

# --- playground examples (off when ioxd is a subproject) ---
option(IOXD_EXAMPLES "Build the playground examples" ${PROJECT_IS_TOP_LEVEL})
if(IOXD_EXAMPLES)
    add_executable(ioxd-hello playground/hello/main.c)
    target_link_libraries(ioxd-hello PRIVATE ioxd::ioxd)
    add_executable(ioxd-test-server tests/server.c)
    target_link_libraries(ioxd-test-server PRIVATE ioxd::ioxd)
    add_executable(ioxd-pipe-server tests/pipe-server.c)
    target_link_libraries(ioxd-pipe-server PRIVATE ioxd::ioxd)
    add_executable(ioxd-unit tests/unit.c)
    target_link_libraries(ioxd-unit PRIVATE ioxd::ioxd)
    enable_testing()
    add_test(NAME unit COMMAND ioxd-unit)
    if(IOXD_HAVE_FAT_LTO)
        target_compile_options(ioxd-hello PRIVATE -flto)
        target_link_options(ioxd-hello PRIVATE -flto)
        target_compile_options(ioxd-test-server PRIVATE -flto)
        target_link_options(ioxd-test-server PRIVATE -flto)
    endif()
endif()

# --- install + export so find_package(ioxd) works ---
install(TARGETS ioxd EXPORT ioxdTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/ioxd.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ioxd DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ioxdTargets
        NAMESPACE ioxd::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ioxd
        FILE ioxdTargets.cmake)

# A config file that re-finds the public Threads dependency, then loads the exported targets.
include(CMakePackageConfigHelpers)
configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ioxdConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/ioxdConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ioxd)
write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/ioxdConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/ioxdConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/ioxdConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ioxd)
