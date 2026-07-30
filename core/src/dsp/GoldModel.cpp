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
//     out_sum = kSumGain * ( cleanBlend * x  +  clipBlend(g) * clip(A(g)·pre·HP(x)) )
//
//   * The dual-ganged GAIN pot works the way the real one does (re-derived
//     2026-07-31, docs §50, against the published schematic): the knob changes the
//     drive amp's GAIN, not the mix. The clean feed stays ~constant (gang 2's
//     divider — the "clean fades out" folklore is only relative to the growing
//     dirt), the dirt path's summing weight is FIXED, and the dirt arrives THROUGH
//     the clean core because the drive rises end-loaded while the clean holds.
//     At GAIN 0 the clipped half is switched fully out (a kept product contract —
//     the real unit measures 0.2-3.9 % even at min; ours is bit-exact clean).
//       cleanBlendAt(g) = 1.0                       (flat)
//       clipBlendAt(g)  = kClipBlendWeight past a short fade-in from 0
//   * A(g) — the drive amp, gang 1 in its ground leg:
//       A = 1 + 422k/((1-g)·100k + 17k) = 4.61x ... 25.82x (+13.3 ... +28.2 dB),
//     END-LOADED (half the dB range in the last quarter-turn). The pre-§50 law
//     (1 + g·100k/1.5k, to 67.7x linear) put the real knob-0.99 drive at the 0.35
//     default — docs §50 has the full model-vs-real table.
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
//     DIODE_SILICON (1N914/1N4148-class, Is = 2.52 nA, n = 1.752 — the SPICE model
//     card's own pair of numbers) is a MEASUREMENT-ONLY counterfactual so the tests
//     can show the knee difference, never a user knob. Measured contrast in this
//     network: silicon clips ~5.5-6.3 dB above germanium. It shipped at n = 1.0
//     until 2026-07-25, where the contrast measured ~0.6-1.7 dB and the A/B was
//     therefore worthless (audit finding 15; docs §36, ADR 008).
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
// makeup gain. NOTE (docs §50 correction): at GAIN 0 the box is a clean buffer
// whose UNITY sits at OUTPUT 0.5 — kSumGain = 2.0 means OUTPUT 1 is +6.02 dB
// (measured; test_gold_model asserts it). §27 documents the same.
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
#include "clipper/dsp/ParamGuard.h"

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

// --- Section 2: the ganged gain section (re-derived 2026-07-31, docs §50) -----------
// The real dual-gang law, from the published schematic (the Chowdhury/ElectroSmash
// values — the reference §27 names): gang 1's lower half sits in the drive op-amp's
// ground leg, so A(g) = 1 + Rf / ((1-g)·pot + Rleg+Rstop) = 1 + 422k/((1-g)·100k+17k)
// = 4.61x (g=0) -> 25.82x (g=1), END-LOADED — half the dB range lives in the last
// quarter-turn. The pre-§50 law (1 + g·pot/1.5k, up to 67.7x LINEAR) delivered the
// real pedal's knob-0.99 drive at the shipped 0.35 default — the owner's "gainy at
// even 35+" was literally correct, and 19 % THD sat where the docs promised
// "mostly-clean with a little grit". The germanium itself was measured RIGHT in the
// corrected topology (the §36 vindication continues) — the law was the bug.
constexpr double kGainPotOhms = 100.0e3;   // dual-ganged GAIN pot
constexpr double kDriveRfOhms = 422.0e3;   // drive op-amp feedback (R12)
constexpr double kDriveRlegOhms = 17.0e3;  // ground-leg fixed part (15k + 2k stop)
// cleanBlend: the real gang-2 divider holds the clean feed nearly CONSTANT (the
// "clean fades out" folklore is only relative to the growing dirt) — flat, per the
// nodal analysis of the published network.
// clipBlend: the summing weight of the dirt path is FIXED in the real circuit (the
// knob changes DRIVE, not mix); a short fade-in below g ~ 0.15 keeps this model's
// documented GAIN-0 contract (clipBlend(0) = 0 -> the crossfade switches the clipped
// half fully OUT, an idealization the real unit doesn't share — it measures 0.2-3.9 %
// even at min. Kept deliberately: bit-exact-clean at zero is a product contract here).
constexpr double kClipBlendWeight = 0.65;  // fixed dirt summing weight (fit, §50)
constexpr double kClipBlendFadeTo = 0.15;  // linear fade-in span keeping clip(0)=0
// The drive path's INPUT network attenuates before the diodes see anything: measured
// on the real topology ~0.20x @220 Hz / 0.65x @1 kHz (the model previously passed
// 0.90 @220 — its 106 Hz corner belonged to FF1, the always-on clean-bass path, not
// the drive branch). One pole + a scale fit to the reference rows (±3 dB, g <= .75):
constexpr double kDrivePreScale = 0.65;
constexpr double kDriveHpHz = 600.0;  // drive-branch HP (was 106.1, mis-assigned; fit: |H|·pre at 220 Hz = 0.22 vs the reference 0.20, at 1 kHz 0.56 vs 0.65)
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
// 1N4148/1N914 SPICE: `IS=2.52n N=1.752`. Was 1.0 until 2026-07-25 — the same
// dropped-ideality error as RatModel (audit finding 15, docs §36, ADR 008). With
// n = 1.0 the silicon "counterfactual" clipped only 0.60-1.70 dB above the
// germanium pair in this same network, so the A/B that exists to show what the
// germanium buys was showing almost nothing; a real 1N34A-vs-1N4148 comparison is
// ~6 dB. MEASURED, settled, in this tree (Rs = 2.2 k, Cp = 4.7 nF), Si over Ge:
// +6.33 dB at 1 V of drive, +5.99 dB at 10 V, +5.47 dB at 100 V.
// The GERMANIUM side was and is correct (n = 1.3, knee 0.286 V at 1 mA) — only the
// silicon reference was wrong, so the pedal's own voice is unchanged by this fix.
constexpr double kSiIdeality = 1.752;
constexpr double kVt = 25.85e-3;

// --- Section 3: treble ---
constexpr double kTonePivotHz = 1000.0;
constexpr float kToneMaxTiltDb = 12.0f;

// --- Section 4: output ---
constexpr double kOutHpHz = 8.0;

// --- Smoothing (the house ~5 ms glide) ---
constexpr double kSmoothSeconds = 0.005;

// NaN-rejecting knob clamp (ParamGuard.h) — audit finding 1.
float clamp01(float v) { return clampParam01(v); }

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

// The three ganged maps. All three clamp through ParamGuard (NaN -> 0), so even a
// direct call from a test or the plugin cannot hand a NaN gain to the smoothers.
double GoldModel::driveGainAt(double knob) {
    const double g = clampParam01(knob);
    return 1.0 + kDriveRfOhms / ((1.0 - g) * kGainPotOhms + kDriveRlegOhms);
}
double GoldModel::cleanBlendAt(double knob) {
    (void)clampParam01(knob);  // NaN-reject for API parity; the clean feed is flat
    return 1.0;
}
double GoldModel::clipBlendAt(double knob) {
    const double g = clampParam01(knob);
    // Fixed dirt weight with the short fade-in that preserves clipBlend(0) = 0.
    return g >= kClipBlendFadeTo ? kClipBlendWeight
                                 : kClipBlendWeight * (g / kClipBlendFadeTo);
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

void GoldModel::reset() {
    Impl& d = *impl_;
    // Smoothers first: a poisoned smoother value never recovers on its own.
    d.driveGain.reset();
    d.cleanMix.reset();
    d.clipMix.reset();
    d.toneTilt.reset();
    d.outLevel.reset();
    d.inHpState = 0.0f;
    d.toneLpState = 0.0f;
    d.dcX1 = 0.0f;
    d.dcY1 = 0.0f;
    d.opAmp.reset();
    d.os.reset();
    // Re-derives the oversampled-section coefficients at the CURRENT rate/factor and
    // resets driveHpState / the WDF cap / the source. Allocation-free.
    d.reprepareGainSection();
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
        // Drive-path input network (§50): the real branch ATTENUATES before the
        // diodes see anything — kDrivePreScale + the (re-assigned) HP corner.
        d.driveHpState = flushDenormal(d.driveHpState + hc * (x - d.driveHpState));
        float u = A * static_cast<float>(kDrivePreScale) * (x - d.driveHpState);
        if (!d.idealOpAmp) u = d.opAmp.processSample(u);  // GBW + slew
        // Germanium diode pair (WDF root); output is the clipping-node voltage.
        d.Vs.setVoltage(static_cast<double>(u));
        d.diodes.incident(d.P1.reflected());
        d.P1.incident(d.diodes.reflected());
        const float clipped = static_cast<float>(chowdsp::wdft::voltage<double>(d.Cp));
        // Anti-denormal (Denormal.h): the WDF shunt cap's wave state is the network's
        // recursive memory and rings down into DOUBLE subnormals on silence. Flushed
        // AFTER the voltage above is read, so this sample is bit-identical to the
        // unguarded network. Audit finding 11, docs §33.
        flushDenormalWdfCapacitor(d.Cp);
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
        // Anti-denormal (Denormal.h): on silence this degenerates to y = dcR*dcY1
        // with dcR ~= 0.9984, which in the subnormal range rounds back to itself and
        // NEVER reaches zero. This one state was GOLD's whole subnormal problem:
        // measured 393607 of 480000 output samples subnormal over 10 s of silence,
        // shoved straight into whatever follows GOLD in the chain. Audit finding 11,
        // docs §33. dcX1 is an input history (assigned, never fed back) — no guard.
        const float y = lvl - d.dcX1 + static_cast<float>(d.dcR) * d.dcY1;
        d.dcX1 = lvl;
        d.dcY1 = flushDenormal(y);
        out[i] = y;
    }
}

}  // namespace clipper::dsp
