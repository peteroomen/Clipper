// Clipper — RAT-style diode clipper (M1). See RatModel.h for the stage overview.
//
// ---------------------------------------------------------------------------
// Circuit values, sources, and knob mappings (all noted as approximations —
// this is circuit-INFORMED, not SPICE-accurate; per the roadmap scope).
// ---------------------------------------------------------------------------
//
// Reference level: input float 1.0f == 1.0 V at the diode stage. A hot
// humbucker DI peaks around ~0.3 V, single-coils lower, so the pre-gain must
// lift the signal past the diode threshold to clip — exactly as the real pedal
// does with its LM308 stage.
//
// THE NETLIST (added 2026-08-01, docs §66). Until then every value in this file
// was sourced from prose and analysis; a full ProCo RAT netlist is now readable
// (Cushychicken/ltspice-guitar-pedals, `proco-rat-distortion.asc`, traced from
// the published schematic — github.com is the only host reachable from this
// container). Traced node by node against the code, the agreements and the
// remaining approximations are:
//
//   AGREES EXACTLY — R8 560 R + C8 4.7 uF leg (60.5 Hz) and R7 47 R + C7 2.2 uF
//   leg (1539 Hz) in the feedback network (kShapeLeg1Hz / kShapeLeg2Hz); R9 the
//   100 k gain pot; R10 = 1 k feeding the antiparallel pair (kRs); the pair
//   itself (D2/D3, 1N914 — modelled as the 1N4148 card, docs §36); C1 = 30 pF
//   op-amp compensation (which is what selects the slew figure below).
//
//   APPROXIMATED, each with its measured cost:
//     * C9 = 100 pF across the gain pot is NOT modelled. Its pole is
//       1/(2*pi*Rf*C9) and the closed-loop corner this file DOES model is
//       GBW/A ~= GBW*(R7||R8)/Rf: both scale as 1/Rf, so their ratio is
//       1/(2*pi*C9*GBW*(R7||R8)) = 36.7x at EVERY knob position. C9's pole is
//       36.7x above a pole that is already in the model, so it can never
//       contribute more than ~0.03 dB in the audio band. Derivation, not an
//       omission.
//     * kCp = 10 nF at the clipping node is FABRICATED (it comes from the WDF
//       library's example, not from the RAT). The real node carries no cap; it
//       is loaded by the tone network, R17(100 k log) + R15 1.5 k into
//       C11 3.3 nF. Replacing it moves the diode drive above ~10 kHz, the alias
//       floor and DiodeClipperADAA's calibration together — its own slice.
//     * STAGE 3's low-pass IS that tone network in form, but the model sweeps
//       the CUTOFF log (500 Hz..20 kHz) where the circuit sweeps the RESISTANCE
//       log (475 Hz..32.15 kHz through the 1.5 k + 3.3 nF).
//     * The three output high-passes (C10 4.7 uF into 1 k = 33.9 Hz, C12 22 nF
//       into 1 M = 7.2 Hz, C13 1 uF into 100 k = 1.6 Hz) are not modelled:
//       together <= 0.75 dB at low E.
//     * The LEVEL pot is a 100 k LOGARITHMIC pot (the netlist says so in its own
//       annotation), last in the chain after the 2N5458 source follower and
//       therefore essentially unloaded — so its law is the bare audio taper,
//       where this file maps it identity-linear (see STAGE 3). Docs §66.3
//       measures what fixing it would cost (-5.04 dB at the shipped default) and
//       why it belongs to a lineup-wide slice: every other dirt pedal carries
//       the same approximation, OverdriveEngine's in the words "identity linear
//       map, as the RAT".
//
// STAGE 1 — gain / shaping (LM308 non-inverting amp; ProCo RAT).
//   * DISTORTION knob -> pre-gain, linear-in-dB over [kDistMinDb, kDistMaxDb]
//     = [0, +66 dB] (M6.1: the real RAT non-inverting HF plateau 1 + P1/(R1||R2);
//     was +54 dB). See the kDistMaxDb constant below.
//   * Pre-clip frequency shaping (M6.1 two-corner RAT feedback voicing): the
//     stage gain RISES with frequency through the feedback network's two series-RC
//     legs (~60.5 Hz and ~1539 Hz corners) and falls toward unity at DC — NOT the
//     old single 320 Hz shelf. Implemented as x - g1*LP60 - g2*LP1539 (see the
//     kShape* constants below).
//   * M6.5: the LM308's op-amp limitations (gain-tracking closed-loop bandwidth
//     and slew-rate limiting) ARE now modelled — at the op-amp output, inside
//     oversampling, before the diode clamp (see the M6.5 note above and
//     LM308Stage.h). They were the missing "thick, not fizzy" behaviour.
//
// STAGE 2 — clipper (WDF, chowdsp_wdf). Antiparallel silicon diode pair to
//   ground (1N914-ish). Built exactly like the library's RC diode-clipper
//   example: a resistive voltage source (series resistance kRs) in PARALLEL with
//   a shunt capacitor (kCp), feeding a diode-pair root. The shunt cap is taken
//   straight from the library example — it aids numerical stability and adds a
//   touch of realism (a gentle HF corner at ~1/(2*pi*Rs*Cp) ~ 16 kHz here).
//     kRs = 1.0 kOhm  (series/source resistance feeding the diodes; modelling
//                      choice — the RAT's clipping node sees the op-amp output
//                      through a 1 k resistor)
//     kCp = 10 nF     (shunt cap; from the library's clipper example)
//     Diode: the 1N4148 SPICE model, Is = 2.52 nA with ideality n = 1.752
//            (IS=2.52n N=1.752), Vt = 25.85 mV, 1 diode per side. MEASURED
//            settled clipping-node ceiling: 0.548 V at 1 V drive, 0.622 V at 3 V,
//            0.678 V at 10 V, 0.738 V at 100 V — i.e. the +/-0.6..0.7 V silicon
//            knee this file has always claimed.
//   The diode pair is the Werner et al. improved model shipped by the library
//   (DiodeQuality::Best). Output is the voltage across the shunt cap = the
//   clipping-node voltage.
//
//   AUDIT FINDING 15 (fixed 2026-07-25 — docs §36, ADR 008). This stage shipped
//   from M1 to v1.1 with the 1N4148's saturation current but its ideality factor
//   DROPPED: `DiodePairT { P1, kDiodeIs, kDiodeVt, 1.0 }`. The library uses its
//   fourth argument as `Vt_eff = nDiodes * Vt`, which is exactly where an ideality
//   factor belongs, so n = 1.0 shrank the junction's thermal voltage by 1.752x and
//   the pair only reached 0.6 V at ~30 A. Measured ceiling was 0.322 / 0.376 /
//   0.421 / 0.434 V at 1 / 10 / 100 / 600 V of drive — 5-6 dB BELOW the +/-0.6 V
//   documented here and in docs §6, with a harder knee than a real silicon pair.
//   This is a deliberate TONE change: at a given DISTORTION setting the pedal is
//   now ~5 dB louder and correspondingly cleaner, because the clamp it runs into
//   sits where a 1N4148 pair's clamp actually sits. No compensating pre-gain was
//   added (see docs §36 for the measured level/THD consequence and why).
//
// STAGE 3 — tone / output (RAT "Filter" + "Volume").
//   * FILTER knob -> one-pole passive low-pass cutoff, LOG-swept. RAT convention:
//     clockwise (knob -> 1) = DARKER. knob 0 -> kFilterMaxHz (20 kHz, ~open),
//     knob 1 -> kFilterMinHz (500 Hz, dark); knob 0.5 ~ 3.16 kHz.
//   * LEVEL knob -> the 100 k LOGARITHMIC volume pot's own law (0 = silence,
//     1 = unity). FIXED 2026-08-10, docs §67, and only as part of the
//     lineup-wide slice §66.3 said it had to be: all five dirt pedals' output
//     pots moved together, because fixing this one alone re-stages the RAT
//     against four siblings still on the wrong law and undoes §36 by a knob law
//     (§66.3 measured exactly that: -11.61 dBFS, the quietest of the five,
//     reproducing the pre-§36 staging to within 0.4 dB).
//     The law is the BARE audio taper — the netlist annotates R14 as
//     "volume pot (100k, logarithmic)", it is driven by the 2N5485 source
//     follower (a few hundred ohms, law-neutral) and its wiper feeds the output
//     jack unloaded, so there is no divider correction to make. Derivation,
//     sources and the per-pedal differences are in OutputPotTaper.h; the ledger
//     entry `rat-level-pot-linear-not-log` is closed.
//     Measured here: -5.21 dB at the shipped 0.8 default, 11.920 % at half
//     rotation (was 50.0 %).
//
// M2 — antialiasing. Stage 2 (and ONLY stage 2, the nonlinearity) now runs
// oversampled through a polyphase halfband cascade (1x/2x/4x/8x, default 4x);
// stages 1 and 3 are linear and stay at the base rate. The WDF capacitor is
// prepared at the OVERSAMPLED rate so its HF corner lands correctly. An
// experimental first-order ADAA memoryless clipper is selectable as an alternate
// stage 2 for measurement (see DiodeClipperADAA.h); the production default is
// WDF + oversampling.
//
// M6.5 — LM308 op-amp model (fizz fix). The M1..M6.1 gain stage used an IDEAL
// op-amp: infinite bandwidth and slew rate passed razor edges straight to the
// diode clamp, which aliases/fizzes — a cranked digital RAT sounded fizzy where
// a real one is thick. The real ProCo RAT's LM308 has TWO limits the model now
// reproduces (see LM308Stage.h), placed at the op-amp OUTPUT node — after the
// frequency-dependent gain, before the shunt-diode clamp, INSIDE oversampling:
//   (a) gain-tracking closed-loop bandwidth  f_c = GBW / A_noise  (a one-pole LP
//       whose corner collapses toward a few hundred Hz as DISTORTION rises), and
//   (b) slew-rate limiting (~0.3 V/us) rounding the steep edges.
// It is the pedal's fixed identity (no user knob); setIdealOpAmp(true) bypasses
// it for A/B / aliasing measurement only.

#include "clipper/dsp/RatModel.h"

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/DiodeClipperADAA.h"
#include "clipper/dsp/LM308Stage.h"
#include "clipper/dsp/OutputPotTaper.h"
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

// --- Stage 1 constants ---
constexpr float kDistMinDb = 0.0f;
// Max pre-clip gain. The real RAT non-inverting stage reaches 1 + P1/(R1||R2)
// with P1 = 100 k DISTORTION pot at max => +67.3 dB at the HF plateau (see the
// shaping network below). We cap at +66 dB (essentially the plateau) so a
// cranked knob actually slams the diodes — previously capped at +54 dB, which
// (with real interface-level input) left the signal short of the diode knee.
constexpr float kDistMaxDb = 66.0f;

// Pre-clip frequency shaping — the ProCo RAT LM308 non-inverting gain stage,
// re-voiced (M6.1) against the real circuit rather than a single 320 Hz shelf.
//
//   A(s) = 1 + Rf/Zg,  Zg = two series-RC legs to ground (from the inverting
//   input), in PARALLEL:
//     leg 1:  R1 = 560 Ohm + C1 = 4.7 uF  -> pole  ~60.5 Hz
//     leg 2:  R2 =  47 Ohm + C2 = 2.2 uF  -> pole ~1539 Hz
//   Rf = 100 k DISTORTION pot at max.
//
// Gain RISES with frequency in two steps and falls toward UNITY (not a fixed
// shelf floor) below ~60 Hz. Normalized to unity at the HF plateau (so the
// DISTORTION knob's dB sets that plateau), the transfer reduces EXACTLY to
//   H_shape(x) = c0*x + g1*(x - LP1(x)) + g2*(x - LP2(x))
//              = x - g1*LP1(x) - g2*LP2(x)      (since c0 + g1 + g2 == 1)
// two one-pole low-passes at the leg corners. Constants below (Ainf = 1 +
// Rf/R1 + Rf/R2 = 2307.23):
//   g1 = (Rf/R1)/Ainf,  g2 = (Rf/R2)/Ainf.
// At DC the residual is c0 = 1/Ainf (~ -67 dB rel. plateau) — the circuit's
// unity DC gain. *Approximation:* the shape is fixed at Rf = 100 k; the real
// pot-dependent shape flattens at low DISTORTION (same spirit as before).
constexpr double kShapeLeg1Hz = 60.4692;    // 1/(2*pi*R1*C1)
constexpr double kShapeLeg2Hz = 1539.2161;  // 1/(2*pi*R2*C2)
constexpr float kShapeG1 = 0.0773964f;      // (Rf/R1)/Ainf  (560-Ohm leg)
constexpr float kShapeG2 = 0.9221702f;      // (Rf/R2)/Ainf  (47-Ohm leg)

// --- LM308 op-amp model (M6.5) ---
// GBW: the LM308 with its ~30 pF dominant-pole compensation has a documented
// unity-gain bandwidth of ~1 MHz (National LM308 datasheet; the "0.5-1 MHz"
// range). We take GBW = 1.0 MHz. The closed-loop corner is GBW / A_noise, so at
// the +66 dB plateau (A ~ 1995) it collapses to ~500 Hz — the "thick, not fizzy"
// cranked-RAT behaviour — and at unity gain it sits at 1 MHz (far above audio;
// clamped to Nyquist in LM308Stage, i.e. transparent). A_noise is taken as the
// (frequency-independent) plateau gain = the smoothed pre-gain, refreshed per
// chunk; the real network's frequency-dependent noise gain is a documented
// simplification (same spirit as the fixed-Rf shaping).
constexpr double kOpAmpGbwHz = 1.0e6;
// Slew rate. The LM308's slew rate is set by its EXTERNAL compensation capacitor
// (bandwidth and slew rate both scale as 1/Cc), so the figure is only meaningful
// together with the Cc the pedal actually fits — and the RAT fits the datasheet's
// STANDARD 30 pF (C1 in the ProCo netlist; see the STAGE-1 source note above).
// The standard-compensation typical is 0.3 V/us, i.e. 0.3e6 V/s in our
// 1.0f == 1.0 V convention. That is what ships.
//
// Un-fitted 2026-08-01 (docs §66). Until then this comment justified the value
// as "(0.15-0.3 V/us is the cited band; 0.3 keeps note attack alive while still
// killing the razor edges - measured)" — i.e. a number picked out of a band by
// tonal outcome, which is the fit this project forbids. The band is not a
// tolerance: the 0.15 V/us figure that circulates belongs to the LM308H (the
// metal-can part), not to the LM308N the RAT uses with 30 pF. The two are
// audibly distinguishable HERE and testSlewInModel asserts the difference — at
// 0.3 V/us a 0.30 V low-E pluck never reaches the clamp (peak demand 74.5 % of
// the limit) and at 0.15 V/us it does. Provenance caveat, recorded rather than
// dressed up: no datasheet PDF was reachable from this container (ti.com's
// mirror, mit.edu, onsemi, studylib and direnc all 403 at CONNECT), so the
// figure is search-summary-grade; the 30 pF it is conditioned on is from the
// netlist and is solid.
//
// What the limiter is worth, measured (docs §66.4): NOTHING below ~1 kHz at any
// realistic level — a 220 Hz tone, a 1 kHz tone and a low-E pluck render
// bit-identically with the clamp removed — and 0.36 dB of level plus 134 Hz of
// spectral centroid on a 4186 Hz tone, where it binds on 93-98 % of oversampled
// samples. It is small because the two LM308 behaviours are IN SERIES and the
// bandwidth pole above (GBW/A = 4.9 kHz at DISTORTION 0.7, 501 Hz at 1.0) has
// already removed most of the slope demand before the clamp sees it.
constexpr double kOpAmpSlewVoltsPerSec = 0.3e6;

// --- Stage 2 constants ---
constexpr double kRs = 1.0e3;    // series/source resistance (Ohm)
constexpr double kCp = 10.0e-9;  // shunt capacitance (F)
constexpr double kDiodeIs = 2.52e-9;   // reverse saturation current (A)  [SPICE IS]
constexpr double kDiodeVt = 25.85e-3;  // thermal voltage (V)
// Emission / ideality factor. The 1N4148 SPICE model is `IS=2.52n N=1.752`; both
// numbers come from the same model card and taking one without the other is not a
// simplification, it is a different diode (audit finding 15). chowdsp_wdf carries it
// through DiodePairT's fourth argument as Vt_eff = n*Vt = 45.29 mV — the same
// arithmetic GoldModel already uses for its germanium pair (kGeIdeality = 1.3).
// BjtStage.h has always had this right (nVt = 0.0453, n ~ 1.75).
constexpr double kDiodeIdeality = 1.752;

// --- Stage 3 constants ---
constexpr double kFilterMinHz = 500.0;    // knob = 1 (dark)
constexpr double kFilterMaxHz = 20000.0;  // knob = 0 (bright)

// --- Smoothing ---
constexpr double kSmoothSeconds = 0.005;  // ~5 ms, same as M0 gain smoothing

// NaN-rejecting knob clamp (ParamGuard.h). The former in-line
// `v < 0 ? 0 : (v > 1 ? 1 : v)` was transparent to NaN, and one NaN latched
// permanently in the smoothers / WDF cap state — audit finding 1.
float clamp01(float v) { return clampParam01(v); }

// One-pole low-pass smoothing coefficient for a given cutoff.
float onePoleCoeff(double cutoffHz, double sampleRate) {
    const double a = 1.0 - std::exp(-kTwoPi * cutoffHz / sampleRate);
    return static_cast<float>(std::clamp(a, 0.0, 1.0));
}
}  // namespace

struct RatModel::Impl {
    double sampleRate = 44100.0;
    int maxBlockSize = 128;
    int osFactor = 4;            // M2 default oversampling
    int stage2Mode = RatModel::STAGE2_WDF;
    bool idealOpAmp = false;     // M6.5: true bypasses the LM308 model (measurement)
    bool slewLimit = true;       // §66: false keeps the BW pole, drops the slew clamp

    // Knob smoothers (mapped physical values, not raw knob positions).
    OnePoleSmoother preGain;    // linear pre-clip gain
    OnePoleSmoother filterCoef;  // one-pole LP coefficient a
    OnePoleSmoother level;       // linear output gain

    // Stage 1 shaping: two first-order low-passes (one per RAT feedback leg)
    // whose outputs are subtracted from the dry signal (see the constants
    // above). Coeffs are fixed after prepare; states carry across blocks.
    float shape1Coef = 0.0f;  // 560-Ohm leg, ~60.5 Hz
    float shape2Coef = 0.0f;  // 47-Ohm leg, ~1539 Hz
    float shape1State = 0.0f;
    float shape2State = 0.0f;

    // Stage 3 low-pass state.
    float loPassState = 0.0f;

    // M2: oversampling cascade for the nonlinear stage, plus the ADAA alternate.
    Oversampler os;
    DiodeClipperADAA adaa;
    std::vector<float> stage1Buf;  // base-rate stage-1 output (sized in prepare)

    // M6.5: LM308 op-amp model (gain-tracking closed-loop LP + slew limiter),
    // run at the oversampled rate on the op-amp output before the diode clamp.
    LM308Stage opAmp;

    // Stage 2: WDF diode-clipper tree (double precision for stability).
    // Declaration order matters: children before the adaptors that reference
    // them, root last.
    chowdsp::wdft::ResistiveVoltageSourceT<double> Vs { kRs };
    chowdsp::wdft::CapacitorT<double> Cp { kCp, 48000.0 };
    chowdsp::wdft::WDFParallelT<double, decltype(Vs), decltype(Cp)> P1 { Vs, Cp };
    chowdsp::wdft::DiodePairT<double, decltype(P1)> diodes {
        P1, kDiodeIs, kDiodeVt, kDiodeIdeality };

    // Re-prepare the stage-2 nonlinearity for the current oversampled rate and
    // reset its state. Called on prepare() and on any oversampling change.
    void reprepareStage2() {
        os.setFactor(osFactor);
        const double osRate = sampleRate * os.factor();
        Cp.prepare(osRate);  // WDF cap runs at the OVERSAMPLED rate
        Vs.setVoltage(0.0);
        adaa.setKnee(DiodeClipperADAA::kDefaultVk);
        adaa.reset();
        // LM308 op-amp model runs at the oversampled rate too (it sits between
        // the gain stage and the diode clamp). Corner is refreshed per chunk from
        // the smoothed pre-gain in processChunk().
        opAmp.prepare(osRate, kOpAmpGbwHz, opAmpSlewRate());
    }

    // The slew rate handed to LM308Stage: the device's own figure, or a value so
    // large the clamp can never bind (measurement path — see setSlewLimit). Not a
    // second constant: kNoSlew is "no limiter", not "a faster op-amp".
    double opAmpSlewRate() const {
        constexpr double kNoSlew = 1.0e30;
        return slewLimit ? kOpAmpSlewVoltsPerSec : kNoSlew;
    }

    // Map a FILTER knob position (0 = bright .. 1 = dark) to a cutoff in Hz,
    // log-swept, then to a one-pole coefficient.
    float filterKnobToCoef(float knob) const {
        const double k = static_cast<double>(clamp01(knob));
        const double fc = kFilterMaxHz * std::pow(kFilterMinHz / kFilterMaxHz, k);
        return onePoleCoeff(fc, sampleRate);
    }
};

RatModel::RatModel() : impl_(std::make_unique<Impl>()) {}
RatModel::~RatModel() = default;

void RatModel::prepare(double sampleRate, int maxBlockSize) {
    Impl& d = *impl_;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    d.maxBlockSize = maxBlockSize > 0 ? maxBlockSize : 128;

    // Smoothers (base rate — stages 1 and 3 are not oversampled).
    d.preGain.prepare(kSmoothSeconds, d.sampleRate);
    d.filterCoef.prepare(kSmoothSeconds, d.sampleRate);
    d.level.prepare(kSmoothSeconds, d.sampleRate);

    // Stage 1 shaping low-passes (fixed corners).
    d.shape1Coef = onePoleCoeff(kShapeLeg1Hz, d.sampleRate);
    d.shape2Coef = onePoleCoeff(kShapeLeg2Hz, d.sampleRate);
    d.shape1State = 0.0f;
    d.shape2State = 0.0f;

    // Stage 3.
    d.loPassState = 0.0f;

    // M2: allocate the oversampling scratch for the worst case (8x) once, here,
    // plus the base-rate stage-1 buffer used before upsampling.
    d.os.prepare(d.maxBlockSize);
    d.stage1Buf.assign(static_cast<size_t>(d.maxBlockSize), 0.0f);

    // Stage 2 WDF/ADAA: prepare at the oversampled rate.
    d.reprepareStage2();
}

void RatModel::setOversampling(int factor) {
    Impl& d = *impl_;
    d.osFactor = factor;
    d.reprepareStage2();
}

void RatModel::reset() {
    Impl& d = *impl_;
    // Smoothers first: `value += coeff*(target - value)` can never climb out of a
    // poisoned value, so the recovery path has to overwrite it.
    d.preGain.reset();
    d.filterCoef.reset();
    d.level.reset();
    d.shape1State = 0.0f;
    d.shape2State = 0.0f;
    d.loPassState = 0.0f;
    d.os.reset();
    // reprepareStage2() re-derives the stage-2 coefficients at the CURRENT rate and
    // factor and resets the WDF cap / ADAA / LM308 state. It re-runs the same
    // coefficient math prepare() does but allocates nothing (os.setFactor and
    // LM308Stage::prepare are both allocation-free) — no DC solve, no settling.
    d.reprepareStage2();
}

int RatModel::oversampling() const { return impl_->os.factor(); }

int RatModel::latencySamples() const { return impl_->os.latencySamples(); }

void RatModel::setStage2Mode(int mode) {
    impl_->stage2Mode = (mode == STAGE2_ADAA) ? STAGE2_ADAA : STAGE2_WDF;
    impl_->adaa.reset();
}

int RatModel::stage2Mode() const { return impl_->stage2Mode; }

void RatModel::setIdealOpAmp(bool ideal) {
    impl_->idealOpAmp = ideal;
    impl_->opAmp.reset();
}

bool RatModel::idealOpAmp() const { return impl_->idealOpAmp; }

void RatModel::setSlewLimit(bool enabled) {
    Impl& d = *impl_;
    if (d.slewLimit == enabled) return;
    d.slewLimit = enabled;
    // Re-derive the clamp threshold at the CURRENT oversampled rate and reset the
    // op-amp state. LM308Stage::prepare allocates nothing and parks the corner at
    // unity noise gain; processChunk() re-aims it from the smoothed pre-gain on
    // the very next chunk, so nothing else has to be re-prepared.
    d.opAmp.prepare(d.sampleRate * d.os.factor(), kOpAmpGbwHz, d.opAmpSlewRate());
}

bool RatModel::slewLimit() const { return impl_->slewLimit; }

void RatModel::setParameter(int paramId, float value) {
    Impl& d = *impl_;
    const float knob = clamp01(value);
    switch (paramId) {
        case PARAM_DISTORTION: {
            const float db = kDistMinDb + (kDistMaxDb - kDistMinDb) * knob;
            d.preGain.setTarget(std::pow(10.0f, db / 20.0f));
            break;
        }
        case PARAM_FILTER:
            d.filterCoef.setTarget(d.filterKnobToCoef(knob));
            break;
        case PARAM_LEVEL:
            // The 100 k LOG volume pot's bare audio taper — unloaded wiper, so
            // no divider correction (docs §67; OutputPotTaper.h carries the
            // netlist annotation and the topology trace).
            d.level.setTarget(static_cast<float>(pot::ratLevel(knob)));
            break;
        default:
            break;
    }
}

void RatModel::process(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;
    // Chunk into <= maxBlockSize pieces so the fixed oversampling scratch is
    // never overrun regardless of the caller's block size (the render tool and
    // tests hand us the whole signal at once). No allocation on this path.
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(d.maxBlockSize, numFrames - off);
        processChunk(in + off, out + off, n);
        off += n;
    }
}

void RatModel::processChunk(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;
    assert(numFrames <= d.maxBlockSize && "chunk exceeds maxBlockSize");
    const float c1 = d.shape1Coef;
    const float c2 = d.shape2Coef;

    // --- Stage 1 (base rate, linear): pre-clip shaping + variable gain. ---
    // Two-corner RAT feedback voicing: shaped = x - g1*LP1(x) - g2*LP2(x)
    // (unity at HF, rising through the two leg corners, -> unity DC). See the
    // stage-1 constants above. Computed into stage1Buf so in/out may alias.
    for (int i = 0; i < numFrames; ++i) {
        const float x = in[i];
        // Anti-denormal (Denormal.h): a one-pole ringing toward silence asymptotes
        // and STICKS in the float subnormal range — flush the state at -600 dB.
        d.shape1State = flushDenormal(d.shape1State + c1 * (x - d.shape1State));
        d.shape2State = flushDenormal(d.shape2State + c2 * (x - d.shape2State));
        const float shaped = x - kShapeG1 * d.shape1State - kShapeG2 * d.shape2State;
        d.stage1Buf[static_cast<size_t>(i)] = shaped * d.preGain.next();
    }

    // --- Stage 2 (oversampled, nonlinear): upsample -> LM308 -> clip -> down. ---
    d.os.upsample(d.stage1Buf.data(), numFrames);
    float* w = d.os.buffer();
    const int osN = d.os.bufferLength();

    // M6.5: LM308 op-amp model (gain-tracking closed-loop bandwidth + slew limit)
    // at the op-amp output, before the diode clamp. The closed-loop corner tracks
    // the current smoothed pre-gain (noise gain), refreshed once per chunk — the
    // 5 ms pre-gain smoothing already provides the glide, so a per-chunk corner
    // update is click-free (control-rate, like AmpModel's 32-sample coeffs).
    if (!d.idealOpAmp) {
        d.opAmp.setNoiseGain(d.preGain.value());
        for (int i = 0; i < osN; ++i) w[i] = d.opAmp.processSample(w[i]);
    }

    if (d.stage2Mode == STAGE2_ADAA) {
        for (int i = 0; i < osN; ++i)
            w[i] = d.adaa.processSampleADAA(w[i]);
    } else {
        for (int i = 0; i < osN; ++i) {
            d.Vs.setVoltage(static_cast<double>(w[i]));
            d.diodes.incident(d.P1.reflected());
            d.P1.incident(d.diodes.reflected());
            w[i] = static_cast<float>(chowdsp::wdft::voltage<double>(d.Cp));
            // Anti-denormal (Denormal.h): the WDF shunt cap's wave state is the
            // network's recursive memory. On silence it rings down into DOUBLE
            // subnormals and sticks there — the RAT's float output stays clean (the
            // double->float cast of a subnormal is 0.0f) but the CPU pays for every
            // sample: measured 328 ms of signal vs 658 ms of silence (2.01x) per 10 s
            // before this flush. Flushed AFTER the voltage above is read, so this
            // sample is bit-identical to the unguarded network. Finding 11, docs §33.
            flushDenormalWdfCapacitor(d.Cp);
        }
    }
    d.os.downsample(out, numFrames);  // -> out (base rate)

    // --- Stage 3 (base rate, linear): tone low-pass then level. ---
    for (int i = 0; i < numFrames; ++i) {
        const float a = d.filterCoef.next();
        d.loPassState = flushDenormal(d.loPassState + a * (out[i] - d.loPassState));
        out[i] = d.loPassState * d.level.next();
    }
}

}  // namespace clipper::dsp
