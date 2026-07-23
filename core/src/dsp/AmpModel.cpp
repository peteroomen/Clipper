// Clipper — AmpModel (M5). JC-120-inspired clean amp, modeled LINEAR. See
// AmpModel.h for the overview.
//
// ---------------------------------------------------------------------------
// Tone stack — centers, ranges, and rationale (JC-120-informed choices; NOT a
// measured transfer function — a musically-sensible linear approximation, in the
// spirit of the RAT model's circuit-informed comments).
//
//   VOLUME  (PARAM_VOLUME): linear-in-dB over [-40, +6] dB across the knob's
//     usable travel. The very bottom of the knob fades to true silence: below
//     kVolFadeKnob (3%) the linear gain is scaled down to 0, so knob 0 is silent
//     rather than sitting at the -40 dB floor. Above that the mapping is
//     db = -40 + 46*knob, gain = 10^(db/20). (A dB-linear taper is what a volume
//     control should feel like; the fade only affects the bottom 3% where -40 dB
//     is already near-inaudible.)
//
//   BASS    (PARAM_BASS):   low-shelf  @ 100 Hz, +/-12 dB (knob 0 = -12, 0.5 = 0,
//                           1 = +12). Shelf slope S = 0.8.
//   MIDDLE  (PARAM_MIDDLE): peaking    @ 650 Hz, +/- 9 dB, Q = 0.7 (broad mid
//                           scoop/boost — the JC "mid" is voiced fairly wide).
//   TREBLE  (PARAM_TREBLE): high-shelf @ 3.5 kHz, +/-12 dB, shelf slope S = 0.8.
//   BRIGHT  (PARAM_BRIGHT): fixed high-shelf +5 dB @ 3 kHz when on (0 dB off).
//     Real bright switches interact with the volume pot (more effect at low
//     volume); we keep it a simple fixed shelf for M5 (documented simplification).
//
//   Centers picked so the three bands are well separated (100 Hz / 650 Hz /
//   3.5 kHz) and land in the ranges the roadmap calls for (bass ~100 Hz, mid
//   ~500-800 Hz, treble ~3-4 kHz). At every knob = 0.5 the stack is flat within
//   a small tolerance (verified by test).
//
// Signal order: bass -> middle -> treble -> bright -> volume.
//
// Smoothing: PARAM_* targets feed OnePoleSmoothers (~8 ms). The four biquads are
// recomputed from the smoothed dB values every kCtrlBlock (32) samples; because
// the dB inputs move only a hair per control tick, the coefficient steps are tiny
// and there is no zipper noise on a knob sweep. Volume is a smoothed per-sample
// linear gain.
// ---------------------------------------------------------------------------

#include "clipper/dsp/AmpModel.h"

#include "clipper/dsp/Biquad.h"
#include "clipper/dsp/OnePoleSmoother.h"

#include <algorithm>
#include <cmath>

namespace clipper::dsp {

namespace {
float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// --- Tone-stack fixed design points ---
constexpr double kBassHz = 100.0;
constexpr double kBassRangeDb = 12.0;   // +/- at knob extremes
constexpr double kBassShelfS = 0.8;

constexpr double kMidHz = 650.0;
constexpr double kMidRangeDb = 9.0;
constexpr double kMidQ = 0.7;

constexpr double kTrebleHz = 3500.0;
constexpr double kTrebleRangeDb = 12.0;
constexpr double kTrebleShelfS = 0.8;

constexpr double kBrightHz = 3000.0;
constexpr double kBrightDb = 5.0;  // when engaged

// --- Volume ---
constexpr float kVolMinDb = -40.0f;
constexpr float kVolMaxDb = 6.0f;
constexpr float kVolFadeKnob = 0.03f;  // below this knob value, fade to silence

// --- Smoothing / control rate ---
constexpr double kSmoothSeconds = 0.008;  // ~8 ms
constexpr int kCtrlBlock = 32;            // recompute biquad coeffs every N samples

float knobToVolumeGain(float knob) {
    const float db = kVolMinDb + (kVolMaxDb - kVolMinDb) * knob;
    float g = std::pow(10.0f, db / 20.0f);
    if (knob < kVolFadeKnob) g *= (knob / kVolFadeKnob);  // -> 0 at knob 0
    return g;
}

// Knob (0..1, flat at 0.5) to a symmetric +/- range in dB.
double knobToDb(float knob, double rangeDb) {
    return (static_cast<double>(knob) - 0.5) * 2.0 * rangeDb;
}
}  // namespace

struct AmpModel::Impl {
    double sampleRate = 44100.0;

    // Smoothed targets: tone gains in dB, bright in dB, volume as linear gain.
    OnePoleSmoother bassDb, midDb, trebleDb, brightDb, volume;

    Biquad bass, mid, treble, bright;
    int ctrlCounter = 0;

    void recomputeCoeffs() {
        bass.setCoeffs(rbj::lowShelf(kBassHz, bassDb.value(), kBassShelfS, sampleRate));
        mid.setCoeffs(rbj::peaking(kMidHz, midDb.value(), kMidQ, sampleRate));
        treble.setCoeffs(rbj::highShelf(kTrebleHz, trebleDb.value(), kTrebleShelfS, sampleRate));
        bright.setCoeffs(rbj::highShelf(kBrightHz, brightDb.value(), 0.9, sampleRate));
    }
};

AmpModel::AmpModel() : impl_(std::make_unique<Impl>()) {}
AmpModel::~AmpModel() = default;

void AmpModel::prepare(double sampleRate, int /*maxBlockSize*/) {
    Impl& d = *impl_;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;

    d.bassDb.prepare(kSmoothSeconds, d.sampleRate);
    d.midDb.prepare(kSmoothSeconds, d.sampleRate);
    d.trebleDb.prepare(kSmoothSeconds, d.sampleRate);
    d.brightDb.prepare(kSmoothSeconds, d.sampleRate);
    d.volume.prepare(kSmoothSeconds, d.sampleRate);

    // Default flat tone, bright off, volume at a sensible unity-ish default.
    d.bassDb.setImmediate(0.0f);
    d.midDb.setImmediate(0.0f);
    d.trebleDb.setImmediate(0.0f);
    d.brightDb.setImmediate(0.0f);
    d.volume.setImmediate(knobToVolumeGain(0.5f));

    d.bass.reset();
    d.mid.reset();
    d.treble.reset();
    d.bright.reset();
    d.ctrlCounter = 0;
    d.recomputeCoeffs();
}

void AmpModel::setParameter(int paramId, float value) {
    Impl& d = *impl_;
    const float knob = clamp01(value);
    switch (paramId) {
        case PARAM_VOLUME:
            d.volume.setTarget(knobToVolumeGain(knob));
            break;
        case PARAM_BASS:
            d.bassDb.setTarget(static_cast<float>(knobToDb(knob, kBassRangeDb)));
            break;
        case PARAM_MIDDLE:
            d.midDb.setTarget(static_cast<float>(knobToDb(knob, kMidRangeDb)));
            break;
        case PARAM_TREBLE:
            d.trebleDb.setTarget(static_cast<float>(knobToDb(knob, kTrebleRangeDb)));
            break;
        case PARAM_BRIGHT:
            d.brightDb.setTarget(knob >= 0.5f ? static_cast<float>(kBrightDb) : 0.0f);
            break;
        default:
            break;
    }
}

void AmpModel::process(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;
    for (int i = 0; i < numFrames; ++i) {
        // Advance the smoothers every sample; recompute coefficients at the
        // control rate from the current smoothed dB values.
        d.bassDb.next();
        d.midDb.next();
        d.trebleDb.next();
        d.brightDb.next();
        if (d.ctrlCounter == 0) d.recomputeCoeffs();
        if (++d.ctrlCounter >= kCtrlBlock) d.ctrlCounter = 0;

        float x = in[i];
        x = d.bass.process(x);
        x = d.mid.process(x);
        x = d.treble.process(x);
        x = d.bright.process(x);
        out[i] = x * d.volume.next();
    }
}

}  // namespace clipper::dsp
