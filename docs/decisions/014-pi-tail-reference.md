# ADR 014: The phase-inverter tail reference, and un-fitting the constants that were calibrated around its absence

Date: 2026-07-29
Status: Accepted

## Context

`LtpInverter` — the long-tailed-pair phase inverter shared by all three valve amps — modelled
the tail as a **two-terminal** network: `Rtail` straight to ground. In a real long-tailed pair
the tail returns through a large resistor to a **negative reference**, and that separation is
the whole point of the topology: the resistor sets the **common-mode rejection**, the
reference sets the **standing current**. Collapsed into one number, a model can have one or
the other. Audit finding 7 measured the cost: the JCM800 and the Twin idled at 94–95 % of B+
at 0.18–0.23 mA per triode (a textbook 12AX7 PI runs 0.5–0.9 mA, plates at 70–85 % of B+), and
the AC30 — which §23 had forced onto the right DC point by shrinking `Rtail` to 2.2 kΩ — had a
leg-gain ratio of 0.550, i.e. it had bought its operating point by destroying the long tail.
Four of nine project PI targets were met; five were XFAILed.

Fixing that roughly doubles each inverter's leg gain, which turns out to be the smaller half
of the decision. Three separate downstream calibrations had been fitted **around** the defect
and had been quietly absorbing it: the two composed amps' interstage staging trims, their
output normalizations, the Twin's phase-inverter plate load, and six test probes/bounds. ADR
008 (the diode ideality factor, whose comparison constant had been fitted to the bug) already
set the precedent for what to do about that.

## Decision

**1. `LtpInverter::Config` gains `tailRef` (volts), the node the tail returns to.** The tail
current becomes `(Vk − tailRef)/Rtail`; the Jacobian is unchanged and `tailRef = 0` is the old
behaviour bit-for-bit (verified: 27/27 ctest, all five goldens digit-identical, before any amp
config moved). It is a **model parameter** standing in for the real circuit's negative return
network, calibrated per amp to land the documented operating point with `Rtail` at its real,
long value — not a parts-bin voltage.

**2. Constants that were fitted around the starved inverter are re-derived by measurement in
this same slice** (the ADR 008 precedent), each against **its own** documented convention so
that no constant absorbs another's error:

| constant | before → after | its convention |
|---|---|---|
| `Jcm800Amp::kInterstageScale` | 0.25 → **0.16** | restores the power-tube grid drive: the power section's measured closed-loop gain ratio, 1.577 over 82–880 Hz |
| `TwinAmp::kInterstageScale` | 0.16 → **0.107** | same, ratio 1.490 |
| `Jcm800PowerAmp::kFullScaleSecV` | 26 → **33 V** | §23's rule — normalization follows the measured cranked swing (cranked peak back to 0.89) |
| `TwinPowerAmp::kFullScaleSecV` | 24 → **42 V** | same (cranked peak back to 0.898, from 1.571) |
| Twin PI `Ra2` | 142 k → **119 k** | §20's own "legs balanced to ~1 %" (measured 0.9978) |

Explicitly **not** divided by the PI's gain ratio: the global NFB absorbs about half of it, so
the measured closed-loop figure is the only defensible one.

**3. The Twin's tail keeps its 22 kΩ length rather than the 10 kΩ on the schematic.** The
audit and the slice plan both called for 10 k. Measured, that costs 6.8 dB of tail impedance
= 6.8 dB of common-mode rejection, and this amp injects its global feedback **single-endedly
into the cold grid**, so half the feedback signal is common mode; what leaks through arrives
in phase on both output grids, which a push-pull pair cannot cancel, and emerges as 2nd
harmonic. Closing the loop then *added* 24 dB of h2 (vs *removing* 1.4 dB before the fix), and
the amp's documented clean-headroom bar went from 2.96 % to 4.5 % THD — a fail. At 22 k it is
3.41 % and passes. In a model whose only common-mode rejection is one resistor, that resistor
stays long.

**4. The audit's Twin `Ra2` 142 k → 100 k proposal is rejected on measurement.** With the tail
fixed, matched 100k/100k loads give a leg ratio of 0.718 and nothing legal reaches 0.90;
unequal plate loads are *how* a finite-tail LTP is balanced. Following the brief there would
have turned a green assertion red (proved by perturbation).

**5. The AC30's lost character is ledgered, not restored.** Balancing the AC30's legs
(0.550 → 0.912) removed the even-harmonic leakage that §23's breakup-ordering guard and the
chime guard had been measuring — h2 at VOLUME 0.6 fell from −19.76 to −33.03 dBc while the odd
(clipping) harmonics did not move. Restoring those numbers needs drive the passive interstage
divider cannot supply (§23 measured the missing gain stage at +30…+35 dB), and re-introducing
the imbalance would re-break the leg-balance assertion this slice made hard. So all five bars
are kept **verbatim** as `expectXfail` entries owned by an AC30 gain-structure slice. An XPASS
is a hard failure, so they cannot rot.

**6. Test probes expressed in volts at the PI grid are re-derived, not the bounds.** Six of
them had been calibrated against inverters that were ~5.7 dB deaf (§23 had already retuned two
for exactly this reason). Each re-derivation is checked against the pre-fix circuit as well as
the post-fix one — a fixed reference must improve, or at least hold, on the old circuit too.
One genuine bound change is called out as such in docs §42.9 (the Twin's cranked-breakup
threshold), together with a *new*, harder assertion — odd-harmonic-only THD — that an
unbalanced inverter cannot fake.

## Consequences

- **Eight of the nine PI targets are hard assertions** (was four), and the five finding-7
  XFAILs are deleted. The remaining miss is finding 8's `Ra2` on the JCM.
- **Both fixed-bias amps reach their rated power for the first time**: the JCM's cranked
  secondary 23.4 V → 29.3 V (34 W → 54 W of a 50 W amp), the Twin's 22.4 V → 37.7 V (31 W →
  89 W of an 85 W amp). Because full scale is re-referenced to that swing, both voices are
  **quieter below clipping** — JCM ~2.0 dB, Twin ~4.9 dB — and correspondingly have that much
  more headroom above their clean range. That is a deliberate, documented trade, and it is
  forced: any Twin normalization keeping the cranked peak inside full scale costs ≥ 3.9 dB.
- **Four goldens move** (`rat_jcm800` −1.77 dB, `sd1_twin_reverb` −4.89, `muff_twin` −4.74,
  `ts_ac30` +0.44) and are deliberately **not re-blessed**. `clean120_chorus` is unchanged at
  the gate's quantisation floor, which is the scope check.
- **The AC30 is measurably cleaner at mid volume** (7.17 % → 2.34 % THD at VOLUME 0.5) and has
  lost most of its 2nd-harmonic chime. This is the honest consequence of removing a defect
  that had been doing that voice's work, and it is the single largest open item the slice
  creates.
- Nothing about the audio thread, allocation, or the chain's declick discipline changes: both
  new constants are memoryless gains and `tailRef` costs nothing (no measurable CPU change,
  interleaved A/B).
- **The next person to touch a valve-amp calibration constant should read docs §42 first.**
  The pattern — a constant fitted around a defect, silently keeping a suite green — has now
  cost two slices (ADR 008, this one) and it is not obviously exhausted.
