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
//  THE M13.3 SEAM — read this before adding the optical voice
// ============================================================================
// Everything except the GAIN CELL is already voice-agnostic. An optical
// compressor is the same block diagram with a different variable-gain element:
//
//   * REUSED AS-IS by M13.3 — `InputStage`, `DriveNetwork`, `LoadStage`,
//     `Splitter`, `Sidechain` (rectifier + envelope integrator), `OutputStage`,
//     the oversampling, the smoothers, `reset()`/park, the denormal policy.
//   * CHANGED by M13.3 — exactly two things:
//       1. `GainCellSpec::kind` gains `kOpticalLdr`, and `applyGainCell()` gains
//          one `case`. The OTA case is `Iout = Iabc·tanh(Vd/2Vt)`; the optical
//          case is a photoresistor whose resistance follows the LED current
//          through a LOG-ish law with its OWN multi-decade lag (the T4B's
//          famous two-stage release). That lag is cell state, so `GainCellSpec`
//          gains the LDR's time constants and `CompressorEngine` gains one more
//          state variable guarded by the same rules.
//       2. `ControlMap` maps the envelope node to whatever the cell consumes —
//          a bias CURRENT here, an LED current there. It is already a struct.
//   * NOT NEEDED by M13.3 — `otaVt`, `swingUp/swingDown` (the CA3080's output
//     compliance). Leave them where they are; they are OTA-only by name.
//
// M13.6 (the noise GATE) is the OTHER named consumer: a gate is this same
// `Sidechain` feeding a different `ControlMap` (below a threshold, cut instead
// of boost). Do not re-derive a second envelope follower for it.
//
// CROSS-SLICE NOTE (2026-07-31): a wah slice landing in parallel also builds an
// envelope follower. They were deliberately NOT shared while both were in
// flight. Unifying them is a named follow-up, not this engine's business.
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

namespace clipper::dsp {

// --- Device models -----------------------------------------------------------
//
// One NPN card and one diode card, both quoted verbatim from a published SPICE
// model, so nothing here is fitted. The Gummel-Poon low-current term (ISE/NE) is
// modelled DELIBERATELY: the detector transistors' base current is what loads
// the clamp network, and their hFE at ~0.2 mA is what sets the attack. With the
// ideal `betaF` alone a 2N3904 would report hFE = 416; with the ISE term the
// same card yields hFE ≈ 106 at 0.2 mA — the datasheet's own figure, for free.
struct NpnDevice {
    double Is = 6.734e-15;    // ON Semi 2N3904 SPICE: IS
    double betaF = 416.4;     // ON Semi 2N3904 SPICE: BF (the IDEAL-region beta)
    double Ise = 6.734e-15;   // ON Semi 2N3904 SPICE: ISE (B-E leakage)
    double Ne = 1.259;        // ON Semi 2N3904 SPICE: NE
    double Vt = 0.02585;      // kT/q at 300 K
};

// 1N4148, the SAME card the RAT's clipper uses after docs §36 fixed the ideality
// factor. Do not drop n back to 1.0 here either.
struct DiodeDevice {
    double Is = 2.52e-9;      // 1N4148 SPICE: IS
    double nVt = 0.0452892;   // n·Vt with n = 1.752 (docs §36), Vt = 25.85 mV
};

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

// Stage 6: the detector. Per leg: a coupling cap into a node with a resistor to
// ground, a diode that clamps the NEGATIVE excursion (anode grounded), and the
// base-emitter junction of a grounded-emitter transistor whose collector
// discharges the envelope cap. The clamp makes it a DC restorer, so what the
// transistor sees is the peak-to-peak swing referred to one diode drop below
// ground — which is why there is no threshold knob: the threshold IS a Vbe.
struct Sidechain {
    double clampCapF;     // C7/C8
    double clampResOhms;  // R16/R17
    double envCapF;       // C9 — the "squish" cap
    double envResOhms;    // R18 — pulls the envelope back up (the RELEASE)
    double supplyV;       // the rail R18 pulls to
    double vceSatV;       // where the discharge transistors bottom out
    // The two legs are NOT driven from equal impedances: the emitter leg is a
    // follower (~1.4 kΩ) and the collector leg sits behind its own load resistor
    // (10 kΩ). That asymmetry limits how much base current the collector leg can
    // deliver on a peak, so the two halves of the "full-wave" rectifier do not
    // contribute equally. Modelled rather than averaged away.
    double srcEmitterOhms;
    double srcCollectorOhms;
};

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
    double envelopeVolts() const { return vEnv_; }
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
    // One sidechain leg: solves the clamp node, advances the coupling cap, and
    // returns the collector current that discharges the envelope cap.
    double detectorLeg(double drive, double& capState, double& vbWarm,
                       double srcOhms) const;
    void advanceEnvelope(double dischargeAmps);
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
    // Sidechain clamp caps, and the Newton warm starts for the two clamp nodes.
    // NOT denormal-guarded, deliberately: they floor at the SOLVER's residual
    // tolerance (~1e-11 V measured after a 40 s silent tail), not at zero, so a
    // guard here could never fire. See detectorLeg() for the derivation.
    double clampP_ = 0.0, clampN_ = 0.0;
    double vbP_ = 0.0, vbN_ = 0.0;
    // The SUSTAIN wiper is smoothed at the BASE rate and HELD across the
    // oversampled sub-samples: the smoother's coefficient was prepared for
    // sampleRate_, so advancing it once per oversampled sample would make a 6 ms
    // ramp land in 1.5 ms at 4x.
    double rTotCtl_ = 0.0;
    int osPhase_ = 0;
    // The envelope node. Rests at a NONZERO operating point — see the header
    // comment on maxAbsRestingState(). No flush here, deliberately.
    double vEnv_ = 8.5786;
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
