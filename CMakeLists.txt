# CMake build for the ioma library plus the playground examples.
#
# Consume it from another CMake project either as a subdirectory:
#     add_subdirectory(ioma)
#     target_link_libraries(myapp PRIVATE ioma::ioma)
# or, after `cmake --install`, via find_package:
#     find_package(ioma REQUIRED)
#     target_link_libraries(myapp PRIVATE ioma::ioma)
cmake_minimum_required(VERSION 3.21)   # PROJECT_IS_TOP_LEVEL, find_program(NO_CACHE)

# The compiler: unless one is given (-DCMAKE_C_COMPILER, CC in the environment, a toolchain file),
# the newest gcc on the PATH - a distro's `gcc` is often older than a `gcc-NN` installed beside it,
# and the code is C23, which needs gcc 14. Decided before project(), where CMake fixes the
# compiler; the make build does the same.
if(NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC} AND NOT CMAKE_TOOLCHAIN_FILE)
    set(ioma_newest_gcc "")
    set(ioma_newest_gcc_version 0)
    foreach(candidate gcc gcc-14 gcc-15 gcc-16)
        unset(candidate_path)
        find_program(candidate_path ${candidate} NO_CACHE)
        if(candidate_path)
            execute_process(COMMAND ${candidate_path} -dumpfullversion
                            OUTPUT_VARIABLE candidate_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(candidate_version VERSION_GREATER ioma_newest_gcc_version)
                set(ioma_newest_gcc ${candidate_path})
                set(ioma_newest_gcc_version ${candidate_version})
            endif()
        endif()
    endforeach()
    if(ioma_newest_gcc)
        set(CMAKE_C_COMPILER ${ioma_newest_gcc})
    endif()
endif()

project(ioma VERSION 0.1.0 LANGUAGES C ASM)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)                 # gnu23: _GNU_SOURCE, statement exprs
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "libioma is C23 and needs gcc 14 or newer; this is gcc ${CMAKE_C_COMPILER_VERSION}. "
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
add_library(ioma
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
add_library(ioma::ioma ALIAS ioma)

target_include_directories(ioma
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ioma>
        PRIVATE
            lib
            third_party/picohttpparser)
target_compile_definitions(ioma PRIVATE _GNU_SOURCE)
target_compile_options(ioma PRIVATE -Wall -Wextra)
# Fat LTO objects when supported: the archive stays plain-linkable, and consumers that link with
# -flto get cross-file inlining (the demo below does).
include(CheckCCompilerFlag)
check_c_compiler_flag("-flto -ffat-lto-objects" IOMA_HAVE_FAT_LTO)
if(IOMA_HAVE_FAT_LTO)
    target_compile_options(ioma PRIVATE -flto -ffat-lto-objects)
    target_link_options(ioma PRIVATE -flto)
endif()
target_link_libraries(ioma PUBLIC Threads::Threads)
set_target_properties(ioma PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)

# Vendored parser: silence its warnings, they are not ours to fix.
set_source_files_properties(third_party/picohttpparser/picohttpparser.c
        PROPERTIES COMPILE_OPTIONS "-w")

# --- playground examples (off when ioma is a subproject) ---
option(IOMA_EXAMPLES "Build the playground examples" ${PROJECT_IS_TOP_LEVEL})
if(IOMA_EXAMPLES)
    add_executable(ioma-hello playground/hello/main.c)
    target_link_libraries(ioma-hello PRIVATE ioma::ioma)
    add_executable(ioma-test-server tests/server.c)
    target_link_libraries(ioma-test-server PRIVATE ioma::ioma)
    add_executable(ioma-pipe-server tests/pipe-server.c)
    target_link_libraries(ioma-pipe-server PRIVATE ioma::ioma)
    add_executable(ioma-unit tests/unit.c)
    target_link_libraries(ioma-unit PRIVATE ioma::ioma)
    enable_testing()
    add_test(NAME unit COMMAND ioma-unit)
    if(IOMA_HAVE_FAT_LTO)
        target_compile_options(ioma-hello PRIVATE -flto)
        target_link_options(ioma-hello PRIVATE -flto)
        target_compile_options(ioma-test-server PRIVATE -flto)
        target_link_options(ioma-test-server PRIVATE -flto)
    endif()
endif()

# --- install + export so find_package(ioma) works ---
install(TARGETS ioma EXPORT iomaTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/ioma.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ioma)
install(EXPORT iomaTargets
        NAMESPACE ioma::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ioma
        FILE iomaTargets.cmake)

# A config file that re-finds the public Threads dependency, then loads the exported targets.
include(CMakePackageConfigHelpers)
configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/iomaConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/iomaConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ioma)
write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/iomaConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/iomaConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/iomaConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ioma)
