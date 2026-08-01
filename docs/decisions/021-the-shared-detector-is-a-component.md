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
