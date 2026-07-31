# ADR 016: The GOLD's 495 Hz summing pole is applied to the dirt branch, and the clean feed stays flat

Date: 2026-07-31
Status: Accepted

> **ADR number check:** 016 was the lowest free number in `docs/decisions/` at the time
> of writing (001–006, 008, 009 ×2, 011, 013, 014, 015 are taken across all branches).
> Numbers are assigned centrally — if a parallel slice claimed 016 first, renumber this
> one rather than merging two 016s.

## Context

The GOLD's summing stage is a transimpedance amplifier (docs §52): three currents meet
at U2A's virtual ground and `R20 = 392 kΩ` turns the sum into a voltage. `C13 = 820 pF`
sits **across `R20`**, so the whole sum is one-pole low-passed at

    1 / (2π · 392 kΩ · 820 pF) = 495.06 Hz

and the tone stage after it puts the top back. That pole is the single largest reason a
clipped Klon reads *creamy* rather than bright: it attenuates the clipper's harmonics far
harder than the fundamental (−0.8 dB at 220 Hz, −7.1 dB at 1 kHz, −15.8 dB at 3 kHz).
This model has never had it. §52 measured its absence as the best available explanation
for the owner's field report ("100 gain and it sounds like a marshall at mid-high gain")
and named it as the second of three coupled defects — but did **not** port it, for one
concrete reason:

> "It **cannot** be ported alone: on this model's flat clean feed it would put a ~−14 dB
> midrange hole in the GAIN-0 path and destroy the transparency spec."

That is the real constraint. `GAIN 0` transparency is a **product contract** in this
model (`clipBlendAt(0) == 0` exactly, the box is a clean buffer), pinned by
`testTransparency`, by the web "transparent at min gain" Playwright spec, and by render
hashes. Putting a 495 Hz one-pole on the composed output would take 1 kHz down 7 dB with
the knob at zero.

So the pole needs a decision, not just an implementation.

## Decision

**Apply the pole to the DIRT branch only, and leave the clean feed exactly as it was.**

In the render loop this is one line — the clipped node voltage passes a 495 Hz one-pole
before it is weighted and summed — but it is not a shortcut, and the reason is algebraic:

    LP(clean_feed · LP⁻¹  +  dirt)  ==  clean_feed  +  LP(dirt)

i.e. "pole on the dirt alone" is **identical** to "pole on the sum, with the clean feed
pre-emphasized by the pole's inverse". The question is therefore whether pre-emphasizing
the clean feed is defensible, and it is — because this model has idealized the *composed*
clean path as flat since §27, and the real one is not flat:

| f (Hz) | real composed clean, `R20·G_clean·|pole|` | this model, `kSumGain·cleanBlend` |
|---|---|---|
| 82   | 2.251 | 2.0 |
| 220  | 2.084 | 2.0 |
| 500  | 1.494 | 2.0 |
| 1000 | 0.758 | 2.0 |
| 3000 | 0.114 | 2.0 |

The real feed falls ~9.5 dB from 82 Hz to 1 kHz. "Flat 2.0" was **already** a
normalization of a shaped path (§52's own independent check quoted `R20·G_clean` =
1.82–2.28 *without* the pole and called the flat 2.0 "inside ±0.8 dB of it"). Carrying
that normalization one stage further to include the pole changes nothing about the clean
path — it is the same idealization, now stated correctly.

What it buys is that the **dirt** path becomes *exactly* the real composed dirt transfer:

    D_model(f) = LP(f) · (R20/R16) · V_node(f)  ==  D_real(f)

The dirt side gets strictly more faithful; the clean side is untouched, bit for bit.

**Consequence that is not a coincidence:** GAIN 0 stays bit-exact. Measured FNV-1a over
four renders at 48 kHz / 4× (220 Hz 0.15 V sine, 0.5 s white noise, and the sine at both
TREBLE extremes) — `85a97e9efc5686ba`, `d10d3ffca9077b36`, `449ef98662e22ec2`,
`2217f25842819f17` — **identical before and after this slice**. The plan file's fallback
contract (|H| within 0.25 dB, documented) was therefore never needed.

The pole runs **inside** the oversampled domain, where the dirt is computed. That is a
free bonus: it band-limits the clipper's products before the decimator sees them, and it
is part of why §52's `gold-summing-alias-at-treble-max` XFAIL now measures −92.9 dB at 4×
instead of −26.5 dB.

## Consequences

**Easier.** The creamy filter exists at all, and it is a component value (`C13 = 820 pF`
across `R20 = 392 kΩ`) rather than a voicing constant. The transparency contract needed no
renegotiation, so no golden, no web spec and no player-expectations GAIN-0 row moved. The
dirt path's composed transfer is now checkable against the reference netlist directly
(`testClippingStageFidelity` asserts 3 kHz sits ≥ 12 dB below 500 Hz in the dirt path;
without the pole it is 4.9 dB).

**Harder / the honest cost.** The model's dirt-vs-clean *relative* spectrum now differs
from the real pedal's by exactly the amount the clean idealization already differed — the
real unit low-passes its clean feed too, and we do not. At the summing node the real
pedal's clean fundamental is ~7 dB darker at 1 kHz than ours, so a real Klon's clean core
is duller under the dirt than this model's. Whether that is audible at guitar
fundamentals (where |pole| ≥ −3 dB below 500 Hz) is not established here; making it
faithful means porting the FF1/FF2 feed-forward networks and re-deriving `kSumGain` as a
frequency-dependent transimpedance, which would end the flat-clean idealization and with
it the bit-exact GAIN-0 contract. That is a product decision, not a modelling one, and it
belongs to the owner rather than to this slice.

**Do not "simplify" this by moving the pole onto the sum.** It would look more faithful,
read more faithful in a diff, and silently break the transparency contract the pedal is
named for.
