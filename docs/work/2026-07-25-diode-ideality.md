# Diode ideality factor — the RAT's silicon clipper reaches its real knee

**Date:** 2026-07-25
**Branch:** fix/diode-ideality
**Roadmap item:** `docs/audits/2026-07-24-project-audit.md` finding **15** (and the
GOLD half of it, `GoldModel.cpp` `kSiIdeality`)

## Goal

The WDF silicon diode pair in `RatModel` clips at the **±0.6–0.7 V** the code and
`docs/DEVELOPMENT.md` §6 both claim it does, because the 1N4148 SPICE model's
ideality factor `N = 1.752` is actually in the model instead of `1.0` — and every
constant that was fitted to the old, wrong ceiling (`DiodeClipperADAA::kDefaultVk`,
the GOLD silicon counterfactual) is re-derived from the corrected diode rather than
left pointing at the bug.

## Approach

**This is a deliberate TONE change, not a fidelity-neutral refactor.** The RAT will
be measurably *cleaner and louder at the same DISTORTION knob*, because the diode
clamp it runs into now sits ~5–6 dB higher.

`chowdsp::wdft::DiodePairT` takes `(Is, Vt, nDiodes)` and uses `nDiodes` purely as
`Vt_eff = nDiodes * Vt` (see the vendored `chowdsp_wdf.h`), which is arithmetically
identical to a diode ideality factor `n`. `GoldModel` already exploits exactly that
for its germanium pair (`kGeIdeality = 1.3`, documented as such) — so the fix is one
constant per site, not a topology change:

- `RatModel.cpp`: `DiodePairT<...> diodes { P1, kDiodeIs, kDiodeVt, 1.0 }` →
  a named `kDiodeIdeality = 1.752`.
- `GoldModel.cpp`: `kSiIdeality = 1.0` → `1.752`. The **germanium** side is
  correct (n = 1.3, knee 0.286 V at 1 mA matches its documentation) and is not
  touched — only the silicon *counterfactual* was wrong.
- `DiodeClipperADAA::kDefaultVk`: **re-derived by measurement** from the corrected
  WDF tree, not guessed and not left at 0.35. `kDefaultVk` exists to make the ADAA
  A/B path comparable to the WDF path; if it stays fitted to the bug the A/B
  silently compares two different circuits.

Explicitly **not** doing: any compensating pre-gain, input-trim or level change.
CLAUDE.md forbids calibrating a constant to absorb a change elsewhere
(`kFullScaleSecV` is the cautionary case), and §11.1 shows this pedal has already
been level-calibrated once. The voicing consequence gets **measured and reported**;
if the pedal now wants more pre-gain, that is a follow-up voicing slice with these
numbers as its input.

## Steps

- [ ] Baseline: measure the shipped static WDF curve (vin 1/10/100/600 V), the RAT
      at default knobs into a realistic 0.3 V-peak input (RMS, peak, THD), the drive
      knob position where clipping becomes audible, the WDF-vs-ADAA A/B, and the
      GOLD Ge-vs-Si contrast in dB. Record every number.
- [ ] `RatModel.cpp`: introduce `kDiodeIdeality = 1.752` and pass it to `DiodePairT`.
- [ ] `GoldModel.cpp`: `kSiIdeality` 1.0 → 1.752.
- [ ] Measure the corrected static WDF ceiling; derive `kDefaultVk` from it and set it.
- [ ] Re-run every baseline measurement; tabulate before → after.
- [ ] Fix the stale comments: `RatModel.h` (~line 10, "±0.6 V" is now true),
      `RatModel.cpp:27-41` (the diode block), `GoldModel.h` (~line 59, the silicon
      counterfactual), `DiodeClipperADAA.h:9-16` (the "~0.33-0.39 V" fit).
- [ ] Update `clipper_rat_tests` / `clipper_gold_tests` assertions to the corrected
      physics, saying per assertion whether the old bound pinned the **bug** or a
      genuine property.
- [ ] Re-baseline `testGermaniumKnee`; its tolerances should get **easier**.
- [ ] `docs/DEVELOPMENT.md` §36 + ADR 008 + `docs/DEVELOPMENT.md` §6 bullet 2 and §7's
      ADAA note.
- [ ] Run the full core suite; run `--golden-report` and report the per-golden,
      per-band deltas. **Do not re-bless.**

## How this will be measured

1. **Static WDF ceiling** — drive the exact `Vs || Cp → DiodePairT` tree with a DC
   `vin` and read the settled cap voltage, at vin = 1 / 10 / 100 / 600 V. Acceptance:
   **0.6–0.7 V** at realistic drive, vs the shipped 0.322 / 0.376 / 0.421 / 0.434.
2. **`kDefaultVk`** — the WDF output at the drive level the RAT actually reaches at
   default knobs, so `Vk·tanh(x/Vk)` limits where the WDF limits. Reported as the
   WDF-vs-ADAA RMS/peak/THD gap before and after.
3. **GOLD contrast** — clipped-path output level (dB) germanium vs silicon at a fixed
   drive. Acceptance: **~6 dB**, not the shipped ~0.6–1.5 dB.
4. **Voicing risk** — RAT at default knobs (dist 0.7 / filter 0.4 / level 0.8) into a
   0.3 V-peak humbucker-level pluck and sine: output RMS, peak, THD, and the
   DISTORTION knob at which THD crosses 5 % / 10 %. Reported, **not compensated**.
5. **Goldens** — `clipper_player_expectations_tests --golden-report`, per-rig
   `GOLDEN-DELTA` lines. `rat_jcm800` is expected to move; report the dB, bless nothing.

## Manual test steps

- [ ] `ctest --test-dir build --output-on-failure` — everything green except the
      golden block, which must fail loudly with a quoted band delta.
- [ ] `build/clipper-render --alias-report` still shows the 4× floor is intact (the
      knee got softer, so aliasing must not get worse).
- [ ] Edge case: a NaN pushed at `rat_set_param` still cannot latch — the corrected
      diode changes the WDF operating point, so re-run `clipper_nan_guard_tests`.
- [ ] Edge case: `--stage2 adaa` and the WDF path, same input, must now land in the
      same output ballpark (they did not before — that is the point of re-deriving Vk).

## Out of scope for this session

- Any pre-gain / input-trim / `kDistMaxDb` re-voicing to compensate for the higher
  ceiling. Measured and reported only.
- Re-blessing the goldens (a human ritual — `scripts/update-goldens.sh` needs a
  confirmation typed at a real tty).
- Rebuilding the committed WASM artifact (`web/public/generated/*`) — out of the
  slice's remit; the artifact staleness gate will correctly flag it.
- The germanium side of GOLD, the other diode-bearing models (SD-1 / TS / Muff use
  `BjtStage`, which already has `nVt = 0.0453`, i.e. n ≈ 1.75 — correct).

---

<!-- Filled in during/after the session -->

## What actually happened

Went to plan. Four notes worth carrying forward.

**1. The `--golden-report` path already existed** (landed with the §31 golden-blessing
ritual) and gives one `GOLDEN-DELTA` line per rig with the *worst* band. To produce the
per-band table the orchestrator asked for I patched a `BAND …` printf into
`measureAgainstGolden`, ran the report, and restored the file byte-for-byte
(md5-verified against `HEAD`). If a per-band verbose flag is wanted permanently that is
its own small slice — I did not expand this one to add it.

**2. `kDefaultVk` was derived, not chosen.** Least squares in dB against the settled
WDF node over 41 log-spaced drive levels (0.5–100 V). Optimum 0.6659 at 0.605 dB rms;
shipped 0.67 at 0.607 dB. Running the same fit against the *old* curve returns 0.3703,
so the shipped 0.35 was already 0.14 dB off its own (wrong) target — it really had been
eyeballed.

**3. The old WDF-vs-ADAA A/B looked healthy, which is the whole lesson.** Pre-fix the
two stages agreed within 0.09–0.98 dB. They were wrong *together*. A comparison path
calibrated against the thing it compares cannot report a disagreement. Hence
`testAdaaTracksWdf`, which measures the two implementations through the real model
instead of re-deriving Vk from RatModel's constants.

**4. Both new absolute tests were perturbation-proven, and both perturbations were
`touch`ed after patch and restore** (the §29 trap: restoring a backup preserves its
mtime and `make` then skips the rebuild). `testClippingCeiling`'s absolute half fails
on the pre-fix diode; `testDiodeLevelContrast` fails with `kSiIdeality` back at 1.0;
`testAdaaTracksWdf` fails with the corrected diode and `kDefaultVk` back at 0.35.

## Measured results

Full tables in docs §36. Headlines:

| property | before | after |
|---|---|---|
| WDF clipping node, 10 V drive | 0.3761 V | **0.6778 V** (+5.12 dB) |
| WDF clipping node, RAT at default DISTORTION 0.7 | 0.381 V | **0.678 V** |
| `testClippingCeiling` measured node peak | (no absolute assertion existed) | 0.727 / 0.737 V, toe 0.664 V |
| `kDefaultVk` fit residual vs the live WDF | 0.860 dB (@0.35) | **0.607 dB** (@0.67) |
| WDF-vs-ADAA rms gap, 0.3 V in | −0.42 dB (both wrong together) | +0.20 dB |
| GOLD Ge-vs-Si contrast, through the model | +0.96…+1.56 dB | **+5.88…+6.11 dB** |
| GOLD Ge-vs-Si onset ratio / slope ratio | 26× / 0.032 | **189× / 0.0063** |
| RAT out, 0.3 V-peak sine, default knobs | −10.80 dBFS, 38.3 % THD | **−5.86 dBFS**, 36.5 % THD |
| RAT out, 110 Hz pluck @0.3 V peak | −12.59 dBFS | **−8.39 dBFS** |
| DISTORTION at THD ≥ 5 % / ≥ 10 % | 0.24 / 0.27 | 0.32 / 0.35 |
| 4× worst-alias / fundamental | −104.1 dB / 0.428 | −102.4 dB / **0.757** (alias-to-signal −3.3 dB better) |
| golden `rat_jcm800` | — | **CHANGED: rms −0.15 dB, worst band +6.50 dB @ 800 Hz** |
| goldens `sd1_twin_reverb` / `muff_twin` / `ts_ac30` / `clean120_chorus` | — | UNCHANGED (≤ 0.11 dB) |

**Voicing judgement: no compensating slice indicated.** The pedal got *louder*, which
is the opposite of the M6.1 "gutless" report, and at its shipped default it is still at
36 % THD. Clipping onset moved 0.08 of knob travel. What does deserve its own slice is
downstream level staging — the amp and the worklet limiter now see ~5 dB more.

## Files created / modified

Core DSP (the whole fix is four constants and their documentation):
- `core/src/dsp/RatModel.cpp` — `kDiodeIdeality = 1.752`, passed to `DiodePairT`; diode
  comment block rewritten with the measured ceiling and the finding-15 history
- `core/include/clipper/dsp/RatModel.h` — the stale "hard clip at ~±0.6 V" claim
- `core/include/clipper/dsp/DiodeClipperADAA.h` — `kDefaultVk` 0.35 → 0.67, and the
  header comment that had cited the bug as its justification
- `core/src/dsp/GoldModel.cpp` — `kSiIdeality` 1.0 → 1.752, plus the section-2 comment
- `core/include/clipper/dsp/GoldModel.h` — the silicon counterfactual's description

Tests:
- `core/tests/test_rat_model.cpp` — `testClippingCeiling` gains an absolute band;
  `testAdaaTracksWdf` new; `testFactorOneRegression` samples regenerated
- `core/tests/test_gold_model.cpp` — `testGermaniumKnee` re-baselined (bounds TIGHTENED
  5×→20× and 0.25×→0.05×; the *margin* to the measurement widened because the measured
  ratios improved by an order of magnitude); `testDiodeLevelContrast` new

Docs:
- `docs/DEVELOPMENT.md` — new §36; §6 bullet 2 and §7's ADAA bullet corrected
- `docs/decisions/008-diode-ideality-and-derived-references.md` — new
- `CLAUDE.md` — Current State
- `docs/work/2026-07-25-diode-ideality.md` — this file

Deliberately **not** touched: `core/tests/goldens/*.wav`, `core/tests/goldens/GOLDENS.md`,
`web/public/generated/*`.

## Deferred to next session

1. **Bless (or reject) `rat_jcm800`.** `clipper_player_expectations_tests` fails until
   someone does. The per-band table is in §36; the ritual is
   `bash scripts/update-goldens.sh -m "…"` plus a `GOLDENS.md` entry. A human act by
   design — the script's confirmation cannot be answered by an agent.
2. **Rebuild the committed WASM artifact.** `core/` changed, so
   `web/public/generated/clipper.js`, the worklet copy and `.build-stamp.json` are
   stale and `check-artifact.mjs` will fail. `bash scripts/build-wasm.sh` (emsdk 6.0.4).
   Never hand-write the stamp.
3. **Downstream level staging.** The RAT delivers ~4–5 dB more into the amp and the
   worklet's soft limiter. Worth a look with the §36 table as the input — and it is a
   *staging* question, not a RAT re-voice.
4. **A per-band verbose mode for `--golden-report`.** Would have saved a
   patch-measure-restore round trip here.
5. **§7's `--alias-report` table** is stale in absolute dB (its ADAA-vs-WDF verdict
   still holds); §36 supersedes it for the RAT. Already flagged in §32.

## Status

- [x] Complete — with the two intentional follow-ups above (golden blessing, artifact
      rebuild), both of which are human/toolchain acts outside this slice's remit. The
      golden test is **expected red** on this branch.
