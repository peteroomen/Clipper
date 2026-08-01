# ADR 026: The SD-1 / TS tone stage is the netlist, and the level it costs is not given back

Date: 2026-08-01
Status: Accepted

## Context

`OverdriveEngine`'s stage 3 shipped a **documented approximation** from M8 (docs
§11.6) and repeated it for the TS in v1.1 (docs §21): *"a first-order treble TILT
about a ~1 kHz pivot, ±12 dB as TONE sweeps 0..1, TRANSPARENT at noon … This
matches the published SD-1 tone response SHAPE without modelling the exact
10k-pot / 0.018 µF / 0.027 µF network — a documented approximation."*

The owner A/B'd the model against a **real SD-1 they own** and reported a gap in
the **upper-mid body** — how much midrange the pedal pushes and where. That has
been the last open item of the post-v1.1 field-report round.

Measured before anything changed, at DRIVE 0.5 / TONE noon, 1 mV:

* the response rose **monotonically** from 41 Hz and then sat **FLAT from 3 kHz to
  12 kHz** — peak 6467 Hz, with 3 k / 6 k / 12 k at **−0.15 / −0.00 / −0.11 dB**
  relative to it. Not a hump. A **shelf**;
* both pedals measured **identical** there, because both carried the same
  approximation.

The real circuits' tone stages are the same topology with different values, and
each of them puts a **LOW-PASS** on the op-amp's non-inverting input —
1/(2π·1k·220n) = **723.4 Hz** on the TS, 1/(2π·10k·18n) = **884.2 Hz** on the
SD-1. Against the clipping stage's own 720.5 Hz high-pass rise, that low-pass is
the *other half* of the family's band-pass. Without it there is no hump to have.

So the item's own title was confirmed by measurement: the gap is the tone network,
not the clipping stage and not the level staging.

## Decision

**1. The tone stage is the netlist.** `OverdriveToneStack.h` implements the real
network — `Rin`/`Cin` into the non-inverting node, `Rbias` to the (AC-ground) bias
rail, the pot bridging the two op-amp inputs with `Cw`+`Rw` from its wiper to
ground, `Rfb` (with `Cfb` on the SD-1 only) around the op-amp — solved in closed
form and realized as a cascade of first-order sections. `OverdriveConfig` carries
the eight component values per pedal; `tonePivotHz` and `toneMaxTiltDb` are gone.

**2. Nothing is re-gained to compensate.** The SD-1's stage has a DC gain of
exactly `R11/(R5+R11)` = 0.5 = **−6.02 dB** and the TS's of **−0.83 dB**, so the
SD-1 gets about 5.7 dB quieter at every setting and the TS about 1.1 dB. That is
docs §36 / ADR 008's precedent applied again: a circuit correction is reported,
not hidden behind a compensating constant. The M11 A4 windows still hold with
6 dB of margin and were **not** re-snugged.

**3. The DRIVE range is NOT fixed in the same slice, and it is reported.** The
same netlists say the DRIVE feedback leg is the pot **in series with a resistor**
(SD-1 `R3` = 33 kΩ, TS `R3` = 51 kΩ), so the real minimum plateau is
1 + 33k/4.7k = **+18.1 dB** (SD-1) and 1 + 51k/4.7k = **+21.5 dB** (TS), against
the model's +12 dB for both. That is a gain-staging defect, it is independent of
the tone network, and mixing it in would have made this slice's measurement
unattributable — the SD-1's two errors happen to be ~6 dB in opposite directions,
which is exactly the situation in which a combined change proves nothing. Left
open, with its numbers, as the named follow-up.

**4. The discretization is matched-Z plus one Nyquist-matching zero, at BASE
rate.** All poles and zeros of this network are real across the whole knob on both
pedals (worst normalized discriminant +0.0339), so a first-order cascade is
available — the structure docs §56.4b names as the cure for direct forms of order
≥ 2, and the reason a per-tap denormal guard is correct here. Matched-Z puts each
corner on exactly its analog frequency; the extra real zero makes |H| at Nyquist
exact as well as at DC, which is what makes the top octave usable at 44.1 kHz
(without it the mapping over-shoots by 3.2 / 5.1 dB at 20 kHz).

## Consequences

**Easier.** The two pedals now differ where the real ones differ: one engine, two
configs, and the tone stage is part of the config rather than part of the engine.
The response has an upper-mid peak (SD-1 **1106 Hz**, TS **777 Hz**) with a real
rolloff above it — SD-1 6 kHz **−9.34 dB** re peak, TS **−11.75 dB**, against a
flat shelf before. The TS's H(s) is now checkable against a published academic
reference (Yeh & Abel, DAFx-07): it reproduces it to **0.000000 dB**.

**Harder / costs.**

* Two goldens move (`sd1_twin_reverb` −5.99 dB, `ts_ac30` −1.83 dB) and the core
  suite is red at that gate until the owner blesses them. Nothing was written.
* The pedals are quieter. A player who had the LEVEL knob set will want it higher.
  That is the honest consequence of item 2, and the DRIVE-range follow-up will
  give most of it back for the right reason rather than the wrong one.
* Max-DRIVE THD on the 220 Hz probe falls on the TS (31.9 → **24.9 %**) and rises
  slightly on the SD-1 (36.7 → **37.6 %**). Neither is a change in clipping — the
  clip stage is untouched. It is the low-pass acting on the harmonics of a low
  probe tone, and the two pedals' low-passes sit at different distances from their
  own DC gain.
* The top octave carries a **stated** discretization residual: worst 1.16 dB (at
  16 kHz / 44.1 kHz), under 0.53 dB below 10 kHz. The cure, if it is ever wanted,
  is to run the tone network inside the oversampled domain; it is a linear filter,
  so that is a pure accuracy/CPU trade and no circuit argument is involved.

**What must NOT be done next.** Do not answer the level drop by raising
`driveMinDb`, by scaling the tone stage, or by changing `dcBlockHz`. If the level
is to come back it comes back from the DRIVE feedback leg's own series resistor,
measured, in its own slice.
