// Clipper — portable DSP core.
//
// GateModel (M13.6a) — the "Curfew" NOISE GATE. Pedal type `gate`.
//
// The lineup's first UTILITY: it does not make a sound, it takes one away. It is
// what makes a high-gain rig playable — silence between phrases without the note
// itself losing its front edge.
//
// ============================================================================
//  IT IS THE SECOND CONSUMER OF M13.1's DETECTOR — that is the point
// ============================================================================
// ADR 019 built `CompressorEngine` config-parameterized from its first line and
// named the noise gate as a consumer of its detector. This model uses the SAME
// `SidechainDetector` — the same full-wave clamp/rectifier and the same envelope
// integrator, with nothing changed but component values. No second envelope
// follower was written.
//
// What it does NOT reuse, and why (the full finding is docs §61.2, and the
// `CompressorEngine.h` banner is corrected in the same slice):
//
//   * `ControlMap` — three resistor values that turn the envelope node into an
//     OTA BIAS CURRENT. A gate has no rheostat, no cell pin and no control
//     current. Its control law is a Schmitt comparator plus an attack/hold/decay
//     ramp into a VCA. That is this file's `GateControl`, NOT a widening of the
//     compressor's struct.
//   * `detectorFromOutput` — for a gate, feed-back is not a variant, it is
//     BROKEN: once the gate closes, a detector tasting the output sees silence
//     and can never re-open. `clipper_gate_tests` measures the latch rather than
//     asserting the argument.
//
// ============================================================================
//  THE CIRCUIT  (a DOCUMENTED RECONSTRUCTION — read docs §61.1 first)
// ============================================================================
// The reference is a Boss NS-2-class noise suppressor. NO SCHEMATIC WAS
// REACHABLE from this container (docs §61.1 lists every host that 403'd), so the
// TOPOLOGY is sourced and almost every component value is chosen under stated
// constraints. Do not re-tune any of them toward a sound; find the schematic.
//
//   in ──┬─────────────────────────────────► VCA (×g) ──[Cout]──► out
//        │                                      ▲
//        └─[Cin]─► sidechain amp ─► THRESHOLD ──┼── the DETECTOR ──┐
//                  (fixed gain)     attenuator  │   (shared)       │
//                                       ▲       │                  ▼
//                            hysteresis ┘       │            envelope node
//                            (+6 dB while open) │                  │
//                                               │                  ▼
//                                        gain ramp ◄── Schmitt comparator
//                                     attack/hold/decay
//
// THE SIGNAL PATH NEVER TOUCHES THE DETECTOR. That is what a gate is, and it is
// why a gate is transparent when open where a compressor never is.
//
// Platform-free (C++17, no OS/browser/Emscripten). Convention: 1.0f == 1.0 V.

#ifndef CLIPPER_DSP_GATE_MODEL_H
#define CLIPPER_DSP_GATE_MODEL_H

#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/SidechainDetector.h"

namespace clipper::dsp {

// The gate's own control stage — everything between the detector's envelope node
// and the VCA's gain. This is the struct that `ControlMap` is NOT (see above).
struct GateControl {
    // The sidechain amp's fixed voltage gain, ahead of the THRESHOLD attenuator.
    double sidechainGain;
    // The THRESHOLD pot's total range, in dB of input level. The pot is a LOG
    // (audio-taper) part — the standard threshold control — so the attenuation is
    // dB-linear in knob rotation and the threshold law is too.
    double thresholdRangeDb;
    // The comparator's reference, as a fraction of the detector's supply rail.
    double refFraction;
    // HYSTERESIS, and the place it is taken. A Schmitt trigger is positive
    // feedback; here the feedback path is the SIDECHAIN ATTENUATOR rather than
    // the comparator's own input divider — while the gate is open the sidechain
    // sees this many dB more signal, so the input must fall this far below the
    // OPEN threshold before the comparator will close. Taking it in the sidechain
    // rather than at the comparator node is what makes the gap a defined number
    // of dB: the detector's transfer is exponential, so a fixed VOLTAGE offset at
    // the comparator would be worth a fraction of a dB (docs §61.5).
    double hysteresisDb;
    // The VCA's control-node time constants. ATTACK and HOLD are FIXED because
    // the reference exposes no control for either; DECAY is the knob.
    double attackSeconds;
    double holdSeconds;
    double decayMinSeconds;
    double decayMaxSeconds;
    // The VCA's off-isolation — a real gain cell does not reach zero. The gate's
    // noise reduction can never exceed this.
    double closedGain;
};

class GateModel {
public:
    // Slots mirror every other pedal's positional triple so the chain ABI stays
    // pedal-agnostic. The reference gate has TWO knobs — slot 1 is carried and
    // unused, exactly as the compressor and the phaser carry theirs. (Its third
    // control, the MODE switch, changes what the FOOTSWITCH does, not the audio;
    // this board's footswitch is bypass on every pedal. See docs §61.3.)
    enum ParamId : int { PARAM_THRESHOLD = 0, PARAM_UNUSED = 1, PARAM_DECAY = 2 };

    GateModel();

    void prepare(double sampleRate, int maxBlockSize);

    // NOT OVERSAMPLED, and that is a MEASURED decision, not an omission
    // (docs §61.7). The gate's signal path is a MULTIPLY — the only nonlinearity
    // in the pedal is in the sidechain, and the sidechain reaches the output only
    // through a control ramp whose own corner is 400 Hz, so nothing it makes can
    // fold back into the band. Measured on the worst stimulus available (a 9 kHz
    // tone forced open and shut at 4 Hz): the non-harmonic floor is −176.93 dB at
    // 1×, −162.18 dB at 4× — i.e. oversampling makes it very slightly WORSE, and
    // costs 72 samples of latency and roughly double the CPU. So this is the
    // phaser's arrangement (a linear stage): the ABI call is accepted and
    // IGNORED, the factor is 1, and the latency is exactly 0. A utility pedal
    // that adds 1.5 ms to a whole board for nothing would be a bug.
    void setOversampling(int factor);
    int oversampling() const { return 1; }
    int latencySamples() const { return 0; }

    void setParameter(int paramId, float value);

    // Process numFrames mono, in -> out (may alias). 1.0f == 1.0 V.
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1): clear every recursive state and re-park at
    // the CACHED quiescent point. Never re-solves. Allocation-free.
    void reset();

    // --- Measurement hooks (NOT user knobs) ---------------------------------
    // TRUE while the comparator says the gate is open. The tests count EDGES of
    // this to measure chatter.
    bool isOpen() const { return open_; }
    // The VCA's current gain (linear). 1.0 = fully open, `closedGain` = shut.
    double gainNow() const { return gain_; }
    // The detector's envelope node, in volts. Rests at the supply rail.
    double envelopeVolts() const { return sc_.envelopeVolts(); }
    // The comparator's two trip points, in envelope volts, at the CURRENT knob.
    double openTripVolts() const;
    double closeTripVolts() const;
    // The gate's own stated threshold law, in dBV peak, for a knob position. This
    // is the LABEL — what the pedal claims. `clipper_gate_tests` measures the
    // level at which the gate really opens and compares against it, which is only
    // a real test because the two are computed by different code from different
    // quantities (a pot law vs. a rendered signal through the detector).
    double statedThresholdDbV(double knob) const;
    // Feed-BACK detector tap. NOT a product mode and NOT a config field: a gate
    // that tastes its own output latches shut forever, and the test proves that
    // by measurement instead of by comment. Default false (feed-forward).
    void setDetectorFromOutput(bool v) { detectorFromOutput_ = v; }
    // The largest absolute value held by any recursive state whose rest value is
    // ZERO. The denormal suite asserts this is EXACTLY 0.0 after a silent tail.
    double maxAbsRestingState() const;

    // The shipped control constants, so a test can quote the design rather than
    // re-typing it.
    const GateControl& control() const { return ctl_; }

private:
    void processChunk(const float* in, float* out, int numFrames);
    void rebuildRates();
    void refreshKnobs(double thresholdKnob, double decayKnob);

    Sidechain det_{};
    NpnDevice npn_{};
    DiodeDevice diode_{};
    GateControl ctl_{};
    SidechainDetector sc_;

    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;

    // --- Smoothed knobs -----------------------------------------------------
    // THRESHOLD is smoothed as the WIPER (the physical quantity), never as the
    // derived attenuation; DECAY likewise, and its time constant is rebuilt from
    // the smoothed wiper once per BASE sample.
    OnePoleSmoother threshWiper_;
    OnePoleSmoother decayWiper_;
    bool snapPending_ = true;  // first process() after prepare() snaps (docs §35)

    // --- Base-rate linear state (rests at ZERO -> guarded) ------------------
    double inHpX1_ = 0.0, inHpY1_ = 0.0, inHpR_ = 0.0;
    double outHpX1_ = 0.0, outHpY1_ = 0.0, outHpR_ = 0.0;

    // --- Control state ------------------------------------------------------
    // Derived once per base sample from the smoothed wipers.
    double scAtten_ = 1.0;      // the THRESHOLD attenuator (closed-state value)
    double hystFactor_ = 1.0;   // ×10^(hysteresisDb/20) while OPEN
    double vRef_ = 0.0;         // the comparator reference (volts)
    double attackCoef_ = 0.0;
    double decayCoef_ = 0.0;
    int holdSamples_ = 0;
    int holdRemaining_ = 0;
    bool open_ = false;
    bool detectorFromOutput_ = false;
    // The VCA gain. Rests at `closedGain` — a real operating point, NOT zero, so
    // it is deliberately NOT denormal-guarded (ADR 006's scope rule).
    double gain_ = 1e-4;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_GATE_MODEL_H
