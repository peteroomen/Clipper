// Clipper — anti-denormal guard tests (perf diagnosis, field-lag report).
//
// Proves that no recursive state in the core is left in the SUBNORMAL range when a
// signal decays to silence. A subnormal in recursive state is a CPU cliff on WASM
// (no flush-to-zero) — the amp-independent "random" crackle the field report
// describes. See Denormal.h, docs §25 + §33, and scripts/denormal_bench.cpp.
//
// BLOCK 1 — the shared primitives: Biquad, OnePoleSmoother (docs §25).
// BLOCK 2 — the states audit finding 11 found unguarded (docs §33): the pedals'
//   output DC blockers, the chowdsp WDF capacitor, the three valve tone stacks, the
//   three power sections' OT / presence / TOP CUT / feedback states, TwinPreamp's
//   bright bleed, and Processor's hand-rolled gain smoother.
//
// WHY BLOCK 2 IS SHAPED DIFFERENTLY. Block 1 asserts on OUTPUT SAMPLES, which works
// because Biquad and OnePoleSmoother are float. Most of block 2's state is DOUBLE,
// and a double subnormal is INVISIBLE in the output: the models emit a float cast of
// a double node voltage, and float(1e-310) is exactly 0.0f. An output-only test
// therefore cannot tell a flushed model from one grinding through subnormals forever
// — which is exactly how this defect survived a green suite AND a benchmark that
// reported a 1.00x cliff (that benchmark only covered block 1's already-guarded
// classes). So block 2 asserts on maxAbsRestingState(), the const diagnostic those
// classes expose for precisely this purpose, and on the WDF capacitor's public
// wdf.a. Each assertion is paired with the number it measured before the fix, so a
// reader can see it had teeth.
//
// Plain-assert (no framework): any failure aborts non-zero. -UNDEBUG keeps asserts
// live in Release (the shipped build type), matching the other core tests.

#include "clipper/Processor.h"
#include "clipper/dsp/Ac30PowerAmp.h"
#include "clipper/dsp/Ac30Preamp.h"
#include "clipper/dsp/Biquad.h"
#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/CompModel.h"
#include "clipper/dsp/DelayModel.h"
#include "clipper/dsp/GateModel.h"
#include "clipper/dsp/GoldModel.h"
#include "clipper/dsp/Jcm800Preamp.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/SdModel.h"
#include "clipper/dsp/TsModel.h"
#include "clipper/dsp/TwinPreamp.h"

#include <chowdsp_wdf/chowdsp_wdf.h>

#include "support/AssertsLive.h"

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
constexpr int kBlock = 128;

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

// ===========================================================================
// BLOCK 2 — audit finding 11: the states the policy had not reached (docs §33)
// ===========================================================================

// A plucked-note program at realistic guitar level, then digital silence. The signal
// half is where the guard must be invisible; the silent half is where it must act.
std::vector<float> riffThenSilence(int signalFrames, int silenceFrames) {
    std::vector<float> v(static_cast<size_t>(signalFrames + silenceFrames), 0.0f);
    const int noteLen = 12000;
    const double hz[] = {82.41, 110.0, 146.83, 196.0};
    for (int i = 0; i < signalFrames; ++i) {
        const double f = hz[static_cast<size_t>((i / noteLen) % 4)];
        const int k = i % noteLen;
        const double env = std::exp(-3.0 * k / noteLen);
        v[static_cast<size_t>(i)] =
            static_cast<float>(0.35 * env * std::sin(2.0 * M_PI * f * k / kFs));
    }
    return v;
}

// Drive a unit with `sig` in 128-frame blocks (the worklet's convention, in place),
// invoking `check(y)` on every output sample.
template <typename Unit, typename Check>
void driveBlocks(Unit& u, const std::vector<float>& sig, Check check) {
    std::vector<float> buf(static_cast<size_t>(kBlock));
    const int n = static_cast<int>(sig.size());
    for (int off = 0; off + kBlock <= n; off += kBlock) {
        for (int i = 0; i < kBlock; ++i)
            buf[static_cast<size_t>(i)] = sig[static_cast<size_t>(off + i)];
        u.process(buf.data(), buf.data(), kBlock);
        for (int i = 0; i < kBlock; ++i) check(buf[static_cast<size_t>(i)]);
    }
}

// --- 2a. The pedals' output DC blockers (float, OUTPUT-observable) -----------
// dcY1_ in OverdriveEngine (SD-1, Screamer) and GoldModel. On silence the recursion
// degenerates to y = dcR*dcY1 with dcR ~= 0.9984, which in the subnormal range rounds
// back to itself and never reaches zero. GOLD is the one whose OUTPUT showed it:
// 393607 of 480000 samples subnormal over 10 s of silence, shoved into the next stage.
// Property: a pedal fed digital silence must eventually emit EXACT digital silence,
// and must never emit a subnormal on the way there.
template <typename Pedal, typename Setup>
void testPedalSilenceIsExactlySilent(const char* what, Setup setup) {
    Pedal p{};
    setup(p);
    // 2 s of program, then 8 s of silence — long enough that an unguarded one-pole is
    // deep in the subnormal band (it gets there in well under a second).
    const std::vector<float> sig = riffThenSilence(2 * 48000, 8 * 48000);

    long subnormals = 0;
    int lastNonZero = -1, idx = 0;
    driveBlocks(p, sig, [&](float y) {
        assert(std::isfinite(y) && "pedal emitted a non-finite sample");
        if (isSubnormal(y)) ++subnormals;
        if (y != 0.0f) lastNonZero = idx;
        ++idx;
    });
    assert(subnormals == 0 && "pedal emitted SUBNORMAL output samples on silence");
    // And it genuinely reached exact zero rather than merely never going subnormal
    // (which a pedal stuck at a normal DC offset would also satisfy).
    assert(lastNonZero >= 0 && "test setup: pedal never produced any output at all");
    assert(lastNonZero < static_cast<int>(sig.size()) - 48000 &&
           "pedal never settled to EXACT digital silence within 7 s of silence");
    std::printf("  [ok] %s: 0 subnormal out, exact digital silence after %.2f s\n", what,
                (lastNonZero - 2 * 48000) / kFs);
}

// --- 2b. The chowdsp WDF capacitor (double) ---------------------------------
// The RAT's / GOLD's diode clipper is a WDF network whose recursive memory is the
// shunt capacitor's wave state. It is private, so the guard reaches it through the
// library's own public API (see flushDenormalWdfCapacitor in Denormal.h). Two claims,
// both checkable because wdf.a mirrors that state and is public:
//   TEETH:        the guarded network's state reaches EXACTLY 0 on a silent tail,
//                 while an identical UNGUARDED network sticks in the subnormal range.
//   TRANSPARENCY: over program-level audio the two networks agree BIT-FOR-BIT.
// The unguarded reference is a second real chowdsp network with the same components,
// not a reimplementation — the only difference between them is the flush.
void testWdfCapacitorGuard() {
    constexpr double kRs = 1.0e3, kCp = 10.0e-9;      // the RAT's stage-2 values
    constexpr double kIs = 2.52e-9, kVt = 25.85e-3;   // silicon diode pair

    struct Net {
        chowdsp::wdft::ResistiveVoltageSourceT<double> Vs{kRs};
        chowdsp::wdft::CapacitorT<double> Cp{kCp, 48000.0};
        chowdsp::wdft::WDFParallelT<double, decltype(Vs), decltype(Cp)> P1{Vs, Cp};
        chowdsp::wdft::DiodePairT<double, decltype(P1)> diodes{P1, kIs, kVt, 1.0};
        double step(double in) {
            Vs.setVoltage(in);
            diodes.incident(P1.reflected());
            P1.incident(diodes.reflected());
            return chowdsp::wdft::voltage<double>(Cp);
        }
    };
    Net guarded, unguarded;
    guarded.Cp.prepare(4.0 * kFs);    // the models run the cap at the OS rate
    unguarded.Cp.prepare(4.0 * kFs);

    // TRANSPARENCY: program-level audio, bit-for-bit.
    for (int i = 0; i < 96000; ++i) {
        double x = 0.6 * std::sin(0.02 * i) + 0.3 * std::sin(0.31 * i + 0.5);
        if (i == 100) x += 1.0;
        if (i > 48000) x += 0.25;
        const double a = guarded.step(x);
        clipper::dsp::flushDenormalWdfCapacitor(guarded.Cp);
        const double b = unguarded.step(x);
        assert(a == b && "guarded WDF network differs from unguarded on normal-range audio");
    }
    std::printf("  [ok] WDF capacitor guard bit-transparent on program-level audio\n");

    // TEETH: a silent tail. The guarded state must land on EXACTLY zero; the unguarded
    // one must be caught sitting in the subnormal range (that is the defect, measured
    // as the RAT's 2.01x and GOLD's 1.96x silence cost).
    bool unguardedWentSubnormal = false;
    for (int i = 0; i < 400000; ++i) {
        guarded.step(0.0);
        clipper::dsp::flushDenormalWdfCapacitor(guarded.Cp);
        unguarded.step(0.0);
        assert(!isSubnormal(guarded.Cp.wdf.a) && "guarded WDF cap state is subnormal");
        if (isSubnormal(unguarded.Cp.wdf.a)) unguardedWentSubnormal = true;
        if (guarded.Cp.wdf.a == 0.0 && unguardedWentSubnormal) break;
    }
    assert(guarded.Cp.wdf.a == 0.0 &&
           "guarded WDF cap state never reached exact zero on a silent tail");
    assert(unguardedWentSubnormal &&
           "the UNGUARDED reference never went subnormal, so this test proves nothing "
           "about the guard — the setup no longer reproduces audit finding 11");
    std::printf("  [ok] WDF capacitor: guarded -> exact 0, unguarded reference CONFIRMED "
                "stuck subnormal (test has teeth)\n");
}

// --- 2c. Double-state units: the resting state must land on EXACTLY zero -----
// Every state maxAbsRestingState() covers rests at zero by construction, so after a
// silent tail an exactly-zero reading is the property, and any tiny nonzero residual
// is a state asymptoting through the double subnormal range. Also assert the state is
// never subnormal DURING the tail, which is the actual CPU cost.
template <typename Unit, typename Setup>
void testRestingStateReachesExactZero(const char* what, Setup setup, double preMeasured) {
    Unit u{};
    setup(u);
    const std::vector<float> sig = riffThenSilence(48000, 6 * 48000);
    std::vector<float> buf(static_cast<size_t>(kBlock));
    const int n = static_cast<int>(sig.size());
    for (int off = 0; off + kBlock <= n; off += kBlock) {
        for (int i = 0; i < kBlock; ++i)
            buf[static_cast<size_t>(i)] = sig[static_cast<size_t>(off + i)];
        u.process(buf.data(), buf.data(), kBlock);
        const double s = u.maxAbsRestingState();
        assert(std::isfinite(s) && "resting state is not finite");
        assert(!clipper::dsp::isSubnormal(s) &&
               "a resting state is SUBNORMAL (permanent audio-thread denormal generator)");
    }
    assert(u.maxAbsRestingState() == 0.0 &&
           "resting state did not reach EXACTLY zero after 6 s of digital silence — it is "
           "asymptoting, which is audit finding 11");
    std::printf("  [ok] %s: resting state exactly 0.0 after silence "
                "(pre-fix silent-tail cost %.2fx vs hardware FTZ)\n",
                what, preMeasured);
}

// --- 2d. Processor's hand-rolled gain smoother -------------------------------
// The one place in the tree that reimplements OnePoleSmoother's recurrence. With GAIN
// at 0 the value asymptotes toward zero and sticks subnormal, and then EVERY sample —
// including full-scale audio — is multiplied by a subnormal. Measured 427187 subnormal
// output samples over 10 s of program before the flush.
// Property: gain 0 must produce EXACT digital silence, not 1e-43 of it.
void testProcessorGainZeroIsExactlySilent() {
    clipper::Processor p;
    p.prepare(kFs, kBlock);
    p.setParameter(clipper::PARAM_GAIN, 0.0f);
    const std::vector<float> sig = riffThenSilence(4 * 48000, 0);

    // The property is the absence of SUSTAINED subnormal generation, so count only after
    // the 5 ms ramp has settled (1 s is ~200x that). During the ramp itself the PRODUCT
    // in[i]*g can be subnormal for a handful of samples — g sweeps through 1e-30 on its
    // way to the snap, and a sample near a zero crossing times a 1e-30 gain underflows.
    // That is a bounded transient of the input, not parked state; the defect was that the
    // gain NEVER left that range, so every sample forever was a subnormal multiply.
    long subnormalsAfterSettle = 0, nonZeroAfterSettle = 0, subnormalsDuringRamp = 0;
    constexpr int kSettle = 48000;
    int idx = 0;
    driveBlocks(p, sig, [&](float y) {
        if (idx < kSettle) {
            if (isSubnormal(y)) ++subnormalsDuringRamp;
        } else {
            if (isSubnormal(y)) ++subnormalsAfterSettle;
            if (y != 0.0f) ++nonZeroAfterSettle;
        }
        ++idx;
    });
    assert(subnormalsAfterSettle == 0 &&
           "Processor at GAIN 0 kept emitting SUBNORMAL output samples after settling");
    assert(nonZeroAfterSettle == 0 &&
           "Processor at GAIN 0 never reached EXACT silence — the gain state is parked in "
           "the subnormal range (audit finding 11)");
    assert(subnormalsDuringRamp < 1000 &&
           "far more subnormals during the ramp than a 5 ms sweep through 1e-30 can "
           "explain — the gain state is not snapping");
    std::printf("  [ok] Processor GAIN=0: exact zero after settling, 0 sustained "
                "subnormals (was 427187 of 480000 on program audio); %ld bounded "
                "ramp-transient samples\n", subnormalsDuringRamp);
}

// TRANSPARENCY for the same recurrence: toward a NONZERO target the guarded loop must
// match the verbatim unguarded recurrence bit for bit, on real audio. The residual of
// a float one-pole toward a nonzero target stalls at ~1e-5 — a NORMAL float, ~25
// orders of magnitude above the 1e-30 snap — so the guard cannot fire here.
void testProcessorBitTransparent() {
    clipper::Processor p;
    p.prepare(kFs, kBlock);
    p.setParameter(clipper::PARAM_GAIN, 1.5f);

    const float coeff = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * kFs)));
    float g = 1.0f;  // prepare() snaps gain_ to the 1.0 default target
    const std::vector<float> sig = riffThenSilence(2 * 48000, 0);
    int idx = 0;
    std::vector<float> buf(static_cast<size_t>(kBlock));
    const int n = static_cast<int>(sig.size());
    for (int off = 0; off + kBlock <= n; off += kBlock) {
        for (int i = 0; i < kBlock; ++i)
            buf[static_cast<size_t>(i)] = sig[static_cast<size_t>(off + i)];
        p.process(buf.data(), buf.data(), kBlock);
        for (int i = 0; i < kBlock; ++i) {
            g += coeff * (1.5f - g);  // the exact pre-guard recurrence
            const float ref = sig[static_cast<size_t>(off + i)] * g;
            assert(buf[static_cast<size_t>(i)] == ref &&
                   "guarded Processor differs from the unguarded recurrence on real audio");
            ++idx;
        }
    }
    assert(idx > 0);
    std::printf("  [ok] Processor bit-transparent vs the unguarded recurrence "
                "(%d samples, nonzero target)\n", idx);
}

// The double overload and the maxAbsState helper carry the same invariants as the
// float one: never return a subnormal, and be the identity above the floor.
void testDoubleFlushInvariant() {
    const double samples[] = {1.0, 0.5, 1e-3, 1e-10, 1e-20, 1e-29, -0.5, -1e-20};
    for (double s : samples) {
        const double f = clipper::dsp::flushDenormal(s);
        assert(f == s && "flushDenormal(double) altered an above-floor value");
        assert(!clipper::dsp::isSubnormal(f));
    }
    const double tiny = 3e-310;  // double subnormal
    assert(clipper::dsp::isSubnormal(tiny) && "test setup: expected a double subnormal");
    assert(clipper::dsp::flushDenormal(tiny) == 0.0);
    assert(clipper::dsp::flushDenormal(1e-35) == 0.0);   // below the 1e-30 floor
    assert(!clipper::dsp::isSubnormal(0.0) && "0.0 is not a subnormal");
    // maxAbsState: magnitude, order-independent, 0.0 for all-zero.
    assert(clipper::dsp::maxAbsState(0.0, 0.0, 0.0) == 0.0);
    assert(clipper::dsp::maxAbsState(1.0, -3.0, 2.0) == 3.0);
    assert(clipper::dsp::maxAbsState(-3.0, 2.0, 1.0) == 3.0);
    std::printf("  [ok] flushDenormal(double)/isSubnormal(double)/maxAbsState invariants\n");
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();
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

    // --- Block 2: audit finding 11 (docs §33) -------------------------------
    std::printf("Block 2 — audit finding 11: the states the policy had not reached...\n");
    testDoubleFlushInvariant();

    using namespace clipper::dsp;

    // 2a. The output DC blockers, via the pedals that own them.
    testPedalSilenceIsExactlySilent<GoldModel>("GOLD output DC blocker", [](GoldModel& u) {
        u.prepare(kFs, kBlock);
        u.setParameter(GoldModel::PARAM_GAIN, 0.6f);
        u.setParameter(GoldModel::PARAM_TREBLE, 0.5f);
        u.setParameter(GoldModel::PARAM_OUTPUT, 0.7f);
    });
    // M13.1's compressor: six linear filter states rest at zero and are flushed;
    // its ENVELOPE node deliberately is not (it rests at ~8.5 V - ADR 006's scope
    // rule), which is why maxAbsRestingState() excludes it. The bar here is the
    // same as every other pedal's: EXACTLY zero, and exactly silent.
    testPedalSilenceIsExactlySilent<CompModel>("Squash compressor (M13.1)", [](CompModel& u) {
        u.prepare(kFs, kBlock);
        u.setParameter(CompModel::PARAM_SUSTAIN, 1.0f);  // worst case: most gain
        u.setParameter(CompModel::PARAM_LEVEL, 1.0f);
    });
    // M13.6a's gate: its FOUR coupling-cap states rest at zero and are flushed.
    // Two states deliberately are not, each at a real operating point (ADR 006):
    // the shared detector's envelope node rests at the 9.0000 V rail, and the VCA
    // gain rests at its off-isolation, 1.0000e-04. maxAbsRestingState() excludes
    // both, so the bar below is the same EXACTLY-zero one every other pedal gets.
    // Worst case for a gate is the knob that keeps it OPEN longest.
    testPedalSilenceIsExactlySilent<GateModel>("Curfew noise gate (M13.6a)",
                                               [](GateModel& u) {
        u.prepare(kFs, kBlock);
        u.setParameter(GateModel::PARAM_THRESHOLD, 0.0f);
        u.setParameter(GateModel::PARAM_DECAY, 1.0f);
    });
    // M13.4's delay. EVERY recursive state it owns rests at zero — including the
    // 550 ms delay LINE, which is the case ADR 006 cares most about here: a
    // decaying feedback tail circulates float subnormals through a quarter of a
    // million samples of ring buffer, which is invisible in the audio and is a
    // permanent WASM denormal source (WASM has no flush-to-zero at all).
    //
    // WHY THE SHORTEST DELAY AND NOT THE LONGEST. This bar is "exact digital
    // silence within 7 s", and on a pedal whose whole job is a feedback loop the
    // ring-down to the -600 dB flush floor is set by the LOOP, not by any guard.
    // Measured settle-to-exact-zero, after the same 2 s of program:
    //
    //     DELAY 0.0 (30 ms):   FB 0.0  0.26 s | FB 0.5  0.78 s | FB 0.8   1.08 s
    //     DELAY 0.5 (290 ms):  FB 0.0  0.52 s | FB 0.5  7.68 s | FB 0.8  15.55 s
    //     DELAY 1.0 (550 ms):  FB 0.0  0.78 s | FB 0.5 15.51 s | FB 0.8 199.85 s
    //
    // 200 s at 550 ms / FEEDBACK 0.8 is the pedal WORKING: a loop gain of 0.736 per
    // 550 ms pass is 4.8 dB/s, so 590 dB takes two minutes. Subnormals are ZERO at
    // every one of those nine settings, which is the property this file is for; the
    // long-line case is covered by clipper_delay_tests over a 40 s tail.
    testPedalSilenceIsExactlySilent<DelayModel>("Echoman BBD delay (M13.4)", [](DelayModel& u) {
        u.prepare(kFs, kBlock);
        u.setParameter(DelayModel::PARAM_DELAY, 0.0f);      // 30 ms: the fastest loop
        u.setParameter(DelayModel::PARAM_FEEDBACK, 0.8f);   // worst-case tail for it
        u.setParameter(DelayModel::PARAM_BLEND, 1.0f);
    });
    testPedalSilenceIsExactlySilent<SdModel>("SD-1 output DC blocker", [](SdModel& u) {
        u.prepare(kFs, kBlock);
        u.setParameter(SdModel::PARAM_DRIVE, 0.6f);
        u.setParameter(SdModel::PARAM_TONE, 0.5f);
        u.setParameter(SdModel::PARAM_LEVEL, 0.7f);  // defaults to 0 = silent; set it
    });
    testPedalSilenceIsExactlySilent<TsModel>("Screamer output DC blocker", [](TsModel& u) {
        u.prepare(kFs, kBlock);
        u.setParameter(TsModel::PARAM_DRIVE, 0.6f);
        u.setParameter(TsModel::PARAM_TONE, 0.5f);
        u.setParameter(TsModel::PARAM_LEVEL, 0.7f);
    });

    // 2b. The WDF capacitor (guarded vs a real unguarded chowdsp network).
    testWdfCapacitorGuard();

    // 2c. The double-state units. The pre-fix numbers are the ISOLATED silent-tail cost
    // against hardware FTZ, measured per component (docs §33): the tone stacks are the
    // extremes, the power sections milder, and every one of them was invisible in the
    // audio, which is why they needed a state-level assertion rather than an output one.
    testRestingStateReachesExactZero<FenderToneStack>(
        "FenderToneStack (Twin TMB)", [](FenderToneStack& t) {
            t.prepare(kFs);
            t.setKnobs(0.5, 0.5, 0.5);
        }, 18.58);
    testRestingStateReachesExactZero<MarshallToneStack>(
        "MarshallToneStack (JCM800 TMB)", [](MarshallToneStack& t) {
            t.prepare(kFs);
            t.setKnobs(0.5, 0.5, 0.5);
        }, 68.22);
    testRestingStateReachesExactZero<TopBoostToneStack>(
        "TopBoostToneStack (AC30)", [](TopBoostToneStack& t) {
            t.prepare(kFs);
            t.setKnobs(0.5, 0.5);
        }, 1.00);
    testRestingStateReachesExactZero<Ac30PowerAmp>(
        "Ac30PowerAmp (OT bandwidth pair)",
        [](Ac30PowerAmp& p) { p.prepare(kFs, kBlock); }, 1.35);

    // NOT asserted here, and the omissions are deliberate + measured (docs §33). The
    // JCM800 and Twin power sections, and the COMPOSED valve preamps, have no state that
    // rests at exactly zero: a push-pull pair's differential plate current does not
    // cancel exactly (~1e-13 idle residual) and a tube stage's coupling cap parks at its
    // grid-current bias point (~1e-11 through TwinPreamp), so every state there idles
    // hundreds of orders of magnitude clear of the subnormal boundary. Their flushes are
    // guard-rails for the post-reset() case, not fixes for a measured cliff, and an
    // assertion on them would have no teeth. See the comments on those classes.
    // The property is asserted where it holds unconditionally: on the three tone stacks
    // above, which is where the 18.6x and 68.2x cliffs actually live.

    // 2d. Processor's hand-rolled smoother: teeth, then transparency.
    testProcessorGainZeroIsExactlySilent();
    testProcessorBitTransparent();

    std::printf("All anti-denormal guard tests passed.\n");
    return 0;
}
