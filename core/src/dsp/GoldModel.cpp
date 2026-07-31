// Clipper — the GOLD overdrive (v1.1 item 6). See GoldModel.h for the section
// overview. Circuit-INFORMED, not SPICE-accurate (the house scope); every
// approximation below is flagged.
//
// ---------------------------------------------------------------------------
// SOURCES — read this before trusting a number in this file
// ---------------------------------------------------------------------------
// Method reference: Jatin Chowdhury, "A Comparison of Virtual Analog Modelling
// Techniques for Desktop and Embedded Implementations" (arXiv:2009.02833), which
// models exactly this pedal by splitting it into sections and treating each with
// nodal analysis / Wave Digital Filters — the same author whose chowdsp_wdf we
// vendor and whose diode-pair root this file uses. HONESTY NOTE: the PDF is NOT
// reachable from this build environment (arxiv.org and ccrma.stanford.edu are both
// refused by the egress proxy — 403 on CONNECT), so this model follows the paper's
// METHOD (section-per-section, WDF for the diode root, nodal/analytic transfer
// functions for the linear sections) but its COMPONENT VALUES come from the widely
// published reverse-engineered schematic of the pedal, and every one of them is
// marked as an approximation below. Where the paper's own values differ, this file
// should be corrected against it — the topology, not the digits, is the claim.
//
// 2026-07-31 UPDATE (docs §52, then §54): the reference IS reachable after all —
// not at arxiv.org, but as the author's own implementation,
// github.com/jatinchowdhury18/KlonCentaur, which carries the section-by-section
// netlist in ChowCentaur/GainStageProcessors/*.{h,cpp} plus the DAFx-19 paper
// sources and the ElectroSmash schematic figures in Paper/. §52 took the summing
// network from it; §54 takes the three things §52 measured as still wrong — the
// diode fit + R13, the R20 || C13 summing pole, and the drive amp's C7/C8 network.
// The netlist files this file now quotes, by name:
//   ClippingStage.h  : R13 = 1 k, C9 = C10 = 1 uF, `ResVs Vbias { 47000.0 }` (R16),
//                      `CustomDiodePairT<double, ...> D23 { 15e-6, 0.02585, P1 }`
//   AmpStage.h       : R11 = 15 k, R12 = 422 k, C7 = 82 nF, C8 = 390 pF,
//                      `newR10b = (1 - gain) * 100000 + 2000`
//   SummingAmp.h     : R20 = 392 k, C13 = 820 pF, H(s) = R20 / (1 + s*C13*R20)
//   PreAmpStage.h    : C3 = 0.1 uF, C5 = 68 nF, C16 = 1 uF, R6 = 10 k, R7 = 1.5 k,
//                      Vbias2 = 15 k (the FF1 summing resistor), gang-1 = g*100 k
//   FeedForward2.h   : the FF2 clean-treble network (R15 22 k, C11 2.2 n, ...)
//
// Reference level convention (model-wide): input float 1.0f == 1.0 V. A hot
// humbucker DI peaks near 0.3 V.
//
// ---------------------------------------------------------------------------
// SECTION 1 — INPUT BUFFER (always on)
// ---------------------------------------------------------------------------
// The hardware buffers the input even when the effect is switched out ("buffered
// bypass"), which is why this pedal is famous as a line driver. Electrically it is
// a unity-gain op-amp follower fed through the input network; the only audible
// thing it does in the band is the DC-blocking corner of the input coupling cap
// into the bias/pulldown resistance:
//     f_in = 1/(2*pi*R_in*C_in) = 1/(2*pi*1.0 MOhm * 0.022 uF) ~= 7.2 Hz.
// Modelled as a one-pole high-pass at kInputHpHz. The op-amp's own bandwidth here
// is far above audio and is NOT modelled (documented omission — a unity follower
// with a 3 MHz-GBW part has a ~3 MHz corner).
//
// ---------------------------------------------------------------------------
// SECTION 2 — GAIN SECTION: the dual-ganged pot, the clean blend, the germanium
// ---------------------------------------------------------------------------
// THE ARCHITECTURE THAT MAKES THIS PEDAL WHAT IT IS. The buffered signal splits
// two ways and is re-summed:
//
//     out_sum = kSumGain * ( cleanBlend * x  +  clipBlend(g) * clip(A(g)·pre·HP(x)) )
//
//   * The dual-ganged GAIN pot works the way the real one does (re-derived
//     2026-07-31, docs §50, against the published schematic): the knob changes the
//     drive amp's GAIN, not the mix. The clean feed stays ~constant (gang 2's
//     divider — the "clean fades out" folklore is only relative to the growing
//     dirt), the dirt path's summing weight is FIXED, and the dirt arrives THROUGH
//     the clean core because the drive rises end-loaded while the clean holds.
//     At GAIN 0 the clipped half is switched fully out (a kept product contract —
//     the real unit measures 0.2-3.9 % even at min; ours is bit-exact clean).
//       cleanBlendAt(g) = 1.0                       (flat)
//       clipBlendAt(g)  = kClipBlendWeight past a short fade-in from 0
//     kClipBlendWeight is DERIVED from the summing network (R20/R16 against the
//     clean feed's own transimpedance) — 4.1702, docs §52. It was 0.65 (a fit) until
//     2026-07-31.
//   * A(g) — the drive amp, gang 1 in its ground leg:
//       A = 1 + 422k/((1-g)·100k + 17k) = 4.61x ... 25.82x (+13.3 ... +28.2 dB),
//     END-LOADED (half the dB range in the last quarter-turn). The pre-§50 law
//     (1 + g·100k/1.5k, to 67.7x linear) put the real knob-0.99 drive at the 0.35
//     default — docs §50 has the full model-vs-real table.
//     THAT LAW IS NOW THE DC LIMIT OF A REAL NETWORK, NOT A SCALAR (docs §54).
//     The drive amp is a non-inverting op-amp stage whose ground leg is
//     R10b = (1-g)·100k + 2k in SERIES with R11 = 15 k, with C7 = 82 nF ACROSS
//     R10b and C8 = 390 pF across the feedback R12 = 422 k. So its gain is
//     frequency- AND knob-dependent: C7 progressively shorts out R10b above
//     1/(2*pi*R10b*C7), which at the wide-open end (R10b = 2 k) sits at 970 Hz and
//     at the closed end (102 k) at 19 Hz. Measured on the network: at g = 1 the
//     stage delivers 25.8x at DC but 47.6x at 220 Hz and 100.5x at 1 kHz; at
//     g = 0.35, 6.15x / 6.78x / 5.12x. The model held a flat A(g) until 2026-07-31,
//     i.e. it was COLDER than the real pedal wherever the knob was up — which is
//     the third of the three defects docs §52 named. `AmpStageNetwork` below is
//     that network, bilinear-transformed, and its DC gain is A(g) BY CONSTRUCTION
//     (the leg resistance is recovered from the smoothed A, see the struct), so
//     §50's law survives as an identity rather than as a second copy.
//     Note the DC minimum is EXACTLY 4.61x and the law is monotone — unlike the TS
//     family this pedal still has an honestly clean setting, because the dirt's
//     summing weight fades to zero at GAIN 0 (the contract below).
//   * HP(x) — the drive path is high-passed BEFORE the clipper:
//       f_hp = 1/(2*pi*R*C) = 1/(2*pi*15 kOhm * 0.1 uF) ~= 106 Hz.
//     The low end therefore reaches the summing node ONLY through the clean half.
//     This is the "it doesn't get mushy" trait: the clipper never sees the bass.
//   * The op-amp. A TL07x-class part on the charge-pump rails: GBW 3 MHz, slew
//     13 V/us. Its closed-loop corner GBW/A at max gain is 3e6/67.67 ~= 44 kHz —
//     ABOVE the audio band, so (unlike the RAT's LM308, which collapses to ~500 Hz)
//     this op-amp adds no audible softening; it is present for honesty, stability
//     and antialiasing. Reuses the house op-amp model (LM308Stage.h — the class is
//     named for its first user, but it is a generic GBW+slew op-amp).
//   * The GERMANIUM clipper (WDF, chowdsp_wdf). Antiparallel 1N34A-class pair at
//     the schematic's own node: driven through R13 and loaded by the summing
//     resistor R16, with a small shunt cap — the library's canonical
//     resistive-source || resistor || cap -> diode root:
//       kR13Ohms = 1 kOhm   the series source resistance (schematic R13)
//       kDirtSumROhms = 47 kOhm  the SAME R16 whose transimpedance sets the dirt's
//                          summing weight below, here as the node's shunt load —
//                          in the real stage the diode node reaches the summing
//                          amp's virtual ground through C10 (a 3.4 Hz HP) and R16,
//                          so R16 loads the node AND carries the dirt current, and
//                          the reference implementation models it exactly that way
//                          (ClippingStage.h: `ResVs Vbias { 47000.0 }`).
//       kCp = 4.7 nF       RETAINED APPROXIMATION, not in the reference netlist.
//                          With the Thevenin source now R13 || R16 = 979 Ohm its
//                          corner is 1/(2*pi*979*4.7n) ~= 34.6 kHz — out of band,
//                          a band-limiting guard-rail ahead of the decimator. The
//                          reference's own HF limit is C8 = 390 pF in the drive amp,
//                          which THIS model now carries exactly (see the amp stage
//                          below), so keeping Cp is a small extra rolloff, not a
//                          stand-in for a missing one.
//       Germanium: Is = 15 uA, Vt = 25.85 mV, ideality n = 1.0
//                  -> knee ~ n*Vt*ln(I/Is) = 0.109 V at 1 mA.
//     WHERE THAT PAIR OF NUMBERS COMES FROM, AND WHY IT SUPERSEDES THE DATASHEET
//     ONES (docs §54): until 2026-07-31 this file carried Is = 200 nA, n = 1.3, a
//     datasheet-shaped guess for "a point-contact germanium diode", giving a
//     0.286 V knee behind Rs = 2.2 kOhm. Measured against the reference
//     implementation's clipping stage — whose diode parameters are FITTED TO A REAL
//     KLON's measured clipper, not to a datasheet — that node ran 5.7 to 8.5 dB HOT
//     across a 0.05..20 V drive sweep (2.0x to 2.7x in voltage). A fit to the actual
//     device beats a datasheet-shaped guess for THIS pedal, so the reference's pair
//     is now what ships. Note what did NOT change: §36's finding was that the
//     SILICON counterfactual had its ideality dropped to 1.0 and was therefore not
//     a 1N4148 at all; that fix stands (kSiIdeality = 1.752 below), and the
//     germanium-vs-silicon CONTRAST is still a real, asserted property — it is
//     simply re-derived against the corrected germanium (docs §54.4).
//       The ideality factor is carried through the library's nDiodes multiplier,
//       which scales Vt (Vt_eff = n*Vt) — the same arithmetic.
//     DIODE_SILICON (1N914/1N4148-class, Is = 2.52 nA, n = 1.752 — the SPICE model
//     card's own pair of numbers) is a MEASUREMENT-ONLY counterfactual so the tests
//     can show the knee difference, never a user knob. Measured contrast in this
//     network: silicon clips ~5.5-6.3 dB above germanium. It shipped at n = 1.0
//     until 2026-07-25, where the contrast measured ~0.6-1.7 dB and the A/B was
//     therefore worthless (audit finding 15; docs §36, ADR 008).
//   * THE SUMMING POLE (R20 || C13 = 392 kOhm || 820 pF = 495 Hz), docs §54.
//     The summing amp is a transimpedance stage (see the summing-network block
//     below) and C13 sits across its feedback resistor, so the whole sum is
//     one-pole low-passed at 495 Hz. That pole is the single largest reason a
//     clipped Klon reads CREAMY rather than bright: it attenuates the clipper's
//     harmonics far harder than the fundamental (-0.8 dB at 220 Hz, -7.1 dB at
//     1 kHz, -15.8 dB at 3 kHz), and the tone stage after it puts the top back.
//     WHERE IT IS APPLIED HERE, AND WHY THAT IS NOT A SHORTCUT: in the real stage
//     the pole sits on the SUM, i.e. on the clean feed too. This model has
//     idealized the COMPOSED clean path as flat (kSumGain = 2.0) since §27 — the
//     real composed clean path is R20*G_clean*|pole|, which measures 2.25 at 82 Hz
//     falling to 0.76 at 1 kHz, so "flat 2.0" was always an idealization of a
//     shaped path, and the GAIN-0 transparency contract is built on it. Applying
//     the pole to the DIRT branch only is exactly that same idealization carried
//     one stage further: algebraically it is
//         LP(clean_feed*LP^-1 + dirt)  ==  clean_feed + LP(dirt),
//     i.e. the clean feed is pre-emphasized by the pole's inverse so the composed
//     clean path stays the flat 2.0 it already was, while the DIRT path becomes
//     EXACTLY the real composed dirt transfer LP(f)*(R20/R16)*V_node(f). The dirt
//     side gets strictly MORE faithful; the clean side is unchanged, bit for bit,
//     which is what keeps GAIN 0 bit-exact (render hashes in docs §54.5). ADR 016.
//   * The charge pump. The real pedal generates a negative rail so its op-amps run
//     on ~+/-9 V instead of a single 9 V supply. Here that is a HEADROOM statement:
//     the summing node carries an explicit +/-kRailVolts clamp which, at guitar
//     levels, never engages — the germanium pair is the only clipper in the box.
//     (Tested: a 1 V input at max gain/output stays well below the rails.)
//
// ---------------------------------------------------------------------------
// SECTION 3 — TREBLE
// ---------------------------------------------------------------------------
// The post-blend treble control, modelled as a shelving TILT about a ~1 kHz pivot:
// the HF half (x - LP_pivot(x)) is scaled by +/- kToneMaxTiltDb. NORMAL SENSE:
// clockwise BRIGHTENS (contrast the RAT's FILTER and the AC30's CUT, both
// inverted). Flat at exactly 0.5. *Approximation:* the hardware's tone network is
// an active stage with a gentle interaction with the output pot; the tilt is a
// first-order stand-in with the same pivot and roughly the same range.
//
// ---------------------------------------------------------------------------
// SECTION 4 — OUTPUT STAGE
// ---------------------------------------------------------------------------
// Output buffer + OUTPUT pot (identity linear map, the house convention) + the
// output coupling cap's DC block at kOutHpHz (~8 Hz). The pot is the pedal's
// makeup gain. NOTE (docs §50 correction): at GAIN 0 the box is a clean buffer
// whose UNITY sits at OUTPUT 0.5 — kSumGain = 2.0 means OUTPUT 1 is +6.02 dB
// (measured; test_gold_model asserts it). §27 documents the same.
//
// M2 — antialiasing. ONLY section 2 (the nonlinearity) is oversampled: the clean
// half, the drive high-pass, the amp, the op-amp model, the WDF clipper and the
// summing node all run at the oversampled rate so the two halves stay sample-
// aligned (the clean/clipped sum must not smear). Sections 1, 3 and 4 are linear
// and stay at the base rate. Default 4x (measured; see docs §27).

#include "clipper/dsp/GoldModel.h"

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/LM308Stage.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/ParamGuard.h"

#include <chowdsp_wdf/chowdsp_wdf.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

namespace clipper::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586;

// --- Section 1: input buffer ---
constexpr double kInputHpHz = 7.2;  // 1/(2*pi*1M*0.022uF) — input coupling cap

// --- Section 2: the ganged gain section (re-derived 2026-07-31, docs §50) -----------
// The real dual-gang law, from the published schematic (the Chowdhury/ElectroSmash
// values — the reference §27 names): gang 1's lower half sits in the drive op-amp's
// ground leg, so A(g) = 1 + Rf / ((1-g)·pot + Rleg+Rstop) = 1 + 422k/((1-g)·100k+17k)
// = 4.61x (g=0) -> 25.82x (g=1), END-LOADED — half the dB range lives in the last
// quarter-turn. The pre-§50 law (1 + g·pot/1.5k, up to 67.7x LINEAR) delivered the
// real pedal's knob-0.99 drive at the shipped 0.35 default — the owner's "gainy at
// even 35+" was literally correct, and 19 % THD sat where the docs promised
// "mostly-clean with a little grit". The germanium itself was measured RIGHT in the
// corrected topology (the §36 vindication continues) — the law was the bug.
constexpr double kGainPotOhms = 100.0e3;   // dual-ganged GAIN pot
constexpr double kDriveRfOhms = 422.0e3;   // drive op-amp feedback (R12)
constexpr double kDriveRlegOhms = 17.0e3;  // ground-leg fixed part (15k + 2k stop)
// cleanBlend: the real gang-2 divider holds the clean feed nearly CONSTANT (the
// "clean fades out" folklore is only relative to the growing dirt) — flat, per the
// nodal analysis of the published network.
// clipBlend: the summing weight of the dirt path is FIXED in the real circuit (the
// knob changes DRIVE, not mix); a short fade-in below g ~ 0.15 keeps this model's
// documented GAIN-0 contract (clipBlend(0) = 0 -> the crossfade switches the clipped
// half fully OUT, an idealization the real unit doesn't share — it measures 0.2-3.9 %
// even at min. Kept deliberately: bit-exact-clean at zero is a product contract here).
//
// --- THE SUMMING NETWORK, DERIVED (2026-07-31, docs §52) ---------------------------
// §50 left this weight as the pedal's last FIT (0.65). It is now the schematic's
// number. Source: the published ElectroSmash gain-stage schematic, re-checked
// component-for-component against Jatin Chowdhury's KlonCentaur reference
// implementation (github.com/jatinchowdhury18/KlonCentaur, DAFx-19 "A Comparison of
// Virtual Analog Modelling Techniques" — the §27 method reference, whose netlist WAS
// reachable this time in ChowCentaur/GainStageProcessors/*.h and Paper/Figures/).
//
// The real summing stage (U2A) is a TRANSIMPEDANCE amp, not a mixer: three paths
// deliver a CURRENT into its virtual ground and one feedback resistor turns the sum
// into a voltage.
//     R20 = 392 kOhm   summing-amp feedback (SummingAmp.h; C13 = 820 pF across it)
//     R16 =  47 kOhm   the GAIN-STAGE/dirt summing resistor: the diode node reaches
//                      the virtual ground through C10 = 1 uF then R16. C10 is the
//                      only post-diode network before that resistor and it is a
//                      3.4 Hz high-pass (1/(2*pi*47k*1uF)) — i.e. NO in-band
//                      post-diode attenuation: |C10 leg| = 47.000 kOhm at 220 Hz.
//     R19 =  15 kOhm   the FF1 (clean, bass) summing resistor, fed from node B
//                      through R7 = 1.5 k with C16 = 1 uF to ground
//     FF2              the clean TREBLE feed: gang-2 wiper -> C11 2.2 nF + R15 22 k
//                      (+ R16) into the same node
// So each path's weight is its transimpedance R20 * G, where G is its
// transconductance into the virtual ground:
//     dirt : G_dirt  = 1/R16                     = 21.277 uS  -> R20/R16 = 8.3404
//     clean: G_clean = G_FF1(f,g) + G_FF2(f,g)   =  4.65-5.92 uS (nodal solve over
//            the netlist above) -> R20*G_clean = 1.82 .. 2.32 across 82 Hz..1 kHz
// The second line is the INDEPENDENT CHECK that this model's summing normalization
// was already right: kSumGain * cleanBlend = 2.0, and the schematic's clean
// transimpedance measures 1.96 at 110 Hz / 2.28 at 220 Hz / 1.82 at 1 kHz. Our flat
// 2.0 sits inside +/-0.8 dB of the real (mildly frequency-shaped) clean feed.
// The dirt weight therefore follows with no fitting left in it:
//     kClipBlendWeight = (R20/R16) / kSumGain = 8.3404 / 2.0 = 4.1702
// Expressed as a ratio to the clean feed at a single frequency it is 3.59 (82 Hz) to
// 4.88 (1 kHz), band-rms 4.45 — the 4.1702 above is the absolute form (both paths'
// transimpedances reproduced, the clean idealized flat), which is why it is written
// as resistors and not as a ratio.
//
// HONESTY, LOUDLY (docs §52, the plan file's HONESTY GATE): this is SIX AND A HALF
// TIMES the 0.65 it replaces, so it makes the pedal LOUDER and DIRTIER, which is the
// OPPOSITE of the field report that commissioned the slice. It was shipped anyway,
// unfitted, because the schematic says so and because re-fitting the mix to taste is
// exactly the failure mode this project has a rule against. The measured gap and the
// two coupled defects it exposes (the diode-node level — our Ge pair clamps ~2.1x
// higher than the reference's fitted 1N34A pair, Is = 15 uA vs our 200 nA — and the
// MISSING R20 || C13 = 495 Hz summing-amp pole, which is what makes the real unit
// creamy rather than bright) are written up in docs §52. Do NOT "fix" this constant
// back to a fit; fix the diode node.
//
// kClipBlendFadeTo: RE-EXAMINED in the same derivation and DELIBERATELY UNCHANGED.
// The real network gives it no support at all — the dirt weight is fixed at every
// knob position (the gangs are in the drive amp's ground leg and the pre-amp divider,
// never in the mix), and the real unit is never clean. The fade exists solely to hold
// clipBlend(0) = 0, the documented bit-exact-clean product contract, and 0.15 is kept
// rather than re-picked because no derivation supports any other span and changing it
// would be a taste move smuggled in beside a derivation.
constexpr double kSummingRfOhms = 392.0e3;  // R20, summing-amp feedback
constexpr double kDirtSumROhms = 47.0e3;    // R16, the dirt path's summing resistor
constexpr double kSumGain = 2.0;  // R20 * G_clean, the clean path's transimpedance
constexpr double kClipBlendWeight = kSummingRfOhms / (kDirtSumROhms * kSumGain);
constexpr double kClipBlendFadeTo = 0.15;  // linear fade-in span keeping clip(0)=0
// C13, the cap ACROSS R20 — the summing amp's own pole and, per docs §54, the
// "creamy" filter this model did not have until 2026-07-31. SummingAmp.h builds
// H(s) = R20/(1 + s*C13*R20); the corner is therefore
//     1/(2*pi*R20*C13) = 1/(2*pi*392k*820p) = 495.06 Hz.
// Applied to the DIRT branch only — see the section-2 header for the algebra that
// makes that identical to "pole on the sum, clean feed pre-emphasized", which is
// what keeps this model's flat composed clean path (and GAIN 0 bit-exact). ADR 016.
constexpr double kSummingCapF = 820.0e-12;
constexpr double kSummingPoleHz = 1.0 / (kTwoPi * kSummingRfOhms * kSummingCapF);
// The drive path's INPUT network attenuates before the diodes see anything: measured
// on the real topology ~0.20x @220 Hz / 0.65x @1 kHz (the model previously passed
// 0.90 @220 — its 106 Hz corner belonged to FF1, the always-on clean-bass path, not
// the drive branch). One pole + a scale fit to the reference rows (±3 dB, g <= .75):
constexpr double kDrivePreScale = 0.65;
constexpr double kDriveHpHz = 600.0;  // drive-branch HP (was 106.1, mis-assigned; fit: |H|·pre at 220 Hz = 0.22 vs the reference 0.20, at 1 kHz 0.56 vs 0.65)
// §54 RE-SCOPES AND RE-VALIDATES these two, and the re-scoping matters. §52 checked
// them against H_pre(f,g) * H_amp(f,g) / A(g) — the WHOLE drive path's shaping,
// normalized by the DC gain — because the model had no amp-stage network and this
// scalar pair stood in for both. `AmpStageNetwork` now carries H_amp exactly, so
// what is left for these two to represent is H_pre ALONE (the pre-amp divider
// C3 into (C5||R6 + gang-1) || (R7 + (15k||C16))). Re-measured against that, on the
// same netlist (all values in the reference's PreAmpStage.h/.cpp):
//     f        82     110     220     500    1000    3000 Hz
//   H_pre  0.1167  0.1317  0.2027  0.3921  0.6409  0.9279   (reference, g = 0.35)
//   model  0.0880  0.1172  0.2238  0.4161  0.5574  0.6374   (0.65 * HP600)
//   delta   -2.45   -1.02   +0.86   +0.52   -1.22   -3.26 dB
// So the pair STANDS unchanged: within +/-1.3 dB across the guitar core band, which
// is the same accuracy §52 recorded, now measured against the right target. The
// residual is honest and named: H_pre is a one-pole HP with a ~1105 Hz corner into a
// 0.93 shelf, and 0.65 * HP600 is a one-pole HP with a 600 Hz corner into a 0.65
// shelf, so the model rolls the top off ~3 dB early and the very bottom ~2.5 dB fast.
// Refitting (scale -> 0.93, corner -> 1105) is a candidate for a later slice; it is
// NOT done here because this slice already changes three networks and a fourth
// constant move would have no isolated perturbation proof. H_pre is also mildly
// knob-dependent below g = 0.15 (0.1735 vs 0.2027 at 220 Hz) where the dirt is muted
// by the contract fade anyway; the model's flat-in-g stand-in ignores that.
constexpr double kRailVolts = 8.6;  // charge-pump rails (+/-9 V minus dropout)

// --- The drive amp's own network (docs §54, reference AmpStage.h) ------------------
// R10b (gang-1's lower half + the 2 k end stop) sits in the op-amp's ground leg in
// series with R11; C7 shunts R10b, C8 shunts the feedback R12. R10b is recovered
// from the SMOOTHED A rather than kept as a second copy of the knob, so the DC gain
// of the discretized network is A(g) exactly (see AmpStageNetwork::setFromGain).
constexpr double kAmpR11Ohms = 15.0e3;    // R11, the fixed part of the ground leg
constexpr double kAmpC7 = 82.0e-9;        // across R10b — the gain-dependent zero
constexpr double kAmpC8 = 390.0e-12;      // across R12 — the stage's own HF rolloff
constexpr double kDriveGainMin = 1.0 + kDriveRfOhms / (kGainPotOhms + kDriveRlegOhms);
constexpr double kDriveGainMax = 1.0 + kDriveRfOhms / kDriveRlegOhms;

// The op-amp: TL07x-class on the charge-pump rails.
constexpr double kOpAmpGbwHz = 3.0e6;
constexpr double kOpAmpSlewVoltsPerSec = 13.0e6;

// WDF clipping network (docs §54: R13 + the R16 node load + the reference's fit).
constexpr double kR13Ohms = 1.0e3;  // R13 — series source resistance (was 2.2 k)
constexpr double kCp = 4.7e-9;      // shunt cap; Thevenin 979 Ohm -> corner ~34.6 kHz
// Germanium (1N34A-class) vs the silicon counterfactual (1N914-class).
// The germanium pair is the reference implementation's FIT TO A REAL UNIT's clipper
// (ClippingStage.h: `{ 15e-6, 0.02585 }`, a plain antiparallel pair, n = 1), which
// supersedes this file's datasheet-shaped 200 nA / n = 1.3 guess for THIS pedal —
// that guess measured 5.7-8.5 dB hot at the node across a 0.05..20 V drive sweep.
constexpr double kGeIs = 15.0e-6;
constexpr double kGeIdeality = 1.0;
constexpr double kSiIs = 2.52e-9;
// 1N4148/1N914 SPICE: `IS=2.52n N=1.752`. Was 1.0 until 2026-07-25 — the same
// dropped-ideality error as RatModel (audit finding 15, docs §36, ADR 008). With
// n = 1.0 the silicon "counterfactual" clipped only 0.60-1.70 dB above the
// germanium pair in this same network, so the A/B that exists to show what the
// germanium buys was showing almost nothing; a real 1N34A-vs-1N4148 comparison is
// ~6 dB. MEASURED, settled, in this tree (Rs = 2.2 k, Cp = 4.7 nF), Si over Ge:
// +6.33 dB at 1 V of drive, +5.99 dB at 10 V, +5.47 dB at 100 V.
// The GERMANIUM side was and is correct (n = 1.3, knee 0.286 V at 1 mA) — only the
// silicon reference was wrong, so the pedal's own voice is unchanged by this fix.
constexpr double kSiIdeality = 1.752;
constexpr double kVt = 25.85e-3;

// --- Section 3: treble ---
constexpr double kTonePivotHz = 1000.0;
constexpr float kToneMaxTiltDb = 12.0f;

// --- Section 4: output ---
constexpr double kOutHpHz = 8.0;

// --- Smoothing (the house ~5 ms glide) ---
constexpr double kSmoothSeconds = 0.005;

// NaN-rejecting knob clamp (ParamGuard.h) — audit finding 1.
float clamp01(float v) { return clampParam01(v); }

float onePoleCoeff(double cutoffHz, double sampleRate) {
    const double a = 1.0 - std::exp(-kTwoPi * cutoffHz / sampleRate);
    return static_cast<float>(std::clamp(a, 0.0, 1.0));
}

// --- The drive amp as a network, not a scalar (docs §54, reference AmpStage.h) -----
// Non-inverting op-amp stage: ground leg = (R10b || C7) + R11, feedback = R12 || C8.
//     H(s) = (b0 s^2 + b1 s + b2) / (a0 s^2 + a1 s + a2)
//     a0 = C7*C8*R10b*R11*R12          b0 = a0
//     a1 = C7*R10b*R11 + C8*R12*(R10b+R11)   b1 = C7*R11*R12 + a1
//     a2 = R10b + R11                  b2 = R12 + a2
// so H(0) = b2/a2 = 1 + R12/(R10b+R11) — §50's gang law, exactly. Discretized with a
// plain bilinear transform (K = 2 fs, no warping): every pole of this network sits
// between 148 Hz and 1.10 kHz across the whole knob travel, and it runs at the
// OVERSAMPLED rate (176.4-384 kHz shipped), so the warping error is far below the
// component tolerances the netlist itself carries.
struct AmpStageNetwork {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    double legOhms = kGainPotOhms + kDriveRlegOhms;  // R10b + R11 (cached)

    void clearState() { x1 = x2 = y1 = y2 = 0.0f; }

    // Recover the ground leg from the SMOOTHED drive gain, so the network's DC gain
    // IS the smoothed A(g) and §50's law is never written down twice:
    //     A = 1 + R12/(R10b + R11)  =>  R10b + R11 = R12/(A - 1).
    // The clamp covers exactly one edge case: a GoldModel whose PARAM_GAIN was never
    // set leaves the smoother at 0, and A = 0 would give a negative leg. The dirt path
    // is multiplied by clipBlend, which is also 0 there, so nothing audible depends on
    // the clamped value — it exists so the coefficients stay finite.
    void setFromGain(double A, double sampleRate) {
        const double a = std::clamp(A, kDriveGainMin, kDriveGainMax);
        const double leg = kDriveRfOhms / (a - 1.0);  // R10b + R11, in [17k, 117k]
        if (leg == legOhms && a2 != 0.0) return;      // idle: no coefficient rebuild
        legOhms = leg;
        const double r10b = std::max(leg - kAmpR11Ohms, 1.0);
        const double r11 = kAmpR11Ohms, r12 = kDriveRfOhms;
        const double A0 = kAmpC7 * kAmpC8 * r10b * r11 * r12;
        const double A1 = kAmpC7 * r10b * r11 + kAmpC8 * r12 * (r10b + r11);
        const double A2 = r10b + r11;
        const double B0 = A0;
        const double B1 = kAmpC7 * r11 * r12 + A1;
        const double B2 = r12 + A2;
        const double K = 2.0 * (sampleRate > 0.0 ? sampleRate : 44100.0), K2 = K * K;
        const double d0 = A0 * K2 + A1 * K + A2;
        b0 = (B0 * K2 + B1 * K + B2) / d0;
        b1 = (-2.0 * B0 * K2 + 2.0 * B2) / d0;
        b2 = (B0 * K2 - B1 * K + B2) / d0;
        a1 = (-2.0 * A0 * K2 + 2.0 * A2) / d0;
        a2 = (A0 * K2 - A1 * K + A2) / d0;
    }

    // Direct form I. Anti-denormal (Denormal.h): y1/y2 are recursive states whose
    // rest value IS zero (this is a pure signal path with no bias), so both are
    // flushed — WASM has no FTZ. x1/x2 are input history, assigned not fed back.
    inline float process(float x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = flushDenormal(static_cast<float>(y));
        return y1;
    }
};

// TREBLE knob -> linear tilt gain applied to the HF half (1.0 == flat at 0.5).
float toneKnobToTilt(float knob) {
    const float db = (clamp01(knob) - 0.5f) * 2.0f * kToneMaxTiltDb;
    return std::pow(10.0f, db / 20.0f);
}
}  // namespace

// The three ganged maps. All three clamp through ParamGuard (NaN -> 0), so even a
// direct call from a test or the plugin cannot hand a NaN gain to the smoothers.
double GoldModel::driveGainAt(double knob) {
    const double g = clampParam01(knob);
    return 1.0 + kDriveRfOhms / ((1.0 - g) * kGainPotOhms + kDriveRlegOhms);
}
double GoldModel::cleanBlendAt(double knob) {
    (void)clampParam01(knob);  // NaN-reject for API parity; the clean feed is flat
    return 1.0;
}
double GoldModel::clipBlendAt(double knob) {
    const double g = clampParam01(knob);
    // Fixed dirt weight with the short fade-in that preserves clipBlend(0) = 0.
    return g >= kClipBlendFadeTo ? kClipBlendWeight
                                 : kClipBlendWeight * (g / kClipBlendFadeTo);
}

struct GoldModel::Impl {
    double sampleRate = 44100.0;
    int maxBlockSize = 128;
    int osFactor = 4;
    int diodeType = GoldModel::DIODE_GERMANIUM;
    bool cleanBlend = true;
    bool idealOpAmp = false;

    // Smoothed physical params (mapped, not raw knob positions).
    OnePoleSmoother driveGain;   // A(g), the drive amp's voltage gain
    OnePoleSmoother cleanMix;    // cleanBlend(g)
    OnePoleSmoother clipMix;     // clipBlend(g)
    OnePoleSmoother toneTilt;    // treble tilt (linear gain on the HF half)
    OnePoleSmoother outLevel;    // OUTPUT pot

    // Section 1 (base rate): input-buffer high-pass state.
    float inHpCoef = 0.0f;
    float inHpState = 0.0f;

    // Section 2 (oversampled): drive-path high-pass state.
    float driveHpCoef = 0.0f;
    float driveHpState = 0.0f;
    // Section 2 (oversampled): the drive amp's R10b/R11/R12/C7/C8 network (§54).
    AmpStageNetwork ampStage;
    // Section 2 (oversampled): the summing amp's R20 || C13 = 495 Hz pole (§54),
    // applied to the dirt branch — see the section-2 header for why that is the
    // faithful placement given this model's idealized-flat composed clean path.
    float sumPoleCoef = 0.0f;
    float sumPoleState = 0.0f;

    // Section 3/4 (base rate): tone pivot low-pass + output DC block.
    float toneCoef = 0.0f;
    float toneLpState = 0.0f;
    double dcR = 0.0;
    float dcX1 = 0.0f, dcY1 = 0.0f;

    Oversampler os;
    LM308Stage opAmp;  // generic GBW + slew op-amp model (see LM308Stage.h)

    // The WDF germanium clipper. Declaration order matters: children first, root
    // last. Double precision, as the RAT's tree.
    //
    // §54: the node is now the schematic's — driven through R13 = 1 k and LOADED by
    // the summing resistor R16 = 47 k (the reference's ClippingStage.h models R16 as
    // its `ResVs Vbias { 47000.0 }` and takes the dirt as the current through it; the
    // 47 k here and the R20/R16 summing weight above are the same resistor). Rsum is
    // a plain resistor rather than R16-in-series-with-C10 because C10 is a 3.4 Hz
    // high-pass — out of band — and the pair is symmetric, so there is no DC to block.
    chowdsp::wdft::ResistiveVoltageSourceT<double> Vs { kR13Ohms };
    chowdsp::wdft::ResistorT<double> Rsum { kDirtSumROhms };
    chowdsp::wdft::CapacitorT<double> Cp { kCp, 48000.0 };
    chowdsp::wdft::WDFParallelT<double, decltype(Vs), decltype(Rsum)> P0 { Vs, Rsum };
    chowdsp::wdft::WDFParallelT<double, decltype(P0), decltype(Cp)> P1 { P0, Cp };
    chowdsp::wdft::DiodePairT<double, decltype(P1)> diodes {
        P1, kGeIs, kVt, kGeIdeality };

    void applyDiodes() {
        if (diodeType == GoldModel::DIODE_SILICON)
            diodes.setDiodeParameters(kSiIs, kVt, kSiIdeality);
        else
            diodes.setDiodeParameters(kGeIs, kVt, kGeIdeality);
    }

    void reprepareGainSection() {
        os.setFactor(osFactor);
        const double osRate = sampleRate * os.factor();
        Cp.prepare(osRate);  // the WDF cap runs at the OVERSAMPLED rate
        Vs.setVoltage(0.0);
        applyDiodes();
        driveHpCoef = onePoleCoeff(kDriveHpHz, osRate);
        driveHpState = 0.0f;
        sumPoleCoef = onePoleCoeff(kSummingPoleHz, osRate);
        sumPoleState = 0.0f;
        ampStage.clearState();
        // Force a coefficient rebuild at the new rate (a2 == 0 is the "unbuilt" flag).
        ampStage.a2 = 0.0;
        ampStage.setFromGain(driveGain.value(), osRate);
        opAmp.prepare(osRate, kOpAmpGbwHz, kOpAmpSlewVoltsPerSec);
    }
};

GoldModel::GoldModel() : impl_(std::make_unique<Impl>()) {}
GoldModel::~GoldModel() = default;

void GoldModel::prepare(double sampleRate, int maxBlockSize) {
    Impl& d = *impl_;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    d.maxBlockSize = maxBlockSize > 0 ? maxBlockSize : 128;

    d.driveGain.prepare(kSmoothSeconds, d.sampleRate);
    d.cleanMix.prepare(kSmoothSeconds, d.sampleRate);
    d.clipMix.prepare(kSmoothSeconds, d.sampleRate);
    d.toneTilt.prepare(kSmoothSeconds, d.sampleRate);
    d.outLevel.prepare(kSmoothSeconds, d.sampleRate);

    d.inHpCoef = onePoleCoeff(kInputHpHz, d.sampleRate);
    d.inHpState = 0.0f;
    d.toneCoef = onePoleCoeff(kTonePivotHz, d.sampleRate);
    d.toneLpState = 0.0f;
    d.dcR = std::exp(-kTwoPi * kOutHpHz / d.sampleRate);
    d.dcX1 = 0.0f;
    d.dcY1 = 0.0f;

    d.os.prepare(d.maxBlockSize);
    d.reprepareGainSection();
}

void GoldModel::setOversampling(int factor) {
    impl_->osFactor = factor;
    impl_->reprepareGainSection();
}

void GoldModel::reset() {
    Impl& d = *impl_;
    // Smoothers first: a poisoned smoother value never recovers on its own.
    d.driveGain.reset();
    d.cleanMix.reset();
    d.clipMix.reset();
    d.toneTilt.reset();
    d.outLevel.reset();
    d.inHpState = 0.0f;
    d.toneLpState = 0.0f;
    d.dcX1 = 0.0f;
    d.dcY1 = 0.0f;
    d.opAmp.reset();
    d.os.reset();
    // Re-derives the oversampled-section coefficients at the CURRENT rate/factor and
    // resets driveHpState / the WDF cap / the source. Allocation-free.
    d.reprepareGainSection();
}
int GoldModel::oversampling() const { return impl_->os.factor(); }
int GoldModel::latencySamples() const { return impl_->os.latencySamples(); }

void GoldModel::setDiodeType(int type) {
    impl_->diodeType = (type == DIODE_SILICON) ? DIODE_SILICON : DIODE_GERMANIUM;
    impl_->applyDiodes();
}
int GoldModel::diodeType() const { return impl_->diodeType; }

void GoldModel::setCleanBlendEnabled(bool enabled) { impl_->cleanBlend = enabled; }
bool GoldModel::cleanBlendEnabled() const { return impl_->cleanBlend; }

void GoldModel::setIdealOpAmp(bool ideal) {
    impl_->idealOpAmp = ideal;
    impl_->opAmp.reset();
}
bool GoldModel::idealOpAmp() const { return impl_->idealOpAmp; }

void GoldModel::setParameter(int paramId, float value) {
    Impl& d = *impl_;
    const float knob = clamp01(value);
    switch (paramId) {
        case PARAM_GAIN:
            // ONE knob, THREE mapped quantities — the dual-ganged pot.
            d.driveGain.setTarget(static_cast<float>(driveGainAt(knob)));
            d.cleanMix.setTarget(static_cast<float>(cleanBlendAt(knob)));
            d.clipMix.setTarget(static_cast<float>(clipBlendAt(knob)));
            break;
        case PARAM_TREBLE:
            d.toneTilt.setTarget(toneKnobToTilt(knob));
            break;
        case PARAM_OUTPUT:
            d.outLevel.setTarget(knob);  // identity linear map, as the RAT/TS
            break;
        default:
            break;
    }
}

void GoldModel::process(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(d.maxBlockSize, numFrames - off);
        processChunk(in + off, out + off, n);
        off += n;
    }
}

void GoldModel::processChunk(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;
    assert(numFrames <= d.maxBlockSize && "chunk exceeds maxBlockSize");

    // --- Section 1 (base rate, linear): the always-on input buffer. ----------
    // One-pole high-pass = x - LP(x). Written into `out` so in/out may alias;
    // os.upsample() copies it away immediately below.
    const float ihc = d.inHpCoef;
    for (int i = 0; i < numFrames; ++i) {
        const float x = in[i];
        d.inHpState = flushDenormal(d.inHpState + ihc * (x - d.inHpState));
        out[i] = x - d.inHpState;
    }

    // Advance the gain-section smoothers across the chunk and use the chunk value
    // for the oversampled loop (control rate — the same discipline as the RAT's
    // op-amp corner and the OverdriveEngine's K; the 5 ms glide keeps it click-free).
    float A = d.driveGain.value(), cleanW = d.cleanMix.value(), clipW = d.clipMix.value();
    for (int i = 0; i < numFrames; ++i) {
        A = d.driveGain.next();
        cleanW = d.cleanMix.next();
        clipW = d.clipMix.next();
    }
    if (!d.cleanBlend) cleanW = 0.0f;  // measurement counterfactual
    if (!d.idealOpAmp) d.opAmp.setNoiseGain(A);
    // The drive amp's coefficients follow the same control-rate discipline as A did
    // before it (one rebuild per chunk, ~0.7-2.9 ms; the 5 ms glide is what keeps a
    // knob move click-free). setFromGain() no-ops when the leg has not moved, so a
    // parked knob costs one comparison per chunk.
    d.ampStage.setFromGain(A, d.sampleRate * d.os.factor());

    // --- Section 2 (oversampled, nonlinear): the ganged gain section. --------
    // Both halves live at the oversampled rate so the clean signal and the clipped
    // signal stay sample-aligned when they meet at the summing node.
    d.os.upsample(out, numFrames);
    float* w = d.os.buffer();
    const int osN = d.os.bufferLength();
    const float hc = d.driveHpCoef;
    for (int i = 0; i < osN; ++i) {
        const float x = w[i];
        // Drive-path input network (§50): the real branch ATTENUATES before the
        // diodes see anything — kDrivePreScale + the (re-assigned) HP corner.
        d.driveHpState = flushDenormal(d.driveHpState + hc * (x - d.driveHpState));
        // §54: the drive amp is the R10b/R11/R12/C7/C8 network, not the scalar A —
        // its DC gain IS A (identity, see AmpStageNetwork::setFromGain), and above
        // the C7 corner it rises well past it (47.6x at 220 Hz / 100.5x at 1 kHz at
        // g = 1, against the 25.8x DC law).
        float u = static_cast<float>(kDrivePreScale) * (x - d.driveHpState);
        u = d.ampStage.process(u);
        if (!d.idealOpAmp) u = d.opAmp.processSample(u);  // GBW + slew
        // Germanium diode pair (WDF root); output is the clipping-node voltage.
        d.Vs.setVoltage(static_cast<double>(u));
        d.diodes.incident(d.P1.reflected());
        d.P1.incident(d.diodes.reflected());
        const float clipped = static_cast<float>(chowdsp::wdft::voltage<double>(d.Cp));
        // Anti-denormal (Denormal.h): the WDF shunt cap's wave state is the network's
        // recursive memory and rings down into DOUBLE subnormals on silence. Flushed
        // AFTER the voltage above is read, so this sample is bit-identical to the
        // unguarded network. Audit finding 11, docs §33.
        flushDenormalWdfCapacitor(d.Cp);
        // §54: the summing amp's own pole, R20 || C13 = 495 Hz — the "creamy" filter.
        // On the dirt branch only: this model's composed CLEAN path is idealized flat
        // (kSumGain = 2.0) and always has been, so pre-emphasizing the clean feed by
        // the pole's inverse — which is what applying the pole to the dirt alone
        // amounts to — leaves the clean path bit-identical (the GAIN-0 transparency
        // contract) while making the dirt path EXACTLY the real composed transfer
        // LP(f)*(R20/R16)*V_node(f). Section-2 header + ADR 016 carry the algebra.
        // Anti-denormal: this state rests at zero (silence -> zero dirt), and WASM has
        // no FTZ. Runs at the OVERSAMPLED rate, which also band-limits the clipper's
        // products before the decimator sees them.
        d.sumPoleState = flushDenormal(d.sumPoleState + d.sumPoleCoef * (clipped - d.sumPoleState));
        const float dirt = d.sumPoleState;
        // The summing amp: the ganged crossfade, then the charge-pump rails (which
        // at guitar levels never engage — the diodes are the only clipper).
        float s = static_cast<float>(kSumGain) * (cleanW * x + clipW * dirt);
        if (s > static_cast<float>(kRailVolts)) s = static_cast<float>(kRailVolts);
        else if (s < -static_cast<float>(kRailVolts)) s = -static_cast<float>(kRailVolts);
        w[i] = s;
    }
    d.os.downsample(out, numFrames);

    // --- Sections 3+4 (base rate, linear): treble tilt -> OUTPUT -> DC block. -
    for (int i = 0; i < numFrames; ++i) {
        const float v = out[i];
        d.toneLpState = flushDenormal(d.toneLpState + d.toneCoef * (v - d.toneLpState));
        float toned = d.toneLpState + d.toneTilt.next() * (v - d.toneLpState);
        // The tone stage is an active stage on the SAME charge-pump rails, so it
        // carries the same clamp. Like the summing node's, it never engages at any
        // real playing level (a 1 V input wide open peaks ~1.6 V) — it exists so a
        // pathological input cannot leave the supply behind.
        if (toned > static_cast<float>(kRailVolts)) toned = static_cast<float>(kRailVolts);
        else if (toned < -static_cast<float>(kRailVolts)) toned = -static_cast<float>(kRailVolts);
        const float lvl = toned * d.outLevel.next();
        // Output coupling cap (one-pole DC blocker).
        // Anti-denormal (Denormal.h): on silence this degenerates to y = dcR*dcY1
        // with dcR ~= 0.9984, which in the subnormal range rounds back to itself and
        // NEVER reaches zero. This one state was GOLD's whole subnormal problem:
        // measured 393607 of 480000 output samples subnormal over 10 s of silence,
        // shoved straight into whatever follows GOLD in the chain. Audit finding 11,
        // docs §33. dcX1 is an input history (assigned, never fed back) — no guard.
        const float y = lvl - d.dcX1 + static_cast<float>(d.dcR) * d.dcY1;
        d.dcX1 = lvl;
        d.dcY1 = flushDenormal(y);
        out[i] = y;
    }
}

}  // namespace clipper::dsp
