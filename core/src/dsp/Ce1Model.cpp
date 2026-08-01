// Clipper — Ce1Model (M13.7). The Boss CE-1 Chorus Ensemble.
// See Ce1Model.h for the overview and the three sourced differences from the
// JC-120 voicing this pedal is built on. Research + sources: docs §62.
//
// ---------------------------------------------------------------------------
// EVERY CONSTANT BELOW, AND WHERE IT CAME FROM
// ---------------------------------------------------------------------------
//
//   RATE, CHORUS mode: kChorusMinHz = 1.0, kChorusMaxHz = 3.0.
//     Both endpoints are measured figures from a teardown of a real unit.
//
//   THE RATE TAPER IS DERIVED, NOT CHOSEN. The same teardown reports the rate at
//     the knob's MIDPOINT as "about 1.75 Hz". That third number picks the law:
//       log  -> sqrt(1.0 * 3.0)   = 1.7321 Hz   (1.0 % from the report)
//       lin  -> (1.0 + 3.0) / 2   = 2.0000 Hz   (14.3 % from the report)
//     So the pot is exponential and rate = min * (max/min)^knob. A linear map is
//     refuted by a factor of fourteen in the residual; nothing was fitted.
//
//   RATE, VIBRATO mode: kVibMinHz = 3.2, kVibMaxHz = 11.6. Measured endpoints
//     from the same teardown. GAP, RECORDED: no midpoint was published for
//     vibrato, so the taper is CARRIED OVER from the chorus law above rather
//     than fitted — it is one LFO circuit with a switched range, so the pot's
//     own taper cannot plausibly change with the switch. If a vibrato midpoint
//     ever turns up and disagrees, this is the line to fix.
//
//   THE CROSS-CHECK THAT LANDED. kChorusMaxHz (3.0) < kVibMinHz (3.2). The two
//     ranges were sourced independently and they do not overlap — and the same
//     teardown states the property separately in prose ("the fastest LFO rate in
//     Chorus mode is slower than the slowest LFO rate in Vibrato mode"). Three
//     numbers agreeing is the strongest check available on this voice, and it is
//     asserted in the test suite so a future edit cannot quietly break it.
//
//   DEPTH -> DELAY WINDOW. The published window is "3-5 ms at minimum depth,
//     3-7 ms at maximum". Read that literally: the delay FLOOR is pinned at 3 ms
//     and the CEILING opens from 5 to 7 ms. As a centred sine sweep that is
//       centre D0 = (3 + ceiling)/2 = 4 + depth  ms
//       amplitude A = (ceiling - 3)/2 = 1 + depth  ms
//     both exactly linear in the knob, both straight off the source, no taper
//     invented. Consequence worth stating: A is NEVER ZERO. The CE-1's depth
//     knob at its minimum still detunes by ~7-35 cents depending on rate, and
//     that is the circuit — do not "fix" it to bottom out at unity.
//
//   GAP, RECORDED: that window is published for the VIBRATO mode's DEPTH knob.
//     The chorus mode's own excursion was NOT separately sourced (chorus mode on
//     the real panel has no depth knob at all — see the ADR note in the header).
//     The same window is used for both, on the grounds that it is one BBD and
//     one sweep generator with a switched range and shape. Named, not hidden.
//
//   MIX. kChorusDry = kChorusWet = 0.5, kVibratoDry = 0.0, kVibratoWet = 1.0.
//     The CE-1's mono jack is a passive wet+dry mix, so equal weights are the
//     topology rather than a taste call; 0.5/0.5 also keeps the level sane
//     (correlated at low frequency the sum is unity, and the comb's peaks reach
//     unity rather than +6 dB). Vibrato is the wet path ALONE — sources state it
//     as "the wet-only sound of the chorus" — so the dry weight is EXACTLY zero,
//     not a small number.
//
//   WAVEFORM. Triangle in chorus, sine in vibrato — sourced, see the header.
//
// ---------------------------------------------------------------------------
// SMOOTHING, AND WHY THE MODE SWITCH IS NOT A STEP
// ---------------------------------------------------------------------------
// Rate, base delay and sweep are smoothed inside ChorusModel (~8 ms) exactly as
// the amp's are. The two MIX WEIGHTS are smoothed here on the same constant, so
// flipping CHORUS <-> VIBRATO CROSSFADES the dry path in or out instead of
// stepping it. That is a departure from ChorusModel's "mode is a hard switch"
// convention and it is deliberate: there, mode 0 is a bypass the chain already
// declicks; here the mode switch is an ordinary automatable parameter that a
// host can sweep, and an instantaneous dry-path removal is a step discontinuity
// on a signal at full level. The WAVEFORM does still switch instantly — it is
// continuous through zero at the switch point by construction (both shapes are
// zero at phase 0 and share their sign), so it cannot step.
//
// DENORMALS (ADR 006). The delay ring is a pure FIFO of the input with NO
// recursion into it, so on silence it reaches EXACTLY 0.0 after one buffer
// length and no flush is reachable — asserted, not assumed. The two mix
// smoothers rest ON their targets and OnePoleSmoother already snaps exactly
// (its own residual guard), including the vibrato dry weight's exact 0.0. There
// is no other recursive state in this file, so it adds no new guard.

#include "clipper/dsp/Ce1Model.h"

#include "clipper/dsp/ChorusModel.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/ParamGuard.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace clipper::dsp {

namespace {

// --- LFO rate, per mode (measured endpoints; taper derived — see the banner) --
constexpr double kChorusMinHz = 1.0;
constexpr double kChorusMaxHz = 3.0;
constexpr double kVibMinHz = 3.2;
constexpr double kVibMaxHz = 11.6;

// --- Delay window, from the published 3-5 / 3-7 ms figures -------------------
constexpr double kDelayFloorMs = 3.0;      // pinned; the sweep's low turning point
constexpr double kCeilMinMs = 5.0;         // ceiling at DEPTH 0
constexpr double kCeilMaxMs = 7.0;         // ceiling at DEPTH 1

// --- Mono mix weights --------------------------------------------------------
constexpr float kChorusDry = 0.5f;
constexpr float kChorusWet = 0.5f;
constexpr float kVibratoDry = 0.0f;
constexpr float kVibratoWet = 1.0f;

constexpr double kSmoothSeconds = 0.008;
constexpr float kModeThreshold = 0.5f;

// Exponential pot law. rate = min * (max/min)^knob; derived from the published
// midpoint (see the banner) rather than assumed.
double rateFor(int mode, float knob) {
    const double k = clampParam01(knob);
    const double lo = (mode == Ce1Model::MODE_VIBRATO) ? kVibMinHz : kChorusMinHz;
    const double hi = (mode == Ce1Model::MODE_VIBRATO) ? kVibMaxHz : kChorusMaxHz;
    return lo * std::pow(hi / lo, k);
}

}  // namespace

struct Ce1Model::Impl {
    ChorusModel chorus;
    double sampleRate = 48000.0;
    int maxBlock = 128;

    // Knob state, kept so a mode change can re-derive the rate from the SAME
    // rate knob against the new range (the knob does not move; its meaning does).
    float rateKnob = 0.5f;
    float depthKnob = 0.5f;
    int mode = MODE_CHORUS;

    OnePoleSmoother dry, wet;

    // Scratch for ChorusModel's stereo pair. Sized in prepare(); the audio thread
    // allocates nothing.
    std::vector<float> sL, sR;

    void applyRate() { chorus.setRateHz(rateFor(mode, rateKnob)); }

    void applyDepth() {
        const double k = clampParam01(depthKnob);
        const double ceilMs = kCeilMinMs + k * (kCeilMaxMs - kCeilMinMs);
        chorus.setBaseDelayMs(0.5 * (kDelayFloorMs + ceilMs));
        chorus.setSweepMs(0.5 * (ceilMs - kDelayFloorMs));
    }

    void applyMode() {
        // THE OWNED ChorusModel STAYS IN MODE_CHORUS FOR BOTH OF OUR MODES, AND
        // THAT IS DELIBERATE — do not "simplify" it to forward the mode.
        //
        // ChorusModel::MODE_CHORUS puts the bit-exact DRY signal on L and the wet
        // one on R, which is exactly the pair the CE-1's mono jack mixes, so our
        // vibrato is just that same pair with the dry weight taken to zero. That
        // makes "vibrato is the wet path alone" structurally true rather than a
        // second code path that has to agree.
        //
        // Forwarding the mode instead is what the first implementation did, and
        // it CLICKED — measured at 17.0x the signal's own slew (docs §62.8).
        // ChorusModel's mode is a hard switch by its own documented convention,
        // and switching it makes L change meaning from dry to wet in one sample
        // while our dry weight is still mid-ramp at 0.5, so the output jumps from
        // (dry+wet)/2 to wet. Keeping one mode and moving only the weights makes
        // the whole transition continuous.
        chorus.setMode(ChorusModel::MODE_CHORUS);
        if (mode == MODE_VIBRATO) {
            chorus.setWaveform(ChorusModel::WAVE_SINE);
            dry.setTarget(kVibratoDry);
            wet.setTarget(kVibratoWet);
        } else {
            chorus.setWaveform(ChorusModel::WAVE_TRIANGLE);
            dry.setTarget(kChorusDry);
            wet.setTarget(kChorusWet);
        }
        applyRate();  // the rate RANGE moved with the mode
    }
};

Ce1Model::Ce1Model() : impl_(std::make_unique<Impl>()) {}
Ce1Model::~Ce1Model() = default;

void Ce1Model::prepare(double sampleRate, int maxBlockSize) {
    Impl& d = *impl_;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    d.maxBlock = maxBlockSize > 0 ? maxBlockSize : 128;

    d.chorus.prepare(d.sampleRate);
    d.sL.assign(static_cast<size_t>(d.maxBlock), 0.0f);
    d.sR.assign(static_cast<size_t>(d.maxBlock), 0.0f);

    d.dry.prepare(kSmoothSeconds, d.sampleRate);
    d.wet.prepare(kSmoothSeconds, d.sampleRate);

    // ChorusModel::prepare resets its own voicing to the JC defaults, so re-push
    // ours AFTER it — the same prepare-then-set contract AmpModel uses. The mix
    // smoothers snap rather than ramp so a freshly prepared pedal is not silent
    // for its first 8 ms.
    d.applyMode();
    d.applyDepth();
    d.dry.setImmediate(d.dry.target());
    d.wet.setImmediate(d.wet.target());
}

void Ce1Model::setOversampling(int /*factor*/) {}
int Ce1Model::oversampling() const { return 1; }
int Ce1Model::latencySamples() const { return 0; }

void Ce1Model::setParameter(int paramId, float value) {
    Impl& d = *impl_;
    switch (paramId) {
        case PARAM_RATE:
            d.rateKnob = clampParam01(value);
            d.applyRate();
            break;
        case PARAM_DEPTH:
            d.depthKnob = clampParam01(value);
            d.applyDepth();
            break;
        case PARAM_MODE: {
            // Discrete by threshold. paramToInt is for indexed choices; this is a
            // two-state switch on a continuous slot, so the threshold is explicit
            // and both ends are asserted in clipper_ce1_tests.
            const int m = (clampParam01(value) >= kModeThreshold) ? MODE_VIBRATO
                                                                  : MODE_CHORUS;
            if (m != d.mode) {
                d.mode = m;
                d.applyMode();
            }
            break;
        }
        default:
            break;
    }
}

void Ce1Model::process(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;
    if (!in || !out || numFrames <= 0) return;

    int done = 0;
    while (done < numFrames) {
        const int n = std::min(numFrames - done, d.maxBlock);
        // ChorusModel needs two DISTINCT output buffers and does not write `in`,
        // so scratch here keeps `in`/`out` free to alias each other.
        d.chorus.processStereo(in + done, d.sL.data(), d.sR.data(), n);
        for (int i = 0; i < n; ++i) {
            const float g = d.dry.next();
            const float w = d.wet.next();
            // sL is the bit-exact dry and sR the wet, in BOTH modes (see
            // applyMode). CHORUS mixes them -> the mono jack, and the comb lives
            // here. VIBRATO takes g to EXACTLY 0.0, so the dry path is genuinely
            // absent rather than merely quiet.
            out[done + i] = g * d.sL[static_cast<size_t>(i)] +
                            w * d.sR[static_cast<size_t>(i)];
        }
        done += n;
    }
}

void Ce1Model::reset() {
    Impl& d = *impl_;
    d.chorus.reset();
    d.dry.reset();
    d.wet.reset();
    // chorus.reset() snaps its smoothers onto their targets but does NOT restore
    // the voicing (it keeps the targets it holds), so nothing to re-push here.
}

int Ce1Model::mode() const { return impl_->mode; }
double Ce1Model::rateHz() const { return rateFor(impl_->mode, impl_->rateKnob); }

double Ce1Model::baseDelayMs() const {
    const double k = clampParam01(impl_->depthKnob);
    const double ceilMs = kCeilMinMs + k * (kCeilMaxMs - kCeilMinMs);
    return 0.5 * (kDelayFloorMs + ceilMs);
}

double Ce1Model::sweepMs() const {
    const double k = clampParam01(impl_->depthKnob);
    const double ceilMs = kCeilMinMs + k * (kCeilMaxMs - kCeilMinMs);
    return 0.5 * (ceilMs - kDelayFloorMs);
}

double Ce1Model::dryGain() const { return static_cast<double>(impl_->dry.value()); }
double Ce1Model::wetGain() const { return static_cast<double>(impl_->wet.value()); }

double Ce1Model::maxAbsRestingState() const {
    return impl_->chorus.maxAbsDelayLine();
}

}  // namespace clipper::dsp
