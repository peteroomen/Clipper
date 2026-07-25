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
// Three scenarios, each timed in the DEFAULT FP environment and (on x86) with
// hardware FTZ+DAZ forced on. FTZ is the "if the CPU flushed" ceiling; the ratio
// default/FTZ is the denormal cliff. The in-code guard (Denormal.h) is what makes
// the DEFAULT column reach the FTZ ceiling WITHOUT any hardware FTZ — the only fix
// available on WASM. Run this on the pre-guard and post-guard primitives to see it.
//
// Build (native, no fast-math so subnormals behave like the shipped WASM code):
//   c++ -std=c++17 -O2 -I core/include scripts/denormal_bench.cpp -o build/denormal_bench
// Run: build/denormal_bench

#include "clipper/dsp/Biquad.h"
#include "clipper/dsp/OnePoleSmoother.h"

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

}  // namespace

int main() {
    std::printf("Clipper denormal cost microbenchmark (%.0f kHz)\n", kFs / 1000.0);
#ifdef HAVE_X86_FTZ
    std::printf("  'cliff' = default-env / hardware-FTZ time ratio. The Denormal.h guard\n"
                "  makes the DEFAULT column reach the FTZ ceiling with NO hardware FTZ.\n\n");
#else
    std::printf("  (x86 FTZ comparison unavailable on this arch)\n\n");
#endif
    report("A tone stack (4 biquads)", 400L * (1200 + 8000), scenarioTone);
    report("B high-Q resonator tail", 300L * (1 + 40000), scenarioResonator);
    report("C smoother -> 0 target", 40L * 300000, scenarioSmoother);
    return 0;
}
