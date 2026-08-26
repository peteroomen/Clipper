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

## Research results (2026-08-25, before any code)

**Owner decisions:** Q1 → **(b)** one knob + the house REVERB slot (§19 JCM precedent).
Q2 → **(a)** synthesise a `tweed8` cab.

### The netlist, transcribed

From `valtyr/rust-5f1` `src/circuit/netlist.rs` + its capacitor table (component
values only; RULE ZERO boundary held — `tube_models.rs`, `solver.rs`, `sim.rs`'s
simulation loop and the whole `dsp/` directory were NOT opened):

| Part | Value | Part | Value |
|---|---|---|---|
| V1A/V1B grid leak | 1 MΩ | V1A/V1B plate load | 100 kΩ |
| V1A/V1B cathode | 1.5 kΩ, bypassed 25 µF | Volume pot | 1 MΩ **log** |
| Input coupling | 25 nF | Interstage coupling | 20 nF ×2 |
| 6V6 grid leak | 1 MΩ | 6V6 cathode | **470 Ω** |
| OT reflected load | **5 kΩ** (25:1) | Supply | 325 V, 5Y3, 16 µF ×3 |
| PS filter B1→B2 | 1 kΩ | B2→B3 | 10 kΩ |

**Two corrections to that source, both sourced independently and both shipping:**

1. **It omits the 6V6 cathode bypass cap.** The real 5F1 bypasses the 470 Ω with
   **25 µF**; search extracts confirm it ("the cathode bypass capacitor is 25 µF…
   a '25-25' attached to the cathode of the 6V6"). Unbypassed, that 470 Ω is strong
   local degeneration and a materially different amp. **We ship the cap.**
2. **Its supply is a 100 Ω series resistance**, which drops only ~4 V and lands the
   plate node at 320 V. A 5Y3 is a notoriously saggy directly-heated rectifier and
   Fender's own measured plate node is **305 V**. We take §55's Thévenin form instead
   (a constant `kRsupply` behind the raw HT), derived below — which also makes SAG a
   first-class property of this amp rather than an afterthought.

It also models the OT primary as a **DC** 5 kΩ to B+, which would sit the plate 170 V
below the rail. This project already does the correct thing (plate at the rail at idle,
`Vp = rail − (i − iq)·R_reflected` as the AC load line), so that simplification is
**not inherited**.

### The 6V6 device card — DERIVED, and validated against two absolute references

The Koren form makes `Ig2/Ip = (kg1/kg2)/atan(Vp/kvb)`. Evaluated against the
**RCA/TAD datasheet** at two operating points — P1 (Vp 250, Vg2 250, Vg1 −12.5 →
Ip 45 mA, Ig2 5 mA) and P2 (Vp 315, Vg2 225, Vg1 −13 → Ip 34 mA):

| fit | P1 Ip | P2 Ip | P1 Ig2 | P2 Ig2 |
|---|---|---|---|---|
| A `pedalkernel` | 0.66× | 0.59× | 1.40× | 1.96× |
| B `vgreff` Koren | **1.03×** | **0.98×** | 2.26× | 3.68× |

**Fit B's shape and `kg1` are right and its `kg2` is not** — the plate current lands
within 2–3 % at two different `Vp` AND `Vg2`, with errors of opposite sign, which is a
real external check rather than a self-fit. So B's `mu/ex/kg1/kp/kvb` are kept
**unmodified** (deliberately NOT trimmed to P1 exactly — that would worsen P2) and
only `kg2` is derived, against P1's screen current, the one screen figure actually
returned by search:

    kg2: 4500 -> 10148.2      (Ig2 at P1 exactly 5.000 mA)

**Shipped card: `mu=10.70  ex=1.310  kg1=1672.0  kp=41.16  kvb=12.7  kg2=10148.2`.**

### The second absolute reference: FENDER'S OWN measured 5F1 voltages

Fender publish the 5F1's operating point — **19 V across the 470 Ω cathode resistor**
(= 40.4 mA total), **plate node ~305 V** (so Vpk = Vg2k = 286 V) and **37 mA** of
plate current, leaving ~3.4 mA of screen current by difference. Evaluating each card
at *those node voltages* — nothing fitted, the currents are the model's prediction:

| card | Ip vs 37 mA | Ig2 vs 3.4 mA | Ik vs 40.4 mA | screen diss |
|---|---|---|---|---|
| **derived (shipped)** | 35.84 mA **0.969×** | 3.87 mA 1.138× | 39.71 mA **0.983×** | **1.11 W** |
| A published | 15.36 mA **0.415×** | 3.45 mA 1.015× | 18.81 mA 0.466× | 0.99 W |
| B published | 35.84 mA 0.969× | 8.72 mA **2.566×** | 44.57 mA 1.103× | **2.50 W** |

**3.1 % on plate current and 1.7 % on total cathode current**, from a card fitted to a
different manufacturer's datasheet and checked at Fender's operating point. Comparable
to §69's Mesa figures (1.25–3.98 %), and this project's second amp with an absolute
external DC reference.

### Audit finding 10, confirmed on a third tube — and this one is FIXED

Finding 10's table is EL34 0.102 / 6L6 **0.210** / EL84 **0.363** against real screen
ratios near 0.10. The 6V6 continues it exactly: both published fits predict
`Ig2/Ip ≈ 0.237–0.244` against the datasheet's **0.111**, i.e. **2.1–2.2× too high**.
At the Champ's own idle the published fit B puts screen dissipation at **2.75 W —
precisely the 6V6GT's rating**, the same "exceeds its rating at idle" pathology the
audit measured on the AC30's EL84. The derived card sits at **1.11–1.32 W**.

*(Correction to this plan's pre-work estimate: it quoted "2.2× and 3.3× too high" from
the bare `kg1/kg2` ratio. The Koren law divides that by `atan(Vp/kvb)`, so the correct
figures are 2.14× and 2.20×. The conclusion — both fits wrong, `kg2` must be derived —
is unchanged.)*

**Known residual, reported not fitted:** one `kg2` cannot match both datasheet points
(P2's screen lands 1.63× high) because **the Koren screen law has no `Vp` dependence**
— which is finding 10's own closing paragraph, and the reason a real power amp's screen
current surges when `Vp` falls below `Vg2`. Not modelled here; named.

### Free corroboration for the Twin

`pedalkernel/models/pentodes.model`'s 6L6GC row is
`MU=8.7 EX=1.35 KG1=1460 KG2=4500 KP=48 KVB=12` — **byte-for-byte this repo's shipped
`Tube6L6Params`** (§20). An independent source arriving for free. Note its `kg1/kg2` =
0.210 is finding 10's exact figure, so that corroboration is of the *plate* fit only.


### The build, in order

1. `ChampPowerAmp` (single-ended 6V6, cathode bias, §55 Thévenin supply), `ChampPreamp`
   (two 12AX7s either side of the 1 MΩ log pot, no tone stack), `ChampAmp` (ONE shared
   oversampling domain from the first commit).
2. `clipper_champ_tests` — 15 tests, every bar in "How this will be measured".
3. C ABI voice 7, the `tweed8` cab, the web front end, the native front end.

### Two real bugs found and fixed during the build

1. **A reference-frame bug in my own first draft, caught by a silent render.** The
   operating-point solve used plate-to-CATHODE volts while the process loop passed
   plate-to-GROUND — a 19 V discrepancy on a cathode-biased tube, so the idle solve and
   the run loop sat on different load lines. It showed as a **0.23 startup transient
   into a silent render** (should be ~0). Fixed by making the run loop cathode-referred;
   silence now measures **3.008e-14**. The comment in `ChampPowerAmp.cpp` records how it
   was caught, because a 19 V frame error is invisible in every steady-state number.
2. **A PRE-EXISTING bug on `main`, found by writing the web spec.**
   `Board.tsx`'s `AMP_TYPE_LABEL` was a `Record<string, string>` with **no `mesa`
   entry**, so **M10.4's Dual Rectifier renders as a BLANK item in the amp menu** — the
   lookup returns `undefined` and React draws nothing. `Record<string, …>` accepts any
   key, so nothing could catch it. Retyped `Record<AmpType, string>`, making a missing
   voice a **build error**. Fourth instance of this class (§61.10 `kFaces`, §62,
   §67.10 `pedalMenuLabel`, §71 `kAmpModelChoices`) and the second fixed structurally.

### One bar that had to be re-aimed, and it was NOT loosened

`testRestingState` was written to assert `maxAbsRestingState() == 0.0` and went red at
**5.434e-11**. Bisected: all three zero-resting states **PLATEAU** (identical at 1, 4, 8,
20 and 41 s), because `TriodeStage`'s grid Newton exits at a residual tolerance and
re-excites them — §69's case, not §56.4b's. Asserting a zero this amp does not have
would have been wrong, so the bar became **what the anti-denormal policy is actually
for**: no subnormal float leaves the model (**0** over the whole silent tail), the
plateau is *settled* (unchanged to a part in 1e6 over a further 30 s), and it sits
decades above the subnormal boundary. Strictly more informative than the original.

## Measured results

All tables are in **docs §72**. Headlines:

| | |
|---|---|
| h2, Champ vs the balanced Twin (identical stimulus) | **−14.84 vs −39.72 dBc = 24.88 dB** |
| Plate knee current (audit finding 9) | **88.4 mA** vs finding 9's 530 mA from one EL34 |
| No NFB | **bit-identical**, 0 of 24000 samples |
| Fender's measured point (1 constant fitted, rest predicted) | Vk **0.9943×**, Ip **0.9805×**, Ik **0.9950×**, Vpk **1.0004×** |
| Screen fit (audit finding 10) | ours **1.12 W** vs the published fit's **2.53 W** against a **2.75 W rating** |
| Breakup at 0.15 V | VOL 0.05 **3.89 %** · 0.10 **7.55 %** · 0.30 **28.61 %** |
| Touch sensitivity at VOL 0.20 | hard **16.63 %** / soft **5.86 %** = **2.84×** |
| Power | section ceiling **5.17 W** (rated ~5); composed cranked **3.89 W**, reported |
| Latency | **72 samples / 1.50 ms** (per-stage domains measured 216) |
| `tweed8` −6 dB corner | **198.3 Hz** vs 75.0 / 103.2 / 104.7 for the three 12" cabs |
| Ragged-100 · `reset()` · NaN · DC · rate spread | 0.000e+00 · 0.000e+00 · 0/24000 · 0.2498 % · 0.014 dB |

**Suites:** core ctest **39/39** (was 38 — `clipper_player_expectations_tests` passes, so
**all five goldens unchanged, nothing blessed**); native **4/4** including
`clipper_amp_voice` (§71's reachability test); node **15 / 10 / 12**; electron **20**;
`tsc --noEmit` clean; web build clean; WASM artifact rebuilt (**113** hashed inputs).

## Files created / modified

**Core:** `ChampPowerAmp.{h,cpp}`, `ChampPreamp.{h,cpp}`, `ChampAmp.{h,cpp}` (new),
`CabIR.{h,cpp}` (+`generateTweed1x8IR`), `clipper_c_api.cpp`, `CMakeLists.txt`,
`tests/test_champ_amp.cpp` (new).
**Web:** `rig.ts`, `params.ts`, `audio.ts`, `App.tsx`, `components/{Amp,Board}.tsx`,
`styles/{tokens,amp}.css`, `assistant/{tools,prompt}.ts`, `tests/{amp,audio}.spec.ts`.
**Native:** `ClipperEngine.{h,cpp}`, `PluginProcessor.{h,cpp}`, `PluginEditor.{h,cpp}`,
`ClipperLookAndFeel.{h,cpp}`.
**Build/docs:** `scripts/build-wasm.sh`, `web/public/generated/*`, `docs/DEVELOPMENT.md`
(§72), `ROADMAP.md`, `CLAUDE.md`, this file.

## Deferred to next session

- **The editor still has no automated behaviour test** (§71's named gap), now covering
  one more face. `updateAmpFace()`'s `case 7` is covered by review.
- **A Vibro-Champ** — cheap now, `OptoTremolo` exists.
- **Audit findings 9 and 10 on the EXISTING push-pull amps.** Deliberately untouched:
  that moves four goldens and is its own slice.
- **The Koren screen law has no `Vp` dependence** — the mechanism behind real screen
  sag, absent in every pentode in this project.
- **`kRptSecondary` (200 Ω) is a reconstruction.** Find a Champ PT winding resistance
  and the supply becomes fully sourced.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
