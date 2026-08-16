// Clipper — portable DSP core.
//
// OutputPotTaper — the five dirt pedals' OUTPUT pots, each with the law its own
// netlist says it has (docs §67).
//
// ---------------------------------------------------------------------------
// WHY THIS FILE EXISTS
// ---------------------------------------------------------------------------
// Every dirt pedal in the lineup used to map its output knob straight to a gain
// target: `level.setTarget(knob)`, an identity linear map, with a comment in each
// file admitting it. docs §66.2 measured the consequence across all five and
// §66.3 registered it as `rat-level-pot-linear-not-log`, deliberately NOT fixed
// there because fixing ONE pedal re-stages the lineup against four still on the
// wrong law. This file is that slice: five pots, one change.
//
// ---------------------------------------------------------------------------
// THE ONE THING NOT TO DO HERE
// ---------------------------------------------------------------------------
// Do NOT collapse these five into one call to a house `audioTaper(k = 4)`. Two
// of the five are NOT a bare audio taper and the netlists say so:
//
//   * the TS/SD-1 pot's WIPER IS LOADED by the output buffer, and a loaded pot
//     does not deliver its bare taper (11.39 % at half rotation, not 11.92);
//   * the GOLD's pot is a LINEAR (B) taper inside a resistive network, and
//     giving it an audio taper would be inventing a letter the sources deny.
//
// docs §63.14 measured what happens when a taper is chosen to fix a level
// complaint instead of derived from the circuit: it cannot, and the defaults
// were the real answer. Every constant below has a source line next to it.
//
// ---------------------------------------------------------------------------
// THE NETWORK (one form, four instances)
// ---------------------------------------------------------------------------
//
//        signal ──[ Rs ]──┬──[ (1-w)·Rp ]──┬── wiper ──> out
//                         │                │
//                       (top)            [ w·Rp ]   [ RL ]
//                                          │           │
//                                        AC gnd     AC gnd
//
// `w` is the WIPER FRACTION (0 at the ground end, 1 at the signal end) — the
// pot's own taper law maps knob -> w. `Rs` is whatever series resistance the
// driving stage presents AHEAD of the pot; `RL` is whatever loads the WIPER.
//
// The two behave completely differently and conflating them is the trap:
//   * `Rs` scales EVERY wiper position by the same constant Rp/(Rs+Rp), so it
//     is LAW-NEUTRAL — it changes the level, never the shape.
//   * `RL` shunts the BOTTOM leg only, so it bends the law, most where the
//     bottom leg is largest (the top of the travel).
//
// ---------------------------------------------------------------------------
// NORMALISATION — deliberate, and it is a SCOPE boundary not a fudge
// ---------------------------------------------------------------------------
// Every law here is normalised so that `law(1) == 1` exactly. Where a network
// has a fixed insertion loss at full rotation (the Muff's 10 k source into a
// 250 k pot = -0.34 dB; the GOLD's 560 Ω R25 in series = -0.52 dB) that loss is
// a LEVEL fact, not a LAW fact, and this slice moves output-pot LAWS only.
// Shipping the un-normalised network would smuggle an absolute level trim in
// under a knob-law change — which is exactly what §66 refused to do and what
// §51 pinned by construction ("`taper(1) = 1` for ANY k pins the top of the
// knob EXACTLY"). The losses are MEASURED and RECORDED in docs §67.3 as their
// own follow-up; they are not lost, they are out of scope.
//
// Consequence, asserted by every pedal's test: **OUTPUT 1.0 is bit-identical to
// what it rendered before this slice.**
//
// Platform-free (C++17, no OS/browser/Emscripten). Header-only on purpose: a new
// .cpp has to be added to `scripts/build-wasm.sh`'s source list inside the
// STAMP:EMCC-ARGS markers (docs §60/§64 both record forgetting it), and there is
// nothing here that needs a translation unit.

#ifndef CLIPPER_DSP_OUTPUT_POT_TAPER_H
#define CLIPPER_DSP_OUTPUT_POT_TAPER_H

#include "clipper/dsp/ParamGuard.h"

#include <cmath>

namespace clipper::dsp::pot {

// ---------------------------------------------------------------------------
// The wiper laws (knob -> wiper fraction)
// ---------------------------------------------------------------------------

// The house audio-taper wiper law, shared with the amps' volume pots
// (Jcm800Preamp::audioTaper, TwinPreamp::audioTaper, Ac30Preamp::audioTaper all
// use this form). k = 4 puts the wiper at
//
//     (e^{k/2} - 1)/(e^k - 1) == 1/(e^{k/2} + 1) == 11.920 %
//
// at half rotation, inside the 10-20 % band §58 cites for an audio-taper pot.
//
// NOTE a correction to docs §66.3, which quotes this as "12.15 %": the closed
// form above is exact and gives 11.920 %. Nothing was calibrated against the
// 12.15 figure — it appears only in prose — but the tests here assert the real
// number so it cannot rot again.
inline constexpr double kAudioTaperK = 4.0;

inline double audioTaperWiper(double knob) {
    const double x = clampParam01(knob);  // NaN-rejecting (ParamGuard.h)
    return (std::exp(kAudioTaperK * x) - 1.0) / (std::exp(kAudioTaperK) - 1.0);
}

// A linear (B) taper: the wiper IS the knob.
inline double linearWiper(double knob) { return clampParam01(knob); }

// ---------------------------------------------------------------------------
// The network (wiper fraction -> delivered gain)
// ---------------------------------------------------------------------------

// V_wiper / V_signal for the divider drawn above. Pass `rLoadOhms <= 0` for an
// unloaded wiper (the pot feeds an output jack and nothing else).
inline double dividerGain(double w, double rSeriesOhms, double rPotOhms,
                          double rLoadOhms) {
    const double rBottom = w * rPotOhms;
    const double rTop = (1.0 - w) * rPotOhms;
    // The bottom leg in parallel with whatever loads the wiper.
    const double rBottomLoaded =
        (rLoadOhms > 0.0) ? (rBottom * rLoadOhms) / (rBottom + rLoadOhms) : rBottom;
    const double rChain = rSeriesOhms + rTop + rBottomLoaded;
    return (rChain > 0.0) ? (rBottomLoaded / rChain) : 0.0;
}

// The same, normalised so the top of the knob is exactly unity (see the
// NORMALISATION note above). `wiperAtFullRotation` is 1.0 for every pot here —
// it is a parameter only so the identity is visible rather than assumed.
inline double normalisedDividerGain(double w, double rSeriesOhms, double rPotOhms,
                                    double rLoadOhms) {
    const double full = dividerGain(1.0, rSeriesOhms, rPotOhms, rLoadOhms);
    return (full > 0.0) ? dividerGain(w, rSeriesOhms, rPotOhms, rLoadOhms) / full : 0.0;
}

// ---------------------------------------------------------------------------
// RAT — 100 k LOGARITHMIC, unloaded
// ---------------------------------------------------------------------------
// SOURCED (netlist-grade). `Cushychicken/ltspice-guitar-pedals`,
// `proco-rat-distortion/proco-rat-distortion.asc`, the author's own annotation:
//
//     TEXT 1808 200 Left 2 ;NOTE: R14 is volume pot (100k, logarithmic)
//
// cross-checked by a parts-list extract ("100K-A pots for volume, tone and
// distortion"). docs §66.3 established this and did not ship it.
//
// TOPOLOGY, re-traced for this slice: the pot is the last element. It is driven
// by J1, the 2N5485 JFET SOURCE FOLLOWER (node 1680,320 -> C13 1 µF -> the pot),
// whose output impedance is ~1/gm ∥ R13 10 k, i.e. a few hundred ohms against a
// 100 k pot — law-neutral AND negligible, so no series term. The wiper goes to
// the output jack and is loaded by nothing inside the pedal.
//
// (The .asc draws R14 as a single `{Rlevel}` rheostat to ground and reads Vout
// ABOVE it — a convenience for the author's level sweep, not the wiring. The
// real pot is a divider with the wiper on the jack. Recorded so a later reader
// of the same file does not "correct" this to a rheostat.)
//
//     => the BARE audio taper. 11.920 % at half rotation.
inline double ratLevel(double knob) { return audioTaperWiper(knob); }

// ---------------------------------------------------------------------------
// TS808 — 100 k LOGARITHMIC, wiper LOADED by the output buffer
// ---------------------------------------------------------------------------
// SOURCED (netlist-grade). `Cushychicken/ltspice-guitar-pedals`,
// `ts808-tube-screamer/ts808_tube_screamer.asc`:
//
//     TEXT 1840 -344 Left 2 ;NOTE: R15 is level pot (1-100k, log)
//
// with the pot drawn as its two legs (R15 = {100k - Rlevel + 1} from the tone
// stage, R16 = {Rlevel + 1} to the V4P5 bias rail) so the wiper fraction is
// Rlevel/100k directly.
//
// CROSS-CHECKED, independently: `UC3Music/IceScreamer`, a TS clone whose build
// doc reads "MILK (Level): 100K logarithmic or lineal installing 10K on R19"
// — i.e. the stock part is logarithmic and a linear one needs a lin-to-log
// conversion resistor to imitate it. Two sources, same letter.
//
// TOPOLOGY, traced node by node out of the .asc's WIRE/FLAG records:
//   * driven by the tone stage's op-amp (U2, an LT1214 stand-in) — a voltage
//     source, so Rs = 0;
//   * the wiper node (FLAG 2288 336 Vlevel) carries R19 = 510 k to V4P5, and
//     through C8 = 1 µF the base of Q2 (2SC4081 EMITTER FOLLOWER, R17 = 10 k
//     emitter, base biased by R18 = 510 k to V4P5).
//
// So the wiper load is
//     RL = R19 ∥ ( R18 ∥ Zin(Q2) )
// with Zin(Q2) = r_pi + (beta+1)·R17. Q2's base sits at the 4.5 V rail, so
// Ie = (4.5 - 0.65)/10 k = 0.385 mA; at beta = 200 (a mid-range figure for this
// class of part) r_pi = 13.4 k and Zin = 2.02 M, giving
//     RL = 510 k ∥ (510 k ∙∥ 2.02 M = 407 k) = 226 k.
//
// **The beta is NOT a fit and it barely matters** — that is asserted, not
// claimed. Across the whole plausible range the delivered half-rotation moves:
//     beta = 120 -> RL 211 k -> 11.354 %
//     beta = 200 -> RL 226 k -> 11.391 %   <- shipped
//     beta = 400 -> RL 240 k -> 11.421 %
//     beta = inf -> RL 255 k -> 11.449 %   (R19 ∥ R18 alone)
// i.e. 0.08 dB from one end of the range to the other. If a future slice sources
// the real transistor, this constant will not move audibly.
//
// Note the loading does NOT disturb the top of the knob: at w = 1 the top leg is
// zero, so `dividerGain(1, ...) == 1` exactly, with or without RL.
inline constexpr double kTsPotOhms = 100.0e3;
inline constexpr double kTsWiperLoadOhms = 226.0e3;

// A LOG pot whose wiper is loaded, op-amp driven (so no series term). This is
// the form `OverdriveEngine` calls with its config's values — it must NOT be
// re-implemented at the call site. (It was, briefly, during this slice: the
// engine inlined the same expression and `tsLevel` existed only for the tests,
// so a perturbation of `tsLevel` moved the TEST's idea of the law and left the
// AUDIO untouched — a bar comparing the two would have agreed for the wrong
// reason. One function, both callers.)
inline double loadedLogPot(double knob, double rPotOhms, double rLoadOhms) {
    return normalisedDividerGain(audioTaperWiper(knob), 0.0, rPotOhms, rLoadOhms);
}

inline double tsLevel(double knob) {
    return loadedLogPot(knob, kTsPotOhms, kTsWiperLoadOhms);
}

// ---------------------------------------------------------------------------
// SD-1 — NOT SOURCED. It inherits the TS's law, and that is a stated assumption
// ---------------------------------------------------------------------------
// **No SD-1 netlist or parts list was reachable from this container.** The proxy
// permits github.com only (re-confirmed this slice: hobby-hour, effectslayouts,
// stompboxschematics, electricdruid, electrosmash and kitrae all fail at the
// egress proxy, and GitHub's code search needs a login). A GitHub search found
// no SD-1 `.asc`. Search-result summaries exist and CONTRADICT each other on
// this exact question — one build guide calls a 100 k AUDIO taper level pot a
// *mod*, another reports builders using B100k — so they settle nothing and are
// not used.
//
// What this file therefore does, and what it does NOT do: it does NOT invent a
// letter for the SD-1 (§57's rule). It carries forward a relationship that has
// ALREADY shipped since the v1.1 refactor and is written into
// `OverdriveEngine.h` in terms:
//
//     "Everything else (the mid-hump corner, the 4558 op-amp values, the
//      tone/level topology) is IDENTICAL — which is exactly why the two pedals
//      are one engine."
//
// The SD-1's output-pot law is that same documented inheritance, unchanged in
// kind by this slice: it was the TS's identity map before and it is the TS's
// sourced law now. It is a **documented reconstruction, not a sourced fact**,
// and it is the largest gap this slice leaves.
//
// It is carried in `OverdriveConfig` rather than hard-coded so a slice that DOES
// find the SD-1 schematic can move one pedal without touching the other.
// §57's rule applies verbatim: do not re-tune it toward a sound; find the
// schematic.

// ---------------------------------------------------------------------------
// MUFF — 250 k AUDIO (A) taper, unloaded, behind a law-neutral 10 k source
// ---------------------------------------------------------------------------
// SOURCED (parts-list-grade, and the author distinguishes the letters).
// `Circle-Circuits/motherboard` — an open-source Big Muff motherboard whose BOM
// lists the three pots with their tapers spelled out:
//
//     "R6 (Gain), R20 (Tone)"  ->  B100k   (linear)
//     "R25 (Level)"            ->  A250k   (audio)
//
// An author who writes B for two pots and A for the third is distinguishing
// them, which is the same class of evidence §63 accepted from LiveSPICE marking
// the Rockerverb's Gain/Bass `Logarithmic` and Treble/Middle/Volume `Linear`.
// Confirmed against the project's own schematic SVG (the label "A250k" sits on
// the VOLUME symbol at 379,68).
//
// TOPOLOGY, read off the same schematic: Q4 (the recovery stage, collector load
// R24 = 10 k) drives the pot's top through C13 = 0.1 µF; the pot's bottom is
// grounded; the wiper goes to the footswitch Return, i.e. straight to the output
// jack. So Rs = 10 k and RL = none.
//
// **Rs = 10 k against a 250 k pot is LAW-NEUTRAL** — it is in series with the
// whole pot, so it scales every wiper position by 250/260 alike. It is worth a
// flat -0.34 dB and it is normalised out (see the NORMALISATION note). The law
// that remains is the bare audio taper, identical in shape to the RAT's.
//
// RECORDED DISAGREEMENT: this BOM's 250 k is not the 100 k the classic ram's-head
// families are usually quoted with. It does not matter to the LAW — an unloaded
// divider's transfer is scale-free — and it is only the letter this file needs.
// Noted so nobody reads 250 k here as a claim about the part.
inline double muffVolume(double knob) { return audioTaperWiper(knob); }

// ---------------------------------------------------------------------------
// GOLD — 10 k LINEAR (B) taper inside a real network. NOT an audio taper.
// ---------------------------------------------------------------------------
// This is the pedal that makes the "do not apply k = 4 five times" instruction
// load-bearing, and both halves of it are sourced.
//
// NETWORK, netlist-grade: `jatinchowdhury18/KlonCentaur` (the same reference §52
// and §54 derived the summing weight and the clipping stage from — it carries
// the netlist AND the ElectroSmash schematic figures), file
// `ChowCentaur/CommonProcessors/OutputStageProcessor.cpp`:
//
//     R1 = 560 + (1 - level)·10000     // R25 in series + the pot's top leg
//     R2 = level·10000 + 1             // the pot's bottom leg
//     C1 = 4.7e-6                      // C15, in series with the whole thing
//
// and its `OutputStageWDF.h` sibling (kept "for reference") names the wiper's
// load explicitly: `P1 = R28 ∥ RVBot` with `R28 = 100e3`.
//
//     => Rs = 560 Ω (R25), Rp = 10 k (the pot), RL = 100 k (R28).
//
// C15 = 4.7 µF makes a high-pass with the ~10.3 k the chain presents at noon:
// **3.3 Hz**, two decades below the band, so it is a level-flat 0.00 dB from low
// E up and is deliberately NOT modelled here. (The GOLD already carries its own
// DC blocking; adding a fourth one to catch 3.3 Hz would be noise in the diff.)
//
// TAPER LETTER, search-summary-grade and CORROBORATED: two independent extracts
// of the ElectroSmash / Coda-Effects Klon analyses state that the ORIGINAL uses
// a LINEAR (B) taper on Output and that an audio taper is a popular MOD. That is
// the same provenance grade §66.4 accepted for the LM308's slew figure, and it
// is labelled as such — but it does not stand alone: the reference
// implementation above maps its `level` control straight to the wiper position,
// i.e. models the pot as linear, independently of the prose.
//
// MEASURED CONSEQUENCE, and it is the point: the delivered law comes out within
// **0.2 dB of the identity map it replaces** at every knob position. So this
// pedal's existing "linear, house convention" map was ALREADY RIGHT, and it is
// now right for a sourced reason instead of a convention. Had the house audio
// taper been applied here it would have taken the GOLD 7.7 dB down at its
// shipped 0.7 default against a source that says linear.
inline constexpr double kGoldSeriesOhms = 560.0;    // R25
inline constexpr double kGoldPotOhms = 10.0e3;      // the OUTPUT pot
inline constexpr double kGoldWiperLoadOhms = 100.0e3;  // R28

inline double goldOutput(double knob) {
    return normalisedDividerGain(linearWiper(knob), kGoldSeriesOhms, kGoldPotOhms,
                                 kGoldWiperLoadOhms);
}

}  // namespace clipper::dsp::pot

#endif  // CLIPPER_DSP_OUTPUT_POT_TAPER_H
