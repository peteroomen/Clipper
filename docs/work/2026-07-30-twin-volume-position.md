# Twin VOLUME pot position — the AB763 order restores the missing headroom

**Date:** 2026-07-30
**Branch:** claude/amps-pedals-fixes-6f557i
**Roadmap item:** owner field report 2026-07-30 ("breakup at 50 on the twin means not enough headroom") — slice 2 of the approved round

## Goal

At hot pickup level the Twin's breakup onset (5 % THD) moves from VOLUME ≈ 0.37 to ≈ 0.73 with
loudness unchanged at every knob position (≤ 0.1 dB) — the blackface headroom the player expects,
recovered from the schematic's own pot position rather than any re-voicing.

## Approach

Circuit-model **topology correction** (a deliberate tone change at mid-volume hot-input settings;
the diagnosis measured its whole effect). The model applies VOLUME **after** the V2 recovery stage
(`TwinPreamp.cpp` process chain), so V2's drive is volume-independent: at 0.316 V input the amp
carries a ~4.4 % THD floor **that the knob cannot remove** (measured identical V2 drive at VOL 0.1
and 0.5). In the real AB763 the channel volume sits **between the tone stack and V2's grid**, so
turning it down unloads every following stage. The diagnosis session's counterfactual (replica
chain, volume moved pre-V2, shipped power amp) measured onset 0.37 → 0.73 with RMS matched to
≤ 0.1 dB everywhere and identity at VOL 1.0 by construction.

The bright cap physically sits across the volume pot, so the bright bleed moves with it.
Smoothing stays per-sample (finding 6 discipline); no constants are re-derived — the interstage
scale is measured at VOL 1.0, where the change is an identity.

## Steps

- [ ] Move the `volSm_`/bright-bleed application in `TwinPreamp::process` from post-V2 to between
      `tone_.process` and `stage_[V2].process`; update the topology comment in `TwinPreamp.h`
- [ ] Measure THD-vs-VOLUME (220 Hz, 0.1 V and 0.316 V) before/after; confirm onset ≈ 0.73 and
      RMS deltas ≤ 0.2 dB per knob position; confirm VOL 1.0 render bit-identical
- [ ] Run the Twin suite: the clean bar (`test_twin_amp.cpp` §clean) should improve ~3.4 % → ~1.2 %;
      re-baseline any Twin reference tables the fix moves, and only those
- [ ] Check the five `finding7-ac30-*` XFAIL bars still XFAIL (the Twin comparisons get HARDER —
      an XPASS here would be a measurement error, investigate before touching anything)
- [ ] Player expectations: A-suite Twin rows re-measure; `sd1_twin_reverb` + `muff_twin` goldens
      WILL move (pedals drive the Twin hot at VOL 0.5) — print the `--golden-report` table for the
      owner's re-bless ritual; `rat_jcm800` / `ts_ac30` / `clean120_chorus` must be unmoved
      (scope check)
- [ ] Web amp-level drift guard: verify inside its gate (level ~unchanged at the guard's probe);
      re-centre the Twin row only if measurement says so
- [ ] Perturbation-proof any rewritten bar (restore old pot position in scratch, confirm red,
      touch after both edits)
- [ ] Full core ctest; `bash scripts/build-wasm.sh`; web build + Playwright; node + electron suites
- [ ] Docs §44 + `TwinPreamp.h` topology note; CLAUDE.md Current State; commit, push, PR

## How this will be measured

THD vs VOLUME table (220 Hz, 0.316 V): onset (5 %) 0.37 → ≈ 0.73, monotonic, VOL 1.0 identical;
RMS per position within 0.2 dB of pre-fix. Twin clean bar improvement. Golden dB table from
`--golden-report` for the two moving goldens, presented to the owner before any bless.

## Manual test steps

- [ ] Native/web: hot pickup, VOLUME 0.5 — clean with headroom; breakup arrives ~0.7+; no level
      jump vs before at matched positions
- [ ] BRIGHT behavior unchanged in character (the cap moved with the pot)
- [ ] Edge: VOL 0 silent, VOL sweep has no zipper (smoothing intact); NaN rejected at ABI

## Out of scope for this session

The Twin's ledgered volume-placement interaction with `finding7-ac30-breakup-vs-twin` (the AC30
slice owns that comparison); JCM Ra2 (slice 3); the Twin tremolo native switch (backlog).

---

<!-- Fill in below during/after the session -->

## What actually happened

As planned. Two additions beyond the plan: a **new perturbation-proven test**
(`test_twin_amp.cpp`, in `testHeadroomSagStability`) pinning the recovered headroom —
the plan only listed re-baselines, but the slice's player property deserved its own bar;
and the bar was set from a measured pre-fix run on the exact probe (110 Hz: pre 10.56 %
vs post 3.48 % at VOL 0.5 hot — a 5 % bar splits with real margin on both sides) rather
than from the 220 Hz diagnosis numbers, which would have left a 0.6 % relative margin.

The web drift guard needed **no** re-centring (RMS moved ≤ 0.08 dB at its probe) — the
plan's "verify" step resolved to no-change. Golden movement matched the prediction in
direction and scope; magnitudes (+2.18 / +5.85 dB RMS) are larger than the amp-alone
tables suggest because the pedals drive the Twin far beyond 0.316 V, where pre-fix V2
was acting as a hard compressor — that compression was the defect.

## Measured results

Docs §44.2–44.3 has the full tables. Headlines: hot-input breakup onset VOL ≈ 0.45 →
**≈ 0.75**; RMS matched ≤ 0.08 dB per position; VOL 1.0 bit-identical; clean bar
3.41 % → 1.17 %; AC30 XFAILs still XFAIL; goldens `sd1_twin_reverb` +2.18 dB /
`muff_twin` +5.85 dB (owner-authorized re-bless), other three unchanged (scope check).
Core ctest 26/26 after the bless; Playwright 71/71; node + electron green.

## Files created / modified

- `core/src/dsp/TwinPreamp.cpp` (pot position), `core/include/clipper/dsp/TwinPreamp.h` (topology doc)
- `core/tests/test_twin_amp.cpp` (new headroom bars)
- `core/tests/goldens/` (sd1_twin_reverb.wav, muff_twin.wav + GOLDENS.md entry)
- `web/public/generated/` (rebuilt artifact), `docs/DEVELOPMENT.md` §44, this file, `CLAUDE.md`

## Deferred to next session

- Slice 3 (JCM Ra2 → 120 k + NFB-window re-derivation), slice 4 (JCM bright cap),
  slice 5 (AC30) — the approved round order
- The Twin tremolo native switch (backlog task)

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
