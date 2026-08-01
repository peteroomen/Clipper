# ADR 019: The compressor is a config-parameterized engine from its first line

Date: 2026-07-31
Status: Accepted — **amended by ADR 021 and ADR 025** (2026-08-01)

## Context

The owner asked for two compressors in the same breath — "the first two types",
meaning the OTA (MXR Dyna Comp / Ross) **and** the optical (LA-2A style). The house
rule is one slice per PR, so they ship as two slices: M13.2 (OTA, this one) and
M13.3 (optical). A third consumer is already on the roadmap — M13.6's noise gate is
the same detector with the gain decision inverted.

That creates a choice at the very start of the first slice: write `CompModel` as a
self-contained Dyna Comp and refactor an engine out of it later, or write the engine
first and make the Dyna Comp a config of it.

This project has already answered this question once, and the answer is on record.
Docs §21: the SD-1 and the TS are the *same* Tube Screamer topology, and the TS
shipped by refactoring the SD-1 onto a shared, config-parameterized
`OverdriveEngine` — **byte-for-byte**, with the M8 suite passing unchanged. That
refactor was possible only because the two circuits turned out to be genuinely the
same shape. Retro-fitting a seam is cheap when you are lucky and expensive when you
are not, and nobody knows which case they are in until they try.

## Decision

Build `CompressorEngine` + `CompressorConfig` **first**, and ship `CompModel` (the
Dyna Comp voice) as a config of it — in the same slice, from the first line, before
there is any second voice to justify it.

Write the seam down explicitly rather than leaving it implied. The engine header
carries a `THE M13.3 SEAM` block naming, for the optical voice:

* **reused as-is** — `InputStage`, `DriveNetwork`, `LoadStage`, the detector and its
  time constants, the whole parameter/reset/park/denormal discipline;
* **changed — exactly two things** — the gain-cell law (one `case` in
  `applyGainCell()`; the OTA case is `Iout = Iabc·tanh(Vd/2Vt)`, the optical case is
  the LDR law) and its config constants;
* **not needed** — `otaVt`, `swingUp`/`swingDown` (CA3080-specific output limits).

M13.6's gate is named in the same block as the other consumer: the same detector,
with the gain decision inverted.

## Consequences

**Easier.** M13.3 becomes a config plus one `case` rather than a second model, and
the reviewer of that PR can check it against a written contract instead of inferring
one. M13.6 gets a detector that has already been measured against an absolute
reference (an independent SPICE run: idle 199.85 µA against the reference's ~192 µA,
settled 16.54 against ~16). The alternative — three hand-rolled detectors — is how
this repo ended up with ~14 broken copies of a parameter clamp before ADR 002.

**Harder / the cost.** The OTA voice pays for generality it does not use: an extra
indirection in the gain cell, and config fields that are dead for it. That cost is
accepted because it is small and measured — the pedal runs 12.1 % of one stream on
signal and 6.1 % on silence, well inside the lineup's range.

## AMENDMENT 2026-08-01 — M13.6a shipped, and the named risk FIRED (docs §61.2)

The noise gate is built. The mitigation below was exercised exactly as written:
the finding is reported and the header corrected, and the engine was **not**
widened. Recording the outcome here so this ADR is not read as a claim that
turned out to be entirely true.

**What held.** The gate genuinely reuses the detector — the same full-wave
clamp/rectifier and the same envelope integrator, with nothing changed but
component values (10 µF/150 kΩ → 47 nF/220 kΩ for the envelope pair, and equal
leg source impedances because a gate's rectifier is driven by an op-amp rather
than a phase splitter). No second envelope follower was written, which was the
whole point.

**What did not hold, and what was done about it.**

1. **`Sidechain` was a config struct with NO CODE attached.** The detector lived
   in `CompressorEngine::detectorLeg()` and `::advanceEnvelope()`, both private,
   with their state in that class's members. There was nothing a second consumer
   could hold. It is now the standalone `SidechainDetector`
   (`core/include/clipper/dsp/SidechainDetector.h`), owned by value by both
   pedals. The move is **bit-identical** for the compressor (verified by render
   hash), so this is a structural correction and not a tone change.
2. **`ControlMap` is not a policy hook.** It is three resistor values that turn
   the envelope node into an OTA bias current. A gate has no rheostat, no cell
   pin and no control current — its control law is a Schmitt comparator plus an
   attack/hold/decay ramp into a VCA. `GateModel` therefore has its own
   `GateControl`, and `ControlMap` was left alone. That is this ADR's own
   instruction followed literally.
3. **`detectorFromOutput` is OTA-only.** For a gate, feed-back is not a variant,
   it is broken: once the gate closes the detector sees only the VCA's
   off-isolation and can never re-open. Measured (feed-forward −0.02 dB vs
   feed-back −80.02 dB on the same note), so the flag stays where it is and the
   gate is feed-forward by construction.

**The rule for M13.3, narrowed by this:** reuse the DETECTOR, and expect the
CONTROL side to be your own. The `CompressorEngine.h` banner says so now.

**The real risk, named.** A seam written before its second consumer exists can be
the *wrong* seam. The mitigation is that it is documented as a claim rather than
assumed as a fact: if M13.3 finds the optical voice needs to change something the
header says is "reused as-is", **that is a finding to report and the header to
correct — not a reason to quietly widen the engine until both fit.** An engine
that grows a parameter every time a voice disagrees with it has stopped being a
shared model and become a union of two, which is worse than either.

**Not covered by this ADR.** The wah slice (§58) shipped an envelope follower of its
own on a parallel branch; the two were deliberately not coupled while both were in
flight. Unifying them — or deciding they are legitimately different (a wah's
follower drives a filter, a compressor's drives a gain cell, and their time
constants are not the same) — is a named follow-up, not a consequence of this
decision.

> **SETTLED 2026-08-01 — see ADR 023.** The follow-up was taken and the answer is
> that they are **legitimately different**, measured rather than argued.
> `SidechainDetector` is a THRESHOLD detector: **1.950 dB** of open-loop
> proportional range on this compressor's own component values, against
> **19.085 dB** for the wah's one-pole. This compressor's graded response is its
> **feedback loop** (§59: 216:1 feed-back vs 3.3:1 feed-forward), which a wah —
> having no gain to reduce — cannot borrow. The substitution was built and run;
> every §58.6 AUTO number regressed and three bars went red. The wah keeps its own
> follower and the component was **not** widened, per the prohibition below.


## Amendment (2026-08-01, ADR 021)

M13.6a's noise gate was the first consumer to actually arrive, and it is the
test this ADR asked for. **The claim held — the detector really is the shared
part, and no second envelope follower was written. The seam as WRITTEN did
not**: `Sidechain` was a config struct with no code attached, the behaviour
sat in two private methods, and `ControlMap` turned out to be three resistor
values rather than a policy hook. It was corrected — extracted into a real
`SidechainDetector` component, compressor bit-identical — rather than widened
until both voices fit, which is exactly what the Consequences section above
said to do. See **ADR 021**.

## AMENDMENT 2026-08-01 (2) — M13.3 SHIPPED, and the ENGINE half of this ADR's
## claim did NOT hold (docs §64.3, ADR 025)

The optical voice is built. It is **not** a config of `CompressorEngine` and **not**
a consumer of `SidechainDetector`, and both were decided by measurement rather than
by inspection.

Of the eight config structs, **one** (`OutputStage`) applies to an optical leveling
amplifier. `applyGainCell()` returns a CURRENT where an optical cell is a RESISTOR
IN A DIVIDER; `DriveNetwork`, `LoadStage`, `Splitter` and the compliance are the
CA3080 pedal's own stages. And that pedal has **no envelope capacitor at all** — an
EL panel emits on both polarities, so the panel is the rectifier and the photocell
is the integrator — so there is nothing for `SidechainDetector` to be. The
substitution was built and run per ADR 023's procedure: the ratio curve goes
non-monotone to 10.40:1, gain reduction at −30 dBV collapses to 0.09 dB, and the
voice's acceptance property (a program-dependent release, 2.892x) goes to 1.000x.

**Scorecard for this ADR's central bet — design the seam before the second consumer
exists.** On the DETECTOR it is 1 for 2 (the gate reuses it; the wah and the optical
voice do not). On the ENGINE it is 0 for 1: `CompressorEngine` now has exactly one
consumer. What it did buy, both times, was that the question got asked and answered
with a number instead of being discovered by a duplicated block. Neither type was
widened. See **ADR 025**.
