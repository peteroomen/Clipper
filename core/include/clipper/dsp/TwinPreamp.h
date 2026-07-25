// Clipper — portable DSP core (M10.1).
//
// TwinPreamp: the Fender blackface AB763 (Twin-style) VIBRATO-CHANNEL preamp as a
// validated core module. Two 12AX7 common-cathode stages around the Fender TMB
// tone stack, then the channel VOLUME pot:
//
//   guitar in
//     -> V1   common cathode, Ra 100k, Rk 1.5k || 25uF (bypassed, warm) — the
//             first gain stage
//     -> Fender TMB tone stack in the PRE-GAIN position (blackface values):
//             treble cap 250 pF, mid 0.047 uF, bass 0.1 uF; treble pot 250k,
//             bass 250k, mid 10k; slope resistor 100k. Driven straight from V1's
//             PLATE (a HIGH source impedance ~ Ra||rp — no cathode follower, so
//             it LOADS differently than the Marshall stack's follower-driven
//             low-Z source). The Fender values give the signature DEEP MID SCOOP.
//     -> V2   recovery common cathode, Ra 100k, Rk 1.5k || 25uF (bypassed)
//     -> VOLUME pot (1M, audio taper) + BRIGHT treble-bleed
//   B+ ~ 410 V throughout (the AB763 runs a higher rail than the JCM's 320 V).
//
// Contrast with the Marshall 2204 preamp (Jcm800Preamp): the Twin has only two
// gain stages (no cold 3rd stage, no cathode follower), both warmly biased
// (1.5k||25uF, NOT the JCM's cold 10k unbypassed 2nd stage), and the tone stack
// sits BEFORE the recovery stage / volume (pre-gain), not after the whole preamp.
// This is why the Twin is the CLEAN-headroom king — nothing here is voiced to
// crunch; the drive, when it comes, is late and mostly from the power stage.
//
// The two triode stages are the only nonlinearity and each antialiases itself
// with the shared M2 Oversampler (the composed TwinAmp measures the requirement,
// docs §20). The Fender tone stack and the volume/bright network are strictly
// linear and run at the base rate.
//
// Process API mirrors Jcm800Preamp: prepare(fs,maxBlock) / setParameter /
// process(mono, in->out). Convention: 1.0f == 1.0 V. Platform-free C++17 (no
// OS/browser/Emscripten includes).

#ifndef CLIPPER_DSP_TWIN_PREAMP_H
#define CLIPPER_DSP_TWIN_PREAMP_H

#include <array>
#include <vector>

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/TriodeStage.h"

namespace clipper::dsp {

// ---------------------------------------------------------------------------
// FenderToneStack — the blackface TMB (FMV) passive tone stack, nodal MNA with
// trapezoidal capacitor companions. SAME netlist topology as the Marshall stack
// (Jcm800Preamp's MarshallToneStack) — the FMV network is shared — but with the
// FENDER component values and a high-impedance (plate-driven) source. Public so
// its response is independently testable: the test derives H(jw) from this same
// netlist via a complex nodal solve and compares.
//
// Netlist (nodes IN, N2, N3, N4, OUT; GND) — identical to MarshallToneStack:
//   Ct  : IN - N2                    (treble cap)
//   Rs* : source - IN                (V1 plate output impedance; * driven input)
//   R1  : IN - N3                    (slope resistor — Fender's 100k, deeper scoop)
//   RT  : N2 -(1-t)RT- OUT -(t)RT- N3  (treble pot; wiper OUT is the output)
//   RB  : N3 -(l*RB)- N4             (bass pot as rheostat)
//   Cb  : N3 - N4                    (bass cap, across the bass pot)
//   RM  : N4 -(m*RM)- GND            (mid pot as rheostat to ground)
//   Cm  : N4 - GND                   (mid cap, across the mid pot)
// (t,m,l = treble,mid,bass pot fractions in [0,1].)
// ---------------------------------------------------------------------------
class FenderToneStack {
public:
    // Canonical blackface AB763 tone-stack component values (documented constants).
    static constexpr double kRslope = 100.0e3;   // slope resistor (Ohm) — Fender's
    static constexpr double kRT = 250.0e3;       // treble pot (Ohm)
    static constexpr double kRB = 250.0e3;       // bass pot (Ohm)
    static constexpr double kRM = 10.0e3;        // mid pot (Ohm)
    static constexpr double kCt = 250.0e-12;     // treble cap (F)
    static constexpr double kCb = 100.0e-9;      // bass cap (F) = 0.1 uF
    static constexpr double kCm = 47.0e-9;       // mid cap (F)  = 0.047 uF

    void prepare(double sampleRate);
    void reset();
    void setSourceImpedance(double rs);          // V1 plate output impedance (Ohm)
    void setKnobs(double bass, double mid, double treble);  // each in [0,1]
    void process(const float* in, float* out, int numFrames);

    double sourceImpedance() const { return rs_; }

    // Anti-denormal diagnostic (Denormal.h, docs §33) — not used by the audio path.
    // All six cap companions rest at zero, so after a silent tail this must be
    // EXACTLY 0.0; a tiny nonzero residual means a state is asymptoting through the
    // double subnormal range. Measured 18.6x slower than hardware FTZ before the flush.
    double maxAbsRestingState() const {
        return maxAbsState(vT_, iT_, vB_, iB_, vM_, iM_);
    }

private:
    void rebuild();  // recompute the 5x5 conductance matrix + its inverse

    static constexpr int N = 5;  // nodes: IN=0, N2=1, N3=2, N4=3, OUT=4
    double sampleRate_ = 44100.0;
    double T_ = 1.0 / 44100.0;
    double rs_ = 38.0e3;   // high-Z plate source (Ra||rp), not a follower
    double bass_ = 0.5, mid_ = 0.5, treble_ = 0.5;
    double geqT_ = 0.0, geqB_ = 0.0, geqM_ = 0.0;  // cap companion conductances
    std::array<std::array<double, N>, N> Ginv_{};  // inverse of the node matrix
    double vT_ = 0.0, iT_ = 0.0;   // Ct  (IN-N2)
    double vB_ = 0.0, iB_ = 0.0;   // Cb  (N3-N4)
    double vM_ = 0.0, iM_ = 0.0;   // Cm  (N4-GND)
    bool dirty_ = true;
};

// ---------------------------------------------------------------------------
// The composed Twin vibrato-channel preamp.
// ---------------------------------------------------------------------------
class TwinPreamp {
public:
    enum ParamId : int {
        PARAM_VOLUME = 0,   // channel volume (1M pot) — audio taper
        PARAM_BASS = 1,     // TMB bass   (0..1)
        PARAM_MID = 2,      // TMB mid    (0..1)
        PARAM_TREBLE = 3,   // TMB treble (0..1)
        PARAM_BRIGHT = 4,   // 0/1 bright switch (treble-bleed across the volume pot)
        PARAM_COUNT = 5,
    };

    enum Stage : int { V1 = 0, V2 = 1, STAGE_COUNT = 2 };

    TwinPreamp();

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);
    int oversampling() const { return oversampling_; }

    // Round-trip oversampling-filter group delay in base-rate samples (both
    // triode stages run serially; the tone stack + volume/bright are linear).
    int latencySamples() const {
        return stage_[V1].latencySamples() + stage_[V2].latencySamples();
    }

    void setParameter(int paramId, float value);
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1). Re-park both triode stages, the Fender stack
    // cap states and the bright treble-bleed filter at the already-solved DC point,
    // without re-solving anything. Knob values are preserved.
    void reset();

    // --- Introspection (measurement / tests) --------------------------------
    double stageQuiescentPlate(int stage) const;
    double stageQuiescentCathode(int stage) const;
    double stageQuiescentCurrent(int stage) const;
    double toneSourceImpedance() const { return toneRs_; }

    double volumeScale() const;  // audio-taper wiper for the current VOLUME knob
    static double audioTaper(double x);  // (e^{k x}-1)/(e^k-1), k=4 — exposed to tests

    const TriodeStage::Config& stageConfig(int stage) const { return cfg_[stage]; }
    FenderToneStack& toneStack() { return tone_; }

    double lastOutputPeak() const { return lastOutPeak_; }

    // NO maxAbsRestingState() on the COMPOSED preamp, deliberately (Denormal.h, docs
    // §33) — and the reason is the most interesting measurement in this slice.
    //
    // The Fender stack's six cap companions DO rest at exactly zero, and standalone they
    // measured the second-worst denormal cliff in the core (18.6x on a silent tail vs
    // hardware FTZ). But WIRED UP HERE they never get there: V1 is a nonlinear tube stage
    // whose own coupling-cap state parks at a nonzero grid-current bias point, so it
    // feeds the stack a small but NORMAL residual forever instead of true digital
    // silence. Measured inside this preamp, the stack + bright state idle at 5.7e-11 —
    // 297 orders of magnitude clear of the subnormal boundary — and the composed preamp
    // measures 0.99x, i.e. no cliff at all.
    //
    // So the bug is real, the fix is real, and TODAY it is masked in-product by an
    // upstream DC residual. That masking is incidental: it depends on a tube stage's idle
    // offset, and it disappears the moment the stack is driven by anything that emits
    // exact zeros (a bypassed stage, a muted chain, a reset()). Assert the property where
    // it holds unconditionally — on FenderToneStack itself, which
    // clipper_denormal_tests does — and do not pretend the composite has it.
    // Same for brightS_: it rests on the same residual.

    // Documented interstage constant: V1 plate AC → tone-stack input trim. The
    // coupling to the pre-gain Fender stack is ~unity (grid-leak coupling); this
    // is 1.0 and named for symmetry with the JCM's documented handoffs.
    static constexpr double kV1ToStack = 1.0;

private:
    void configureStages();

    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;

    std::array<TriodeStage, STAGE_COUNT> stage_;
    std::array<TriodeStage::Config, STAGE_COUNT> cfg_{};
    FenderToneStack tone_;

    double volume_ = 0.5;
    double bass_ = 0.5, mid_ = 0.5, treble_ = 0.5;
    bool bright_ = false;
    double toneRs_ = 38.0e3;

    // BRIGHT treble-bleed: a one-pole high-shelf whose boost RISES as volume
    // FALLS (the cap across the volume pot bleeds treble around the attenuation —
    // so it "bites" at low volume and fades in at full volume). TPT one-pole.
    double brightA_ = 0.0;   // high-pass coefficient at kBrightHz
    double brightS_ = 0.0;   // filter state
    static constexpr double kBrightHz = 2000.0;  // treble-bleed corner
    static constexpr double kBrightMax = 2.2;    // max extra HF gain (~+7 dB) @ vol=0

    std::vector<float> buf_;
    double lastOutPeak_ = 0.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_TWIN_PREAMP_H
