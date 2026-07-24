// Clipper — the GOLD overdrive (v1.1 item 6). See GoldModel.h for the section
// overview. Circuit-INFORMED, not SPICE-accurate (the house scope); every
// approximation below is flagged.
//
// ---------------------------------------------------------------------------
// SOURCES — read this before trusting a number in this file
// ---------------------------------------------------------------------------
// Method reference: Jatin Chowdhury, "A Comparison of Virtual Analog Modelling
// Techniques for Desktop and Embedded Implementations" (arXiv:2009.02833), which
// models exactly this pedal by splitting it into sections and treating each with
// nodal analysis / Wave Digital Filters — the same author whose chowdsp_wdf we
// vendor and whose diode-pair root this file uses. HONESTY NOTE: the PDF is NOT
// reachable from this build environment (arxiv.org and ccrma.stanford.edu are both
// refused by the egress proxy — 403 on CONNECT), so this model follows the paper's
// METHOD (section-per-section, WDF for the diode root, nodal/analytic transfer
// functions for the linear sections) but its COMPONENT VALUES come from the widely
// published reverse-engineered schematic of the pedal, and every one of them is
// marked as an approximation below. Where the paper's own values differ, this file
// should be corrected against it — the topology, not the digits, is the claim.
//
// Reference level convention (model-wide): input float 1.0f == 1.0 V. A hot
// humbucker DI peaks near 0.3 V.
//
// ---------------------------------------------------------------------------
// SECTION 1 — INPUT BUFFER (always on)
// ---------------------------------------------------------------------------
// The hardware buffers the input even when the effect is switched out ("buffered
// bypass"), which is why this pedal is famous as a line driver. Electrically it is
// a unity-gain op-amp follower fed through the input network; the only audible
// thing it does in the band is the DC-blocking corner of the input coupling cap
// into the bias/pulldown resistance:
//     f_in = 1/(2*pi*R_in*C_in) = 1/(2*pi*1.0 MOhm * 0.022 uF) ~= 7.2 Hz.
// Modelled as a one-pole high-pass at kInputHpHz. The op-amp's own bandwidth here
// is far above audio and is NOT modelled (documented omission — a unity follower
// with a 3 MHz-GBW part has a ~3 MHz corner).
//
// ---------------------------------------------------------------------------
// SECTION 2 — GAIN SECTION: the dual-ganged pot, the clean blend, the germanium
// ---------------------------------------------------------------------------
// THE ARCHITECTURE THAT MAKES THIS PEDAL WHAT IT IS. The buffered signal splits
// two ways and is re-summed:
//
//     out_sum = kSumGain * ( cleanBlend(g) * x  +  clipBlend(g) * clip(A(g)*HP(x)) )
//
//   * BOTH weights come from ONE dual-ganged GAIN pot, so a single knob
//     CROSS-FADES a full-bandwidth clean signal against a clipped one. At GAIN 0
//     the clipped half is switched out entirely and the pedal is a clean buffer /
//     boost (this is why it measures transparent and why players run it always-on);
//     as the knob comes up the clipped half rises while the clean half only falls
//     part-way, so the dirt arrives THROUGH a clean core instead of replacing it.
//     Pot: dual-ganged 100 kOhm (kGainPotOhms).
//       cleanBlend(g) = 1 - kCleanTilt*g   (1.00 -> 0.55: the clean core NEVER
//                                           leaves — approximation of the real
//                                           network's second wiper law)
//       clipBlend(g)  = g                  (linear wiper on the clipped signal)
//   * A(g) — the drive amp. Non-inverting stage whose feedback leg is the OTHER
//     gang: A = 1 + g*P/R_g with P = 100 kOhm, R_g = 1.5 kOhm, so
//       A(0) = 1.0  ... A(1) = 1 + 100e3/1.5e3 = 67.67x = +36.6 dB.
//     Note the minimum is EXACTLY unity — unlike the TS family (which grinds even
//     at DRIVE 0), this pedal has an honestly clean setting.
//   * HP(x) — the drive path is high-passed BEFORE the clipper:
//       f_hp = 1/(2*pi*R*C) = 1/(2*pi*15 kOhm * 0.1 uF) ~= 106 Hz.
//     The low end therefore reaches the summing node ONLY through the clean half.
//     This is the "it doesn't get mushy" trait: the clipper never sees the bass.
//   * The op-amp. A TL07x-class part on the charge-pump rails: GBW 3 MHz, slew
//     13 V/us. Its closed-loop corner GBW/A at max gain is 3e6/67.67 ~= 44 kHz —
//     ABOVE the audio band, so (unlike the RAT's LM308, which collapses to ~500 Hz)
//     this op-amp adds no audible softening; it is present for honesty, stability
//     and antialiasing. Reuses the house op-amp model (LM308Stage.h — the class is
//     named for its first user, but it is a generic GBW+slew op-amp).
//   * The GERMANIUM clipper (WDF, chowdsp_wdf). Antiparallel 1N34A-class pair,
//     driven through the stage's output/feedback resistance and shunted by the
//     stage's small cap — the library's canonical resistive-source || cap -> diode
//     root, exactly as RatModel builds its silicon clipper:
//       kRs = 2.2 kOhm, kCp = 4.7 nF  (HF corner 1/(2*pi*Rs*Cp) ~= 15.4 kHz)
//       Germanium: Is = 200 nA, ideality n = 1.3, Vt = 25.85 mV
//                  -> knee ~ n*Vt*ln(I/Is) = 0.29 V at 1 mA (a point-contact Ge
//                     diode; roughly HALF the silicon 0.6 V and, far more audibly,
//                     a MUCH softer knee: it starts bending decades of current
//                     earlier, which is the germanium "bloom").
//       The ideality factor is carried through the library's nDiodes multiplier,
//       which scales Vt (Vt_eff = n*Vt = 33.6 mV) — the same arithmetic.
//     DIODE_SILICON (1N914-class, Is = 2.52 nA, n = 1.0) is a MEASUREMENT-ONLY
//     counterfactual so the tests can show the knee difference, never a user knob.
//   * The charge pump. The real pedal generates a negative rail so its op-amps run
//     on ~+/-9 V instead of a single 9 V supply. Here that is a HEADROOM statement:
//     the summing node carries an explicit +/-kRailVolts clamp which, at guitar
//     levels, never engages — the germanium pair is the only clipper in the box.
//     (Tested: a 1 V input at max gain/output stays well below the rails.)
//
// ---------------------------------------------------------------------------
// SECTION 3 — TREBLE
// ---------------------------------------------------------------------------
// The post-blend treble control, modelled as a shelving TILT about a ~1 kHz pivot:
// the HF half (x - LP_pivot(x)) is scaled by +/- kToneMaxTiltDb. NORMAL SENSE:
// clockwise BRIGHTENS (contrast the RAT's FILTER and the AC30's CUT, both
// inverted). Flat at exactly 0.5. *Approximation:* the hardware's tone network is
// an active stage with a gentle interaction with the output pot; the tilt is a
// first-order stand-in with the same pivot and roughly the same range.
//
// ---------------------------------------------------------------------------
// SECTION 4 — OUTPUT STAGE
// ---------------------------------------------------------------------------
// Output buffer + OUTPUT pot (identity linear map, the house convention) + the
// output coupling cap's DC block at kOutHpHz (~8 Hz). The pot is the pedal's
// makeup gain: at GAIN 0 / OUTPUT 1 the box is a unity clean buffer.
//
// M2 — antialiasing. ONLY section 2 (the nonlinearity) is oversampled: the clean
// half, the drive high-pass, the amp, the op-amp model, the WDF clipper and the
// summing node all run at the oversampled rate so the two halves stay sample-
// aligned (the clean/clipped sum must not smear). Sections 1, 3 and 4 are linear
// and stay at the base rate. Default 4x (measured; see docs §27).

#include "clipper/dsp/GoldModel.h"

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/LM308Stage.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/Oversampler.h"

#include <chowdsp_wdf/chowdsp_wdf.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

namespace clipper::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586;

// --- Section 1: input buffer ---
constexpr double kInputHpHz = 7.2;  // 1/(2*pi*1M*0.022uF) — input coupling cap

// --- Section 2: the ganged gain section ---
constexpr double kGainPotOhms = 100.0e3;  // dual-ganged GAIN pot
constexpr double kDriveRgOhms = 1.5e3;    // the drive amp's to-ground leg
// cleanBlend(g) = 1 - kCleanTilt*g. The clean core never leaves the mix (0.45 at
// max) — the audible reason the pedal keeps note definition when pushed.
constexpr double kCleanTilt = 0.55;
constexpr double kDriveHpHz = 106.1;  // 1/(2*pi*15k*0.1uF) — lows skip the clipper
constexpr double kSumGain = 2.0;      // the summing amp's non-inverting gain (1 + R/R)
constexpr double kRailVolts = 8.6;    // charge-pump rails (+/-9 V minus dropout)

// The op-amp: TL07x-class on the charge-pump rails.
constexpr double kOpAmpGbwHz = 3.0e6;
constexpr double kOpAmpSlewVoltsPerSec = 13.0e6;

// WDF clipping network.
constexpr double kRs = 2.2e3;   // series/source resistance into the diode node
constexpr double kCp = 4.7e-9;  // shunt cap (HF corner ~15.4 kHz)
// Germanium (1N34A-class) vs the silicon counterfactual (1N914-class).
constexpr double kGeIs = 200.0e-9;
constexpr double kGeIdeality = 1.3;
constexpr double kSiIs = 2.52e-9;
constexpr double kSiIdeality = 1.0;
constexpr double kVt = 25.85e-3;

// --- Section 3: treble ---
constexpr double kTonePivotHz = 1000.0;
constexpr float kToneMaxTiltDb = 12.0f;

// --- Section 4: output ---
constexpr double kOutHpHz = 8.0;

// --- Smoothing (the house ~5 ms glide) ---
constexpr double kSmoothSeconds = 0.005;

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float onePoleCoeff(double cutoffHz, double sampleRate) {
    const double a = 1.0 - std::exp(-kTwoPi * cutoffHz / sampleRate);
    return static_cast<float>(std::clamp(a, 0.0, 1.0));
}

// TREBLE knob -> linear tilt gain applied to the HF half (1.0 == flat at 0.5).
float toneKnobToTilt(float knob) {
    const float db = (clamp01(knob) - 0.5f) * 2.0f * kToneMaxTiltDb;
    return std::pow(10.0f, db / 20.0f);
}
}  // namespace

double GoldModel::driveGainAt(double knob) {
    const double g = knob < 0.0 ? 0.0 : (knob > 1.0 ? 1.0 : knob);
    return 1.0 + g * kGainPotOhms / kDriveRgOhms;
}
double GoldModel::cleanBlendAt(double knob) {
    const double g = knob < 0.0 ? 0.0 : (knob > 1.0 ? 1.0 : knob);
    return 1.0 - kCleanTilt * g;
}
double GoldModel::clipBlendAt(double knob) {
    return knob < 0.0 ? 0.0 : (knob > 1.0 ? 1.0 : knob);
}

struct GoldModel::Impl {
    double sampleRate = 44100.0;
    int maxBlockSize = 128;
    int osFactor = 4;
    int diodeType = GoldModel::DIODE_GERMANIUM;
    bool cleanBlend = true;
    bool idealOpAmp = false;

    // Smoothed physical params (mapped, not raw knob positions).
    OnePoleSmoother driveGain;   // A(g), the drive amp's voltage gain
    OnePoleSmoother cleanMix;    // cleanBlend(g)
    OnePoleSmoother clipMix;     // clipBlend(g)
    OnePoleSmoother toneTilt;    // treble tilt (linear gain on the HF half)
    OnePoleSmoother outLevel;    // OUTPUT pot

    // Section 1 (base rate): input-buffer high-pass state.
    float inHpCoef = 0.0f;
    float inHpState = 0.0f;

    // Section 2 (oversampled): drive-path high-pass state.
    float driveHpCoef = 0.0f;
    float driveHpState = 0.0f;

    // Section 3/4 (base rate): tone pivot low-pass + output DC block.
    float toneCoef = 0.0f;
    float toneLpState = 0.0f;
    double dcR = 0.0;
    float dcX1 = 0.0f, dcY1 = 0.0f;

    Oversampler os;
    LM308Stage opAmp;  // generic GBW + slew op-amp model (see LM308Stage.h)

    // The WDF germanium clipper. Declaration order matters: children first, root
    // last. Double precision, as the RAT's tree.
    chowdsp::wdft::ResistiveVoltageSourceT<double> Vs { kRs };
    chowdsp::wdft::CapacitorT<double> Cp { kCp, 48000.0 };
    chowdsp::wdft::WDFParallelT<double, decltype(Vs), decltype(Cp)> P1 { Vs, Cp };
    chowdsp::wdft::DiodePairT<double, decltype(P1)> diodes {
        P1, kGeIs, kVt, kGeIdeality };

    void applyDiodes() {
        if (diodeType == GoldModel::DIODE_SILICON)
            diodes.setDiodeParameters(kSiIs, kVt, kSiIdeality);
        else
            diodes.setDiodeParameters(kGeIs, kVt, kGeIdeality);
    }

    void reprepareGainSection() {
        os.setFactor(osFactor);
        const double osRate = sampleRate * os.factor();
        Cp.prepare(osRate);  // the WDF cap runs at the OVERSAMPLED rate
        Vs.setVoltage(0.0);
        applyDiodes();
        driveHpCoef = onePoleCoeff(kDriveHpHz, osRate);
        driveHpState = 0.0f;
        opAmp.prepare(osRate, kOpAmpGbwHz, kOpAmpSlewVoltsPerSec);
    }
};

GoldModel::GoldModel() : impl_(std::make_unique<Impl>()) {}
GoldModel::~GoldModel() = default;

void GoldModel::prepare(double sampleRate, int maxBlockSize) {
    Impl& d = *impl_;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    d.maxBlockSize = maxBlockSize > 0 ? maxBlockSize : 128;

    d.driveGain.prepare(kSmoothSeconds, d.sampleRate);
    d.cleanMix.prepare(kSmoothSeconds, d.sampleRate);
    d.clipMix.prepare(kSmoothSeconds, d.sampleRate);
    d.toneTilt.prepare(kSmoothSeconds, d.sampleRate);
    d.outLevel.prepare(kSmoothSeconds, d.sampleRate);

    d.inHpCoef = onePoleCoeff(kInputHpHz, d.sampleRate);
    d.inHpState = 0.0f;
    d.toneCoef = onePoleCoeff(kTonePivotHz, d.sampleRate);
    d.toneLpState = 0.0f;
    d.dcR = std::exp(-kTwoPi * kOutHpHz / d.sampleRate);
    d.dcX1 = 0.0f;
    d.dcY1 = 0.0f;

    d.os.prepare(d.maxBlockSize);
    d.reprepareGainSection();
}

void GoldModel::setOversampling(int factor) {
    impl_->osFactor = factor;
    impl_->reprepareGainSection();
}
int GoldModel::oversampling() const { return impl_->os.factor(); }
int GoldModel::latencySamples() const { return impl_->os.latencySamples(); }

void GoldModel::setDiodeType(int type) {
    impl_->diodeType = (type == DIODE_SILICON) ? DIODE_SILICON : DIODE_GERMANIUM;
    impl_->applyDiodes();
}
int GoldModel::diodeType() const { return impl_->diodeType; }

void GoldModel::setCleanBlendEnabled(bool enabled) { impl_->cleanBlend = enabled; }
bool GoldModel::cleanBlendEnabled() const { return impl_->cleanBlend; }

void GoldModel::setIdealOpAmp(bool ideal) {
    impl_->idealOpAmp = ideal;
    impl_->opAmp.reset();
}
bool GoldModel::idealOpAmp() const { return impl_->idealOpAmp; }

void GoldModel::setParameter(int paramId, float value) {
    Impl& d = *impl_;
    const float knob = clamp01(value);
    switch (paramId) {
        case PARAM_GAIN:
            // ONE knob, THREE mapped quantities — the dual-ganged pot.
            d.driveGain.setTarget(static_cast<float>(driveGainAt(knob)));
            d.cleanMix.setTarget(static_cast<float>(cleanBlendAt(knob)));
            d.clipMix.setTarget(static_cast<float>(clipBlendAt(knob)));
            break;
        case PARAM_TREBLE:
            d.toneTilt.setTarget(toneKnobToTilt(knob));
            break;
        case PARAM_OUTPUT:
            d.outLevel.setTarget(knob);  // identity linear map, as the RAT/TS
            break;
        default:
            break;
    }
}

void GoldModel::process(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(d.maxBlockSize, numFrames - off);
        processChunk(in + off, out + off, n);
        off += n;
    }
}

void GoldModel::processChunk(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;
    assert(numFrames <= d.maxBlockSize && "chunk exceeds maxBlockSize");

    // --- Section 1 (base rate, linear): the always-on input buffer. ----------
    // One-pole high-pass = x - LP(x). Written into `out` so in/out may alias;
    // os.upsample() copies it away immediately below.
    const float ihc = d.inHpCoef;
    for (int i = 0; i < numFrames; ++i) {
        const float x = in[i];
        d.inHpState = flushDenormal(d.inHpState + ihc * (x - d.inHpState));
        out[i] = x - d.inHpState;
    }

    // Advance the gain-section smoothers across the chunk and use the chunk value
    // for the oversampled loop (control rate — the same discipline as the RAT's
    // op-amp corner and the OverdriveEngine's K; the 5 ms glide keeps it click-free).
    float A = d.driveGain.value(), cleanW = d.cleanMix.value(), clipW = d.clipMix.value();
    for (int i = 0; i < numFrames; ++i) {
        A = d.driveGain.next();
        cleanW = d.cleanMix.next();
        clipW = d.clipMix.next();
    }
    if (!d.cleanBlend) cleanW = 0.0f;  // measurement counterfactual
    if (!d.idealOpAmp) d.opAmp.setNoiseGain(A);

    // --- Section 2 (oversampled, nonlinear): the ganged gain section. --------
    // Both halves live at the oversampled rate so the clean signal and the clipped
    // signal stay sample-aligned when they meet at the summing node.
    d.os.upsample(out, numFrames);
    float* w = d.os.buffer();
    const int osN = d.os.bufferLength();
    const float hc = d.driveHpCoef;
    for (int i = 0; i < osN; ++i) {
        const float x = w[i];
        // Drive-path high-pass: the lows never reach the clipper.
        d.driveHpState = flushDenormal(d.driveHpState + hc * (x - d.driveHpState));
        float u = A * (x - d.driveHpState);
        if (!d.idealOpAmp) u = d.opAmp.processSample(u);  // GBW + slew
        // Germanium diode pair (WDF root); output is the clipping-node voltage.
        d.Vs.setVoltage(static_cast<double>(u));
        d.diodes.incident(d.P1.reflected());
        d.P1.incident(d.diodes.reflected());
        const float clipped = static_cast<float>(chowdsp::wdft::voltage<double>(d.Cp));
        // The summing amp: the ganged crossfade, then the charge-pump rails (which
        // at guitar levels never engage — the diodes are the only clipper).
        float s = static_cast<float>(kSumGain) * (cleanW * x + clipW * clipped);
        if (s > static_cast<float>(kRailVolts)) s = static_cast<float>(kRailVolts);
        else if (s < -static_cast<float>(kRailVolts)) s = -static_cast<float>(kRailVolts);
        w[i] = s;
    }
    d.os.downsample(out, numFrames);

    // --- Sections 3+4 (base rate, linear): treble tilt -> OUTPUT -> DC block. -
    for (int i = 0; i < numFrames; ++i) {
        const float v = out[i];
        d.toneLpState = flushDenormal(d.toneLpState + d.toneCoef * (v - d.toneLpState));
        float toned = d.toneLpState + d.toneTilt.next() * (v - d.toneLpState);
        // The tone stage is an active stage on the SAME charge-pump rails, so it
        // carries the same clamp. Like the summing node's, it never engages at any
        // real playing level (a 1 V input wide open peaks ~1.6 V) — it exists so a
        // pathological input cannot leave the supply behind.
        if (toned > static_cast<float>(kRailVolts)) toned = static_cast<float>(kRailVolts);
        else if (toned < -static_cast<float>(kRailVolts)) toned = -static_cast<float>(kRailVolts);
        const float lvl = toned * d.outLevel.next();
        // Output coupling cap (one-pole DC blocker).
        const float y = lvl - d.dcX1 + static_cast<float>(d.dcR) * d.dcY1;
        d.dcX1 = lvl;
        d.dcY1 = y;
        out[i] = y;
    }
}

}  // namespace clipper::dsp
