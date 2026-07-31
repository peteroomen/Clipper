# ADR NNN: The wah's resonant tank is modelled OUTSIDE the transistor's feedback loop

**ADR NUMBER NEEDED.** § and ADR numbers are assigned centrally (CLAUDE.md) and
017 is already taken, so this file is deliberately named `NNN-` rather than
guessing. Rename it and update the cross-references in
`docs/DEVELOPMENT.md` §58.4 and `core/include/clipper/dsp/WahModel.h`.

Date: 2026-07-31
Status: Proposed (pending its number)

## Context

`WahModel` (docs §58) is the lineup's first FILTER pedal — a Dunlop GCB-95-class
Cry Baby. In the real circuit the LC tank does **not** sit in front of the
transistor: it sits **in the feedback/degeneration path of the common-emitter
stage**. Filter and gain are one stage. The transistor's collector drives the
tank, the tank's impedance sets the stage's gain versus frequency, and the whole
thing is one coupled nonlinear system.

Modelling that faithfully means putting a nonlinear per-sample Newton solve
*inside* a resonator whose coefficients change every sample — the tank's state
and the transistor's operating point would have to be solved together, at the
oversampled rate, on every sample of a filter whose ω0 sweeps 2.3 octaves.

Two other facts pushed on the decision:

- The house convention is that nonlinearities live inside a **small** oversampled
  domain and linear filters stay at base rate (CLAUDE.md, "Oversampling"). A
  coupled solve forces the resonator into the oversampled domain too.
- The pedal's headline properties — the sweep law and the resonance height/width
  across the sweep — are **small-signal** properties. They are what makes a wah
  feel like a wah, and they are unaffected by where the nonlinearity sits.

## Decision

**Split them.** The model is:

```
in -> [ TPT state-variable resonator, BASE rate, linear ]
   -> x kTankDivider
   -> [ 4x oversampled: BjtStage common emitter ] -> out
```

and the staging between the two carries **no fitted constant**: the transistor
stage's own small-signal gain `G0` is measured from the model in `prepare()`, and
`kTankDivider = 10^(18/20) / G0` forces the pedal's small-signal resonant boost to
the published +18 dB by construction.

## Consequences

**What this makes easier**

- The resonator's coefficients can be rebuilt **every sample** cheaply, which is
  the single thing a wah cannot compromise on. Measured: the whole pedal costs
  **7.0 % of one 48 kHz stream**, against the four-BjtStage Muff's 30 %.
- The nonlinearity stays in a small oversampled domain: **72 samples** of
  latency, alias floor **−118.8 dB** at the shipped 4× and moving 45.8 dB with
  the factor.
- The small-signal properties are exactly measurable and exactly derivable, which
  is what let §58 pin the sweep law to **0.59 % rms against an independent
  measurement** and the peak height to **0.010 dB of spread across the travel**.
- The published +18 dB is honoured by construction rather than by tuning, and
  where the pedal starts to bark becomes a *prediction* of the component values
  (measured: clean at 0.10 V, 9.1 % THD at 0.50 V) rather than a knob.

**What this costs, audibly**

- **The tank never sees the clipped output.** In the real pedal, slamming the
  transistor loads the tank differently, so a cranked wah's resonance damps and
  detunes slightly under a hot signal. This model's resonance is
  level-independent. A player pushing a fuzz into a wah would hear the real one
  "squash" its peak; this one keeps it.
- **No intermodulation between the sweep and the distortion inside one stage.**
  The filter's output is distorted, but the distortion is not re-filtered by the
  moving tank within the same stage.

**What must NOT happen next**

Do not "fix" either of those by re-fitting `kTankDivider`, `kPeakBoostDb`, or the
`BjtStage` component values — they are a measured identity and a published
figure, and bending them would recreate exactly the failure mode ADR 008 and the
`kFullScaleSecV` history warn about. The honest fix is a **coupled** solve, and
it is its own slice with its own CPU budget.
