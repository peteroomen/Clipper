cmake_minimum_required(VERSION 3.16)
project(clipper_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Portable DSP core. Zero platform/OS/browser dependencies. This target must
# build with any standard C++17 compiler (native or Emscripten).
add_library(clipper_core STATIC
    src/Processor.cpp
)
target_include_directories(clipper_core PUBLIC include)

if(NOT MSVC)
    target_compile_options(clipper_core PRIVATE -Wall -Wextra)
endif()

# The C ABI wrapper is a separate object so native builds can exercise it too.
# It only depends on the portable core.
add_library(clipper_c_api STATIC
    src/clipper_c_api.cpp
)
target_link_libraries(clipper_c_api PUBLIC clipper_core)

# --- Tests (native only; skipped under Emscripten) ---------------------------
if(NOT EMSCRIPTEN)
    enable_testing()
    add_executable(clipper_tests tests/test_processor.cpp)
    target_link_libraries(clipper_tests PRIVATE clipper_core)
    add_test(NAME clipper_tests COMMAND clipper_tests)
endif()
