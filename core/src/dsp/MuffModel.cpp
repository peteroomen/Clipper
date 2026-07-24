// Clipper — MuffModel (ROADMAP v1.1 item 4). See MuffModel.h for the topology
// overview and the documented "ram's head"-family canon; BjtStage.{h,cpp} for the
// per-stage Ebers-Moll device model + nodal Newton. This file composes the four
// stages + the mid-scoop tone stack and does the level/knob mapping.
//
// ---------------------------------------------------------------------------
// Signal path (all inside ONE Oversampler, so the four solves + the tone stack
// stay sample-aligned at the oversampled rate):
//
//   x → ×inputDrive → [Q1] → ×sustain → [Q2 diodes] → [Q3 diodes] →
//     → toneStack → [Q4] → ×(outputTrim·volume) → downsample → out
//
// SUSTAIN is a level pot between the input boost and the clip stages (how hard the
// diodes are slammed = how much fuzz + sustain). The two clip stages' diodes clip
// near idle (BjtStage.h), so the pair compresses the signal into the wall of
// sustain. VOLUME is a plain output gain (a Muff makes far more than unity).
// ---------------------------------------------------------------------------

#include "clipper/dsp/MuffModel.h"

#include "clipper/dsp/BjtStage.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/Oversampler.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace clipper::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586;
constexpr double kSmoothSeconds = 0.005;  // 5 ms glide (shared family value)

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float onePoleCoeff(double cutoffHz, double sampleRate) {
    const double a = 1.0 - std::exp(-kTwoPi * cutoffHz / sampleRate);
    return static_cast<float>(std::clamp(a, 0.0, 1.0));
}

// --- Level canon -----------------------------------------------------------
// Fixed input-drive gain into Q1: a guitar DI (0.05–0.3 V) barely tickles a 9 V
// stage, and the Muff has enormous front-end gain, so lift it hard. SUSTAIN then
// sets how much of Q1's swing hits the clip stages.
constexpr double kInputDrive = 12.0;
// SUSTAIN pot: a level divider into Q2. Even at 0 the clip stages see enough to
// grind (a Muff has no clean setting) — floor 0.06, up to 1.0 at max.
constexpr float kSustainFloor = 0.06f;
// Output trim: the recovery stage (Q4) collector AC is a few volts; scale so max
// SUSTAIN peaks near ~1.0 with VOLUME at unity, then VOLUME (0..1) rides on top.
constexpr double kOutputTrim = 0.32;
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
    // Low-pass leg (bass/dark side).
    lpState_ += static_cast<float>(aLp_) * (x - lpState_);
    // High-pass leg = x − LP_hp(x) (treble/buzz side).
    hpLpState_ += static_cast<float>(aHp_) * (x - hpLpState_);
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
        // like the OverdriveEngine's control-rate sampling.
        float sus = sustain.value();
        float vol = volume.value();
        for (int i = 0; i < numFrames; ++i) { sus = sustain.next(); vol = volume.next(); }
        const float outGain = static_cast<float>(kOutputTrim) * vol;

        os.upsample(in, numFrames);
        float* w = os.buffer();
        const int osN = os.bufferLength();
        for (int i = 0; i < osN; ++i) {
            float x = static_cast<float>(kInputDrive) * w[i];
            x = q1.processSample(x);       // input boost
            x *= sus;                      // SUSTAIN pot
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
            impl_->sustain.setTarget(kSustainFloor + (1.0f - kSustainFloor) * knob);
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
