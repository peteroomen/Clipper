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
# It exports both the M0 gain core and (since M3) the RAT model, so it links
# clipper_dsp as well. clipper_dsp is defined further down; CMake resolves the
# target name at generation time regardless of declaration order.
add_library(clipper_c_api STATIC
    src/clipper_c_api.cpp
)
target_link_libraries(clipper_c_api PUBLIC clipper_core clipper_dsp)

# --- M1: RAT diode-clipper model ---------------------------------------------
# The pedal model is NEW code beside the M0 gain core (which the WASM pipeline
# still uses unchanged). It depends on chowdsp_wdf for the WDF diode stage.
#
# chowdsp_wdf is header-only, pulled via FetchContent and PINNED to release
# v1.0.0 (commit 36b5775555af21f0f417d2bc866ba7b4b2788614). FetchContent caches
# the checkout under build/_deps, so a re-configure of an already-populated
# build does not re-download. SYSTEM marks its headers as system includes so
# they do not trip our -Wall -Wextra. The library's own tests/benchmarks are
# force-disabled (they are off by default when not top-level, but be explicit).
include(FetchContent)
# chowdsp_wdf v1.0.0 declares an old cmake_minimum_required that CMake 4.x
# refuses outright. This variable (introduced in CMake 4, silently ignored by
# older CMakes) tells CMake to configure such subprojects as if they required
# at least 3.5. Remove once the pin moves to a release that raises its minimum.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CHOWDSP_WDF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CHOWDSP_WDF_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(chowdsp_wdf
    GIT_REPOSITORY https://github.com/Chowdhury-DSP/chowdsp_wdf.git
    GIT_TAG        36b5775555af21f0f417d2bc866ba7b4b2788614  # v1.0.0
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(chowdsp_wdf)

# Portable DSP models. Still platform-free C++17 (no OS/browser/Emscripten deps);
# chowdsp_wdf is likewise header-only and portable.
#   - RatModel.cpp     : M1/M2 RAT pedal (WDF diode clipper, oversampled)
#   - AmpModel.cpp     : M5 JC-120-style clean amp (linear tone stack)
#   - ReverbModel.cpp  : M6.7 spring-flavored algorithmic reverb (owned by AmpModel)
#   - CabConvolver.cpp : M5 partitioned FFT cab convolver
#   - CabIR.cpp        : M5 procedural default 2x12 cab impulse response
#   - TriodeStage.cpp  : M9.1 12AX7 common-cathode gain stage (Koren + nodal Newton)
#   - Jcm800Preamp.cpp : M9.2 JCM800 2204 preamp (4x TriodeStage + TMB tone stack)
#   - Jcm800PowerAmp.cpp: M9.3 JCM800 2204 power section (LTP PI + push-pull EL34 +
#                        output transformer + NFB/presence + B+ sag)
#   - Jcm800Amp.cpp    : M9.3 composed full 2204 (preamp -> power section)
add_library(clipper_dsp STATIC
    src/dsp/RatModel.cpp
    src/dsp/SdModel.cpp
    src/dsp/AmpModel.cpp
    src/dsp/ChorusModel.cpp
    src/dsp/ReverbModel.cpp
    src/dsp/CabConvolver.cpp
    src/dsp/CabIR.cpp
    src/dsp/TriodeStage.cpp
    src/dsp/Jcm800Preamp.cpp
    src/dsp/Jcm800PowerAmp.cpp
    src/dsp/Jcm800Amp.cpp
)
target_include_directories(clipper_dsp PUBLIC include)
target_link_libraries(clipper_dsp PUBLIC chowdsp_wdf)
if(NOT MSVC)
    target_compile_options(clipper_dsp PRIVATE -Wall -Wextra)
endif()

# --- Tests (native only; skipped under Emscripten) ---------------------------
if(NOT EMSCRIPTEN)
    enable_testing()
    add_executable(clipper_tests tests/test_processor.cpp)
    target_link_libraries(clipper_tests PRIVATE clipper_core)
    if(NOT MSVC)
        # -UNDEBUG keeps assert() live even in a Release build (the documented
        # build type), so these tests actually check something.
        target_compile_options(clipper_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME clipper_tests COMMAND clipper_tests)

    add_executable(clipper_rat_tests tests/test_rat_model.cpp)
    target_link_libraries(clipper_rat_tests PRIVATE clipper_dsp)
    # Shared measurement infra (aliasing metric) lives under tools/measure and is
    # included by both the tests and the render CLI. Never part of the DSP core.
    target_include_directories(clipper_rat_tests PRIVATE tools)
    if(NOT MSVC)
        # -UNDEBUG keeps assert() live even in a Release build (the documented
        # build type), so these tests actually check something.
        target_compile_options(clipper_rat_tests PRIVATE -Wall -Wextra -UNDEBUG)
    endif()
    add_test(NAME clipper_rat_tests COMMAND clipper_rat_tests)

    # --- M8: SD-1 model tests -------------------------------------------------
    add_executable(clipper_sd_tests tests/test_sd_model.cpp)
    target_link_libraries(clipper_sd_tests PRIVATE clipper_dsp)
    # Shares the tools/measure aliasing metric with the RAT tests.
    target_include_directories(clipper_sd_tests PRIVATE tools)
    if(NOT MSVC)
        target_compile_options(clipper_sd_tests PRIVATE -Wall -Wextra -UNDEBUG)
    endif()
    add_test(NAME clipper_sd_tests COMMAND clipper_sd_tests)

    # --- M5: amp + cab tests --------------------------------------------------
    add_executable(clipper_amp_tests tests/test_amp_model.cpp)
    target_link_libraries(clipper_amp_tests PRIVATE clipper_dsp)
    if(NOT MSVC)
        # -UNDEBUG keeps assert() live in the Release build.
        target_compile_options(clipper_amp_tests PRIVATE -Wall -Wextra -UNDEBUG)
    endif()
    add_test(NAME clipper_amp_tests COMMAND clipper_amp_tests)

    # --- M9.1: 12AX7 triode stage tests ---------------------------------------
    add_executable(clipper_triode_tests tests/test_triode_stage.cpp)
    target_link_libraries(clipper_triode_tests PRIVATE clipper_dsp)
    # Shared measurement infra (aliasing metric) under tools/measure.
    target_include_directories(clipper_triode_tests PRIVATE tools)
    if(NOT MSVC)
        # -UNDEBUG keeps assert() live in the Release build.
        target_compile_options(clipper_triode_tests PRIVATE -Wall -Wextra -UNDEBUG)
    endif()
    add_test(NAME clipper_triode_tests COMMAND clipper_triode_tests)

    # --- M9.2: JCM800 2204 preamp tests ---------------------------------------
    add_executable(clipper_jcm800_tests tests/test_jcm800_preamp.cpp)
    target_link_libraries(clipper_jcm800_tests PRIVATE clipper_dsp)
    # Shared measurement infra (aliasing metric) under tools/measure.
    target_include_directories(clipper_jcm800_tests PRIVATE tools)
    if(NOT MSVC)
        # -UNDEBUG keeps assert() live in the Release build.
        target_compile_options(clipper_jcm800_tests PRIVATE -Wall -Wextra -UNDEBUG)
    endif()
    add_test(NAME clipper_jcm800_tests COMMAND clipper_jcm800_tests)

    # --- M9.3: JCM800 2204 power section + full amp tests ----------------------
    add_executable(clipper_jcm800_power_tests tests/test_jcm800_power.cpp)
    target_link_libraries(clipper_jcm800_power_tests PRIVATE clipper_dsp)
    # Shared measurement infra (aliasing metric) under tools/measure.
    target_include_directories(clipper_jcm800_power_tests PRIVATE tools)
    if(NOT MSVC)
        # -UNDEBUG keeps assert() live in the Release build.
        target_compile_options(clipper_jcm800_power_tests PRIVATE -Wall -Wextra -UNDEBUG)
    endif()
    add_test(NAME clipper_jcm800_power_tests COMMAND clipper_jcm800_power_tests)

    # --- CLI render harness (native only, NOT part of the WASM build) ---------
    add_executable(clipper-render tools/render/main.cpp)
    target_link_libraries(clipper-render PRIVATE clipper_dsp)
    # dr_wav lives here and is included ONLY by the tool, never by the DSP core.
    # tools/ also carries the shared measurement infra (measure/AliasMetric.h).
    target_include_directories(clipper-render PRIVATE tools/third_party tools)
    if(NOT MSVC)
        target_compile_options(clipper-render PRIVATE -Wall -Wextra)
    endif()
endif()
