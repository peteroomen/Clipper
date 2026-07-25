// Clipper — denormal cost microbenchmark (perf diagnosis, field-lag report).
//
// Reproduces the amp-independent "random" lag the field report describes on the
// CLEAN (JC-120) path. When a signal DECAYS TO SILENCE, the recursive float state
// of a Biquad (z1/z2) or a OnePoleSmoother (value) rings down through the SUBNORMAL
// float range (magnitudes below ~1.18e-38). WASM has NO hardware flush-to-zero, and
// native builds do not enable FTZ/DAZ by default, so every subnormal multiply/add
// takes the CPU's microcoded slow path (10-100x). That is periodic crackle that
// looks random and is independent of which amp is loaded (every amp's tone stack
// uses Biquad; every param uses OnePoleSmoother).
//
// Each scenario is timed in the DEFAULT FP environment and (on x86) with hardware
// FTZ+DAZ forced on. FTZ is the "if the CPU flushed" ceiling; the ratio default/FTZ
// is the denormal cliff. The in-code guard (Denormal.h) is what makes the DEFAULT
// column reach the FTZ ceiling WITHOUT any hardware FTZ — the only fix available on
// WASM. Run this on the pre-guard and post-guard code to see it.
//
// SECTION 1 (A-C) exercises the two shared PRIMITIVES, Biquad and OnePoleSmoother.
// SECTION 2 (D-...) drives the REAL GEAR through the C++ models.
//
// Section 2 exists because section 1 on its own is MISLEADING, and that is what let
// audit finding 11 survive a "clean" tool: Biquad and OnePoleSmoother were guarded in
// the very slice that added this benchmark, so section 1 has reported a ~1.00x cliff
// ever since — while a RAT sat at 1.94x and GOLD pushed 392342 subnormal samples per
// 10 s of silence into whatever came after it in the chain. A microbenchmark that only
// covers the code you already fixed measures your fix, not your product. Section 2
// drives whole units the way the worklet does (128-frame blocks, 48 kHz) and reports
// SIGNAL vs SILENCE, because the denormal cliff is by definition a silence-only cost:
// a unit that is 2x slower on silence than on a full-scale riff is a unit whose
// recursive state is parked in the subnormal range.
//
// Build (native, no fast-math so subnormals behave like the shipped WASM code).
// Section 2 needs the DSP models, so link the core's static lib:
//   cmake -S core -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
//   c++ -std=c++17 -O2 -I core/include scripts/denormal_bench.cpp \
//       build/libclipper_dsp.a -o build/denormal_bench
// Run: build/denormal_bench

#include "clipper/Processor.h"
#include "clipper/dsp/Ac30Amp.h"
#include "clipper/dsp/Biquad.h"
#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/GoldModel.h"
#include "clipper/dsp/Jcm800Amp.h"
#include "clipper/dsp/MuffModel.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/OverdriveEngine.h"
#include "clipper/dsp/RatModel.h"
#include "clipper/dsp/SdModel.h"
#include "clipper/dsp/TsModel.h"
#include "clipper/dsp/TwinAmp.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <pmmintrin.h>
#include <xmmintrin.h>
#define HAVE_X86_FTZ 1
#endif

using clipper::dsp::Biquad;
using clipper::dsp::OnePoleSmoother;
using namespace clipper::dsp::rbj;

namespace {

constexpr double kFs = 48000.0;

void setFtz(bool on) {
#ifdef HAVE_X86_FTZ
    _MM_SET_FLUSH_ZERO_MODE(on ? _MM_FLUSH_ZERO_ON : _MM_FLUSH_ZERO_OFF);
    _MM_SET_DENORMALS_ZERO_MODE(on ? _MM_DENORMALS_ZERO_ON : _MM_DENORMALS_ZERO_OFF);
#else
    (void)on;
#endif
}

double now() {
    return std::chrono::duration<double>(
               std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
}

// --- Scenario A: the AmpModel tone stack (4 biquads) over repeated note decays.
// Realistic clean-path load: each "note" excites the stack, then a silent tail
// where the states ring down through the subnormal band.
std::pair<double, double> scenarioTone() {
    constexpr int notes = 400, attack = 1200, tail = 8000;
    Biquad bass, mid, treble, bright;
    bass.setCoeffs(lowShelf(100.0, 3.0, 0.8, kFs));
    mid.setCoeffs(peaking(650.0, -2.0, 0.7, kFs));
    treble.setCoeffs(highShelf(3500.0, 4.0, 0.8, kFs));
    bright.setCoeffs(highShelf(3000.0, 5.0, 0.9, kFs));
    volatile float sink = 0.0f;
    double sum = 0.0;
    const double t0 = now();
    for (int nn = 0; nn < notes; ++nn) {
        for (int i = 0; i < attack; ++i) {
            const float env = std::exp(-3.0f * i / attack);
            const float x = env * std::sin(2.0 * M_PI * 196.0 * i / kFs);
            sink = bright.process(treble.process(mid.process(bass.process(x))));
            sum += sink;
        }
        for (int i = 0; i < tail; ++i) {
            sink = bright.process(treble.process(mid.process(bass.process(0.0f))));
            sum += sink;
        }
    }
    return {now() - t0, sum};
}

// --- Scenario B: a HIGH-Q resonator excited once, then a long silent tail. Poles
// near the unit circle decay slowly, so the state DWELLS in the subnormal band for
// thousands of samples — the sustained cliff (this is the reverb-tail / ringing-EQ
// shape; the reverb already carries its own 1e-20 guard, this isolates the biquad).
std::pair<double, double> scenarioResonator() {
    constexpr int reps = 300, tail = 40000;
    Biquad r;
    r.setCoeffs(peaking(180.0, 18.0, 12.0, kFs));  // sharp, high-Q -> long ring
    volatile float sink = 0.0f;
    double sum = 0.0;
    const double t0 = now();
    for (int k = 0; k < reps; ++k) {
        sink = r.process(1.0f);  // unit impulse
        sum += sink;
        for (int i = 0; i < tail; ++i) {
            sink = r.process(0.0f);
            sum += sink;
        }
    }
    return {now() - t0, sum};
}

// --- Scenario C: a OnePoleSmoother whose target is set to 0 with a nonzero value,
// then advanced for a long time. WITHOUT the guard the value asymptotes toward 0
// and STICKS in the subnormal range, emitting a subnormal multiply EVERY sample
// forever (a bright switch toggled off, then keep playing). WITH the guard it snaps
// to exactly 0. The starkest sustained case.
std::pair<double, double> scenarioSmoother() {
    constexpr int reps = 40, run = 300000;
    OnePoleSmoother sm;
    sm.prepare(0.008, kFs);
    volatile float sink = 0.0f;
    double sum = 0.0;
    const double t0 = now();
    for (int k = 0; k < reps; ++k) {
        sm.setImmediate(1.0f);
        sm.setTarget(0.0f);  // decay toward zero -> subnormal sink without the guard
        for (int i = 0; i < run; ++i) {
            sink = sm.next();
            sum += sink;
        }
    }
    return {now() - t0, sum};
}

void report(const char* name, long samples,
            std::pair<double, double> (*fn)()) {
    const double realtime = samples / kFs;
    setFtz(false);
    auto d = fn();
#ifdef HAVE_X86_FTZ
    setFtz(true);
    auto f = fn();
    setFtz(false);
    std::printf("  %-26s default %8.2f ms (%6.0fx RT) | FTZ %8.2f ms (%6.0fx RT) | cliff %.1fx  chk %.4g\n",
                name, d.first * 1e3, realtime / d.first, f.first * 1e3,
                realtime / f.first, d.first / f.first, d.second);
#else
    std::printf("  %-26s default %8.2f ms (%6.0fx RT)  chk %.4g\n",
                name, d.first * 1e3, realtime / d.first, d.second);
#endif
}

// ===========================================================================
// SECTION 2 — whole units, SIGNAL vs SILENCE (audit finding 11)
// ===========================================================================
// The shape of the measurement: prepare the unit, play kSeconds of a realistic
// full-scale riff (timed), then feed kSeconds of EXACT DIGITAL SILENCE (timed).
// Identical instruction count, identical block structure, identical everything —
// the only difference is the magnitude of the numbers flowing through the recursive
// state. So `silence / signal` is a pure denormal ratio: 1.0 means the state is
// flushed, 2.0 means every silent sample is paying the microcode penalty.
//
// The silence pass also COUNTS the unit's own subnormal output samples. That is the
// part a downstream stage inherits: a pedal emitting subnormal floats makes the amp
// and cab after it slow too, so this number is a chain-wide cost, not a local one.

constexpr int kBlock = 128;               // the worklet's block size
constexpr int kSeconds = 10;
constexpr int kFrames = static_cast<int>(kFs) * kSeconds;

// A plucked-note program at realistic guitar level: repeated decaying notes, so the
// state is genuinely exercised (attack + decay), not just held at DC.
void fillRiff(std::vector<float>& v) {
    constexpr double kNoteHz[] = {82.41, 110.0, 146.83, 196.0, 246.94, 329.63};
    const int noteLen = static_cast<int>(kFs) / 2;
    for (size_t i = 0; i < v.size(); ++i) {
        const int n = static_cast<int>(i) / noteLen;
        const int k = static_cast<int>(i) % noteLen;
        const double f = kNoteHz[static_cast<size_t>(n) % 6];
        const double env = std::exp(-3.0 * k / noteLen);
        v[i] = static_cast<float>(0.35 * env *
                                  (std::sin(2.0 * M_PI * f * k / kFs) +
                                   0.3 * std::sin(4.0 * M_PI * f * k / kFs)));
    }
}

struct GearTiming {
    double signalSec = 0.0;
    double silenceSec = 0.0;
    long subnormalOut = 0;
    double checksum = 0.0;
};

// Run one unit: signal pass then silence pass, in blocks, in place — the worklet's
// exact calling convention. `Unit` only has to expose process(const float*, float*, int).
template <typename Unit>
GearTiming runGear(Unit& u, const std::vector<float>& riff) {
    GearTiming r;
    std::vector<float> buf(static_cast<size_t>(kBlock), 0.0f);

    // Both passes count subnormal OUTPUT samples inline, inside the timed region.
    // Deliberately inline, not in a separate sweep afterwards: the subnormals a unit
    // emits are largely a RING-DOWN transient in the first seconds of silence (the
    // AC30's 1588 all land there), so a second pass over a already-settled unit reads
    // zero and reports the bug as fixed when it is not. The counting branch is
    // identical in the signal pass, the silence pass, the default run and the FTZ run,
    // so every ratio in the table remains apples-to-apples.
    // The signal pass must count too: a unit whose LEVEL state has sunk subnormal emits
    // subnormals precisely when fed real audio (out = in * subnormal) — that is
    // Processor, which reads 0 on silence and 445514 on signal.
    const double t0 = now();
    for (int off = 0; off + kBlock <= kFrames; off += kBlock) {
        for (int i = 0; i < kBlock; ++i) buf[static_cast<size_t>(i)] = riff[static_cast<size_t>(off + i)];
        u.process(buf.data(), buf.data(), kBlock);
        for (int i = 0; i < kBlock; ++i) {
            const float y = buf[static_cast<size_t>(i)];
            if (clipper::dsp::isSubnormal(y)) ++r.subnormalOut;
            r.checksum += y;
        }
    }
    r.signalSec = now() - t0;

    const double t1 = now();
    for (int off = 0; off + kBlock <= kFrames; off += kBlock) {
        for (int i = 0; i < kBlock; ++i) buf[static_cast<size_t>(i)] = 0.0f;
        u.process(buf.data(), buf.data(), kBlock);
        for (int i = 0; i < kBlock; ++i) {
            const float y = buf[static_cast<size_t>(i)];
            if (clipper::dsp::isSubnormal(y)) ++r.subnormalOut;
            r.checksum += y;
        }
    }
    r.silenceSec = now() - t1;
    return r;
}

// Make + prepare a fresh unit, then time it. Fresh per pass so the FTZ pass starts
// from the same state as the default pass (state is path-dependent).
template <typename Unit, typename Setup>
void reportGear(const char* name, const std::vector<float>& riff, Setup setup) {
    Unit u{};
    setup(u);
    const GearTiming d = runGear(u, riff);

#ifdef HAVE_X86_FTZ
    setFtz(true);
    Unit u2{};
    setup(u2);
    const GearTiming f = runGear(u2, riff);
    setFtz(false);
    std::printf("  %-18s signal %7.0f ms | silence %7.0f ms (%5.2fx) | "
                "hwFTZ %7.0f / %7.0f ms | subnormal out %7ld  chk %.4g\n",
                name, d.signalSec * 1e3, d.silenceSec * 1e3,
                d.silenceSec / d.signalSec, f.signalSec * 1e3, f.silenceSec * 1e3,
                d.subnormalOut, d.checksum);
#else
    std::printf("  %-18s signal %7.0f ms | silence %7.0f ms (%5.2fx) | "
                "subnormal out %7ld  chk %.4g\n",
                name, d.signalSec * 1e3, d.silenceSec * 1e3,
                d.silenceSec / d.signalSec, d.subnormalOut, d.checksum);
#endif
}

}  // namespace

int main() {
    std::printf("Clipper denormal cost microbenchmark (%.0f kHz)\n", kFs / 1000.0);
#ifdef HAVE_X86_FTZ
    std::printf("  'cliff' = default-env / hardware-FTZ time ratio. The Denormal.h guard\n"
                "  makes the DEFAULT column reach the FTZ ceiling with NO hardware FTZ.\n\n");
#else
    std::printf("  (x86 FTZ comparison unavailable on this arch)\n\n");
#endif
    std::printf("SECTION 1 — shared primitives (Biquad, OnePoleSmoother). Both have been\n"
                "guarded since docs §25, so a ~1.00x cliff here is EXPECTED and proves\n"
                "nothing about the gear. See section 2.\n");
    report("A tone stack (4 biquads)", 400L * (1200 + 8000), scenarioTone);
    report("B high-Q resonator tail", 300L * (1 + 40000), scenarioResonator);
    report("C smoother -> 0 target", 40L * 300000, scenarioSmoother);

    std::printf("\nSECTION 2 — whole units, %d s of signal then %d s of DIGITAL SILENCE,\n"
                "in %d-frame blocks (the worklet's convention). silence/signal is the\n"
                "denormal ratio: 1.0x = state flushed, ~2x = subnormal microcode on every\n"
                "silent sample. 'subnormal out' is what the NEXT stage in the chain\n"
                "inherits. Audit finding 11.\n",
                kSeconds, kSeconds, kBlock);
    std::vector<float> riff(static_cast<size_t>(kFrames));
    fillRiff(riff);

    using namespace clipper::dsp;
    reportGear<RatModel>("D RAT", riff, [](RatModel& u) {
        u.prepare(kFs, kBlock);
        u.setParameter(RatModel::PARAM_DISTORTION, 0.6f);
        u.setParameter(RatModel::PARAM_FILTER, 0.5f);
        u.setParameter(RatModel::PARAM_LEVEL, 0.7f);
    });
    reportGear<GoldModel>("E GOLD", riff, [](GoldModel& u) {
        u.prepare(kFs, kBlock);
        u.setParameter(GoldModel::PARAM_GAIN, 0.6f);
        u.setParameter(GoldModel::PARAM_TREBLE, 0.5f);
        u.setParameter(GoldModel::PARAM_OUTPUT, 0.7f);
    });
    // Set every knob explicitly. LEVEL/VOLUME defaults to 0 on these three (prepare()
    // snaps the smoother onto a 0 target), so leaving them alone benchmarks a pedal
    // rendering digital silence into its own output — the internal state still shows
    // its cliff, but the 'subnormal out' column is meaningless. Ask how it is set.
    reportGear<SdModel>("F SD-1", riff, [](SdModel& u) {
        u.prepare(kFs, kBlock);
        u.setParameter(SdModel::PARAM_DRIVE, 0.6f);
        u.setParameter(SdModel::PARAM_TONE, 0.5f);
        u.setParameter(SdModel::PARAM_LEVEL, 0.7f);
    });
    reportGear<TsModel>("G Screamer", riff, [](TsModel& u) {
        u.prepare(kFs, kBlock);
        u.setParameter(TsModel::PARAM_DRIVE, 0.6f);
        u.setParameter(TsModel::PARAM_TONE, 0.5f);
        u.setParameter(TsModel::PARAM_LEVEL, 0.7f);
    });
    reportGear<MuffModel>("H Muff", riff, [](MuffModel& u) {
        u.prepare(kFs, kBlock);
        u.setParameter(MuffModel::PARAM_SUSTAIN, 0.6f);
        u.setParameter(MuffModel::PARAM_TONE, 0.5f);
        u.setParameter(MuffModel::PARAM_VOLUME, 0.7f);
    });
    reportGear<Jcm800Amp>("I JCM800", riff, [](Jcm800Amp& u) { u.prepare(kFs, kBlock); });
    reportGear<TwinAmp>("J Twin", riff, [](TwinAmp& u) { u.prepare(kFs, kBlock); });
    reportGear<Ac30Amp>("K AC30", riff, [](Ac30Amp& u) { u.prepare(kFs, kBlock); });
    // The M0 gain core with GAIN at 0: its hand-rolled smoother recurrence (the one
    // place in the tree that reimplements OnePoleSmoother) sinks into subnormals and
    // then multiplies EVERY sample by a subnormal, forever.
    reportGear<clipper::Processor>("L Processor g=0", riff, [](clipper::Processor& u) {
        u.prepare(kFs, kBlock);
        u.setParameter(clipper::PARAM_GAIN, 0.0f);
    });

    std::printf("\nReading the table: compare `silence` against `hwFTZ` silence, NOT against\n"
                "`signal`. A unit can legitimately be cheaper or dearer on silence (the tube\n"
                "and BJT Newton solves converge in fewer iterations when nothing is moving),\n"
                "so the signal/silence ratio alone proves nothing. It is `silence` >> `hwFTZ\n"
                "silence` that means subnormals, because FTZ changes NOTHING ELSE.\n"
                "  Worked example: the Muff is 2.8x dearer on silence than on signal and has\n"
                "  NO denormal problem at all — its hwFTZ column is the same 2.8x. That cost\n"
                "  is BjtStage's Ebers-Moll Newton taking more iterations near the quiescent\n"
                "  point, which is a separate performance question, not this one.\n");
    return 0;
}
