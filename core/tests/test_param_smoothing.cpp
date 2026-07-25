// Clipper — PARAMETER SMOOTHING tests (2026-07-24 audit finding 6, docs §35).
//
// THE DEFECT THIS PINS
// `OnePoleSmoother` was used by the pedals, the clean AmpModel, chorus, phaser and
// reverb — and by NONE of Jcm800Preamp/PowerAmp, TwinPreamp/PowerAmp, Ac30Preamp/
// PowerAmp. Those assigned knob values straight to state and evaluated the resulting
// scale factor ONCE PER BLOCK, applied as a constant. Behind up to 76 dB of gain that
// makes a single knob step a step discontinuity in a 72 dB path, and in the web app a
// knob DRAG pushes values at pointer rate, so it is a continuous stream of them.
//
// It also contradicted an invariant the architecture states in two places —
// web/worklet/clipper-processor.js and native/src/ClipperEngine.h both say "Plain knob
// moves are NOT bracketed — the core's ~5 ms one-pole smoothing already declicks
// those." The chain layer deliberately does not bracket a knob move because it relies on
// that. For the valve amps the reliance was unfounded, which is what this suite exists
// to stop happening again.
//
// WHAT IS ASSERTED
//
//   A. STEP / STEADY SLEW, for EVERY knob on all three valve amps, with the knob moved
//      MID-RENDER between blocks and the worst of eight step positions kept. See
//      support/StepSlew.h for the metric and for why both of those refinements are
//      load-bearing. Three moves per continuous knob: one arrow-key step (0.05, the UI's
//      KEY_STEP) and a preset-sized 0.40 jump in each direction.
//
//   B. THE CONTROL CASE. The already-smoothed clean amp (AmpModel) goes through the
//      IDENTICAL harness and has to clear the identical bars. Without it the bars are
//      numbers someone made up; with it they are "no worse than the amp that was doing
//      this correctly all along".
//
//   C. SENSITIVITY. The same moves spliced HARD — the two settings rendered separately
//      and cut together, which is what "assign the knob straight to state" produces at
//      its worst — must blow past the bar by a wide margin on the gain-like controls.
//      Without this, A could be passing on a metric that cannot fail.
//
//   D. NO RAMP FOR A KNOB SET BEFORE THE FIRST BLOCK. Smoothing must not change a
//      STATIC render: a value written after prepare() but before the first process()
//      has to apply instantly, bit-for-bit identically to writing it before prepare().
//      This is the property that keeps the goldens and native/tests/identical_core_test
//      valid, and it is why the preamps carry a deferred prime rather than relying on
//      prepare() alone (the C ABI prepares inside amp_create and the knobs arrive
//      afterwards).
//
// One sample rate (48 kHz). The metric is a ratio of two slews measured on the same
// render, so it is nearly rate- and level-independent (verified across 0.005..0.1 V and
// 110/220 Hz while developing this: the ratios moved by under 10 %); running three rates
// would triple a ~3 minute suite for no new information.

#include "support/AssertsLive.h"
#include "support/StepSlew.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "clipper/dsp/Ac30Amp.h"
#include "clipper/dsp/AmpModel.h"
#include "clipper/dsp/Jcm800Amp.h"
#include "clipper/dsp/TwinAmp.h"

using clipper::test::maxStep;
using clipper::test::measureHardSpliceStepSlew;
using clipper::test::measureStepSlew;
using clipper::test::StepSlewConfig;
using clipper::test::StepSlewResult;

namespace {

// --- Bars --------------------------------------------------------------------------
// The clean amp, measured by this very harness, is the reference: worst 1.37x for a
// 0.05 arrow-key step and 2.84x for a 0.40 jump (its TREBLE shelf). The bars sit just
// above those, so "as good as the amp that was already smoothed" is what passes.
constexpr double kBarSmallStep = 2.0;   // one 0.05 arrow-key step
constexpr double kBarBigJump = 3.0;     // a 0.40 preset/assistant-sized jump
// Sensitivity: an unsmoothed hard splice of the SAME move must exceed this. The
// pre-fix implementation measured 68.9x-121.9x on the four gain-like controls.
constexpr double kSensitivityBar = 4.0 * kBarBigJump;

StepSlewConfig baseConfig() {
    StepSlewConfig c;
    c.sampleRate = 48000.0;
    c.toneHz = 220.0;
    // 0.015 V at the amp input: a real pluck, and low enough that the power sections are
    // not slamming their clipping mechanism — a squared-off output would hide any click
    // inside its own edges (see the note in support/StepSlew.h).
    c.amplitude = 0.015;
    c.blockSize = 64;
    c.preFrames = 24000;   // 0.5 s: every coupling cap and grid leak settled
    c.positions = 8;       // 8 x 64 = 512 samples > 2 cycles of 220 Hz
    c.slewWindow = 8000;
    c.settleAfter = 2400;  // 50 ms = 6 tau at 8 ms
    // Room for the latest step position plus its settled slew window.
    c.postFrames = c.positions * c.blockSize + c.settleAfter + c.slewWindow + 100;
    return c;
}

bool failed = false;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL %s\n", what.c_str());
        failed = true;
    }
}

// A knob under test.
struct Knob {
    enum Kind {
        CONT,      // a continuous 0..1 knob: measured at one arrow step and both jumps
        SWITCH,    // a 0/1 toggle: a 0.05 step is a no-op, so only the toggle is measured
        EXTERNAL,  // routed to a sub-unit with its OWN smoothing policy (ReverbModel).
                   // Still held to the step/slew bars, but excluded from the static-render
                   // invariance block, which is a property of THIS slice's units only.
    };
    const char* name;
    int pid;
    Kind kind = CONT;

    bool isSwitch() const { return kind == SWITCH; }
};

// One (unit, knob, move) measurement. `Amp` is default-constructible, has
// prepare(fs,block) / setParameter(int,float) / process(in,out,n) — the shared shape of
// every amp voice in the core.
template <class Amp>
double measure(const char* unit, const Knob& k, const std::vector<Knob>& all, float from,
               float to, double bar) {
    const StepSlewConfig c = baseConfig();
    // The amp voices are deliberately non-copyable, so each render gets a fresh one
    // through a unique_ptr rather than by assignment.
    std::unique_ptr<Amp> amp;
    const StepSlewResult r = measureStepSlew(
        c,
        [&] {
            amp = std::make_unique<Amp>();
            // Every other knob sits at its own sensible mid position, so the amp under
            // test is in a realistic state rather than at whatever the defaults are.
            for (const Knob& o : all)
                if (o.pid != k.pid) amp->setParameter(o.pid, o.isSwitch() ? 0.0f : 0.5f);
            amp->setParameter(k.pid, from);
            amp->prepare(c.sampleRate, c.blockSize);
        },
        [&] { amp->setParameter(k.pid, to); },
        [&](const float* in, float* out, int n) { amp->process(in, out, n); });

    std::printf("  [A ] %-9s %-9s %.2f -> %.2f : slew %.6f/%.6f  seam %.6f  ratio %6.2fx  "
                "(bar %.1fx, worst pos %d)\n",
                unit, k.name, from, to, r.slewBefore, r.slewAfter, r.seam, r.ratio, bar,
                r.worstPosition);
    std::fflush(stdout);
    check(r.slew > 0.0, std::string(unit) + " " + k.name + ": no steady signal to measure");
    check(r.ratio <= bar,
          std::string(unit) + " " + k.name + ": a knob move steps " +
              std::to_string(r.ratio) +
              "x the signal's own steady slew — that is a click. Every continuous knob "
              "must be one-pole smoothed and applied PER SAMPLE (audit finding 6).");
    return r.ratio;
}

// The sensitivity reference for the same move: no smoothing at all.
template <class Amp>
double measureSplice(const char* unit, const Knob& k, const std::vector<Knob>& all, float from,
                     float to) {
    const StepSlewConfig c = baseConfig();
    std::unique_ptr<Amp> amp;
    auto prep = [&](float v) {
        return [&, v] {
            amp = std::make_unique<Amp>();
            for (const Knob& o : all)
                if (o.pid != k.pid) amp->setParameter(o.pid, o.isSwitch() ? 0.0f : 0.5f);
            amp->setParameter(k.pid, v);
            amp->prepare(c.sampleRate, c.blockSize);
        };
    };
    const StepSlewResult r = measureHardSpliceStepSlew(
        c, prep(from), prep(to),
        [&](const float* in, float* out, int n) { amp->process(in, out, n); });
    std::printf("  [C ] %-9s %-9s %.2f -> %.2f HARD SPLICE: step %.6f = %7.2fx slew "
                "(must exceed %.1fx)\n",
                unit, k.name, from, to, r.seam, r.ratio, kSensitivityBar);
    std::fflush(stdout);
    check(r.ratio > kSensitivityBar,
          std::string(unit) + " " + k.name +
              ": an UNSMOOTHED version of this move does not exceed the bar, so the bar "
              "cannot fail and block A proves nothing");
    return r.ratio;
}

template <class Amp>
void runUnit(const char* unit, const std::vector<Knob>& knobs, double* worstSmall,
             double* worstBig) {
    std::printf("\n--- %s ---\n", unit);
    for (const Knob& k : knobs) {
        if (k.isSwitch()) {
            // A switch has exactly one move worth measuring, and it is a big one.
            *worstBig = std::max(*worstBig,
                                 measure<Amp>(unit, k, knobs, 0.0f, 1.0f, kBarBigJump));
            continue;
        }
        *worstSmall =
            std::max(*worstSmall, measure<Amp>(unit, k, knobs, 0.5f, 0.55f, kBarSmallStep));
        *worstBig = std::max(*worstBig, measure<Amp>(unit, k, knobs, 0.5f, 0.90f, kBarBigJump));
        *worstBig = std::max(*worstBig, measure<Amp>(unit, k, knobs, 0.5f, 0.10f, kBarBigJump));
    }
}

// --- D. a knob set before the first block must not ramp ------------------------------
// Two renders of the same static settings. In `beforePrepare` the knobs are written and
// THEN prepare() runs (the house convention — ClipperEngine::prepare, identical_core_test
// and every golden do this, and prepare() snaps the smoothers). In `afterPrepare` they
// are written after prepare() but still before the first process(). The two must be
// BIT-identical, or a smoothed valve amp has changed what a static render sounds like
// and the goldens are no longer measuring the same amp.
//
// EXTERNAL knobs are excluded, and that is a real distinction rather than a convenience:
// PARAM_REVERB routes to ReverbModel, whose mix smoother has NO deferred prime, so a
// reverb mix written after prepare() has always ramped for ~8 ms. The clean120 golden
// depends on that — renderAmpChain() calls amp_create (which prepares AmpModel at its
// 0.5 volume default) and only then writes volume 0.4, so an 8 ms ramp is BAKED INTO the
// committed clean120_chorus.wav. Giving AmpModel a deferred prime would therefore MOVE a
// golden; the valve amps get one precisely so that they DON'T.
//
// `expectInstant` is false for exactly one unit: the clean AmpModel, which has NO
// deferred prime and therefore still ramps. That is asserted rather than skipped, because
// it is a real property of a committed golden: if somebody gives AmpModel a deferred prime
// this check goes red and tells them clean120_chorus.wav has to be re-measured, instead of
// the golden gate discovering it later as an unexplained voicing drift.
template <class Amp>
void checkNoStartupRamp(const char* unit, const std::vector<Knob>& knobs, bool expectInstant) {
    constexpr int kN = 8000;
    std::vector<float> in(kN);
    for (int i = 0; i < kN; ++i)
        in[static_cast<size_t>(i)] =
            static_cast<float>(0.05 * std::sin(2.0 * M_PI * 196.0 * (i / 48000.0)));

    auto render = [&](bool beforePrepare) {
        Amp amp;
        auto push = [&] {
            // Deliberately NOT the values prepare() would default to.
            float v = 0.15f;
            for (const Knob& k : knobs) {
                if (k.kind == Knob::EXTERNAL) continue;  // see the note above
                amp.setParameter(k.pid, k.isSwitch() ? 1.0f : v);
                v = v >= 0.85f ? 0.15f : v + 0.17f;
            }
        };
        if (beforePrepare) push();
        amp.prepare(48000.0, 128);
        if (!beforePrepare) push();
        std::vector<float> out(static_cast<size_t>(kN), 0.0f);
        for (int off = 0; off < kN; off += 128)
            amp.process(in.data() + off, out.data() + off, std::min(128, kN - off));
        return out;
    };

    const std::vector<float> a = render(true);
    const std::vector<float> b = render(false);
    const bool identical = std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    double worst = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    std::printf("  [D ] %-9s knobs set after prepare() vs before: max |Δ| %.3e %s (expected %s)\n",
                unit, worst, identical ? "BIT-IDENTICAL" : "DIFFERS",
                expectInstant ? "BIT-IDENTICAL" : "DIFFERS");
    std::fflush(stdout);
    if (expectInstant)
        check(identical,
              std::string(unit) +
                  ": a knob written between prepare() and the first process() RAMPED instead "
                  "of applying instantly. A static render is no longer what it was — the "
                  "goldens and identical_core_test are both invalidated. The unit needs its "
                  "deferred smoother prime.");
    else
        check(!identical,
              std::string(unit) +
                  ": this unit has acquired a deferred smoother prime. That is a behaviour "
                  "change, not a no-op: renderAmpChain() writes volume 0.4 AFTER amp_create "
                  "has prepared this model at its 0.5 default, so the ~8 ms ramp is baked "
                  "into the committed clean120_chorus.wav golden. Re-measure that golden "
                  "deliberately (scripts/update-goldens.sh) or revert the prime.");
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();

    using J = clipper::dsp::Jcm800Amp;
    using T = clipper::dsp::TwinAmp;
    using A = clipper::dsp::Ac30Amp;
    using C = clipper::dsp::AmpModel;

    // PRESENCE (JCM) and TOP CUT (AC30) are deliberately NOT smoothed: both are already
    // applied per sample inside the power section, and both measure at or below the clean
    // amp's baseline, so a smoother on them would be pure CPU for no audible gain. They
    // are listed here anyway — the bar is what keeps that decision honest.
    const std::vector<Knob> jcm{{"GAIN", J::PARAM_GAIN, Knob::CONT},
                                {"MASTER", J::PARAM_MASTER, Knob::CONT},
                                {"BASS", J::PARAM_BASS, Knob::CONT},
                                {"MID", J::PARAM_MID, Knob::CONT},
                                {"TREBLE", J::PARAM_TREBLE, Knob::CONT},
                                {"PRESENCE", J::PARAM_PRESENCE, Knob::CONT},
                                {"REVERB", J::PARAM_REVERB, Knob::EXTERNAL}};
    // The Twin's tremolo (SPEED / INTENSITY / TREMOLO_ENABLE) is out of scope: it is off
    // by default, has its own suite (clipper_opto_tremolo_tests) and its own open XFAILs.
    const std::vector<Knob> twin{{"VOLUME", T::PARAM_VOLUME, Knob::CONT},
                                 {"BASS", T::PARAM_BASS, Knob::CONT},
                                 {"MID", T::PARAM_MID, Knob::CONT},
                                 {"TREBLE", T::PARAM_TREBLE, Knob::CONT},
                                 {"BRIGHT", T::PARAM_BRIGHT, Knob::SWITCH},
                                 {"REVERB", T::PARAM_REVERB, Knob::EXTERNAL}};
    const std::vector<Knob> ac30{{"VOLUME", A::PARAM_VOLUME, Knob::CONT},
                                 {"BASS", A::PARAM_BASS, Knob::CONT},
                                 {"TREBLE", A::PARAM_TREBLE, Knob::CONT},
                                 {"TOPCUT", A::PARAM_TOPCUT, Knob::CONT},
                                 {"REVERB", A::PARAM_REVERB, Knob::EXTERNAL}};
    const std::vector<Knob> clean{{"VOLUME", C::PARAM_VOLUME, Knob::CONT},
                                  {"BASS", C::PARAM_BASS, Knob::CONT},
                                  {"MIDDLE", C::PARAM_MIDDLE, Knob::CONT},
                                  {"TREBLE", C::PARAM_TREBLE, Knob::CONT},
                                  {"BRIGHT", C::PARAM_BRIGHT, Knob::SWITCH},
                                  {"REVERB", C::PARAM_REVERB, Knob::EXTERNAL}};

    std::printf("=== Clipper parameter-smoothing tests (audit finding 6) ===\n");
    std::printf("48 kHz, %g Hz @ %g V, block %d, worst of %d mid-render step positions.\n"
                "Bars: %.1fx for one 0.05 arrow-key step, %.1fx for a 0.40 jump.\n",
                baseConfig().toneHz, baseConfig().amplitude, baseConfig().blockSize,
                baseConfig().positions, kBarSmallStep, kBarBigJump);

    double vSmall = 0.0, vBig = 0.0, cSmall = 0.0, cBig = 0.0;
    runUnit<J>("jcm800", jcm, &vSmall, &vBig);
    runUnit<T>("twin", twin, &vSmall, &vBig);
    runUnit<A>("ac30", ac30, &vSmall, &vBig);
    // B — the control case: the clean amp, same harness, same bars.
    runUnit<C>("clean120", clean, &cSmall, &cBig);

    // C — sensitivity, on the four controls the audit measured worst.
    std::printf("\n--- sensitivity (an unsmoothed splice of the same move) ---\n");
    measureSplice<J>("jcm800", jcm[0], jcm, 0.5f, 0.90f);  // GAIN
    measureSplice<J>("jcm800", jcm[1], jcm, 0.5f, 0.90f);  // MASTER
    measureSplice<T>("twin", twin[0], twin, 0.5f, 0.90f);  // VOLUME
    measureSplice<A>("ac30", ac30[0], ac30, 0.5f, 0.90f);  // VOLUME

    // D — a knob set before the first block applies instantly.
    std::printf("\n--- static-render invariance ---\n");
    checkNoStartupRamp<J>("jcm800", jcm, true);
    checkNoStartupRamp<T>("twin", twin, true);
    checkNoStartupRamp<A>("ac30", ac30, true);
    // The clean amp has no deferred prime and must not get one — see the note above.
    checkNoStartupRamp<C>("clean120", clean, false);

    std::printf("\nworst valve-amp ratio: %.2fx (0.05 step) / %.2fx (0.40 jump)\n", vSmall, vBig);
    std::printf("worst clean-amp ratio: %.2fx (0.05 step) / %.2fx (0.40 jump)  <- the control\n",
                cSmall, cBig);

    if (failed) {
        std::printf("\nFAIL: valve-amp parameter smoothing is not doing its job.\n");
        return 1;
    }
    std::printf("\nAll parameter-smoothing tests passed.\n");
    return 0;
}
