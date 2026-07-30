// Clipper — portable DSP core (M10.2; topology completed 2026-07-30, docs §46).
//
// Ac30Preamp: the Vox AC30 "TOP BOOST" channel preamp as a validated core module.
// The full top-boost channel — TWO 12AX7 gain positions and the cathode follower
// that drives the interactive tone network, with the channel VOLUME between them:
//
//   guitar in
//     -> V1   common cathode, Ra 100k, Rk 1.5k || 25uF (bypassed, warm), B+ 300 V
//     -> VOLUME (audio taper) — the historic top-boost-kit insertion point: the knob
//        sets how hard V2 and everything after it is driven, so on the AC30 the
//        VOLUME knob IS the overdrive, and breakup TRACKS the knob (docs §46; the
//        owner chose this position over the literal AC30/6 post-stack order, which
//        leaves V2's drive fixed — the Twin's §44 defect mirrored)
//     -> V2   the TOP BOOST gain triode: common cathode, same canonical values as V1
//        (Ra 100k, Rk 1.5k || 25uF, B+ 300 V) — the stage the original model was
//        missing entirely (§23 ledgered it; §46 lands it)
//     -> CF   direct-coupled cathode follower (Rk 100k), the low-Z driver the real
//        top-boost network is fed from (rout ≈ 1/gm || Rk, ~230 Ω)
//     -> TOP BOOST tone stack (the interactive treble/bass "brilliance" network):
//        slope 100k, treble pot 500k, bass pot 1M, caps 47 pF / 0.022 uF / 0.022 uF,
//        Cb IN SERIES with the bass rheostat and a 500 k load on the wiper output
//        (both fixed by audit finding 5 — the parallel-Cb + unloaded-output netlist
//        was a structural ~−36 dB mid hole no knob could dial out). SIGNATURE: a
//        lossy, bright, INTERACTIVE network — the wiper taps between the treble-cap
//        node and the bass network, so each knob shifts the other's response, and
//        the mid scoop is now DIALABLE by the bass rheostat instead of hardwired.
//
// Contrast with the Marshall/Fender preamps: the top-boost channel's chime is the
// bright lossy stack + the single-ended V2's even harmonics + the class-A power
// section — not stacked cold-stage preamp gain (V2 is warm, not a JCM V1B).
//
// The three triode stages are the preamp nonlinearity; each antialiases itself with
// the shared M2 Oversampler (the composed Ac30Amp measures the requirement, docs
// §23/§46). The tone stack and the volume are strictly linear at the base rate.
//
// Process API mirrors TwinPreamp: prepare(fs,maxBlock) / setParameter / process(mono,
// in->out). Convention: 1.0f == 1.0 V. Platform-free C++17.

#ifndef CLIPPER_DSP_AC30_PREAMP_H
#define CLIPPER_DSP_AC30_PREAMP_H

#include <array>
#include <vector>

#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/TriodeStage.h"

namespace clipper::dsp {

// ---------------------------------------------------------------------------
// TopBoostToneStack — the Vox AC30 top-boost interactive treble/bass network,
// nodal MNA with trapezoidal capacitor companions (same discipline as the Marshall
// / Fender FMV stacks — the test derives H(jw) from THIS SAME netlist via a complex
// nodal solve and compares; the topology itself is checked against the published
// top-boost schematic, which is what the pre-§46 netlist failed). NO mid control
// (the AC30 top-boost has none) — but the mid SCOOP is dialable via the bass leg.
//
// Netlist (nodes SRC, IN, N2, OUT, N4, N5; GND). SRC is the driven source behind
// its output impedance rs (the cathode follower, ~230 Ω, since §46); the 0.022 uF
// INPUT coupling cap Cc sits in SERIES to IN:
//   Cc  : SRC - IN                   (0.022 uF interstage coupling — series)
//   Ct  : IN  - N2                   (47 pF treble cap — the "brilliance" bypass)
//   R1  : IN  - N4                   (100k slope resistor)
//   RT  : N2 -(1-t)RT- OUT -(t)RT- N4  (500k treble pot; wiper OUT = the output)
//   Cb  : N4 - N5                    (0.022 uF bass cap — IN SERIES with the
//                                     rheostat, audit finding 5's first error: the
//                                     pre-§46 netlist stamped it N4-GND in parallel,
//                                     hardwiring a ~−36 dB mid hole)
//   RB  : N5 -(b)RB- GND             (1M bass pot as a rheostat under Cb)
//   RL  : OUT - GND                  (500k — the following volume-pot/grid-leak
//                                     load, finding 5's second error: the pre-§46
//                                     netlist left the wiper UNLOADED)
// (t,b = treble,bass pot fractions in [0,1].) The wiper OUT tapping BETWEEN the
// treble-cap-fed N2 and the bass network at N4 is the source of the strong treble
// <-> bass interaction, the AC30 tone-control signature. The Cb+RB series branch is
// a frequency-dependent shunt at N4: RB small → mids/highs at N4 shunted (the Vox
// "V"), RB large → the branch lifts away and the mids fill in.
//
// KNOB SMOOTHING (2026-07-25, audit finding 6 — docs §35): the two pot fractions are
// one-pole smoothed (~8 ms) per sample and the matrix inverse is re-derived at a
// 32-sample control rate, only while a smoother is moving. See the note on
// MarshallToneStack in Jcm800Preamp.h for the reasoning.
// ---------------------------------------------------------------------------
class TopBoostToneStack {
public:
    // Canonical AC30 top-boost tone-stack component values (documented constants).
    static constexpr double kRslope = 100.0e3;   // slope resistor (Ohm)
    static constexpr double kRT = 500.0e3;       // treble pot (Ohm)
    static constexpr double kRB = 1.0e6;         // bass pot (Ohm)
    static constexpr double kRLoad = 500.0e3;    // wiper load (following pot/grid leak)
    static constexpr double kCt = 47.0e-12;      // treble cap (F) = 47 pF
    static constexpr double kCb = 22.0e-9;       // bass cap (F)   = 0.022 uF
    static constexpr double kCc = 22.0e-9;       // input coupling cap (F) = 0.022 uF

    void prepare(double sampleRate);
    void reset();
    void setSourceImpedance(double rs);          // V1 plate output impedance (Ohm)
    void setKnobs(double bass, double treble);   // each in [0,1] (NO mid)
    void process(const float* in, float* out, int numFrames);

    // Snap the knob smoothers onto their targets and re-derive the matrix, no ramp.
    void snapKnobs();

    double sourceImpedance() const { return rs_; }

    // Anti-denormal diagnostic (Denormal.h, docs §33) — not used by the audio path.
    // All six cap companions rest at zero (including the vC_/iC_ pair for the series
    // input coupling cap, which the other two stacks do not have), so after a silent
    // tail this must be EXACTLY 0.0.
    double maxAbsRestingState() const {
        return maxAbsState(vC_, iC_, vT_, iT_, vB_, iB_);
    }

private:
    void rebuild();  // recompute the 6x6 conductance matrix + its inverse

    static constexpr int N = 6;  // nodes: SRC=0, IN=1, N2=2, OUT=3, N4=4, N5=5
    static constexpr double kKnobSmoothSeconds = 0.008;  // ~8 ms, as AmpModel
    static constexpr int kCtrlBlock = 32;  // matrix re-derivation interval, in samples

    double sampleRate_ = 44100.0;
    double T_ = 1.0 / 44100.0;
    double rs_ = 45.0e3;   // high-Z plate source (Ra||rp), not a follower
    OnePoleSmootherD bassSm_, trebleSm_;
    double bass_ = 0.5, treble_ = 0.5;  // what the matrix was built from
    bool knobsMoving_ = false;
    int ctrlCounter_ = 0;
    double geqC_ = 0.0, geqT_ = 0.0, geqB_ = 0.0;  // cap companion conductances
    std::array<std::array<double, N>, N> Ginv_{};  // inverse of the node matrix
    double vC_ = 0.0, iC_ = 0.0;   // Cc  (SRC-IN)
    double vT_ = 0.0, iT_ = 0.0;   // Ct  (IN-N2)
    double vB_ = 0.0, iB_ = 0.0;   // Cb  (N4-N5, in series with the bass rheostat)
    bool dirty_ = true;
};

// ---------------------------------------------------------------------------
// The composed AC30 top-boost preamp.
// ---------------------------------------------------------------------------
class Ac30Preamp {
public:
    enum ParamId : int {
        PARAM_VOLUME = 0,   // channel volume — audio taper (the AC30's "overdrive")
        PARAM_BASS = 1,     // top-boost bass   (0..1)
        PARAM_TREBLE = 2,   // top-boost treble (0..1)
        PARAM_COUNT = 3,
    };

    enum Stage : int { V1 = 0, V2 = 1, CF = 2, STAGE_COUNT = 3 };

    Ac30Preamp();

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);
    int oversampling() const { return oversampling_; }

    // All three serial per-stage oversampling round trips (docs §46 — was V1 only,
    // which under-reported the moment V2/CF landed).
    int latencySamples() const {
        return stage_[V1].latencySamples() + stage_[V2].latencySamples() +
               stage_[CF].latencySamples();
    }

    void setParameter(int paramId, float value);
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1). Re-park the triode stage and the top-boost
    // stack cap states at the already-solved DC point, without re-solving anything.
    // Knob values are preserved.
    void reset();

    // --- Introspection (measurement / tests) --------------------------------
    double stageQuiescentPlate(int stage) const;
    double stageQuiescentCathode(int stage) const;
    double stageQuiescentCurrent(int stage) const;
    double toneSourceImpedance() const { return toneRs_; }

    // The audio-taper wiper for the current VOLUME knob — i.e. the smoother's TARGET,
    // the steady-state scale the knob asks for (what an analytic gain test compares to).
    double volumeScale() const;
    static double audioTaper(double x);  // (e^{k x}-1)/(e^k-1), k=4 — exposed to tests

    const TriodeStage::Config& stageConfig(int stage) const { return cfg_[stage]; }
    TopBoostToneStack& toneStack() { return tone_; }

    double lastOutputPeak() const { return lastOutPeak_; }

    // V1 plate AC → tone-stack input trim (grid-leak coupling ~unity). Named for
    // symmetry with the JCM/Twin documented handoffs.
    static constexpr double kV1ToStack = 1.0;

private:
    void configureStages();
    // Deferred prime — see the identical note on Jcm800Preamp::primeSmoothers.
    void primeSmoothers();

    static constexpr double kSmoothSeconds = 0.008;  // ~8 ms, as AmpModel

    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;

    std::array<TriodeStage, STAGE_COUNT> stage_;
    std::array<TriodeStage::Config, STAGE_COUNT> cfg_{};
    TopBoostToneStack tone_;

    double volume_ = 0.5;
    // VOLUME is this amp's PRIMARY overdrive control and measured the single worst step
    // discontinuity in the whole rig (audit finding 6). Smoothed, applied per sample.
    OnePoleSmootherD volSm_;
    bool primed_ = false;
    double bass_ = 0.5, treble_ = 0.5;
    double toneRs_ = 45.0e3;

    std::vector<float> buf_;
    double lastOutPeak_ = 0.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_AC30_PREAMP_H
