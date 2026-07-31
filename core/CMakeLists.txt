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
    src/dsp/OverdriveEngine.cpp
    src/dsp/SdModel.cpp
    src/dsp/TsModel.cpp
    src/dsp/AmpModel.cpp
    src/dsp/ChorusModel.cpp
    src/dsp/PhaserModel.cpp
    src/dsp/WahModel.cpp
    src/dsp/ReverbModel.cpp
    src/dsp/CabConvolver.cpp
    src/dsp/CabIR.cpp
    src/dsp/TriodeStage.cpp
    src/dsp/Jcm800Preamp.cpp
    src/dsp/Jcm800PowerAmp.cpp
    src/dsp/Jcm800Amp.cpp
    src/dsp/OptoTremolo.cpp
    src/dsp/TwinPreamp.cpp
    src/dsp/TwinPowerAmp.cpp
    src/dsp/TwinAmp.cpp
    src/dsp/BjtStage.cpp
    src/dsp/MuffModel.cpp
    src/dsp/GoldModel.cpp
    src/dsp/Ac30Preamp.cpp
    src/dsp/Ac30PowerAmp.cpp
    src/dsp/Ac30Amp.cpp
)
target_include_directories(clipper_dsp PUBLIC include)
target_link_libraries(clipper_dsp PUBLIC chowdsp_wdf)
if(NOT MSVC)
    target_compile_options(clipper_dsp PRIVATE -Wall -Wextra)
endif()

# --- Tests (native only; skipped under Emscripten) ---------------------------
if(NOT EMSCRIPTEN)
    enable_testing()

    # Every test target's warning + assert flags, in ONE place.
    #
    # 2026-07-25 (test/assert-real-properties). This used to be seventeen copies of
    #     if(NOT MSVC)
    #         target_compile_options(<t> PRIVATE -Wall -Wextra -UNDEBUG)
    #     endif()
    # and the `if(NOT MSVC)` was the 2026-07-24 audit's sharpest process finding: these
    # suites are plain `int main` + <cassert>, and `NDEBUG` — which CMake defines in every
    # Release build, the documented build type — erases `assert()` outright. So on MSVC the
    # ENTIRE 1129-line player-expectations suite compiled to a no-op `main` that printed
    # "All M11 player-expectations tests passed" and exited 0, with zero golden WAVs
    # present. Not a weaker test: no test at all, reported green.
    #
    # MSVC accepts `/UNDEBUG` (undefine a preprocessor symbol) exactly as GCC/Clang accept
    # `-UNDEBUG`, and it lands after the config's `/DNDEBUG` on the command line, so there
    # was never a reason for the guard. Asserts are now live on every platform, and
    # core/tests/support/AssertsLive.h makes it a BUILD ERROR if this ever stops working.
    #
    # `tests` on the include path is what lets a test say #include "support/Xfail.h".
    function(clipper_add_test_flags target)
        if(MSVC)
            target_compile_options(${target} PRIVATE /W3 /UNDEBUG)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -UNDEBUG)
        endif()
        target_include_directories(${target} PRIVATE tests)
    endfunction()

    # Register a companion ctest entry that lists a binary's XFAILed known defects.
    #
    # The binary prints them during the real run, but `ctest --output-on-failure` shows
    # nothing for a passing test — and a known defect nobody can see is how the audit's
    # findings survived a green suite in the first place. So: run the binary with
    # `--xfail-ledger`, which prints the ledger and exits 77, and tell ctest that 77 means
    # skipped. A plain `ctest` now prints, in its DEFAULT summary:
    #
    #     Test #NN: clipper_twin_tests_xfail_ledger ............ ***Skipped
    #
    # so every open defect under XFAIL is visible without any extra flag, named after the
    # target that measures it. An XPASS (the defect got fixed, the XFAIL did not get
    # deleted) fails the REAL test — see core/tests/support/Xfail.h.
    function(clipper_add_xfail_ledger target)
        add_test(NAME ${target}_xfail_ledger COMMAND ${target} --xfail-ledger)
        set_tests_properties(${target}_xfail_ledger PROPERTIES SKIP_RETURN_CODE 77)
    endfunction()
    add_executable(clipper_tests tests/test_processor.cpp)
    target_link_libraries(clipper_tests PRIVATE clipper_core)
    clipper_add_test_flags(clipper_tests)
    add_test(NAME clipper_tests COMMAND clipper_tests)

    add_executable(clipper_rat_tests tests/test_rat_model.cpp)
    target_link_libraries(clipper_rat_tests PRIVATE clipper_dsp)
    # Shared measurement infra (aliasing metric) lives under tools/measure and is
    # included by both the tests and the render CLI. Never part of the DSP core.
    target_include_directories(clipper_rat_tests PRIVATE tools)
    clipper_add_test_flags(clipper_rat_tests)
    add_test(NAME clipper_rat_tests COMMAND clipper_rat_tests)

    # --- M8: SD-1 model tests -------------------------------------------------
    add_executable(clipper_sd_tests tests/test_sd_model.cpp)
    target_link_libraries(clipper_sd_tests PRIVATE clipper_dsp)
    # Shares the tools/measure aliasing metric with the RAT tests.
    target_include_directories(clipper_sd_tests PRIVATE tools)
    clipper_add_test_flags(clipper_sd_tests)
    add_test(NAME clipper_sd_tests COMMAND clipper_sd_tests)

    # --- v1.1: TS808-style "Screamer" model tests -----------------------------
    add_executable(clipper_ts_tests tests/test_ts_model.cpp)
    target_link_libraries(clipper_ts_tests PRIVATE clipper_dsp)
    # Shares the tools/measure aliasing metric with the RAT/SD-1 tests.
    target_include_directories(clipper_ts_tests PRIVATE tools)
    clipper_add_test_flags(clipper_ts_tests)
    add_test(NAME clipper_ts_tests COMMAND clipper_ts_tests)
    # --- v1.1: phaser ("Ninety") model tests ----------------------------------
    add_executable(clipper_phaser_tests tests/test_phaser_model.cpp)
    target_link_libraries(clipper_phaser_tests PRIVATE clipper_dsp)
    clipper_add_test_flags(clipper_phaser_tests)
    add_test(NAME clipper_phaser_tests COMMAND clipper_phaser_tests)

    # --- The FILTER pedal: wah / envelope filter ("Weeper") tests -------------
    add_executable(clipper_wah_tests tests/test_wah_model.cpp)
    target_link_libraries(clipper_wah_tests PRIVATE clipper_dsp)
    clipper_add_test_flags(clipper_wah_tests)
    add_test(NAME clipper_wah_tests COMMAND clipper_wah_tests)

    # --- M5: amp + cab tests --------------------------------------------------
    add_executable(clipper_amp_tests tests/test_amp_model.cpp)
    target_link_libraries(clipper_amp_tests PRIVATE clipper_dsp)
    clipper_add_test_flags(clipper_amp_tests)
    add_test(NAME clipper_amp_tests COMMAND clipper_amp_tests)

    # --- M9.1: 12AX7 triode stage tests ---------------------------------------
    add_executable(clipper_triode_tests tests/test_triode_stage.cpp)
    target_link_libraries(clipper_triode_tests PRIVATE clipper_dsp)
    # Shared measurement infra (aliasing metric) under tools/measure.
    target_include_directories(clipper_triode_tests PRIVATE tools)
    clipper_add_test_flags(clipper_triode_tests)
    add_test(NAME clipper_triode_tests COMMAND clipper_triode_tests)

    # --- M9.2: JCM800 2204 preamp tests ---------------------------------------
    add_executable(clipper_jcm800_tests tests/test_jcm800_preamp.cpp)
    target_link_libraries(clipper_jcm800_tests PRIVATE clipper_dsp)
    # Shared measurement infra (aliasing metric) under tools/measure.
    target_include_directories(clipper_jcm800_tests PRIVATE tools)
    clipper_add_test_flags(clipper_jcm800_tests)
    add_test(NAME clipper_jcm800_tests COMMAND clipper_jcm800_tests)

    # --- M9.3: JCM800 2204 power section + full amp tests ----------------------
    add_executable(clipper_jcm800_power_tests tests/test_jcm800_power.cpp)
    target_link_libraries(clipper_jcm800_power_tests PRIVATE clipper_dsp)
    # Shared measurement infra (aliasing metric) under tools/measure.
    target_include_directories(clipper_jcm800_power_tests PRIVATE tools)
    clipper_add_test_flags(clipper_jcm800_power_tests)
    add_test(NAME clipper_jcm800_power_tests COMMAND clipper_jcm800_power_tests)
    # XFAILs: audit findings 7 + 8 (starved PI, Ra2 imbalance, no even-harmonic cancel).
    clipper_add_xfail_ledger(clipper_jcm800_power_tests)

    # --- M10.1: Twin (Fender blackface) amp tests -----------------------------
    add_executable(clipper_twin_tests tests/test_twin_amp.cpp)
    target_link_libraries(clipper_twin_tests PRIVATE clipper_dsp)
    # Shared measurement infra (aliasing metric) under tools/measure.
    target_include_directories(clipper_twin_tests PRIVATE tools)
    clipper_add_test_flags(clipper_twin_tests)
    add_test(NAME clipper_twin_tests COMMAND clipper_twin_tests)
    # NO XFAILs since 2026-07-29 (finding 7, docs §42): the Twin PI's plate-fraction and
    # standing-current XFAILs XPASSed once the tail got a reference and were deleted, so
    # there is no ledger entry to register. `clipper_twin_tests --xfail-ledger` still
    # answers, with a count of zero.

    # --- OptoTremolo (AB763 optical tremolo) ----------------------------------
    # New 2026-07-25 (test/assert-real-properties). The audit found OptoTremolo had no
    # test of its own and was unreachable through the composed TwinAmp in any render
    # (every one passes INTENSITY 0 / chorusMode 0), so its measured SPEED-dependent
    # depth collapse and ~5 dB level sag were invisible. This target pins the depth and
    # mean level ACROSS the SPEED range and drives the effect through TwinAmp itself.
    add_executable(clipper_opto_tremolo_tests tests/test_opto_tremolo.cpp)
    target_link_libraries(clipper_opto_tremolo_tests PRIVATE clipper_dsp)
    clipper_add_test_flags(clipper_opto_tremolo_tests)
    add_test(NAME clipper_opto_tremolo_tests COMMAND clipper_opto_tremolo_tests)
    # XFAILs: the SPEED-dependent depth collapse + level sag (audit Medium/DSP).
    clipper_add_xfail_ledger(clipper_opto_tremolo_tests)

    # --- v1.1 item 4: Muff fuzz + reusable BjtStage (Ebers-Moll BJT) tests -----
    add_executable(clipper_muff_tests tests/test_muff_model.cpp)
    target_link_libraries(clipper_muff_tests PRIVATE clipper_dsp)
    # Shares the tools/measure aliasing metric with the RAT/SD-1/TS tests.
    target_include_directories(clipper_muff_tests PRIVATE tools)
    clipper_add_test_flags(clipper_muff_tests)
    add_test(NAME clipper_muff_tests COMMAND clipper_muff_tests)
    # No clipper_add_xfail_ledger: the Muff suite has ZERO known-bad properties as of
    # docs §53 (the DC-blocked diode branch made finding 16's bass half and the ±20 V
    # slam cap both XPASS, and both are asserted outright now). Registering a ledger
    # with an empty decl list would report Passed instead of ***Skipped.

    # --- v1.1 item 6: GOLD overdrive (clean-blend + germanium WDF) tests -------
    add_executable(clipper_gold_tests tests/test_gold_model.cpp)
    target_link_libraries(clipper_gold_tests PRIVATE clipper_dsp)
    # Shares the tools/measure aliasing metric with the RAT/SD-1/TS/Muff tests.
    target_include_directories(clipper_gold_tests PRIVATE tools)
    clipper_add_test_flags(clipper_gold_tests)
    add_test(NAME clipper_gold_tests COMMAND clipper_gold_tests)
    # No XFAIL ledger: §52's two entries (gold-summing-rails-engage and its shadow
    # gold-summing-alias-at-treble-max) both XPASSed when §54 fixed the diode node —
    # exactly the slice they named — and are now hard assertions in the suite.

    # --- M10.2: AC30 (Vox top boost) amp tests --------------------------------
    add_executable(clipper_ac30_tests tests/test_ac30_amp.cpp)
    target_link_libraries(clipper_ac30_tests PRIVATE clipper_dsp)
    # Shared measurement infra (aliasing metric) under tools/measure.
    target_include_directories(clipper_ac30_tests PRIVATE tools)
    clipper_add_test_flags(clipper_ac30_tests)
    add_test(NAME clipper_ac30_tests COMMAND clipper_ac30_tests)
    # XFAILs. History: the 2026-07-29 finding-7 leg-balance entry and the five voicing
    # entries it created were all XPASSed and deleted by the §46 gain-structure slice, and
    # this registration came off with them. It is BACK as of 2026-07-31 (docs §55) with
    # exactly one entry — `finding4-ac30-bias-swing-short`. Retiring the static sag
    # saturator made the shared cathode's bias swing measurable against a published real-amp
    # number for the first time, and the model reaches +8.7 % where a real AC30 measures
    # +25 %. The cathode network is already the published 50 Ω ∥ 250 µF; the shortfall is in
    # how much extra AVERAGE current the EL84 fits draw under drive, which is findings 9/10.
    clipper_add_xfail_ledger(clipper_ac30_tests)

    # --- M11: Player Expectations Suite ---------------------------------------
    # Tests the PLAYER'S expectations, not the circuit (docs §26): universal gear
    # invariants (min-knob usability, hum torture, knob monotonicity, level
    # sanity), live-convention processing (in-place 128-frame blocks — the way
    # the worklet actually calls the C ABI), and golden "first five minutes" rig
    # renders gated per third-octave band (goldens under tests/goldens/;
    # regenerate ONLY via --update-goldens / scripts/update-goldens.sh).
    # Links clipper_c_api because block B drives the exact worklet entry points.
    add_executable(clipper_player_expectations_tests tests/test_player_expectations.cpp)
    target_link_libraries(clipper_player_expectations_tests PRIVATE clipper_c_api)
    # dr_wav (golden WAV IO) + shared measurement infra live under tools/, never
    # in the DSP core.
    target_include_directories(clipper_player_expectations_tests PRIVATE tools tools/third_party)
    target_compile_definitions(clipper_player_expectations_tests PRIVATE
        CLIPPER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/goldens")
    clipper_add_test_flags(clipper_player_expectations_tests)
    add_test(NAME clipper_player_expectations_tests COMMAND clipper_player_expectations_tests)
    # XFAILs: control-rate parameter sampling makes the five dirt pedals' output depend on
    # the host block size during the initial smoother snap (audit Medium/DSP). Found by
    # replacing block B's vacuous 128-only comparison with a ragged block size.
    clipper_add_xfail_ledger(clipper_player_expectations_tests)

    # --- Audit finding 1: non-finite parameter guard + recovery ---------------
    # Injects NaN / +Inf / -Inf into EVERY parameter of EVERY unit (six pedals,
    # four amp voices) over BOTH the C ABI (the worklet's path) and the direct C++
    # setParameter (the JUCE plugin's path), and asserts: the ABI rejects the write
    # bit-exactly, the models never let a non-finite value into recursive state, a
    # subsequent good write genuinely restores the reference render, and reset()
    # recovers a unit poisoned by a NaN AUDIO sample. Links clipper_c_api because
    # half the suite drives the exact worklet entry points.
    #   -DCLIPPER_NAN_TEST_NO_RESET compiles the guard blocks only (used to verify
    #   the suite fails against a checkout that predates the reset() API).
    add_executable(clipper_nan_guard_tests tests/test_nan_guard.cpp)
    target_link_libraries(clipper_nan_guard_tests PRIVATE clipper_c_api)
    clipper_add_test_flags(clipper_nan_guard_tests)
    add_test(NAME clipper_nan_guard_tests COMMAND clipper_nan_guard_tests)

    # --- Perf diagnosis: anti-denormal guard tests ----------------------------
    # Block 1 proves Biquad / OnePoleSmoother never leave recursive state in the
    # subnormal float range on a decaying tail (the amp-independent WASM crackle —
    # see Denormal.h and docs §25).
    #
    # Block 2 covers audit finding 11 (docs §33): the recursive states the policy had
    # been written for but never applied to — the pedals' output DC blockers, the
    # chowdsp WDF capacitor, the three valve tone stacks, the three power sections'
    # OT / presence / TOP CUT / feedback states, TwinPreamp's bright bleed, and
    # Processor's hand-rolled gain smoother. Block 2 needs clipper_core as well (for
    # clipper::Processor) and drives real chowdsp WDF networks directly, so that
    # block's "guarded vs unguarded" comparison uses the actual library rather than a
    # reimplementation of it.
    add_executable(clipper_denormal_tests tests/test_denormal.cpp)
    target_link_libraries(clipper_denormal_tests PRIVATE clipper_dsp clipper_core)
    clipper_add_test_flags(clipper_denormal_tests)
    add_test(NAME clipper_denormal_tests COMMAND clipper_denormal_tests)

    # --- Solver-perf pass: tube-solver early-exit regression (docs §25) -------
    # Renders a riff through each valve amp voice at production solver tolerances
    # and at 1000x-tighter reference tolerances; the two must agree below
    # -120 dBFS. Pins the accuracy contract behind every solver speed shortcut.
    add_executable(clipper_tube_solver_tests tests/test_tube_solver.cpp)
    target_link_libraries(clipper_tube_solver_tests PRIVATE clipper_dsp)
    clipper_add_test_flags(clipper_tube_solver_tests)
    add_test(NAME clipper_tube_solver_tests COMMAND clipper_tube_solver_tests)

    # --- Audit finding 2: cab swap real-time safety ---------------------------
    # A cab change used to run 11-46 ms of IR synthesis, heap copying and FFT
    # partitioning inside process(), against a 2.667 ms deadline. The C ABI now
    # double-buffers the per-side convolver pair: amp_prepare_cab_* builds the new
    # cab in the inactive pair (off the render path) and amp_commit_cab flips an
    # int. This suite asserts (A) the commit performs exactly ZERO allocations —
    # counted with a REPLACED GLOBAL operator new, not inferred from a clock — and
    # costs less than one render block, while the prepare still allocates (proof the
    # work moved rather than vanished); and (B) the swapped-in cab is BIT-IDENTICAL
    # to the pre-fix construction across a scripted session including a swap-back.
    # Links clipper_c_api because block A drives the exact worklet entry points.
    #   -DCLIPPER_CAB_TEST_LEGACY_ABI=1 routes the commit through the old monolithic
    #   amp_set_cab_builtin, which is how "this test fails before the fix" was
    #   verified: block A fails, block B still passes (B is the fidelity guard).
    add_executable(clipper_cab_swap_tests tests/test_cab_swap.cpp)
    target_link_libraries(clipper_cab_swap_tests PRIVATE clipper_c_api)
    if(NOT MSVC)
        # -UNDEBUG keeps assert() live in the Release build.
        target_compile_options(clipper_cab_swap_tests PRIVATE -Wall -Wextra -UNDEBUG)
    endif()
    add_test(NAME clipper_cab_swap_tests COMMAND clipper_cab_swap_tests)

    # --- Audit finding 6: valve-amp parameter smoothing (docs §35) -------------
    # The three valve amps assigned knob values straight to state and applied the
    # resulting scale ONCE PER BLOCK, behind up to 76 dB of gain — a measured 68.9x to
    # 121.9x the signal's own steady slew on their gain-like controls, against 1.37x for
    # the already-smoothed clean amp. This target measures the step/steady-slew ratio for
    # EVERY knob on all three voices with the knob moved MID-RENDER (worst of eight step
    # positions, because a step at an output zero crossing produces none), drives the
    # clean amp through the identical harness as the control case, proves the bar can
    # FAIL with an unsmoothed hard splice, and pins the bit-identity of a static render.
    # The metric itself lives in tests/support/StepSlew.h, shared with the definition
    # native/tests/chain_edit_test.cpp uses.
    add_executable(clipper_param_smoothing_tests tests/test_param_smoothing.cpp)
    target_link_libraries(clipper_param_smoothing_tests PRIVATE clipper_dsp)
    clipper_add_test_flags(clipper_param_smoothing_tests)
    add_test(NAME clipper_param_smoothing_tests COMMAND clipper_param_smoothing_tests)

    # --- perf/halfband-no-modulo: halfband resampler tests (docs §32) ---------
    # The doubled-ring rewrite of HalfbandFilter.h deleted an integer division per
    # tap from the hottest loop in the project (audit perf item 2). A perf change to
    # the resampler is only acceptable if it is BIT-IDENTICAL, so this target carries
    # a verbatim copy of the pre-change (modulo-indexed) implementation and compares
    # the shipped classes against it sample-for-sample — plus the stopband (worst-case
    # image / alias rejection across the audio band, measured through the live
    # objects) and the round-trip group delay measured from an impulse, so a wrong
    # filter or a wrong latency cannot hide behind the identity check.
    add_executable(clipper_halfband_tests tests/test_halfband.cpp)
    target_link_libraries(clipper_halfband_tests PRIVATE clipper_dsp)
    # Shared measurement infra (Goertzel) under tools/measure.
    target_include_directories(clipper_halfband_tests PRIVATE tools)
    clipper_add_test_flags(clipper_halfband_tests)
    add_test(NAME clipper_halfband_tests COMMAND clipper_halfband_tests)

    # --- Per-unit CPU cost benchmark (native only; docs §25) ------------------
    add_executable(clipper-bench tools/bench/main.cpp)
    target_link_libraries(clipper-bench PRIVATE clipper_dsp)
    if(NOT MSVC)
        target_compile_options(clipper-bench PRIVATE -Wall -Wextra)
    endif()

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
