# ADR 017: The AC30's supply is a constant Thévenin source behind a derived rail

Date: 2026-07-31
Status: Accepted

## Context

Audit finding 4 called the AC30's "sag" a static saturator. It was narrower than that:
step 6b of `Ac30PowerAmp::processSampleOS` multiplied the **OT secondary** by
`1/(1 + k·(Idemand − Iidle))` off an envelope of `|ipUp − ipDown|`. Because
`vSec ∝ (ipUp − ipDown)`, that is algebraically `y = x/(1 + k|x|)` — a memoryless soft
clipper *after* the transformer, where no supply can reach. Replacing it with a real
dynamic supply (docs §55) forced two modelling choices where the honest answer and the
"match the published number" answer diverge. Both are departures from the real circuit,
so both are recorded here rather than left as constants a future slice might "fix" back.

**1. How the rectifier's source impedance varies with current.** A GZ34 is a valve: its
own drop is *sub*-linear in current (space charge, ≈ I^⅔ — 75.6 Ω at the published
17 V @ 225 mA, falling toward ~50 Ω at 250 mA). A capacitor-input supply pushes the other
way: as load rises the conduction angle shrinks, which *adds* effective resistance. The
draft carried a `kRectKnee` term making `Reff` rise with current, which models the second
effect and ignores the first.

**2. What sets the rail voltage.** A published AC30 idles with its OT centre tap at 342 V.
This model idles at 309.4904457 V. The obvious move is to raise `kVsupply` until the model
reads 342.

## Decision

**1. A constant Thévenin series resistance** — `kRsupply` = 134.6 Ω
(`kRrectGz34` 75.6 Ω + `kRptSecondary` 59.0 Ω), with the current-dependent knee deleted.
Over this amp's actual 190–260 mA operating window the valve's falling resistance and the
conduction angle's rising resistance very nearly cancel, so a constant is the defensible
model and the growing knee was a voicing knob wearing a physics label.

**2. `kVsupply` is derived to preserve the pre-slice idle point exactly**, not fitted to
the published 342 V:

    kVsupply = 309.4904457 V + 0.1903616816 A × 134.6 Ω = 335.1131 V

The 32 V gap to the published figure is **not** this parameter's error to absorb. Audit
finding 9 owns it: the EL84 screen-current fit runs ~3× hot (12.67 mA/tube against a real
3–5 mA), so the total draw that sets the drop across any source impedance is already wrong
in a way finding 9 is scoped to fix. Closing the gap here would calibrate one constant to
cover another's known error — the exact failure mode that put two separate factor-of-2
mistakes inside `kFullScaleSecV` (docs §42).

Related, and decided the same way: `kRptSecondary` = 59 Ω is the PT HT winding
(118 Ω end-to-end, one half conducting per cycle), **not** the 78 Ω choke DCR the draft
used. In a stock AC30 the OT centre tap is fed from the reservoir *before* the choke; the
7 H / 78 Ω unit is the Brian May / Dave Peterson mod part, and putting it in the plate path
would have been modelling somebody's modification and calling it the amp.

## Consequences

**Easier.** Every number §55 measures is now unambiguously *dynamics* rather than a moved
operating point — the idle rail, Vk, Ip and Ig2 all reproduce to seven significant figures
(309.4904457 → 309.4904201 V; 34.92347 → 34.92347 mA/tube). That is what makes the
before/after sag table trustworthy at all. Removing the knee also removes a constant with
no measurement behind it from a hot loop.

**Harder / the cost.** The model's rail sits ~32 V below a real AC30's, so any future work
comparing absolute plate voltages against a schematic or a real amp must read this ADR
first, and must fix finding 9 rather than `kVsupply`. **Do not raise `kVsupply` toward
342 V without fixing the screen-current fit in the same slice** — doing so would push the
idle point off the value every §55 measurement is referenced to and would bury finding 9
deeper.

The constant-resistance choice is falsifiable and should be revisited if the amp is ever
driven far outside 190–260 mA (a different output valve complement, or a deliberate
over-drive study), where the two cancelling effects no longer cancel.

**Left open, now an XFAIL** (`finding4-ac30-bias-swing-short`): a real AC30's cathode
swings 10.0 → 12.5 V (+25 %) under drive; this model reaches 9.518 → 10.347 V (+8.7 %)
driven to absurdity. The cathode network is already the published one (50 Ω / 250 µF), so
the shortfall is average-current draw — the same finding 9. Do **not** chase it with a gain
term on the cathode integrator.
