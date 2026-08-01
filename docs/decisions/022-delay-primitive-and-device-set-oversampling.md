# ADR 022: The delay line is a primitive; the DEVICE sets the oversampling factor

Date: 2026-08-01
Status: Accepted

## Context

M13.4 adds the lineup's first delay — an EHX Deluxe Memory Man-style BBD analog
echo (docs §60). It is a new DSP family, and two of its decisions are the kind a
later slice could reasonably "fix" back without knowing why they were made.

**First: what belongs to the delay line and what belongs to the device.** A
bucket-brigade echo is not one thing. There is a ring buffer with fractional
readout, and there is everything that makes it a *BBD* — the clock law, the
charge transfer efficiency, the compander, the anti-alias and reconstruction
filter pair, the feedback path. Folding them together is smaller code today and
is what most single-pedal implementations do.

**Second: the oversampling factor.** This project's convention is unambiguous:
*"4× is the measured default for every nonlinear stage."* It is stated in
CLAUDE.md, it is what every oversampled pedal in the tree uses, and a slice
shipping 8× looks at a glance like a slice that did not read the convention.

## Decision

**1. `DelayLine` is a primitive with no knowledge of the device.** It is a plain
interpolating ring buffer — write, read at a fractional distance, that is all. It
does not know what a bucket brigade is, has no feedback path, no compander, no
knobs. `DelayModel` is the BBD behaviour built on top of it. The primitive
carries its own bars in the delay suite (§60.7) rather than being an untested
header that happens to work in its first consumer.

**2. The oversampling factor is 8×, and it is derived from the device, not
chosen against the house convention.** The BBD's clock reaches **136.53 kHz** at
the short end of the DELAY travel. The internal rate must therefore clear
**2 × 136.53 = 273.07 kHz**, or the *device's own* clock images fold inside the
oversampled domain before the reconstruction filter can reach them. At 44.1 kHz
base, 4× is 176.4 kHz and does **not** clear it; 8× is 352.8 kHz and does.

The derivation is confirmed by measurement rather than asserted — worst
non-harmonic product in the wet signal at DELAY 0.0 on a 3 kHz tone:

| factor | internal rate (48 kHz base) | worst non-harmonic |
| --- | --- | --- |
| 1× | 48 kHz | −70.94 dB |
| 2× | 96 kHz | −66.08 dB |
| 4× | 192 kHz | −65.49 dB |
| **8×** | **352.8 / 384 kHz** | **−122.08 dB** |

**56.58 dB in one step, exactly where the derivation says the step should be.**
1× → 4× buys 5.5 dB; the whole benefit is in the step that clears the clock.

## Consequences

**Easier.** M13.6's flanger and any future tape echo inherit a tested delay
primitive instead of re-deriving one. The BBD's own aliasing — which is *correct*
at the long end of the travel, where a 9 kHz tone into a 7.45 kHz clock folds
hard and a real DMM does the same — stays a property of the modelled device
rather than an artefact of the host's sample rate.

**The cost, stated plainly.** 8× costs **2.9 % → 5.4 %** of one 48 kHz stream
against what 4× would cost. Latency does **not** move, because the dry path never
enters the oversampled domain (which is also what keeps BLEND 0 bit-identical).

**What this does NOT license.** 8× here is not a precedent for raising the factor
anywhere else. It is 8× because a *sampled device inside the model* has a clock
above 4×'s Nyquist — not because more oversampling is better. Any other slice
reaching for 8× owes the same derivation and the same table; without a clock to
clear, the measured answer is still 4× (see the 1× → 4× rows above, which move
5.5 dB, and §33/§7's own alias tables).

**Two things a later slice must not silently undo.**

* **Do not fold `DelayLine` into `DelayModel`** because "it is only used once."
  It is used once *today*; the separation is the deliverable.
* **Do not drop the factor to 4× for CPU** without re-deriving the clock. The
  56 dB is not headroom, it is the difference between modelling a BBD and
  modelling its foldover.

**Named follow-up, recorded here so it is not mistaken for an oversight.**
`ChorusModel` has its own cubic-interpolating ring buffer, written before this
primitive existed, and it still indexes with `%` (docs §32's measured cost).
Folding it onto `DelayLine` is fidelity-sensitive — `ChorusModel` is in the
`clean120_chorus` golden — so it is its own slice with a bit-identity bar, not a
tidy-up to be done in passing.
