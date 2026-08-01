# RAT polish — output level staging + LM308 slew rate

**Date:** 2026-08-01
**Branch:** claude/rat-polish-6f557i
**Roadmap item:** §36's named follow-up ("downstream level staging") + the open
"RAT polish slice: output level trim + LM308 slew-rate research" item.
**Docs section:** §66. **ADR:** 027 if needed.

## Goal

Answer two named items with measurements rather than adjectives:

1. **Is the RAT mis-staged** relative to the other dirt pedals at realistic input, now
   that §36 raised its clipping ceiling ~5 dB? If yes, fix it **from the circuit**;
   if no, say so with the numbers and change nothing.
2. **The LM308 slew rate** — is it modelled, is its value sourced, and does it do
   anything audible at realistic guitar levels? Model it from a datasheet figure, or
   document it as deliberately not modelled. Both outcomes acceptable.

Everything measured **before** anything is believed. This slice is expected to be
**fidelity-neutral** unless a measurement forces otherwise; if a tone change ships,
`rat_jcm800` moves and is reported un-blessed.

## Approach

**Research channel.** Same as §57/§59: `WebFetch` 403s on every host tried (ti.com
mirror, mit.edu, onsemi, studylib, direnc, electrosmash DNS-fails). Reachable:
`WebSearch` result summaries, and **github.com over git**. The RAT is in
`Cushychicken/ltspice-guitar-pedals` as `proco-rat-distortion.asc` — a full LTspice
netlist with the pot values, tapers and the op-amp compensation cap. That is the
primary source for this slice; every component claim below is traced to it.

**Part 1.** Render all five dirt pedals at their shipped defaults at the two house
probe levels (0.15 V unity-trim pickup, 0.10 V §45 convention) plus 0.30 V, at
48 kHz, and tabulate out RMS / peak / THD / gain. Repeat the RAT with §36's diode
reverted so the question "did §36 break the staging?" is answered both sides of the
fix. Then audit `RatModel`'s stage 3 against the netlist's output section.

**Part 2.** The premise ("this model almost certainly does not model it") is checked
first: §11.4 / M6.5 shipped `LM308Stage` with a slew clamp. So the questions become
(a) is 0.3 V/µs sourced or fitted, (b) does the clamp engage at realistic levels, and
(c) can any test fail if the constant changes. Instrumented scratch build counts
clamp engagements per stimulus; a per-value A/B build (0.15 / 0.3 / 0.6 / ∞ V/µs)
measures what the constant is worth audibly.

## Steps

- [x] Baseline: five-pedal staging table at 0.10 / 0.15 / 0.30 V
- [x] Baseline: pre-§36 RAT in the same table
- [x] Baseline: `--golden-report` (all five UNCHANGED on a clean tree)
- [x] Clone/read the RAT netlist; trace it node by node against `RatModel.cpp`
- [x] Source the LM308 slew rate + the RAT's compensation cap
- [x] Instrument the slew clamp; measure engagement per stimulus
- [x] A/B the slew constant at 0.15 / 0.3 / 0.6 / ∞ V/µs
- [ ] Perturbation-check `testSlewRate` — can it fail if the model's constant moves?
- [ ] Ship whatever the measurements justify, and nothing else
- [ ] §66 + CLAUDE.md Current State; full test matrix; WASM artifact if `core/` changed

## How this will be measured

- **Staging:** out RMS dBFS / peak / THD at 220 Hz for rat/sd1/ts/muff/gold at
  defaults, at 0.10 and 0.15 V peak. "Mis-staged" means the RAT sits outside the
  spread its four siblings define. (Scratch `stage_probe`.)
- **Slew engagement:** percentage of oversampled samples at which the clamp binds,
  and the demanded vs delivered dV/dt in V/µs, per stimulus (scratch instrumented
  `LM308Stage`).
- **Slew audibility:** RMS, peak, spectral centroid, >5 kHz band energy and attack
  peak, at four values of the constant, same binary otherwise.
- **Goldens:** `--golden-report` five-row table (report only; **no blessing** — that
  is the owner's call and out of scope).
- **Teeth:** every bar added or moved is perturbation-proven (patch → red, restore →
  green, `touch` after both).

## Manual test steps

- [ ] `ctest --test-dir build --output-on-failure` (exit code read UNPIPED)
- [ ] `cd web && npm run build` + Playwright; node suites; native tests + the
      `Clipper` target (CI compiles the editor as of PR #49)
- [ ] Edge case: perturb `kOpAmpSlewVoltsPerSec` to the LM308H figure (0.15 V/µs) and
      to a fast op-amp (3 V/µs) — the suite must go red both ways
- [ ] Edge case: confirm no golden other than (possibly) `rat_jcm800` moves

## Out of scope for this session

- `OverdriveEngine` / `SdModel` / `TsModel` (a parallel slice owns them), and the
  `sd1_twin_reverb` / `ts_ac30` goldens.
- Blessing any golden.
- Any lineup-wide output-pot taper change (see "Deferred").
- The clipping-node load network (`kCp`) — a clipper change, its own slice.

---

## What actually happened

### Part 1 — the RAT is NOT mis-staged; §36 moved it INTO the pack, not out of it

Five dirt pedals at their shipped defaults, 220 Hz, 48 kHz, tail RMS/peak/THD:

| in Vpk | | rat | sd1 | ts | muff | gold |
|---|---|---|---|---|---|---|
| 0.10 | out dBFS | **−6.77** | −9.87 | −11.39 | −4.82 | −11.08 |
| 0.10 | THD % | 32.3 | 11.5 | 5.7 | 33.1 | 2.8 |
| 0.15 | out dBFS | **−6.40** | −7.97 | −9.19 | −4.73 | −8.38 |
| 0.15 | THD % | 34.9 | 16.9 | 10.3 | 33.9 | 4.0 |
| 0.30 | out dBFS | **−5.86** | −5.82 | −6.68 | −4.63 | −4.16 |

At the unity-trim probe the RAT sits **1.6 dB above the SD-1 and 1.7 dB below the
Muff**, inside a 4.5 dB lineup spread. At 0.30 V it is within 1.7 dB of every
sibling. There is no level outlier to fix.

The same table with §36's diode reverted (ideality 1.0, everything else shipped)
shows what the follow-up was actually worried about:

| in Vpk | rat PRE-§36 | rat SHIPPED | quietest sibling | loudest sibling |
|---|---|---|---|---|
| 0.10 | −11.49 | −6.77 | ts −11.39 | muff −4.82 |
| 0.15 | −11.17 | −6.40 | ts −9.19 | muff −4.73 |
| 0.30 | −10.80 | −5.86 | ts −6.68 | gold −4.16 |

**Before §36 the RAT was the quietest pedal in the lineup** (0.15 V: 2.0 dB below the
TS, 6.4 dB below the Muff). §36 did not knock it out of staging — it moved it from
the bottom of the pack to the middle. A compensating trim would put it back where it
was, and there is no measurement that asks for that. **No level change ships.**

### Part 1b — the netlist audit, and the one item that IS an approximation

`proco-rat-distortion.asc` traced node by node against `RatModel.cpp`:

| netlist | model | verdict |
|---|---|---|
| R8 560 Ω + C8 4.7 µF leg (60.5 Hz) | `kShapeLeg1Hz` 60.4692 | exact |
| R7 47 Ω + C7 2.2 µF leg (1539 Hz) | `kShapeLeg2Hz` 1539.2161 | exact |
| R9 gain pot 100 k **logarithmic** | DISTORTION linear-in-dB 0…+66 dB | the log pot's law, in dB |
| C1 30 pF op-amp compensation | GBW 1 MHz + 0.3 V/µs | see Part 2 |
| C9 100 pF across the gain pot | **not modelled** | masked — derived below |
| R10 1 k into the diode pair | `kRs` = 1 kΩ | exact |
| D2/D3 1N914 pair to ground | 1N4148 SPICE pair | §36 |
| (no cap at the clip node) | `kCp` = 10 nF "from the library example" | **fabricated pole** — named follow-up |
| R17 tone pot 100 k log + R15 1.5 k into C11 3.3 nF | one-pole LP, 500 Hz…20 kHz log | form right, range/law approximate |
| C12 22 nF/1 M, C13 1 µF/100 k, C10 4.7 µF/1 k | no high-passes at all | ≤0.75 dB at low E — documented |
| R14 volume pot 100 k **logarithmic** | `level.setTarget(knob)` — identity linear | **the real approximation** |

Two of those deserve numbers rather than adjectives.

**C9 (100 pF across the feedback pot) is masked at every knob position, by a constant
factor.** Its pole is `1/(2π·Rf·C9)` and the closed-loop corner already in the model
is `GBW/A ≈ GBW·(R7∥R8)/Rf`. Both scale as `1/Rf`, so their ratio is
`1/(2π·C9·GBW·(R7∥R8)) = 1/(2π·100p·1e6·43.3) = **36.7×**` — independent of the
DISTORTION knob. The component the model already has is 36.7 octave-less times lower
in frequency at every setting, so C9 can never contribute more than ~0.03 dB inside
the audio band. Not modelled, and now documented as a derivation rather than an
omission.

**The LEVEL pot is the one real approximation, and it is deliberately NOT fixed
here.** The netlist's own annotation reads `NOTE: R14 is volume pot (100k,
logarithmic)`, cross-checked by a parts-list extract ("100K-A pots for volume, tone
and distortion"), and it is the last element in the chain — driven by the 2N5458
source follower and loaded only by whatever follows, so its law is the bare taper.
`RatModel.cpp` maps it `level.setTarget(knob)` and has said so since M1: *"A proper
audio-taper volume law is a future refinement."* Measured consequence of fixing it
with the house `audioTaper` (k = 4, 12.15 % at half rotation — inside the 10–20 %
audio-taper spec §58 cites): the default LEVEL 0.8 would drop **5.04 dB**, putting
the RAT at −11.44 dBFS, i.e. **the quietest pedal in the lineup again** — exactly
where §36 found it. It is not shipped, for three reasons, all measured:

1. This slice's own measurement says the level is correct; shipping a −5 dB knob-law
   change on top of that reads as the compensating trim §36 forbade, arriving by a
   circuit-flavoured route.
2. **Every** pedal in the lineup carries the same approximation —
   `OverdriveEngine::setParameter` says `level_.setTarget(knob); // identity linear
   map, as the RAT`. Fixing one pedal's pot alone measurably breaks the parity this
   slice just established. It is a lineup-wide slice (five output pots at once), and
   two of those files belong to a parallel slice this session.
3. It moves `rat_jcm800`, i.e. costs an owner bless, for a change no measurement in
   this slice asks for.

Named as a follow-up with the numbers, not silently dropped.

### Part 2 — the slew rate IS modelled; the VALUE is right; its JUSTIFICATION was a fit

**The premise is refuted before the first measurement.** `LM308Stage` has shipped a
slew clamp since M6.5 (docs §11.4), at the op-amp output node, inside oversampling,
with a `reset()` entry, at **0.3 V/µs**.

**Provenance, and the ambiguity the sources actually contain.** The RAT's own netlist
carries `C1 = 30p` — the LM308's *standard* compensation, which is the condition the
0.3 V/µs figure is quoted under. Search summaries agree ("The LM308 has a slew rate
of 0.3 V/µs … around 40 times slower than the TL071"; "0.3 V/µs with standard
compensation, and the typical and suggested value by the datasheet for compensation
is 30 pF, same as used in the Rat design"). The competing 0.15 V/µs figure that
circulates attaches to the **LM308H** (the metal-can part), not to the LM308N the RAT
uses. **No datasheet PDF was reachable** — ti.com's mirror, mit.edu, onsemi, studylib
and direnc all returned 403 — so the provenance is search-summary-grade, and that is
recorded rather than dressed up.

The shipped constant therefore needs no change. What needed changing is the sentence
justifying it, which read: *"(0.15-0.3 V/us is the cited band; 0.3 keeps note attack
alive while still killing the razor edges — measured.)"* — a value picked from a band
by tonal outcome, i.e. the fit this project forbids. Replaced by the derivation (the
RAT's 30 pF compensation selects the standard-compensation figure) plus the LM308H
disambiguation and the measurement below, which shows the two candidate figures are
**audibly distinguishable on ordinary playing**.

**Does the clamp engage at realistic levels?** Instrumented, 4× at 48 kHz, shipped
0.3 V/µs, percentage of oversampled samples at which the clamp binds:

| stimulus (defaults unless noted) | clamp % | demanded dV/dt | delivered |
|---|---|---|---|
| 220 Hz 0.10 V | 0.000 % | 0.0158 V/µs | 0.0158 |
| 220 Hz 0.15 V | 0.000 % | 0.0237 | 0.0237 |
| 220 Hz 0.15 V, dist 1.0 | 0.000 % | 0.0774 | 0.0774 |
| 1 kHz 0.15 V | 0.000 % | 0.1026 | 0.1026 |
| low-E pluck 0.15 V | 0.000 % | 0.1117 | 0.1117 |
| **low-E pluck 0.30 V** | **0.000 %** | **0.2235** | 0.2235 |
| high-E pluck 0.15 V | 0.000 % | 0.2167 | 0.2167 |
| **high-E pluck 0.30 V, dist 1.0** | **1.257 %** | **21.33** | 0.3000 |
| **4186 Hz 0.15 V** | **92.97 %** | **2.4949** | 0.3000 |
| **4186 Hz 0.15 V, dist 1.0** | **97.84 %** | **6.8330** | 0.3000 |

So the LM308's slew limit is **dormant on low and mid notes at any realistic level
and dominant on high ones** — which is precisely the mechanism the sources describe
("upper frequencies zero-cross faster than the slew rate can keep up"). It is not a
decoration and it is not a wall: at 0.30 V of low E the demand reaches **74.5 % of
the limit** and never crosses it.

**What is it worth audibly?** Same binary, only the constant changed:

| case | 0.15 V/µs | **0.3 (shipped)** | 0.6 | ∞ (no clamp) |
|---|---|---|---|---|
| 220 Hz 0.15 V, rms dBFS | −6.403 | **−6.403** | −6.403 | −6.403 |
| 1 kHz 0.15 V, rms dBFS | −6.076 | **−6.076** | −6.076 | −6.076 |
| low-E pluck 0.30 V, rms | −30.577 | **−30.577** | −30.577 | −30.577 |
| 4186 Hz 0.15 V, rms | −9.150 | **−8.755** | −8.601 | −8.601 |
| 4186 Hz 0.15 V d1.0, rms | −9.150 | **−8.758** | −8.465 | −8.398 |
| 4186 Hz 0.15 V d1.0, centroid Hz | 3530 | **4563** | 4701 | 4697 |
| high-E pluck 0.30 V d1.0, attack peak | 0.5871 | **0.5886** | 0.5973 | 0.5989 |

Two honest readings. (1) On everything below ~1 kHz the constant is worth **exactly
zero** — the rows are identical to every digit printed. (2) Where it engages it is
**small but real**: 0.36 dB of level and 134 Hz of centroid at the high-E probe
against no clamp, and 1.7 % off a pick attack. The reason it is small is structural
and worth recording: the closed-loop bandwidth pole (`GBW/A` = 4.9 kHz at DISTORTION
0.7, 501 Hz at 1.0) has already removed most of the slope demand before the clamp
sees it, and the diode clamp behind it discards amplitude information — the two
LM308 behaviours are **in series, and the bandwidth one dominates**.

### The bar that could not fail — found, as instructed to assume

`testSlewRate` builds its **own** `LM308Stage` with local copies
`const double GBW = 1.0e6, SR = 0.3e6;` and asserts the clamp arithmetic. Nothing in
the suite reads `RatModel.cpp`'s `kOpAmpSlewVoltsPerSec`. Perturbation-proven below:
the constant can be changed to 0.15 or 3.0 V/µs and the whole core suite stays green.

Fixed with a model-level test built on a new measurement hook,
`RatModel::setSlewLimit(bool)` (the `setIdealOpAmp` pattern: not a user knob, not on
the C ABI). Three bars, all absolute, all through the shipped signal path.

## Measured results

**Goldens — `--golden-report`, the full five rows, measured AFTER the slice.**
No tone change shipped, so nothing moved and nothing was blessed:

```
GOLDEN-DELTA rat_jcm800       UNCHANGED +0.00  0.00   252  12
GOLDEN-DELTA sd1_twin_reverb  UNCHANGED +0.00  0.02  3200  12
GOLDEN-DELTA muff_twin        UNCHANGED +0.00  0.00  5080  12
GOLDEN-DELTA ts_ac30          UNCHANGED +0.00  0.00  2016   8
GOLDEN-DELTA clean120_chorus  UNCHANGED -0.00  0.11   252   7
```

(columns: delta dB RMS, worst band dB, that band's centre Hz, band index. The
identical table was measured on a clean tree before any edit — the two runs agree
row for row, which is the scope check.)

**The slew limiter, through the shipped model** (`setSlewLimit` A/B):

| stimulus | 44.1 kHz | 48 kHz | 96 kHz |
|---|---|---|---|
| low-E pluck 0.30 V, defaults — max abs diff | 0.0e+00 | 0.0e+00 | 0.0e+00 |
| low-E pluck 0.30 V, DISTORTION 1.0 | 0.0e+00 | 0.0e+00 | 0.0e+00 |
| 220 Hz / 1 kHz 0.15 V | 0.0e+00 | 0.0e+00 | 0.0e+00 |
| 4186 Hz 0.15 V, dist 1.0 — level cost | 0.343 dB | 0.360 dB | 0.354 dB |

**Perturbations** (all `touch`ed after patch AND restore): P1 slew → 0.15 V/µs
**RED**; P2 → 0.6 **RED**; P3 → 16 (a TL071) **RED**; P4 `setSlewLimit` neutered
**RED**; P5 LEVEL → the house audio taper **RED** (and P5b, with the drift guard
relaxed, **XPASSes** the new XFAIL, which fails the suite — the ratchet working).
Restore green each time, including the full `ctest`.

**The bar that could not fail:** `testSlewRate` stayed green at both 0.15 and
3.0 V/µs. Only `testFactorOneRegression` — a hardcoded-sample drift guard §36
labels as such, and regenerates whenever the clipper legitimately changes — noticed.

**Suites.** Core `ctest` **35 → 36 entries, 100 % passed, 0 failed out of 36** (29 real tests
+ 7 skipped ledgers; the new entry is `clipper_rat_tests_xfail_ledger`) — exit code read
unpiped from a backgrounded run. `npm run build` green (tsc + vite). Node suites
15 / 10 / 12, electron 20. Native `ctest -R 'clipper_identical_core|clipper_chain_edit|clipper_cab_state'`
**3/3**, and `cmake --build native/build --target Clipper` links clean (the editor
compiles).

**Playwright: 67 passed, 7 flaky, 9 failed — environmental, and PROVEN so.** Every
single failure is the specs' own harness gate firing with the message *"a render
came back silent (Chromium OfflineAudioContext flake)"* — never a wrong number. The
failing set differed between runs, the RAT spec (the only one this slice could
affect) passes in isolation, and the decisive check: with **origin/main's own
`clipper.js` swapped in** (via scratch copies, no `git checkout --`), the delay and
comp specs fail **identically**. Pre-existing, load-related, documented in
`playwright.config.ts` itself.

## Files created / modified

- `core/src/dsp/RatModel.cpp` — the netlist audit as a header comment, the
  slew-rate justification un-fitted, the `slewLimit` flag + `opAmpSlewRate()`
- `core/include/clipper/dsp/RatModel.h` — `setSlewLimit` / `slewLimit`
- `core/tests/test_rat_model.cpp` — `testSlewInModel` (new), `testSlewRate`
  re-scoped, `testLevelLinearity` labelled + the new XFAIL, ledger wiring
- `core/CMakeLists.txt` — `clipper_add_xfail_ledger(clipper_rat_tests)`
- `web/public/generated/clipper.js` + `.build-stamp.json` — rebuilt (94 inputs)
- `docs/DEVELOPMENT.md` §66, `CLAUDE.md` Current State, this file

**NOT touched**, deliberately: `core/tools/render/main.cpp` (a `--no-slew` flag was
considered and dropped — the file is being edited by a parallel slice and the hook's
purpose here is the test), `OverdriveEngine`/`SdModel`/`TsModel`, any golden.

## Deferred to next session

- **Output-pot tapers, lineup-wide.** All five dirt pedals map LEVEL identity-linear;
  the RAT's netlist says 100 kΩ log, and the SD-1/TS engine says "as the RAT". One
  slice, five pots, one owner bless — not one pedal at a time. Numbers above.
- **`kCp` = 10 nF is a fabricated pole.** The netlist has no cap at the clipping node;
  the real load is `R17(log 100 k) + R15 1.5 k` into `C11 3.3 nF`, i.e. the tone
  network loads the clip node instead of following it. Replacing it moves the diode
  drive above ~10 kHz, the alias floor and the `DiodeClipperADAA` calibration
  together — a §54-shaped clipper slice, with `testAdaaTracksWdf` re-tied in it.
- **FILTER law.** The model log-sweeps the *cutoff* (500 Hz…20 kHz); the circuit
  log-sweeps the *resistance* (475 Hz…32.15 kHz). Worth ~1.4 dB of corner at noon.
- **Output high-passes** (C10 4.7 µF/1 k = 33.9 Hz, C12 22 nF/1 M = 7.2 Hz,
  C13 1 µF/100 k = 1.6 Hz): together ≤0.75 dB at low E. Cheap, but a tone change with
  no complaint behind it.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
