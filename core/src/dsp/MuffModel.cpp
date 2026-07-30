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
//     → toneStack → [Q4] → dcBlock → ×(outputTrim·volume) → downsample → out
//
// SUSTAIN is a full-range audio-taper attenuator BETWEEN the clean input boost (Q1) and
// the clip stages: it sets how hard the high-gain Q2→Q3 cascade is slammed = how much
// fuzz + sustain + compression. The two clip stages' diodes clip near idle (BjtStage.h),
// so the pair compresses into the wall of sustain WHEN driven — but because Q1 is clean
// and the pot is a true full-range attenuator (down to a −54 dB floor), rolling SUSTAIN
// down leaves the cascade near-linear: dynamics return and a quiet signal (single-coil
// hum) is NOT compressed up. See the docs §24 field-fix postmortem. VOLUME is a plain
// output gain (a Muff makes far more than unity).
//
// The `dcBlock` in that path landed 2026-07-25 (audit finding 16, DC half — docs §37,
// ADR 009); see kOutCouplingHz below. The clip stages' SERIES BASE RESISTORS (the
// finding's other half, and the max-sustain blowout's root cause) landed 2026-07-31 —
// docs §49 and the Rs comment in configureStages().
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
// SUSTAIN taper: an audio pot wired as a full-range input attenuator (a real
// Big-Muff SUSTAIN is a 100 kA pot; at minimum it nearly grounds the clipper input).
// PIECEWISE decibel-linear, with the break at the shipped default (0.6):
//   knob ≥ 0.6: kClipDriveMax · 10^((kSustainFloorDb/20)·(1−knob)) — the pre-2026-07-30
//               law VERBATIM, so the default and everything above it is bit-identical
//               by construction (the muff_twin golden renders at 0.6).
//   knob < 0.6: decibel-linear from kSustainMinDb (knob 0) up to the −21.6 dB the
//               upper law gives at the break.
// Why the −54 dB floor had to deepen (docs §43, field report 2026-07-30): the clip
// stages' clean window ends at ~1–3 mV at the base (the diodes conduct at idle —
// BjtStage.h), so a −54 dB floor still put ~11.5 mV of a 0.1 V pluck into Q2 at
// knob ZERO — 4–10× past clean. Measured: the whole travel moved output < 2.2 dB
// and THD never dropped below 27 %; knob 0.14 was the HOTTEST point of the sweep
// (Q2/Q3's gain-expansion hump, +6.7 dB at ~30 mV, parks exactly there). At −84 dB
// the bottom of the knob is a tame, dynamic fuzz (measured 1.9 % THD / −41.5 dBFS at
// knob 0, 14.2 % / −26.0 at 0.14, 220 Hz 0.1 V input, 48 kHz) and still leaks — a
// real pot's escape hatch, deeper. (−80 measured 3.5 % / 20.5 %: the 0.14 figure sat
// above the ~15 % real-Muff sustain-1-2 feel the field report asked for; −84 is the
// value chosen by that measurement.)
// A real audio pot is two resistive segments meeting mid-rotation, so a slope break
// is truer to the part than a single exponent; landing it on the default is what
// kept the golden untouched in §43.
// §49 RE-DERIVATION (2026-07-31): the clip stages gained their schematic 10 k series
// base resistors (Config::Rs below), which widens their clean window ~10× — the −84 dB
// floor §43 chose against the UNPROTECTED stages then left the knob bottom nearly
// silent-clean. Re-derived against the SAME §43 player bars on the corrected circuit:
// −54 collapses the level authority again (0.14 only 2 dB under the wall), −84 measures
// 2.8 % at 0.14 (too polite), −70 lands it — knob 0.14 = −25.9 dBFS / 10.7 % THD (tame
// fuzz), knob 0 = −38 dBFS / 2.6 % (the escape hatch), wall from ~0.4, max 39.8 %
// (articulate — the fundamental survives now, see the Rs comment in configureStages).
// The ≥ 0.6 upper law is unchanged; the golden moves ANYWAY because the clip stages
// changed, so the §43 bit-pin no longer applies (bless held for the owner).
constexpr double kSustainFloorDb = -54.0;
constexpr double kSustainMinDb = -70.0;
constexpr double kSustainBreak = 0.6;
// Output trim: the recovery stage (Q4) collector AC is a few volts; scale so the default
// (SUSTAIN 0.6 / VOLUME 0.6) peaks ~1.0 V, then VOLUME (0..1) rides on top.
//
constexpr double kOutputTrim = 0.40;

// --- The output coupling cap (audit finding 16, DC half — docs §37, ADR 009) ---------
// This slice adds ONE of the two components finding 16 named. The other — a series base
// resistor on the clip stages, which is what restores the missing BASS — is deliberately
// held back to its own slice, because choosing its value means departing from the
// schematic to compensate for a base-node impedance this model gets wrong (measured
// ~1.8 k against the real stage's ~4 k). See the XFAIL kXfMuffBass in
// core/tests/test_muff_model.cpp: the bass defect is still measured and still named.
//
// The OUTPUT coupling cap, which the model simply did not have. Q4's collector feeds
// 0.1 uF into the 100 k VOLUME pot: f = 1/(2*pi*100k*0.1u) = 15.92 Hz. The siblings all
// carry dcBlockHz = 12.0 for the same reason (SdModel.cpp: "the asymmetric clip produces
// DC"); this one is derived from the Muff's own two components rather than copied.
// Placed after Q4 and BEFORE the VOLUME multiply, because in the pedal the cap precedes
// the pot, and inside the oversampled domain so it also blocks the DC the four
// asymmetric stages rectify before that DC reaches the decimator.
constexpr double kOutCouplingF = 100.0e-9;
constexpr double kVolumePotOhms = 100.0e3;
constexpr double kOutCouplingHz = 1.0 / (kTwoPi * kVolumePotOhms * kOutCouplingF);

// The SUSTAIN audio taper: knob (0..1) -> drive multiplier into Q2 (floor..max).
// Piecewise in dB with the break at the shipped default — see the constant block above.
double sustainDrive(float knob01) {
    const double k = static_cast<double>(clampParam01(knob01));
    if (k >= kSustainBreak)
        return kClipDriveMax * std::pow(10.0, (kSustainFloorDb / 20.0) * (1.0 - k));
    const double dbAtBreak = kSustainFloorDb * (1.0 - kSustainBreak);
    const double db = kSustainMinDb + (dbAtBreak - kSustainMinDb) * (k / kSustainBreak);
    return kClipDriveMax * std::pow(10.0, db / 20.0);
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

    // Output coupling cap (0.1 uF into the 100 k VOLUME pot, kOutCouplingHz). One-pole
    // DC blocker at the OVERSAMPLED rate, matching the siblings' OverdriveEngine form:
    //   y = x - x1 + R*y1,  R = exp(-2*pi*f/rate).
    // dcY1_ is recursive, so it carries the Denormal.h guard (WASM has no flush-to-zero).
    double dcR = 0.0;
    float dcX1 = 0.0f, dcY1 = 0.0f;

    void configureStages() {
        BjtStage::Config base;  // Q1/Q4: no diodes (boost / recovery)
        BjtStage::Config c1 = base;
        BjtStage::Config c4 = base;
        q1.configure(c1);
        q4.configure(c4);
        BjtStage::Config clip = base;
        clip.diodes.present = true;  // Q2/Q3: the two clipping stages
        // The schematic's 10 k SERIES BASE RESISTOR on each clip stage (docs §49,
        // ADR 009 / audit finding 16's bass half — and, measured 2026-07-31, the
        // max-sustain blowout's root cause). Without it the source drives the base
        // node directly: the feedback diodes cannot form their limiting divider, the
        // stage blows past the ±0.6 V clamp at high drive (collector dragged to
        // 6.7 V, phase +30°), each stage preferentially amplifies the PREVIOUS
        // stage's distortion over the note (THD > 100 %, the fundamental partially
        // CANCELLED at 110 Hz), and the 470 pF Miller cap has no impedance to work
        // against so the ~1.2 kHz anti-harshness low-pass never forms. With Rs the
        // clamp self-limits (output pinned ~0.65 V at ANY drive), max-sustain THD
        // measures 152 -> 41 % at 220 Hz and 1177 -> 54 % at 110 Hz, the wall
        // compresses flat and monotonically, and Newton converges in fewer
        // iterations. Q1/Q4 keep Rs = 0: their networks are part of ADR 009's
        // DC-blocked-diode follow-up, not this slice.
        clip.Rs = 10.0e3;
        BjtStage::Config c2 = clip;
        BjtStage::Config c3 = clip;
        q2.configure(c2);
        q3.configure(c3);
    }

    void repreparePerRate() {
        os.setFactor(osFactor);
        const double osRate = sampleRate * os.factor();
        q1.prepare(osRate);
        q2.prepare(osRate);
        q3.prepare(osRate);
        q4.prepare(osRate);
        tone.prepare(osRate);
        dcR = std::exp(-kTwoPi * kOutCouplingHz / osRate);
        dcX1 = 0.0f;
        dcY1 = 0.0f;
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
            // Output coupling cap (0.1 uF into the 100 k VOLUME pot). Before the VOLUME
            // multiply, as in the circuit, and before downsample so the DC the four
            // asymmetric stages rectify never reaches the decimator.
            const float y = x - dcX1 + static_cast<float>(dcR) * dcY1;
            dcX1 = x;
            dcY1 = flushDenormal(y);
            w[i] = y * outGain;            // VOLUME
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
    // The output coupling cap's state is recursive: a poisoned dcY1 never recovers on its
    // own (audit finding 1's rule — every recursive state belongs in reset()).
    d.dcX1 = 0.0f;
    d.dcY1 = 0.0f;
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
    // The output coupling cap's coefficient is rate-dependent too (docs §37).
    impl_->dcR = std::exp(-kTwoPi * kOutCouplingHz / osRate);
    impl_->dcX1 = 0.0f;
    impl_->dcY1 = 0.0f;
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
