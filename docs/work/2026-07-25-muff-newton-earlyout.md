# Muff Newton residual early-out (audit finding 12)

**Date:** 2026-07-25
**Branch:** perf/muff-newton-earlyout
**Roadmap item:** `docs/audits/2026-07-24-project-audit.md` finding 12 — "The Muff costs 3.2× more CPU when you are *not* playing"

## Goal

`BjtStage`'s damped Newton stops burning its 30-step backtracking line search on an
already-converged system, so the Muff's idle CPU drops to (or below) its playing CPU.

## Approach

The residuals `r[0..2]` are **node currents in amps** (KCL sums). The line search accepts a
trial step only on a *strict* residual decrease (`infNorm3(rt) < cur * (1.0 - 1e-4*lam)`), and
the only loop exit is on step size (`|lam·dx| < 1e-9` V). At the quiescent operating point the
residual is already at the floating-point floor, so no trial step can make it strictly
smaller: the search burns all 30 backtracks, every iteration, each one a full Ebers-Moll
system evaluation with 4 `std::exp` calls.

Fix: a **residual-norm early-out at the top of the iteration**, tolerance in amps, scaled by
the existing `tubeSolverTolScale()` (`TubeSolverMode.h`) rather than a second knob.

Fidelity intent going in: **bit-identical**, since the solution should not change. See "What
actually happened" — that turned out to be false, and disproving it is the slice's main
finding.

## Steps

- [x] Instrument the residual ∞-norm distribution; pick the tolerance from the data
- [x] Capture baseline raw-float renders (sustain × input-level grid) for the bit-identity diff
- [x] Add the residual early-out + the `tubeSolverTolScale()` hook in `BjtStage.cpp`
- [x] Fix the `it + 1` iteration over-report
- [x] Diff old binary vs new binary, byte-for-byte, over the whole grid
- [x] Sweep the tolerance to find whether bit-identity is reachable at all
- [x] Extend `test_tube_solver.cpp` to cover the Muff (the BJT solver was not in the gate)
- [x] Add an idle-solver-cost test and a ±20 V slam convergence sweep to `test_muff_model.cpp`
- [x] Perturb both directions to prove the new assertions have teeth
- [x] Re-measure the silence/signal table and the `clipper-bench` Muff row
- [x] Update `docs/DEVELOPMENT.md` §34 + the §25.3 bench row, the header comments, `CLAUDE.md`

## How this will be measured

1. **Bit-identity** — a scratch renderer dumps raw `float` buffers for 5 sustain settings
   (including MIN and MAX) × 5 input levels (silence, 0.001, 0.20, 1.00, ±20 V slam), built
   against the pre-change and post-change library, compared with `cmp`.
2. **Silence-vs-signal ratio** — wall ms for 10 s of audio at 48 kHz in 128-frame blocks,
   silence vs hot signal, interleaved before/after to control for machine drift.
3. **`build/clipper-bench --unit muff`** — the docs §25.3 per-unit row, before → after.
4. **Solver-accuracy gate** — `clipper_tube_solver_tests`, extended to the Muff.
5. **Goldens + full core suite** — must be untouched and green.

## Manual test steps

- [x] `ctest --test-dir build --output-on-failure` — 24/24, 6 XFAIL ledgers Skipped
- [x] `build/clipper-bench --unit muff` before → after
- [x] Edge case: ±20 V slam (double the existing ±10 V test) across every rate × oversampling
      — output finite and bounded, iteration count within the cap
- [x] Edge case: sustain at MIN and at MAX
- [x] Edge case: reference mode (`setTubeSolverReferenceMode(true)`) — the early-out tightens
      below the idle residual ceiling and stops firing, which is what makes the reference
      render a genuine ground truth
- [x] Edge case: does the early-out still fire at 192 kHz × 8 oversampling? (Yes — the idle
      residual ceiling is rate-independent)

## Out of scope for this session

- The other solver-perf items in the audit's performance list: the duplicated `exp()` in the
  tube solvers (item 4), the per-sample divides (item 5), the redundant `settleDC()` (item 9).
- The same line-search pattern in the *valve* solvers — `TriodeStage` and the power amps use a
  plain step-size exit with no backtracking, so finding 12 does not apply to them.
- The control-rate parameter-sampling XFAIL (worst case: the Muff). Untouched — it is about
  which smoothed value a chunk keeps, not about the solver. Still XFAILs at 1.473; not deleted.
- The newly-found slam iteration-cap saturation (see below) — left as a measured XFAIL.
- The committed WASM artifact: `core/` changed, so `scripts/build-wasm.sh` is **needed**, but
  the orchestrator rebuilds and commits it (no Emscripten in this worktree).

---

## What actually happened

**The acceptance bar of bit-identity is unreachable, and proving that is the main result.**
The pre-fix solver drove the residual all the way to the floating-point floor, so *any*
early-out that actually fires declines refinement it performed. I swept the tolerance against
the pre-fix binary rather than arguing about it:

| `kNewtonResidualTolA` | fires when parked? | worst difference vs the pre-fix solver |
|---|---|---|
| 1e-13 | yes | −81.8 dBFS abs / −89.0 dB rel |
| 1e-14 | yes | −104.3 / −111.8 |
| 1e-15 | yes | −108.5 / −116.0 |
| 1e-16 | yes | −124.3 / −129.5 |
| **1e-17** | **yes** | **−127.4 / −134.1** ← chosen |
| 1e-18 | **no** | −132.5 / −137.7 |
| 1e-19 | **no** | bit-identical (never fires) |

Bit-identity exists only where the fix does not. So the honest bar is the project's own
**−120 dBFS solver-accuracy gate** (§25), the same one every valve solver's early exit is held
to, and **1e-17 is the tightest value that still fires** — 7.4 dB inside that gate.

The window's floor is the **idle residual ceiling**, measured at **2.0600e-18 A** and stable to
five figures across 44.1/48/88.2/96/192 kHz × 1/2/4/8 oversampling × sustain MIN/mid/MAX,
because at the parked point `gCin·(vin−Vb)` and `histCin` cancel exactly and the residual is set
by the DC branch currents rather than by `gCin = Cin/T`. So the 4.9× margin does not erode with
rate or oversampling.

Other things worth recording:

1. **A whole bit-identity sweep was vacuous, and a test caught it.** `PARAM_VOLUME`'s smoother
   defaults to **0**, so my first 25 renders — driven only through `PARAM_SUSTAIN` — were
   digital silence, and compared byte-identical for entirely the wrong reason. The
   `assert(peak > 0.01)` guard I had just added to the gate test is what surfaced it. That
   guard is now commented to stay.
2. **The DC operating-point solve had to opt out.** It runs once in `prepare()` so there is no
   per-sample win, and its final iterate seeds `vbQ_`/`vcQ_`/`veQ_` + `settleDC()` — including
   it contributed ~3× more output difference than the per-sample early-out alone. It now passes
   `tol = 0.0`, which fires only on an exactly-zero residual (where the step is exactly zero),
   so opting out is exact rather than approximate.
3. **The audit's "iteration count is flat at 7" is not what this build measures, and the real
   number is worse.** On silence the solver reports **1** iteration (0 with the off-by-one
   fixed) against 8–13 on signal. Idle did *strictly less* Newton work and still cost 2.7× the
   time. The cause is evaluations, not iterations: **31.00 system evaluations per solve on
   silence vs 4.1–6.7 on signal**, with **100 %** of idle iterations burning all 30 backtracks.
4. **The audit's suggested second half is dead code.** Short-circuiting the backtracking on
   `cur <= tol` can never fire once the top-of-loop early-out exists, because `cur` is
   `infNorm3(r)` with `r` unchanged between the two points. Not added.
5. **The `it + 1` over-report was real, not hypothetical.** A ±20 V slam at 96 kHz × 4 made
   `lastMaxNewtonIterations()` return **61** against `kMaxNewtonIter == 60`, measured on the
   pre-fix binary. Fixed.
6. **The worst case was not the obvious one.** The early-out's cost at hot-DI level is −228 dBFS;
   at a loud input (riff ×5) and sustain 0.70 it is −127 dBFS, 19 dB worse. The gate now drives
   MIN/mid/MAX sustain at both levels, because a hot-DI-only test would have reported a number
   100 dB better than the truth.

## Measured results

**Bit-identity: NOT achieved — reported rather than worked around.** Worst difference vs the
pre-fix solver over 25 renders (5 sustain × 5 input levels, 2 s each at 48 kHz): **−127.4 dBFS
absolute / −134.1 dB relative**, on a loud input at sustain 0.70. Silence and the ±20 V slam are
*exactly* 0 (parked, and never converged-to-tolerance, respectively). Per solve the error is
1e-17 A ÷ ~1.9e-4 S ≈ **53 femtovolts** of node voltage; the −127 dBFS figure is that amplified
by four cascaded high-gain stages and accumulated through their recursive state.

**Silence vs signal**, 10 s of audio @ 48 kHz, 128-frame blocks, interleaved, two passes (wall ms):

| sustain | input | before | after | speedup |
|---|---|---|---|---|
| 0.00 | silence | 6797–6820 | 498–501 | **13.6×** |
| 0.00 | hot 0.20 | 2375–2511 | 1857–1863 | 1.28× |
| 0.60 | silence | 6871–7002 | 494–498 | **13.9×** |
| 0.60 | hot 0.20 | 2479–2534 | 2007–2108 | 1.22× |
| 1.00 | silence | 6837–6982 | 488–534 | **13.4×** |
| 1.00 | hot 0.20 | 2598–2653 | 2114–2186 | 1.21× |

**Silence/signal ratio 2.58–2.87× → 0.23–0.27×** (the audit's target was ~1.0; the fix removes
30 of 31 evaluations rather than merely equalising, so idle ends up cheaper than playing).
System evaluations per solve on silence **31.00 → 1.00**.

**`clipper-bench --unit muff`** (interleaved, 3 runs each):
**3.81–3.93× realtime / 25.4–26.2 % → 4.78–4.94× / 20.3–20.9 %**. §25.3 updated, with a note
that the previous 4.10× row was measured on different hardware.

**Solver-accuracy gate:** `clipper_tube_solver_tests` now covers the Muff at sustain
MIN/mid/MAX × hot/loud: **−138.5 to −228.8 dBFS** against the −120 dBFS gate. The three valve
amps are unchanged (−258.9 / −258.9 / −246.8 dBFS).

**Slam convergence:** ±20 V at every rate × oversampling — output finite and bounded
everywhere, iteration count within the cap everywhere, 14–18 iterations at 10 of 16
combinations. **6 of 16 exhaust the 60-iteration cap** (2× at all four base rates, 4× at 88.2
and 96 kHz) — pre-existing, measured identical on the pre-fix solver, now recorded as an XFAIL.

**Teeth (perturbation, both directions):** `kNewtonResidualTolA = 1e-19` → `clipper_muff_tests`
RED (early-out stops firing); `= 1e-11` → `clipper_tube_solver_tests` RED (truncates real
work); restoring the `it + 1` over-report → `clipper_muff_tests` RED. Restored → both GREEN.

**Goldens:** untouched, no re-blessing. Core suite **24/24 pass + 6 XFAIL ledgers Skipped**.
No XPASS.

## Files created / modified

- `core/src/dsp/BjtStage.cpp` — the residual early-out, `kNewtonResidualTolA` + the measured
  sweep table as its rationale, the `tubeSolverTolScale()` hook, `tol` plumbed per call site so
  the DC solve can opt out, the `it + 1` iteration-count fix
- `core/include/clipper/dsp/BjtStage.h` — solver doc + `lastMaxNewtonIterations()` contract
- `core/include/clipper/dsp/TubeSolverMode.h` — scope note: it now covers the BJT solver, and
  the multiplier applies to a tolerance in amps as well as ones in volts
- `core/tests/test_tube_solver.cpp` — the Muff added to the gate, at MIN/mid/MAX sustain ×
  hot/loud input, with a non-silence guard
- `core/tests/test_muff_model.cpp` — `testIdleSolverCost` (0 iterations when parked, 48
  combinations), `testSlamConvergence` (±20 V across 16 combinations), the
  `muff-slam-exhausts-newton-cap` XFAIL
- `docs/DEVELOPMENT.md` — new §34; §25.3 bench row + footnote
- `CLAUDE.md` — Current State
- `docs/work/2026-07-25-muff-newton-earlyout.md` — this file

## Deferred to next session

- **The WASM artifact must be rebuilt** (`bash scripts/build-wasm.sh`) — `core/` changed and
  there is no Emscripten toolchain in this worktree. The orchestrator owns this.
- **`muff-slam-exhausts-newton-cap`** — 6 of 16 rate × oversampling combinations do not
  converge under a ±20 V slam. Its own slice: the damping / step-clamp strategy above |10 V|.
  Do not raise `kMaxNewtonIter` — that buys iterations, not convergence.
- The audit's remaining solver-perf items (4, 5, 9) — each its own slice.
- The control-rate parameter-sampling XFAIL stays open, still naming the Muff at 1.473.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
