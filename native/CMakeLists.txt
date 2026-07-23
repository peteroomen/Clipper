cmake_minimum_required(VERSION 3.22)

# Clipper native shell: a JUCE audio plugin (Standalone + VST3 + AU) + a console
# "identical-core" test, all wrapping the IDENTICAL portable C++ DSP core (../core)
# — a re-wrap, not a rewrite. See docs/DEVELOPMENT.md "Native app (JUCE)".
project(clipper_native VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- Build git hash (shown in the editor corner) -----------------------------
# Configured at CMake time; falls back to "dev" outside a git checkout.
set(CLIPPER_GIT_HASH "dev")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _git_hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_git_hash)
        set(CLIPPER_GIT_HASH "${_git_hash}")
    endif()
endif()
message(STATUS "Clipper native build hash: ${CLIPPER_GIT_HASH}")

# --- The portable DSP core (consumed AS-IS; no modifications) -----------------
# Adds the clipper_dsp target (RatModel/SdModel/AmpModel/ChorusModel/Cab*/…). Core
# also builds its own ctest suites; that is harmless here and proves nothing broke.
#
# The VST3/AU wrappers are SHARED modules, so every object linked into them must be
# position-independent. clipper_dsp is a STATIC lib in core/CMakeLists.txt (no -fPIC
# there); rather than edit core, request PIC globally here BEFORE adding it, so the
# core targets are compiled -fPIC for the shared-module link. Additive, core-source
# untouched.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/core)

# --- JUCE (FetchContent, PINNED) ---------------------------------------------
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4        # pinned; a recent JUCE 8 release
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# --- The shared native code (engine + plugin + editor) -----------------------
# juce_add_plugin creates the shared-code target `Clipper` plus the format wrappers
# Clipper_Standalone / Clipper_VST3 / (Clipper_AU on Apple).
set(CLIPPER_FORMATS Standalone VST3)
if(APPLE)
    list(APPEND CLIPPER_FORMATS AU)   # AudioUnit is macOS-only (Logic target)
endif()

juce_add_plugin(Clipper
    PRODUCT_NAME "Clipper"
    COMPANY_NAME "Clipper"
    BUNDLE_ID "com.clipper.native"
    PLUGIN_MANUFACTURER_CODE Clpr
    PLUGIN_CODE Clp1
    FORMATS ${CLIPPER_FORMATS}
    IS_SYNTH FALSE
    # Without this the macOS Standalone gets NO input at all — the OS silently
    # denies the device (any input, muted or not) because the app never asks
    # for mic permission. Injects NSMicrophoneUsageDescription + the runtime
    # permission request. (Field bug: "audio doesn't work at all" on Mac.)
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Clipper needs your audio interface to hear the guitar."
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_STEP FALSE
    VST3_CATEGORIES Fx Distortion
    AU_MAIN_TYPE kAudioUnitType_Effect)

target_sources(Clipper PRIVATE
    src/ClipperEngine.cpp
    src/PluginProcessor.cpp
    src/PluginEditor.cpp)

target_compile_definitions(Clipper PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    CLIPPER_GIT_HASH="${CLIPPER_GIT_HASH}")

target_link_libraries(Clipper PRIVATE
    clipper_dsp
    juce::juce_audio_utils)
target_link_libraries(Clipper PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# --- Identical-core console test ---------------------------------------------
# A JUCE console app so it can instantiate the real ClipperAudioProcessor. It
# compares the plugin's stereo output against a from-scratch chain built from the
# core classes directly (RatModel/SdModel/AmpModel/Cab*/OutputLimiter). Linking
# `Clipper` (shared code) brings ClipperAudioProcessor + all JUCE modules; linking
# clipper_dsp gives the reference chain the same core classes.
juce_add_console_app(clipper_identical_test PRODUCT_NAME clipper_identical_test)
target_sources(clipper_identical_test PRIVATE tests/identical_core_test.cpp)
target_link_libraries(clipper_identical_test PRIVATE
    Clipper
    clipper_dsp
    juce::juce_audio_utils
    juce::juce_recommended_config_flags)
target_include_directories(clipper_identical_test PRIVATE src)

enable_testing()
add_test(NAME clipper_identical_core COMMAND clipper_identical_test)
