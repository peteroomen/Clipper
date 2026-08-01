# ADR 020: The CE-1 un-gangs INTENSITY rather than shipping a dead knob

Date: 2026-08-01
Status: Accepted

## Context

The real Boss CE-1's front panel is asymmetric. **CHORUS** mode has a single
**INTENSITY** control — a ganged pot that moves LFO rate and depth together, with
the rate being the part a player actually hears. **VIBRATO** mode has two:
**RATE** and **DEPTH**.

This project's pedal ABI gives every pedal three parameter slots, and the CE-1
needs MODE as one of them. Reproducing the factory gang faithfully would therefore
leave one of the remaining two slots doing **nothing at all** whenever the pedal is
in chorus mode — which is the majority of its use.

CLAUDE.md forbids that in terms: *"Don't ship dead UI (controls wired to nothing,
or a knob whose top half does nothing)."* That rule exists because this project has
already shipped one — §43's Muff SUSTAIN taper, where the bottom half of the knob
travel did essentially nothing and the owner reported it as a defect.

So the two rules collide: model the circuit faithfully, or don't ship dead UI.

## Decision

**RATE and DEPTH are independent controls in both modes.** MODE selects which rate
**range** and which LFO **waveform** apply (chorus: 1.0–3.0 Hz triangle; vibrato:
3.2–11.6 Hz sine — both sourced). A player who wants the factory chorus feel moves
RATE alone and leaves DEPTH where it sits.

The gang is *not* modelled. This is a deliberate departure from the real control
layout, and it is the only one in the slice — the signal path itself is the JC-120
chorus circuit this project already models, with the CE-1's sourced knob ranges,
waveform and mono output.

## Consequences

**Easier.** Every knob does something at every setting. The pedal is also more
useful than the original in a DAW, where RATE and DEPTH are separately
automatable — a slow, deep chorus is reachable here and is not on a real CE-1.

**The cost, stated plainly.** The chorus mode's *knob feel* is not the real
pedal's. A player who knows a CE-1 will find that INTENSITY's single-axis sweep —
where rate and depth rise together — cannot be reproduced by moving one of our
controls. Settings exist in this model that the hardware cannot reach.

**Recorded so it is not "discovered" later.** A future slice reading the CE-1's
panel could reasonably conclude the gang is missing and re-introduce it as a
fidelity fix. It is not missing; it was traded away, once, against the dead-UI
rule. If that trade is ever revisited, the honest options are (a) gang them and
accept a dead slot in chorus mode, (b) gang them and spend the freed slot on
something real, or (c) keep this. Do not silently pick (a).

**Not covered by this ADR.** The stereo image. A real CE-1 is mono-in/stereo-out
and this project's pedal chain is mono, so the width is lost — that is a chain
limitation, not a control-layout decision, and it is documented in §62.5.
