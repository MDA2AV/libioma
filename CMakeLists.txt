# CMake build for the ioma library plus the playground examples.
#
# Consume it from another CMake project either as a subdirectory:
#     add_subdirectory(ioma)
#     target_link_libraries(myapp PRIVATE ioma::ioma)
# or, after `cmake --install`, via find_package:
#     find_package(ioma REQUIRED)
#     target_link_libraries(myapp PRIVATE ioma::ioma)
cmake_minimum_required(VERSION 3.21)   # PROJECT_IS_TOP_LEVEL
project(ioma VERSION 0.1.0 LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)                 # gnu11: _GNU_SOURCE, typeof, statement exprs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Release, Debug, RelWithDebInfo, ..." FORCE)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O3 -g")

find_package(Threads REQUIRED)
include(GNUInstallDirs)

# --- the library (static by default; -DBUILD_SHARED_LIBS=ON for a .so) ---
add_library(ioma
        src/uring.c
        src/coro.c
        src/switch_x86_64.S
        src/bufring.c
        src/conn.c
        src/proactor.c
        src/http.c
        src/api.c
        src/router.c
        src/run.c
        third_party/picohttpparser/picohttpparser.c)
add_library(ioma::ioma ALIAS ioma)

target_include_directories(ioma
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ioma>
        PRIVATE
            third_party/picohttpparser)
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
install(FILES
            include/ioma.h include/http.h include/proactor.h include/coro.h include/uring.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ioma)
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
