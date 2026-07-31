# ADR 010: The Muff clip stages' diode branch is DC-blocked, and that costs a Newton node

Date: 2026-07-31
Status: Accepted

**Number note:** 010 was not minted here. ADR 009's own status line reserves it —
"Accepted (DC half). The bass half is deferred to ADR 010" — and this is that slice.

## Context

ADR 009 split audit finding 16 in two and shipped only the uncontroversial half (the output
coupling cap). It refused to pick a value for the clip stages' series base resistor, because
doing so would have meant calibrating a constant against a suspected upstream error, and it
named that error precisely:

> **the clip stages' base-node impedance of ~1.8 kΩ is probably too low.** If it were closer
> to the real circuit's, the schematic's 100 k would land the corner correctly *and* cost
> proportionate gain, and no compromise value would be needed.

It then set the order for the follow-up: establish whether ~1.8 kΩ is right, fix it if it is
not, and only then choose the resistor.

§49 did the first step and landed the schematic's 10 k RS, which took the low E from −41 to
−14.2 dB re 1 kHz and turned the max-sustain blowout into an articulate wall. It could not
finish the job, and said so: the base node was still ~1.8 k because the **diode feedback
branch was DC-coupled**, and blocking it needs a fourth solver node.

The published network (ElectroSmash *Big Muff Pi Analysis*; guitarscience.net *A Case Study:
Re-engineering the Big Muff π*) is RS = 10 k, RA = 100 k to ground, RF = 470 k, and — the
component this model never had — **C6/C7, 1 µF, in series with the antiparallel feedback
diodes**, giving RB = RS//RA//RF = 8.9 k.

## Decision

**1. Model C6/C7, with a fourth Newton node.** `BjtStage::Config` gains `Cdiode`. When it is
> 0 the diode pair sits behind the cap and the junction between them becomes a fourth
unknown, Vd, solved by a 4×4 Gaussian elimination with partial pivoting. `Cdiode == 0` keeps
the existing 3-node Cramer path, **bit-identically** — verified by render digest at 5 rates ×
3 stage shapes, iteration counts included — so Q1/Q4 and every future non-Muff `BjtStage`
user are untouched.

**2. Use `Config::Rbg` (RA = 100 k) at the same time, not separately.** §49 measured Rbg
alone parking the stage deeper in the knee, which is what a bias divider does while the
diodes are still shorting the base at DC. RA and C6/C7 are one decision.

**3. Overturn the "no headroom" canon.** `BjtStage.h` documented the clip stages' idle-
conducting diodes as a fact about Big Muffs ("almost no clean headroom, clips essentially
always"). It was an artifact of the missing cap. With the branch blocked the DC solve finds
the diodes OFF (Vd = Vb, zero branch current) and the stage biases at Vc = 4.95 V / Ic =
0.397 mA — a published clip stage idles near 4 V and ~0.4 mA.

**4. Un-fit `kClipDriveMax`, 6.0 → 1.0, rather than re-tune it.** A real SUSTAIN pot is a
passive divider and can never pass more than unity; 6× was 15.6 dB of gain that existed only
to slam stages with no headroom. The compensation comes off with the error it compensated
for. `kSustainMinDb` is then re-derived −70 → −65 against the *same* §43 player bar.

**5. Fix the step limiter only on the new path.** The damped Newton's ±10 V safety clamp
clips step **components**, which rotates the direction off Newton's; a rotated direction need
not be a descent direction, so the line search can reject all 30 backtracks and the solve
stands still for the entire iteration cap. That is the mechanism behind the
`muff-slam-exhausts-newton-cap` XFAIL. The 4-node path **scales** the direction to the same
bound. The 3-node path deliberately keeps the defective clamp, because fixing it there would
move every existing user's audio and break decision 1.

## Consequences

**Better, measured:**

* Low E (82.4 Hz) re 1 kHz: −41.14 (as filed) → −14.24 (§49) → **−5.48 dB**. Audit finding 16
  is closed; its XFAIL XPASSed and is now a hard assertion.
* ±20 V slam: **5 of 16 rate × oversampling at the Newton cap → 0 of 16**, worst 18 of 60.
  That XFAIL XPASSed too. `clipper_muff_tests` has zero known-bad properties.
* Sustain (the owner's "doesn't scream through"): the output fundamental holds ~1 s longer at
  every setting — at max sustain, +3.275 s over the input's own decay → **+4.175 s**.
* The wall stayed articulate: max-sustain THD 40.8 → 37.6 %, compression 0.09 → 0.40 dB
  across a 20 dB input sweep.

**What it costs:**

* **CPU, substantially.** Interleaved same-machine A/B: 5.62× realtime / 17.8 % of a stream →
  3.2–3.3× / 30–31 %. Two of four stages now run a 4×4 solve per sample, and they swing
  instead of sitting pinned.
* **Idle is no longer free.** The 1 µF cap can only discharge through the diodes' own leakage
  (τ ≈ 5 s), so a played-then-quiet stage is relaxing rather than parked and presents a ~2e-11 A
  residual — six decades above the 1e-17 early-out. Measured 1.75 system evaluations per solve
  against 1.00 fully parked. Audit finding 12's *pathology* (31.00, all 30 backtracks burning)
  has **not** returned: 0.00 % of iterations burn. This is real circuit behaviour, not a solver
  defect, and it is recorded rather than hidden.
* The `muff_twin` golden moves −1.09 dB RMS / 13.18 dB at 252 Hz. Not blessed here.
* One known defect is knowingly left in place: the 3-node path's component-wise step clamp.

**Follow-ups this names:**

1. Exploit the 4×4's structure (J[2][3] = J[3][2] = 0; the emitter row does not see Vd) with a
   specialised solve or a Schur complement onto the 3-node system, to recover the per-sample
   overhead. Its own perf slice, with its own bit-identity bar.
2. Direction-preserving step limiting for the 3-node path — a deliberate tone change for
   RAT/GOLD/SD/TS-adjacent users of `BjtStage` and for Q1/Q4, so it needs its own argument.
3. `Re` is 390 Ω here against the published stage's 100 Ω. Out of scope, and it is why this
   model's clip-stage collector idles at 4.95 V where a real one sits nearer 4.2 V.
