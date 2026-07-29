# Finding 7 — the phase-inverter tail reference (LtpInverter tailRef)

**Date:** 2026-07-29
**Branch:** claude/amps-pedals-fixes-6f557i (designated session branch; slice-per-PR)
**Roadmap item:** 2026-07-24 audit finding 7 — "The phase-inverter model cannot express a real long tail; two of three amps idle near cutoff". Docs §42, ADR 014 (§38–41 / ADR 010–013 are reserved for the four in-flight branches per the 2026-07-29 handoff).

## Goal

All three valve amps' phase inverters idle at the project's own targets — 0.5–0.9 mA per
triode, plates at 70–85 % of B+, leg-gain ratio ≥ 0.90 where balance is the design intent —
by giving `LtpInverter` the tail reference a real long-tailed pair returns to, with
`Rtail = 10 k` restored on all three amps, and every finding-7 XFAIL deleted and asserted
for real.

## Approach

This is a **deliberate tone change** (the PI operating point moves on all three amps, and
the PI sits inside the NFB loop on two of them). It is judged by measurement: the LtpProbe
table before → after, and per-golden per-band dB tables put to the owner before any bless.

The audit's fix, verbatim mechanics:

1. `LtpInverter::Config` gains `double tailRef = 0.0` (volts; the node the tail returns
   to; 0 keeps today's behaviour bit-identically for any config that doesn't set it).
2. `prepare()`: the tail residual becomes `k1.Ip + k2.Ip − (Vk − tailRef)/Rtail`.
   The Jacobian is unchanged (∂/∂Vk of the tail term is the same).
3. `processSample()`: same change with `gTail`; `J22` unchanged.
4. **JCM800:** keep `Rtail = 10 k` (the 2204's real part), calibrate `tailRef` to land
   0.5–0.9 mA/triode and 70–85 % B+. `Ra2 = 82 k` stays — that is finding 8's slice, and
   its Ra2 sweep must be **re-measured after** this change (the shipped sweep was taken
   against the ground-referenced tail).
5. **Twin:** `Rtail` 22 k → 10 k (the AB763's documented shared tail) with a calibrated
   `tailRef`, and `Ra2` 142 k → **100 k** — the header already documents "balanced
   100k/100k plate loads, 10k shared tail"; the 142 k exists in no AB763 and was an
   artifact compensating the starved tail (audit finding 7's Twin note). Balance must
   still measure ≥ 0.90 (expect ~1.0 from a real long tail with matched loads); the
   Twin's balance assertion is already hard — it must stay green through the change.
6. **AC30:** `Rtail` 2.2 k → 10 k with a calibrated `tailRef`. The 2.2 k was the §23
   second-amendment workaround that hit the operating point by destroying the long-tail
   property (leg ratio 0.550). Keep the same operating point (~0.53 mA, ~82 % B+ — that
   part was right); restore the common-mode rejection. Plate loads stay 100k/110k (the
   documented "deliberately less balanced than the Twin" voicing).
7. **XFAIL ratchet:** finding-7 XFAILs (`finding7-jcm-pi-plate-fraction`,
   `finding7-jcm-pi-standing-current`, `finding7-twin-pi-plate-fraction`,
   `finding7-twin-pi-standing-current`, `finding7-ac30-pi-leg-balance`) are expected to
   XPASS → delete each and pass `nullptr` to `assertLtpTargets` so they become hard
   assertions, in this same slice. The finding-8 XFAILs (`finding8-jcm-pi-leg-balance`,
   push-pull cancellation) are expected to remain XFAIL with Ra2 = 82 k; if any XPASSes,
   it also gets deleted/asserted here and finding 8 re-scoped.
8. **No downstream recalibration.** `kFullScaleSecV`, `kInterstageScale` etc. are NOT
   touched — re-deriving them belongs to findings 9 and 5. What must be measured here is
   how far the composed amps move (gain, breakup ordering, sag window) so the tone change
   is argued with numbers, not vibes.
9. Update the stale comments/docs that the fix invalidates: `Ac30PowerAmp.cpp`'s
   two-terminal-tail block, `TwinPowerAmp.cpp`'s 142 k rationale, `Jcm800PowerAmp.h`'s
   PI section, `LtpProbe.h`'s "as checked out" table, DEVELOPMENT.md §23's amendment
   (annotate, don't rewrite history), new §42, ADR 014.
10. Core changed → `bash scripts/build-wasm.sh`, commit all three artifacts.

## Steps

- [ ] Branch check (designated branch, reset from origin/main — done)
- [ ] Implement tailRef in `LtpInverter` (header + both solve sites)
- [ ] Bit-identity sanity: with `tailRef = 0` and unchanged configs, goldens/ctest identical
- [ ] Calibrate tailRef per amp (JCM800, Twin, AC30) against the LtpProbe targets
- [ ] Twin Ra2 → 100 k; AC30/Twin Rtail → 10 k; verify leg ratios
- [ ] Delete XPASSing XFAILs, flip to hard assertions; run full ctest
- [ ] Measure composed-amp deltas: gain sweeps, breakup ordering (AC30 earliest, Twin
      cleanest, JCM hardest — audit "verified correct", must not break), sag windows,
      golden --golden-report per-band tables
- [ ] Perturbation proof: revert tailRef (set 0) in a scratch copy → new assertions fail;
      restore; `touch` after both steps
- [ ] CPU check: interleaved same-machine A/B on the three amps (Newton convergence at
      the new operating point), ≥3 runs
- [ ] Docs: §42, ADR 014, CLAUDE.md Current State, stale comments
- [ ] Rebuild WASM artifact, full local gates (ctest, web build+test, node suites, electron)
- [ ] Present golden tables to owner; bless only what is authorised; PR

## How this will be measured

- `LtpProbe` (`measureLtp`) per amp, before → after: plate %B+, Ip/triode (Ohm's law),
  leg gains and ratio. Targets: 70–85 %, 0.5–0.9 mA, ratio ≥ 0.90 (Twin ≈ 1.0).
- `clipper_player_expectations_tests --golden-report`: per-band dB per golden.
  `clean120_chorus` must be **UNCHANGED** (scope check — no PI in the clean amp).
- Compression-curve ordering across the three amps (existing tests) still correct.
- `clipper-bench` interleaved A/B for CPU.
- Perturbation: `tailRef → 0` fails the new hard assertions on all three amps.

## Manual test steps

- [ ] Web app: play each valve amp at moderate volume — no silence, no gross level jump
      beyond the measured numbers; knob sweeps stay smooth (finding 6's smoothing intact).
- [ ] Crank the AC30 VOLUME: breakup still arrives early and hot (§23's fix must survive).
- [ ] Edge: amp-voice swap mid-note still declicks; `amp_reset` still parks clean
      (the PI quiescent point moved — reset must re-park at the *new* point).
- [ ] Edge: NFB sanity on JCM/Twin — closed-loop gain still measures below open-loop.

## Out of scope for this session's slice

- Finding 8 (JCM Ra2) — needs its sweep re-measured post-tailRef; next slice.
- Findings 9 (plate load line + kFullScaleSecV), 10 (screen fits), 4 (AC30 sag),
  5 (AC30 tone stack + kInterstageScale).
- Any downstream level re-staging or golden bless without explicit owner authorisation.

---

<!-- Fill in below during/after the session -->

## What actually happened

## Measured results

## Files created / modified

## Deferred to next session

## Status

- [x] In progress
- [ ] Complete
- [ ] Partial — see deferred
