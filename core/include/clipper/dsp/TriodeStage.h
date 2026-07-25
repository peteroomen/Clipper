// Clipper — portable DSP core.
//
// TriodeStage (M9.1): a validated 12AX7 common-cathode gain stage — the single
// building block that the JCM800 preamp (M9 phase 2) instantiates 3-4x (plus a
// cathode follower), and that every future valve amp reuses. Standalone and
// offline-validated with the M2 measurement discipline; module fidelity is the
// milestone (no user-facing feature yet).
//
// ---------------------------------------------------------------------------
// Device model — Koren 12AX7 plate-current law.
// ---------------------------------------------------------------------------
// Norman Koren, "Improved SPICE models for vacuum-tube amplifiers" (1996), the
// canonical triode approximation used across the modelling literature (Yeh/CCRMA,
// Pakarinen & Yeh). Published 12AX7 parameters:
//
//     mu = 100, ex = 1.4, kg1 = 1060, kp = 600, kvb = 300
//
//     E1 = (Va/kp) * log(1 + exp(kp * (1/mu + Vgk/sqrt(kvb + Va^2))))
//     Ip = (E1^ex / kg1) * (1 + sign(E1))          [amps; sign() zeroes cutoff]
//
// Va = plate-to-cathode voltage, Vgk = grid-to-cathode voltage. The (1 + sign)
// factor rectifies to cutoff for E1 <= 0 and is part of the parameter fit (with
// these constants a -2 V / 250 V bias yields ~0.95 mA, matching the datasheet).
//
// ---------------------------------------------------------------------------
// Circuit — JCM800 2204 first-stage-style common cathode (parameterizable).
// ---------------------------------------------------------------------------
//   B+  = 320 V   plate supply
//   Ra  = 100 k   plate load
//   Rk  = 820 Ohm cathode resistor (self-bias); optional bypass cap Ck
//                 (0.68 uF JCM800 first-stage voicing, or 22 uF fully bypassed)
//   Rg  = 68 k    grid stopper (grid-current clamp path)
//   Cc  = 22 nF   interstage coupling cap  ─┐  the blocking-distortion RC:
//   Rgl = 1 M     next-stage grid leak     ─┘  tau = Rgl*Cc = 22 ms
//
// The stage is a faithful cascade element: an INPUT coupling (Cc + grid leak Rgl
// = the previous stage's output coupling / this grid's DC block) AND an OUTPUT
// coupling (Cc + next-stage grid leak Rgl) that both DC-blocks the output and
// LOADS the plate with the next 1 M grid leak (so the mid-band gain is
// -gm*(Ra || Rgl || rp), not -gm*(Ra || rp)). Every stage is identical, so the
// two couplings share the same Cc / Rgl. Blocking distortion is testable on a
// single stage because it is THIS grid conducting into THIS input coupling cap
// that shifts the bias — recovering through Rgl with tau = Rgl*Cc = 22 ms.
//
// Two large-signal behaviours the ideal transfer curve lacks, both modelled and
// measured (docs/DEVELOPMENT.md, M9.1):
//   * Grid conduction — for Vgk >~ 0 the grid draws current (soft clamp, ~2 k
//     conduction resistance); fed back through the 68 k grid stopper it squashes
//     positive grid peaks asymmetrically (the source of the stage's 2nd-harmonic
//     dominance / one-sided soft clip).
//   * Blocking distortion — that same grid current charges the coupling cap; it
//     recovers through the 1 M grid leak with tau = 22 ms, so a hard overdrive
//     burst shifts the bias toward cutoff and recovers over ~one RC. Present and
//     testable.
//
// ---------------------------------------------------------------------------
// Solver — per-sample nodal Newton (3 unknowns: Va, Vg, Vk).
// ---------------------------------------------------------------------------
// Each sample solves the nonlinear KCL system for the plate (Va), grid (Vg) and
// cathode (Vk) nodes. The reactive elements (coupling cap Cc, cathode bypass Ck)
// are backward-Euler companions folded into the residuals; the coupling network
// collapses to a Thevenin source into the grid node, so the Newton stays 3x3 with
// an analytic Jacobian. Initial guess = the previous sample's solution (the RC
// time constants are milliseconds, the step is microseconds, so 2-4 iterations
// converge). Iteration cap + damped fallback guarantee no NaN / divergence on a
// +/-10 V slam (see the .cpp and the stability test). Cathode network folded in:
// an UNBYPASSED Rk is instantaneous local feedback (degeneration) that lowers the
// gain per the standard -mu*Ra/(Ra+rp+(mu+1)Rk) formula.
//
// Oversampling-ready: the whole (nonlinear + reactive) stage runs inside a
// polyphase-halfband cascade (Oversampler, shared with M2). Aliasing measured at
// 4x vs 8x (docs); the shipped default is 4x.
//
// Convention: 1.0f == 1.0 V. Input is the grid-drive voltage; output is the
// next-stage grid voltage (plate AC through the output coupling, DC-blocked),
// inverting (common-cathode), mid-band |gain| ~64x fully bypassed. Zero
// platform/OS/browser deps (portable C++17).

#ifndef CLIPPER_DSP_TRIODE_STAGE_H
#define CLIPPER_DSP_TRIODE_STAGE_H

#include "clipper/dsp/Oversampler.h"

namespace clipper::dsp {

class TriodeStage {
public:
    // Koren device parameters (default = 12AX7). Cited in the header above.
    struct KorenParams {
        double mu = 100.0;
        double ex = 1.4;
        double kg1 = 1060.0;
        double kp = 600.0;
        double kvb = 300.0;
    };

    // Stage topology. CommonCathode is the M9.1 default (plate load Ra, self-bias
    // Rk, input+output interstage couplings). CathodeFollower (M9.2) models the
    // JCM800 V2B direct-coupled cathode follower: plate tied to B+ (no Ra drop),
    // output taken at the cathode across a large Rk load, grid DC-coupled to an
    // external bias (the previous stage's plate) via `gridBias`. Purely additive:
    // the CommonCathode path is byte-for-byte unchanged.
    enum class Topology { CommonCathode, CathodeFollower };

    // Circuit values (default = JCM800 first-stage-style). All parameterizable so
    // the same module serves every stage of the preamp and future amps.
    struct Config {
        double bPlus = 320.0;     // plate supply (V)
        double Ra = 100.0e3;      // plate load (Ohm)
        double Rk = 820.0;        // cathode resistor (Ohm)
        double Ck = 0.68e-6;      // cathode bypass cap (F); 0 = unbypassed
        double Rg = 68.0e3;       // grid stopper (Ohm)
        double Cc = 22.0e-9;      // interstage coupling cap (F)
        double Rgl = 1.0e6;       // next-stage grid leak (Ohm)
        // Grid-conduction soft clamp: Igk = ig0 * softplus((Vgk - vgt)/vgn),
        // ig0 = vgn/rgk. Conduction resistance ~rgk above threshold vgt, knee vgn.
        double gridRgk = 2.0e3;   // grid conduction resistance (Ohm)
        double gridVgt = 0.0;     // conduction onset Vgk (V)
        double gridVgn = 0.1;     // conduction knee width (V); sharp enough that
                                  // idle (Vgk~-1.1 V) grid current is negligible
        KorenParams tube{};

        // --- M9.2 additive fields (ignored by the CommonCathode path) ----------
        Topology topology = Topology::CommonCathode;
        double gridBias = 0.0;    // CathodeFollower only: DC grid voltage set by the
                                  // direct-coupled driving stage's plate (V). The
                                  // audio input rides on it (DC-coupled, no cap).
    };

    TriodeStage();

    // Configure for a sample rate and max block size. Solves the DC operating
    // point and parks all node/companion states there (no start-up thump).
    // maxBlockSize sizes the oversampling scratch (worst case 8x).
    void prepare(double sampleRate, int maxBlockSize);

    // Replace the circuit/device configuration. Re-solves the operating point on
    // the next prepare() (call before prepare, or call prepare again after).
    void configure(const Config& cfg);
    const Config& config() const { return cfg_; }

    // Select oversampling factor for the stage: 1 / 2 / 4 / 8 (other values snap
    // down to the nearest power of two). Default 4. Re-prepares the reactive
    // companions at the oversampled rate and resets the filter state.
    void setOversampling(int factor);
    int oversampling() const { return os_.factor(); }
    int latencySamples() const { return os_.latencySamples(); }

    // Process numFrames of mono audio, in -> out (may alias). Input is the grid
    // drive in volts; output is the plate AC voltage in volts (DC removed).
    void process(const float* in, float* out, int numFrames);

    // --- Introspection (measurement / tests) --------------------------------
    double quiescentPlateVoltage() const { return vaQuiescent_; }     // Va_q (V)
    double quiescentCathodeVoltage() const { return vkQuiescent_; }   // Vk_q (V)
    double quiescentCurrent() const { return iqQuiescent_; }          // Iq (A)
    double plateVoltage() const { return va_; }             // live Va (V)
    double couplingCapVoltage() const { return vCc_; }      // blocking-cap V
    int lastMaxNewtonIterations() const { return lastMaxIters_; }
    static constexpr int kMaxNewtonIter = 50;

    // Koren plate current Ip(Va, Vgk) with the given params (A). Exposed so tests
    // derive the analytic operating point / small-signal rp, gm, mu independently.
    static double korenPlateCurrent(double Va, double Vgk, const KorenParams& p);

private:
    void reprepareReactive();  // companions at the current oversampled rate
    void solveOperatingPoint();  // DC bias -> vaQuiescent_/vkQuiescent_/iqQuiescent_
    void settleDC();  // run silent samples to the exact discrete fixed point
    // One oversampled-rate sample: nodal Newton, updates node + companion state,
    // returns plate AC voltage (Va - Vq).
    inline float processSampleOS(float x);

    // --- M9.2 cathode-follower path (additive; separate from the CC path) ------
    void solveFollowerOperatingPoint();  // 2x2 DC solve at gridBias, Va = B+ - Vk
    inline float processSampleFollowerOS(float x);  // returns cathode AC (Vk - Vk_q)

    Config cfg_{};
    double sampleRate_ = 44100.0;
    double osRate_ = 176400.0;
    int maxBlockSize_ = 128;

    Oversampler os_;

    // Operating point.
    double vaQuiescent_ = 0.0;
    double vkQuiescent_ = 0.0;
    double iqQuiescent_ = 0.0;

    // Live node voltages (Newton warm-start carries across samples).
    double va_ = 0.0;
    double vg_ = 0.0;
    double vk_ = 0.0;

    // Reactive companion state (backward Euler).
    double gCc_ = 0.0;    // coupling-cap conductance Cc/T (both couplings)
    double gRgl_ = 0.0;   // grid-leak conductance 1/Rgl
    double gCk_ = 0.0;    // cathode-cap conductance Ck/T (0 if unbypassed)
    double Gk_ = 0.0;     // 1/Rk + gCk_  (cathode node total conductance)
    double gOut_ = 0.0;   // output series-RC (Cc||Rgl) conductance loading Va
    double vCc_ = 0.0;    // input coupling-cap voltage (blocking distortion state)
    double vCk_ = 0.0;    // cathode-cap voltage (== Vk history)
    double vCo_ = 0.0;    // output coupling-cap voltage (plate side - out side)

    int lastMaxIters_ = 0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_TRIODE_STAGE_H
