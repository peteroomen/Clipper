# JCM800 GAIN taper — breakup onset 20 → 30, top of the knob pinned

**Date:** 2026-07-31
**Branch:** claude/jcm-gain-taper-6f557i
**Roadmap item:** owner feedback round 3 (Drive doc "Clipper Feedback", 2026-07-31):
"Breakup is still slightly early. 20 sounds like what I want 30 to sound like. I like
the total saturation though, 100 is perfect right where it is." Post-#21/#23 re-test at
unity trim; chugs and GAIN 100 confirmed right ("sounds the same as Logic Pro's JCM").

## Goal

The GAIN knob's breakup onset moves from ~20 to ~30 with GAIN 1.0 bit-identical —
a taper-law reshape only, no circuit change.

## Approach

Deliberate tone change (knob-feel), judged by a measured THD-vs-knob curve. The GAIN
pot's audio taper is `audioTaper(x) = (e^{kx} − 1)/(e^k − 1)` with `k = 4`
(Jcm800Preamp.h, exposed for tests). k was calibrated before the finding-7/8 + bright
cap work un-starved the drive path, so the same knob now reaches breakup earlier —
the constant is stale, not wrong-in-kind. Raising k pushes the low knob quieter while
`audioTaper(1) = 1` keeps the top EXACTLY pinned by construction.

Derivation is by measurement, not by picking a k that "feels right": sweep the
composed amp's THD vs knob (220 Hz, the §45 probe level, unity-trim-equivalent
input), find the knob where THD crosses the onset bar (the same ≥ 5 % convention the
round has used), and choose the k that puts today's knob-0.20 drive at knob 0.30
(equivalently: solve audioTaper_new(0.30) = audioTaper_old(0.20) → k ≈ solve, then
verify the measured onset lands 0.30 ± 0.02 and the curve stays monotonic). MASTER
uses the same audioTaper — it must NOT change: the taper constant becomes per-pot
(kGainTaperK new, kMasterTaperK = 4 verbatim) unless measurement shows the master
feel unaffected either way; the owner's report is about GAIN only.

## Steps (for the implementing agent)

- [ ] `Jcm800Preamp`: split the taper constant — GAIN gets the re-derived k, MASTER
      keeps k = 4 byte-for-byte. Comment carries the derivation (old k, new k, the
      measured onset table) per house style.
- [ ] Measurement harness (scratch, not committed): THD vs knob 0.05…1.00 step 0.05
      through the composed Jcm800Amp at the §45 probe convention, before/after table
      printed. GAIN 1.0 render hash identical before/after (the pin).
- [ ] `core/tests/test_jcm800_*.cpp`: a perturbation-proven bar — onset (THD ≥ 5 %)
      at knob 0.28–0.34, and audioTaper(1.0) == 1.0 exactly; reverting k to 4 must
      fail the onset bar (record the perturbation in the plan/PR).
- [ ] Player-expectations A3/A4 jcm rows re-baselined if they move (GAIN default 0.5
      drive drops slightly — the documented rows must match the new measurement).
- [ ] `--golden-report` for rat_jcm800 (default GAIN 0.5 → the golden WILL move a
      little); table prepared for the owner, NO bless in the implementing agent —
      the verifier (main session) presents it.
- [ ] WASM rebuild (`bash scripts/build-wasm.sh`) + all three artifacts committed.
- [ ] Full core ctest; web build + Playwright; node suites. Docs §51 + CLAUDE.md
      entry drafted (verifier reviews before commit).

## How this will be measured

The before/after THD-vs-knob table (220 Hz composed-amp probe): onset 0.20 → 0.30 ±
0.02, monotonic, GAIN 1.0 bit-identical by render hash, MASTER law byte-identical.
Golden delta table for rat_jcm800 presented to the owner before any bless.

## Manual test steps

- [ ] Owner: breakup begins ≈ 30 (was 20); 100 unchanged; sweep still smooth
- [ ] Edge: GAIN 0 still fully clean; knob 0.5 slightly cleaner than before (expected
      — the default sits between onset and saturation)

## Out of scope for this session

Any circuit change (the preamp/PI/power are validated by the Logic A/B), MASTER
taper, the other round slices.

---

## What actually happened

Implemented as planned — one constant split into two, no circuit change — with one
honest correction to the plan's premise, found by measuring before choosing anything.

**The plan assumed the measured onset was already at knob 0.20. It is not, at the
docs §45 probe level (220 Hz, 0.10 V peak, MASTER 0.5): it measures 0.2717.** The §45
table itself already implied this (GAIN 0.3 → 5.07 % post-Ra2). So the "solve
audioTaper_new(0.30) = audioTaper_old(0.20)" derivation and the "measured onset lands
at 0.30 ± 0.02" acceptance number are not the same requirement at that probe level.

Resolved by measuring the onset against input level FIRST, before any constant was
chosen: the owner's reported "breakup at 20" reproduces exactly at **0.15 V peak**
(onset 0.2057), an ordinary unity-trim pickup level. The §45 0.10 V probe is a quieter
reference reading the same amp at 0.2717. The shipped constant is therefore derived
from the owner's sentence (the literal remap), and BOTH probe levels are reported:

- at 0.15 V (the field-report anchor): onset **0.2057 → 0.3064** — meets 0.30 ± 0.02
- at 0.10 V (the §45 reporting convention): onset **0.2717 → 0.3764**

The alternative — picking k so the 0.10 V onset lands on 0.30 — was rejected because it
delivers only −1.6 dB at knob 0.20, roughly a quarter of what the report asks for.

Because the GAIN pot's wiper is the ONLY thing the knob controls (the §47 bright-cap
coefficients are rebuilt from that same wiper), the remap is exact end to end, which
gave a stronger proof than a THD bar: the new GAIN 0.30 render reproduces the pre-slice
GAIN 0.20 render to −147.0 dBFS. `taper(1) = 1` for any k pins GAIN 1.0 by construction
and the render hash confirms it bit-identical.

## Measured results

**k:** `kGainTaperK = 5.0521652926683824`, the bisected root of
`gainTaper(0.30) == audioTaper_{k=4}(0.20) = 0.022865358743438216` (residual 4.6e-16
relative; the shipped constant measures 1.041e-17 absolute in-test).
`kMasterTaperK = 4.0` — MASTER untouched, IEEE-754 bit fingerprint over 101 knob
positions `572cafc287be552a` before AND after.

**THD vs GAIN — composed `Jcm800Amp`, 220 Hz, 0.10 V peak (§45 probe), MASTER 0.5,
BASS/MID 0.5, TREBLE 0.6, PRESENCE 0.5, 48 kHz, 4x, THD = harmonics 2..8, out dBFS is
settled-half RMS:**

| knob | THD before | out before | THD after | out after |
|------|-----------|-----------|-----------|-----------|
| 0.05 |  0.714 % | −45.70 |  0.434 % | −52.67 |
| 0.10 |  1.347 % | −38.79 |  0.727 % | −45.49 |
| 0.15 |  2.151 % | −34.38 |  1.109 % | −40.77 |
| 0.20 |  3.178 % | −31.00 |  1.613 % | −37.04 |
| 0.25 |  4.425 % | −28.20 |  2.283 % | −33.85 |
| 0.30 |  5.731 % | −25.80 |  **3.178 %** | **−31.00** |
| 0.35 |  6.923 % | −23.72 |  4.324 % | −28.40 |
| 0.40 |  8.131 % | −21.89 |  5.612 % | −26.02 |
| 0.45 |  9.700 % | −20.27 |  6.853 % | −23.84 |
| 0.50 | 12.182 % | −18.91 |  8.153 % | −21.86 |
| 0.55 | 16.422 % | −17.94 |  9.955 % | −20.07 |
| 0.60 | 22.159 % | −17.38 | 13.136 % | −18.60 |
| 0.65 | 27.872 % | −17.06 | 18.871 % | −17.65 |
| 0.70 | 32.960 % | −16.85 | 25.823 % | −17.16 |
| 0.75 | 37.385 % | −16.71 | 32.166 % | −16.88 |
| 0.80 | 41.201 % | −16.60 | 37.596 % | −16.70 |
| 0.85 | 44.449 % | −16.50 | 42.162 % | −16.57 |
| 0.90 | 47.137 % | −16.42 | 45.899 % | −16.46 |
| 0.95 | 49.233 % | −16.33 | 48.777 % | −16.35 |
| 1.00 | 50.599 % | −16.22 | **50.599 %** | **−16.22** |

Monotonic: YES before, YES after. Onset (THD ≥ 5 %, bisected): **0.2717 → 0.3764**.
The bolded 0.30 row after IS the 0.20 row before, to every printed digit; the 1.00 row
is bit-identical.

**Same probe at 0.15 V peak (the owner-report anchor level) — the acceptance bar:**

| knob | 0.05 | 0.10 | 0.15 | 0.20 | 0.25 | 0.30 | 0.35 | 0.40 | 0.45 | 0.50 |
|---|---|---|---|---|---|---|---|---|---|---|
| before | 1.07 | 2.04 | 3.30 | 4.82 | 6.30 | 7.63 | 9.15 | 11.46 | 15.54 | 21.54 |
| after  | 0.65 | 1.09 | 1.68 | 2.46 | 3.51 | 4.82 | 6.20 |  7.50 |  9.04 | 11.51 |

| knob | 0.55 | 0.60 | 0.65 | 0.70 | 0.75 | 0.80 | 0.85 | 0.90 | 0.95 | 1.00 |
|---|---|---|---|---|---|---|---|---|---|---|
| before | 27.65 | 33.04 | 37.66 | 41.58 | 44.84 | 47.46 | 49.43 | 50.68 | 50.90 | 49.12 |
| after  | 16.24 | 23.26 | 30.13 | 36.04 | 40.98 | 44.99 | 48.07 | 50.17 | 50.98 | 49.12 |

Onset: **0.2057 → 0.3064** (bar 0.30 ± 0.02 → MET; test band [0.28, 0.34] → MET).

**Pre-slice onset vs input level (measured before k was chosen):** 0.05 V → 0.4056,
0.075 → 0.3243, 0.10 → 0.2717, 0.15 → 0.2057, 0.20 → 0.1651, 0.25 → 0.1373,
0.30 → 0.1170, 0.40 → 0.0893, 0.50 → 0.0713.

**Pins:**
- GAIN 1.0 render fingerprint `484ed0c37929dc69`, RMS −16.223011 dBFS, THD 50.5993 % —
  IDENTICAL before and after. GAIN 0.0 `55aacd9676fb8497`, identical.
- MASTER law bit fingerprint `572cafc287be552a` — identical.
- Remap fidelity: pre-slice GAIN 0.20 vs post-slice GAIN 0.30, 24000 samples:
  max |Δ| = 4.47e-08 = **−147.0 dBFS absolute / −119.6 dB relative to peak**.
- Wiper drop vs the k = 4 law: −6.98 dB @ 0.05, −6.17 @ 0.20, −5.54 @ 0.30,
  −4.14 @ 0.50, −2.56 @ 0.70, −0.87 @ 0.90, **0.00 @ 1.00**.

**Test bar (`testGainTaperOnset`, new, `test_jcm800_power.cpp`, 48 kHz):**
THD 4.27 % @ knob 0.28 (bar < 5) / 5.93 % @ 0.34 (bar ≥ 5) / 11.51 % @ 0.50 /
36.04 % @ 0.70 (monotone); `gainTaper(0.30) − audioTaper(0.20)` = 1.041e-17.
**Perturbation proof** (`kGainTaperK` → 4.0, `touch`ed after both patch and restore):
THD **7.10 %** @ 0.28 and

    Assertion `tLo < 0.05 && "JCM800 GAIN breaks up EARLIER than knob 0.28 (taper
    too hot)"' failed.   (exit 134)

Restored → green again (4.27 % / 5.93 %).

**Player-expectations rows re-baselined** (comments only, no bar moved): A1 defaults
−33.5 → −37.6 dBFS, peak 0.126 → 0.095; A3 GAIN THD 0.2→10.8→48.5 → **0.2→6.9→48.5**;
A3 MASTER level −240→−18.4→−8.9 → **−240→−21.9→−7.5**; A3 treble −30.3→−15.7 →
−29.3→−14.1; the LEVEL-knob top-half reference jcm master +9.5 → **+14.4 dB**;
A4 default-rig delta +1.7 → **−2.5 dB** (window −10..+10 unchanged, still inside).

**Golden report (`--golden-report`, nothing written):**

    GOLDEN-DELTA rat_jcm800      CHANGED   -1.08 dB RMS, worst band 4.50 dB @ 2016 Hz (11 bands)
    GOLDEN-DELTA sd1_twin_reverb UNCHANGED +0.00, 0.02 @ 3200
    GOLDEN-DELTA muff_twin       UNCHANGED +0.00, 0.01 @ 252
    GOLDEN-DELTA ts_ac30         UNCHANGED +0.00, 0.00 @ 2016
    GOLDEN-DELTA clean120_chorus UNCHANGED -0.00, 0.11 @ 252

NOT blessed — that is the owner's call. `clipper_player_expectations_tests` is
therefore RED at the ±1.0 dB RMS gate, by design; the other 24 core targets pass.

**Gates:** core ctest **24/25** (only `clipper_player_expectations_tests` fails, at the
unblessed golden; 4 XFAIL ledgers Skipped as usual) · `cd web && npm run build` PASS ·
`npx playwright test` **71/71 PASS** (the amp-level drift guard needed no re-centring) ·
`npm run test:server` 15/15 · `test:history` PASS · `test:scripts` PASS ·
`cd electron && npm test` 20/20 · `node web/scripts/check-artifact.mjs` PASS after the
WASM rebuild.

## Files created / modified

- `core/include/clipper/dsp/Jcm800Preamp.h` — `kGainTaperK` / `kMasterTaperK`, the new
  `gainTaper()` declaration, the derivation + measured onset table in the comment
- `core/src/dsp/Jcm800Preamp.cpp` — shared `taperLaw(x, k)`; `audioTaper` = MASTER
  (k = 4, byte-identical), `gainTaper` = GAIN; `gainInterstageScale()` uses `gainTaper`
- `core/tests/test_jcm800_power.cpp` — new `testGainTaperOnset` (onset bracket + law
  properties), registered in `main`
- `core/tests/test_jcm800_preamp.cpp` — the two GAIN-wiper analytic sites now use
  `gainTaper`
- `core/tests/test_player_expectations.cpp` — reference-row comments re-baselined
- `web/public/generated/clipper.js`, `clipper-processor.js`, `.build-stamp.json` —
  rebuilt (`bash scripts/build-wasm.sh`)
- `docs/DEVELOPMENT.md` — new §51
- `CLAUDE.md` — Current State entry
- Scratch (NOT committed): `jcm_taper_probe.cpp` (sweep / onset bisection / render
  fingerprints / raw-render diff), `onset_level.cpp`, `solvek.cpp`

## Deferred to next session

- **The `rat_jcm800` bless.** Owner decision, with the table above. Until then the core
  suite's golden assert is red on this branch.
- **A pre-existing staleness found in passing, NOT fixed here (out of scope):**
  `test_player_expectations.cpp`'s A1 comment still says every level pot "kills to
  −240", but the JCM's GAIN at 0 has measured **−93.5 dBFS** since §47 — the bright-cap
  network's DC divider is `Rl/(Rl+Rs+Ru)` with `Rl = max(wiper, 1e-4)·1M`, so wiper 0
  is 6.8e-5, not 0. Unchanged by this slice (GAIN 0.0 render hash identical), but the
  comment and possibly the intent want a look.
- The §45 XFAIL `finding8-jcm-even-harmonic-cancel` is untouched (12.7 dB of 20).

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
