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

- [ ] Baseline table: all five pedals, shipped defaults, before any edit
- [ ] Per pedal: source the pot law from a netlist; record sourced vs assumed
- [ ] Apply; re-measure the spread
- [ ] **Rewrite `testLevelLinearity` in all three test files** — `test_rat_model.cpp:509`,
      `test_ts_model.cpp:279`, `test_sd_model.cpp:314`. §66 flagged these
      explicitly: they **assert the linear map**, so they are drift guards on a
      known approximation and the fixing slice must rewrite them, not delete them
- [ ] Delete the `rat-level-pot-linear-not-log` XFAIL — an **XPASS is a hard
      failure**, so it must go in this slice (ctest 35 → 34, ledgers 6 → 5)
- [ ] `--golden-report` table; stop and hand it to the owner (see below)
- [ ] WASM artifact rebuild — **last step**, and re-run it if `main` moved
- [ ] Perturbation-prove every new bar

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

## Measured results

## Files created / modified

## Deferred to next session

## Status

- [x] In progress
- [ ] Complete
- [ ] Partial — see deferred
