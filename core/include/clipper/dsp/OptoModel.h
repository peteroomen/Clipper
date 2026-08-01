// Clipper — portable DSP core.
//
// OptoModel (M13.3) — the "Lumen" OPTICAL COMPRESSOR: a Teletronix LA-2A-style
// electro-optical leveling amplifier, in pedal form. Pedal type `opto`, and the
// lineup's SECOND dynamics processor after M13.1's OTA "Squash".
//
// ============================================================================
//  WHAT MAKES IT AN OPTO, AND IT IS NOT A KNOB
// ============================================================================
// A T4B module is an electroluminescent panel lighting a cadmium-sulfide
// photoresistor. The panel is driven by the AMPLIFIED AUDIO, so:
//
//   * the panel is the RECTIFIER — it emits on both polarities;
//   * the photocell is the INTEGRATOR — and it is a lousy one, in exactly the
//     way that makes the pedal famous.
//
// **There is no envelope capacitor anywhere in this pedal's control path.** The
// time constants ARE the cell's, and the cell's release is two-stage and
// PROGRAM DEPENDENT: the same depth of gain reduction recovers at one speed
// after a stab and at another after a sustained passage, because the slow branch
// is retarded by the cell's own trap occupancy. That property is this voice's
// acceptance bar (docs §64.4) and it is the one thing an RC-detector compressor
// cannot have at any coefficient.
//
// ============================================================================
//  THE THREE SEAM QUESTIONS ADR 019/021/023 LEFT FOR THIS SLICE
// ============================================================================
// 1. **Is this a config of `CompressorEngine`?  NO — reported, not widened.**
//    ADR 019 predicted "a config plus one `applyGainCell()` case". Of that
//    engine's eight config structs, ONE (`OutputStage`) survives contact with
//    this circuit. `DriveNetwork` is the CA3080's differential input attenuator,
//    `LoadStage` its output compliance, `Splitter` the Dyna Comp's phase
//    splitter, `ControlMap` the Iabc path, and `applyGainCell()` returns a
//    CURRENT into a load where an optical cell is a RESISTOR in a divider. Full
//    seam-by-seam table in docs §64.3; the decision is ADR 025. This model
//    therefore takes `GateModel`'s shape — a standalone model that HOLDS the
//    component that really is shared — which is the precedent M13.6a set one
//    slice earlier for exactly this reason.
//
// 2. **Is `SidechainDetector` the detector?  NO — and it was measured, not
//    argued** (ADR 023's procedural rule: build the substitution and run it).
//    It is a THRESHOLD detector with ~2 dB of proportional range; this pedal's
//    gentle ratio comes from a MULTI-DECADE proportional control law, and it has
//    no envelope cap for the component to be. The substitution was built and
//    every published-behaviour bar it is checked against failed. Numbers in docs
//    §64.3. The component was NOT widened.
//
// 3. **What IS shared, then?** `OptoCell` — the photocell, extracted as a
//    component from the first line (ADR 021's lesson applied on the way in), with
//    ROADMAP M13.5's photocell-driven Uni-Vibe as its named future consumer.
//
// ============================================================================
//  THE CIRCUIT  (a DOCUMENTED RECONSTRUCTION — read docs §64.1 first)
// ============================================================================
// NO SCHEMATIC WAS READ. The proxy here permits github.com only and `WebFetch`
// returned 403 for every other host, so the channel was search-result extracts.
// The TOPOLOGY, the module's construction, the published attack/release/max-GR
// figures and both device exponents are SOURCED; every resistor, capacitor and
// time constant below is either DERIVED from those published figures in the open
// or is a stated reconstruction. §57's rule applies verbatim: do not re-tune any
// of them toward a sound; find the schematic.
//
//   in ─[Cin]──[Rs 100k]──┬──► ×A0 ──[headroom]──┬──► GAIN pot ─[Cout]─► out
//                         │                      │
//                    R_cell (T4B)                │   the sidechain taps HERE,
//                         │                      │   BEFORE the GAIN pot — so
//                        gnd                     │   make-up gain does not
//                         ▲                      │   change the compression
//                         │                      ▼
//                         │            MODE: COMPRESS = this node (feed-BACK)
//                         │                  LIMIT    = 24/25 of it + 1/25 of
//                         │                             the pedal's INPUT
//                         │                      │
//                         │            PEAK REDUCTION pot → sidechain amp
//                         │                      │        → step-up (≤ 90 V pk)
//                         │                      ▼
//                         └──── OptoCell ◄── EL panel (2M2 bleeder across it)
//
// The gain-reduction element is a DIVIDER whose bottom leg is the cell, so the
// pedal's gain reduction is `20·log10(1 + Rs/R_cell)` — which is why the knee is
// soft (a dark cell barely loads the divider) and why the ratio stiffens as the
// cell lights up. `Rs` is DERIVED, not chosen: the published maximum gain
// reduction (over 40 dB) at the published lit-cell resistance (under 1 kΩ)
// requires Rs ≈ 99 kΩ, so it is the 100 kΩ standard value and the model's own
// ceiling measures 40.09 dB.
//
// **The ratio is a PREDICTION, not a fit.** Deep in reduction the divider's
// sensitivity saturates at the composite device exponent p = n·γ (EL brightness
// exponent × CdS gamma), so a feed-back loop settles at a ratio of 1 + p. With
// the published n = 2…3 and γ = 0.58…0.92 that is 2.2:1 … 3.8:1, and the
// widely-published figure for the reference unit is "about 3:1". The shipped
// mid-range values (n = 2.5, γ = 0.75) give 2.875:1 analytically; docs §64.5
// reports what the rendered pedal actually measures against that.
//
// ============================================================================
//  THE THREE KNOBS, AND WHY THE THIRD IS NOT DEAD UI
// ============================================================================
// PEAK REDUCTION (slot 0), MODE (slot 1), GAIN (slot 2). §61 refused to ship the
// noise gate's MODE switch because it changes what the FOOTSWITCH does rather
// than the audio. This one is the opposite case and the difference is sourced:
// the reference's COMPRESS position shorts the sidechain to the output (100 %
// feed-back), while LIMIT feeds the sidechain a linear combination of the output
// and the pedal's own INPUT — so under gain reduction the sidechain sees the
// un-reduced peak and clamps harder and sooner. It is a discrete two-state
// switch in a knob slot (the CE-1 precedent), and `clipper_opto_tests` measures
// the difference rather than asserting it.
//
// Platform-free (C++17, no OS/browser/Emscripten). Convention: 1.0f == 1.0 V.

#ifndef CLIPPER_DSP_OPTO_MODEL_H
#define CLIPPER_DSP_OPTO_MODEL_H

#include "clipper/dsp/AsymSoftClipper.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/OptoCell.h"
#include "clipper/dsp/Oversampler.h"

namespace clipper::dsp {

// The pedal's own constants — everything between the cell and the jacks. This is
// the struct `ControlMap` is NOT: it has no rheostat, no cell pin and no bias
// current, because there is no OTA anywhere in this pedal (ADR 021's rule
// followed literally rather than by widening someone else's struct).
struct OptoVoice {
    // --- The audio path ------------------------------------------------------
    double seriesOhms;      // Rs, the gain-reduction divider's top leg
    double ampGain;         // the leveling amplifier's FIXED voltage gain
    double headroomV;       // its output swing (a soft, ADAA-limited ceiling)
    double gainRangeDb;     // the GAIN pot: an OUTPUT ATTENUATOR, 0 dB at 1.0
    double inCouplingHz;    // input coupling corner
    double outCouplingHz;   // output coupling corner

    // --- The sidechain -------------------------------------------------------
    double sidechainMaxGain;  // amp × step-up, at PEAK REDUCTION 1.0
    double peakRangeDb;       // the PEAK REDUCTION pot's span (a log part)
    double panelClampV;       // the panel drive ceiling — the sourced ~90 V peak
    double panelBleedHz;      // the 2M2 bleeder with the panel's own capacitance
    double limitFeedForward;  // MODE=LIMIT: this much of the INPUT in the mix
};

class OptoModel {
public:
    // Slots mirror every other pedal's positional triple so the chain ABI stays
    // pedal-agnostic. THREE real controls — unlike the compressor's and the
    // gate's two, slot 1 carries a genuine (discrete) control here.
    enum ParamId : int {
        PARAM_PEAK_REDUCTION = 0,
        PARAM_MODE = 1,  // < 0.5 COMPRESS, >= 0.5 LIMIT — discrete, not a blend
        PARAM_GAIN = 2
    };

    OptoModel();

    void prepare(double sampleRate, int maxBlockSize);

    // OVERSAMPLED at the house 4x default, and that is a MEASURED decision —
    // the FIRST thing this slice checked was whether it could take the gate's
    // arrangement (accept and ignore) and the answer was no (docs §64.7). The
    // gain-reduction divider is a MULTIPLY BY AN AUDIO-RATE SIGNAL: the cell's
    // conductance carries a residual ripple at 2*f0 because the panel is the
    // rectifier, so `x * div` generates sum and difference products that fold.
    // Measured on a single 9 kHz tone at 0.30 V with PEAK REDUCTION at 1.0, the
    // worst non-harmonic component in band moves −86.16 dB (base rate) to
    // −114.09 dB (the same model run at 8x the rate) — it MOVES with the factor,
    // which is §54's tell for real aliasing rather than legitimate IMD. The ADAA
    // headroom limiter's own half-sample averaging is the second reason: its
    // droop at 10 kHz is −1.99 dB at 48 kHz and −0.13 dB at 4x.
    void setOversampling(int factor);
    int oversampling() const;
    int latencySamples() const;

    void setParameter(int paramId, float value);

    // Process numFrames mono, in -> out (may alias). 1.0f == 1.0 V.
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1): clear every recursive state and re-park
    // DARK. Never re-solves — a cell with no light on it is at its dark
    // resistance by definition, so there is nothing to solve. Allocation-free.
    void reset();

    // --- Measurement hooks (NOT user knobs) ---------------------------------
    // The cell's resistance right now, and the gain reduction it is applying, in
    // dB. The tests read reduction from here instead of inferring it from audio.
    double cellResistanceOhms() const { return cell_.resistanceOhms(); }
    double gainReductionDb() const;
    // The cell's trap occupancy (0..1) — the state that makes the release
    // program dependent. Exposed so a test can attribute a measured release
    // difference to the mechanism instead of just observing it.
    double cellMemory() const { return cell_.memory(); }
    // The panel drive the sidechain last delivered, in volts.
    double panelVolts() const { return vPanel_; }
    // The ratio this pedal's DEVICE EXPONENTS predict deep in reduction
    // (1 + n·γ). Not read back from a bar — the test compares a RENDERED static
    // curve against it, and against the published figure for the real unit.
    double predictedDeepRatio() const { return 1.0 + cell_.compositeExponent(); }
    // The largest absolute value held by any recursive state whose rest value is
    // ZERO. The denormal suite asserts this is EXACTLY 0.0 after a silent tail.
    double maxAbsRestingState() const;

    const OptoVoice& voice() const { return voice_; }
    const OptoCell& cell() const { return cell_; }

private:
    void processChunk(const float* in, float* out, int numFrames);
    void rebuildRates();
    void refreshKnobs(double peakKnob, double gainKnob);

    OptoVoice voice_{};
    OptoCell cell_;

    double sampleRate_ = 44100.0;
    double osRate_ = 176400.0;
    int maxBlockSize_ = 128;
    int osFactor_ = 4;

    // --- Smoothed knobs -----------------------------------------------------
    // Both are smoothed as the WIPER (the physical quantity), never as the
    // derived gain. MODE is a discrete switch and is NOT smoothed — the pedal's
    // topology changes, which the chain layer declicks (the CE-1 precedent).
    OnePoleSmoother peakWiper_;
    OnePoleSmoother gainWiper_;
    bool limitMode_ = false;
    bool snapPending_ = true;  // first process() after prepare() snaps (docs §35)

    // --- Base-rate linear state (all rest at ZERO -> all guarded) -----------
    double inHpX1_ = 0.0, inHpY1_ = 0.0, inHpR_ = 0.0;
    double outHpX1_ = 0.0, outHpY1_ = 0.0, outHpR_ = 0.0;
    double panelHpX1_ = 0.0, panelHpY1_ = 0.0, panelHpR_ = 0.0;

    // --- Derived per base sample from the smoothed wipers -------------------
    // The wipers are smoothed at the BASE rate and HELD across the oversampled
    // sub-samples: the smoother's coefficient was prepared for sampleRate_, so
    // advancing it once per oversampled sample would make a 6 ms ramp land in
    // 1.5 ms at 4x (the CompressorEngine precedent).
    double scGain_ = 1.0;    // the sidechain's total voltage gain
    double makeup_ = 1.0;    // the GAIN pot's attenuation (<= 1.0)
    int osPhase_ = 0;
    // The loop's one-sample memory: the cell resistance the NEXT sample divides
    // with. A feedback loop needs one sample of delay somewhere; the cell's own
    // transport lag is 10 ms, i.e. ~480 samples at 48 kHz, so this is the
    // physically small place to put it. Rests at the DARK resistance — a real
    // operating point, so it is deliberately NOT denormal-guarded (ADR 006).
    double rCell_ = 10.0e6;
    double vPanel_ = 0.0;

    AsymSoftClipper headroom_;  // the amplifier's swing, ADAA
    Oversampler os_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_OPTO_MODEL_H
