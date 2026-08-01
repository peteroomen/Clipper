# ADR 023: The wah's envelope follower is NOT the shared `SidechainDetector`

Date: 2026-08-01
Status: Accepted
Relates to: ADR 019, ADR 021 (this is the follow-up they both named)

## Context

Three envelope followers were written in this repo within a week, on three
parallel branches that deliberately did not talk to each other:

* M13.1's compressor (§59) — a clamp/rectifier + envelope integrator.
* M13.6a's noise gate (§61) — the same block, different component values.
  ADR 021 extracted it into a real component, `SidechainDetector`, owned by
  value by both.
* §58's wah — its own one-pole peak follower on `|x|`.

The wah's was the last one standing, and "unify the two envelope followers" has
been a named follow-up in §58.5, §58.7, ADR 019, ADR 021 and CLAUDE.md ever
since. ADR 021 also wrote down the thing that must *not* happen:

> when M13.3 arrives, if the optical voice needs something `SidechainDetector`
> does not offer, the answer is still to report it and correct the component —
> not to grow it a parameter per voice until it is a union of three models.

So this slice had exactly two acceptable outcomes: unify, or refuse **with a
measurement**. It refuses.

## The measurement that decided it

`SidechainDetector` is a **THRESHOLD detector**. That is not a criticism — it is
what both of its consumers want — but it is the opposite of what a wah needs.

The metric is the **proportional range**: the input dynamic range, in dB, over
which the detector's output travels from 10 % to 90 % of its own full swing.
That is "how much of a player's pick-strength range the control responds to
proportionally". Measured open loop, on a 146.83 Hz tone, on the shipped
component values of each consumer:

| detector | envelope pair | proportional range |
| --- | --- | --- |
| M13.1 compressor | 10 µF / 150 kΩ | **1.95 dB** |
| M13.6a gate | 47 nF / 220 kΩ | **2.38 dB** |
| best wah-plausible config found | 10 µF / 16.67 kΩ | **6.71 dB** |
| **the wah's shipped follower** | — (ideal `\|x\|` + one-pole) | **19.09 dB, and no threshold at all** |

The compressor's *graded* response is therefore **the feedback loop**, not the
detector — §59 already measured that (216:1 feed-back vs 3.3:1 feed-forward) and
this is the same fact from the other side. The gate's is a comparator, and §61.4
deliberately *builds* a 40 dB dB-linear threshold on top of that steepness.

**A wah has no gain to reduce, so it is feed-forward by necessity** and cannot
borrow the loop. Given the published 166.7 ms release as a hard constraint
(`R_env·C_env`), the range is set purely by the current balance — the sidechain
drive gain is a pure level translation (identical range to 3 decimal places
across three decades of drive). Pushing R_env down widens it, and it reaches the
shipped follower's range only in the limit:

| R_env | C_env | proportional range |
| --- | --- | --- |
| 16.7 kΩ | 10 µF | 6.64 dB |
| 1.67 kΩ | 100 µF | 13.81 dB |
| 167 Ω | 1 000 µF | 17.09 dB |
| 16.7 Ω | 10 000 µF | 18.09 dB |

(The 16.7 kΩ row reads 6.64 here against 6.71 above — the extension sweep uses a
coarser bisection to cover seven decades. Harness noise, and neither figure is
within 12 dB of the follower's.)

That trend is the point: the wah's ideal rectifier **is the R_env → 0 limit** of
this detector — the limit in which the transistor's exponential is swamped by
the pull-up. It is not a component value. A 10 000 µF cap behind a 17 Ω resistor
across a 9 V rail is a power supply, not a detector, and it still measures 1 dB
short.

**The substitution was built and run** (scratch tree, `SidechainDetector` in
`WahModel`, envelope pair pinned to the published release, calibrated at the
follower's own 0.25 V reference level). Every §58.6 AUTO number moved, and three
existing bars go red — the full before → after table is in **docs §58.8**.
Headlines: sweep depth at full SENSE **1.534 → 0.958 octaves** (failing §58's own
`> 1.0` acceptance bar); a quiet 0.05 V pick **0.254 → 0.000 octaves** (the pedal
becomes a static filter until you dig in); time-to-peak **82.7 ms flat → 56–179 ms
depending on level**; `maxAbsRestingState()` **exactly 0.0 → 2.675e-13**, because
the detector's node rests at a nonzero operating point *by design* and is
explicitly not flushed. CPU rises ~27 % (4.8 % → 6.1 % of one 48 kHz stream).

## Decision

**The wah keeps its own envelope follower. `SidechainDetector` is not widened,
and the wah is not made a consumer of it.**

The two blocks share a description ("rectify and integrate") and not a circuit.
Sharing them would require a `rectifierKind` / `asymmetricAttack` /
`idealRectifier` axis on `SidechainDetector` whose two settings have no component
in common — which is exactly the union-of-N-models ADR 021 forbids, arrived at
one slice earlier than ADR 021 expected.

The refusal is given teeth rather than left as prose: `clipper_wah_tests` gains
`testFollowerLevelLaw`, three player-observable bars (a quiet pick still opens
the filter; sweep depth is proportional to pick strength; time-to-peak does not
move with level), each perturbation-proven red under the substitution.

## Consequences

**Easier.** The wah keeps a follower whose behaviour is derived from a published
*behavioural* spec (Geofex's ~10 ms attack / ~500 ms drift-back) rather than from
component values it does not have — which is honest, because **the GCB-95 has no
envelope follower at all**; §58's AUTO mode is a synthesised feature. Forcing it
onto a transcribed netlist would have meant inventing component values to hit a
published time constant, i.e. fitting parts to a target — the thing §57 spent a
whole slice undoing.

**Harder / the cost.** Two envelope followers stay in the repo, and a reader has
to know why. That is what this ADR and §58.8 are for, and the test is the part
that will still be true after the docs rot.

**What this does NOT license.** M13.3's optical compressor is still expected to
reuse `SidechainDetector` — it is a feed-back compressor with a gain cell, the
case the component is built for. "Two circuits both rectify and integrate" is
not a reason to share, and it is also not a reason *not* to: the test is whether
the shared thing reproduces the consumer's measured behaviour. Run the
substitution and measure it, as this slice did.

> **SETTLED 2026-08-01 — see ADR 025, and the expectation above was WRONG.** M13.3
> shipped and it refuses too. It followed this slice's procedure exactly (a replica
> of its own loop, validated against the shipped model to 0.05 dB, with one block
> switchable) and measured a proportional range of **14.323 dB** against this
> detector's 2.031, a ratio curve going non-monotone to **10.40:1** with 0.09 dB of
> gain reduction at −30 dBV, and its whole acceptance property collapsing from
> 2.892x to **1.000x**. The physical reason is one this ADR could not have guessed
> from the category: **that pedal has no envelope capacitor** — an EL panel emits on
> both polarities, so the panel is the rectifier and the photocell is the
> integrator. `SidechainDetector` still has exactly two consumers. What the
> procedure bought was that the question got answered with a number.

**A general lesson, and it is the mirror of ADR 021's.** ADR 021's finding was
"a config struct is not a seam — extract the block." This one's is the other
half: **an extracted block is not automatically the right block for the next
consumer, and the way to find out is to build the substitution and measure it,
not to compare block diagrams.** Both followers rectify and integrate; on the
one number that matters to a wah they differ by 12.4 dB.
