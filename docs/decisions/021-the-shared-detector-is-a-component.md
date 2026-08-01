# ADR 021: A shared sub-circuit is a COMPONENT, not a config struct

Date: 2026-08-01
Status: Accepted
Amends: ADR 019

## Context

ADR 019 declared that M13.2's compressor engine was written config-parameterized
from its first line, with the optical voice (M13.3) and the noise gate (M13.6a) as
its **named consumers**, and that the seam was documented in
`CompressorEngine.h`. It also named the risk explicitly:

> a seam written before its second consumer exists can be the *wrong* seam … if
> M13.3 finds the optical voice needs to change something the header says is
> "reused as-is", **that is a finding to report and the header to correct — not a
> reason to quietly widen the engine until both fit.**

M13.6a is the first consumer to actually arrive. It is therefore the test of that
claim, and this ADR records what the test found.

## What the test found

**The claim held. The structure did not.**

**Right:** the envelope detector genuinely *is* the shared part. A gate needs the
same rectifier-and-integrator; it differs only in component values — the envelope
pair goes from the compressor's `10 µF / 150 kΩ = 1.500 s` to the gate's
`47 nF / 220 kΩ = 10.34 ms`, and the gate's rectifier legs are op-amp driven so
their source impedances are equal rather than split. **No second envelope follower
was written**, which was the entire point of ADR 019.

**Wrong — the seam was a struct with no code behind it.** `Sidechain` was a
*configuration* type. The behaviour lived in `CompressorEngine::detectorLeg()` and
`::advanceEnvelope()`, both **private**, with their state in that class's members.
There was nothing a second consumer could actually hold. The header described a
seam that did not exist as a thing.

**Wrong — `ControlMap` was not a policy hook.** On inspection it is three resistor
values producing an OTA bias current: a compressor-specific detail, not the
general "what do we do with the envelope" abstraction the banner implied.

**Wrong — `detectorFromOutput` is not a shared variant.** For a compressor,
feed-back versus feed-forward is a topology choice worth measuring (§59 measured
216:1 against 3.3:1). For a gate, taking the detector from the output is a *latch*,
not a variant — the gate would hold itself closed.

## Decision

**A shared sub-circuit is extracted as a COMPONENT — a type that owns its own state
and behaviour and is held by value — not described by a config struct that a single
owner interprets.**

Concretely: `SidechainDetector` is now a standalone type owned by value by both
`CompressorEngine` and `GateModel`. `ControlMap` was **not** widened; the gate has
its own `GateControl`. `CompressorEngine.h`'s seam banner is corrected to describe
what is actually shared.

The extraction is verified behaviour-preserving rather than assumed: the compressor
is **bit-identical** across two `clipper-render` renders, byte for byte, with its
suite, the denormal suite and the NaN-guard suite unchanged.

## Consequences

**Easier.** M13.3's optical voice now inherits a detector that is a real object with
a real interface, measured against an outside reference (§59's independent SPICE
run: 199.85 µA idle / 16.54 settled against ~192 / ~16). The gate's own numbers came
out of that same detector — 40.00 dB of threshold span, monotone, worst error
0.01 dB.

**The general lesson, which is the reason this ADR exists.** ADR 019's instinct —
design the seam before the second consumer — was right, and it saved a duplicated
envelope follower. Its *execution* confused "parameterized" with "shared": a config
struct describes a variation, it does not create a reusable thing. **When you
believe two circuits share a block, extract the block. A struct of numbers that one
class reads is not a seam; it is a settings bag.**

**Harder / the cost.** One more type to hold, and the compressor pays an indirection
it did not before. That cost was measured as zero-audible (bit-identical) and is
accepted.

**What this does NOT license.** ADR 019's prohibition still stands and now has
precedent: when M13.3 arrives, if the optical voice needs something
`SidechainDetector` does not offer, the answer is still to report it and correct the
component — not to grow it a parameter per voice until it is a union of three
models.

## 2026-08-01 — the prohibition was exercised, and it held (ADR 023)

The next thing to reach for this component was not M13.3 but the wah (§58), the
repo's last remaining separate envelope follower and a follow-up named in this
ADR's own text. **The prohibition above is exactly what bound that slice, and the
outcome is a REFUSAL rather than a widening.**

`SidechainDetector` is a **threshold** detector — measured, open loop, as the
input dB range from 10 % to 90 % of the rail: **1.950 dB** on M13.1's component
values, **2.384 dB** on M13.6a's, against **19.085 dB and no threshold** for the
wah's one-pole on `|x|`. Both existing consumers *want* the steepness (the
compressor closes a feedback loop around it — §59's 216:1 vs 3.3:1; the gate
feeds it to a comparator and §61.4 builds a 40 dB dB-linear threshold on it). A
wah has no gain to reduce, so it is feed-forward by necessity and needs the other
thing. Making both fit would have required a `rectifierKind` /
`asymmetricAttack` axis whose two settings share no component — the union of
three models, named above.

**It was decided by building the substitution, not by comparing block diagrams**,
and that is the procedural rule this leaves behind for M13.3: every §58.6 AUTO
number regressed (full-SENSE sweep 1.534 → 0.958 octaves, failing §58's own bar;
a 0.10 V pick moves the filter 0.000 octaves; time-to-peak stops being
level-independent; `maxAbsRestingState()` exactly 0.0 → 2.675e-13, because the
two designs sit on **opposite sides of ADR 006's scope rule**). Details in docs
**§58.8**; the decision in **ADR 023**. The refusal carries a
perturbation-proven test (`testFollowerLevelLaw`) so it is not only prose, and
this component's header now states the threshold property up front.

## 2026-08-01 — the prohibition was exercised a SECOND time, and it held again (ADR 025)

M13.3's optical compressor arrived and ran the substitution this ADR asked for.
`SidechainDetector` was **not** widened for it either, and the reason is stronger
than the wah's: that pedal has **no envelope capacitor**. Its detector is an
electroluminescent panel lighting a CdS photocell — the panel emits on both
polarities, so the PANEL rectifies and the CELL integrates. Measured on a replica
validated to 0.05 dB: proportional range 2.031 dB here against **14.323 dB** for
the panel-and-cell; the ratio curve goes non-monotone to **10.40:1**; and the
voice's acceptance property collapses from 2.892x to **1.000x**. Docs §64.3.

That slice also found this ADR's *other* half does not extend to the engine: of
`CompressorEngine`'s eight config structs one applies to an optical leveler, so
`OptoModel` is standalone (the shape `GateModel` took) and holds a new component,
`OptoCell`. **ADR 025** records it, and adds the layer above this one: an ENGINE is
not automatically the right engine either, and "two pedals do the same JOB" is a
much weaker signal than "two pedals contain the same SUB-CIRCUIT."
