# ADR 008: A diode is its whole SPICE card, and a reference curve is derived from the circuit — never fitted to it

Date: 2026-07-25
Status: Accepted

## Context

`RatModel`'s WDF clipper shipped from M1 through v1.1 with the 1N4148's saturation
current (`Is = 2.52 nA`) and its **ideality factor dropped to 1.0**. The SPICE model
card those numbers come from reads `IS=2.52n N=1.752`; one of the two was taken and
the other silently defaulted. `chowdsp_wdf`'s `DiodePairT` uses its fourth argument
as `Vt_eff = nDiodes * Vt`, which is arithmetically exactly an ideality factor, so
`n = 1.0` shrank the junction's thermal voltage by 1.752× and the pair's measured
clipping ceiling came out at **0.32–0.43 V instead of 0.6–0.7 V — 5–6 dB low**, with a
harder knee than a real silicon pair. `RatModel.h`, `RatModel.cpp` and
`docs/DEVELOPMENT.md` §6 all documented ±0.6 V; `BjtStage.h` had the physics right all
along (`nVt = 0.0453`, n ≈ 1.75), so the codebase disagreed with itself.

Two things made this more than a one-constant slip.

**First, no test could catch it.** `testClippingCeiling` asserted only that doubling
the input barely moves the peak. Every saturating clipper satisfies that at *any*
ceiling. There was no absolute reference anywhere near the diode.

**Second, a second constant was fitted to the defect.** `DiodeClipperADAA::kDefaultVk`
— the knee of the memoryless `Vk·tanh(x/Vk)` curve that exists *purely* to be
A/B'd against the WDF stage — was set to 0.35 with a header comment citing the
"~0.33-0.39 V" WDF measurement as its justification. The comparison path had been
calibrated to agree with the circuit's error. Nothing linked the two constants, so
fixing the diode alone would have left `--stage2 adaa` quietly comparing two different
circuits while continuing to look plausible. `GoldModel`'s silicon *counterfactual*
carried the same `n = 1.0` error, which collapsed its germanium-vs-silicon A/B from a
real ~6 dB contrast to ~1 dB — an A/B that measured almost nothing about the thing it
existed to demonstrate.

## Decision

**1. A device's parameters are taken as a set, from one named source.** `RatModel`
gets `kDiodeIdeality = 1.752` and `GoldModel`'s silicon counterfactual gets
`kSiIdeality = 1.752`, both cited to the same 1N4148 SPICE card that already supplied
their `Is`. If a model uses part of a device card, it uses all of it, or it documents
in the ADR why not and what that costs audibly. GOLD's **germanium** side is left
alone: `n = 1.3` with a 0.286 V knee at 1 mA matches its own documentation, so the
pedal's actual voice is unchanged — only its counterfactual moved.

**2. This is accepted as a deliberate tone change, un-compensated.** The RAT is now
~5 dB louder and correspondingly less saturated at a given DISTORTION setting. No
pre-gain, input trim or `kDistMaxDb` was adjusted to absorb it. That restraint is the
decision, not an oversight: §11.1 already records this pedal being level-calibrated
once in response to a "no balls" report, and CLAUDE.md names `kFullScaleSecV`
absorbing two factor-of-2 errors as the cautionary case for exactly this reflex. The
consequence is measured and published in §36 (output +4.2 to +5.0 dB at realistic
input, clipping onset moving from DISTORTION 0.24 to 0.32 for THD ≥ 5 %), and any
re-voicing is a separate slice arguing from those numbers.

**3. A reference curve is DERIVED from the circuit by a stated procedure, never
fitted by eye to whatever the circuit currently does.** `kDefaultVk` is now the
least-squares optimum, in dB, of `Vk·tanh(x/Vk)` against the settled WDF node voltage
over 41 log-spaced drive levels from 0.5 V to 100 V — 0.6659, rms error 0.605 dB,
shipped rounded to 0.67 (0.607 dB). The procedure and its residual are recorded in the
header, so the next person who changes the diode knows how to re-derive it instead of
guessing.

**4. Every derived reference gets a test that fails when it drifts from its source.**
`testAdaaTracksWdf` drives the WDF and ADAA stage-2 modes through the real model at
the shipped default knobs and requires their rms and peak to agree within 1.5 dB. It
compares two *implementations* through the model rather than re-deriving `Vk` from
`RatModel`'s own constants, so it is a property and not a tautology. A derived constant
without such a test is a comment, not an invariant.

**5. Where an external absolute reference exists, assert against it.** The new
absolute half of `testClippingCeiling` (clipping node in 0.60–0.85 V, from the 1N4148
datasheet's 0.62–0.72 V forward drop) and the new `testDiodeLevelContrast`
(germanium-vs-silicon in 4–9 dB, from real 1N34A and 1N4148 forward drops) both cite
references outside our netlist. Both were perturbation-tested: reverting either
ideality factor fails them.

## Consequences

- **`rat_jcm800`'s golden is stale by up to 6.50 dB in one third-octave band**, and it
  was deliberately **not** re-blessed — `clipper_player_expectations_tests` fails on
  the branch that introduces the fix, with the per-band table in §36 for a human to
  judge. That is the golden ritual (§31) working as intended rather than a broken
  build, but it does mean this correction cannot merge without a human blessing act.
- **The committed WASM artifact goes stale** in the same slice (`core/` changed), so
  `check-artifact.mjs` fails until someone runs `bash scripts/build-wasm.sh`.
- **Two other tests had to be recaptured or re-baselined.**
  `testFactorOneRegression`'s hardcoded samples are a drift guard rather than a
  property, so a deliberate clipper change necessarily invalidates them and they must
  be regenerated (the check that nothing *else* moved is that they grew by ~5 dB, the
  amount the ceiling grew). `testGermaniumKnee`'s bounds were **loosened** — from 5×
  to 20× on onset and 0.25× to 0.05× on slope — because the bug had been making its
  genuine properties *harder* to satisfy, not because they were pinning the defect.
  Loosening a bound normally signals a problem; the distinction is that the measured
  margins improved by an order of magnitude at the same time (26× → 189×,
  0.032 → 0.0063), which is recorded so a future reader does not read this as a
  concession.
- **A little more CPU, and slightly better aliasing.** A softer knee means the diode
  solver's Newton iteration is no worse-conditioned and the alias floor holds: 4×
  worst-alias moves −104.1 → −102.4 dB while the fundamental rises 4.98 dB, i.e.
  alias-to-signal *improves* by ~3.3 dB.
- **Cost accepted:** the fix makes the RAT unfamiliar to anyone who has been playing
  the shipped version. Its DISTORTION knob is now hotter and cleaner at every
  position. There is no migration path for a saved rig — a stored `distortion: 0.7`
  will sound different after this lands. Presets do not exist yet (they are on the
  post-v1.1 list), which is the main reason it is acceptable to do this now rather
  than later.
