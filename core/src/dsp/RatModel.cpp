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
//     = [0, +54] dB. The real RAT non-inverting gain is 1 + P1/Rg with P1 the
//     100 k DISTORTION pot and Rg ~ 47 Ohm at midband, i.e. unity up to ~+66 dB;
//     we cap at +54 dB so the offline model stays well-behaved without the
//     LM308's slew limiting (slew limiting = deliberate future refinement, M2+).
//   * Pre-clip frequency shaping: the RAT feedback network (P1 = 100 k in the
//     feedback, and a 47 Ohm + 2.2 uF series leg to ground, with a small ~100 pF
//     cap across the feedback) makes the stage gain RISE toward the mids/highs
//     with corners very roughly in the 100-800 Hz region — this is what makes
//     the RAT tight/aggressive rather than woolly. We model it as a first-order
//     high-shelf: unity above the corner, bass shelved down to kShelfBassGain.
//     Corner kShelfCornerHz = 320 Hz, bass gain 0.30 (~ -10.5 dB) are chosen to
//     sit in that documented band. (Approximation: we do NOT reproduce the exact
//     two-pole feedback transfer function; a single shelf captures the audible
//     "cut the lows before clipping" character. Exact component-accurate EQ is a
//     future refinement.) NOTE: op-amp slew limiting is intentionally NOT
//     modelled here (future refinement).
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
//     Diode: Is = 2.52e-9 A, Vt = 25.85 mV, 1 diode per side -> silicon knee
//            ~ +/-0.6 V (1N914/1N4148 datasheet-ish values).
//   The diode pair is the Werner et al. improved model shipped by the library
//   (DiodeQuality::Best). Output is the voltage across the shunt cap = the
//   clipping-node voltage.
//
// STAGE 3 — tone / output (RAT "Filter" + "Volume").
//   * FILTER knob -> one-pole passive low-pass cutoff, LOG-swept. RAT convention:
//     clockwise (knob -> 1) = DARKER. knob 0 -> kFilterMaxHz (20 kHz, ~open),
//     knob 1 -> kFilterMinHz (500 Hz, dark); knob 0.5 ~ 3.16 kHz.
//   * LEVEL knob -> clean linear output gain, identity map [0, 1] (0 = silence,
//     1 = unity). A proper audio-taper volume law is a future refinement.
//
// No oversampling / ADAA in M1 (that is M2): high gain WILL alias/fizz.

#include "clipper/dsp/RatModel.h"

#include <chowdsp_wdf/chowdsp_wdf.h>

#include <algorithm>
#include <cmath>

namespace clipper::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586;

// --- Stage 1 constants ---
constexpr float kDistMinDb = 0.0f;
constexpr float kDistMaxDb = 54.0f;
constexpr double kShelfCornerHz = 320.0;  // pre-clip high-shelf corner
constexpr float kShelfBassGain = 0.30f;   // bass gain relative to treble (unity)

// --- Stage 2 constants ---
constexpr double kRs = 1.0e3;    // series/source resistance (Ohm)
constexpr double kCp = 10.0e-9;  // shunt capacitance (F)
constexpr double kDiodeIs = 2.52e-9;   // reverse saturation current (A)
constexpr double kDiodeVt = 25.85e-3;  // thermal voltage (V)

// --- Stage 3 constants ---
constexpr double kFilterMinHz = 500.0;    // knob = 1 (dark)
constexpr double kFilterMaxHz = 20000.0;  // knob = 0 (bright)

// --- Smoothing ---
constexpr double kSmoothSeconds = 0.005;  // ~5 ms, same as M0 gain smoothing

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// One-pole low-pass smoothing coefficient for a given cutoff.
float onePoleCoeff(double cutoffHz, double sampleRate) {
    const double a = 1.0 - std::exp(-kTwoPi * cutoffHz / sampleRate);
    return static_cast<float>(std::clamp(a, 0.0, 1.0));
}
}  // namespace

struct RatModel::Impl {
    double sampleRate = 44100.0;

    // Knob smoothers (mapped physical values, not raw knob positions).
    OnePoleSmoother preGain;    // linear pre-clip gain
    OnePoleSmoother filterCoef;  // one-pole LP coefficient a
    OnePoleSmoother level;       // linear output gain

    // Stage 1 shaping: first-order high-shelf state (a low-pass whose output is
    // the "bass" we partially subtract). shelfCoef is fixed after prepare.
    float shelfCoef = 0.0f;
    float shelfLpState = 0.0f;

    // Stage 3 low-pass state.
    float loPassState = 0.0f;

    // Stage 2: WDF diode-clipper tree (double precision for stability).
    // Declaration order matters: children before the adaptors that reference
    // them, root last.
    chowdsp::wdft::ResistiveVoltageSourceT<double> Vs { kRs };
    chowdsp::wdft::CapacitorT<double> Cp { kCp, 48000.0 };
    chowdsp::wdft::WDFParallelT<double, decltype(Vs), decltype(Cp)> P1 { Vs, Cp };
    chowdsp::wdft::DiodePairT<double, decltype(P1)> diodes { P1, kDiodeIs, kDiodeVt, 1.0 };

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

void RatModel::prepare(double sampleRate, int /*maxBlockSize*/) {
    Impl& d = *impl_;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;

    // Smoothers.
    d.preGain.prepare(kSmoothSeconds, d.sampleRate);
    d.filterCoef.prepare(kSmoothSeconds, d.sampleRate);
    d.level.prepare(kSmoothSeconds, d.sampleRate);

    // Stage 1 shelf corner (fixed).
    d.shelfCoef = onePoleCoeff(kShelfCornerHz, d.sampleRate);
    d.shelfLpState = 0.0f;

    // Stage 3.
    d.loPassState = 0.0f;

    // Stage 2 WDF: prepare cap at the real sample rate (resets state + impedance,
    // which propagates to the diode model).
    d.Cp.prepare(d.sampleRate);
    d.Vs.setVoltage(0.0);
}

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
    const float shelfCoef = d.shelfCoef;

    for (int i = 0; i < numFrames; ++i) {
        const float x = in[i];

        // --- Stage 1: pre-clip shaping (high-shelf) then variable gain. ---
        // bass = LP(x); shaped = x - (1 - bassGain) * bass  => unity above the
        // corner, kShelfBassGain below it.
        d.shelfLpState += shelfCoef * (x - d.shelfLpState);
        const float shaped = x - (1.0f - kShelfBassGain) * d.shelfLpState;
        const float g = d.preGain.next();
        const double v = static_cast<double>(shaped * g);  // volts into diodes

        // --- Stage 2: WDF diode clipper. ---
        d.Vs.setVoltage(v);
        d.diodes.incident(d.P1.reflected());
        d.P1.incident(d.diodes.reflected());
        const float clipped =
            static_cast<float>(chowdsp::wdft::voltage<double>(d.Cp));

        // --- Stage 3: tone low-pass (smoothed cutoff) then level. ---
        const float a = d.filterCoef.next();
        d.loPassState += a * (clipped - d.loPassState);
        out[i] = d.loPassState * d.level.next();
    }
}

}  // namespace clipper::dsp
