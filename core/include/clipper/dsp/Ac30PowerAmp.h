// Clipper — portable DSP core (M10.2).
//
// Ac30PowerAmp: the Vox AC30 "top boost" CLASS-A(-ish) POWER SECTION — a 12AX7
// long-tailed-pair phase inverter (run HOT, it clips early), FOUR EL84 output
// pentodes in a CATHODE-BIASED push-pull, the post-PI TOP CUT control, the output
// transformer, NO negative feedback, and a REAL dynamic supply (GZ34 source impedance
// + reservoir droop) whose compression is dominated by the shared cathode bias.
// This is where the AC30's chime, bloom and class-A compression live. Modelled from
// circuit physics and MEASURED (docs §23), same discipline as the M9.3 Marshall and
// M10.1 Twin power sections. Convention: real circuit VOLTS internally; process()
// output normalized so 1.0f == full scale (kFullScaleSecV). Platform-free C++17.
//
// This milestone's NEW MACHINERY (the reason M10.2 exists): (a) an EL84 Koren
// pentode fit, (b) CATHODE bias with real DYNAMICS, (c) NO feedback loop at all,
// (d) a dynamic supply + bias, rewritten for audit finding 4 in docs §55.
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
// cathode network — Rk = 50 Ω ∥ Ck = 250 µF to ground (canon AC30; see kCkCathode,
// corrected from 50 µF in the §55 sag slice, τ = Rk·Ck = 12.5 ms). The cathode
// voltage Vk is set by the TOTAL cathode current (plate + screen, all four tubes)
// through that network, so each tube's grid-cathode bias is Vg1k = Vg_grid − Vk,
// and at idle Vg1k ≈ −Vk (a self-bias ~−7…−10 V from the ~190 mA total quiescent
// draw). THE BIAS MOVES WITH THE SIGNAL: in class A the sum of the two anti-phase
// tube currents is an EVEN function of the drive, so it carries a DC term that
// GROWS with drive amplitude. That extra average current charges Ck → Vk RISES →
// every tube's bias COOLS under sustained drive → the gain COMPRESSES: the famous
// class-A "bloom", then squash. It recovers on τ = Rk·Ck when the drive falls. We
// model the network EXPLICITLY (a backward-Euler Rk∥Ck node integrated per
// oversampled sample from the summed cathode current) and MEASURE the shift against
// the published real-amp numbers — a real AC30's common cathode sits at 10.0 V idle
// and 12.5 V at full output (200 → 250 mA through the shared 50 Ω). This dynamic
// bias node is the physical origin of the AC30's touch-sensitive compression. It is
// the amp's DOMINANT compressor in the real circuit (bias +2.5 V against a rail that
// barely moves); in THIS model it moves the right way and only a third as far —
// measured, attributed to findings 9/10 and ledgered, see §6's "REFUTED 1" below.
//
// Class A: the tubes idle HOT (~35–45 mA/tube) so both conduct over most of the
// cycle. EL84 grid conduction charges the PI→grid coupling caps → the usual BLOCKING
// on hard overdrive (τ = Rg·Cc = 4.84 ms; kCoupRg was corrected from 1 MΩ to the
// AC30's real 220 k in §55 — at 1 MΩ the blocking was deep enough to drag the average
// draw DOWN under drive, i.e. the bias moved the wrong way). Per-tube plate-load
// saturation via a 1-D Newton.
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
// 6. SAG — a REAL dynamic supply: GZ34 source impedance, reservoir droop and the
//    shared cathode bias, replacing a memoryless saturator (docs §55)
// ===========================================================================
// REWRITTEN 2026-07-31 for audit finding 4. What was here before, and why it is gone:
//
//   RETIRED — the "GZ34 SAG PROPER" step 6b. It computed a demand envelope from
//   |ipUp − ipDown| and multiplied the SECONDARY by 1/(1 + kSagCompGain·(Idemand −
//   Iidle)). Since vSec ∝ (ipUp − ipDown), that is algebraically y = x/(1 + k|x|) —
//   a MEMORYLESS SOFT CLIPPER wearing a supply's clothes, not a supply at all. Three
//   measured consequences, all confirmed on this branch before it was removed:
//     · it applied gain reduction at DEAD-CLEAN levels — a 6.02 dB input step from
//       0.05 to 0.10 V at the PI grid came out 5.02 dB, i.e. a full dB of "sag" where
//       the EL84 grids move ±1.6 V against a −9.52 V self-bias and draw no extra
//       supply current at all;
//     · above ~0.5 V it exactly cancelled the output rise — the drive sweep went
//       0.395 → 0.434 peak for a FIFTY-fold drive increase (0.82 dB), so there was no
//       touch sensitivity, no pick attack and no bloom-then-squash left to hear;
//     · its 2.6 ms/40 ms envelope, riding a full-wave-rectified audio signal, ripples
//       at 2·f₀, which for a low E (82.41 Hz, ripple at 165 Hz) is well inside the
//       envelope's own passband and amplitude-modulates the note — so it added
//       distortion in a FREQUENCY-DEPENDENT way, worst exactly where an AC30 is played.
//       Measured, composed amp, one steady 0.25 V sine at a time, THD at 82.41 Hz as a
//       RATIO to THD at 440 Hz — before → after: VOLUME 0.70 1.77× → 1.52×, 0.80
//       1.62× → 1.36×, 0.85 1.53× → 1.33×. The low notes keep ~15 % less excess grit
//       than the high ones did.
//   It is RETIRED OUTRIGHT rather than reduced: with the supply below modelled for
//   real there is nothing left for it to stand in for, and leaving a shrunken copy
//   would be a fitted constant sitting on top of physics that already covers it.
//
//   HONESTY NOTE on that third point, because it is easy to over-claim: the owner's
//   report was "the low end is thin", and a thin low end could equally have been a
//   steady-state LEVEL tilt. It was not, and this slice does NOT fix one. The same
//   probe's fundamental levels re 440 Hz measure 82 Hz at −2.64 / −3.43 / −3.81 dB
//   before and −2.97 / −3.52 / −3.91 dB after (VOLUME 0.70 / 0.80 / 0.85) — the tilt
//   is 0.1–0.3 dB DEEPER, not shallower. What this slice gives the low end is dynamic,
//   not spectral: less low-frequency-specific distortion (above), a pick attack that
//   survives (docs §55.3's pluck table), and 6.8 dB of restored dynamic range (the
//   0.05 → 25 V drive sweep spanned 13.1 dB of output and now spans 19.9). If a
//   steady-state low-end tilt is still wanted after listening, it is the tone stack's
//   or the OT's, and it is a different slice.
//
// What replaces it is the real thing, in two coupled parts, both integrated per
// oversampled sample (backward Euler) and both feeding the tubes on the NEXT sample
// (the ms time constants dwarf the µs step — the same accurate decoupling the JCM and
// Twin use):
//
//  (a) THE SUPPLY. A Thévenin HT source kVsupply behind a CONSTANT series resistance
//      kRsupply = kRrectGz34 + kRptSecondary, charging the reservoir kCreservoir, which
//      the output stage's total cathode current discharges. Every term has a published
//      source (see the constants). The soft "rectifier knee" that used to multiply
//      kRsupply is GONE and that is a measured call, not a simplification: a GZ34's own
//      drop is SUB-linear in current (space-charge, ≈ I^⅔ — the published points are
//      17 V at 225 mA and ~12.5 V per plate at 250 mA, i.e. 75.6 Ω falling to ~50 Ω),
//      while a capacitor-input supply's shrinking conduction angle adds effective
//      resistance that RISES with load. Over the 190–260 mA window this amp actually
//      occupies the two effects very nearly cancel, so a constant Thévenin resistance
//      is the honest model and the old growing knee was a voicing knob with a physics
//      label on it.
//
//  (b) THE SHARED CATHODE — the AC30's compression mechanism, and the one the published
//      measurement pins: a real AC30's common cathode sits at 10.0 V at idle and 12.5 V
//      at full output (200 mA → 250 mA through the shared 50 Ω). That is a 25 % COLDER
//      bias arriving on a −10 V grid reference, where the same amp's B+ moves only a few
//      volts, because a GZ34 is the STIFFEST of the valve rectifiers and because cathode
//      bias self-regulates the average draw. So the honest AC30 is NOT "the saggiest
//      rail"; it is the amp whose BIAS moves. kCkCathode is corrected from 50 µF to the
//      AC30's real 250 µF (JMI original; 220 µF in later production) in the same change
//      — τ = Rk·Ck goes 2.5 ms → 12.5 ms. That single value is most of the low-end
//      story: a 2.5 ms bias time constant TRACKS the envelope of a low-E note (82 Hz,
//      12.2 ms period) and modulates it; a 12.5 ms one integrates across it and lets the
//      note stand up.
//
// ---------------------------------------------------------------------------------
// TWO CLAIMS THIS SLICE'S OWN FIRST DRAFT MADE AND THE MEASUREMENTS REFUTED. Both are
// corrected here rather than fitted away, because the alternative was inventing a gain
// term to force the published number — the failure mode this file's kFullScaleSecV
// history exists to warn about.
//
//   REFUTED 1: "the bias moves a couple of volts on a ~10 V reference; it is the
//   DOMINANT compressor". It is not, in this model. Swept to absurdity (a 400 Hz burst
//   at 1 → 64 V at the PI grid), Vk moves 9.518 → 9.628 / 9.660 / 9.710 / 9.765 /
//   9.928 / 10.222 / 10.347 V. The most the bias will move is +0.83 V, a third of the
//   real amp's +2.5 V, and the rail correspondingly moves only 309.49 → 306.86 V.
//   The shortfall is NOT in the cathode network — Rk, Ck and the integration are the
//   published circuit — it is in how much extra AVERAGE current these tubes draw when
//   driven. Attribution, from the idle point this model already had: our EL84 idles at
//   34.92 mA plate + 12.67 mA screen = 47.59 mA/tube, against a real one's ~45 + ~5.
//   The TOTAL is nearly right (190.4 mA vs the published 200) and the SPLIT is badly
//   wrong — audit finding 9's ~3× hot screen fit — so a quarter of the standing draw
//   sits in a screen term whose curvature does not grow with drive the way the plate
//   term's does, and the class-A average-current rise that charges Ck is suppressed in
//   the same proportion. That is findings 9 and 10 (the screen fits, the plate load
//   line), not this slice, and it is LEDGERED as an XFAIL rather than papered over:
//   see `finding4-ac30-bias-swing-short` in core/tests/test_ac30_amp.cpp.
//
//   REFUTED 2: "the ORDERING claim survives — the AC30 compresses hardest of the
//   three". On the shared JCM/Twin burst metric (attack envelope vs settled envelope)
//   the AC30 now measures 1.06 dB against the Twin's 1.16 and the JCM's 1.76 — it is
//   the LEAST compressing of the three, not the most. The old 4–8 dB window and the
//   Twin < JCM < AC30 ordering were measuring the retired saturator's envelope
//   follower, not a supply; the JCM's fixed-bias class-AB stage really does drop its
//   rail 39 V on this burst where the AC30's drops 1.9, and that is the physics. The
//   ordering assertion is therefore REVERSED IN THE TESTS with its derivation, and the
//   AC30's compression is asserted where it actually lives — on Vk and on the rail —
//   instead of on an output envelope that no longer has a compressor behind it.

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
    double idleTotalCathodeCurrent() const { return iIdleTotal_; }  // A, all four tubes
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
    // guard-rail. Likewise vRail_/vScreen_/vk_ (rail, screen and shared-cathode nodes,
    // all parked at their DC point — see the measured resting values in
    // Ac30PowerAmp.cpp) and the coupling / warm-start states. The retired sag
    // envelope iSagEnv_ used to be listed here too; it no longer exists (docs §55).
    double maxAbsRestingState() const { return maxAbsState(otHpS_, otLpS_); }
    const El34Params& tube() const { return tubeEl84_; }
    const LtpInverter& inverter() const { return ltp_; }

    // Documented constants (cited in the tests / docs §23).
    static constexpr double kRaa = 8000.0;           // OT plate-to-plate load (Ω)
    static constexpr int kTubesPerSide = 2;          // 4 tubes: 2 paralleled per phase
    static constexpr double kRppReflected = kRaa / 2.0;   // per-tube reflected (Ω)
    static constexpr double kRkCathode = 50.0;       // SHARED cathode resistor (Ω) — canon
    // SHARED cathode bypass. CORRECTED 2026-07-31 (docs §55, audit finding 4): the
    // AC30's cathode network is 50 Ω bypassed by 250 µF (JMI original; later production
    // 220 µF), not the 50 µF this model carried. τ = Rk·Ck therefore goes 2.5 ms →
    // 12.5 ms. This is the BLOOM's time constant, and the value is load-bearing for the
    // low end rather than cosmetic: at 2.5 ms the bias node tracks a low-E note's own
    // envelope (82.41 Hz = a 12.2 ms period) and amplitude-modulates it; at the real
    // 12.5 ms it integrates across several cycles, which is what lets a bass note keep
    // its weight while the amp still compresses on the pick attack.
    static constexpr double kCkCathode = 250.0e-6;   // SHARED cathode cap (F) — τ = 12.5 ms
    // ---- The supply (docs §55). Every term below has a published source. ----------
    // GZ34 (5AR4) effective DC source resistance, from the Philips/Mullard published
    // drop: 17 V at 225 mA DC output ⇒ 75.6 Ω. (A GZ34 is the STIFFEST of the common
    // valve rectifiers — a 5U4's drop is ~50 V where this one's is ~17 V, which is
    // precisely why a real AC30's B+ moves so little and its CATHODE moves so much.)
    static constexpr double kRrectGz34 = 75.6;       // GZ34 source Z (Ω), published drop
    // The other half of the reservoir's CHARGING path: the HT secondary winding the
    // conducting rectifier plate sees. The original AC30 power transformer's HT winding
    // measures 118 Ω end to end, and a full-wave centre-tapped rectifier conducts
    // through ONE half at a time, so the series DCR per conduction is 118/2 = 59 Ω.
    //
    // NOT THE CHOKE, and this is a correction made inside this slice against its own
    // first draft (which had 78 Ω here, the published 7 H / 260 mA / 78 Ω DCR figure).
    // Two things are wrong with that: the 7 H / 78 Ω unit is the "Brian May / Dave
    // Peterson" spec, a MOD rather than the stock part, and — decisively — in a STOCK
    // AC30 the OT centre tap is fed from the reservoir BEFORE the choke (the OT primary
    // red lead lands on the same filter cap as GZ34 pin 8); the choke feeds the SCREEN
    // and preamp node downstream. Putting the choke DCR in the plate path would have
    // been modelling somebody's modification and calling it the amp. This model's
    // screen node already hangs off the rail through kRscreen, which is where a choke
    // DCR would belong — and kRscreen is entangled with finding 9's hot screen fit, so
    // it is deliberately not touched here.
    static constexpr double kRptSecondary = 59.0;    // HT winding half, 118 Ω end-to-end
    // Total Thévenin source resistance, CONSTANT — see the header's §6(a) for why the
    // old growing "rectifier knee" was removed rather than re-fitted (the tube's own
    // drop is sub-linear, the capacitor-input conduction angle is super-linear, and
    // over this amp's 190–260 mA window they cancel).
    static constexpr double kRsupply = kRrectGz34 + kRptSecondary;   // 134.6 Ω
    // HT Thévenin no-load. DERIVED, not measured off a real amp, and the reason is a
    // house rule: a published AC30 idles with its OT centre tap at 342 V, but this
    // model's EL84 screen-current fit runs ~3× hot (audit finding 9 — 12.67 mA/tube
    // against a real 3–5 mA), so the screen node and therefore the whole draw are
    // already wrong in a way finding 9 owns. Moving kVsupply to hit 342 V here would be
    // calibrating one constant to cover another's known error — the exact failure mode
    // that put two factor-of-2 mistakes inside kFullScaleSecV. So this slice PRESERVES
    // the model's existing idle operating point exactly while the source impedance
    // becomes the derived kRsupply:
    //     kVsupply = 309.4904457 V + 0.1903616816 A × 134.6 Ω = 335.1131 V
    // where 309.4904457 V is the measured idle CT and 190.3616816 mA the measured idle
    // total cathode current, both on the pre-slice circuit. The idle rail, screen, Vk
    // and both quiescent currents are therefore unchanged to seven figures — asserted
    // in the tests — so the voicing the owner asked us to protect does not move with
    // the supply, and every difference this slice measures is DYNAMICS.
    // The residual 32.5 V gap to the published 342 V is finding 9's to close.
    static constexpr double kVsupply = 335.1131;     // B+ Thévenin no-load (V)
    // Reservoir. AC30 HT smoothing was 16 µF in the earliest JMI amps and 32 µF by the
    // common later spec; 32 µF with kRsupply gives a supply RC of 4.9 ms.
    static constexpr double kCreservoir = 32.0e-6;   // reservoir (F) — τ = 4.9 ms
    static constexpr double kRscreen = 470.0;        // screen dropping R (Ω)
    static constexpr double kCscreen = 22.0e-6;      // screen filter cap (F) — slow sag
    static constexpr double kOtLfHz = 80.0;          // OT LF corner (Hz)
    static constexpr double kOtHfHz = 11000.0;       // OT HF corner (Hz)
    static constexpr double kCoupCc = 22.0e-9;       // PI→EL84 coupling cap (F) 0.022uF
    // EL84 grid leak. CORRECTED 2026-07-31 from 1.0 MΩ (docs §55), and it is in this
    // slice because it is what was stopping the bias dynamics from being honest, not
    // because it was nearby. 220 k is the AC30's standard output-stage grid-leak value
    // (the same 220 k a Plexi uses) and is what BOTH sibling power sections already
    // carry — Jcm800PowerAmp and TwinPowerAmp are 220 k each. The stale 1 MΩ looks like
    // it was chosen to make this amp's blocking τ read "22 ms" so it matched the Twin's
    // comment (the Twin gets 22 ms from 220 k × 0.1 µF; copying the NUMBER instead of
    // the RESISTOR at the AC30's 0.022 µF needs a megohm). τ = Rg·Cc is now 4.84 ms,
    // the JCM's value. PERTURBATION-MEASURED both ways on the §55 burst sweep (400 Hz,
    // 1 → 64 V at the PI grid), so the cost of the correction is on the record rather
    // than assumed. Peak Vk reached, 220 k → 1 MΩ:
    //     grid V     1      2      4      8     16     32     64
    //     220 k   9.628  9.660  9.710  9.765  9.928 10.222 10.347
    //     1 MΩ    9.625  9.661  9.685  9.744  9.779  9.966 10.104
    // i.e. the megohm leak suppresses the class-A bias rise by roughly a third at every
    // level above 4 V, because the deeper grid blocking pushes the grids negative and
    // takes back the extra average draw that is supposed to charge Ck. It does that
    // while ADDING output-envelope compression (1.65–2.03 dB against 220 k's
    // 1.02–1.08) — so the stale value was buying "sag" from the wrong mechanism, which
    // is exactly the trade this slice exists to undo. The correct resistor is kept and
    // the lost dB is reported, not compensated.
    static constexpr double kCoupRg = 220.0e3;       // EL84 grid leak (Ω) — τ = 4.84 ms
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
    // Re-derived 2026-07-30 (docs §46, its own §42.6 convention — cranked swing only,
    // on the SAME probe the product test and the sibling amps use: VOLUME 1.0, 0.5 V
    // at 110 Hz): the completed preamp + re-staged interstage measured a cranked
    // secondary peak of 10.97 V; 10.97 / 0.9 ≈ 12.2 puts fully-cranked at the ~0.9
    // project window. (The 220 Hz hot-pickup probe then peaks ~0.63 — cranked defines
    // full scale, not the hot-pickup level, per the convention.)
    //
    // RE-DERIVED AGAIN 2026-07-31 (docs §55), on the identical §46 probe — VOLUME 1.0,
    // 0.5 V at 110 Hz through the composed Ac30Amp, whole-render peak — because
    // retiring the memoryless step-6b saturator means the power section finally
    // delivers the swing its own tubes and OT produce, instead of one a waveshaper had
    // already flattened. Measured cranked secondary: 16.858 V at 48 kHz (16.860 at
    // 44.1, 16.863 at 96 — rate-independent to 0.03 %), so 16.860 / 0.9 = 18.733 puts
    // fully cranked back in the project's ~0.9 peak window with headroom for
    // transients (measured back: peak 0.8981 / 0.8983 / 0.8984). This constant FOLLOWS
    // the measured swing; it is never a level knob in its own right (§42.6).
    //
    // Honest note recorded while re-deriving: on this slice's base (origin/main
    // 7e2a10d) that same probe measures 8.169 V, i.e. the shipped 12.2 was leaving
    // cranked at peak 0.6696 rather than the ~0.9 §46 recorded when it chose the value.
    // That pre-existing 2.8 V drift away from §46's 10.97 V is not explained by this
    // slice and was not caused by it — it is simply swept up by the re-derivation, and
    // it is written down here so a future reader does not attribute the whole
    // 12.2 → 18.733 jump to step 6b's removal.
    static constexpr double kFullScaleSecV = 18.733;

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
    double iIdleTotal_ = 0.0;         // quiescent total cathode current (introspection)

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
