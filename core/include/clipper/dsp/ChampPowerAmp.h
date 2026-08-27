// Clipper — portable DSP core (M10.10).
//
// ChampPowerAmp: the Fender tweed Champ 5F1 POWER SECTION — ONE 6V6GT beam power
// tetrode, cathode-biased, straight into the output transformer. There is no phase
// inverter, no negative feedback, no master volume and no tone stack anywhere in
// this amp. Modelled from circuit physics and MEASURED (docs §73), same discipline
// as the M9.3 Marshall, M10.1 Twin and M10.2 AC30 power sections. Convention: real
// circuit VOLTS internally; process() output normalized so 1.0f == full scale
// (kFullScaleSecV). Platform-free C++17.
//
// ===========================================================================
// WHY THIS FILE EXISTS: IT IS THE FIRST SINGLE-ENDED OUTPUT STAGE IN THE PROJECT
// ===========================================================================
// Every other power section here — Jcm800PowerAmp, TwinPowerAmp, Ac30PowerAmp,
// OrangePowerAmp, RockerverbPowerAmp, MesaPowerAmp — is a push-pull pair behind a
// phase inverter. Three consequences follow from having one tube instead of two,
// and each is an assertion in clipper_champ_tests rather than a claim here:
//
//  (a) NOTHING CANCELS THE EVEN HARMONICS. A push-pull pair's h2 cancellation is
//      the property TwinPowerAmp exists to have (the balanced Twin leaks −42.8 dBc,
//      docs §42). A single-ended stage has no opposite leg to cancel against, so
//      h2 is DOMINANT BY TOPOLOGY. That is most of why a Champ sounds like a Champ
//      and it is not a voicing choice — no re-skin of the existing machinery can
//      produce it, and no amount of tuning can remove it.
//
//  (b) THE PLATE LOAD LINE IS REAL HERE — audit finding 9, from the other side.
//      Finding 9 measured that the push-pull sections need ~530 mA from one EL34
//      before the plate reaches the knee, more than twice a real tube's peak
//      cathode current, because `Vp = rail − (i − iq)·Rreflected` is a SINGLE-ENDED
//      relation being applied to a centre-tapped primary where each plate actually
//      depends on the DIFFERENTIAL current. So in those amps plate-load saturation,
//      which their headers name as the clipping mechanism, never actually happens
//      and clipping comes entirely from grid cutoff and conduction.
//
//      In a genuinely single-ended stage that same equation is EXACTLY CORRECT: one
//      tube, one primary winding, `Vp = rail − (Ip − Iq)·kRload` is the AC load line
//      through the quiescent point. This file therefore uses the identical solver
//      shape as its push-pull siblings and is the one place in the project where it
//      is right. Measured: the plate reaches the knee at a PHYSICAL current well
//      inside a 6V6's rating, so plate-load saturation is a live clipping mechanism
//      here for the first time. Do NOT "fix" this to match the siblings.
//
//  (c) THERE IS NO NEGATIVE FEEDBACK AT ALL. Not "a shallow loop" — none. The
//      anti-NFB catcher precedent from the AC30 (docs §23) applies verbatim:
//      setFeedbackEnabled() is an inert no-op and the test asserts open-loop and
//      closed-loop renders are BIT-IDENTICAL, so a later slice cannot quietly add a
//      loop to tidy the distortion up.
//
// ===========================================================================
// THE 6V6GT DEVICE CARD — DERIVED, NOT INHERITED (docs §73.2, audit finding 10)
// ===========================================================================
// TWO published Koren 6V6 fits are reachable and they DISAGREE substantially. Both
// were evaluated against the RCA/TAD datasheet at two operating points before any
// code was written — P1 (Vp 250, Vg2 250, Vg1 −12.5 → Ip 45 mA, Ig2 5 mA) and
// P2 (Vp 315, Vg2 225, Vg1 −13 → Ip 34 mA):
//
//   fit                                   P1 Ip    P2 Ip    P1 Ig2   P2 Ig2
//   A  pedalkernel  mu 12  kg1 1100 ...    0.66x    0.59x     1.40x    1.96x
//   B  vgreff Koren mu 10.7 kg1 1672 ...   1.03x    0.98x     2.26x    3.68x
//
// Fit B's SHAPE and kg1 are right and its kg2 is not: the plate current lands within
// 2–3 % at two different Vp AND Vg2, with errors of OPPOSITE SIGN, which is a real
// external check rather than a self-fit. So B's mu/ex/kg1/kp/kvb ship UNMODIFIED —
// deliberately NOT trimmed to hit P1 exactly, because that would worsen P2 — and
// only kg2 is derived, against P1's screen current (the one screen figure that could
// actually be sourced):
//
//   kg2: 4500 -> 10148.2      (Ig2 at P1 exactly 5.000 mA)
//
// THIS IS AUDIT FINDING 10 ON A THIRD TUBE, AND IT IS FIXED HERE. Finding 10's table
// reads EL34 0.102 / 6L6 0.210 / EL84 0.363 for Ig2/Ip against real ratios near 0.10.
// The 6V6 continues it exactly: both published fits predict Ig2/Ip ≈ 0.237–0.244
// against the datasheet's 5/45 = 0.111, i.e. 2.1–2.2x too high. At this amp's own
// idle the published fit B puts screen dissipation at 2.75 W — PRECISELY the 6V6GT's
// rating — which is the same "exceeds its rating at idle" pathology the audit
// measured on the AC30's EL84. The derived card sits at 1.12 W.
//
// KNOWN RESIDUAL, REPORTED NOT FITTED: one kg2 cannot match both datasheet points
// (P2's screen lands 1.63x high) because THE KOREN SCREEN LAW HAS NO Vp DEPENDENCE
// — which is finding 10's own closing paragraph, and the reason a real power tube's
// screen current surges when Vp falls below Vg2. Not modelled; named. Do not close
// it by re-fitting kg2 to split the difference: that would trade a sourced number
// for a fitted one and still not produce the surge.
//
// ===========================================================================
// THE SECOND ABSOLUTE REFERENCE: FENDER'S OWN MEASURED 5F1 VOLTAGES
// ===========================================================================
// Fender publish this amp's operating point — 19 V across the 470 Ω cathode resistor
// (= 40.4 mA total cathode current), a plate node of ~305 V (so Vpk = Vg2k = 286 V)
// and 37 mA of plate current, leaving ~3.4 mA of screen current by difference. That
// is an ABSOLUTE external reference of the same kind §69's Mesa sheets provided, and
// only the SECOND one this project has ever had for an amp.
//
// EXACTLY ONE constant is derived against it — kVsupply, which pins the 305 V node.
// Everything else below is the device card's own prediction at that node:
//
//   plate node  305.000 V  (Fender 305)   1.0000x   <- the one derived number
//   cathode Vk   18.892 V  (Fender  19)   0.9943x
//   Ip           36.280 mA (Fender  37)   0.9805x
//   Ig2           3.916 mA (Fender ~3.4)  1.1517x
//   Ik           40.196 mA (Fender  40.4) 0.9950x
//   Vpk         286.108 V  (Fender 286)   1.0004x
//
// 0.5–2 % on three independent quantities from a card fitted to a DIFFERENT
// manufacturer's datasheet. If you change the device card, this table is the check.
//
// ===========================================================================
// THE SUPPLY — a §55 Thévenin source, and this amp SAGS
// ===========================================================================
// kRsupply is CONSTANT (docs §55's call, and its reasoning transfers: a growing
// "rectifier knee" is a voicing knob rather than physics):
//   kRrect5Y3     480 Ω  SOURCED — the 5Y3GT's published 60 V drop at 125 mA
//                        (Tung-Sol / Sylvania), the same method §55 used for the
//                        GZ34's 75.6 Ω from Philips' 17 V at 225 mA.
//   kRptSecondary 200 Ω  RECONSTRUCTION — the conducting half of a small combo's HT
//                        winding. NAMED as a reconstruction; §57's rule applies.
//
// 680 Ω against the AC30's 134.6 Ω is the headline: a 5Y3 is a directly-heated
// rectifier in a very small amp, and with the 16 µF reservoir the supply RC is
// 10.9 ms. That is why a cranked Champ compresses and blooms the way it does, and
// the sag is a SUPPLY effect acting on the rail, the screen and the bias the tube
// itself sees — never a saturator downstream of the transformer (docs §55's
// retired mechanism; do not reintroduce it).
//
// NOTE ON THE SOURCE NETLIST: the one component-level source reachable for this amp
// models the supply as a 100 Ω series resistance, which drops ~4 V and lands the
// plate node at 320 V against Fender's own measured 305 V. It also models the OT
// primary as a DC 5 kΩ to B+, which would sit the plate 170 V below the rail. Both
// are that project's simplifications and NEITHER is inherited here. See docs §73.1.
//
#pragma once

#include "clipper/dsp/Jcm800PowerAmp.h"   // El34Params + the shared Koren evaluators
#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/TriodeStage.h"

namespace clipper::dsp {

// 6V6GT Koren pentode parameters. Shape + kg1 are the published "GE data sheet"
// Koren fit, validated against TWO datasheet plate currents (1.03x / 0.98x); kg2 is
// DERIVED against the datasheet screen current — see the header. Reuses the same
// El34Params-shaped struct + el34PlateCurrent/el34ScreenCurrent evaluators (the
// Koren pentode form is device-agnostic — only the six constants differ).
struct Tube6V6Params {
    double mu = 10.70;
    double ex = 1.310;
    double kg1 = 1672.0;
    double kp = 41.16;
    double kvb = 12.7;
    double kg2 = 10148.2;   // DERIVED (published fits say 4500 — finding 10)
};

// The published kg2 both reachable fits carry, kept ONLY so the test can assert the
// contrast (screen dissipation 2.75 W at this amp's idle — exactly the tube's
// rating) and so nobody "restores" it thinking the derived value is the error.
inline constexpr double kPublished6V6Kg2 = 4500.0;

// Bridge the 6V6 fit into the shared El34Params-typed evaluators.
inline El34Params to6V6(const Tube6V6Params& p) {
    El34Params e;
    e.mu = p.mu; e.ex = p.ex; e.kg1 = p.kg1; e.kp = p.kp; e.kvb = p.kvb; e.kg2 = p.kg2;
    return e;
}

class ChampPowerAmp {
public:
    enum ParamId : int {
        PARAM_DRIVE = 0,   // input drive trim (0..1) — how hard the volume signal hits
                           // the 6V6 grid. No presence, no master, no feedback: this
                           // amp has exactly one knob and it is upstream of here.
        PARAM_COUNT = 1,
    };

    ChampPowerAmp();

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);
    int oversampling() const { return os_.factor(); }
    int latencySamples() const { return os_.latencySamples(); }

    void setParameter(int paramId, float value);

    // Recovery seam (audit finding 1) — re-park every dynamic state at the
    // ALREADY-SOLVED idle point (no solveOperatingPoint()). Allocation-free.
    void reset();

    // THE 5F1 HAS NO NEGATIVE FEEDBACK. This is an inert no-op kept only so the amp
    // presents the same seam as its siblings; clipper_champ_tests asserts that a
    // render with it on and off is BIT-IDENTICAL. Do not wire a loop to it.
    void setFeedbackEnabled(bool) {}
    bool feedbackEnabled() const { return false; }

    void process(const float* in, float* out, int numFrames);

    // --- Introspection (measurement / tests) --------------------------------
    double tubeQuiescentPlateCurrent() const { return iqTube_; }   // A
    double tubeQuiescentScreenCurrent() const { return ig2qTube_; }
    double railIdle() const { return vRailIdle_; }
    double cathodeIdle() const { return vkIdle_; }
    double railNow() const { return vRail_; }
    double cathodeNow() const { return vk_; }
    double lastOutputPeak() const { return lastOutPeak_; }
    const El34Params& tube() const { return tube6V6_; }

    // Solve the plate load line at an arbitrary operating point — the test uses this
    // to find the current at which the plate reaches the knee (finding 9's bar) with
    // no need to reach inside the class.
    double plateAtCurrent(double vg1k, double vg2, double rail, double& ipOut) const;

    // Documented constants (cited in the tests / docs §73).
    static constexpr double kRload = 5000.0;         // OT reflected primary load (Ω)
    static constexpr double kOtTurns = 25.0;         // 25:1, sqrt(5000/8)
    static constexpr double kRkCathode = 470.0;      // 6V6 cathode resistor (Ω)
    static constexpr double kCkCathode = 25.0e-6;    // 6V6 cathode BYPASS cap (F)
                                                     // τ = 11.75 ms. The one
                                                     // component-level source for this
                                                     // amp OMITS this cap; the real
                                                     // 5F1 has it and search extracts
                                                     // confirm 25 µF. Unbypassed, the
                                                     // 470 Ω is strong local
                                                     // degeneration and a materially
                                                     // different amp. Docs §73.1.
    static constexpr double kRgl = 1.0e6;            // 6V6 grid leak (Ω)
    static constexpr double kCoupCc = 20.0e-9;       // V1B plate → 6V6 grid cap (F)

    static constexpr double kRrect5Y3 = 480.0;       // SOURCED: 60 V @ 125 mA
    static constexpr double kRptSecondary = 200.0;   // reconstruction (small combo)
    static constexpr double kRsupply = kRrect5Y3 + kRptSecondary;   // 680 Ω
    // DERIVED so the fixed point lands on Fender's own measured 305 V plate node.
    // It is the ONLY constant fitted to that measurement; Vk, Ip, Ik and Vpk all
    // fall out of the device card and land within 0.5–2 % (see the header table).
    static constexpr double kVsupply = 332.3333;     // B+ Thévenin no-load (V)
    static constexpr double kCreservoir = 16.0e-6;   // reservoir (F) — τ = 10.9 ms

    // OT bandwidth. RECONSTRUCTION, and deliberately the NARROWEST in the lineup:
    // this is a ~5 W single-ended transformer, so (a) the core is tiny and (b) the
    // primary carries the tube's full DC plate current, which partially magnetizes
    // the core and costs low-end headroom that a push-pull OT does not pay (the two
    // halves' DC cancels there). A Champ genuinely has almost no bottom end and that
    // is where it comes from. Compare kOtLfHz across the lineup: Twin/Mesa 40,
    // Orange/Rockerverb 45, JCM 75, AC30 80 — and this 150.
    static constexpr double kOtLfHz = 150.0;         // OT LF corner (Hz)
    static constexpr double kOtHfHz = 9000.0;        // OT HF corner (Hz)

    // Full-scale secondary volts (§42.6 probe). DERIVED from the measured cranked
    // secondary swing, not chosen: a cranked ChampAmp (VOLUME 1.0, a 0.30 V pluck
    // level at 220 Hz) swings 14.1272 V peak at the secondary, and the house
    // convention puts that at 0.9 of full scale -> 14.1272 / 0.9. Re-derive it with
    // the same probe if the device card or the supply ever moves. Docs §73.
    static constexpr double kFullScaleSecV = 15.6969;

    // Denormal scope (ADR 006) is decided BY MEASUREMENT in the .cpp, not here.
    double maxAbsRestingState() const;

private:
    inline float processSampleOS(float x);
    inline double solveTubePlate(double vg1k, double vg2, double rail,
                                 double& vpOut, double& baseOut) const;
    inline double solveTubeGrid(double vDriveAC, double& vCc, double& vgWarm) const;
    void solveOperatingPoint();

    Oversampler os_;
    Tube6V6Params card_{};
    El34Params tube6V6_{};

    double sampleRate_ = 48000.0;
    int maxBlockSize_ = 128;
    double drive_ = 1.0;

    // Idle point (solved once in prepare()).
    double iqTube_ = 0.0, ig2qTube_ = 0.0;
    double vRailIdle_ = 0.0, vkIdle_ = 0.0;

    // Dynamic state.
    double vRail_ = 0.0, vk_ = 0.0;
    double vCc_ = 0.0, vgWarm_ = 0.0, vpWarm_ = 0.0;
    double otLpS_ = 0.0, otHpS_ = 0.0;
    double lastOutPeak_ = 0.0;

    // Precomputed coefficients.
    double gRes_ = 0.0, gCk_ = 0.0, gCc_ = 0.0, gRg_ = 0.0;
    double otLpA_ = 0.0, otHpA_ = 0.0;
    double gridVgn_ = 0.0, gridRgk_ = 0.0;

    double otTurnsRatio() const { return kOtTurns; }
};

}  // namespace clipper::dsp
