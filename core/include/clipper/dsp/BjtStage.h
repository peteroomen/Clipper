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
//   Cf  = 470 pF         feedback cap across Rf (HF rolloff — the Muff's smoothness)
//   Rs  = 10 k           OPTIONAL series base resistor (clip stages — docs §49)
//   Rbg = 100 k          OPTIONAL base-to-ground bias resistor (clip stages — §53)
//   D±  = 1N4148 pair    OPTIONAL antiparallel diodes in the collector→base branch
//   Cd  = 1 µF           OPTIONAL DC-BLOCKING cap in series with that diode pair
//
// Collector feedback self-biases the collector: the base sits ≈0.6 V above the
// emitter, and the collector sits Ib·Rf above the base (with Rbg present, the
// bias-divider current through Rf dominates Ib and lifts the collector to a real
// mid-rail operating point).
//
// THE DIODE BRANCH IS DC-BLOCKED WHEN Cd > 0, AND THAT CHANGES THE STAGE'S WHOLE
// CHARACTER (docs §53, 2026-07-31). The real Big Muff puts a 1 µF cap (C6/C7) in
// series with the feedback diodes precisely so the branch carries NO DC: the bias
// is set by Rf + Rbg alone and the diodes only clip the AC swing. Until §53 this
// model had the pair DC-coupled, so the idle collector-base voltage exceeded the
// ~0.5 V knee and the diodes CONDUCTED AT IDLE, pinning the collector ~0.26 V
// above the base. That was documented here as canon ("a Big-Muff stage has almost
// no clean headroom") — it was an artifact of the missing cap, and it is what cost
// the pedal its bass (the conducting pair shunted the base node to ~1.8 k, putting
// each clip stage's coupling corner at ~900 Hz) and its swing. With Cd present the
// DC solve finds the diodes OFF (Vd = Vb, zero branch current), so a clip stage
// biases like a real one and clips symmetrically about that bias.
//
// ---------------------------------------------------------------------------
// Solver — per-sample nodal Newton (3 unknowns, or 4 with the DC-blocked branch).
// ---------------------------------------------------------------------------
// Each sample solves the nonlinear KCL system for the base (Vb), collector (Vc)
// and emitter (Ve) nodes with an analytic 3×3 Jacobian (Cramer's rule, shared
// solve3x3). Reactive elements (Cin input coupling, Cf feedback cap) are
// backward-Euler companions folded into the residuals; the input coupling cap
// collapses to a Thevenin from the driving node. Warm-start = the previous
// sample's solution (RC constants are ms, the step is µs, so 2–4 iterations
// converge). Iteration cap + per-iteration step clamps + a clamped exp guarantee
// no NaN/divergence on a ±20 V slam (see the .cpp and the stability test).
//
// With Cd > 0 the junction between Cd and the diode pair becomes a FOURTH unknown
// (Vd) and the system is 4×4 (Gaussian elimination with partial pivoting). The
// two paths are separate code: with Cd == 0 the stage runs the identical 3×3
// residual/Jacobian/Cramer solve it always did, BIT-IDENTICALLY (verified by
// render hash — docs §53), so Q1/Q4 and every non-Muff user are untouched.
//
// The loop exits on the KCL RESIDUAL reaching kNewtonResidualTolA (1e-17 A — the
// .cpp carries the measured derivation and the accuracy trade), so a warm start that
// is already the solution costs ONE system evaluation and ZERO iterations. Before
// that early-out existed, an already-converged sample was the solver's WORST case:
// the backtracking line search accepts only a STRICT residual decrease, which is
// unsatisfiable at the floating-point floor, so it burned all 30 backtracks — 31
// evaluations to reproduce the answer it already had. That is why the Muff cost ~3x
// more CPU when you were NOT playing than when you were (audit finding 12, docs §34,
// fixed 2026-07-25). The early-out is NOT bit-identical to the old solver — it
// declines sub-picovolt refinement the old one performed — but it is 7.4 dB inside
// the project's -120 dBFS solver-accuracy gate, measured every run by
// test_tube_solver.cpp. The DC operating-point solve deliberately opts out.
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
        double Cf = 470.0e-12; // feedback cap across Rf (F)
        EbersMoll bjt{};
        DiodePair diodes{};
        // Finding 16 (bass half) / the max-sustain blowout — docs §49, ADR 009:
        // the real coupling network's SERIES BASE RESISTOR. With Rs = 0 every code
        // path reduces EXACTLY to the pre-§49 solver (bit-identical, verified), so
        // the RAT/GOLD/SD/TS users are untouched.
        double Rs = 0.0;       // series base resistor between Cin and the base (Ω)
        // Base-to-ground bias resistor (RA on the published Big Muff schematics,
        // 100 k). Plumbed by §49 but left UNUSED there, because with the diode
        // branch still DC-COUPLED it only parked the stage deeper in the knee.
        // USED as of docs §53, together with Cdiode below — the two are one
        // decision: RA sets the DC bias current through Rf, which is only the
        // real circuit's bias once the diodes stop carrying DC.
        double Rbg = 0.0;      // base-to-ground bias resistor (Ω); 0 = absent
        // The DC-BLOCKING cap in series with the antiparallel diode pair (C6/C7 on
        // the published schematics, 1 µF) — docs §53, ADR 009's named follow-up.
        // > 0 adds a FOURTH Newton node (the Cd↔diode junction) and the diodes
        // then carry no DC. 0 keeps the legacy DC-coupled pair and the 3-node
        // solver, bit-identically. Only meaningful when diodes.present.
        double Cdiode = 0.0;   // DC-block cap in the diode branch (F); 0 = absent
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

    // Recovery seam (audit finding 1). Re-park the node voltages and the cap
    // companions at the settled DC operating point WITHOUT re-solving: prepare()
    // runs a damped Newton and then settles up to 12·Rf·Cin worth of silent
    // samples. The settled state is cached at the end of settleDC(), so this is a
    // handful of assignments — allocation-free, and bit-identical to a fresh
    // prepare(). The DC-blocked diode branch's node + cap state (vd_, vCd_) are
    // parked here too; every recursive state in this class must be.
    void reset();

    // --- Introspection (measurement / tests) --------------------------------
    double quiescentCollectorVoltage() const { return vcQ_; }  // Vc_q (V)
    double quiescentBaseVoltage() const { return vbQ_; }       // Vb_q (V)
    double quiescentEmitterVoltage() const { return veQ_; }    // Ve_q (V)
    double quiescentCollectorCurrent() const { return icQ_; }  // Ic_q (A)
    // The DC-blocked branch's junction node at the settled operating point (V).
    // With the branch present this must equal the base voltage — the diodes carry
    // no DC, which is the whole point of C6/C7 (docs §53). Meaningless (parked at
    // vb_) when Cdiode == 0.
    double quiescentDiodeNodeVoltage() const { return vdPark_; }
    // High-water mark of Newton iterations that actually MOVED the iterate, since
    // prepare()/reset(). Guaranteed <= kMaxNewtonIter (it used to be able to report
    // kMaxNewtonIter + 1, because the solver returned `it + 1` even when the loop
    // exhausted the cap without taking a step — audit finding 12's secondary note).
    // 0 is the healthy PARKED reading: with no input the warm start is already the
    // solution and the residual early-out returns without iterating, which is the
    // property test_muff_model.cpp block "idle solver cost" pins.
    int lastMaxNewtonIterations() const { return lastMaxIters_; }
    static constexpr int kMaxNewtonIter = 60;

    // Ebers-Moll collector current Ic(Vbe, Vbc) (A). Exposed so tests derive the
    // analytic operating point independently of the solver.
    static double ebersMollIc(double Vbe, double Vbc, const EbersMoll& p);
    static double ebersMollIb(double Vbe, double Vbc, const EbersMoll& p);

    // True when this stage runs the 4-node system (diodes present AND DC-blocked).
    bool usesDcBlockedDiodes() const;

private:
    void reprepareReactive();
    void solveOperatingPoint();  // DC bias (caps open, diodes live) -> quiescents
    void settleDC();             // run silent samples to the discrete fixed point
    void cachePark();            // snapshot the settled state for reset()

    Config cfg_{};
    double sampleRate_ = 176400.0;

    // Operating point. vdQ_ is the DC-blocked branch's junction node; the DC solve
    // puts it exactly at vbQ_ (zero diode current), and it mirrors vbQ_ when the
    // branch is absent so nothing downstream ever reads an uninitialised node.
    double vcQ_ = 0.0, vbQ_ = 0.0, veQ_ = 0.0, icQ_ = 0.0, vdQ_ = 0.0;

    // Live node voltages (Newton warm-start carries across samples). vd_ is the
    // 4th node — the DC-block cap ↔ diode-pair junction — and is inert (and stays
    // parked at vb_) unless cfg_.Cdiode > 0.
    double vb_ = 0.0, vc_ = 0.0, ve_ = 0.0, vd_ = 0.0;

    // Reactive companions (backward Euler).
    double gCin_ = 0.0;  // Cin/T
    double gCf_ = 0.0;   // Cf/T
    double gCd_ = 0.0;   // Cdiode/T (0 when the branch is DC-coupled)
    double vCin_ = 0.0;  // input-cap voltage history (Vin − Vb)
    double vCf_ = 0.0;   // feedback-cap voltage history (Vc − Vb)
    double vCd_ = 0.0;   // diode-block-cap voltage history (Vc − Vd)

    // The SETTLED zero-input fixed point, snapshotted by cachePark(). reset()
    // restores from here so recovery costs no Newton solve and no settling.
    double vbPark_ = 0.0, vcPark_ = 0.0, vePark_ = 0.0, vdPark_ = 0.0;
    double vCinPark_ = 0.0, vCfPark_ = 0.0, vCdPark_ = 0.0;

    int lastMaxIters_ = 0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_BJT_STAGE_H
