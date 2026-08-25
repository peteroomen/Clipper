# Fender Champ 5F1 — the tweed, and the lineup's first SINGLE-ENDED output stage

**Date:** 2026-08-25
**Branch:** claude/roadmap-review-build-planning-l0eov1
**Roadmap item:** M10.10 — Fender Champ (5F1). Also carries audit **findings 9 and 10**
(the plate load line; the screen-current fits) onto a tube where both can be measured
against an absolute external reference for the first time.

## Goal

The tweed Champ as amp voice **7** (`champ`), wired end to end in one slice — the
first Fender in the lineup that distorts, and the first power section here that is
**not push-pull**.

## Why this voice is not a re-skin

Every amp in this lineup — JCM800, Twin, AC30, OR120, Rockerverb, Mesa — is a
push-pull pair behind a phase inverter. The Champ has **no phase inverter at all**:
one 6V6GT, cathode-biased, straight into the output transformer. Four consequences,
each of which is an acceptance bar below:

1. **Nothing cancels the even harmonics.** A push-pull pair's h2 cancellation is the
   property `TwinPowerAmp` exists to have (the balanced Twin leaks −42.8 dBc). A
   single-ended stage has no opposite leg, so h2 is *dominant by topology* — not a
   voicing choice, and not something a re-skin of existing machinery can produce.
2. **The plate load line is real here.** Audit finding 9 measured that the push-pull
   amps need ~530 mA from one EL34 before the plate reaches the knee — more than
   twice a real tube's peak cathode current — so plate-load saturation, which the
   headers *name* as the clipping mechanism, never actually happens; clipping comes
   entirely from grid cutoff and conduction. For a genuinely single-ended stage
   `Vp = rail − Ip·R_L` is the correct relation, so this is the first amp in the
   project where plate-load clipping happens at physical currents.
3. **There is no negative feedback.** The AC30's anti-NFB catcher precedent applies:
   assert open-loop == closed-loop **bit-exact**, so nobody can quietly add a loop.
4. **There is no tone stack and no master volume.** Which means the voicing is
   entirely the coupling caps, the cathode bypasses and the OT — there is nothing
   downstream to correct it with, and no master to decouple drive from level.

## Correction to the roadmap entry (do this in the same slice)

`ROADMAP.md`'s M10.10 says *"Single-ended class A 6V6, **ONE tone control**, no
negative feedback"*. **The 5F1 has NO tone control** — one knob, volume, and nothing
else. The Champ did not gain a tone control until the 1964 blackface AA764. Same
class of error as §69's and §67's roadmap framings, and it gets corrected in place
with the reason recorded, not silently.

## Research channel — what is sourced and what is not

Honest position up front: this is **better than §57's OR120 (a full reconstruction)
and worse than §69's Mesa (factory sheets with marked DC voltages)**. It is a
*partial transcription*.

| Fact | Source | Grade |
|---|---|---|
| Topology: 25 nF in → V1A (12AX7, cathode-biased) → 1 MΩ **log** volume pot → V1B → 20 nF → 6V6GT → OT | `valtyr/rust-5f1` README signal flow | documented |
| OT 25:1 turns, **5 kΩ reflected**; supply 325 V HV through a 5Y3, three-stage RC filter, 16 µF caps | same | documented |
| 6V6 cathode **470 Ω**, bypass **25 µF** | search extracts (multiple, consistent) | search-grade |
| ONE volume knob, no tone control, no NFB | search extracts (multiple, consistent) | search-grade |
| Component-level R/C values | `valtyr/rust-5f1` `src/circuit/netlist.rs` | **to be read — see RULE ZERO below** |
| **6V6GT device card, fit A** — `MU=12 EX=1.3 KG1=1100 KG2=4500 KP=300 KVB=150` | `ajmwagar/pedalkernel` `models/pentodes.model` | published fit |
| **6V6GT device card, fit B** — `MU=10.70 EX=1.310 KG1=1672.0 KG2=4500 KP=41.16 KVB=12.7` ("GE data sheet") | `vgreff/LTSpiceLibraries` `Koren_Tubes.lib` | published fit |
| **6V6GT datasheet class-A1 operating point: Vp 250 V, Vg2 250 V, Vg1 −12.5 V, Ip 45 mA, Ig2 5 mA, R_L 5 kΩ, Pout 4.5 W @ 10 % THD** | TAD 6V6GT-CZ datasheet via search | **ABSOLUTE reference** |

**Unreachable, confirmed this session:** `robrobinette.com`, `ampbooks.com`,
`frank.pocnet.net` and `prowessamplifiers.com` all fail at the egress proxy, and
GitHub's *code search API* is repository-scoped here (public `git clone` works, which
is how the two device cards above were read). So there is no Fender factory sheet in
reach and **§57's rule applies to everything not in the table: do not re-tune it
toward a sound; find the schematic.**

**RULE ZERO boundary, stated before the work starts.** `valtyr/rust-5f1` is *another
vendor's model of this same amp*. §67 records that none of the three GitHub Uni-Vibe
implementations was opened, and that stands. The line this slice draws is §63's: a
**netlist is a circuit description** (LiveSPICE's `.schx` was parsed node by node)
while a **tube fit, a solver or a voicing constant is someone's model**. So:
`src/circuit/netlist.rs` may be read for R/C values; `tube_models.rs`, `solver.rs`,
`sim.rs`, `companion.rs` and the DSP directory **may not be opened at all**, and no
constant in this slice may be justified by agreement with that project's output.

## The two published 6V6 fits DISAGREE, and neither will pass the screen check

This is the most useful thing the research turned up, and it is audit finding 10
appearing on a third tube *before a line of code is written*.

The Koren pentode form makes the screen/plate ratio a **fixed constant**
`Ig2/Ip ≈ kg1/kg2`. The datasheet operating point above gives the truth:

    Ig2/Ip = 5 mA / 45 mA = 0.111

The two published fits imply:

| fit | kg1/kg2 | vs datasheet 0.111 |
|---|---|---|
| pedalkernel  | 1100/4500 = **0.244** | **2.2× too high** |
| vgreff/Koren | 1672/4500 = **0.372** | **3.3× too high** |

Which is exactly the pattern finding 10 measured for the EL84 (0.363) and the 6L6
(0.210). **So the fit is not going to be inherited — `kg2` gets DERIVED against the
datasheet screen current**, and the plate side is then checked independently against
the datasheet plate current at the same bias. Champ becomes the first tube in this
project whose screen fit is right, and that is a hard assert, not a note.

The 6L6GC row in `pedalkernel/models/pentodes.model` is
`MU=8.7 EX=1.35 KG1=1460 KG2=4500 KP=48 KVB=12` — **byte-for-byte this repo's shipped
`Tube6L6Params`**. That is an independent corroboration of the existing Twin fit
arriving for free, and it is worth recording in §20's section.

## Approach

Deliberate tone change? **No** — a new voice cannot move an existing one. The bar is
that **all five goldens stay UNCHANGED at ±0.00 and nothing is blessed**, and the
scope check is that no shared class is edited. New files plus the C ABI, CMake, the
two tools, and the two front ends.

- `core/include/clipper/dsp/ChampPowerAmp.h` / `.cpp` — the single-ended stage. New
  `Tube6V6Params` + a `to6V6()` bridge into the existing `El34Params`-typed Koren
  evaluators, exactly as `Tube6L6Params` does (the Koren pentode form is
  device-agnostic; only the six constants differ). **`LtpInverter` is NOT used and
  must not be** — there is no phase inverter in this amp.
- `core/include/clipper/dsp/ChampPreamp.h` / `.cpp` — two 12AX7 common-cathode
  stages either side of the 1 MΩ log volume pot, composed from `TriodeStage`.
- `core/include/clipper/dsp/ChampAmp.h` / `.cpp` — composition + the supply.
- **ONE oversampling domain from the first commit** (§63.14's lesson applied up
  front, as §69 did): both halves prepared at the oversampled rate with their own
  resamplers at 1×. Expect latency 72 samples / 1.50 ms, not the Rockerverb's 360.
- `AMP_CHAMP = 7`, `AMP_MODEL_COUNT` → **8**. §71's reachability test then covers it
  automatically — and that is the point of that constant existing.
- **No new param id.** VOLUME → the shared slot 0. Nothing else is on the panel.
  (See the two open questions below.)

## How this will be measured

Each bar is a hard assert in a new `clipper_champ_tests`, and every one is a contrast
against an existing voice or an absolute external number — never against an analytic
form derived from the same netlist.

1. **Single-ended h2 dominance.** h2 measured on the identical house stimulus,
   Champ vs the balanced Twin. Bar: the Champ's h2/h3 ratio is dominant and its h2 is
   ≥ 20 dB above the Twin's −42.8 dBc leakage. This is the headline and it cannot be
   faked by a re-voice.
2. **The screen fit is right** — `Ig2/Ip` at the datasheet bias lands on **0.111**,
   against the 0.244 / 0.372 the two published fits imply. Plate current at the same
   bias lands on the datasheet's 45 mA independently.
3. **Rated power** — 4.5 W @ 10 % THD into 5 kΩ, the datasheet's own figure. §42's
   "smallest scale that reaches rated power" criterion, with §57.3's standing
   instruction: if it falls short, **report it, do not close it with a higher
   `kVsupply` or a softer screen network**.
4. **Plate-load saturation happens at a PHYSICAL current** — the current at which the
   plate reaches the knee is inside a real 6V6's peak cathode rating, where finding 9
   measured 530 mA for the push-pull EL34. Asserted as an absolute window.
5. **No NFB** — open-loop and closed-loop renders **bit-identical** (the AC30's
   anti-NFB catcher, `0` differing samples).
6. **Breakup is immediate** — ≥5 % THD onset at a LOW volume position, contrasted
   against the Twin's 0.9 and the AC30's 0.5 on the same probe. "Breaks up almost
   immediately" as a number.
7. The house block: DC offset on signal (and with a +0.1 V input offset), ragged
   128-frame block invariance == 0.000e+00, `reset()` vs a fresh model == 0.000e+00,
   one NaN → 0/48000, rate spread over 44.1–96 kHz, alias floor vs oversampling
   factor, `maxAbsRestingState()` per ADR 006 **decided by measurement**, CPU.
8. **Goldens:** all five `--golden-report` rows UNCHANGED at ±0.00. Nothing blessed.
9. **Perturbation-proven:** every bar above patched red and restored green, tabulated
   in the plan file. Specifically including *"put the published kg1/kg2 back"* (bar 2)
   and *"re-introduce a phase inverter / mirror the tube"* (bar 1).

## Manual test steps

- [ ] Select the Champ in the web app, play through it at VOLUME ~3 — clean, small,
      touch-sensitive; at ~7 it should be breaking up without touching anything else.
- [ ] Confirm the panel shows what the plan settles in Q1 below and nothing dead.
- [ ] Switch amp voice Champ ↔ Twin mid-playback — no pop (the declick path).
- [ ] Native: the voice appears in the plugin's `ampModel` choice list and is
      selectable — §71's exact defect, now covered by `amp_voice_test`.
- [ ] Edge case: VOLUME at 0 → silence, no denormal cost on the silent tail.
- [ ] Edge case: slam ±20 V at every rate × oversampling factor — finite, bounded,
      Newton converges inside the cap.

## Two open questions for the owner — these change the work

**Q1 — the panel. The real 5F1 has exactly ONE knob.** Options: (a) ship one knob,
fully faithful — a striking panel, and the only amp here with no tone control;
(b) one knob **plus the house REVERB slot**, which is the §19 JCM800 precedent
("usability over authenticity" — the 2204 has no reverb either and was given one).
**Recommendation: (b).** Every other amp in the lineup has reverb, and a Champ with a
spring tail is the single most common way people actually record one. It costs no
authenticity argument that §19 has not already made and won.

**Q2 — the cab.** A Champ is an **8-inch speaker in a small open-back tweed box**;
the existing cabs are a 2×12, a 4×12 and an Orange 4×12. Reusing any of them
misrepresents the amp badly — the small box is a large part of why a Champ sounds
like a Champ. Options: (a) synthesise a **`tweed8`** in the §15 modal-synthesis house
style (roadmap doctrine: synthesise, never commit a captured third-party IR); (b) ship
on `clean212` and defer. **Recommendation: (a)**, accepting it grows the slice — and
noting §57's native trap: `CabChoice` ids and the C ABI's built-in ids already diverge
at 2 deliberately, so a new cab is **appended**, never inserted, and the two spaces are
mapped in code.

## Out of scope for this session

- The **Vibro-Champ** (the 5F1's tremolo sibling) — `OptoTremolo` exists, so it is
  cheap later, but it is a different amp.
- Audit findings 9 and 10 **on the existing push-pull amps**. This slice builds the
  plate load line correctly for a genuinely single-ended stage and derives one screen
  fit against a datasheet; it does **not** touch `Jcm800PowerAmp` / `TwinPowerAmp` /
  `Ac30PowerAmp`, because that would move four goldens and is its own slice.
- The 5Y3 rectifier as a *switchable* item (the Mesa's rect select). The Champ has one
  rectifier; it is modelled, not exposed.
- Anything on the pedal parameter path — a parallel session is mid-slice on the
  10-band EQ there (`claude/roadmap-priorities-kok3vx`), and this slice deliberately
  stays on the amp side to avoid the collision.

---

<!-- Fill in below during/after the session -->

## What actually happened

## Measured results

## Files created / modified

## Deferred to next session

## Status

- [x] In progress
- [ ] Complete
- [ ] Partial — see deferred
