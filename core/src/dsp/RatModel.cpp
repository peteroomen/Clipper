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
//   * LEVEL knob -> clean linear output gain, identity map [0, 1] (0 = silence,
//     1 = unity). A proper audio-taper volume law is a future refinement.
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
// Slew rate: the LM308 is a slow op-amp, ~0.3 V/us with standard compensation
// (datasheet typical). Referred to our 1.0f == 1.0 V convention that is
// 0.3e6 V/s. Steep gain-stage edges are rate-limited before the diode clamp,
// softening the transitions that alias/fizz. (0.15-0.3 V/us is the cited band;
// 0.3 keeps note attack alive while still killing the razor edges — measured.)
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
        opAmp.prepare(osRate, kOpAmpGbwHz, kOpAmpSlewVoltsPerSec);
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
            d.level.setTarget(knob);  // identity map, 0..1 linear
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
