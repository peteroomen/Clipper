# ADR 026: The lamp is its own component, and `OptoCell` stays unwidened

Date: 2026-08-10
Status: Accepted

## Context

`OptoCell` (ADR 021, ADR 025, docs §64) models an electro-optical attenuator cell
— an electroluminescent panel lighting a CdS photoresistor — and shipped with
M13.3's optical compressor as its first consumer. Its header names its second
consumer by name and gives an instruction in terms:

> NAMED FUTURE CONSUMER: ROADMAP **M13.5, the Uni-Vibe** … That is the same device
> with a different light source (an incandescent lamp, whose own thermal lag
> belongs to the LAMP and not here) and different component values. **Do not grow
> this class a `lightSourceKind` axis to cover it:** the EL law lives in
> `elExponent`, and a lamp's drive law is a different function of a different
> quantity. Extract THAT if and when it arrives.

M13.5 is that consumer. This ADR records what the extraction actually turned out
to be, because the seam is slightly different from what the instruction predicted
and the difference is the interesting part.

Two earlier seam predictions in this repo came out NO when their consumer arrived
(ADR 019's `CompressorEngine` config, ADR 021's `ControlMap`), and ADR 023 left
the standing procedure: **build the substitution and measure it**, do not argue.

## Decision

**1. The lamp is extracted as `core/include/clipper/dsp/LampDrive.h`, a
header-only component held by value, and `OptoCell` gets ZERO new fields and ZERO
new methods.**

`LampDrive` is a driven tungsten filament: a lumped heat capacity, electrical
heating `v²/R(T)` with tungsten's positive temperature coefficient `R ∝ T^ρ`,
radiative cooling `k(T⁴ − T_amb⁴)`, and luminous output in the cell's own
sensitivity band as `L ∝ T^m`. It converts DRIVE VOLTS to NORMALIZED LIGHT.

**2. The seam is `elExponent = 1.0`, and that is the whole of it.**

`OptoCell` already maps its drive quantity to light by a power law and light to
excess conductance by the published CdS gamma. The EL panel it was written for
supplies the first law itself, so its drive quantity is a voltage. A lamp does
not. Setting `elExponent = 1.0` (with `panelMaxV = 1.0`) means "the quantity I am
handing you IS the light" — the existing, published-source field used with a
degenerate exponent. The source law then lives entirely in `LampDrive`, which is
exactly where `OptoCell.h` said to put it.

**3. The instruction's stated REASON was right, and it is now measured rather
than asserted.** The header says a lamp's drive law is "a different function of a
different quantity". It is stronger than that: **a lamp's light is not a function
of the drive at all, it is a function of a STATE.** `clipper_vibe_tests`'
`testOptoCellIsUsedUnwidened` hands two identically-configured `LampDrive`s the
same 0.66 V after different histories and measures **21.43×** different light. No
`lightSourceKind` case on `OptoCell` could have expressed that, because the cell's
drive→light map is memoryless by construction.

**4. The rise/fall asymmetry is DERIVED and there is deliberately no
`riseTau`/`fallTau` pair on `LampSpec`.** Heating is whatever the drive can force
through a resistance that falls as the filament cools (the inrush); cooling is
only what the T⁴ law removes, and it collapses as T drops. `k` and the heat
capacity are both derived — `k` from steady state at the nominal operating point,
`C` from one honest small-signal time constant. Measured consequence: the sweep
rises faster than it falls, by **1.17× at 0.57 Hz rising to 1.38× at 3.20 Hz**,
against `PhaserModel`'s **1.00** on the identical metric.

**5. The cell's DYNAMIC constants are the Uni-Vibe's own, not the LA-2A's, and
that was forced by a published figure rather than chosen.** `OptoCell`'s default
card is pinned to the LA-2A's published 10 ms attack and 0.5–5 s complete release.
Reusing it unchanged is refuted by the Uni-Vibe's own published top speed of
7.6 Hz (half period 65.8 ms): a cell whose slow branch releases over 180 ms–2.7 s
cannot deliver the "intense vibrato" the sources describe there. The release is
therefore pinned by the standard first-order criterion — the modulation must
survive to at least half depth at that published speed, τ = √3/(2π·7.6) = 36.3 ms.

## Consequences

**Easier.** A third consumer of `OptoCell` (any photocell circuit) and a second
consumer of `LampDrive` (an opto tremolo with a real bulb, a photocell-based
compressor with a lamp instead of a panel) can each hold what they need without
either class growing an axis. The two device cards are now genuinely orthogonal:
`LampSpec` says how bright the source is, `OptoCellSpec` says what the cell does
about it.

**Harder / the honest costs.**

- **`LampSpec` is entirely RECONSTRUCTED.** Only "it is a miniature incandescent
  lamp" is sourced. The structure is textbook filament thermodynamics, but
  `operatingK`, `thermalTauSeconds`, `brightnessExponent` and
  `resistanceExponent` are this model's own. §57's rule applies verbatim: do not
  re-tune them toward a sound; find the schematic.
- **A denormal contradiction, and it is the same class on both sides of ADR 006's
  scope rule.** Inside `OptoModel` the cell's three states rest at EXACTLY zero
  and are asserted to; inside `VibeModel` they rest at a real operating point,
  because a Uni-Vibe's lamp is never off. `VibeModel::maxAbsRestingState()`
  therefore covers the four allpass memories and *nothing else*, and says so.
  This mirrors ADR 023's finding for the wah's follower — one object cannot be on
  both sides — except that here the two consumers are the SAME class and it is
  the light source that decides.
- **Four cells are held where one would compute the same number.** They are four
  physical devices sharing one lamp and one reflective box, so they are
  configured identically and a test asserts they agree exactly. Inventing a
  per-cell tolerance today would be exactly the fitting §57 forbids; the four
  objects are where a sourced one would go.
