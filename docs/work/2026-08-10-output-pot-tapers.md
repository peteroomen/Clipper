# The output-pot tapers — lineup-wide

**Date:** 2026-08-10
**Branch:** `feat/output-pot-tapers`
**Roadmap item:** §66's named follow-up, and the XFAIL ledger entry
`rat-level-pot-linear-not-log` it opened.

## Goal

Give all five dirt pedals' OUTPUT pots their real logarithmic law in one slice,
one bless — closing `rat-level-pot-linear-not-log` and removing the
lineup-wide approximation §66 measured but deliberately did not fix.

## Why this is one slice and not five

§66 registered the XFAIL on the RAT and said in terms why it stopped there:
every sibling carries the identical approximation, so fixing one pedal alone
re-stages the whole lineup against four pedals still on the wrong law, and it
would undo §36's diode fix **by a knob law**. Confirmed by reading, 2026-08-10 —
all five map the knob straight to a gain target:

| Pedal | Site |
| --- | --- |
| RAT | `RatModel` (its own output stage) |
| SD-1 · TS | `OverdriveEngine.cpp:141` — *"identity linear map, as the RAT"* |
| Muff | `MuffModel.cpp:407` — `impl_->volume.setTarget(knob)` |
| GOLD | `GoldModel.cpp:821` — *"identity linear map, as the RAT/TS"* |

## The measurement that starts the slice, before any edit

§66's numbers are the RAT's only. **Re-measure all five first** at the 0.15 V
unity-trim probe, 220 Hz, 48 kHz, shipped defaults — the §66 baseline is
`rat −6.40 · sd1 −7.97 · ts −9.19 · muff −4.73 · gold −8.38 dBFS`, a 4.5 dB
spread. That table is the before/after ledger and the scope check.

**The known trap, stated up front:** §66 measured that substituting the house
`audioTaper` (k = 4) on the RAT alone takes it to −11.61 dBFS — the quietest of
five, reproducing the pre-§36 staging to within 0.4 dB. That is the *correct*
law producing a *wrong* lineup, because the other four are still linear. So the
acceptance bar is the **spread after all five move**, not any pedal's absolute
level.

## Approach

**Deliberate tone change.** Each pedal's output pot gets the law its own netlist
says it has — sourced per pedal, not one house constant applied five times. §66
established the RAT's from `Cushychicken/ltspice-guitar-pedals`
(`proco-rat-distortion.asc`, annotation `R14 is volume pot (100k, logarithmic)`,
sitting last after the source follower and essentially unloaded, so its law is
the bare audio taper). The other four need the same treatment: **find the
netlist, read the pot, record what could not be sourced.** Where no source is
reachable, say so and carry the approximation rather than inventing a letter —
§57's rule.

**Do not** reach for `audioTaper(k = 4)` five times because it is there. A pot
that is loaded by the next stage does not deliver its bare taper, and §63.14
already measured what happens when a taper is chosen to fix a level complaint
instead of derived: it cannot, and the defaults were the real answer.

## Steps

- [x] Baseline table: all five pedals, shipped defaults, before any edit
- [x] Per pedal: source the pot law from a netlist; record sourced vs assumed
- [x] Apply; re-measure the spread
- [x] **Rewrite `testLevelLinearity` in all three test files** — there were FIVE
      sites, not three — `test_rat_model.cpp:509`,
      `test_ts_model.cpp:279`, `test_sd_model.cpp:314`. §66 flagged these
      explicitly: they **assert the linear map**, so they are drift guards on a
      known approximation and the fixing slice must rewrite them, not delete them
- [x] Delete the `rat-level-pot-linear-not-log` XFAIL — an **XPASS is a hard
      failure**, so it must go in this slice (ctest 35 → 34, ledgers 6 → 5)
- [x] `--golden-report` table; stop and hand it to the owner (see below)
- [x] WASM artifact rebuild — **last step**, and re-run it if `main` moved
- [x] Perturbation-prove every new bar

## How this will be measured

| Property | Bar |
| --- | --- |
| Lineup spread at shipped defaults | tighter than, or comparable to, the 4.5 dB baseline — the number recorded, not snugged |
| Each pot's law | half-rotation delivers 10–20 % (the documented audio-taper band) where the netlist says logarithmic |
| §36 not undone | the RAT does not return to the bottom of the pack |
| GAIN/DISTORTION behaviour | unchanged — this slice moves output pots only |
| Goldens | **four will move; see below** |

## ⚠️ This slice cannot be completed without the owner

Four of the five goldens contain one of these pedals — `rat_jcm800`,
`sd1_twin_reverb`, `muff_twin`, `ts_ac30`. Only `clean120_chorus` has no pedal
in it, and GOLD is in no golden rig at all. So four goldens move by construction.

Re-blessing is a ritual, not a command: clean tree, a printed dB table measured
against the *previous* goldens, a confirmation typed at `/dev/tty` (which `yes |`
and CI cannot answer), and a justification in `GOLDENS.md`. **This environment
has no `/dev/tty`** — CLAUDE.md records that the three 2026-07-31 blesses ran the
script's gates by hand on explicit in-chat owner authorization.

**So "done" for this slice = the code, the tests, the XFAIL closed, and a
measured `--golden-report` table presented for sign-off.** The core suite will be
red at exactly those four golden asserts and nowhere else until then — which is
the §36/§47/§51 precedent, and is the design working.

## Manual test steps

- [ ] Each pedal at OUTPUT 0.5 is audibly quieter than before, and the *lineup*
      still balances when swapping pedals on the same board
- [ ] OUTPUT 1.0 unchanged per pedal (a taper pins `taper(1) = 1`) — verify by
      render hash, the §51 precedent
- [ ] Edge: OUTPUT 0.0 is still silence, not a floor
- [ ] Edge: sweep OUTPUT min→max in one block — no zipper

## Out of scope

- The GAIN/DISTORTION tapers (a different pot on a different part of the circuit)
- `kCp`, the RAT's fabricated clipping-node pole (§66's other follow-up)
- M13.5 the Uni-Vibe (parallel slice — **see the artifact note below**)

## ⚠️ Parallel-slice hazard

M13.5 is running at the same time and also touches `core/`, so **both slices must
rebuild `web/public/generated/clipper.js` + `.build-stamp.json`.** CLAUDE.md
records this exact collision burning the project once: two PRs each rebuilt the
artifact, the merge conflicted on the binary, and taking either side would have
shipped an engine holding one fix while the source held both. **Cure:** rebuild
as the final step, and if the other slice merges first, merge `main` and rebuild
again before merging. Never resolve that conflict by picking a side.

---

<!-- Fill in below during/after the session -->

## What actually happened

Full write-up: **docs §67**. The short version:

**All five output pots moved, and only four of them got an audio taper.** Each
pedal's law was sourced from its own netlist rather than from one house constant:
the RAT's 100 k LOG pot is unloaded so it gets the bare taper; the TS's is
logarithmic but its wiper is LOADED by the output buffer (226 kΩ, traced out of
the `.asc`'s WIRE/FLAG records), so it delivers 11.391 % at half rotation instead
of 11.920; the Muff's is an A250k whose wiper drives the jack, so it gets the bare
taper too; and **the GOLD's is a LINEAR (B) pot inside a real R25/R28 network, so
it did not get an audio taper at all** — giving it one would have taken it 7.71 dB
down at its shipped default against every source available.

**THE HEADLINE: §36 holds, and that is what the lineup-wide shape bought.** The
RAT measures exactly the −11.61 dBFS §66.3 measured for the one-pedal version, and
still holds **rank 2 of 5** at the shipped defaults — the same rank it had before —
where fixing it alone would have put it at rank 5. Absolute level was never the
property; rank was.

**Three things came out differently from the plan.**

1. **`testLevelLinearity` was in three files; the linear map was asserted in
   SIX.** `test_muff_model.cpp`'s `testVolumeLinearity`, `test_gold_model.cpp`'s
   "OUTPUT is a linear pot (house convention)" and — found only when the web suite
   ran — `web/tests/audio.spec.ts`'s "LEVEL scales RMS" all carried it. All six
   rewritten so the band EXCLUDES the linear map rather than accommodating it. A
   seventh clause, `cab.spec.ts`'s declick "the batch landed" check, was a relative
   level proxy and is now an absolute one (the bypassed input's own RMS).
2. **The SD-1 could not be sourced at all** and is the one reconstruction here.
   No netlist, no parts list, and the search extracts contradict each other, so no
   letter was invented for it (§57's rule): it carries the TS's law through the
   shared `OverdriveEngine`, exactly as it has always carried the TS's tone stack —
   now stated as an assumption and guarded by a test that goes red if the two
   silently diverge.
3. **The acceptance bar as written is NOT met, and the reason is the defaults.**
   The spread at shipped defaults goes 4.5 → 8.0 dB. At a COMMON output position it
   was 6.40 dB before at every position and is 6.40 dB at OUTPUT 1.0 after — the
   shipped defaults were a level calibration expressed in linear-pot coordinates,
   and changing the law invalidates the coordinates, not the calibration. Recorded,
   not snugged; re-deriving the five defaults is the named follow-up and §67.5
   already carries the solved values.

## Measured results

| in Vpk | rat | sd1 | ts | muff | gold | spread |
|---|---|---|---|---|---|---|
| 0.15 V BEFORE | −6.40 | −7.97 | −9.19 | −4.73 | −8.38 | 4.5 dB |
| 0.15 V AFTER | **−11.61** | −16.44 | −16.50 | −14.85 | −8.54 | 8.0 dB |

The BEFORE row is row-for-row identical to §66.2's, which is the scope check.
Per-pedal change at the shipped default: rat −5.21 · sd1 −8.46 · ts −7.31 ·
muff −10.13 · **gold −0.16**.

Half-rotation delivered, against the 10–20 % audio-taper band: rat **11.920 %** ·
sd1/ts **11.391 %** · muff **11.920 %** · gold **48.976 %** (linear, by design).

**OUTPUT 1.0 is BIT-IDENTICAL on all five** (FNV-1a render hashes against a
pristine build of merged `main`). The RAT's factor-1 drift guard was regenerated
and every one of its seven samples moved by the same **0.737965** = the analytic
`audioTaper(0.9)/0.9`, which is the proof that only the pot moved.

Core ctest **35 → 34 entries**, repo ledgers **6 → 5**
(`rat-level-pot-linear-not-log` XPASSed → deleted → hard).
**14 perturbations, all RED on the named bar, every restore GREEN** (§67.7).

Goldens: four CHANGED, `clean120_chorus` UNCHANGED at −0.00.
**NOTHING BLESSED** — see Status.

## Files created / modified

- `core/include/clipper/dsp/OutputPotTaper.h` — **new.** Every pedal's law, its
  source and its provenance grade, header-only.
- `core/src/dsp/{RatModel,OverdriveEngine,MuffModel,GoldModel}.cpp` — the four
  call sites; `core/src/dsp/{SdModel,TsModel}.cpp` — the two configs.
- `core/include/clipper/dsp/OverdriveEngine.h` — the law in `OverdriveConfig`, so
  the SD-1 can move alone when it is sourced.
- `core/tests/test_{rat,sd,ts,muff,gold}_model.cpp` — five rewritten bars;
  `test_player_expectations.cpp` — the new A5 lineup-staging bar + the Muff's A4
  window shifted 10 dB; `core/CMakeLists.txt` — the RAT ledger registration off.
- `docs/DEVELOPMENT.md` §67, `CLAUDE.md`, this file.

## Deferred to next session

1. **Re-derive the five shipped OUTPUT defaults** on the new laws — §67.5 carries
   the solved knob positions (rat 0.9454 · sd1 0.9319 · ts 0.9461 · muff 0.8753 ·
   gold 0.7124 reproduce the old delivered gain exactly). Deliberately NOT bundled
   here: it is a second concern with its own bless, and mixing it into these
   goldens would leave the owner unable to attribute the drift.
2. **Find the SD-1 schematic** — the one reconstruction in this slice.
3. The two un-normalised insertion losses (Muff −0.341 dB, GOLD −0.519 dB), which
   are absolute-level facts kept out of a knob-law slice.
4. Confirm the GOLD's taper letter against a schematic rather than a published
   analysis.

## Status

- [ ] In progress
- [x] **Complete.**
- [ ] Partial — see deferred

The plan predicted this slice could not finish without the owner, and that was
right: the four goldens were presented as a measured `--golden-report` table with
nothing written, and **the owner authorized the bless explicitly on 2026-08-10
("bless and merge")**. The script's gates were then run by hand — clean tree,
report re-checked against the approved figures immediately before writing,
justification into `GOLDENS.md` in the same commit — the documented 2026-07-31
precedent for an environment with no `/dev/tty`.

**The bless's first attempt aborted, and the abort was correct.** The golden
write-back check turned out to be unsound for quiet renders (docs §67.11): it was a
per-third-octave BAND proxy, and this slice's level drop pushed `sd1_twin_reverb`'s
reverb tail close enough to the 16-bit floor to trip it. Fixed in its own commit
first — the round-trip is now measured per SAMPLE (1.51–1.59 LSB on all five,
level-independent), `kQuantizationFloorDb` untouched so UNCHANGED/CHANGED keeps its
resolution, and perturbation-proven.
