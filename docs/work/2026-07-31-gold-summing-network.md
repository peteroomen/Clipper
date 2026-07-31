# GOLD dirt summing weight — the schematic's network, not a fit

**Date:** 2026-07-31
**Branch:** claude/gold-summing-6f557i
**Roadmap item:** owner feedback round 3 (Drive doc "Clipper Feedback"): "Much better,
but still too much gain. Edge of breakup is around 5, 0 is fully transparent which is
good, 100 gain and it sounds like a marshall at mid-high gain. So still too much gain
for sure, really they only get creamy/crunchy at max, they don't reach marshall levels.
Not too warm/tinny or vowley now, sounds great."

## Goal

The GOLD's dirt loudness comes from the published schematic's summing network instead
of the §50 fit (`kClipBlendWeight = 0.65`): audible breakup onset moves well past
knob 5, and max gain reads creamy/crunchy — while the tone character (which the owner
has signed off) and the GAIN-0 bit-exact-clean contract stay untouched.

## Approach

Deliberate tone change, judged against the published schematic + the reference THD
rows the §50 slice already used. The insight from the feedback: our max measures
15.3 % THD — inside the real unit's published 15–25 % band — yet reads as "Marshall
mid-high gain". Perceived gain tracks the DIRT'S LOUDNESS AGAINST THE CLEAN, not THD
alone, and `kClipBlendWeight = 0.65` is the last constant in this pedal that is a fit
rather than a derivation.

Derive the real summing ratio: in the published circuit the output summing stage
mixes the clean feed and the diode path through fixed resistors (the knob changes
drive, not mix — §50). The implementing agent must pull the actual component values
from the named references (the ElectroSmash Centaur analysis and/or the Chowdhury
KlonCentaur source, both already cited in GoldModel.cpp §50 comments; network access
is available) and compute the dirt-branch weight RELATIVE to the clean feed at the
summing node, including any post-diode attenuation the real network has before the
summing resistor. Replace 0.65 with the derived value (name the resistors in the
comment); reconsider `kClipBlendFadeTo` in the same derivation (the fade-in span is
also a §50 idealization — if the real network implies dirt stays buried under the
clean at low knob, the fade may become unnecessary or shorter; keep `clipBlend(0)=0`
as the documented contract either way).

HONESTY GATE: if the schematic-derived weight does NOT land the owner's percept
(breakup onset still early in an A/B render), do NOT re-fit to taste — ship the
derived value with the measured tables and report the gap; the next probe is then the
diode-node drive, not the mix.

## Steps (for the implementing agent)

- [ ] Research: extract the summing-network component values from the references;
      document the derivation (resistor names + arithmetic) in the plan file and the
      code comment
- [ ] `GoldModel.cpp`: derived weight replaces 0.65; fade-in span re-examined in the
      same derivation; GAIN-0 render hash IDENTICAL before/after (the transparency
      contract — assert by hash in the report)
- [ ] Measure: THD + output level vs GAIN (0.05 steps, 220 Hz, 0.1 V AND 0.15 V peak
      — the unity-trim pickup level the JCM slice established as the field-report
      anchor), plus the dirt-to-clean level ratio at the summing node per knob.
      Before/after tables in the plan file
- [ ] Acceptance: near-clean at knob ≤ 0.10 at the 0.15 V anchor (THD within ~2× of
      the GAIN-0 floor); audible-grit onset mid-knob; max THD stays inside the 15–25 %
      reference band; dirt-vs-clean ratio = the schematic value by construction
- [ ] `test_gold_model.cpp`: re-derive the crossfade/level-contrast/knee probes IF the
      new weight moves them off their properties (§50 precedent — argue each in
      diode-node/summing-node units in comments); add a perturbation-proven bar
      pinning the derived weight (reverting to 0.65 must fail); prove teeth with the
      touch-after-patch-AND-restore discipline
- [ ] Player-expectations gold rows re-baselined (A1/A3/A4 + the A4 window if the
      default level moves)
- [ ] `--golden-report`: must show ZERO changed goldens (GOLD is in no golden rig;
      the GAIN-0 contract protects the transparency spec) — record the clean report
- [ ] WASM rebuild + artifacts committed; full core ctest (expect 25/25 — no golden
      moves on this branch); web build + Playwright (the gold worklet spec may need
      probe re-derivation if its pushed-harmonic bar moves — same honesty rules);
      node suites
- [ ] Docs §52 + CLAUDE.md entry + this plan file's bottom sections
- [ ] ONE commit on claude/gold-summing-6f557i (fix: …), measured before→after in the
      body, standard trailers; NO push, NO PR, NO golden writes

## How this will be measured

The before/after THD + level tables at both input levels, the dirt-to-clean summing
ratio table, the GAIN-0 hash pin, and the derivation arithmetic from named schematic
components. Owner's ear-test afterwards: edge of breakup well past 5; 100 creamy.

## Manual test steps

- [ ] Owner: GAIN 0 transparent; breakup onset well past 5; 35 mostly clean;
      100 creamy/crunchy, NOT Marshall; then GOLD-as-boost into the JCM (the famous
      use) — staging sanity
- [ ] Edge: Ge/Si contrast still ~6 dB at high drive; NaN rejected; sweep no zipper

## Out of scope for this session

The drive gang law (§50, validated), the treble network (owner signed off the tone),
the op-amp model, all other pedals.

---

## What actually happened

**The HONESTY GATE fired, in the direction nobody expected.** The plan's hypothesis was
that `kClipBlendWeight = 0.65` was a fit that would derive SMALLER and fix the "too much
gain" report. The schematic says the opposite: the derived weight is **4.1702**, six and
a half times larger. It was shipped anyway, unfitted, per the gate.

**The reference WAS reachable this time.** §27's honesty note says the DAFx-19 paper was
refused by the egress proxy. `electrosmash.com` and `coda-effects.com` still fail (DNS /
403), but Jatin Chowdhury's `KlonCentaur` repository clones fine and carries the whole
thing: the section-by-section netlist in
`ChowCentaur/GainStageProcessors/{PreAmpStage,AmpStage,FeedForward2,ClippingStage,SummingAmp}.h/.cpp`,
the paper source `Paper/Klon_Model.tex`, and the ElectroSmash-derived schematic figures
`Paper/Figures/{FullCircuit,GainStageCircuit}.png`. Every value below was taken from the
netlist and cross-checked against the figure.

**The derivation.** The summing stage (U2A) is a TRANSIMPEDANCE amp, not a mixer: three
paths push a current into its virtual ground and one feedback resistor turns the sum into
a voltage.

| path | route to the virtual ground | transconductance |
|---|---|---|
| dirt | diode node → `C10` 1 µF → `R16` 47 kΩ | `1/R16` = **21.277 µS** |
| clean, bass (FF1) | node B → `R7` 1.5 kΩ → (`C16` 1 µF ∥ `R19` 15 kΩ) | 5.87 µS @ 220 Hz |
| clean, treble (FF2) | gang-2 wiper → `C11` 2.2 nF + `R15` 22 kΩ (+ `R16`) | 0.15 µS @ 220 Hz |
| feedback | `R20` 392 kΩ ∥ `C13` 820 pF | the transimpedance |

`C10` is the ONLY post-diode network before the summing resistor, and it is a 3.4 Hz
high-pass — so there is no in-band post-diode attenuation to divide out (the dirt branch
measures 47.000 kΩ at 220 Hz). Therefore

```
dirt transimpedance  = R20/R16 = 392k/47k        = 8.3404
clean transimpedance = R20·(G_FF1 + G_FF2)       = 1.96 @110 Hz / 2.28 @220 / 1.82 @1 kHz
kClipBlendWeight     = (R20/R16)/kSumGain = 8.3404/2.0 = 4.1702
```

The clean row is the independent check and it LANDS: this model's `kSumGain·cleanBlend`
has been 2.0 since §27, and the schematic's clean transimpedance is 1.82–2.28 across the
band. So the clean side was already right and only the dirt side was a fit.

`kClipBlendFadeTo = 0.15` was re-examined and deliberately **unchanged** — the real
network gives it no support at all (the weight is fixed at every knob position), it
exists only to hold the `clipBlend(0) = 0` product contract, and no derivation supports
any other span. `kDrivePreScale`/`kDriveHpHz` were **validated, not changed**: the
netlist's `H_pre·H_amp/A(g)` measures 0.2234 @220 Hz / 0.5343 @1 kHz at g = 0.35 against
this model's 0.2238 / 0.5574 — §50's fit was right to 0.02 / 0.35 dB.

**What the derived value costs**, all measured: +13.3 dB of output at max, breakup onset
EARLIER (5 % THD at GAIN 0.53 → 0.28), max THD 16.8 → 23.7 % at the 0.15 V anchor
(27.5–28.3 % at 0.30/0.50 V, i.e. outside the 15–25 % reference band), and two properties
lost to the tone stage's ±8.6 V rail clamp engaging at max TREBLE — recorded as
XFAILs, not papered over.

**Where the perceived gain actually lives** (the gate's "next probe"): (1) this model's
germanium node runs ~2.1× hot — `Is = 200 nA, n = 1.3` (knee 0.286 V) through
`Rs = 2.2 kΩ` against the reference's fitted `Is = 15 µA, Vt = 25.85 mV` (knee 0.109 V)
through the schematic's `R13 = 1 kΩ`; the schematic's weight is right, the node it
multiplies is too loud. (2) The `R20 ∥ C13` = **495 Hz** summing-amp pole does not exist
in this model at all — that pole is what keeps a clipped Klon creamy instead of bright,
and it cannot be ported without the tone stage (alone it would put a ~−14 dB midrange
hole in the GAIN-0 path).

## Measured results

**GAIN-0 transparency contract — BIT-IDENTICAL.** FNV-1a over the render, before → after:
`85a97e9efc5686ba` → `85a97e9efc5686ba` (220 Hz 0.15 V sine, 48 kHz, TREBLE 0.5, OUT 0.5)
and `5b6b300ab9fd3a0d` → `5b6b300ab9fd3a0d` (0.5 s white noise).

THD % and dirt-to-clean summing-node ratio (48 kHz, 220 Hz, TREBLE 0.5, OUTPUT 0.5;
"dirt/clean" = dirt-only RMS ÷ the GAIN-0 clean RMS at the same input):

| GAIN | THD 0.10 V | THD 0.15 V | out dBFS 0.15 V | dirt/clean 0.15 V |
|---|---|---|---|---|
| 0.00 | 0.000 → 0.000 | 0.000 → 0.000 | −19.50 → −19.50 | 0.000 → 0.000 |
| 0.05 | 0.290 → 1.035 | 0.689 → 2.535 | −18.76 → −13.93 | 0.202 → 1.296 |
| 0.10 | 0.580 → 1.373 | 1.382 → 3.399 | −17.83 → −9.50 | 0.415 → 2.665 |
| 0.15 | 0.861 → 1.593 | 2.061 → 3.960 | −16.79 → −6.31 | 0.641 → 4.114 |
| 0.20 | 0.964 → 1.751 | 2.298 → 4.346 | −16.70 → −6.08 | 0.661 → 4.238 |
| 0.25 | 1.086 → 1.934 | 2.572 → 4.784 | −16.60 → −5.84 | 0.681 → 4.370 |
| 0.30 | 1.233 → 2.152 | 2.885 → 5.281 | −16.50 → −5.60 | 0.703 → 4.511 |
| **0.35** | **1.413 → 2.416** | **3.245 → 5.840** | **−16.39 → −5.34** | **0.727 → 4.662** |
| 0.40 | 1.637 → 2.740 | 3.657 → 6.469 | −16.28 → −5.08 | 0.752 → 4.822 |
| 0.45 | 1.916 → 3.138 | 4.127 → 7.176 | −16.16 → −4.80 | 0.778 → 4.994 |
| 0.50 | 2.266 → 3.630 | 4.664 → 7.969 | −16.03 → −4.52 | 0.807 → 5.178 |
| 0.55 | 2.706 → 4.238 | 5.276 → 8.856 | −15.89 → −4.22 | 0.838 → 5.375 |
| 0.60 | 3.257 → 4.987 | 5.972 → 9.846 | −15.74 → −3.92 | 0.871 → 5.588 |
| 0.65 | 3.943 → 5.902 | 6.763 → 10.948 | −15.59 → −3.59 | 0.907 → 5.818 |
| 0.70 | 4.787 → 7.005 | 7.662 → 12.178 | −15.42 → −3.26 | 0.946 → 6.067 |
| 0.75 | 5.820 → 8.326 | 8.688 → 13.554 | −15.23 → −2.91 | 0.988 → 6.340 |
| 0.80 | 7.074 → 9.895 | 9.860 → 15.094 | −15.04 → −2.53 | 1.035 → 6.639 |
| 0.85 | 8.582 → 11.742 | 11.206 → 16.829 | −14.82 → −2.14 | 1.087 → 6.972 |
| 0.90 | 10.402 → 13.921 | 12.770 → 18.805 | −14.58 → −1.71 | 1.145 → 7.344 |
| 0.95 | 12.600 → 16.499 | 14.600 → 21.072 | −14.31 → −1.26 | 1.211 → 7.770 |
| 1.00 | 15.300 → 19.603 | 16.772 → 23.702 | −14.00 → −0.75 | 1.289 → 8.270 |

Output level at 0.10 V, GAIN 1.0: −15.85 → −1.79 dBFS. Breakup onset (first GAIN whose
THD crosses a bar, 0.15 V, 220 Hz): 1 % **0.08 → 0.02**, 3 % **0.32 → 0.08**,
5 % **0.53 → 0.28**, 10 % **0.81 → 0.61**. Max-GAIN THD vs input (220 Hz):
0.05 V 9.20 → 10.70, 0.10 V 15.30 → 19.60, 0.15 V 16.77 → 23.70, 0.30 V 15.15 → 27.52,
0.50 V 12.21 → 28.31 %.

**Acceptance, honestly scored:** near-clean at knob ≤ 0.10 — FAIL (3.40 % at the 0.15 V
anchor vs a 0.00 % GAIN-0 floor); audible-grit onset mid-knob — FAIL (moved to 0.28);
max inside 15–25 % — PARTIAL (23.70 % at the anchor, 27.5–28.3 % at 0.30/0.50 V);
dirt-vs-clean ratio = the schematic value — PASS, by construction.

**Perturbation proof** (touch after BOTH patch and restore, per CLAUDE.md):
restoring `kClipBlendWeight = 0.65` makes `clipper_gold_tests` abort at
`test_gold_model.cpp:244` — *"dirt summing weight drifted from the schematic's
R20/(R16*kSumGain) = 392k/(47k*2) — do NOT re-fit it (docs §52)"*. Restored + touched +
rebuilt: exit 0.

**Goldens: ZERO changed.** `--golden-report`: `rat_jcm800 UNCHANGED +0.00`,
`sd1_twin_reverb UNCHANGED +0.00`, `muff_twin UNCHANGED +0.00`, `ts_ac30 UNCHANGED
+0.00`, `clean120_chorus UNCHANGED −0.00`.

**Alias floor** (max GAIN, 0.2 V C8): TREBLE noon (clipper isolated, rails idle) 4× is
−108.3 dB @44.1 k / −127.7 @96 k — the M2 bar, asserted hard. TREBLE max: 1× −20.3 /
4× −26.5 / 8× −26.4 dB, flat in the OS factor because the base-rate rail clamp, not
aliasing, dominates — XFAIL.

## Files created / modified

- `core/src/dsp/GoldModel.cpp` — `kSummingRfOhms` (R20) + `kDirtSumROhms` (R16),
  `kClipBlendWeight = R20/(R16·kSumGain)`, the full derivation + honesty note in the
  comment, and the §52 validation note on `kDrivePreScale`/`kDriveHpHz`
- `core/tests/test_gold_model.cpp` — the perturbation-proven weight bar; `testAliasing`
  split (hard M2 bar on the isolated stimulus + XFAIL on the max-treble one);
  `testHeadroomAndOutput` restated in rail volts + the rail-engagement XFAIL; two
  `XfailDecl`s, the ledger and `ledgerMain`/`reportXfails` in `main`
- `core/CMakeLists.txt` — `clipper_add_xfail_ledger(clipper_gold_tests)`
- `core/tests/test_player_expectations.cpp` — gold A1/A2/A3 rows re-baselined, A4 window
  −3..17 → 9..29, stale `GoldModel.cpp:351` line reference → `:487`
- `docs/DEVELOPMENT.md` — §52
- `web/public/generated/{clipper.js,clipper-processor.js,.build-stamp.json}` — rebuilt
- `CLAUDE.md`, this plan file

## Deferred to next session

- **THE GOLD DIODE NODE — the actual next probe.** `kGeIs = 200 nA` / `kGeIdeality = 1.3`
  (knee 0.286 V) and `kRs = 2.2 kΩ` against the reference's fitted `Is = 15 µA, n = 1`
  (knee 0.109 V) through the schematic's `R13 = 1 kΩ`. Fixing it closes BOTH new XFAILs.
  Do not chase it by re-fitting `kClipBlendWeight`, and do not raise `kRailVolts`.
- **The `R20 ∥ C13` = 495 Hz summing-amp pole + the active tone stage, together.** The
  best explanation on the table for "sounds like a Marshall at mid-high gain". Cannot be
  done alone without wrecking the GAIN-0 transparency spec.
- **The gain-dependent drive shaping.** The reference's `C7 = 82 nF` across `R10b` makes
  the amp stage's 1 kHz gain 100.5× at g = 1 vs its 25.8× DC law; this model's drive
  shaping is gain-independent. Measured, not fixed — and it makes the dirt hotter, so it
  is downstream of the diode-node slice.
- **The owner decision this branch exists for:** the derived value is faithful to the
  schematic and is worse against the field report. Keep it and fix the diode node, or
  revert to `0.65` and record it as an acknowledged fit. This branch is unpushed.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
