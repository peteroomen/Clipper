// Clipper — portable DSP core.
//
// v1.1 item 6: the GOLD overdrive — a circuit model of the famously-hoarded
// gold-boxed "transparent" overdrive (the Klon Centaur; user-facing name is the
// trademark-safe "Myth", docs §27). It is NOT a Tube-Screamer variant, so it does
// NOT reuse OverdriveEngine: its soul is a PARALLEL CLEAN/DIRT BLEND driven by a
// DUAL-GANGED gain pot, which the TS-family engine (a single feedback clipper with
// a clean pedestal) cannot express.
//
// Four sections, modelled in circuit order (see GoldModel.cpp for values, sources,
// and every documented approximation):
//
//   1. INPUT BUFFER   — the always-on buffered input (this pedal buffers even when
//                       "bypassed" in hardware). Unity op-amp follower + the input
//                       network's DC-blocking corner. Linear, base rate.
//   2. GAIN SECTION   — the dual-ganged GAIN pot. One gang sets the drive amp's
//                       gain (1 + P/Rg); the other cross-fades the CLEAN buffered
//                       signal against the CLIPPED one at the summing node. The
//                       drive path is high-passed BEFORE clipping (the lows stay
//                       clean — the "transparent" trait), amplified through an
//                       op-amp model (bandwidth + slew), then clipped by an
//                       antiparallel GERMANIUM (1N34A-class) diode pair built with
//                       chowdsp_wdf. Nonlinear → runs at 4× oversampling.
//   3. TREBLE         — the post-blend treble control: a shelving tilt about a
//                       ~1 kHz pivot, flat at noon, normal sense (clockwise
//                       BRIGHTENS — unlike the RAT's FILTER or the AC30's CUT).
//   4. OUTPUT STAGE   — output buffer + OUTPUT pot + the coupling-cap DC block.
//
// The charge pump (a −9 V generator, so the op-amps run on ±9 V rather than a
// single 9 V rail) shows up here as HEADROOM: the summing node carries an explicit
// ±rail clamp that, at guitar levels, is never reached — the germanium pair is the
// ONLY thing that clips. That is why the pedal stays clean-sounding underneath its
// dirt, and the test suite asserts the rails stay out of the way.
//
// Platform-free (C++17, no OS/browser/Emscripten). Param ids mirror the house
// positional triple (0 = GAIN, 1 = TREBLE, 2 = OUTPUT) so the C ABI and the worklet
// treat this pedal exactly like every other. Heavy WDF state lives behind a pimpl.

#ifndef CLIPPER_DSP_GOLD_MODEL_H
#define CLIPPER_DSP_GOLD_MODEL_H

#include <memory>

namespace clipper::dsp {

class GoldModel {
public:
    // Normalized knob positions in [0, 1]; mapped to physical units internally
    // (GoldModel.cpp documents every mapping, mirrored in docs §27).
    enum ParamId : int {
        PARAM_GAIN = 0,    // dual-ganged GAIN: amp gain + clean/clipped crossfade
        PARAM_TREBLE = 1,  // treble tilt (0 = dark, 0.5 = flat, 1 = bright)
        PARAM_OUTPUT = 2,  // output level
        PARAM_COUNT
    };

    // Diode flavour. GERMANIUM is the pedal (1N34A-class, soft knee ~0.3 V);
    // SILICON is a MEASUREMENT-ONLY counterfactual (1N914-class, ~0.6 V hard-ish
    // knee) used by the tests to show what the germanium pair actually buys.
    enum DiodeType : int {
        DIODE_GERMANIUM = 0,
        DIODE_SILICON = 1,
    };

    GoldModel();
    ~GoldModel();

    GoldModel(const GoldModel&) = delete;
    GoldModel& operator=(const GoldModel&) = delete;

    void prepare(double sampleRate, int maxBlockSize);

    // Oversampling for the nonlinear gain section: 1 / 2 / 4 / 8 (default 4).
    void setOversampling(int factor);
    int oversampling() const;
    int latencySamples() const;

    // --- measurement hooks (NOT user knobs) ---------------------------------
    // Swap the clipping diodes for the silicon counterfactual (default germanium).
    void setDiodeType(int type);
    int diodeType() const;
    // Force the CLEAN half of the blend to zero, leaving only the clipped path —
    // the counterfactual that shows the clean blend IS the transparency.
    void setCleanBlendEnabled(bool enabled);
    bool cleanBlendEnabled() const;
    // Bypass the op-amp model (ideal op-amp) for the bandwidth/aliasing A/B.
    void setIdealOpAmp(bool ideal);
    bool idealOpAmp() const;

    void setParameter(int paramId, float value);

    // Process numFrames of mono audio, in -> out (may alias). 1.0f == 1.0 V.
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1). Clear every recursive state — the five knob
    // smoothers (snapped onto their targets), the input/drive high-passes, the tone
    // pivot low-pass, the output DC block, the op-amp model, the germanium WDF cap
    // and the oversampler histories. Keeps rate / factor / diode type / knobs: a
    // state flush, not a re-prepare. Allocates nothing.
    void reset();

    // Analytic helpers the tests measure against (pure functions of the knob).
    // Drive-amp voltage gain at a GAIN knob position: 1 + knob*P_gain/R_g.
    static double driveGainAt(double knob);
    // The two ganged blend weights at a GAIN knob position (clean, clipped).
    static double cleanBlendAt(double knob);
    static double clipBlendAt(double knob);

private:
    void processChunk(const float* in, float* out, int numFrames);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_GOLD_MODEL_H
