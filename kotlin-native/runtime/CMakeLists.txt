# Disclaimer: This script is configured to generate indexing configurations for Clangd.
cmake_minimum_required(VERSION 3.15...3.29)
project(runtime VERSION 1.0.0 LANGUAGES CXX C)

# Force CMake to output compile_commands.json for Clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 1. Enforce C++ Standard & System Definitions
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)
set(GOOGLETEST_DIR ${CMAKE_SOURCE_DIR}/googletest)

# 2. Automatically find ALL source files across all subdirectories
file(GLOB_RECURSE ALL_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cxx"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.mm"
)

# 3. Filter out the build directory to avoid infinite loops
list(FILTER ALL_SOURCES EXCLUDE REGEX "${CMAKE_CURRENT_BINARY_DIR}/.*")

# 4. Automatically detect all header directories
file(GLOB_RECURSE ALL_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

set(INCLUDE_DIRS "")
foreach(header_file ${ALL_HEADERS})
    get_filename_component(header_dir ${header_file} DIRECTORY)
    list(APPEND INCLUDE_DIRS ${header_dir})
endforeach()
list(REMOVE_DUPLICATES INCLUDE_DIRS)

# 5. Define 'runtime' directly as a library target since there is no main function
add_library(runtime STATIC ${ALL_SOURCES})

# 6. Target-scoped Include Directories
target_include_directories(runtime PUBLIC
    ${INCLUDE_DIRS}
    ${SRC_DIR}/main/cpp
    ${SRC_DIR}/debug/headers
    ${CMAKE_SOURCE_DIR}/../common/src/hash/headers
    ${GOOGLETEST_DIR}/googletest/include
    ${GOOGLETEST_DIR}/googlemock/include
)

# 7. Global Compiler Flags / Macros for Clangd parsing
target_compile_definitions(runtime PUBLIC
    -DKONAN_OSX=1
    -DKONAN_MACOSX=1
    -DKONAN_X64=1
    -DKONAN_OBJC_INTEROP
    -DKONAN_CORE_SYMBOLICATION=1
    -DKONAN_HAS_CXX11_EXCEPTION_FUNCTIONS=1
    -DKONAN_REPORT_BACKTRACE_TO_IOS_CRASH_LOG=1
)
