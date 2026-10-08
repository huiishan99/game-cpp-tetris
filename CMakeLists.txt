cmake_minimum_required(VERSION 3.16)

project(CppTetrisGame LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep the MSVC package self-contained without a separate VC++ runtime install.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(TETRIS_CORE_SOURCES
    src/block.cpp
    src/blocks.cpp
    src/game.cpp
    src/grid.cpp
    src/high_score.cpp
    src/leaderboard.cpp
    src/position.cpp
    src/settings.cpp
    src/settings_options.cpp
    src/sound.cpp
)

add_library(tetris_core ${TETRIS_CORE_SOURCES})
target_include_directories(tetris_core PUBLIC src)

if(WIN32)
    target_link_libraries(tetris_core PUBLIC winmm)
    add_executable(tetris WIN32 src/main.cpp)
    set_target_properties(tetris PROPERTIES OUTPUT_NAME "main")
    target_link_libraries(tetris PRIVATE tetris_core gdi32 user32)

    install(TARGETS tetris RUNTIME DESTINATION .)
    install(FILES Font/monogram.ttf DESTINATION Font)
    install(FILES README.md VERSION DEVLOG.md ROADMAP.md DESTINATION .)
    install(DIRECTORY docs/ DESTINATION docs)
else()
    message(STATUS "Win32 GUI target is skipped on this platform; building the portable core only.")
endif()

if(MSVC)
    target_compile_options(tetris_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(tetris_core PRIVATE -Wall -Wextra)
endif()

enable_testing()

add_executable(tetris_core_tests tests/core_tests.cpp)
target_link_libraries(tetris_core_tests PRIVATE tetris_core)
add_test(NAME tetris_core_tests COMMAND tetris_core_tests)

if(WIN32)
    add_executable(tetris_windows_ui_tests tests/windows_ui_tests.cpp)
    target_link_libraries(tetris_windows_ui_tests PRIVATE tetris_core gdi32 user32)
    add_test(NAME tetris_windows_ui_tests
        COMMAND tetris_windows_ui_tests "${CMAKE_BINARY_DIR}/test-evidence/native-ui")
    set_tests_properties(tetris_windows_ui_tests PROPERTIES
        TIMEOUT 90 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

