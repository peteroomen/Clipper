# ADR 025: The optical compressor is neither a config of `CompressorEngine` nor a consumer of `SidechainDetector`

Date: 2026-08-01
Status: Accepted
Amends: ADR 019, ADR 021. Follows the procedure ADR 023 set.

## Context

ADR 019 built `CompressorEngine` config-parameterized from its first line and
named **M13.3, the optical voice, as its consumer**: "a config plus one
`applyGainCell()` case". It also named the risk in terms — a seam written before
its second consumer exists can be the *wrong* seam, and the answer to that is *"a
finding to report and the header to correct, not a reason to quietly widen the
engine until both fit."*

ADR 021 was that risk firing once, for the noise gate: the CLAIM (the detector is
the shared part) held, the STRUCTURE did not, and the fix was to extract
`SidechainDetector` as a real component rather than widen a config struct. It
narrowed the rule for M13.3 to **"reuse the DETECTOR, and expect the CONTROL side
to be your own"**, and restated the prohibition:

> if the optical voice needs something `SidechainDetector` does not offer, the
> answer is still to report it and correct the component — not to grow it a
> parameter per voice until it is a union of three models.

ADR 023 then refused the wah as a consumer of the same component, and left the
procedural rule this ADR follows: **build the substitution and measure it.**
"Both blocks rectify and integrate" is neither a reason to share nor a reason to
refuse.

M13.3 is now built. This ADR records what the two seams measured.

## The measurements

### 1. It is not a config of `CompressorEngine`

Of that engine's eight config structs, **one** (`OutputStage`) survives contact
with an optical leveling amplifier. `DriveNetwork` is the CA3080's differential
input attenuator; `LoadStage` is what the cell's output CURRENT develops its
voltage across, plus that chip's output compliance; `Splitter` is the Dyna Comp's
phase splitter; `ControlMap` is the Iabc path (ADR 021 already said so); and
`applyGainCell()` returns a **current**, where an optical cell is a **resistor in
a divider** and returns nothing at all — it changes a gain.

Making it fit would take four axes — a gain-cell kind whose two cases have
different units, flags to bypass `LoadStage` / `Splitter` / the compliance, and a
detector kind — whose settings share no component. That is the union-of-N-models
ADR 021 forbids.

Seam-by-seam table in docs **§64.3**.

### 2. `SidechainDetector` is not this pedal's detector — built and run

The substitution was built rather than argued, per ADR 023. A replica of the
shipped loop with one switchable block, validated against the shipped model first
(settled gain reduction agrees to **0.05 dB** at −24 / −12 / 0 dBV), and given the
detector's best configuration: the gate's op-amp precision-rectifier clamp form,
the envelope pair pinned to the reference's published 60 ms 50 %-release, M13.1's
own 10 µF cap, calibrated at ONE point.

ADR 023's own metric — proportional range, 10 % → 90 % of swing, open loop,
146.83 Hz, all rows at one rate:

| block | proportional range |
| --- | --- |
| M13.1's detector (10 µF / 150 kΩ) | **2.031 dB** (§58.8 measured 1.950 — the instrument agrees) |
| the best opto-plausible detector config | **8.901 dB** |
| this pedal's EL panel + CdS cell | **14.323 dB** |

And the consequences on the replica: a smooth monotone ratio rising toward the
predicted 2.875:1 becomes a **non-monotone 10.40:1 wall** with **0.09 dB of gain
reduction at −30 dBV** (the level the reference's own manual says it has already
started compressing at), and — with the detector standing in for the cell's
dynamics too — the acceptance property vanishes: complete release **0.364 s after
a 150 ms stab and 0.364 s after a 6 s passage, 1.000×**, which is M13.1's own
figure, because an RC is an RC.

**The physical reason, stated plainly: this pedal has no envelope capacitor.** An
EL panel emits on both polarities, so the panel is the rectifier and the
photocell — with its two-stage, trap-retarded recovery — is the integrator. There
is no component in the reference for `SidechainDetector` to be.

## Decision

**`OptoModel` is a standalone model, like `GateModel`. `CompressorEngine` was not
widened, `SidechainDetector` was not widened, and the optical voice consumes
neither.**

What it does share is a new component, extracted on the way in rather than after
the fact (ADR 021's lesson applied forwards): **`OptoCell`** — the
electroluminescent-panel-plus-photoresistor attenuator, owning its own state, held
by value, with ROADMAP **M13.5's photocell-driven Uni-Vibe** as its named future
consumer.

`CompressorEngine.h`'s M13.3 seam banner and `SidechainDetector.h`'s "next
consumer" note are corrected to say what was found.

## Consequences

**Easier.** Each of the three dynamics pedals is now readable on its own terms,
and the one thing genuinely shared between two of them (`SidechainDetector`) stays
a description of one circuit rather than a switchboard. The photocell arrives as a
component with a named consumer instead of a private detail M13.5 would have had
to re-derive.

**Harder / the cost.** `CompressorEngine` now has exactly one consumer, which
makes ADR 019's central bet — design the seam before the second consumer exists —
0 for 2 on the *engine* and 1 for 2 on the *detector*. The plumbing `OptoModel`
does not inherit (prepare / reset / oversampling / smoother discipline) is
duplicated, which is the house norm — `RatModel`, `WahModel`, `GateModel` and
`DelayModel` all carry their own — but it is duplication.

**The general lesson, and it is the third in the sequence.** ADR 021: a config
struct is not a seam, extract the block. ADR 023: an extracted block is not
automatically the right block for the next consumer. This one is the layer above
both: **an ENGINE is not automatically the right engine either, and "two pedals do
the same JOB" is a much weaker signal than "two pedals contain the same
SUB-CIRCUIT."** A Dyna Comp and an LA-2A are both compressors and share no block
at all; a Dyna Comp and an NS-2 do completely different jobs and share a detector
exactly. Pick the seam by circuit, not by category.

**What this does NOT license.** It is not a licence to write a private copy of
something that *is* shared. The test is unchanged and it is empirical: build the
substitution and measure it. Two of the three attempts so far have come out
"refuse", and both refusals carry perturbation-proven tests so they cannot rot
into prose.
