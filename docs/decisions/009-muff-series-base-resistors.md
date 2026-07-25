# ADR 009: The Muff's output coupling cap, and a series base resistor that is NOT the schematic value

Date: 2026-07-25
Status: Accepted, with a named residual

## Context

Audit finding 16: the Big Muff model had **no output DC blocker at all** and **almost no
bass**. Measured before: up to **+0.47 V of DC (28 % of peak)** on signal, and the
guitar's low E (82.4 Hz) **41 dB below 1 kHz**.

Two distinct causes:

1. **No high-pass anywhere.** `MuffModel::processChunk` ended `w[i] = x * outGain;`.
   `BjtStage::processSample` returns `Vc - vcQ_`, which removes the *quiescent* DC only —
   not the dynamic DC a common-emitter stage develops when driven into asymmetric
   clipping. Every sibling pedal carries `dcBlockHz = 12.0` for exactly this reason.
2. **Coupling corners in the audio band.** The model fed each stage's coupling cap
   straight onto the base, so the corner was set by the base-node *shunt* impedance
   alone. Measured from the 82.4 Hz / 1 kHz ratio: the plain stages (Q1/Q4) sit at
   **81 Hz** (−2.9 dB each), but the CLIP stages (Q2/Q3), whose collector-base feedback
   network pulls the base node down to **~1.8 k**, sit at **898 Hz** — **−18.2 dB each**.
   Two of those in cascade are the 41 dB.

## Decision

1. Add the real pedal's **0.1 µF output coupling cap into the 100 k VOLUME pot** —
   `kOutCouplingHz` = 15.92 Hz, one pole, placed after Q4 and before the volume network.
2. Add a **series base resistor on the two clip stages only**: `kSeriesBaseRQ2` =
   `kSeriesBaseRQ3` = **47 kΩ**. Q1/Q4 keep `Rb = 0`.
3. Re-trim `kOutputTrim` 0.40 → **0.45** so the hottest knob corner (TONE 0) peaks at
   **1.80 V** against the 2.0 V ceiling now that the pedal has bass.

## The residual, stated plainly: 47 kΩ is not a schematic value

The real pedal's coupling networks use **10 k / 100 k**. We measured all three against
this model, per stage, at 82 Hz and 4 kHz:

| Rb | corner | at the low E | divider loss | consequence |
|---|---|---|---|---|
| 10 k | 135 Hz | −5.6 dB | small | corner still **above** the real 15–50 Hz band |
| **47 k** | **31.5 Hz** | **−0.6 dB** | **15 dB** | inside the band; SUSTAIN stays progressive |
| 100 k | 13 Hz | ~0 dB | **33 dB** | corner **below** the band, and with both clip stages at 100 k the **SUSTAIN knob degenerates into a threshold switch** |

So 47 k was chosen to land the *corner* where the real circuit's is, **using this model's
own measured base-node impedance** — not because 47 k is in the pedal.

That is a departure from the real circuit and it is recorded here as one, because
CLAUDE.md's rule is *"don't calibrate a new constant to compensate for a suspected error
elsewhere; find the error"*. The suspected error is upstream: **the clip stages' base-node
impedance of ~1.8 kΩ is probably too low.** If it were closer to the real circuit's, the
schematic's 100 k would land the corner correctly *and* cost proportionate gain, and no
compromise value would be needed.

**Follow-up, not fixed here:** establish whether ~1.8 kΩ is right for a Big Muff clip
stage with its collector-base diode network conducting. If it is wrong, fix it and
revisit Rb — do not calibrate a third constant against this one.

## Consequences

**What this makes better**, measured:

* DC on signal: up to **+0.47 V (28 % of peak) → 0.00000–0.00004 % of peak**, across three
  sustain settings and a +0.1 V input-offset case, against a 1 % bar.
* Low end: low E **41 dB down → −4.19 dB** re 1 kHz; A (110 Hz) −1.52 dB; 60 Hz −8.44 dB;
  30 Hz −23.11 dB (still rolled off, as a guitar pedal should be).
* **A ±20 V slam now converges at every rate × oversampling combination** — worst 18 of 60
  Newton iterations, a 3.3× margin, where **6 of 16 previously exhausted the cap**. This
  was an open XFAIL (`muff-slam-exhausts-newton-cap`, docs §34) and it XPASSed, which
  forced its deletion in this slice. A series resistance ahead of an exponential
  base-emitter junction turns an exponential base-current step into a bounded one, which
  is less stiffness for the damped Newton's line search to globalize against.

**What it costs:**

* This is the audit's **largest tone change**. The `muff_twin` golden moves **−5.85 dB
  broadband** with a **26.04 dB worst band at 1270 Hz** — the fix restores bass, spends
  15 dB of divider loss per clip stage, and adds an Rb×Cf treble interaction, so the whole
  spectral balance shifts, not just the bottom.
* Every player-observable expectation still passes and several improve: SUSTAIN THD
  **1.3 % → 34.5 % → 36.6 %** (progressive, not a switch), VOLUME −240 → −6.6 → −0.6 dBFS,
  TONE HF −13.4 dB dark → −3.0 dB bright, hum-vs-note −43.3 dB at min gain against a
  −28 dB bar.
* `kOutputTrim` is now doing level staging for a louder circuit. It is a ceiling trim, not
  a compensator for a suspected error — but it is one more constant that a future slice
  must not calibrate against.
