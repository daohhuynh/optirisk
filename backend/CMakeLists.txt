cmake_minimum_required(VERSION 3.25)
project(optirisk LANGUAGES C CXX)

# ── C++23 Standard ──────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Maximum Performance Flags ───────────────────────────────────────
# Apple Silicon (arm64) doesn't support AVX2/FMA; use native optimizations instead
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -Wall -Wextra -Werror -DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -fsanitize=address,undefined -Wall -Wextra -Werror")
else()
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mavx2 -mfma -Wall -Wextra -Werror -DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -fsanitize=address,undefined -Wall -Wextra -Werror")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── uSockets (vendored, pre-built static library) ──────────────────
set(USOCKETS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/uWebSockets/uSockets)
add_library(uSockets STATIC IMPORTED)
set_target_properties(uSockets PROPERTIES
    IMPORTED_LOCATION ${USOCKETS_DIR}/uSockets.a
)

# ── Sources ─────────────────────────────────────────────────────────
add_executable(optirisk
    src/main.cpp
)

target_include_directories(optirisk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # uWebSockets headers (App.h, WebSocket.h, etc.)
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/uWebSockets/src
    # uSockets public header (libusockets.h)
    ${USOCKETS_DIR}/src
)

target_compile_definitions(optirisk PRIVATE
    # No SSL — raw binary WebSocket for minimal latency
    LIBUS_NO_SSL
)

# ── Threading ───────────────────────────────────────────────────────
find_package(Threads REQUIRED)
target_link_libraries(optirisk PRIVATE
    Threads::Threads
    uSockets
    z   # zlib — required by uWebSockets for permessage-deflate (even if disabled)
)

# ── Tests ───────────────────────────────────────────────────────────
# Standalone test binaries, one per subsystem. These were previously built by
# hand and silently rotted out of sync with the headers (test_cascade_engine
# stopped compiling when run_cascade_tick gained its OptionsBook parameter).
# Building them as part of the default target keeps that from recurring.
option(OPTIRISK_BUILD_TESTS "Build the standalone subsystem test binaries" ON)

if(OPTIRISK_BUILD_TESTS)
    enable_testing()

    set(OPTIRISK_TESTS
        src/concurrency/test_disruptor.cpp
        src/network/test_wire_protocol.cpp
        src/compute/test_simd_engine.cpp
        src/compute/test_cascade_engine.cpp
        src/compute/test_monte_carlo.cpp
        src/market/test_order_book.cpp
    )

    foreach(test_src IN LISTS OPTIRISK_TESTS)
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

# ── Compile Commands (for IDE integration) ──────────────────────────
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
