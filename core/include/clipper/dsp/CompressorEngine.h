// Clipper — portable DSP core.
//
// CompressorEngine (M13.1): the shared COMPRESSOR engine, and the project's
// first DYNAMICS processor. It is written from the first line as a
// config-parameterized engine because the lineup wants TWO compressors — the
// CA3080 OTA box modelled here (`CompModel`, the Dyna Comp / Ross voice) and a
// later OPTICAL / LA-2A-style voice (M13.3). That is the exact shape
// `OverdriveEngine` has for the SD-1 and the TS (docs §21): one engine, two
// thin wrappers, zero duplicated DSP.
//
// ============================================================================
//  THE M13.3 SEAM — SETTLED 2026-08-01, AND THE ANSWER IS NO (docs §64.3,
//  ADR 025). The optical voice is NOT a config of this engine.
// ============================================================================
// This block used to predict that M13.3 would be "a config plus one
// `applyGainCell()` case", reusing `InputStage`, `DriveNetwork`, `LoadStage`,
// `Splitter`, `Sidechain` and `OutputStage` as-is. M13.3 is now built
// (`OptoModel` / `OptoCell`, docs §64) and that prediction was wrong. Corrected
// here rather than made true by widening this engine — ADR 019's own instruction.
//
// Of the eight config structs below, exactly ONE survives contact with an
// optical leveling amplifier:
//
//   * `OutputStage` — yes.
//   * `InputStage` — partly (a coupling corner, but no emitter follower).
//   * `DriveNetwork` — NO. It is the CA3080's differential input attenuator.
//     There is no such network in an optical compressor.
//   * `GainCellSpec` / `applyGainCell()` — NO, and this is the structural one:
//     this function returns a CURRENT into `LoadStage`. An optical cell is a
//     RESISTOR IN A DIVIDER — it returns no current at all, it changes a gain.
//     A second `case` here would have to mean something different by units.
//   * `LoadStage`, `Splitter`, `compliance_` — NO. All three are the Dyna Comp's
//     own stages and would have to be bypassed by a flag.
//   * `Sidechain` / `SidechainDetector` — NO, and it was MEASURED not argued.
//     An EL panel emits on both polarities, so in that pedal the PANEL is the
//     rectifier and the PHOTOCELL is the integrator: there is no envelope
//     capacitor anywhere in its control path for this component to be. The
//     substitution was built and run per ADR 023's procedure — the ratio curve
//     goes non-monotone and reaches 10.4:1, gain reduction at −30 dBV collapses
//     to 0.09 dB, and the voice's whole acceptance property (a program-dependent
//     release, 2.892x) goes to 1.000x, which is THIS pedal's own figure.
//   * `ControlMap` — NO (ADR 021 already said so, for the gate).
//
// So `OptoModel` is a STANDALONE model that holds the component genuinely shared
// (`OptoCell`, the photocell) — the shape `GateModel` already took one slice
// earlier, for the same reason. **This engine has one consumer, and that is what
// it is: the OTA compressor's engine.** Do not add a `kOpticalCell` case to
// `applyGainCell()` in a later slice; the seam was checked and it is not here.
//
// The general lesson, from ADR 025: "two pedals do the same JOB" is a much weaker
// signal than "two pedals contain the same SUB-CIRCUIT". A Dyna Comp and an LA-2A
// are both compressors and share no block; a Dyna Comp and an NS-2 do completely
// different jobs and share a detector exactly.
//
// ============================================================================
//  M13.6 SHIPPED, AND IT CORRECTED THIS BANNER — read before trusting the above
// ============================================================================
// This block used to say: "M13.6 (the noise GATE) is the OTHER named consumer:
// a gate is this same `Sidechain` feeding a different `ControlMap`." M13.6 has
// now been built (docs §61) and that sentence was HALF right, in the way ADR 019
// warned a seam written before its second consumer could be. Corrected, with the
// measurements in docs §61.2:
//
//   * RIGHT — the detector really is the shared part. The gate uses the SAME
//     full-wave clamp/rectifier + envelope integrator with nothing but different
//     component values, and no second envelope follower was written.
//   * WRONG (structure) — `Sidechain` was a config struct with NO CODE attached.
//     The detector itself lived in `CompressorEngine::detectorLeg()` and
//     `::advanceEnvelope()`, both PRIVATE, with their state in this class's
//     members. There was nothing a second consumer could hold. It is now the
//     standalone `SidechainDetector` (`SidechainDetector.h`), which BOTH pedals
//     own by value. The move is bit-identical for the compressor — verified by
//     render hash, docs §61.2.
//   * WRONG (`ControlMap`) — it is not a policy hook. It is three resistor
//     values (`potOhms`/`seriesOhms`/`cellPinV`) that turn an envelope voltage
//     into an OTA BIAS CURRENT. A gate has no rheostat, no cell pin and no
//     current: its control law is a Schmitt comparator plus an attack/hold/decay
//     ramp. `GateModel` therefore has its OWN control stage and this struct was
//     NOT widened to cover both — an engine that grows a field every time a voice
//     disagrees has become a union of two models (ADR 019's named risk).
//   * WRONG (`detectorFromOutput`) — for a gate, feed-back is not a variant, it
//     is broken: once the gate closes, a detector tapping the output sees silence
//     and can never re-open. Measured, not argued (docs §61.7). The field stays
//     OTA-only; the gate is feed-forward by construction.
//
// The rule that left for M13.3 — "reuse the DETECTOR, expect the CONTROL side to
// be your own" — was then TESTED by M13.3 and came out no on both halves. See the
// corrected M13.3 block above and docs §64.3. The gate remains this detector's
// second and (so far) last consumer.
//
// CROSS-SLICE NOTE (2026-07-31): a wah slice landing in parallel also builds an
// envelope follower. They were deliberately NOT shared while both were in
// flight. Unifying them is a named follow-up, not this engine's business.
//
// SETTLED 2026-08-01 (docs §58.8, ADR 023): the answer is NO — measured, not
// argued. `SidechainDetector` is a THRESHOLD detector (1.950 dB of proportional
// range on THIS pedal's own component values, open loop); this compressor's
// graded response is its FEEDBACK LOOP, not the detector. A wah has no gain to
// reduce, needs a proportional follower open loop, and measures 19.085 dB. The
// substitution was built and every §58.6 number regressed. The wah keeps its own
// follower and `SidechainDetector` was NOT widened. Read the "THIS IS A
// *THRESHOLD* DETECTOR" banner in `SidechainDetector.h` before M13.3.
//
// ============================================================================
//  THE CIRCUIT THIS CONFIG DESCRIBES  (see docs §59 for the sources)
// ============================================================================
// A Dyna Comp / Ross-family OTA compressor is a FEED-BACK compressor:
//
//    in --[Cin/Rin]--> emitter follower --> [drive network] --> CA3080 (-in)
//                                                                  |
//                                        Iout = Iabc·tanh(Vd/2Vt)  |
//                                                                  v
//                                        [R_load ∥ C_load] --> phase splitter
//                                                              |          |
//                                                    emitter (AUDIO OUT)   collector
//                                                              |          |
//                                                       [ the SIDECHAIN taps HERE ]
//                                                    clamp caps + diodes + Q3/Q4
//                                                                  |
//                                                     discharges the envelope cap
//                                                                  |
//                                                  Q5 follower + SUSTAIN rheostat
//                                                                  |
//                                                            Iabc  (back to the OTA)
//
// The detector tap is the thing online writeups get wrong most often, so it is
// a CONFIG FIELD and a measurement hook (`setDetectorTap`), not a comment: the
// Dyna Comp taps the gain cell's OUTPUT (feed-back), and `clipper_comp_tests`
// proves the claim is load-bearing by flipping it and watching the compression
// ratio collapse.
//
// Platform-free (C++17, no OS/browser/Emscripten). Convention: 1.0f == 1.0 V.

#ifndef CLIPPER_DSP_COMPRESSOR_ENGINE_H
#define CLIPPER_DSP_COMPRESSOR_ENGINE_H

#include "clipper/dsp/AsymSoftClipper.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/SidechainDetector.h"

namespace clipper::dsp {

// --- Per-voice configuration -------------------------------------------------

// Stage 1: the input coupling cap into the emitter follower's base network,
// collapsed to the one HP corner and the one gain the network actually has.
struct InputStage {
    double couplingHz;  // 1/(2π·Rtot·Cin) — the first bass rolloff
    double gain;        // the emitter follower's own (slightly sub-unity) gain
};

// Stage 2: the network that develops the OTA's DIFFERENTIAL input voltage. It is
// SECOND ORDER with two well-separated REAL poles and two real zeros, so it ships
// as a CASCADE OF TWO FIRST-ORDER SECTIONS rather than a direct-form biquad —
// docs §56.4b: a direct form of order >= 2 cannot be flushed to exact zero by the
// house one-liner, and its slowest pole here sits at 9.3 Hz (radius 0.99970 at
// 192 kHz), which is exactly the state-rounding case §56 measured as audible hiss.
struct DriveNetwork {
    double zeroHz[2];   // the two real zeros (Hz)
    double poleHz[2];   // the two real poles (Hz)
    double hfGain;      // |H| as f -> infinity (the s^2 coefficient ratio)
    // Sign: the signal drives the OTA's INVERTING input, so H is negative and the
    // whole pedal is phase-INVERTING. That is the circuit; it is not a bug.
};

// Stage 3: the variable-gain element.
struct GainCellSpec {
    // M13.3 adds kOpticalLdr here and one case in applyGainCell(). Nothing else.
    enum class Kind { kOtaTanh };
    Kind kind = Kind::kOtaTanh;
    double otaVt = 0.02585;  // OTA-only: the differential pair's thermal voltage
};

// Stage 4: what the cell's output CURRENT develops its voltage across, and the
// supply compliance that voltage runs into.
struct LoadStage {
    double rLoadOhms;   // R_load ∥ Z_in(next stage)
    double poleHz;      // 1/(2π·R_load·C_load)
    double swingUp;     // headroom above the bias rail before the cell runs out
    double swingDown;   // headroom below it (ASYMMETRIC on a single supply)
};

// Stage 5: the phase splitter. Equal collector and emitter resistors give two
// anti-phase copies of the same amplitude; the EMITTER leg is the audio output
// and BOTH legs drive the sidechain (that is what makes it full-wave).
struct Splitter {
    double gain;  // |A| of each leg (an emitter follower is slightly sub-unity)
};

// Stage 6: the detector. Its component values, its device cards and its code
// all live in `SidechainDetector.h` — SHARED with M13.6's noise gate (docs
// §61.2). The struct name `Sidechain` is unchanged, so every config below reads
// exactly as it did when M13.1 shipped.

// Stage 7: envelope node -> whatever the gain cell consumes. Here: a Q5 emitter
// follower, the SUSTAIN rheostat, a fixed series resistor, and the OTA's bias
// pin sitting one junction above the negative rail.
struct ControlMap {
    double potOhms;       // the SUSTAIN pot, wired as a RHEOSTAT (not a divider)
    double seriesOhms;    // the fixed resistor that sets the MAXIMUM control current
    double cellPinV;      // the gain cell's control-pin voltage
};

// Stage 8: output coupling + the LEVEL pot.
struct OutputStage {
    double couplingHz;    // the second bass rolloff
    double potOhms;       // LEVEL pot (total)
    double seriesOhms;    // series resistance above the pot (caps the max output)
    double srcOhms;       // the driving stage's output impedance
};

struct CompressorConfig {
    InputStage in;
    DriveNetwork drive;
    GainCellSpec cell;
    LoadStage load;
    Splitter split;
    Sidechain det;
    ControlMap ctl;
    OutputStage out;
    NpnDevice npn;
    DiodeDevice diode;
    // TRUE = the detector tastes the gain cell's OUTPUT (FEED-BACK — the Dyna
    // Comp, and every Ross-family box). FALSE = it tastes the cell's INPUT
    // (feed-forward). Shipped true; the false branch exists so a test can prove
    // the topology claim by measurement instead of by comment.
    bool detectorFromOutput = true;
};

// --- The engine --------------------------------------------------------------

class CompressorEngine {
public:
    // Slots mirror every other pedal's positional triple so the chain ABI stays
    // pedal-agnostic. A compressor has TWO knobs: slot 1 is carried and unused,
    // exactly as the phaser carries slots 1/2.
    enum ParamId : int { PARAM_SUSTAIN = 0, PARAM_UNUSED = 1, PARAM_LEVEL = 2 };

    explicit CompressorEngine(const CompressorConfig& cfg) : cfg_(cfg) {}

    void prepare(double sampleRate, int maxBlockSize);

    void setOversampling(int factor);
    int oversampling() const { return os_.factor(); }
    int latencySamples() const { return os_.latencySamples(); }

    void setParameter(int paramId, float value);

    // Process numFrames mono, in -> out (may alias). 1.0f == 1.0 V.
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1): clear every recursive state and re-park at
    // the CACHED quiescent point. Never re-solves — prepare() is not a recovery
    // path. Allocation-free.
    void reset();

    // --- Measurement hooks (NOT user knobs) ---------------------------------
    // Flip the detector tap. Shipped false==feed-forward is NOT a product mode;
    // `clipper_comp_tests` uses it to show the ratio collapses without feedback.
    void setDetectorTap(bool fromOutput) { cfg_.detectorFromOutput = fromOutput; }
    bool detectorFromOutput() const { return cfg_.detectorFromOutput; }
    // The control current the gain cell is running on RIGHT NOW (amps) and the
    // envelope node's voltage. The tests measure gain reduction from these
    // instead of inferring it from the audio.
    double controlCurrent() const { return iCtl_; }
    double envelopeVolts() const { return sc_.envelopeVolts(); }
    // Static (small-signal) gain of the cell at the CURRENT control current.
    double cellGainAtDc() const;
    // The largest absolute value held by any recursive state whose rest value is
    // ZERO. The denormal suite asserts this is EXACTLY 0.0 after a silent tail.
    // vEnv_ is deliberately NOT in here — it rests at a nonzero operating point
    // (measured 8.5786 V) and can never be subnormal (ADR 006's scope rule).
    double maxAbsRestingState() const;

private:
    void processChunk(const float* in, float* out, int numFrames);
    void rebuildRates();
    void solveQuiescent();
    // Returns the gain cell's OUTPUT CURRENT for a differential input voltage.
    double applyGainCell(double vDiff) const;
    void updateControl();

    CompressorConfig cfg_;

    double sampleRate_ = 44100.0;
    double osRate_ = 176400.0;
    int maxBlockSize_ = 128;
    int osFactor_ = 4;

    // --- Smoothed knobs -----------------------------------------------------
    // The SUSTAIN wiper feeds a resistance, so it is smoothed as the WIPER (the
    // physical quantity), never as the derived current.
    OnePoleSmoother sustainWiper_;
    OnePoleSmoother level_;
    bool snapPending_ = true;  // first process() after prepare() snaps (docs §35)

    // --- Base-rate linear state (all rest at ZERO -> all guarded) -----------
    double inHpX1_ = 0.0, inHpY1_ = 0.0, inHpR_ = 0.0;
    double outHpX1_ = 0.0, outHpY1_ = 0.0, outHpR_ = 0.0;

    // --- Oversampled state --------------------------------------------------
    // Drive network: TWO first-order sections (docs §56.4b), each rests at zero.
    double drvB0_[2] = {0.0, 0.0}, drvB1_[2] = {0.0, 0.0}, drvA1_[2] = {0.0, 0.0};
    double drvX1_[2] = {0.0, 0.0}, drvY1_[2] = {0.0, 0.0};
    double drvScale_ = 1.0;
    // Gain-cell load pole (rests at zero).
    double loadCoef_ = 0.0, loadY1_ = 0.0;
    // The SHARED detector: the two clamp caps, their Newton warm starts, and the
    // envelope node. None of it is denormal-guarded, deliberately — see
    // `SidechainDetector::leg()` for the measured derivation. M13.6's gate owns
    // one of these too (docs §61.2).
    SidechainDetector sc_;
    // The SUSTAIN wiper is smoothed at the BASE rate and HELD across the
    // oversampled sub-samples: the smoother's coefficient was prepared for
    // sampleRate_, so advancing it once per oversampled sample would make a 6 ms
    // ramp land in 1.5 ms at 4x.
    double rTotCtl_ = 0.0;
    int osPhase_ = 0;
    double vEnvPark_ = 8.5786;  // the cached quiescent point reset() re-parks at
    double iCtl_ = 0.0;
    double iCtlPark_ = 0.0;
    double vbe5_ = 0.62;        // warm start for Q5's junction (never zero)
    double vbe5Park_ = 0.62;

    AsymSoftClipper compliance_;  // the cell's supply compliance, ADAA
    Oversampler os_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_COMPRESSOR_ENGINE_H
