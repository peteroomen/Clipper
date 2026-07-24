// Clipper — anti-denormal guard tests (perf diagnosis, field-lag report).
//
// Proves the shared float-recursive primitives (Biquad, OnePoleSmoother) never
// leave their state in the SUBNORMAL float range when a signal decays to silence.
// A subnormal in recursive state is a CPU cliff on WASM (no flush-to-zero) — the
// amp-independent "random" crackle the field report describes. See Denormal.h,
// docs §25, and scripts/denormal_bench.cpp for the measured slowdown.
//
// Plain-assert (no framework): any failure aborts non-zero. -UNDEBUG keeps asserts
// live in Release (the shipped build type), matching the other core tests.

#include "clipper/dsp/Biquad.h"
#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/OnePoleSmoother.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using clipper::dsp::Biquad;
using clipper::dsp::isSubnormal;
using clipper::dsp::OnePoleSmoother;
namespace rbj = clipper::dsp::rbj;

namespace {

constexpr double kFs = 48000.0;

// A biquad's TDF2 output during a silent tail equals its state z1_ (y = b0*x + z1_
// with x = 0). So the output stream during silence exposes the state: it must
// never be subnormal, and it must reach EXACTLY 0 (proving the state was flushed
// to zero rather than left asymptoting through the subnormal range).
void testBiquadTailNeverSubnormal(const clipper::dsp::BiquadCoeffs& c, const char* what) {
    Biquad b;
    b.setCoeffs(c);
    b.reset();

    // Excite hard, then run a long silent tail.
    for (int i = 0; i < 256; ++i) b.process(i == 0 ? 1.0f : 0.2f * std::sin(0.1 * i));

    bool reachedExactZero = false;
    for (int i = 0; i < 400000; ++i) {
        const float y = b.process(0.0f);
        assert(!isSubnormal(y) && "biquad tail sample is subnormal");
        assert(std::isfinite(y) && "biquad tail sample not finite");
        if (y == 0.0f) { reachedExactZero = true; break; }
    }
    assert(reachedExactZero && "biquad tail never flushed to exact zero (stuck in subnormal decay)");
    std::printf("  [ok] biquad tail never subnormal, flushes to exact zero: %s\n", what);
}

// A OnePoleSmoother decaying toward a ZERO target must SNAP to exactly 0 — without
// the guard it asymptotes and STICKS in the subnormal range (value_ keeps halving
// because its ULP shrinks with it), emitting a subnormal multiply every sample
// forever. This is the dominant sustained clean-path denormal source (bright switch
// off, chorus depth 0, volume 0). Assert it reaches exact 0 and stays there.
void testSmootherSnapsToZero(float start, const char* what) {
    OnePoleSmoother sm;
    sm.prepare(0.008, kFs);
    sm.setImmediate(start);
    sm.setTarget(0.0f);

    bool snapped = false;
    for (int i = 0; i < 400000; ++i) {
        const float v = sm.next();
        assert(std::isfinite(v) && "smoother value not finite");
        assert(!isSubnormal(v) && "smoother value is subnormal (stuck denormal generator)");
        if (v == 0.0f) { snapped = true; break; }
    }
    assert(snapped && "smoother never snapped exactly to 0 (stuck asymptoting into subnormals)");
    for (int i = 0; i < 1000; ++i)
        assert(sm.next() == 0.0f && "smoother drifted off 0 after snapping");
    std::printf("  [ok] smoother snaps to exact 0, never stuck subnormal: %s\n", what);
}

// A NONZERO target does not sink into subnormals (value_ settles ~1 ULP of the
// target away, a NORMAL float), so the guard need not snap it — assert only that
// the residual never becomes subnormal and the value converges close to target.
void testSmootherConverges(float start, float target, const char* what) {
    OnePoleSmoother sm;
    sm.prepare(0.008, kFs);
    sm.setImmediate(start);
    sm.setTarget(target);

    float v = start;
    for (int i = 0; i < 200000; ++i) {
        v = sm.next();
        assert(std::isfinite(v) && "smoother value not finite");
        assert(!isSubnormal(v) && "smoother value is subnormal");
        assert(!isSubnormal(target - v) && "smoother residual is subnormal");
    }
    // Converges to within a float one-pole's natural settling floor (the increment
    // underflows value_'s ULP at a residual ~1e-5 — a NORMAL float, which is exactly
    // why a nonzero target never sinks into subnormals). Well below any audible param
    // resolution.
    assert(std::fabs(v - target) < 1e-4f && "smoother did not converge to target");
    std::printf("  [ok] smoother converges, residual never subnormal: %s\n", what);
}

// Guard invariant: flushDenormal never returns a subnormal, and is transparent
// above the floor (bit-exact for any audible value).
void testFlushInvariant() {
    const float samples[] = {1.0f, 0.5f, 1e-3f, 1e-10f, 1e-20f, 1e-29f, -0.5f, -1e-20f};
    for (float s : samples) {
        const float f = clipper::dsp::flushDenormal(s);
        assert(!isSubnormal(f));
        // Above the floor (all these are), the value is passed through unchanged.
        assert(f == s && "flushDenormal altered an above-floor value");
    }
    // Subnormals and sub-floor values flush to exactly zero.
    const float tiny = 3e-40f;  // subnormal
    assert(isSubnormal(tiny) && "test setup: expected a subnormal");
    assert(clipper::dsp::flushDenormal(tiny) == 0.0f);
    assert(clipper::dsp::flushDenormal(1e-35f) == 0.0f);  // below the 1e-30 floor
    std::printf("  [ok] flushDenormal: no subnormal out, transparent above floor\n");
}

// BIT-TRANSPARENCY: for normal-range audio the guard must change NOTHING. Run the
// guarded Biquad against a local UNGUARDED TDF2 with the same coefficients over a
// full-scale program-like signal (mixed tones + an impulse + a step) and assert
// every output sample is BIT-IDENTICAL. The guard only fires below 1e-30 (-600 dB),
// which this signal's state never reaches, so any difference is a guard bug.
void testBiquadBitTransparent(const clipper::dsp::BiquadCoeffs& c, const char* what) {
    Biquad guarded;
    guarded.setCoeffs(c);
    guarded.reset();

    float z1 = 0.0f, z2 = 0.0f;  // unguarded reference TDF2 (pre-guard math, verbatim)
    for (int i = 0; i < 96000; ++i) {
        float x = 0.6f * std::sin(0.02 * i) + 0.3f * std::sin(0.31 * i + 0.5);
        if (i == 100) x += 1.0f;              // impulse
        if (i > 48000) x += 0.25f;            // DC step
        const float yRef = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * yRef + z2;
        z2 = c.b2 * x - c.a2 * yRef;
        const float y = guarded.process(x);
        assert(y == yRef && "guarded biquad differs from unguarded on normal-range audio");
    }
    std::printf("  [ok] biquad bit-transparent on normal-range audio: %s\n", what);
}

// Same proof for the smoother: any NONZERO-target trajectory (and a zero-target
// trajectory down to the -600 dB floor) must match an unguarded reference bit for
// bit. The float one-pole naturally stalls at a NORMAL residual (~ULP of target),
// far above the 1e-30 snap, so the guard never fires on audible trajectories.
void testSmootherBitTransparent() {
    OnePoleSmoother sm;
    sm.prepare(0.008, kFs);
    sm.setImmediate(1.0f);
    sm.setTarget(0.25f);

    // Reference: the identical unguarded recurrence.
    const float coeff = static_cast<float>(1.0 - std::exp(-1.0 / (0.008 * kFs)));
    float ref = 1.0f;
    for (int i = 0; i < 96000; ++i) {
        ref += coeff * (0.25f - ref);
        const float v = sm.next();
        assert(v == ref && "guarded smoother differs from unguarded on a nonzero target");
    }
    std::printf("  [ok] smoother bit-transparent toward a nonzero target\n");
}

}  // namespace

int main() {
    std::printf("Running anti-denormal guard tests...\n");

    testFlushInvariant();

    // The exact AmpModel tone-stack voicings (the clean JC-120 path) + a sharp
    // resonator (the long-ring case), across the shelves/peaks/passes that recur
    // everywhere in the core.
    testBiquadTailNeverSubnormal(rbj::lowShelf(100.0, 12.0, 0.8, kFs), "low-shelf 100Hz +12dB");
    testBiquadTailNeverSubnormal(rbj::peaking(650.0, 9.0, 0.7, kFs), "peaking 650Hz +9dB");
    testBiquadTailNeverSubnormal(rbj::highShelf(3500.0, 12.0, 0.8, kFs), "high-shelf 3.5kHz +12dB");
    testBiquadTailNeverSubnormal(rbj::peaking(180.0, 18.0, 12.0, kFs), "high-Q resonator 180Hz Q12");
    testBiquadTailNeverSubnormal(rbj::highPass(150.0, 0.707, kFs), "high-pass 150Hz");
    testBiquadTailNeverSubnormal(rbj::lowPass(5200.0, 0.707, kFs), "low-pass 5.2kHz");

    // Bit-transparency above the floor: guarded primitives must equal the exact
    // unguarded recurrences sample-for-sample on program-level audio.
    testBiquadBitTransparent(rbj::lowShelf(100.0, 12.0, 0.8, kFs), "low-shelf 100Hz +12dB");
    testBiquadBitTransparent(rbj::peaking(650.0, 9.0, 0.7, kFs), "peaking 650Hz +9dB");
    testBiquadBitTransparent(rbj::peaking(180.0, 18.0, 12.0, kFs), "high-Q resonator 180Hz Q12");
    testSmootherBitTransparent();

    testSmootherSnapsToZero(1.0f, "1.0 -> 0 (bright off / chorus depth 0)");
    testSmootherSnapsToZero(0.001f, "tiny -> 0");
    testSmootherSnapsToZero(-0.8f, "negative -> 0");
    testSmootherConverges(1.0f, 0.5f, "1.0 -> 0.5 (nonzero target residual)");
    testSmootherConverges(0.0f, 0.25f, "0 -> 0.25");

    std::printf("All anti-denormal guard tests passed.\n");
    return 0;
}
