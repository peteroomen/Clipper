// Clipper — portable DSP core (M13.6 — the ten-band graphic EQ).
//
// EqModel: the "Decade" — a homage to the ten-band pedal graphic EQs, and the
// board's first EQ voice. TEN gyrator band legs summed around one inverting
// op-amp stage, plus a GAIN slider in front and a VOLUME slider behind.
//
// TOPOLOGY (transcribed — see docs §72.1 for the sourced/derived/reconstructed
// table). The reference ten-band's own schematic is NOT reachable from this
// container; what IS reachable is a complete annotated netlist for a SEVEN-band
// gyrator EQ of the same family (Boss GE-7, in the same LTspice repository §59
// parsed for the Dyna Comp and §66 for the RAT), and the topology below is
// traced from it node by node.
//
//        Vin ──[Rin]──┬── V⁻ (op-amp inverting input, held at 0)
//                     │
//        Vout ─[Rf]───┘
//
//        per band k:   Vin ──[(1-x)·Rp]──┬──[x·Rp]── Vout
//                                        │
//                                     W_k│
//                                        ├── C1 ── L ── Rs ── V⁻
//
// THE LEG RUNS FROM THE POT WIPER TO THE VIRTUAL GROUND, not to ground, and
// that is the whole circuit. A first draft of this model had it to ground; the
// ten pots then sit end-to-end across the summing node's feedback, which loads
// it with 1 kΩ and measures −20 dB of flat insertion loss with essentially NO
// equalisation. Written down here because it is an easy and silent mistake:
// both versions look like "a pot with a resonant leg on the wiper".
//
// WHAT THE REAL TOPOLOGY BUYS, and why each of these is a CONSEQUENCE rather
// than a coefficient anybody chose:
//
//   * FLAT AT CENTRE, EXACTLY. At x = 0.5 the wiper sits at (Vin + Vout)/2,
//     which is identically zero while Vout = −Vin. The leg therefore injects
//     NOTHING into the summing node — not "very little", nothing — so ten
//     centred sliders are bit-identical to the bare inverting stage. Measured
//     +0.0000 dB at every probe. It is not a calibration and must not become
//     one.
//   * BOOST AND CUT ARE EXACT MIRRORS. x and (1-x) exchange the roles of Vin
//     and Vout in the same expression. Measured +12.000 / −12.000 dB.
//   * PROPORTIONAL Q. The leg's damping is dominated by the impedance the POT
//     presents at the wiper: ~Rp/4 with the slider centred, ~0 with it hard
//     over. So the bandwidth NARROWS as the slider leaves centre, which is most
//     of why a graphic EQ sounds like one. The published behaviour (Rane note
//     101, Bohn) is ~1 octave at +3 dB narrowing to ~1/3 octave at full boost.
//   * BANDS INTERACT. Every leg injects into the SAME summing node, so two
//     adjacent sliders up is not the sum of each one alone. The published
//     description of proportional-Q designs says exactly this. It falls out of
//     the node equation; there is no cross-coupling term to tune.
//
// THE TWO RECONSTRUCTED CONSTANTS, AND THE MISS THEY LEAVE (§57's rule applies
// verbatim to both — do not re-tune them toward a sound; find the ten-band's
// schematic). `kLegLossOhms` is pinned to the published ±12 dB range and
// `kLegZ0Ohms` to the published 1/3-octave width at full boost. That fixes the
// model completely — and then the THIRD published figure comes out wrong:
//
//     narrowing from +3 dB to full boost:  measured 1.51x, published ~3x
//
// This is REPORTED, not fitted away, and it is registered as an XFAIL
// (`eq-proportional-q-shallow`). The cause is structural and is named so the
// next slice does not re-fit constants at it: within this topology max boost
// and full-boost bandwidth are BOTH governed by the single lumped leg loss
// `Rs`, so the two published figures OVER-DETERMINE the model. Making them
// independent needs the leg's real loss structure — the gyrator's series R1 and
// its parallel R2 modelled separately, or whatever series element sits between
// wiper and leg in the ten-band that this reconstruction cannot see. A smaller
// `Rs` demonstrably moves the ratio the right way (1.51x → 2.37x at the GE-7's
// own sourced 330 Ω) and takes the boost to +29 dB, which is why it cannot
// simply be lowered.
//
// PER-BAND VALUES ARE DERIVED, NOT TRANSCRIBED. The netlist in reach is a
// SEVEN-band from a different manufacturer, so its component values cannot be
// carried across. What carries is its design equation — `L = R1·R2·C2`, which
// reproduces all six of its gyrator bands' own labelled centres to
// −4.41…+3.22 % — plus the observation that its bands hold a roughly constant
// characteristic impedance √(L/C1). So each band here is laid out the way a
// real one is: one shared Z0, and L = Z0/(2πf0), C1 = 1/(2πf0·Z0).
//
// NOT CARRIED ACROSS, and named so it is not read as an oversight: the GE-7's
// TOP band has no gyrator at all — a bare 47 n + 820 Ω series RC. That is a
// GE-7 fact, and whether the ten-band reference does the same is unknown, so
// all ten bands here are gyrator legs.
//
// LINEAR AND TIME-INVARIANT: no nonlinearity anywhere, so NO oversampling and
// zero latency — the phaser precedent (§12). `setOversampling` is accepted and
// ignored, and the tests assert the render is bit-identical at 1/2/4/8x so a
// later slice cannot quietly add 72 samples of latency for nothing (§70's
// shape).
//
// Platform-free C++17 (no OS/browser/Emscripten includes); heavy state behind a
// pimpl. Parameter ids: slot 0 = GAIN and slot 2 = VOLUME, reusing the shared
// three-knob shape the way the phaser takes slot 0 as SPEED and the delay takes
// it as DELAY; slot 1 is carried and UNUSED, as the compressor and gate carry
// it. The ten band sliders are ids 3..12 — the first pedal on this board whose
// control surface is larger than three slots.

#ifndef CLIPPER_DSP_EQ_MODEL_H
#define CLIPPER_DSP_EQ_MODEL_H

#include <memory>

namespace clipper::dsp {

class EqModel {
public:
    static constexpr int kNumBands = 10;

    enum ParamId : int {
        PARAM_GAIN = 0,      // input gain slider (0..1; 0.5 == unity)
        PARAM_UNUSED_1 = 1,  // carried for the shared 3-param shape; ignored
        PARAM_VOLUME = 2,    // output level slider (0..1; 0.5 == unity)
        // The ten band sliders, 0..1, 0.5 == flat. MUST stay contiguous and in
        // ascending frequency order: the C ABI, the worklet and the native
        // engine all address them as PARAM_BAND_BASE + i.
        PARAM_BAND_BASE = 3,
        PARAM_COUNT = PARAM_BAND_BASE + kNumBands
    };

    EqModel();
    ~EqModel();
    EqModel(const EqModel&) = delete;
    EqModel& operator=(const EqModel&) = delete;

    void prepare(double sampleRate);
    void reset();

    void setParameter(int paramId, float value);
    // Band i (0..kNumBands-1), 0..1 slider position; 0.5 is flat.
    void setBand(int index, float knob01);

    void process(const float* in, float* out, int numFrames);

    // Linear and time-invariant: accepted and IGNORED (see the banner).
    void setOversampling(int factor);
    int latencySamples() const;

    // --- Published / derived constants the tests pin ---------------------------
    // Nominal ISO centre of band i, in Hz.
    static double bandCentreHz(int index);
    // The band leg's characteristic impedance √(L/C1) and its lumped loss, both
    // RECONSTRUCTED (see the banner).
    static double legZ0Ohms();
    static double legLossOhms();
    // Slider knob (0..1) -> the leg's position fraction x actually used.
    static double sliderToX(double knob01);

    // --- Measurement hooks (NOT user knobs) -----------------------------------
    // Every recursive state in this model rests at exactly zero on silence (the
    // caps and the inductor currents all discharge), so ADR 006 says they are
    // all guarded and this must read exactly 0.0 after a silent tail.
    double maxAbsRestingState() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_EQ_MODEL_H
