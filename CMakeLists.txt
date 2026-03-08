cmake_minimum_required(VERSION 3.20)
project(incrust LANGUAGES CXX)

# std::jthread / std::stop_token require macOS 13+ in Apple's libc++
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS deployment version")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands so clangd/IntelliSense picks up C++20 flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Sources ────────────────────────────────────────────────────────────────────
add_executable(incrust
    src/main.cpp
    src/processor.cpp
)

target_include_directories(incrust PRIVATE src)

# ── Compiler flags ─────────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(incrust PRIVATE /W4 /WX)
else()
    target_compile_options(incrust PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
    )
endif()

# ── Threads ────────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
target_link_libraries(incrust PRIVATE Threads::Threads)

# ── cpp-httplib (header-only static file server) ──────────────────────────────
# Download the single header directly — avoids the FetchContent cmake
# infrastructure, which can disturb the include-path order on macOS/LLVM.
set(HTTPLIB_HEADER "${CMAKE_SOURCE_DIR}/src/vendor/httplib.h")
if(NOT EXISTS "${HTTPLIB_HEADER}")
    message(STATUS "Downloading cpp-httplib single header...")
    file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/src/vendor")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.5/httplib.h"
        "${HTTPLIB_HEADER}"
        STATUS DOWNLOAD_STATUS
    )
    list(GET DOWNLOAD_STATUS 0 DOWNLOAD_RESULT)
    if(NOT DOWNLOAD_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to download httplib.h — check your internet connection.")
    endif()
    message(STATUS "cpp-httplib downloaded to ${HTTPLIB_HEADER}")
endif()
target_include_directories(incrust PRIVATE "${CMAKE_SOURCE_DIR}/src/vendor")
