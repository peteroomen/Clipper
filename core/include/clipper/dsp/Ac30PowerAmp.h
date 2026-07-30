// Clipper — portable DSP core (M10.2).
//
// Ac30PowerAmp: the Vox AC30 "top boost" CLASS-A(-ish) POWER SECTION — a 12AX7
// long-tailed-pair phase inverter (run HOT, it clips early), FOUR EL84 output
// pentodes in a CATHODE-BIASED push-pull, the post-PI TOP CUT control, the output
// transformer, NO negative feedback, and a DEEP tube-rectifier (GZ34) supply SAG.
// This is where the AC30's chime, bloom and class-A compression live. Modelled from
// circuit physics and MEASURED (docs §23), same discipline as the M9.3 Marshall and
// M10.1 Twin power sections. Convention: real circuit VOLTS internally; process()
// output normalized so 1.0f == full scale (kFullScaleSecV). Platform-free C++17.
//
// This milestone's NEW MACHINERY (the reason M10.2 exists): (a) an EL84 Koren
// pentode fit, (b) CATHODE bias with real DYNAMICS, (c) NO feedback loop at all,
// (d) tube-rectifier sag deeper than the JCM/Twin.
//
// ===========================================================================
// 1. PHASE INVERTER — 12AX7 long-tailed pair (reuses LtpInverter)
// ===========================================================================
// The top-boost PI is a 12AX7 long-tailed pair — REUSE the M9.3 LtpInverter with
// the M9.1 Koren 12AX7 device law (no new triode fit). Unlike the Twin's closely
// balanced PI (100k/142k, ratio 0.946, which cancels even harmonics for a clean
// stage), the AC30 PI is a simpler, HOTTER design: a lower B+ node and a mildly
// ASYMMETRIC plate pair (100k / 110k) so it (i) runs out of headroom EARLY — its
// own asymmetric soft clip is part of the sound — and (ii) leaves a residual
// EVEN-harmonic imbalance (ratio 0.912 against the Twin's 0.946). That residual,
// together with the cathode-bias common-mode dynamics below, is a source of the
// AC30's prominent 2nd harmonic ("chime") — the opposite design intent from the
// Twin. Documented, not vibed.
//
// The tail is 10 k to a −10 V reference (finding 7, docs §42). It used to be 2.2 k
// straight to ground, which bought the right DC point at the cost of the long-tail
// property itself (leg ratio 0.550, i.e. the "residual imbalance" above was not a
// voicing choice but a 2:1 defect). Both are now true at once.
//
// ===========================================================================
// 2. TOP CUT — the post-PI treble-cut control (INVERTED sense)
// ===========================================================================
// A pot + cap ACROSS the PI outputs shunts HF between the two anti-phase plates,
// rolling off the treble that reaches the EL84 grids. On the real amp CLOCKWISE =
// MORE CUT (a darker top) — an authentic INVERTED sense we preserve and label CUT
// in the UI. Modelled as a one-pole low-pass on each anti-phase PI plate AC drive
// whose corner LOWERS as the knob rises: kTopCutHiHz (knob 0, barely any cut) →
// kTopCutLoHz (knob 1, full cut), log-mapped. It sits AFTER the PI, BEFORE the
// EL84 coupling — a power-amp HF control, so it tames the top WITHOUT touching the
// preamp tone stack's chime the way turning the treble knob down would.
//
// ===========================================================================
// 3. EL84 CATHODE-BIASED PUSH-PULL QUAD — the CENTERPIECE
// ===========================================================================
// FOUR EL84 output pentodes (Koren pentode law — same equations as the M9.3 EL34 /
// M10.1 6L6, only the six constants differ). EL84 parameter fit (DOCUMENTED FIT — a
// widely-circulated Koren EL84 set in the modeling-community parameter tables;
// pentode fits are looser than triodes, hence the ±10 % validation band):
//   mu = 20, ex = 1.35, kg1 = 1300, kp = 42, kvb = 24, kg2 = 2400
// (kg1/kg2 trimmed from the base community fit to land the ~35 mA/tube cathode-biased
// class-A idle at this rail — pentode fits are loose, hence the ±10 % validation band).
// Modelled as a push-pull PAIR of "super-tubes" (kTubesPerSide = 2 paralleled EL84
// per side), same reflection as the Twin's 6L6 quad (per-tube reflected load Raa/2).
//
// CATHODE BIAS — the AC30 signature. There is NO fixed negative supply: the EL84
// grids sit at 0 V DC through their grid leaks, and the whole quad shares ONE
// cathode network — Rk ≈ 50 Ω ∥ Ck ≈ 50 µF to ground (canon AC30). The cathode
// voltage Vk is set by the TOTAL cathode current (plate + screen, all four tubes)
// through that network, so each tube's grid-cathode bias is Vg1k = Vg_grid − Vk,
// and at idle Vg1k ≈ −Vk (a self-bias ~−7…−10 V from the ~150 mA total quiescent
// draw). THE BIAS MOVES WITH THE SIGNAL: in class A the sum of the two anti-phase
// tube currents is an EVEN function of the drive, so it carries a DC term that
// GROWS with drive amplitude. That extra average current charges Ck → Vk RISES →
// every tube's bias COOLS under sustained drive → the gain COMPRESSES: the famous
// class-A "bloom", then squash. It recovers on the Rk·Ck time constant (τ ≈ 2.5 ms)
// when the drive falls. We model the network EXPLICITLY (a backward-Euler Rk∥Ck node
// integrated per oversampled sample from the summed cathode current) and MEASURE the
// shift: quiescent Vk vs the analytic fixed point, and the Vk rise + gain
// compression under a sustained loud passage vs the analytic RC. This dynamic bias
// node is the physical origin of the AC30's touch-sensitive compression.
//
// Class A: the tubes idle HOT (~35–45 mA/tube) so both conduct over most of the
// cycle. EL84 grid conduction charges the PI→grid coupling caps → the usual BLOCKING
// on hard overdrive (τ = Rg·Cc). Per-tube plate-load saturation via a 1-D Newton.
//
// ===========================================================================
// 4. OUTPUT TRANSFORMER — linear v1
// ===========================================================================
// Raa = 8 k plate-to-plate reflected load (the EL84 quad into the rated speaker).
// Turns ratio n = √(Raa/Rload) = √(8000/8) ≈ 31.6. Two documented corners: LF
// ~80 Hz, HF ~11 kHz. Core saturation EXPLICITLY DEFERRED — v1 is linear; the
// nonlinearity lives in the tubes. Output normalized to 1.0f == full scale.
//
// ===========================================================================
// 5. NO NEGATIVE FEEDBACK — the raw forward voicing IS the point
// ===========================================================================
// The AC30 has NO global negative feedback loop (unlike the JCM's ~3.4 dB and the
// Twin's ~3.2 dB). The forward path stands on its own — bright, immediate, and
// uncompressed-by-a-loop. kFeedbackBeta is 0. setFeedbackEnabled() is retained ONLY
// as the test seam for the ANTI-NFB assertion (the mirror of the JCM/Twin sign-
// catcher): toggling "feedback" must leave the output BIT-EXACT, because there is no
// loop to toggle. A stray feedback path anywhere would break that assert.
//
// ===========================================================================
// 6. SAG — DEEP tube-rectifier (GZ34) supply (deeper than JCM > Twin)
// ===========================================================================
// The AC30 runs a GZ34 (5AR4) valve rectifier into a HUNGRY class-A quad. A subtle
// but important physics point shaped the model: a BALANCED class-A push-pull draws a
// near-CONSTANT total B+ current (the two anti-phase tubes swap conduction, their SUM
// is flat), so its plate rail barely sags from average draw the way the JCM's fixed-
// bias class-AB stage does. The model has TWO parts:
//  (a) PHYSICAL rail + screen + cathode (processSampleOS step 6a): the plate rail
//      droops modestly from the actual cathode current through a soft-knee source
//      impedance Reff = kRsupply·(1 + kRectKnee·Icath); the screen follows it; and the
//      shared cathode cap integrates the summed cathode current (τ = Rk·Ck) — the
//      MEASURED dynamic bias shift (Vk RISES under sustained drive → bias cools → the
//      class-A bloom).
//  (b) GZ34 SAG PROPER (step 6b): the valve rectifier cannot supply the DELIVERED
//      SIGNAL current — the differential (push-pull) current into the OT primary,
//      which swells with output even though the SUM does not. We model that lost
//      headroom as a demand-envelope COMPRESSION applied to the secondary: Idemand =
//      idle draw + |differential current|, followed by a fast-attack / slow-release
//      envelope (kSagAtkHz / kSagRelHz), giving sag = 1/(1 + kSagCompGain·(Idemand −
//      Iidle)). Under a loud sustained passage the demand swells, the output squashes
//      (bloom → squash), and it recovers on the release RC. Applying the rectifier
//      sag to the delivered output rather than starving the tube DC bias keeps the
//      class-A cathode bloom (a) intact — a DOCUMENTED simplification (the rectifier's
//      peak-current limit, not a full diode+reservoir SPICE), ledgered in docs §23.
// Sag DEPTH lands in a documented 4–8 dB window — DEEPER than the JCM800's ~3.4 dB and
// MUCH deeper than the Twin's ~2.1 dB (ordering Twin < JCM < AC30, asserted).

#ifndef CLIPPER_DSP_AC30_POWER_AMP_H
#define CLIPPER_DSP_AC30_POWER_AMP_H

#include <vector>

#include "clipper/dsp/Jcm800PowerAmp.h"  // El34Params/pentode law + LtpInverter (reused)
#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/TriodeStage.h"

namespace clipper::dsp {

// EL84 Koren pentode parameters (documented fit; see header). Reuses the shared
// El34Params-shaped struct + el34PlateCurrent/el34ScreenCurrent evaluators (the
// Koren pentode form is device-agnostic — only the six constants differ).
struct TubeEl84Params {
    double mu = 20.0;
    double ex = 1.35;
    double kg1 = 1300.0;
    double kp = 42.0;
    double kvb = 24.0;
    double kg2 = 2400.0;
};

// Bridge the EL84 fit into the shared El34Params-typed evaluators.
inline El34Params toEl84(const TubeEl84Params& p) {
    El34Params e;
    e.mu = p.mu; e.ex = p.ex; e.kg1 = p.kg1; e.kp = p.kp; e.kvb = p.kvb; e.kg2 = p.kg2;
    return e;
}

class Ac30PowerAmp {
public:
    enum ParamId : int {
        PARAM_DRIVE = 0,    // PI input drive trim (0..1) — how hard the volume signal
                            // hits the (hot) phase inverter.
        PARAM_TOPCUT = 1,   // TOP CUT (0..1) — post-PI treble cut. INVERTED sense:
                            // 0 = no cut (bright), 1 = full cut (dark).
        PARAM_COUNT = 2,
    };

    Ac30PowerAmp();

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);
    int oversampling() const { return os_.factor(); }
    int latencySamples() const { return os_.latencySamples(); }

    void setParameter(int paramId, float value);

    // Recovery seam (audit finding 1) — re-park every dynamic state at the
    // ALREADY-SOLVED idle point (no bisection re-solve of the shared cathode node,
    // no LtpInverter::prepare()). Allocation-free; see Jcm800PowerAmp::reset().
    void reset();

    // Retained ONLY for the anti-NFB test seam: there is no loop, so this is inert
    // (the output is bit-exact regardless). Mirrors the JCM/Twin API shape.
    void setFeedbackEnabled(bool on) { fbEnabled_ = on; }
    bool feedbackEnabled() const { return fbEnabled_; }

    void process(const float* in, float* out, int numFrames);

    // --- Introspection (measurement / tests) --------------------------------
    double tubeQuiescentPlateCurrent() const { return iqTube_; }  // A / tube
    double tubeQuiescentScreenCurrent() const { return ig2qTube_; }
    double railIdle() const { return vRailIdle_; }
    double screenIdle() const { return vScreenIdle_; }
    double railNow() const { return vRail_; }
    double screenNow() const { return vScreen_; }
    double cathodeIdle() const { return vkIdle_; }   // quiescent shared cathode V
    double cathodeNow() const { return vk_; }        // live cathode V (bias shift)
    double lastOutputPeak() const { return lastOutPeak_; }

    // Anti-denormal diagnostic (Denormal.h, docs §33) — not used by the audio path.
    // The OT bandwidth pair are the states that rest at EXACTLY zero, so after a silent
    // tail this must read exactly 0.0. This amp has no global NFB loop, so unlike the
    // JCM800's and the Twin's its secondary genuinely settles to zero rather than to a
    // ~1e-28 loop residual — which is why THESE two, and not the ones nearer the signal,
    // were the whole of the AC30's measured silent-tail cliff (isolated stage 1.35x ->
    // 0.99x; 1588 -> 2 subnormal output samples; bisected in Ac30PowerAmp.cpp).
    //
    // DELIBERATELY EXCLUDED, measured rather than assumed: topCutS1_/topCutS2_ idle at
    // 2.84e-14 (one ULP of the LTP plate voltage — the Newton fixed point does not land
    // exactly on quiescentPlate1()), so they never go subnormal and their flush is a
    // guard-rail. Likewise iSagEnv_ (relaxes onto the class-A idle draw),
    // vRail_/vScreen_/vk_ (rail, screen and shared-cathode nodes at their DC point) and
    // the coupling / warm-start states — see Ac30PowerAmp.cpp for each.
    double maxAbsRestingState() const { return maxAbsState(otHpS_, otLpS_); }
    const El34Params& tube() const { return tubeEl84_; }
    const LtpInverter& inverter() const { return ltp_; }

    // Documented constants (cited in the tests / docs §23).
    static constexpr double kRaa = 8000.0;           // OT plate-to-plate load (Ω)
    static constexpr int kTubesPerSide = 2;          // 4 tubes: 2 paralleled per phase
    static constexpr double kRppReflected = kRaa / 2.0;   // per-tube reflected (Ω)
    static constexpr double kRkCathode = 50.0;       // SHARED cathode resistor (Ω) — canon
    static constexpr double kCkCathode = 50.0e-6;    // SHARED cathode cap (F) — τ≈2.5 ms
    static constexpr double kVsupply = 350.0;        // B+ Thévenin no-load (V)
    // GZ34 (5AR4) valve rectifier: a HIGH base source impedance (vs the JCM's 150 Ω /
    // Twin's 80 Ω solid-state supplies) PLUS a soft knee — Reff = kRsupply·(1 +
    // kRectKnee·Idemand). The reservoir is discharged by the DEMAND-current envelope
    // Idemand (a fast-attack / slow-release follower of the instantaneous cathode
    // current, see §6): a class-A push-pull's DC average current barely rises under
    // drive, but the PEAK/RMS current the rectifier must actually deliver rises a LOT,
    // and a GZ34 cannot supply it — so the rail collapses under a loud sustained
    // passage and RECOVERS on the release RC. This is the physical, MEASURED origin of
    // the AC30's deep sag (a documented modeling choice — the rectifier's peak-current
    // limit is captured by the demand envelope rather than a full diode+reservoir SPICE).
    static constexpr double kRsupply = 150.0;        // GZ34 base source Z (Ω)
    static constexpr double kRectKnee = 2.2;         // soft-knee: Reff grows with demand (1/A)
    static constexpr double kCreservoir = 32.0e-6;   // reservoir (F)
    static constexpr double kSagAtkHz = 60.0;        // demand-envelope attack (fast, ~2.6 ms)
    static constexpr double kSagRelHz = 4.0;         // demand-envelope release (slow, ~40 ms) — the bloom
    static constexpr double kSagCompGain = 7.5;      // GZ34 output-sag depth (1/A) → 4–8 dB
    static constexpr double kRscreen = 470.0;        // screen dropping R (Ω)
    static constexpr double kCscreen = 22.0e-6;      // screen filter cap (F) — slow sag
    static constexpr double kOtLfHz = 80.0;          // OT LF corner (Hz)
    static constexpr double kOtHfHz = 11000.0;       // OT HF corner (Hz)
    static constexpr double kCoupCc = 22.0e-9;       // PI→EL84 coupling cap (F) 0.022uF
    static constexpr double kCoupRg = 1.0e6;         // EL84 grid leak (Ω) — τ=22 ms
    // NO feedback loop (the AC30 has none). Kept at 0 so the forward path stands
    // alone; the anti-NFB test asserts toggling this changes nothing.
    static constexpr double kFeedbackBeta = 0.0;
    // TOP CUT corner sweep (log map): knob 0 → kTopCutHiHz (bright, no cut), knob 1
    // → kTopCutLoHz (dark, full cut). Inverted sense per the real control.
    static constexpr double kTopCutHiHz = 8000.0;
    static constexpr double kTopCutLoHz = 850.0;
    // CUT knob TAPER skew (M10.2 "muddy-fix", docs §23). The corner is log-mapped from
    // knob^kTopCutSkew, NOT the raw knob. Rationale: the AC30 reuses the SHARED
    // 'presence' slot, which defaults to 0.5 — and with the bare log map, knob 0.5 sits
    // at a ~2.6 kHz corner, i.e. the face opened with ~half the top-cut engaged and
    // measured ~3 dB darker at 3 kHz than a real top-boost (whose CUT is usually run
    // near minimum). Skewing the knob (>1) pushes the audible cut into the UPPER half of
    // travel so the default 0.5 is a MILD cut, while the FULL range is preserved (knob 0
    // → kTopCutHiHz, knob 1 → kTopCutLoHz). An analytic re-taper of the control law, not
    // an ear-tuned fudge to the physics: the corner endpoints are unchanged. 0.5^2.3 ≈
    // 0.20, so knob 0.5 → corner ≈ 5.1 kHz (a gentle top trim, not a mid-tame).
    static constexpr double kTopCutSkew = 2.3;
    // Secondary volts that map to 1.0 full scale (calibrated, docs §23): fully
    // cranked a power sine peaks ~0.9 with headroom to 1.0 for transients. RE-DERIVED
    // in the §23 second amendment: this constant FOLLOWS the measured swing (as the
    // Twin's 24 V and the JCM's 26 V do), and the corrected PI operating point means
    // the EL84s are finally driven to their real swing — cranked (VOLUME 1.0, a 0.5 V
    // pickup) the secondary now reaches 11.8 V where the starved section reached 7.5 V,
    // so leaving 7.5 here would have pushed the normalized output 1.4 dB PAST full
    // scale. 10.0 V puts the cranked power sine back at peak 0.88 with headroom to 1.0
    // for transients. NOTE this is a NORMALIZATION, not a power rating: 30 W into 8 Ω
    // would be √(30·8) = 15.5 V RMS at the secondary, but our OT/sag model delivers a
    // smaller absolute swing and every voice is normalized to its own cranked peak, so
    // the amps stay level-comparable to each other rather than to a wattmeter.
    static constexpr double kFullScaleSecV = 10.0;

    static double otTurnsRatio() { return 31.623; }  // √(Raa/8) = √1000
    double feedbackBeta() const { return kFeedbackBeta; }  // always 0

private:
    void solveOperatingPoint();
    // Park every dynamic state at the (already-solved) idle point. Shared by
    // setOversampling() and reset() so the two can never drift apart.
    void parkState();
    inline float processSampleOS(float x);
    // Plate-load Newton with the hoisted Koren base (Ip = base·atan(Vp/kvb), exact
    // dIp/dVp); baseOut feeds the shared-E1 screen current (§25).
    inline double solveTubePlate(double vg1k, double vg2, double rail, double& vpOut,
                                 double& baseOut) const;
    // Grid-node solve; vgWarm carries the previous sample's solution (§25).
    inline double solveTubeGrid(double vpPlateAC, double vpPlateQ, double& vCc,
                                double& vgWarm) const;

    double sampleRate_ = 44100.0;
    double osRate_ = 176400.0;
    int maxBlockSize_ = 128;

    Oversampler os_;
    LtpInverter ltp_;
    TubeEl84Params tubeEl84cfg_{};
    El34Params tubeEl84_{};   // the EL84 fit routed through the pentode evaluators

    double drive_ = 1.0;
    double topCut_ = 0.0;       // 0..1
    bool fbEnabled_ = true;     // inert (no loop) — anti-NFB test seam

    double iqTube_ = 0.0, ig2qTube_ = 0.0;
    double vRailIdle_ = 320.0, vScreenIdle_ = 315.0;
    double vkIdle_ = 8.0;

    // Live state.
    double vRail_ = 320.0, vScreen_ = 315.0;
    double vk_ = 8.0;                 // SHARED cathode node (dynamic bias)
    double gRes_ = 0.0, gScr_ = 0.0, gCk_ = 0.0;
    double iSagEnv_ = 0.0;            // GZ34 demand-current envelope (drives the sag)
    double gSagAtk_ = 0.0, gSagRel_ = 0.0;  // envelope attack/release coeffs
    double iIdleTotal_ = 0.0;         // quiescent total cathode current (envelope floor)

    double vCcUp_ = 0.0, vCcDown_ = 0.0;
    // Previous-sample grid solutions (Newton warm start, §25). Idle grid DC is 0
    // (the grid leak returns to ground — cathode bias, no negative supply).
    double vgUp_ = 0.0, vgDown_ = 0.0;
    double gCc_ = 0.0, gRg_ = 0.0;
    double gridRgk_ = 1500.0, gridVgn_ = 0.5;

    double otHpA_ = 0.0, otHpS_ = 0.0;
    double otLpA_ = 0.0, otLpS_ = 0.0;
    double topCutA_ = 0.0, topCutS1_ = 0.0, topCutS2_ = 0.0;  // TOP CUT one-pole per leg

    double lastOutPeak_ = 0.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_AC30_POWER_AMP_H
