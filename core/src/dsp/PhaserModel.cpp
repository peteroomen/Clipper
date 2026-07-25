// Clipper — PhaserModel (v1.1 item 3). Script-era 4-stage JFET phaser ("Ninety").
// See PhaserModel.h for the overview, the topology, and the LFO-shape rationale.
//
// ---------------------------------------------------------------------------
// Chosen numbers and why (mirrors the AmpModel/ChorusModel documentation style):
//
//   STAGES  kNumStages = 4 first-order allpasses. Each rotates phase 0 → −180°;
//     four in series rotate 0 → −720°, so 0.5·(dry+wet) nulls where the total
//     phase is an ODD multiple of 180° (wet is anti-phase with dry): at −180°
//     and −540°. Two crossings in-band → exactly TWO notches. For IDENTICAL
//     stages the analog corner ratios fall out of −8·atan(f/fc): notch₁ at
//     tan(22.5°)·fc ≈ 0.414·fc and notch₂ at tan(67.5°)·fc ≈ 2.414·fc (a 5.83×
//     spread). The discrete model warps these slightly; the tests compare the
//     RENDERED notches to the exact discrete transfer function, and separately
//     sanity-check the ≈0.414/2.414 physics.
//
//   CORNER SWEEP  fc ∈ [kFcMin, kFcMax] = [200, 2000] Hz — a clean decade, the
//     JFETs' ~decade of drain-source resistance swing. Over a cycle the two
//     notches therefore sweep notch₁ ≈ 83…828 Hz and notch₂ ≈ 483…4828 Hz — the
//     moving comb lives right across the guitar's low-mids and presence, the
//     Phase-90 whoosh. Depth (this sweep span) is FIXED — script-authentic.
//
//   LFO  a rounded triangle modulating LOG(fc): logfc = logCenter + 0.5·lfo·
//     ln(kFcMax/kFcMin), lfo ∈ [−1,1]. A triangle in log-fc = constant octaves/s
//     (the even Phase-90 movement); a small aligned-cosine blend (kRound) rounds
//     the reversals so there is no turnaround tick (no-zipper test). Aligned so
//     both the triangle and the cosine peak at phase 0.5 and trough at phase 0.
//
//   SPEED  0..1 knob → LFO Hz, LOG mapped kMinHz..kMaxHz (0.06..8 Hz):
//     rate = kMinHz·(kMaxHz/kMinHz)^knob. Log so the slow tape-warble region
//     (a Phase 90 lives most of its life <2 Hz) occupies most of the knob and
//     the Leslie-ish fast end is reachable but compressed.
//
//   DETUNE  ±1.5% fixed per-stage spread (kDetune). Real JFETs never match, so
//     the four notch contributions do not stack into one infinitely-deep null;
//     this keeps the composite notch finite but still >20 dB. Off (measurement
//     hook) → four identical stages, the clean analytic reference.
//
//   NO FEEDBACK. The script Phase 90 has no regeneration path (that is the later
//     block-logo/"Script" addition). Omitted here on purpose (docs §20 ledger).
//
//   FIRST-ORDER ALLPASS, bilinear form  H(z) = (a + z⁻¹)/(1 + a·z⁻¹) with
//     a = (t − 1)/(t + 1), t = tan(π·fc/fs). This puts the −90° phase point at
//     fc exactly (prewarped), so the notch geometry above holds at every rate.
//     Difference eq (per section): y = a·x + x1 − a·y1 (x1/y1 = prev in/out).
//
// The corner (hence all four coefficients) is recomputed PER SAMPLE from the
// continuous LFO — no block stepping — so a fast sweep has no zipper. The LFO
// rate is one-pole smoothed (~8 ms) so a SPEED move never clicks; the LFO phase
// is continuous so a rate change never jumps. Deterministic + allocation-free in
// process().
// ---------------------------------------------------------------------------

#include "clipper/dsp/PhaserModel.h"

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/ParamGuard.h"

#include <algorithm>
#include <cmath>

namespace clipper::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586;

constexpr int kNumStages = 4;       // 4 allpass sections -> 2 notches
constexpr double kFcMin = 200.0;    // corner at LFO trough (Hz)
constexpr double kFcMax = 2000.0;   // corner at LFO peak (Hz); a clean decade
constexpr double kMinHz = 0.06;     // LFO rate at speed = 0
constexpr double kMaxHz = 8.0;      // LFO rate at speed = 1
constexpr double kRound = 0.15;     // cosine blend that rounds the triangle peaks
constexpr double kSmoothSeconds = 0.008;

// Fixed per-stage component-tolerance detune (multiplies the shared corner).
// Symmetric-ish spread averaging ~1.0; deterministic (no RNG) so renders repeat.
constexpr double kDetune[kNumStages] = {0.985, 0.995, 1.005, 1.015};

// NaN-rejecting knob clamps (ParamGuard.h) — audit finding 1. A NaN corner
// coefficient latches in the six allpass memories permanently.
double clamp01d(double v) { return clampParam01(v); }
float clamp01f(float v) { return clampParam01(v); }

// Rounded-triangle LFO in [-1, 1] as a function of cycle phase in [0, 1).
// Triangle troughs (-1) at phase 0, peaks (+1) at phase 0.5. The aligned cosine
// (-cos(2π·phase): trough at 0, peak at 0.5) rounds the triangle's corners.
double roundedTriangle(double phase) {
    const double tri = phase < 0.5 ? (4.0 * phase - 1.0) : (3.0 - 4.0 * phase);
    const double cos1 = -std::cos(kTwoPi * phase);
    return (1.0 - kRound) * tri + kRound * cos1;
}

// LFO value [-1,1] -> corner frequency (Hz), geometric (log) map over the decade.
double lfoToCornerHz(double lfo) {
    const double logCenter = 0.5 * (std::log(kFcMin) + std::log(kFcMax));
    const double halfSpan = 0.5 * (std::log(kFcMax) - std::log(kFcMin));
    return std::exp(logCenter + lfo * halfSpan);
}
}  // namespace

struct PhaserModel::Impl {
    double sampleRate = 44100.0;

    OnePoleSmoother rateHz;   // speed -> LFO Hz
    double lfoPhase = 0.0;    // [0, 1) cycle phase
    double curCornerHz = kFcMin;

    bool frozen = false;
    double frozenPhase = 0.0;
    bool detune = true;

    // Per-stage allpass state (previous input/output).
    double x1[kNumStages] = {0, 0, 0, 0};
    double y1[kNumStages] = {0, 0, 0, 0};

    // First-order allpass coefficient for a corner fc at the engine rate, clamped
    // safely below Nyquist. a = (t-1)/(t+1), t = tan(pi*fc/fs).
    double coeffForCorner(double fc) const {
        double f = fc;
        const double fmax = 0.45 * sampleRate;
        if (f < 10.0) f = 10.0;
        if (f > fmax) f = fmax;
        const double t = std::tan(kTwoPi * 0.5 * f / sampleRate);
        return (t - 1.0) / (t + 1.0);
    }
};

PhaserModel::PhaserModel() : impl_(std::make_unique<Impl>()) {}
PhaserModel::~PhaserModel() = default;

void PhaserModel::prepare(double sampleRate) {
    Impl& d = *impl_;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    d.rateHz.prepare(kSmoothSeconds, d.sampleRate);
    d.rateHz.setImmediate(static_cast<float>(speedKnobToHz(0.35)));
    reset();
}

void PhaserModel::reset() {
    Impl& d = *impl_;
    // Snap the rate smoother onto its target — a poisoned smoother value never
    // recovers on its own (audit finding 1).
    d.rateHz.reset();
    d.lfoPhase = 0.0;
    d.curCornerHz = lfoToCornerHz(roundedTriangle(0.0));
    for (int s = 0; s < kNumStages; ++s) {
        d.x1[s] = 0.0;
        d.y1[s] = 0.0;
    }
}

void PhaserModel::setSpeed(float knob01) {
    impl_->rateHz.setTarget(static_cast<float>(speedKnobToHz(clamp01f(knob01))));
}

void PhaserModel::setParameter(int paramId, float value) {
    if (paramId == PARAM_SPEED) setSpeed(value);
    // Slots 1 and 2 are carried for the shared 3-knob pedal shape but unused.
}

void PhaserModel::setFrozen(bool frozen, double phase01) {
    Impl& d = *impl_;
    d.frozen = frozen;
    d.frozenPhase = clamp01d(phase01);
}

void PhaserModel::setDetune(bool on) { impl_->detune = on; }

double PhaserModel::currentCornerHz() const { return impl_->curCornerHz; }

double PhaserModel::stageDetune(int stage) {
    if (stage < 0 || stage >= kNumStages) return 1.0;
    return kDetune[stage];
}

int PhaserModel::numStages() { return kNumStages; }

double PhaserModel::speedKnobToHz(double knob01) {
    const double k = clamp01d(knob01);
    return kMinHz * std::pow(kMaxHz / kMinHz, k);
}

double PhaserModel::cornerHzAtPhase(double phase01) {
    return lfoToCornerHz(roundedTriangle(clamp01d(phase01)));
}

void PhaserModel::process(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;

    for (int i = 0; i < numFrames; ++i) {
        const double rate = d.rateHz.next();  // advance smoother every sample

        // LFO position: frozen at a fixed phase for measurement, else the running
        // continuous phase.
        const double phase = d.frozen ? d.frozenPhase : d.lfoPhase;
        const double fc = lfoToCornerHz(roundedTriangle(phase));
        d.curCornerHz = fc;

        // Recompute the shared coefficient once, then apply the fixed per-stage
        // detune as a small multiplicative corner offset (cheap: reuses fc).
        double y = static_cast<double>(in[i]);
        for (int s = 0; s < kNumStages; ++s) {
            const double fcs = d.detune ? fc * kDetune[s] : fc;
            const double a = d.coeffForCorner(fcs);
            const double x = y;
            // Anti-denormal (Denormal.h): the allpass memory rings down through
            // the DOUBLE subnormal range on a silent tail — flush it at -600 dB.
            const double yn = flushDenormal(a * x + d.x1[s] - a * d.y1[s]);
            d.x1[s] = x;
            d.y1[s] = yn;
            y = yn;
        }

        // 50/50 dry + wet sum (the two moving notches).
        out[i] = static_cast<float>(0.5 * (static_cast<double>(in[i]) + y));

        if (!d.frozen) {
            d.lfoPhase += rate / d.sampleRate;
            if (d.lfoPhase >= 1.0) d.lfoPhase -= std::floor(d.lfoPhase);
        }
    }
}

}  // namespace clipper::dsp
