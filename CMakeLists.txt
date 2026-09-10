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
        lib/clients/timer.c
        lib/clients/socket.c
        lib/http/engine.c
        lib/http/api.c
        lib/http/find.c
        lib/http/static.c
        lib/http/compress.c
        lib/http/router.c
        lib/http/run.c
        lib/json/json.c
        lib/tls/certs.c
        lib/tls/handshake.c
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
# TLS: OpenSSL for the handshake only; the kernel does the records.
option(IOXD_TLS "TLS listeners (needs OpenSSL 3)" ON)
if(IOXD_TLS)
    find_package(OpenSSL 3 REQUIRED)
    target_compile_definitions(ioxd PRIVATE IOXD_TLS=1)
    target_link_libraries(ioxd PUBLIC OpenSSL::SSL OpenSSL::Crypto)
else()
    target_compile_definitions(ioxd PRIVATE IOXD_TLS=0)
endif()
target_compile_options(ioxd PRIVATE -Wall -Wextra)
# Fat LTO objects when supported: the archive stays plain-linkable, and consumers that link with
# -flto get cross-file inlining (the demo below does).
include(CheckCCompilerFlag)
check_c_compiler_flag("-flto -ffat-lto-objects" IOXD_HAVE_FAT_LTO)
if(IOXD_HAVE_FAT_LTO)
    target_compile_options(ioxd PRIVATE -flto -ffat-lto-objects)
    target_link_options(ioxd PRIVATE -flto)
endif()
# A coroutine's stack ends at a guard page, which only stops a frame that touches every page as it
# grows: stack clash protection makes the compiler emit those probes.
check_c_compiler_flag(-fstack-clash-protection IOXD_HAVE_STACK_CLASH)
if(IOXD_HAVE_STACK_CLASH)
    target_compile_options(ioxd PRIVATE -fstack-clash-protection)
endif()
# A shared build exports ioxd_* and nothing else, through the version script the make build uses.
set(IOXD_VERSION_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ioxd.map)
if(BUILD_SHARED_LIBS)
    target_link_options(ioxd PRIVATE -Wl,--version-script,${IOXD_VERSION_SCRIPT})
    set_target_properties(ioxd PROPERTIES LINK_DEPENDS ${IOXD_VERSION_SCRIPT})
endif()
target_link_libraries(ioxd PUBLIC Threads::Threads)
# Response compression: brotli and zlib, each AUTO (when pkg-config finds it), ON (required) or OFF.
find_package(PkgConfig QUIET)
foreach(codec IN ITEMS BROTLI ZLIB)
    if(codec STREQUAL "BROTLI")
        set(module libbrotlienc)
    else()
        set(module zlib)
    endif()
    set(IOXD_${codec} AUTO CACHE STRING "compress with ${module}: AUTO, ON or OFF")
    set(IOXD_${codec}_FLAG 0)
    if(NOT IOXD_${codec} STREQUAL "OFF" AND PKG_CONFIG_FOUND)
        pkg_check_modules(${codec} IMPORTED_TARGET ${module})
        if(${codec}_FOUND)
            target_link_libraries(ioxd PUBLIC PkgConfig::${codec})
            set(IOXD_${codec}_FLAG 1)
        elseif(IOXD_${codec} STREQUAL "ON")
            message(FATAL_ERROR "IOXD_${codec}=ON but pkg-config does not find ${module}")
        endif()
    endif()
    target_compile_definitions(ioxd PRIVATE IOXD_${codec}=${IOXD_${codec}_FLAG})
    message(STATUS "${codec}: ${IOXD_${codec}_FLAG}")
endforeach()
set_target_properties(ioxd PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)

# Vendored parser: silence its warnings, they are not ours to fix.
set_source_files_properties(third_party/picohttpparser/picohttpparser.c
        PROPERTIES COMPILE_OPTIONS "-w")

# --- playground examples (off when ioxd is a subproject) ---
option(IOXD_EXAMPLES "Build the playground examples" ${PROJECT_IS_TOP_LEVEL})
if(IOXD_EXAMPLES)
    add_executable(ioxd-hello playground/hello/main.c)
    target_link_libraries(ioxd-hello PRIVATE ioxd::ioxd)
    file(GLOB IOXD_EXAMPLE_SRC CONFIGURE_DEPENDS playground/examples/*.c)   # the manual's examples
    foreach(src ${IOXD_EXAMPLE_SRC})
        get_filename_component(name ${src} NAME_WE)
        add_executable(ioxd-example-${name} ${src})
        target_link_libraries(ioxd-example-${name} PRIVATE ioxd::ioxd)
    endforeach()
    add_executable(ioxd-test-server tests/server.c)
    target_link_libraries(ioxd-test-server PRIVATE ioxd::ioxd)
    add_executable(ioxd-pipe-server tests/pipe-server.c)
    target_link_libraries(ioxd-pipe-server PRIVATE ioxd::ioxd)
    add_executable(ioxd-unit tests/unit.c)
    target_link_libraries(ioxd-unit PRIVATE ioxd::ioxd)
    target_include_directories(ioxd-unit PRIVATE lib)   # http/find.h: the search, one function per instruction set
    add_executable(ioxd-router-test tests/router_test.c)
    target_link_libraries(ioxd-router-test PRIVATE ioxd::ioxd)
    # The suites, through the one script `make check` runs, so the sequence lives in one place.
    # The three that talk to the HTTP fixture share one entry, and so one fixture on one port, as
    # they do under make: a fixture bound to a port the suite before it filled with TIME_WAIT
    # connections has some of its new ones reset. Both entries bind fixed ports, so neither may
    # run beside anything else.
    enable_testing()
    set(IOXD_CHECK_PORT 8099    CACHE STRING "first port the fixture listens on; it takes the two after it")
    set(IOXD_PIPE_PORT  8102    CACHE STRING "port the pipe fixture listens on")
    set(IOXD_TLS_PYTHON python3 CACHE STRING "a python with tlslite-ng, for tests/tls_early.py")
    add_test(NAME unit COMMAND ioxd-unit)
    add_test(NAME router COMMAND ioxd-router-test)
    add_test(NAME suites COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-suites.sh
             --suite smoke --suite conformance --suite stress --suite tls --suite static --suite compress
             --port ${IOXD_CHECK_PORT}
             --server $<TARGET_FILE:ioxd-test-server>
             --tls-python ${IOXD_TLS_PYTHON}
             --work ${CMAKE_CURRENT_BINARY_DIR}/check/suites)
    add_test(NAME pipes COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-suites.sh
             --suite pipes
             --pipe-port ${IOXD_PIPE_PORT}
             --pipe-server $<TARGET_FILE:ioxd-pipe-server>
             --work ${CMAKE_CURRENT_BINARY_DIR}/check/pipes)
    set_tests_properties(suites pipes PROPERTIES RUN_SERIAL TRUE)
    add_custom_target(check
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            DEPENDS ioxd-unit ioxd-router-test ioxd-test-server ioxd-pipe-server
            USES_TERMINAL)
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

# A config file that re-finds the public dependencies - Threads, and OpenSSL when this build has
# TLS, which it is told through IOXD_TLS - then loads the exported targets.
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
