// Clipper — MuffModel (ROADMAP v1.1 item 4). See MuffModel.h for the topology
// overview and the documented "ram's head"-family canon; BjtStage.{h,cpp} for the
// per-stage Ebers-Moll device model + nodal Newton. This file composes the four
// stages + the mid-scoop tone stack and does the level/knob mapping.
//
// ---------------------------------------------------------------------------
// Signal path (all inside ONE Oversampler, so the four solves + the tone stack
// stay sample-aligned at the oversampled rate):
//
//   x → ×inputDrive → [Q1 CLEAN boost] → ×sustainDrive → [Q2 diodes] → [Q3 diodes] →
//     → toneStack → [Q4] → ×(outputTrim·volume) → downsample → out
//
// SUSTAIN is a full-range audio-taper attenuator BETWEEN the clean input boost (Q1) and
// the clip stages: it sets how hard the high-gain Q2→Q3 cascade is slammed = how much
// fuzz + sustain + compression. The two clip stages' diodes clip near idle (BjtStage.h),
// so the pair compresses into the wall of sustain WHEN driven — but because Q1 is clean
// and the pot is a true full-range attenuator (down to a −54 dB floor), rolling SUSTAIN
// down leaves the cascade near-linear: dynamics return and a quiet signal (single-coil
// hum) is NOT compressed up. See the docs §24 field-fix postmortem. VOLUME is a plain
// output gain (a Muff makes far more than unity).
// ---------------------------------------------------------------------------

#include "clipper/dsp/MuffModel.h"

#include "clipper/dsp/BjtStage.h"
#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/ParamGuard.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace clipper::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586;
constexpr double kSmoothSeconds = 0.005;  // 5 ms glide (shared family value)

// NaN-rejecting knob clamp (ParamGuard.h) — audit finding 1.
float clamp01(float v) { return clampParam01(v); }

float onePoleCoeff(double cutoffHz, double sampleRate) {
    const double a = 1.0 - std::exp(-kTwoPi * cutoffHz / sampleRate);
    return static_cast<float>(std::clamp(a, 0.0, 1.0));
}

// --- Level canon (see docs §24 field-fix postmortem) -----------------------
// Q1 is a CLEAN input BOOSTER, not a clipper. A guitar DI (0.05–0.3 V) must leave
// Q1 in its linear region (Q1 clips above ~0.05 V at its base) so that quiet input
// — single-coil hum especially — passes Q1 undistorted and the SUSTAIN pot after it
// genuinely governs the clipping. The pre-fix value (12.0) drove Q1 to a rail-clipped
// square for ANY input including a −40 dBFS hum, so the pot could not clean anything
// up: a −40 dBFS hum came out at −11 dBFS (the field report). 0.5 keeps Q1 clean for
// hum/soft picking and only lets loud playing tickle it.
constexpr double kInputDrive = 0.5;
// SUSTAIN drive into Q2 AT MAX (knob = 1). The clipping/compression is developed
// here, AFTER the pot, by the high-gain Q2→Q3 cascade — so the pot is a true
// full-range attenuator into the first clipper. ~6× puts several volts into Q2 at
// max for a healthy wall while Q1 stays clean.
constexpr double kClipDriveMax = 6.0;
// SUSTAIN taper: an honest audio (decibel-linear, ≈ log) pot, floor..max. A real
// Big-Muff SUSTAIN is a 100 kA (audio) pot wired as a full-range input attenuator;
// at minimum it nearly grounds the clipper input. We model that as a decibel-linear
// law susGain(knob) = kClipDriveMax · 10^((kSustainFloorDb/20)·(1−knob)); knob 0 →
// −54 dB below max (≈0.012, a near-off "escape hatch" that is quiet but not silent —
// a real pot leaks), knob 1 → kClipDriveMax. The pre-fix taper was LINEAR with a
// hot 0.06 (−24 dB) floor, so min sustain still slammed the clippers.
constexpr double kSustainFloorDb = -54.0;
// Output trim: the recovery stage (Q4) collector AC is a few volts; scale so the
// default (SUSTAIN 0.6 / VOLUME 0.6) peaks ~1.2 V, then VOLUME (0..1) rides on top.
constexpr double kOutputTrim = 0.40;

// The SUSTAIN audio taper: knob (0..1) -> drive multiplier into Q2 (floor..max).
double sustainDrive(float knob01) {
    const double k = static_cast<double>(clampParam01(knob01));
    return kClipDriveMax * std::pow(10.0, (kSustainFloorDb / 20.0) * (1.0 - k));
}
}  // namespace

// --- MuffToneStack ---------------------------------------------------------

void MuffToneStack::prepare(double sampleRate) {
    aLp_ = onePoleCoeff(kFlpHz, sampleRate);
    aHp_ = onePoleCoeff(kFhpHz, sampleRate);
    reset();
}
void MuffToneStack::reset() { lpState_ = 0.0f; hpLpState_ = 0.0f; }
void MuffToneStack::setTone(float tone01) { tone_ = clamp01(tone01); }

float MuffToneStack::processSample(float x) {
    // Low-pass leg (bass/dark side). Both one-pole states are flushed through the
    // anti-denormal guard (Denormal.h) so a silent tail can never park them in the
    // float subnormal range.
    lpState_ = flushDenormal(lpState_ + static_cast<float>(aLp_) * (x - lpState_));
    // High-pass leg = x − LP_hp(x) (treble/buzz side).
    hpLpState_ = flushDenormal(hpLpState_ + static_cast<float>(aHp_) * (x - hpLpState_));
    const float hp = x - hpLpState_;
    return (1.0f - tone_) * lpState_ + tone_ * hp;
}

double MuffToneStack::analyticMagDb(double freqHz, double tone) {
    const double wLp = kTwoPi * kFlpHz, wHp = kTwoPi * kFhpHz;
    const double w = kTwoPi * freqHz;
    const std::complex<double> jw(0.0, w);
    const std::complex<double> hLp = 1.0 / (1.0 + jw / wLp);
    const std::complex<double> hHp = (jw / wHp) / (1.0 + jw / wHp);
    const std::complex<double> h = (1.0 - tone) * hLp + tone * hHp;
    return 20.0 * std::log10(std::abs(h) + 1e-12);
}

// --- MuffModel::Impl -------------------------------------------------------

struct MuffModel::Impl {
    // The four transistor stages Q1..Q4. Q2/Q3 carry the clip diodes.
    BjtStage q1, q2, q3, q4;
    MuffToneStack tone;
    Oversampler os;

    double sampleRate = 44100.0;
    int maxBlockSize = 128;
    int osFactor = 4;
    int clipMode = CLIP_OVERSAMPLED;

    OnePoleSmoother sustain;  // level into the clip stages
    OnePoleSmoother volume;   // output level

    void configureStages() {
        BjtStage::Config base;  // Q1/Q4: no diodes (boost / recovery)
        q1.configure(base);
        q4.configure(base);
        BjtStage::Config clip = base;
        clip.diodes.present = true;  // Q2/Q3: the two clipping stages
        q2.configure(clip);
        q3.configure(clip);
    }

    void repreparePerRate() {
        os.setFactor(osFactor);
        const double osRate = sampleRate * os.factor();
        q1.prepare(osRate);
        q2.prepare(osRate);
        q3.prepare(osRate);
        q4.prepare(osRate);
        tone.prepare(osRate);
    }

    void prepare(double sr, int mbs) {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        maxBlockSize = mbs > 0 ? mbs : 128;
        sustain.prepare(kSmoothSeconds, sampleRate);
        volume.prepare(kSmoothSeconds, sampleRate);
        configureStages();
        os.prepare(maxBlockSize);
        repreparePerRate();
    }

    void processChunk(const float* in, float* out, int numFrames) {
        // Sample control params once per chunk (5 ms glide makes it click-free),
        // like the OverdriveEngine's control-rate sampling. The SUSTAIN smoother
        // rides the raw knob (0..1); the audio taper is applied after smoothing.
        float susKnob = sustain.value();
        float vol = volume.value();
        for (int i = 0; i < numFrames; ++i) { susKnob = sustain.next(); vol = volume.next(); }
        const float susDrive = static_cast<float>(sustainDrive(susKnob));
        const float outGain = static_cast<float>(kOutputTrim) * vol;

        os.upsample(in, numFrames);
        float* w = os.buffer();
        const int osN = os.bufferLength();
        for (int i = 0; i < osN; ++i) {
            float x = static_cast<float>(kInputDrive) * w[i];
            x = q1.processSample(x);       // input boost (CLEAN — see kInputDrive)
            x *= susDrive;                 // SUSTAIN pot: full-range attenuator into Q2
            x = q2.processSample(x);       // clip stage 1 (diodes)
            x = q3.processSample(x);       // clip stage 2 (diodes)
            x = tone.processSample(x);     // mid-scoop tone stack
            x = q4.processSample(x);       // recovery stage
            w[i] = x * outGain;            // VOLUME
        }
        os.downsample(out, numFrames);
    }
};

MuffModel::MuffModel() : impl_(std::make_unique<Impl>()) {}
MuffModel::~MuffModel() = default;

void MuffModel::prepare(double sampleRate, int maxBlockSize) {
    impl_->prepare(sampleRate, maxBlockSize);
}

void MuffModel::reset() {
    Impl& d = *impl_;
    d.sustain.reset();  // a poisoned smoother value never recovers on its own
    d.volume.reset();
    d.q1.reset();
    d.q2.reset();
    d.q3.reset();
    d.q4.reset();
    d.tone.reset();
    d.os.reset();
}

void MuffModel::setOversampling(int factor) {
    impl_->osFactor = factor;
    impl_->repreparePerRate();
}
int MuffModel::oversampling() const { return impl_->os.factor(); }
int MuffModel::latencySamples() const { return impl_->os.latencySamples(); }

void MuffModel::setClipMode(int mode) {
    // NAIVE forces 1× (aliasing A/B); OVERSAMPLED restores the shipped factor.
    impl_->clipMode = (mode == CLIP_NAIVE) ? CLIP_NAIVE : CLIP_OVERSAMPLED;
    const int f = (impl_->clipMode == CLIP_NAIVE) ? 1 : impl_->osFactor;
    impl_->os.setFactor(f);
    const double osRate = impl_->sampleRate * impl_->os.factor();
    impl_->q1.prepare(osRate);
    impl_->q2.prepare(osRate);
    impl_->q3.prepare(osRate);
    impl_->q4.prepare(osRate);
    impl_->tone.prepare(osRate);
}
int MuffModel::clipMode() const { return impl_->clipMode; }

void MuffModel::setParameter(int paramId, float value) {
    const float knob = clamp01(value);
    switch (paramId) {
        case PARAM_SUSTAIN:
            // Smooth the raw knob; the audio taper (sustainDrive) is applied per chunk.
            impl_->sustain.setTarget(knob);
            break;
        case PARAM_TONE:
            impl_->tone.setTone(knob);
            break;
        case PARAM_VOLUME:
            impl_->volume.setTarget(knob);
            break;
        default:
            break;
    }
}

void MuffModel::process(const float* in, float* out, int numFrames) {
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(impl_->maxBlockSize, numFrames - off);
        impl_->processChunk(in + off, out + off, n);
        off += n;
    }
}

const BjtStage& MuffModel::stage(int index) const {
    switch (index) {
        case 0: return impl_->q1;
        case 1: return impl_->q2;
        case 2: return impl_->q3;
        default: return impl_->q4;
    }
}
double MuffModel::inputDriveGain() { return kInputDrive; }
double MuffModel::outputTrim() { return kOutputTrim; }

}  // namespace clipper::dsp
