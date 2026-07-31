# ADR 009: The Muff gets its output coupling cap — and why the series base resistor is a SEPARATE decision

Date: 2026-07-25
Status: Accepted (DC half). The bass half was deferred to ADR 010 and is now **closed** —
docs §49 landed the series base resistor and **ADR 010 / docs §53** landed the DC-blocking
diode caps that this ADR predicted were the real upstream error. See "Why this is split".

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

Add the real pedal's **0.1 µF output coupling cap into the 100 k VOLUME pot** —
`kOutCouplingHz` = 15.92 Hz, one pole, placed after Q4 and **before** the VOLUME multiply
(in the pedal the cap precedes the pot) and **inside** the oversampled domain, so it also
blocks the DC the four asymmetric stages rectify before that DC reaches the decimator.

`kOutputTrim` stays at **0.40**. It is NOT touched here.

## Why this is split

Finding 16 names two defects — no output DC blocker, and almost no bass — and they have
very different evidentiary standing.

The **cap** is unarguable: the pedal has it, every sibling already carries
`dcBlockHz = 12.0` for the same stated reason ("the asymmetric clip produces DC"), and the
golden barely moves (**−0.03 dB broadband, 0.80 dB worst band at 5080 Hz**).

The **series base resistor** is a judgement call, and it accounts for essentially the whole
tone change (**−5.85 dB broadband, 26.04 dB at 1270 Hz** when both land together). Its value
cannot be taken from the schematic without first resolving an upstream discrepancy — see
below. Bundling the two would have bought the uncontroversial fix at the price of arguing
the contested one, so they are separate PRs.

## The deferred half, and why it is not simply "add the resistor the pedal has"

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

**The follow-up slice must, in this order:** establish whether ~1.8 kΩ is right for a Big
Muff clip stage with its collector-base diode network conducting (the model's own comment
puts the real stage at **~4 kΩ**, so the discrepancy is already suspected, not
hypothetical); fix that if it is wrong; and only then choose Rb — ideally the schematic
value. Do not calibrate a third constant against this one.

**And re-derive `kOutputTrim` against the combination, not either half.** The bundled
version raised it 0.40 → 0.45 to hold the TONE 0 corner under the 2.0 V ceiling once the
bass came back. This slice leaves it at 0.40 and measures sustain 1.0 peaking at
**2.0096 V** — marginally over that ceiling already, from the cap alone. Stacking the two
adjustments without re-deriving is exactly how `kFullScaleSecV` ended up absorbing two
separate factor-of-2 mistakes.

## Consequences

**What this makes better**, measured:

* DC on signal: up to **+0.47 V (28 % of peak) → 0.00000–0.00004 % of peak**, across three
  sustain settings and a +0.1 V input-offset case, against a 1 % bar.
* Low end: low E **41 dB down → −4.19 dB** re 1 kHz; A (110 Hz) −1.52 dB; 60 Hz −8.44 dB;
  30 Hz −23.11 dB (still rolled off, as a guitar pedal should be).
* Low end is **unchanged and still broken here** — low E measures **−41.14 dB** re 1 kHz,
  which independently reproduces the audit's "41 dB down" to the digit. It is recorded as
  XFAIL `finding16-muff-almost-no-bass`, measured and printed, not skipped.
* A **±20 V slam still exhausts the Newton cap at 6 of 16** rate × oversampling
  combinations (XFAIL `muff-slam-exhausts-newton-cap`, docs §34). Measured on the deferred
  branch, the series base resistors fix that as a side effect — worst **18 of 60**, a 3.3×
  margin — because a series resistance ahead of the exponential base-emitter junction turns
  an exponential base-current step into a bounded one, which is less stiffness for the
  damped Newton's line search to globalize against. That win belongs to the deferred half
  and is claimed there, not here.

**What it costs:**

* The `muff_twin` golden moves **−0.03 dB broadband, 0.80 dB worst band at 5080 Hz** —
  re-blessed on owner authorisation with that table recorded in `GOLDENS.md`. Removing DC
  from a fuzz is nearly invisible in the spectrum; the audible change is that the output
  stops riding on an offset.
* Every player-observable expectation still passes.
* `main()`'s `ledgerMain` call had to be **restored**: the bundled slice removed it on the
  assumption it would have no XFAILs left, while `core/CMakeLists.txt` still registered
  `clipper_muff_tests_xfail_ledger`. Left that way, `--xfail-ledger` would have run the
  whole suite and exited 0, so ctest would report the ledger entry as **Passed** rather
  than `***Skipped` — turning the line that advertises open defects into a silent duplicate
  test run. Same failure shape as the advisory native job and the existence-only artifact
  check: a guard that looks present and does nothing.
