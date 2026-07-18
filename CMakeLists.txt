cmake_minimum_required(VERSION 3.20)
project(balatro_core VERSION 1.0.0 LANGUAGES C)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/balatro_ids.h")
    message(FATAL_ERROR "Run tools/generate_content.py before configuring")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(BALATRO_SANITIZE "Enable address and undefined-behavior sanitizers" OFF)
option(BALATRO_BUILD_TESTS "Build native tests" ON)

set(BALATRO_SOURCES
    src/balatro.c
    src/rng.c
    src/poker.c
    src/economy.c
    src/content_generated.c
    src/shop.c
    src/packs.c
    src/scoring.c
)
add_library(balatro_core STATIC ${BALATRO_SOURCES})
target_include_directories(balatro_core PUBLIC include PRIVATE src)
target_compile_options(balatro_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(balatro_core PUBLIC m)

add_library(balatro_shared SHARED ${BALATRO_SOURCES})
set_target_properties(balatro_shared PROPERTIES OUTPUT_NAME balatro)
target_include_directories(balatro_shared PUBLIC include PRIVATE src)
target_compile_options(balatro_shared PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(balatro_shared PUBLIC m)

if(BALATRO_SANITIZE)
    target_compile_options(balatro_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(balatro_core PUBLIC -fsanitize=address,undefined)
    target_compile_options(balatro_shared PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(balatro_shared PUBLIC -fsanitize=address,undefined)
endif()

add_executable(balatro_benchmark tools/benchmark.c)
target_link_libraries(balatro_benchmark PRIVATE balatro_core)

if(BALATRO_BUILD_TESTS)
    enable_testing()
    add_executable(test_core tests/test_core.c)
    # The native suite uses assert() for calls and checks, so keep it enabled
    # in every build configuration.
    if(MSVC)
        target_compile_options(test_core PRIVATE /UNDEBUG)
    else()
        target_compile_options(test_core PRIVATE -UNDEBUG)
    endif()
    target_link_libraries(test_core PRIVATE balatro_core)
    add_test(NAME core COMMAND test_core)
endif()
