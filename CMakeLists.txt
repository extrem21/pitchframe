cmake_minimum_required(VERSION 3.20)
project(pitchframe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# pitchframe_lib — all parsing and book logic; linked by binary and tests
# ---------------------------------------------------------------------------
add_library(pitchframe_lib STATIC
    src/parser/itch_parser.cpp
    src/book/order_book.cpp
    src/common/alloc_counter.cpp
)
target_include_directories(pitchframe_lib PUBLIC include)
target_compile_options(pitchframe_lib PRIVATE
    -Wall -Wextra -Wshadow -fno-rtti -fno-exceptions
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Debug>:-fsanitize=address -fsanitize=undefined>
)
target_link_options(pitchframe_lib PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address -fsanitize=undefined>
)

# ---------------------------------------------------------------------------
# pitchframe — thin driver executable
# ---------------------------------------------------------------------------
add_executable(pitchframe src/main.cpp)
target_link_libraries(pitchframe PRIVATE pitchframe_lib)
target_compile_options(pitchframe PRIVATE
    -Wall -Wextra -Wshadow -fno-rtti -fno-exceptions
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Debug>:-fsanitize=address -fsanitize=undefined>
)
target_link_options(pitchframe PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address -fsanitize=undefined>
)

# ---------------------------------------------------------------------------
# Heap allocation counter (debug / verification only)
# cmake .. -DPITCHFRAME_COUNT_ALLOCS=ON   — enables global operator new override
# Do NOT enable in the test or bench targets; GTest allocates freely.
# ---------------------------------------------------------------------------
option(PITCHFRAME_COUNT_ALLOCS "Enable heap allocation counter" OFF)
if(PITCHFRAME_COUNT_ALLOCS)
    target_compile_definitions(pitchframe_lib PUBLIC PITCHFRAME_COUNT_ALLOCS)
    target_compile_definitions(pitchframe     PRIVATE PITCHFRAME_COUNT_ALLOCS)
endif()

# ---------------------------------------------------------------------------
# Google Test via FetchContent
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(pitchframe_tests
    tests/parser_test.cpp
    tests/book_test.cpp
)
target_link_libraries(pitchframe_tests PRIVATE pitchframe_lib GTest::gtest_main)
target_compile_options(pitchframe_tests PRIVATE -Wall -Wextra -Wshadow)

include(GoogleTest)
gtest_discover_tests(pitchframe_tests)

# ---------------------------------------------------------------------------
# itch_bench — benchmark stub (Google Benchmark wired up in a later milestone)
# ---------------------------------------------------------------------------
add_executable(itch_bench bench/itch_bench.cpp)
target_link_libraries(itch_bench PRIVATE pitchframe_lib)
target_compile_options(itch_bench PRIVATE
    -Wall -Wextra -Wshadow -fno-rtti -fno-exceptions
    $<$<CONFIG:Release>:-O2>
)
