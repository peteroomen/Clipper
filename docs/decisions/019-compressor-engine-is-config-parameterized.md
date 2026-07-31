# ADR 019: The compressor is a config-parameterized engine from its first line

Date: 2026-07-31
Status: Accepted

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
