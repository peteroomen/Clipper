// Clipper — portable DSP core (M13.10, docs §70).
//
// DropModel — the "Cellar" POLYPHONIC DROP-TUNE pedal. Pedal type `drop`.
//
// The lineup's first PITCH effect, and the first voice in this repo that is NOT
// a circuit. There is no netlist for a DigiTech Drop — it is a DSP algorithm in
// a fixed-function processor — so §57's rule ("do not re-tune a constant toward
// a sound; find the schematic") cannot apply here.
//
// **IT IS REPLACED RATHER THAN DROPPED, because without it this becomes a pile
// of constants tuned until it sounds right — the exact failure §43/§50/§52 spent
// three slices undoing.** The replacement: every constant is justified against a
// measurement with an EXTERNAL reference — cents error against the exact
// frequency ratio, latency in samples, artifact energy in dB. A constant that can
// only be defended with "it sounds better here" does not ship. See PitchShifter.h,
// where two structural errors were found and fixed that way.
//
// ============================================================================
//  WHAT IS SOURCED, AND IT IS A PRODUCT SPEC RATHER THAN A CIRCUIT
// ============================================================================
//   * NINE selector positions: 1–7 semitones down, OCT, OCT + DRY.
//   * Only the OCT + DRY position blends dry. Every other position is 100 % WET —
//     this is a TUNING pedal, not a harmoniser, and that is a deliberate design
//     choice of the reference rather than an omission.
//   * Polyphonic: it shifts all six strings at once with no monophonic tracking.
//   * True bypass; a momentary/latching footswitch mode switch.
//
// Everything else — the window, the crossfade, the SOLA span — is this model's
// own and is derived by measurement in PitchShifter.h.
//
// ============================================================================
//  FAITHFUL: THERE IS NO MIX KNOB, AND THAT IS A DECISION (owner, 2026-08-18)
// ============================================================================
// A wet/dry blend would be useful for octave-down bass doubling, and the board's
// standing rule is against dead UI. But the reference has no such control, and
// adding one would be a departure ADR 020 requires be recorded with its cost.
// The owner chose faithful, so slots 1 and 2 are carried and unused — exactly as
// the compressor, the gate and the phaser carry theirs.
//
// The MOMENTARY/LATCHING switch is NOT shipped either, for the reason §61.3
// settled for the gate's MODE: it changes what the FOOTSWITCH does, and this
// board's footswitch is bypass on every pedal. Documented, not built.
//
// ============================================================================
//  LATENCY IS REPORTED AS ZERO, AND THAT IS DELIBERATE
// ============================================================================
// The shifter's read delay is a SAWTOOTH by construction — it sweeps across the
// window and re-seats — so there is no single group delay to hand a host. The
// mean is ~36 ms at 48 kHz. Reporting a fixed number would be a lie about a
// varying quantity, and compensating for it would misalign the dry path at the
// OCT + DRY position. So `latencySamples()` returns 0 and the real figure is
// documented here and asserted in the suite. Same call, same reason, as the BBD
// delay's (docs §60).
//
// NOT OVERSAMPLED. The signal path is a delay-line READ plus a crossfade
// multiply — linear and time-varying, with no nonlinearity anywhere — and the
// crossfade envelope moves at the GRAIN rate (≤ 10 Hz), nowhere near audio rate.
// That is the chorus/phaser case (§12), not §64's optical compressor (whose gain
// cell is multiplied by an audio-rate sidechain). The ABI call is accepted and
// IGNORED, the factor is 1 and the latency contribution is 0 — and the test
// suite measures the alias floor across 1/2/4/8x to prove oversampling buys
// nothing, rather than asserting it.
//
// Convention: 1.0f == 1.0 V. Platform-free C++17.

#ifndef CLIPPER_DSP_DROP_MODEL_H
#define CLIPPER_DSP_DROP_MODEL_H

#include <vector>

#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/PitchShifter.h"

namespace clipper::dsp {

class DropModel {
public:
    // Positional triple, like every other pedal, so the chain ABI stays
    // pedal-agnostic. Only slot 0 is real — see the banner.
    enum ParamId : int { PARAM_AMOUNT = 0, PARAM_UNUSED1 = 1, PARAM_UNUSED2 = 2 };

    // The reference's nine selector positions, in its own order.
    enum Position : int {
        DROP_1 = 0, DROP_2, DROP_3, DROP_4, DROP_5, DROP_6, DROP_7,
        DROP_OCT, DROP_OCT_DRY,
        POSITION_COUNT
    };

    // Semitones down for each position. OCT and OCT+DRY are both 12; they differ
    // only in whether the dry signal is summed.
    static int semitonesFor(int pos) {
        static const int k[POSITION_COUNT] = {1, 2, 3, 4, 5, 6, 7, 12, 12};
        return k[pos < 0 ? 0 : (pos >= POSITION_COUNT ? POSITION_COUNT - 1 : pos)];
    }
    static bool blendsDryAt(int pos) { return pos == DROP_OCT_DRY; }

    // The window the shifter runs at. DERIVED in PitchShifter.h — it is the
    // smallest window whose span constraint still admits the 50 ms SOLA search a
    // major triad needs, with margin.
    static constexpr double kWindowSeconds = 0.065;

    // Quantize a 0..1 knob to one of the nine detents. Exposed so the C ABI and
    // the tests agree on the mapping by construction rather than by copying it.
    static int positionFor(double knob01);

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);  // accepted and IGNORED — see the banner
    int latencySamples() const { return 0; }
    void setParameter(int paramId, float value);
    void reset();
    void process(const float* in, float* out, int numFrames);

    // --- Introspection (measurement / tests) --------------------------------
    int position() const { return position_; }
    double ratio() const { return shifter_.ratio(); }
    double meanDelaySamples() const { return shifter_.meanDelaySamples(); }
    double maxAbsRestingState() const { return shifter_.maxAbsRestingState(); }

private:
    void applyPosition();

    PitchShifter shifter_;
    double sampleRate_ = 44100.0;
    double amount_ = 0.0;   // knob; default is position 0 == ONE SEMITONE DOWN
    int position_ = DROP_1;
    // The dry sum at OCT + DRY is switched, not swept, so it is smoothed to keep
    // a position change from stepping the output (audit finding 6's rule).
    OnePoleSmootherD drySm_;
    std::vector<float> buf_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_DROP_MODEL_H
