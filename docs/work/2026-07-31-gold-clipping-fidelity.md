# GOLD clipping-stage fidelity — fix the three errors the summing weight was hiding

**Date:** 2026-07-31
**Branch:** claude/gold-fidelity-6f557i (stacked on claude/gold-summing-6f557i, draft PR #31)
**Roadmap item:** owner decision 2026-07-31 ("Full fidelity slice") on the §52 finding:
the schematic-faithful summing weight (4.1702, derived, kept) exposed that the old fit
was compensating three named departures from the real circuit. Field target (owner):
edge of breakup well past knob 5; max = creamy/crunchy, NOT Marshall; GAIN 0 transparent.

## Goal

The three §52-named errors fixed against the reference (the cloned Chowdhury
KlonCentaur netlist + DAFx-19 paper + schematic figures, already validated by
reproducing the §50 gang law), with the derived summing weight retained:

1. **The diode node** — the reference's measured clipper fit (Is = 15 µA,
   Vt = 25.85 mV — a 0.109 V soft knee) behind the schematic's **R13 = 1 kΩ** source
   resistance (ours ran a 0.286 V knee behind 2.2 kΩ — ~2.1× hot at the node).
2. **The 495 Hz summing pole** (R20 ∥ C13 = 392 kΩ ∥ 820 pF) — the "creamy" filter —
   modeled TOGETHER with whatever clean-path normalization keeps the GAIN-0 contract
   (the pole alone carves −14 dB of mids from the clean path; the real unit's
   following stages sit after it too).
3. **Gain-dependent drive shaping** — C7 = 82 nF across R10b makes the drive amp's
   gain frequency- AND knob-dependent (1 kHz gain ~100× at g = 1 vs the 25.8× DC
   law); ours is flat.

The §52 XFAILs (`gold-summing-rails-engage`, `gold-summing-alias-at-treble-max`) name
this slice as their fix: they must XPASS → delete → harden, or their measured numbers
must improve with an honest re-owned decl.

## Approach

Deliberate fidelity change judged against the reference implementation itself: the
agent can RENDER the reference topology's transfer curves from the netlist values and
compare ours stage-by-stage (drive node level, diode node voltage, post-summing
spectrum) — the strongest oracle this pedal has ever had. Contracts that stand:

- **GAIN-0 transparency**: the composed clean path at GAIN 0 / TREBLE noon /
  OUTPUT 0.5 stays unity-flat. Prefer bit-exact by construction (normalize the
  clean-path composition so H(noon) ≡ the §27 clean response); if the faithful
  topology cannot deliver bit-exact, the fallback contract is |H| within 0.25 dB
  20 Hz–10 kHz with the decision documented ADR-style — do NOT silently change what
  "transparent" means.
- **The Ge/Si counterfactual**: refitting the germanium moves the Ge-vs-Si contrast;
  re-derive kSiIdeality's counterpart honestly (the property is the CONTRAST, ~6 dB
  at high drive — keep the property, re-derive the probe, §50 discipline).
- The drive gang law A(g) at DC stays the §50 schematic law by construction (C7 adds
  the frequency dependence on top; the DC law is the identity check).

Field acceptance (0.15 V anchor, 220 Hz): near-clean at knob ≤ 0.10; audible-grit
onset mid-knob or later; max THD inside the reference 15–25 % band WITH the summing
pole's spectral tilt present (measure the dirt's 3 kHz-vs-500 Hz ratio before/after —
"creamy" is a measurable darkening); output level at max within a few dB of the §50
level (the +13.3 dB overshoot must come back down as the diode node softens).
HONESTY GATE: if the faithful trio still misses the percept, ship faithful + report
the gap with stage-by-stage tables against the reference render.

## Steps (for the implementing agent)

- [ ] Start from origin/claude/gold-summing-6f557i (fetch + branch); read §52's code
      comments, draft PR #31's description, the cloned reference (re-clone if needed)
- [ ] Stage-by-stage comparison harness FIRST (ours vs reference-derived curves):
      drive-node |H| at 3 knob points, diode-node clip curve, post-summing spectrum
- [ ] Fix 1: diode fit + R13; Fix 3: C7 shaping (netlist zero/pole); Fix 2: the
      summing pole + clean-path normalization per the contract above
- [ ] Re-measure EVERYTHING §52 tabled (THD/level/dirt-ratio at both input levels)
      plus the creamy-tilt metric; before/after/reference three-way tables
- [ ] XFAILs: rails-engage + alias-shadow re-measured → XPASS→harden or re-own
      honestly; knee/contrast probes re-derived as needed (node-voltage arguments)
- [ ] Perturbation-proven bars for each of the three fixes (revert each alone → red)
- [ ] Player-expectations gold rows re-baselined; A4 window re-centred if needed
- [ ] `--golden-report` ZERO changed (GOLD in no golden rig; GAIN-0 contract);
      full core ctest green modulo ledgers; WASM rebuild; web build + Playwright
      (gold worklet spec: honest re-derivation if moved); node suites
- [ ] Docs §54 (§53 is the Muff slice, in flight) + ADR for the transparency-contract
      decision if the fallback fired + CLAUDE.md entry + plan bottom sections
- [ ] ONE commit on claude/gold-fidelity-6f557i, fix: …, tables in the body,
      standard trailers; NO push, NO PR, NO golden writes

## How this will be measured

The three-way tables (before / after / reference), the onset + THD + level + tilt
acceptance rows at the 0.15 V anchor, the GAIN-0 contract proof (hash or |H| table +
ADR), the XFAIL dispositions, and the owner's ear afterwards.

## Manual test steps

- [ ] Owner: GAIN 0 transparent; breakup onset well past 5; 100 creamy/crunchy;
      GOLD-as-boost into the JCM
- [ ] Edge: Ge/Si contrast preserved as a property; NaN/reset/rates; no zipper

## Out of scope for this session

The buffer/input stage, the OUTPUT pot law, all other pedals, the Muff slice
(running in parallel — do not touch BjtStage or MuffModel).

---

## What actually happened

All three fixes landed, against the cloned `KlonCentaur` netlist as the oracle. Full
write-up is **docs §54**; ADR **016** covers the one architectural decision. Highlights
and the things that were NOT anticipated by the plan:

1. **Diode node.** `Is` 200 nA → 15 µA, ideality 1.3 → 1.0, `Rs` 2.2 kΩ → `R13` = 1 kΩ,
   and — beyond the plan's letter — the node's own **`R16` = 47 kΩ load** added to the WDF
   tree, because that is how the reference builds it (`ResVs Vbias { 47000.0 }`) and it is
   the *same* 47 k whose transimpedance `R20/R16` §52 already uses as the summing weight.
   `kCp = 4.7 nF` was **kept** and re-labelled: with the new Thevenin source R13 ∥ R16 =
   979 Ω its corner is 34.6 kHz, i.e. an out-of-band anti-alias guard-rail rather than a
   stand-in for the reference's own HF limit (which fix 3 now carries exactly).
2. **The 495 Hz summing pole**, on the **dirt branch only**. The plan's preferred contract
   ("normalize the composed clean path") turns out to be *algebraically identical* to
   applying the pole to the dirt alone — `LP(clean·LP⁻¹ + dirt) == clean + LP(dirt)` — so
   the bit-exact option was available and was taken. The dirt path becomes exactly the real
   composed dirt transfer; the clean path keeps the flat-2.0 idealization §27 gave it. The
   0.25 dB fallback contract never fired, so ADR 016 documents the decision that *was*
   made rather than a relaxation.
3. **C7 = 82 nF across R10b**, via a new `AmpStageNetwork` — the reference's exact
   R10b/R11/R12/C7/C8 2nd-order transfer, bilinear-transformed, coefficients rebuilt at
   control rate. The ground leg is **recovered from the smoothed `A`**
   (`R10b + R11 = R12/(A−1)`) rather than from a second copy of the knob, so §50's gang law
   is the network's DC gain *by construction* — and that identity is bar (4) of the new
   test rather than a comment.

**Un-anticipated finding that changes how you write probes for this pedal:** the reference
germanium's `Is = 15 µA` gives a zero-bias incremental resistance of only
`Vt/(2·Is) = 861.7 Ω` (against 84.0 kΩ for the old pair), so it loads its own node by
6.78 dB at *any* level. The pair has no truly linear region — the "bloom" as a number.
Every small-signal bar in the suite is therefore a ratio, in which that loading cancels.

**Re-scoped, not changed:** `kDrivePreScale`/`kDriveHpHz`. §52 validated them against
`H_pre·H_amp/A(g)`; with `H_amp` now exact they stand for `H_pre` alone, and re-measured
against that target they are within ±1.3 dB across the core band (table in §54.7). Not
refitted — a fourth constant move would have had no isolated perturbation proof.

## Measured results

Three-way tables (before / after / reference) are in **docs §54.2–54.4**. Headlines:

- **Drive node, GAIN 1.00, error vs the reference:** 220 Hz −4.63 → **+0.68 dB**,
  500 Hz −9.59 → **+0.35**, 1 kHz −13.14 → **−1.33**. Worst error anywhere in the table
  13.1 → **3.6 dB**.
- **Diode node:** +5.7…+8.5 dB hot → **0.00 dB** (it *is* the reference network). Knee at
  1 mA 0.2862 → **0.1086 V**; rendered node ceiling 0.336 → **0.148 V**.
- **Summing (dirt) transimpedance:** flat 8.340 → `8.340·|pole|` = 8.228 (82 Hz) …
  **1.358 (3 kHz)**, i.e. the 12.7 dB of 3 kHz-vs-500 Hz tilt that makes it creamy.
- **Field anchor, 0.15 V / 220 Hz, TREBLE noon, OUTPUT 0.5** (THD %, out dBFS):
  GAIN 0.35 **5.84 → 4.59 %** / −5.34 → **−11.33 dBFS**; GAIN 1.00 **23.70 → 14.59 %** /
  −0.74 → **−7.90 dBFS**. Breakup onset 5 % **0.28 → 0.42**, 10 % **0.61 → 0.87**.
- **GAIN-0 contract: BIT-EXACT.** Four render hashes identical before/after
  (`85a97e9efc5686ba`, `d10d3ffca9077b36`, `449ef98662e22ec2`, `2217f25842819f17`).
- **`--golden-report`: all five UNCHANGED** (+0.00 / +0.00 / +0.00 / +0.00 / −0.00).

**Honesty gate, second reading — four of five acceptance rows land, one misses:**
grit onset mid-knob ✔ (5 % at 0.42), creamy tilt present ✔ (whole-pedal HF harmonics
−22.9 → −29.4 dB), near-clean below knob 0.10 ~ (2.86 % — inside the real unit's own
0.2–3.9 % range but not "clean"), max level back toward §50 ~ (−7.90 vs §50's −14.00 and
§52's −0.74), **max THD 14.59 % — 0.4 points UNDER the reference's 15–25 % band**.
Nothing was re-gained to close it.

### XFAIL dispositions — both XPASSed, both deleted, both hardened

| §52 XFAIL | measured after | now |
|---|---|---|
| `gold-summing-rails-engage` | 8.624 V → **1.274 V** (rail 8.600) | hard assert in `testHeadroomAndOutput` |
| `gold-summing-alias-at-treble-max` | 4× −26.5 → **−92.9 dB**, and 1× −27.3 / 8× −106.8 | hard, **two** bars in `testAliasing`: −60 dB AND ≥ 20 dB of 1×→4× improvement |

`clipper_add_xfail_ledger(clipper_gold_tests)` removed. **ctest 26 → 25, repo ledgers 5 → 4.**

### Perturbation transcripts (touch after BOTH patch and restore)

| # | perturbation | result |
|---|---|---|
| 1 | `kR13Ohms` → 2.2e3, `kGeIs` → 200e-9, `kGeIdeality` → 1.3 | **RED**, exit 134 — `testDiodeLevelContrast` "silicon does not clip far enough above germanium" (contrast 14.29 → **4.58–6.16 dB**). Probed separately, the diode-node bar also fails: node ceiling 0.1477 → **0.3186 V** vs the 0.10–0.22 band. Restore → **exit 0** |
| 2 | `const float dirt = d.sumPoleState;` → `= clipped;` (pole removed) | **RED**, exit 134 — `testClippingStageFidelity` "the 495 Hz summing pole (R20 \|\| C13) is missing" (3 kHz-vs-500 Hz −17.64 → **−4.93 dB**, bar −12). Restore → **exit 0** |
| 3 | `kAmpC7` → 1.0e-18 (C7 removed, network otherwise intact) | **RED**, exit 134 — `testClippingStageFidelity` "the drive amp's gain is not KNOB-dependent in frequency". Tilt at g = 0.15/0.35/0.75/1.00: 7.27 / 7.26 / 7.24 / **7.23 dB**, spread **−0.04 dB** vs the +8.07 dB with C7 (bar 5). Restore → **exit 0** |
| 4 | `kSiIdeality` → 1.0 (the §36 regression, to prove the re-derived Ge/Si band still has teeth) | **RED**, exit 134 — caught first by `testGermaniumKnee` (onset 139× → **3.8×**, bar 20×); the contrast band also fails at **8.51–10.22 dB** vs the 11.0 floor. Restore → **exit 0** |

### Gate results

- Core `ctest`: **25/25 passed, 0 failed**; 4 `_xfail_ledger` entries Skipped (gold's is gone).
- `--golden-report`: **all five UNCHANGED**, nothing written, no bless.
- `bash scripts/build-wasm.sh`: rebuilt; `sourceHash e6aa67d2f9f2…`, 65 inputs, 189 655 bytes.
  `check-artifact.mjs`: in step.
- `cd web && npm run build` (tsc --noEmit + vite): clean.
- `npx playwright test`: **71 passed**. The `gold worklet` spec passes unchanged — but its
  `pushedH3 > 0.1·pushedF1` bar now measures **0.1056**, i.e. 5.6 % of margin where it had
  ~2×. Left alone deliberately (lowering it would be loosening a bound to go green); it is
  now the tightest guard on this pedal and is flagged in §54.8.
- `npm run test:server` 15 / `test:history` 10 / `test:scripts` 12 / `cd electron && npm test` 20 — all pass.

## Files created / modified

- `core/src/dsp/GoldModel.cpp` — the three fixes + `AmpStageNetwork` + the sources block
- `core/tests/test_gold_model.cpp` — XFAILs deleted and hardened; new
  `testClippingStageFidelity` (4 bars); `testDiodeLevelContrast` band re-derived;
  `testGermaniumKnee` re-baselined
- `core/tests/test_player_expectations.cpp` — GOLD M11 rows re-baselined (A4 window
  deliberately NOT re-centred)
- `core/CMakeLists.txt` — `clipper_add_xfail_ledger(clipper_gold_tests)` removed
- `docs/DEVELOPMENT.md` — new §54
- `docs/decisions/016-gold-summing-pole-on-the-dirt-branch.md` — new ADR
- `web/public/generated/clipper.js`, `web/public/generated/.build-stamp.json` — rebuilt artifact
- `CLAUDE.md` — Current State entry

## Deferred to next session

- **The max-THD gap (14.59 % vs the reference band's 15 %).** Report, not fit. If it is to
  be closed it should be closed by whichever remaining approximation is actually wrong —
  the two candidates are below — never by re-gaining.
- **`kDrivePreScale` / `kDriveHpHz` refit** to `H_pre`'s real shape (one-pole HP, ~1105 Hz
  corner into a 0.93 shelf, vs the model's 600 Hz into 0.65). Worth ~2.5–3.6 dB at the band
  edges. Its own slice so it gets its own perturbation proof.
- **The FF1/FF2 clean feed-forward networks.** This is ADR 016's named cost: porting them
  ends the flat-clean idealization, and with it the bit-exact GAIN-0 contract. A product
  decision for the owner, not a modelling one.
- **The gold worklet spec's `pushedH3` bar** (0.1056 against 0.1). Expect to re-derive it in
  the next slice that touches this dirt path.
- The reference's ±4.5 V drive-amp output clip (this model documents ±8.6 V charge-pump
  rails and the diodes clamp long before either), and the OUTPUT pot law.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
