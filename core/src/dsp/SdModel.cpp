// Clipper — Boss SD-1 Super Overdrive model (M8). See SdModel.h for the overview.
//
// ---------------------------------------------------------------------------
// Circuit values, targets DERIVED analytically, and knob mappings. Circuit-
// INFORMED, not SPICE-accurate (per roadmap scope). Reference: 1.0f == 1.0 V.
// ---------------------------------------------------------------------------
//
// THE DEFINING SD-1-vs-RAT DIFFERENCE. The RAT clips with a diode pair SHUNTED
// to ground after a gain stage — a hard, symmetric clamp (odd harmonics). The
// SD-1 puts the diodes INSIDE the op-amp feedback loop, so the op-amp output is
//
//     V_out = V_in + V_fb ,   V_fb = the voltage the feedback diodes allow.
//
// The clean input V_in ALWAYS passes (the "+V_in" pedestal); only the amplified
// feedback component clips, and it clips SOFTLY (a resistor Rf is always in
// parallel with the diodes). That is the Tube-Screamer-family character: at low
// drive a mostly-clean signal with a light asymmetric edge, never a hard square.
//
// STAGE 1 — non-inverting gain + feedback soft clip.
//   Feedback  Zf = 1 MOhm DRIVE pot (|| 47 pF, || the 2-vs-1 diodes).
//   To ground Zg = R_g 4.7 kOhm + C_g 0.047 uF.
//   Non-inverting gain  A(s) = 1 + Zf/Zg. With Zg = R_g + 1/(sC_g):
//       A(s) = 1 + (Zf/R_g) * (s R_g C_g)/(1 + s R_g C_g)
//            = 1 + K * HP(s) ,   HP = first-order high-pass, unity at HF, 0 at DC,
//     corner  f_mid = 1/(2*pi*R_g*C_g) = 1/(2*pi*4700*0.047e-6) = 720.5 Hz.
//   THE MID-HUMP: gain is UNITY at DC and rises through f_mid to the HF plateau
//   1 + K. K = Zf/R_g is set by DRIVE; at max Zf = 1 MOhm => K = 1e6/4.7e3 = 212.8
//   => plateau 213.8 => +46.6 dB. So the bass below ~720 Hz stays comparatively
//   clean (unity) while the mids/highs are slammed — the SD-1's forward midrange.
//
//   We realise A(s) directly as  V_out = V_in + f( K * HP720(V_in) ), where f is
//   the asymmetric soft clipper (AsymSoftClipper.h). For SMALL signals f(u)~=u so
//   V_out ~= V_in*(1 + K*HP720) — exactly the analytic shelf (the mid-hump test
//   pins this within +/-1.5 dB). For LARGE signals f saturates => soft clip.
//
//   DRIVE knob -> plateau gain, linear-in-dB over [kDriveMinDb, kDriveMaxDb] =
//   [12, 46.6] dB (plateau 4x .. 214x). Min is NOT unity: even at DRIVE 0 a hot
//   input (~0.3 V) * (K_min~=3) clears the ~0.5 V diode knee, so the stage still
//   clips lightly — the SD-1 has no fully-clean setting, by design.
//
//   ASYMMETRY: 2 diodes clip the positive feedback swing (~2*Vf), 1 the negative
//   (~Vf). Knees Vp=0.95 V, Vn=0.50 V (AsymSoftClipper defaults) => an even-
//   harmonic component absent from the symmetric RAT (the asymmetry test measures
//   the 2nd harmonic vs a forced-symmetric reference).
//
//   4558 OP-AMP (LM308Stage reused with 4558 values). The SD-1 uses a FAST chip
//   (unlike the RAT's slow LM308): GBW ~= 3 MHz, slew ~= 1.7 V/us. The closed-loop
//   corner is GBW / A_noise; at the +46.6 dB max (A ~= 214) it sits at 3e6/214 ~=
//   14 kHz — high enough that the mid-hump voicing is unaffected in-band, and the
//   op-amp adds only a gentle top-octave softening at max drive (measured by the
//   closed-loop-bandwidth test). *Approximation* (as in M6.5): the op-amp band-
//   limit + slew are applied to the amplified FEEDBACK drive u only, before the
//   clip — the unity-gain clean pedestal is already full-bandwidth (its own
//   closed-loop corner is GBW/1 = 3 MHz), so limiting only the high-gain path is
//   the physically dominant effect.
//
// STAGE 3 — SD-1 tone control + level.
//   The real tone control is a 10k pot blending a 0.018 uF treble-lean path
//   against a 10k/0.027 uF darker path — a first-order treble TILT about a mid
//   pivot. We approximate it as: split the signal at kTonePivotHz into low/high
//   halves and scale the HIGH half by a tilt gain g_t in [-12, +12] dB as TONE
//   sweeps 0..1 (g_t = 0 dB, i.e. TRANSPARENT, at noon). This matches the
//   published SD-1 tone response SHAPE (progressive treble cut/boost, bass ~fixed)
//   without modelling the exact pot law — a documented approximation. LEVEL is a
//   clean linear output gain (identity map, as in the RAT).
//   A ~12 Hz output DC-blocker (the real pedal's output coupling cap) removes the
//   DC the asymmetric clip produces.
//
// M2 — antialiasing. Only the nonlinear feedback clip runs oversampled (default
// 4x); the 4558 op-amp model and the ADAA soft-clip both live at the oversampled
// rate. The pedestal V_in is upsampled alongside so it stays sample-aligned with
// the clipped feedback (no separate delay line); it is band-limited so it
// reconstructs exactly on downsample. ADAA is the production clip; a naive path
// is selectable for the aliasing A/B (see AsymSoftClipper.h).

#include "clipper/dsp/SdModel.h"

#include "clipper/dsp/AsymSoftClipper.h"
#include "clipper/dsp/LM308Stage.h"
#include "clipper/dsp/Oversampler.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

namespace clipper::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586;

// --- Stage 1: mid-hump + DRIVE ---
constexpr double kMidHumpHz = 720.5;  // 1/(2*pi*4.7k*0.047uF) — the mid-hump corner
constexpr float kDriveMinDb = 12.0f;  // plateau ~4x: clips lightly at a hot input
constexpr float kDriveMaxDb = 46.6f;  // plateau 1 + 1M/4.7k ~= 213.8x (+46.6 dB)

// --- 4558 op-amp (fast, unlike the RAT's LM308) ---
constexpr double kOpAmpGbwHz = 3.0e6;          // ~3 MHz gain-bandwidth product
constexpr double kOpAmpSlewVoltsPerSec = 1.7e6;  // ~1.7 V/us slew

// --- Stage 3: tone tilt + DC blocker ---
constexpr double kTonePivotHz = 1000.0;  // treble-tilt split frequency
constexpr float kToneMaxTiltDb = 12.0f;  // +/-12 dB treble at the extremes
constexpr double kDcBlockHz = 12.0;      // output coupling-cap high-pass

// --- Smoothing ---
constexpr double kSmoothSeconds = 0.005;  // ~5 ms, matches the RAT/M0 smoothers

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float onePoleCoeff(double cutoffHz, double sampleRate) {
    const double a = 1.0 - std::exp(-kTwoPi * cutoffHz / sampleRate);
    return static_cast<float>(std::clamp(a, 0.0, 1.0));
}

// DRIVE knob (0..1) -> feedback gain K (= plateau_gain - 1), linear-in-dB.
float driveKnobToK(float knob) {
    const float db = kDriveMinDb + (kDriveMaxDb - kDriveMinDb) * clamp01(knob);
    return std::pow(10.0f, db / 20.0f) - 1.0f;
}

// TONE knob (0..1) -> treble tilt LINEAR gain (1.0 == flat at knob 0.5).
float toneKnobToTilt(float knob) {
    const float db = (clamp01(knob) - 0.5f) * 2.0f * kToneMaxTiltDb;
    return std::pow(10.0f, db / 20.0f);
}
}  // namespace

struct SdModel::Impl {
    double sampleRate = 44100.0;
    int maxBlockSize = 128;
    int osFactor = 4;
    int clipMode = SdModel::CLIP_ADAA;
    bool idealOpAmp = false;
    bool symmetric = false;

    // Smoothed physical params.
    OnePoleSmoother driveK;   // feedback gain K (plateau - 1)
    OnePoleSmoother toneTilt;  // treble tilt linear gain
    OnePoleSmoother level;     // output gain

    // Stage 1 mid-hump high-pass (runs at the oversampled rate; hp = x - LP720).
    float midHumpCoef = 0.0f;
    float midLpState = 0.0f;

    // Stage 3 tone tilt low-pass (base rate) + DC blocker state.
    float toneCoef = 0.0f;
    float toneLpState = 0.0f;
    double dcR = 0.0;
    float dcX1 = 0.0f, dcY1 = 0.0f;

    // M2 oversampling for the feedback clip.
    Oversampler os;

    // 4558 op-amp (bandwidth + slew) and the asymmetric ADAA soft clipper, both
    // at the oversampled rate.
    LM308Stage opAmp;
    AsymSoftClipper clip;

    void applyKnees() {
        if (symmetric) {
            const double v = 0.5 * (AsymSoftClipper::kDefaultVp + AsymSoftClipper::kDefaultVn);
            clip.setKnees(v, v);
        } else {
            clip.setKnees(AsymSoftClipper::kDefaultVp, AsymSoftClipper::kDefaultVn);
        }
    }

    void reprepareStage2() {
        os.setFactor(osFactor);
        const double osRate = sampleRate * os.factor();
        midHumpCoef = onePoleCoeff(kMidHumpHz, osRate);
        midLpState = 0.0f;
        opAmp.prepare(osRate, kOpAmpGbwHz, kOpAmpSlewVoltsPerSec);
        applyKnees();
        clip.reset();
    }
};

SdModel::SdModel() : impl_(std::make_unique<Impl>()) {}
SdModel::~SdModel() = default;

void SdModel::prepare(double sampleRate, int maxBlockSize) {
    Impl& d = *impl_;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    d.maxBlockSize = maxBlockSize > 0 ? maxBlockSize : 128;

    d.driveK.prepare(kSmoothSeconds, d.sampleRate);
    d.toneTilt.prepare(kSmoothSeconds, d.sampleRate);
    d.level.prepare(kSmoothSeconds, d.sampleRate);

    d.toneCoef = onePoleCoeff(kTonePivotHz, d.sampleRate);
    d.toneLpState = 0.0f;
    d.dcR = std::exp(-kTwoPi * kDcBlockHz / d.sampleRate);
    d.dcX1 = 0.0f;
    d.dcY1 = 0.0f;

    d.os.prepare(d.maxBlockSize);
    d.reprepareStage2();
}

void SdModel::setOversampling(int factor) {
    impl_->osFactor = factor;
    impl_->reprepareStage2();
}
int SdModel::oversampling() const { return impl_->os.factor(); }
int SdModel::latencySamples() const { return impl_->os.latencySamples(); }

void SdModel::setClipMode(int mode) {
    impl_->clipMode = (mode == CLIP_NAIVE) ? CLIP_NAIVE : CLIP_ADAA;
    impl_->clip.reset();
}
int SdModel::clipMode() const { return impl_->clipMode; }

void SdModel::setIdealOpAmp(bool ideal) {
    impl_->idealOpAmp = ideal;
    impl_->opAmp.reset();
}
bool SdModel::idealOpAmp() const { return impl_->idealOpAmp; }

void SdModel::setSymmetric(bool symmetric) {
    impl_->symmetric = symmetric;
    impl_->applyKnees();
}
bool SdModel::symmetric() const { return impl_->symmetric; }

void SdModel::setParameter(int paramId, float value) {
    Impl& d = *impl_;
    const float knob = clamp01(value);
    switch (paramId) {
        case PARAM_DRIVE:
            d.driveK.setTarget(driveKnobToK(knob));
            break;
        case PARAM_TONE:
            d.toneTilt.setTarget(toneKnobToTilt(knob));
            break;
        case PARAM_LEVEL:
            d.level.setTarget(knob);  // identity linear map, as the RAT
            break;
        default:
            break;
    }
}

void SdModel::process(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(d.maxBlockSize, numFrames - off);
        processChunk(in + off, out + off, n);
        off += n;
    }
}

void SdModel::processChunk(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;
    assert(numFrames <= d.maxBlockSize && "chunk exceeds maxBlockSize");

    // Advance the DRIVE smoother across the chunk; use the chunk value as the
    // feedback gain K for the oversampled section (control-rate, like the RAT's
    // op-amp corner and AmpModel's coeffs — the 5 ms glide makes it click-free).
    float K = d.driveK.value();
    for (int i = 0; i < numFrames; ++i) K = d.driveK.next();
    const float noiseGain = K + 1.0f;  // non-inverting closed-loop noise gain
    if (!d.idealOpAmp) d.opAmp.setNoiseGain(noiseGain);

    // --- Stage 1+2 (oversampled): V_out = V_in + f(K * HP720(V_in)). ---
    // Upsample the RAW input; the clean pedestal V_in and the clipped feedback
    // are summed at the oversampled rate so they stay sample-aligned. os.upsample
    // copies `in` internally first, so in/out may alias.
    d.os.upsample(in, numFrames);
    float* w = d.os.buffer();
    const int osN = d.os.bufferLength();
    const float hc = d.midHumpCoef;
    for (int i = 0; i < osN; ++i) {
        const float x = w[i];
        // Mid-hump high-pass: hp = x - LP720(x)  (unity at HF, 0 at DC).
        d.midLpState += hc * (x - d.midLpState);
        const float hp = x - d.midLpState;
        float u = K * hp;                       // amplified feedback drive
        if (!d.idealOpAmp) u = d.opAmp.processSample(u);  // 4558 BW + slew
        const float vfb = (d.clipMode == CLIP_NAIVE) ? d.clip.processSampleNaive(u)
                                                     : d.clip.processSampleADAA(u);
        w[i] = x + vfb;                          // clean pedestal + soft-clipped fb
    }
    d.os.downsample(out, numFrames);

    // --- Stage 3 (base rate, linear): DC block -> tone tilt -> level. ---
    for (int i = 0; i < numFrames; ++i) {
        const float v = out[i];
        // One-pole DC blocker (output coupling cap, ~12 Hz).
        const float y = v - d.dcX1 + static_cast<float>(d.dcR) * d.dcY1;
        d.dcX1 = v;
        d.dcY1 = y;
        // Treble tilt: scale the HF half (y - LP_pivot) by the tilt gain.
        d.toneLpState += d.toneCoef * (y - d.toneLpState);
        const float hpTone = y - d.toneLpState;
        const float toned = d.toneLpState + d.toneTilt.next() * hpTone;
        out[i] = toned * d.level.next();
    }
}

}  // namespace clipper::dsp
