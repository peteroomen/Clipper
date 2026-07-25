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
// The `dcBlock` in that path and the SERIES BASE RESISTORS on all four stages landed
// together on 2026-07-25 (audit finding 16 — docs §37, ADR 009). See kSeriesBaseR* and
// kOutCouplingHz below for the component values and the measurements behind them.
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
// Output trim: the recovery stage (Q4) collector AC is a few volts; scale so the default
// (SUSTAIN 0.6 / VOLUME 0.6) peaks ~1.0 V, then VOLUME (0..1) rides on top.
//
// 0.40 -> 0.45 on 2026-07-25 (docs §37). The series base resistors (kSeriesBaseR* below)
// are a divider as well as a corner-setter, so the cascade lost ~15 dB per clip stage of
// small-signal gain and the default peak fell 1.2 V -> 0.889 V. This is a LEVEL TRIM only.
// It does not go all the way back to 1.2 V, and the ceiling — not the old number — is why:
// the pedal now HAS bass, so its hottest knob corner (TONE 0, the all-low-pass leg, on a
// low-heavy pluck) rose relative to the default. Restoring 1.2 V would have put that corner
// at 2.16 V, over the 2.0 V pedal peak ceiling that test_player_expectations.cpp's A1 block
// asserts. The bar was NOT moved to accommodate this; the trim was chosen to keep ~10 %
// headroom under it. Measured at 0.45: default sine peak 1.00 V, default pluck peak 1.13 V,
// TONE 0 pluck peak 1.80 V (ceiling 2.0 V).
constexpr double kOutputTrim = 0.45;

// --- Coupling network canon (audit finding 16, fixed 2026-07-25 — docs §37, ADR 009) ---
// Until that slice every BjtStage drove its Cin = 100 nF from an IDEAL voltage source
// straight onto the base, so each coupling corner was set by the base-node SHUNT impedance
// alone. MEASURED per stage at the 4x rate, small-signal (a first-order corner back-solved
// from the 82.4 Hz / 1 kHz ratio):
//
//   plain stage (Q1/Q4)      base node ~19.6 k  ->  corner   81 Hz
//   CLIP  stage (Q2/Q3)      base node ~1.8 k   ->  corner  898 Hz   <-- the real defect
//
// The clip stages' diodes conduct at idle BY DESIGN (BjtStage.h), and that shunts
// base<->collector down to ~1.8 k. Two of those in cascade are what put the guitar's low E
// (82.4 Hz) 41 dB below 1 kHz: -18.2 dB each, against -2.9 dB each from Q1/Q4.
//
// Fix: the series resistance the real pedal's coupling networks all have, on the two stages
// where the model's corner is nowhere near the real 15-50 Hz band. 47 k against the
// measured 1.8 k base node puts BOTH clip corners at 31.5 Hz, inside that band.
constexpr double kSeriesBaseRQ2 = 47.0e3;
constexpr double kSeriesBaseRQ3 = 47.0e3;
//
// WHY 47 k AND NOT THE SCHEMATIC'S 10 k / 100 k — measured, per stage, at 4 kHz and 82 Hz:
//
//   Rb  10 k -> corner 135 Hz (still -5.6 dB at the low E, ABOVE the real band)
//   Rb  47 k -> corner  31.5 Hz, -0.6 dB at the low E, 15 dB of divider loss
//   Rb 100 k -> corner  13 Hz (BELOW the band), 33 dB of divider loss; with both clip
//               stages at 100 k the SUSTAIN knob turns into a threshold switch —
//               measured peak at SUSTAIN 0.3 collapses from 0.51 V to 0.05 V.
//
// Rb is a divider against the base node as well as a corner-setter, so it is chosen to land
// the CORNER where the real circuit's is, using this model's own measured base impedance.
// That the impedance is 1.8 k rather than the real stage's ~4 k is the idle-diode model's
// doing and is recorded, not compensated for elsewhere.
//
// WHY Q1/Q4 KEEP Rb = 0 (their 81 Hz corners are NOT fixed here). Rb also works against the
// Miller-multiplied feedback cap, and a plain stage's gain of ~26 turns Cf = 470 pF into
// ~12 nF at the base. Measured on one plain stage, 4 kHz relative to 1 kHz:
//
//   Cf = 470 pF: Rb 0 -> -0.07 dB | Rb 10 k -> -5.63 dB | Rb 100 k -> -9.57 dB
//   Cf = 0     : Rb 0 -> +0.03 dB | Rb 10 k -> +0.01 dB | Rb 100 k ->  0.00 dB
//
// The treble cost is ENTIRELY the feedback cap, and a real Big Muff carries the 470 pF only
// on its two CLIPPING stages — this model gives all four the clip Config's Cf, which is
// pre-existing canon and is deliberately NOT relitigated here. Whole-pedal measurement:
// Rb = 10 k on Q1 and Q4 buys 4.3 dB at the low E and spends 11 dB at 4 kHz. The clip
// stages are immune (1.8 k base node, so Miller barely shows: -0.6 dB at 4 kHz at 100 k).
// So the follow-up slice is "give Q1/Q4 their real feedback network, THEN give them Rb".
constexpr double kSeriesBaseRQ1 = 0.0;
constexpr double kSeriesBaseRQ4 = 0.0;

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

    // Output coupling cap (0.1 uF into the 100 k VOLUME pot, kOutCouplingHz). One-pole
    // DC blocker at the OVERSAMPLED rate, matching the siblings' OverdriveEngine form:
    //   y = x - x1 + R*y1,  R = exp(-2*pi*f/rate).
    // dcY1_ is recursive, so it carries the Denormal.h guard (WASM has no flush-to-zero).
    double dcR = 0.0;
    float dcX1 = 0.0f, dcY1 = 0.0f;

    void configureStages() {
        BjtStage::Config base;  // Q1/Q4: no diodes (boost / recovery)
        BjtStage::Config c1 = base; c1.Rb = kSeriesBaseRQ1;
        BjtStage::Config c4 = base; c4.Rb = kSeriesBaseRQ4;
        q1.configure(c1);
        q4.configure(c4);
        BjtStage::Config clip = base;
        clip.diodes.present = true;  // Q2/Q3: the two clipping stages
        BjtStage::Config c2 = clip; c2.Rb = kSeriesBaseRQ2;
        BjtStage::Config c3 = clip; c3.Rb = kSeriesBaseRQ3;
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
