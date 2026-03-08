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
