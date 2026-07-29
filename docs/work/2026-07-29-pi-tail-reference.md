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

Two phases in one session. **Phase 1** implemented the audit's mechanics verbatim
(`LtpInverter::Config::tailRef`, the tail residual in both solve sites, per-amp
calibration, the XFAIL ratchet) and hit a stop condition: with the inverters corrected the
JCM800 came out +3.7…+5.7 dB hot and the Twin +3.4…+4.9 dB, both clipping past full scale,
and the §23 breakup ordering inverted. **Phase 2** un-fitted the constants that had been
absorbing the missing inverter gain — the orchestrator's call, on the ADR 008 precedent.
Full narrative in docs §42; ADR 014 records the decisions.

Six things did not go as planned, all decided by measurement:

1. **The audit's Twin `Ra2` 142 k → 100 k is wrong.** With the tail fixed, matched
   100k/100k loads measure a leg ratio of 0.718 and nothing legal reaches 0.90. Unequal
   plate loads are *how* a finite-tail LTP is balanced. Following the brief would have
   turned a green assertion red (perturbation pass C). `Ra2` was still re-derived — 142 k →
   **119 k** — because its *value* had been fitted against the old ground-referenced tail
   (§20: "measured swing ratio 1.007"); the balance optimum against the corrected tail is
   119 k (ratio 0.9978).
2. **The plan's "Twin `Rtail` 22 k → 10 k (the schematic value)" is also wrong**, and this
   was the most consequential finding of phase 2. The Twin injects its global NFB
   single-endedly into the cold grid, so half of it is common mode; the tail impedance is
   the only thing rejecting it. At 10 k, *closing the loop added 24 dB of 2nd harmonic*
   (open-loop −61.9 dBc → closed −33.0), and the Twin's documented clean-headroom bar went
   2.96 % → 4.5 % THD, a fail that no staging constant can fix. At 22 k (the value the amp
   already shipped) with `tailRef = −26 V` it is 3.41 % and passes.
3. **`kFullScaleSecV` had to move after all**, and for its own documented reason: the
   cranked secondary swing genuinely changed, because a starved inverter could not drive
   the output tubes to rated power. JCM 23.4 → 29.3 V pk (34 → 54 W of 50 W), Twin 22.4 →
   37.7 V (31 → 89 W of 85 W). Normalizing to the documented ~0.9 cranked peak costs
   ~2.0 dB (JCM) and ~4.9 dB (Twin) of level below clipping — forced, since any Twin
   normalization keeping the cranked peak inside full scale costs ≥ 3.9 dB.
4. **The staging refit is NOT the PI gain ratio.** The global NFB absorbs about half of it;
   the measured closed-loop power-section gain ratio (1.577 JCM, 1.490 Twin over 82–880 Hz)
   is the defensible number. 0.25 → 0.16 and 0.16 → 0.107.
5. **The AC30's "chime" and its mid-volume "breakup" were its phase inverter's 2:1 leg
   imbalance** leaking 2nd harmonic past the push-pull cancellation (h2 at VOLUME 0.6:
   −19.76 → −33.03 dBc; the odd/clipping harmonics did not move). Five §23 bars therefore
   fail with their bounds untouched, and restoring them needs drive the amp's *passive*
   interstage divider cannot supply. They are ledgered as XFAILs owned by an AC30
   gain-structure slice. **This is the slice's biggest open item and it is a voicing call
   for the owner.**
6. **Six test probes/bounds had been calibrated against the starved inverters** — the
   sag-ordering probe (90 V at the PI grid, where the metric measures the clipping ceiling
   rather than the rail), the AC30 cathode-bloom probe (§23 had already retuned it once for
   this reason), the JCM sag-recovery window, the power-compression monotonicity bound, and
   two "2× beats 1× by 8 dB" aliasing bars. Each re-derived with the physics named and
   checked on the pre-fix circuit too. Exactly one genuine *bound* changed (the Twin's
   cranked-breakup threshold, 25 % → 18 %), and a harder new assertion — odd-harmonic-only
   THD — was added beside it.

## Measured results

Full tables in docs §42. Headlines:

- **PI operating points, 8 of 9 targets now hard assertions** (was 4). JCM 80.0/81.6 % B+,
  0.680/0.763 mA, ratio 0.703 (finding 8); Twin 80.7/79.6 %, 0.792/0.703 mA, ratio
  **0.9978**; AC30 79.3/78.5 %, 0.622/0.587 mA, ratio **0.912**. Rate-independent.
- **Constants:** `Jcm800Amp::kInterstageScale` 0.25 → 0.16 · `Jcm800PowerAmp::kFullScaleSecV`
  26 → 33 V · `TwinAmp::kInterstageScale` 0.16 → 0.107 ·
  `TwinPowerAmp::kFullScaleSecV` 24 → 42 V · Twin PI `Rtail` 22 k (kept), `tailRef` −26 V,
  `Ra2` 142 k → 119 k · JCM PI `tailRef` −12 V · AC30 PI `Rtail` 2.2 k → 10 k, `tailRef`
  −10 V.
- **Cranked peaks back inside full scale:** JCM 1.163 → **0.890**, Twin 1.661 → **0.898**.
- **Twin clean bar** (VOLUME 0.5, hot 0.10 V DI): 2.96 % → 5.73 % (phase 1) → **3.41 %**.
- **NFB** still negative and still analytic: JCM −3.41 → **−5.977 dB** (analytic −5.995;
  0.023 dB from its own 6.0 dB "meaningful loop" ceiling — flagged), Twin −3.24 →
  **−5.24 dB** (analytic −5.24), AC30 β = 0 bit-exact.
- **Goldens** (not re-blessed): `rat_jcm800` +4.39 → **−1.77 dB**, `sd1_twin_reverb` +3.43 →
  **−4.89**, `muff_twin` +3.87 → **−4.74**, `ts_ac30` **+0.44** (AC30 untouched in phase 2),
  `clean120_chorus` **UNCHANGED** (−0.00 dB, worst band 0.11 = the gate's own floor).
- **ctest 25/26 entries pass** (26 entries: the AC30 ledger came back). The only failure is
  `clipper_player_expectations_tests`' golden gate, awaiting an owner bless. Open XFAILs
  10 → 15 across 5 ledger entries (5 new AC30 voicing entries; 5 finding-7 entries deleted
  as XPASSes).
- **CPU:** interleaved same-machine A/B, no regression (see §42 / the report).

## Files created / modified

```
core/include/clipper/dsp/Jcm800PowerAmp.h   tailRef + PI text + kFullScaleSecV 26→33
core/include/clipper/dsp/Jcm800Amp.h        kInterstageScale 0.25→0.16
core/include/clipper/dsp/TwinPowerAmp.h     PI text (22k/−26V/119k) + kFullScaleSecV 24→42
core/include/clipper/dsp/TwinAmp.h          kInterstageScale 0.16→0.107
core/include/clipper/dsp/Ac30PowerAmp.h     PI text
core/src/dsp/Jcm800PowerAmp.cpp             tail residual (2 sites), tailRef −12 V
core/src/dsp/Jcm800Amp.cpp                  staging derivation comment
core/src/dsp/TwinPowerAmp.cpp               Rtail 22k / tailRef −26 V / Ra2 119k + rationale
core/src/dsp/TwinAmp.cpp                    staging derivation comment
core/src/dsp/Ac30PowerAmp.cpp               Rtail 10k / tailRef −10 V + rationale
core/tests/support/LtpProbe.h               iTail fix + before/after tables
core/tests/test_jcm800_power.cpp            2 XFAILs deleted, βA reference, compression
                                            property, sag-recovery window, alias bar
core/tests/test_twin_amp.cpp                2 XFAILs deleted, thdOdd(), cranked bars,
                                            sag probe, alias bar
core/tests/test_ac30_amp.cpp                1 XFAIL deleted, 5 new XFAILs, sag probe,
                                            cathode-bloom probe
core/CMakeLists.txt                         AC30 xfail ledger re-registered
docs/DEVELOPMENT.md                         §42
docs/decisions/014-pi-tail-reference.md     new
docs/work/2026-07-29-pi-tail-reference.md   this file
```

## Deferred to next session

- **The four moved goldens need an owner bless** (tables in §42.7). Nothing re-blessed.
- **The AC30 gain-structure slice** — five ledgered XFAILs (`finding7-ac30-chime`,
  `-breakup-onset`, `-breakup-vs-twin`, `-mid-thd`, `-mid-vs-twin`). The voice is now
  measurably cleaner at mid volume and has lost most of its 2nd-harmonic chime; recovering
  it needs preamp gain (the missing top-boost stage + a VOLUME re-taper), not a constant.
- **Finding 8** — the JCM's `Ra2 = 82 k` (ratio 0.703, 8.1 dB of cancellation), with the NFB
  margin at 5.977/6.0 dB to watch. The Twin's leg 2 needing 19 % more plate load than leg 1
  is the same family of question.
- **The Twin's residual closed-loop 2nd harmonic** — ~7 dB above its pre-fix figure at
  matched drive, because the single-ended NFB injection's common-mode half is only rejected
  as well as one tail resistor rejects it.
- **The WASM artifact has NOT been rebuilt** and the web/native front ends were not touched
  (out of scope for these two phases; `core/` changed, so `bash scripts/build-wasm.sh` is
  required before this can land).
- Not run: web build/tests, node suites, electron (core-only session).

## Status

- [ ] In progress
- [ ] Complete
- [x] Partial — see deferred (core work complete and measured; golden bless, WASM rebuild
      and the AC30 voicing follow-up are outstanding)
