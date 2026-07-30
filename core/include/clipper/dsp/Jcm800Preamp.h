// Clipper — portable DSP core (M9.2).
//
// Jcm800Preamp: the full Marshall JCM800 2204 PREAMP as a validated core module
// (M9 phase 2). Phase 1 (M9.1) delivered the single validated 12AX7 common-cathode
// TriodeStage; this composes FOUR of them into the 2204 preamp voicing chain:
//
//   guitar in
//     -> V1A   common cathode, Ra 100k, Rk 820 || 0.68uF (bypassed, bright)
//     -> 0.022uF coupling + 470k / 1M GAIN pot          (the GAIN knob)
//     -> V1B   common cathode, Ra 82k, Rk 10k UNBYPASSED (the cold 2nd stage)
//     -> 0.022uF coupling + 470k grid leak
//     -> V2A   common cathode, Ra 100k, Rk 820 || 0.68uF (bright)
//     -> DIRECT-COUPLED cathode follower V2B (100k cathode load, low-Z output)
//     -> Marshall TMB tone stack (slope 33k, treble 250k, bass 1M, mid 25k)
//     -> MASTER volume (1M, audio taper)
//   B+ ~ 320 V throughout.
//
// The cascade is THE nonlinearity: each TriodeStage antialiases itself with the
// shared M2 Oversampler (measured OS requirement in docs/DEVELOPMENT.md §14). The
// tone stack and the interstage/GAIN/MASTER networks are strictly linear and run
// at the base rate.
//
// Why the follower exists: the passive TMB stack is lossy and impedance-sensitive;
// the cathode follower presents a LOW source impedance (~1/gm || Rk ~ a few hundred
// ohms) so the tone stack response matches its (high-Z-source) analytic transfer.
//
// Composition of validated stages: V1A/V1B/V2A are TriodeStage in its unchanged
// M9.1 CommonCathode mode (each stage's grid-leak / coupling network is expressed
// through its Config: Rgl sets the plate load = the NEXT stage's input resistance,
// and the built-in input+output couplings AC-couple the cascade). V2B is the
// additive TriodeStage CathodeFollower topology, direct-coupled: its grid DC bias
// is V2A's quiescent plate voltage, solved at prepare().
//
// Process API mirrors RatModel/SdModel: prepare(fs,maxBlock) / setParameter /
// process(mono, in->out). No C ABI yet (that lands in M9 phase 4). Convention:
// 1.0f == 1.0 V. Platform-free C++17 (no OS/browser/Emscripten includes).

#ifndef CLIPPER_DSP_JCM800_PREAMP_H
#define CLIPPER_DSP_JCM800_PREAMP_H

#include <array>
#include <vector>

#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/TriodeStage.h"

namespace clipper::dsp {

// ---------------------------------------------------------------------------
// Marshall TMB (FMV) passive tone stack — nodal MNA, discretized with
// trapezoidal (bilinear) capacitor companions. Public so its response is
// independently testable (the test derives H(jw) from the same netlist via a
// complex nodal solve and compares).
//
// Netlist (nodes IN, N2, N3, N4, OUT; GND):
//   Ct  : IN - N2                    (treble cap)
//   Rs* : source - IN                (follower output impedance; * driven input)
//   R1  : IN - N3                    (slope resistor)
//   RT  : N2 -(1-t)RT- OUT -(t)RT- N3  (treble pot; wiper OUT is the output)
//   RB  : N3 -(l*RB)- N4             (bass pot as rheostat)
//   Cb  : N3 - N4                    (bass cap, across the bass pot)
//   RM  : N4 -(m*RM)- GND            (mid pot as rheostat to ground)
//   Cm  : N4 - GND                   (mid cap, across the mid pot)
// (t,m,l = treble,mid,bass pot fractions in [0,1].)
//
// Cap values: the spec's "0.47" treble cap is 0.47 nF = 470 pF (a 0.47 uF treble
// cap would put the treble corner at ~1 Hz — unphysical); bass/mid = 0.022 uF.
//
// KNOB SMOOTHING (2026-07-25, audit finding 6 — docs §35). setKnobs() used to swap
// the whole 5x5 conductance matrix and its inverse at a block boundary, with the
// trapezoidal cap states carried across unchanged: a one-step topology change
// mid-note. Now the three pot fractions are one-pole smoothed (~8 ms) per sample and
// the matrix is re-derived from them at a 32-sample control rate — the same shape
// AmpModel uses for its four biquads. Carrying the cap states IS correct here, and is
// exactly why this works: they are physical node voltages/currents and a real pot
// wiper perturbs the network under them too. What was wrong was the SIZE of the
// perturbation, and smoothing makes each one ~8 % of a knob step instead of all of it.
// Interpolating between two matrix INVERSES was rejected: the inverse of a
// conductance matrix is not affine in the pot fraction, so a blend of two inverses is
// not the inverse of any network.
// ---------------------------------------------------------------------------
class MarshallToneStack {
public:
    // Canonical JCM800 2204 tone-stack component values (documented constants).
    static constexpr double kRslope = 33.0e3;    // slope resistor (Ohm)
    static constexpr double kRT = 250.0e3;       // treble pot (Ohm)
    static constexpr double kRB = 1.0e6;         // bass pot (Ohm)
    static constexpr double kRM = 25.0e3;        // mid pot (Ohm)
    static constexpr double kCt = 470.0e-12;     // treble cap (F) = spec "0.47" nF
    static constexpr double kCb = 22.0e-9;       // bass cap (F)
    static constexpr double kCm = 22.0e-9;       // mid cap (F)

    void prepare(double sampleRate);
    void reset();
    void setSourceImpedance(double rs);          // follower output impedance (Ohm)
    void setKnobs(double bass, double mid, double treble);  // each in [0,1]
    void process(const float* in, float* out, int numFrames);

    // Snap the knob smoothers onto their targets and re-derive the matrix from them,
    // with no ramp. Called from the owning preamp's deferred prime (the first
    // process() after prepare()) and from reset().
    void snapKnobs();
    // Anti-denormal diagnostic (Denormal.h, docs §33) — not used by the audio path.
    // All six cap companions rest at zero, so after a silent tail this must be EXACTLY
    // 0.0. Measured 68.2x slower than hardware FTZ before the flush: the worst denormal
    // cliff of any single component in the core.
    double maxAbsRestingState() const {
        return maxAbsState(vT_, iT_, vB_, iB_, vM_, iM_);
    }

private:
    void rebuild();  // recompute the 5x5 conductance matrix + its inverse

    static constexpr int N = 5;  // nodes: IN=0, N2=1, N3=2, N4=3, OUT=4
    static constexpr double kKnobSmoothSeconds = 0.008;  // ~8 ms, as AmpModel
    static constexpr int kCtrlBlock = 32;  // matrix re-derivation interval, in samples

    double sampleRate_ = 44100.0;
    double T_ = 1.0 / 44100.0;
    double rs_ = 371.0;
    // Smoothed knob targets, and the values the CURRENT matrix inverse was built from.
    OnePoleSmootherD bassSm_, midSm_, trebleSm_;
    double bass_ = 0.5, mid_ = 0.5, treble_ = 0.5;
    // False whenever all three smoothers sit exactly on their targets, which is the
    // case for every sample of a static render — so the per-sample smoother work and
    // the control-tick comparison cost literally nothing when nobody is turning a knob.
    bool knobsMoving_ = false;
    int ctrlCounter_ = 0;
    double geqT_ = 0.0, geqB_ = 0.0, geqM_ = 0.0;  // cap companion conductances
    std::array<std::array<double, N>, N> Ginv_{};  // inverse of the node matrix
    // Trapezoidal cap states (v = across-cap voltage, i = cap current), prev step.
    double vT_ = 0.0, iT_ = 0.0;   // Ct  (IN-N2)
    double vB_ = 0.0, iB_ = 0.0;   // Cb  (N3-N4)
    double vM_ = 0.0, iM_ = 0.0;   // Cm  (N4-GND)
    bool dirty_ = true;
};

// ---------------------------------------------------------------------------
// The composed 2204 preamp.
// ---------------------------------------------------------------------------
class Jcm800Preamp {
public:
    enum ParamId : int {
        PARAM_GAIN = 0,    // preamp volume (1M pot) — the drive knob, audio taper
        PARAM_MASTER = 1,  // master volume (1M pot) — audio taper
        PARAM_BASS = 2,    // TMB bass   (0..1)
        PARAM_MID = 3,     // TMB mid    (0..1)
        PARAM_TREBLE = 4,  // TMB treble (0..1)
        PARAM_COUNT = 5,
    };

    // Stage indices for introspection.
    enum Stage : int { V1A = 0, V1B = 1, V2A = 2, V2B = 3, STAGE_COUNT = 4 };

    Jcm800Preamp();

    // Configure for a sample rate and max block size. Solves every stage's DC
    // operating point (including the direct-coupled follower's shared bias) and
    // parks all state at it.
    void prepare(double sampleRate, int maxBlockSize);

    // Oversampling factor for EVERY triode stage in the cascade (1/2/4/8).
    // Default 4 (the measured requirement; see docs §14).
    void setOversampling(int factor);
    int oversampling() const { return oversampling_; }

    // Round-trip oversampling-filter group delay in base-rate samples (0 at 1x).
    // Every 12AX7 triode stage oversamples independently and runs SERIALLY, so the
    // four stages' halfband group delays accumulate; the passive TMB tone stack is
    // a linear IIR (no counted latency). Sums the per-stage accessors.
    int latencySamples() const {
        int n = 0;
        for (int s = V1A; s <= V2B; ++s)
            n += stage_[static_cast<size_t>(s)].latencySamples();
        return n;
    }

    // Normalized parameter in [0,1], clamped (NaN-rejecting — see ParamGuard.h).
    void setParameter(int paramId, float value);

    // Recovery seam (audit finding 1). Re-park all four triode stages and the TMB
    // cap states at the already-solved DC point. Does NOT re-solve: the follower
    // bias, the per-stage operating points and the tone-stack matrix inverse all
    // stay as prepared, so this costs ~microseconds where prepare() costs ~tens of
    // milliseconds. Knob values are preserved (only recursive state is cleared).
    void reset();

    // Process mono audio, in -> out (may alias). Input = guitar grid drive in V.
    void process(const float* in, float* out, int numFrames);

    // --- Introspection (measurement / tests) --------------------------------
    double stageQuiescentPlate(int stage) const;    // plate-cathode V at idle
    double stageQuiescentCathode(int stage) const;  // cathode V at idle
    double stageQuiescentCurrent(int stage) const;  // plate current at idle (A)
    double followerGridBias() const { return followerGridBias_; }
    double followerSourceImpedance() const { return followerRout_; }

    // Interstage scalars for the CURRENT knob settings (analytic-gain tests). These
    // report the smoothers' TARGETS — i.e. the steady-state scale the knobs ask for,
    // which is what an analytic small-signal prediction has to be compared against.
    double gainInterstageScale() const;   // V1A out -> V1B grid drive (GAIN net), DC
    // The gain network's real parts (docs §47): V1A's coupling feeds kGainSeriesOhms
    // into the 1M gain pot, and the 2204's 470 pF BRIGHT CAP bridges the pot's top
    // lug to the wiper. kGainDivider is the network's DC series loss with the pot
    // fully up — 1M / (1M + 470k) — and doubles as the exact DC gain factor
    // (H(0) = wiper · kGainDivider for any wiper, from the nodal solve below).
    static constexpr double kGainDivider = 0.68;  // 1M / (1M + 470k) series loss
    static constexpr double kGainSeriesOhms = 470.0e3;
    static constexpr double kGainPotOhms = 1.0e6;
    static constexpr double kBrightCapF = 470.0e-12;
    double masterScale() const;           // tone-stack out -> output (MASTER)
    // Audio taper law for the GAIN / MASTER pots: (e^{k x} - 1)/(e^k - 1), k = 4
    // (~12% at noon). Exposed so tests reproduce it.
    static double audioTaper(double x);

    // White-box: peak |grid drive| seen by V1B during the last process() call
    // (V1B's linear window before cutoff/grid conduction ~= its cathode bias).
    double lastV1bGridDrivePeak() const { return lastV1bGridPeak_; }
    double lastOutputPeak() const { return lastOutPeak_; }

    // Component/config accessors so tests derive the per-stage analytic gains.
    const TriodeStage::Config& stageConfig(int stage) const { return cfg_[stage]; }

private:
    void configureStages();
    // Deferred prime: snap GAIN/MASTER/tone-stack smoothers onto their targets. The
    // house convention is "push targets, then prepare" (ClipperEngine::prepare,
    // identical_core_test), but the C ABI prepares inside amp_create and the knobs
    // arrive afterwards — so the snap has to happen at the first process() instead,
    // or every ABI render (and every golden) would ramp 8 ms up from the defaults.
    void primeSmoothers();

    static constexpr double kSmoothSeconds = 0.008;  // ~8 ms, as AmpModel

    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;

    std::array<TriodeStage, STAGE_COUNT> stage_;
    std::array<TriodeStage::Config, STAGE_COUNT> cfg_{};
    MarshallToneStack tone_;

    double gain_ = 0.5, master_ = 0.5;
    // The GAIN network scale and the MASTER wiper, smoothed and applied PER SAMPLE.
    // These carry the POST-taper linear scale (not the knob), so the audio taper's
    // exp() stays out of the sample loop — same trade AmpModel makes for its volume.
    OnePoleSmootherD gainSm_, masterSm_;
    // Bright-cap shelf state (docs §47): one-pole/one-zero bilinear of the exact
    // series-R + pot + 470 pF network, coefficients re-derived at kBcCtrlBlock while
    // the GAIN smoother moves. bcY1_ rests at ZERO -> flushDenormal (docs §33).
    static constexpr int kBcCtrlBlock = 32;
    double bcB0_ = 0.0, bcB1_ = 0.0, bcA1_ = 0.0;  // y = b0 x + b1 x1 - a1 y1
    double bcX1_ = 0.0, bcY1_ = 0.0;
    double bcW_ = -1.0;  // wiper the coefficients were built from (-1 = stale)
    int bcCtr_ = 0;
    bool bcBypass_ = false;
    void rebuildBrightCap(double wiper);
    bool primed_ = false;
    double bass_ = 0.5, mid_ = 0.5, treble_ = 0.5;
    double followerGridBias_ = 0.0;
    double followerRout_ = 371.0;

    std::vector<float> buf_;   // interstage scratch (maxBlock)
    double lastV1bGridPeak_ = 0.0;
    double lastOutPeak_ = 0.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_JCM800_PREAMP_H
