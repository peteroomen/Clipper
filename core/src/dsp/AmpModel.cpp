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
#include "clipper/dsp/ChorusModel.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/ParamGuard.h"
#include "clipper/dsp/ReverbModel.h"

#include <algorithm>
#include <cmath>

namespace clipper::dsp {

namespace {
// Knob clamping is clampParam01 from ParamGuard.h (this file's old private
// clamp01 let NaN through, and one NaN latched permanently in the biquad /
// smoother state — audit finding 1).

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

// --- Volume (M6.5 re-stage: an audio-taper MASTER, unity-ceilinged) ---
// History: M5 (linear-in-dB -40..+6) put the default knob (0.4) at -21.6 dB —
// the whole rig came out ~20 dB too quiet. M6.1 fixed that with a loud-biased
// quartic taper whose top was +6 dB (0.4 == unity) — but that pushed the CLEAN
// (pedal-bypassed) chain output well past full scale at realistic input levels,
// so the worklet's soft limiter clipped every cycle: a clean amp that fizzes.
//
// M6.5: the amp is a CLEAN platform, so its ceiling is UNITY (clean full scale),
// not +6 dB. Same quartic SHAPE (span/feel unchanged), ceiling pulled to 0 dB:
//   db(knob) = kVolMaxDb - kVolTaperDb*(1-knob)^4,  kVolMaxDb = 0.
// The DEFAULT 0.4 now sits at ~-6 dB (real headroom below the 0.97 limiter), and
// the top of the knob is clean full scale (unity) — the amp never soft-clips of
// its own accord; "louder than clean full scale" is the system/master's job. The
// bottom third stays a usable quiet range that fades to true silence at 0. This
// keeps the M6.1 loudness win (still ~+16 dB vs M5 at default, and the knob still
// spans ~+6 dB of clean boost above the default) while making the clean path
// measurably clean (testCleanPathTHD). The taper SPAN is identical, so the tone/
// volume-sweep tests (which assert on span, not absolute level) are unchanged.
constexpr float kVolMaxDb = 0.0f;      // knob 1.0 = unity = clean full scale
constexpr float kVolTaperDb = 46.0f;   // taper depth: db at knob 0 = 0 - 46 = -46
constexpr float kVolFadeKnob = 0.03f;  // below this knob value, fade to true silence

// --- Smoothing / control rate ---
constexpr double kSmoothSeconds = 0.008;  // ~8 ms
constexpr int kCtrlBlock = 32;            // recompute biquad coeffs every N samples

float knobToVolumeGain(float knob) {
    // Loud-biased quartic audio taper (see the constants above). t=(1-knob)^4.
    const float t = 1.0f - knob;
    const float taper = (t * t) * (t * t);
    const float db = kVolMaxDb - kVolTaperDb * taper;
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

    // M6.7: the spring-flavored reverb, in the JC-120 spring-tank position —
    // applied to the post-volume mono voice BEFORE the chorus split, so the tail
    // blooms in stereo through the chorus. AmpModel owns it and routes PARAM_REVERB
    // here. Default mix 0 => bit-exact passthrough.
    ReverbModel reverb;

    // M6.3: the chorus/vibrato stereo split, fed the amp's post-volume mono
    // voice. AmpModel owns it and routes PARAM_CHORUS_* here.
    ChorusModel chorus;

    // Compute the mono voice (tone stack + volume) sample-by-sample into `out`.
    // Shared by process() and processStereo(). in and out may alias.
    void processMonoVoice(const float* in, float* out, int numFrames) {
        for (int i = 0; i < numFrames; ++i) {
            bassDb.next();
            midDb.next();
            trebleDb.next();
            brightDb.next();
            if (ctrlCounter == 0) recomputeCoeffs();
            if (++ctrlCounter >= kCtrlBlock) ctrlCounter = 0;

            float x = in[i];
            x = bass.process(x);
            x = mid.process(x);
            x = treble.process(x);
            x = bright.process(x);
            out[i] = x * volume.next();
        }
    }

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

    // Reverb prepared to the same rate; default mix 0 (dry) so a fresh amp is a
    // bit-exact passthrough through the reverb block.
    d.reverb.prepare(d.sampleRate);
    d.reverb.setMix(0.0f);

    // Chorus prepared to the same rate; defaults (mode off, mid speed, mid depth)
    // are applied here so an engaged chorus opens on a sensible voice.
    d.chorus.prepare(d.sampleRate);
    d.chorus.setSpeed(0.3f);
    d.chorus.setDepth(0.5f);
    d.chorus.setMode(ChorusModel::MODE_OFF);
}

void AmpModel::reset() {
    Impl& d = *impl_;
    // Snap the smoothers onto their targets first (a poisoned smoother value can
    // never recover on its own: value += coeff*(target - value) stays NaN), then
    // recompute the coefficients from the now-clean dB values before clearing the
    // biquad histories, so the filters restart consistent with the knobs.
    d.bassDb.reset();
    d.midDb.reset();
    d.trebleDb.reset();
    d.brightDb.reset();
    d.volume.reset();
    d.recomputeCoeffs();
    d.bass.reset();
    d.mid.reset();
    d.treble.reset();
    d.bright.reset();
    d.ctrlCounter = 0;
    d.reverb.reset();
    d.chorus.reset();
}

void AmpModel::setParameter(int paramId, float value) {
    Impl& d = *impl_;
    // Chorus MODE carries an integer 0/1/2, so it must NOT be clamped to 0..1
    // like the knobs; handle it before the clamp. std::lround(NaN) is unspecified,
    // so it goes through paramToInt (non-finite -> MODE_OFF).
    if (paramId == PARAM_CHORUS_MODE) {
        d.chorus.setMode(paramToInt(value, ChorusModel::MODE_OFF));
        return;
    }
    const float knob = clampParam01(value);
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
        case PARAM_CHORUS_SPEED:
            d.chorus.setSpeed(knob);
            break;
        case PARAM_CHORUS_DEPTH:
            d.chorus.setDepth(knob);
            break;
        case PARAM_REVERB:
            d.reverb.setMix(knob);
            break;
        default:
            break;
    }
}

void AmpModel::process(const float* in, float* out, int numFrames) {
    impl_->processMonoVoice(in, out, numFrames);
}

void AmpModel::processStereo(const float* in, float* outL, float* outR, int numFrames) {
    Impl& d = *impl_;
    // Tone stack + volume into outL as scratch (the mono voice), then the spring
    // reverb IN PLACE on that mono voice (JC-120 spring-tank position: after the
    // preamp, BEFORE the split), then split into the stereo pair so the wet tail
    // blooms in stereo through the chorus. Both ReverbModel (reads the dry sample
    // before writing) and ChorusModel (reads before overwriting) tolerate in==out,
    // so no extra buffer is needed.
    d.processMonoVoice(in, outL, numFrames);
    d.reverb.process(outL, outL, numFrames);
    d.chorus.processStereo(outL, outL, outR, numFrames);
}

}  // namespace clipper::dsp
