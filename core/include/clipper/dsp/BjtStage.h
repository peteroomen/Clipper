// Clipper — portable DSP core.
//
// BjtStage (ROADMAP v1.1 item 4): a validated Ebers-Moll NPN common-emitter gain
// stage with collector-feedback bias — the FIRST bipolar (BJT) device model in the
// core, and the reusable building block the four-transistor Muff fuzz (MuffModel)
// instantiates 4×. It is the BJT sibling of the M9.1 TriodeStage: same nodal-Newton
// house style (per-sample solve, analytic Jacobian, warm-start, iteration cap,
// slam-proof damping), a different device (a transistor, not a valve). It is
// deliberately general so the future Fuzz Face (a 2-transistor germanium PNP-ish
// stage) and the RG100 solid-state preamp can reuse the same machinery with a new
// Config (see the reuse notes in MuffModel.h / docs §24).
//
// ---------------------------------------------------------------------------
// Device model — Ebers-Moll NPN (2N5088-class silicon).
// ---------------------------------------------------------------------------
// The classic transport-form Ebers-Moll equations (Ebers & Moll, 1954), the
// canonical large-signal BJT model. With Vbe = Vb−Ve, Vbc = Vb−Vc:
//
//     If = Is·(exp(Vbe/Vt) − 1)          forward diffusion current
//     Ir = Is·(exp(Vbc/Vt) − 1)          reverse diffusion current
//     Ic = If − Ir·(1 + 1/βR)            collector current
//     Ib = If/βF + Ir/βR                 base current
//     Ie = Ic + Ib                       emitter current
//
// The reverse (Ir) term is what makes the transistor SATURATE when the collector
// is pulled toward (or below) the base — exactly what happens on the fuzz's hard
// clipping peaks, so it is part of the character, not a nicety. Published 2N5088
// small-signal-NPN parameters (documented canon, early-70s "ram's head" family):
//
//     Is = 1.0e-14 A, βF = 400, βR = 4, Vt = 25.85 mV (kT/q at T = 300 K).
//
// βF = 400 is the 2N5088's high-hFE class (that is WHY the Muff has so much gain
// on tap); βR ≈ 4 is a typical low reverse beta; Vt is the thermal voltage (the
// temperature dependence lives here — 300 K is the documented reference).
//
// ---------------------------------------------------------------------------
// Circuit — collector-feedback-biased common emitter (the Muff sustain stage).
// ---------------------------------------------------------------------------
//   Vcc = 9 V            supply
//   Rc  = 10 k           collector load (to Vcc)
//   Rf  = 470 k          COLLECTOR-FEEDBACK bias resistor (base ↔ collector)
//   Re  = 390 Ω          emitter degeneration (unbypassed — local feedback)
//   Cin = 100 nF         input coupling cap (DC-blocks the previous stage)
//   Rb  = 0 Ω (default)  SERIES base resistor in front of Cin — see below
//   Cf  = 470 pF         feedback cap across Rf (HF rolloff — the Muff's smoothness)
//   D±  = 1N4148 pair    OPTIONAL antiparallel diodes base↔collector (clip stages)
//
// **Rb, and why it is load-bearing (audit finding 16, fixed 2026-07-25 — docs §37,
// ADR 009).** Until that slice `Cin` was driven by an IDEAL voltage source landing
// directly on the base node, so the coupling high-pass corner was set by the base-node
// SHUNT impedance alone — rπ ∥ (Miller-reduced Rf) ∥ the diode conductance. On a clip
// stage the diodes conduct at idle by design, which shunts base↔collector hard, and the
// corner back-solved to ≈250 Hz PER STAGE. Cascade four of those and the guitar's low E
// (82.4 Hz) lands 41 dB below 1 kHz: a fuzz with no fuzz-sized low end. Every coupling
// cap in the real pedal sees a series resistance (the input series R, the SUSTAIN pot's
// track, the interstage 10 k's), which is what puts the real corners at 15–50 Hz. Rb is
// that resistance. Rb = 0 reproduces the pre-fix behaviour BIT FOR BIT (gIn == gCin), so
// it stays the default and no other BjtStage consumer is disturbed.
//
// Note that Rb is also a DIVIDER against the base-node impedance — that is physics, not
// a side effect, and it is why the Muff's drive constants were recalibrated in the same
// slice (docs §37). A future Fuzz Face / RG100 Config should set Rb from its own netlist
// rather than inherit 0.
//
// Collector feedback self-biases the collector: the base sits ≈0.6 V above the
// emitter, and the collector sits a diode-drop-or-so above the base (Vc = Vb +
// Ib·Rf with the diodes off). Because Rf is large and βF high, the idle
// collector-base voltage on a CLIP stage exceeds the ~0.5 V diode knee — so the
// clipping diodes CONDUCT NEAR IDLE and pin the collector close to Vb. That is not
// a bug: it is why a Big-Muff-family stage has almost no clean headroom and clips
// essentially always, the root of the pedal's wall-of-sustain compression. The DC
// operating-point solve therefore INCLUDES the diodes for a clip stage.
//
// ---------------------------------------------------------------------------
// Solver — per-sample nodal Newton (3 unknowns: Vb, Vc, Ve).
// ---------------------------------------------------------------------------
// Each sample solves the nonlinear KCL system for the base (Vb), collector (Vc)
// and emitter (Ve) nodes with an analytic 3×3 Jacobian (Cramer's rule, shared
// solve3x3). Reactive elements (Cin input coupling, Cf feedback cap) are
// backward-Euler companions folded into the residuals; the series base resistor Rb and
// the input coupling cap collapse to ONE Thevenin from the driving node. Warm-start = the previous
// sample's solution (RC constants are ms, the step is µs, so 2–4 iterations
// converge). Iteration cap + per-iteration step clamps + a clamped exp guarantee
// no NaN/divergence on a ±10 V slam (see the .cpp and the stability test).
//
// The stage is run at whatever rate prepare() is given: MuffModel prepares its four
// BjtStages at the OVERSAMPLED rate and drives them per oversampled sample through
// ONE shared Oversampler (unlike TriodeStage, which owns its own). So BjtStage has
// no Oversampler member — it is a pure per-sample device, composable in a cascade.
//
// Convention: 1.0f == 1.0 V. Input is the base-drive voltage (AC, DC-blocked by
// Cin); output is the collector AC voltage (Vc − Vc_quiescent), inverting
// (common-emitter). Zero platform/OS/browser deps (portable C++17).

#ifndef CLIPPER_DSP_BJT_STAGE_H
#define CLIPPER_DSP_BJT_STAGE_H

namespace clipper::dsp {

class BjtStage {
public:
    // Ebers-Moll device parameters (default = 2N5088-class). Cited in the header.
    struct EbersMoll {
        double Is = 1.0e-14;   // saturation current (A)
        double betaF = 400.0;  // forward current gain (hFE)
        double betaR = 4.0;    // reverse current gain
        double Vt = 0.02585;   // thermal voltage kT/q at 300 K (V)
    };

    // Antiparallel silicon diode pair (1N4148) across base↔collector. Modeled as
    // a symmetric shunt Id(v) = 2·Isd·sinh(v/(n·Vt)) — the soft, symmetric clip.
    struct DiodePair {
        bool present = false;   // clip stages set this true
        double Isd = 4.35e-9;   // 1N4148 saturation current (A)
        double nVt = 0.0453;    // n·Vt emission voltage (n ≈ 1.75) → ~0.5 V knee
    };

    // Circuit values (default = Muff sustain-stage collector-feedback network).
    // All parameterizable so the same module serves every stage AND future pedals.
    struct Config {
        double Vcc = 9.0;      // supply (V)
        double Rc = 10.0e3;    // collector load (Ω)
        double Rf = 470.0e3;   // collector-feedback bias resistor (Ω)
        double Re = 390.0;     // emitter degeneration resistor (Ω)
        double Cin = 100.0e-9; // input coupling cap (F)
        // SERIES resistance between the driving node and Cin (Ω). Together with the
        // base-node impedance it sets the coupling corner: f = 1/(2π·(Rb+Zbase)·Cin).
        // 0 = the pre-2026-07-25 ideal-source behaviour, kept as the DEFAULT so every
        // other consumer of BjtStage stays bit-identical (audit finding 16, docs §37).
        double Rb = 0.0;
        double Cf = 470.0e-12; // feedback cap across Rf (F)
        EbersMoll bjt{};
        DiodePair diodes{};
    };

    BjtStage();

    void configure(const Config& cfg);
    const Config& config() const { return cfg_; }

    // Prepare for a sample rate (MuffModel passes the OVERSAMPLED rate). Solves the
    // DC operating point (including the clip diodes) and parks node + companion
    // states there, so silence in → silence out from sample 0 (no turn-on thump).
    void prepare(double sampleRate);

    // One sample: nodal Newton at the prepared rate. `vin` is the base-drive
    // voltage; returns the collector AC voltage (Vc − Vc_quiescent). May be called
    // in a tight per-sample cascade (no allocation, no oversampling here).
    float processSample(float vin);

    // Recovery seam (audit finding 1). Re-park the three node voltages and the two
    // cap companions at the settled DC operating point WITHOUT re-solving:
    // prepare() runs a damped 3-D Newton and then settles up to 12·Rf·Cin worth of
    // silent samples. The settled state is cached at the end of settleDC(), so this
    // is six assignments — allocation-free, and bit-identical to a fresh prepare().
    void reset();

    // --- Introspection (measurement / tests) --------------------------------
    double quiescentCollectorVoltage() const { return vcQ_; }  // Vc_q (V)
    double quiescentBaseVoltage() const { return vbQ_; }       // Vb_q (V)
    double quiescentEmitterVoltage() const { return veQ_; }    // Ve_q (V)
    double quiescentCollectorCurrent() const { return icQ_; }  // Ic_q (A)
    int lastMaxNewtonIterations() const { return lastMaxIters_; }
    static constexpr int kMaxNewtonIter = 60;

    // Ebers-Moll collector current Ic(Vbe, Vbc) (A). Exposed so tests derive the
    // analytic operating point independently of the solver.
    static double ebersMollIc(double Vbe, double Vbc, const EbersMoll& p);
    static double ebersMollIb(double Vbe, double Vbc, const EbersMoll& p);

private:
    void reprepareReactive();
    void solveOperatingPoint();  // DC bias (caps open, diodes live) -> quiescents
    void settleDC();             // run silent samples to the discrete fixed point
    void cachePark();            // snapshot the settled state for reset()

    Config cfg_{};
    double sampleRate_ = 176400.0;

    // Operating point.
    double vcQ_ = 0.0, vbQ_ = 0.0, veQ_ = 0.0, icQ_ = 0.0;

    // Live node voltages (Newton warm-start carries across samples).
    double vb_ = 0.0, vc_ = 0.0, ve_ = 0.0;

    // Reactive companions (backward Euler).
    double gCin_ = 0.0;  // Cin/T
    // The series-Rb + Cin branch seen from the base node: gIn_ = gCin_/(1 + Rb·gCin_).
    // Equals gCin_ exactly when Rb == 0, which is what makes that path bit-identical.
    double gIn_ = 0.0;
    double gCf_ = 0.0;   // Cf/T
    double vCin_ = 0.0;  // input-cap voltage history (Vin − Vb)
    double vCf_ = 0.0;   // feedback-cap voltage history (Vc − Vb)

    // The SETTLED zero-input fixed point, snapshotted by cachePark(). reset()
    // restores from here so recovery costs no Newton solve and no settling.
    double vbPark_ = 0.0, vcPark_ = 0.0, vePark_ = 0.0;
    double vCinPark_ = 0.0, vCfPark_ = 0.0;

    int lastMaxIters_ = 0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_BJT_STAGE_H
