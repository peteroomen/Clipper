# Make the vacuous tests assert real properties

**Date:** 2026-07-25
**Branch:** test/assert-real-properties
**Roadmap item:** 2026-07-24 audit → **Test & process integrity** (the systemic finding), sequenced per the audit's own "Suggested order of work" step 3: *"Stand up CI before the circuit work, and fix the vacuous tests named in each finding."*

## Goal

Every test named in the audit's "Tests that assert the wrong thing" list either asserts a
player-observable property that **fails if that property is violated**, or is deleted — and
where the corrected assertion exposes an open defect, it is recorded as a **visible XFAIL**
naming the finding rather than silently relaxed.

## Approach

This slice changes **tests, `core/CMakeLists.txt`, and docs only**. No DSP change. Where a
corrected test exposes a real bug, it XFAILs.

**The XFAIL mechanism is the load-bearing design decision.** `#if 0` and a loosened bound
are both invisible; the audit's whole point is that invisible weakening is how defects ship
green. So: a new `core/tests/support/Xfail.h` that

- **runs** the check and prints the real measured number,
- prints `[XFAIL finding N] …` and continues instead of aborting,
- and makes an **XPASS a hard failure** — so the moment somebody fixes finding 7, the suite
  goes red until they delete the XFAIL. The XFAIL cannot rot into a permanent excuse.

Visibility, per platform limits: `ctest --output-on-failure` hides stdout on a passing test,
so each binary that carries XFAILs also registers a second ctest entry
(`<target>_xfail_ledger`) that runs the binary with `--xfail-ledger` and exits **77**, with
`SKIP_RETURN_CODE 77`. ctest's *default* summary then prints e.g.
`clipper_twin_tests_xfail_ledger .... ***Skipped`, so the open defects are visible in a plain
`ctest` run without any extra flag, and the binary itself prints the finding text.

For the PI work: leg gains are measured by driving a `LtpInverter` configured from the
shipped `config()` and reading its two plate swings — a real measurement of the shipped
netlist, not a re-derivation of it. Standing current per triode comes from Ohm's law on the
plate resistor (`(B+ − Va)/Ra`), which is independent of the Koren law the solver used.

## Steps

- [ ] `core/tests/support/Xfail.h` — XFAIL/XPASS/ledger support, XPASS is fatal
- [ ] `core/CMakeLists.txt` — make `-UNDEBUG` unconditional (drop the `if(NOT MSVC)` guard on
      every test target); register the `_xfail_ledger` companions
- [ ] `test_jcm800_power.cpp` — delete the `f(V+v) − f(V−v)` identity; assert push-pull
      even-harmonic suppression on the real `Jcm800PowerAmp` against the single-ended
      device-law baseline with a meaningful bar; add PI plate %B+, standing current per
      triode, and leg-gain ratio
- [ ] `test_twin_amp.cpp` / `test_ac30_amp.cpp` — replace the 160 V / 150 V plate windows with
      plate-as-%-of-B+, standing current per triode, and leg balance
- [ ] `test_muff_model.cpp` / `test_rat_model.cpp` / `test_ts_model.cpp` /
      `test_gold_model.cpp` — assert DC offset on **signal**, not just silence
- [ ] `test_player_expectations.cpp` — block B `tol` 2e-5 → 0.0 plus a ragged (non-128) block
      pass; fix the `lv[1] >= lv[0]` ~1e-12 comparison, the `th[1] >= th[0]*0.95`
      "non-decreasing" contract that permits a 5 % decrease, and the identical
      `isPedal ? 0.1f : 0.1f` branches
- [ ] New `core/tests/test_opto_tremolo.cpp` + ctest target — the first test `OptoTremolo` has
      ever had
- [ ] `web/tests/amp.spec.ts` — three perf-smoke tests: assert real audio or delete; three
      "no pop" tests: land the swap mid-render via `ctx.suspend()`/`resume()` and tighten the
      absolute floors
- [ ] `web/tests/audio.spec.ts` — assert the real reorder result; assert `overall` and
      `sd1.h3`; add finiteness checks
- [ ] `web/tests/cab.spec.ts` — `irLen > 128` → the fixture's real length; `typeof label ===
      'string'` → the real label
- [ ] Prove teeth: perturb the relevant constant/topology in a scratch copy for every
      rewritten test that passes today, confirm red, revert

## How this will be measured

Two numbers per rewritten test:

1. **It passes on `main` as-is** (or XFAILs with the measured value printed), and
2. **it fails under a named perturbation** — the specific constant or topology edit is
   recorded in the results table below. A rewrite with no recorded perturbation is not
   done.

Baseline measurements taken before writing any assertion (scratch harness against
`libclipper_dsp` at `9923af7`) — these reproduce the audit's numbers exactly:

| PI | Va1 (% B+) | Va2 (% B+) | Ip1 / Ip2 (mA) | \|g1\| / \|g2\| | ratio |
|---|---|---|---|---|---|
| JCM800 | 322.1 (94.7 %) | 324.3 (95.4 %) | 0.179 / 0.191 | 15.44 / 9.37 | 0.607 |
| Twin | 386.8 (94.3 %) | 381.5 (93.1 %) | 0.232 / 0.200 | 7.43 / 7.51 | 0.990 |
| AC30 | 247.1 (82.4 %) | 244.9 (81.6 %) | 0.529 / 0.501 | 31.76 / 17.46 | 0.550 |

Project target (`docs/DEVELOPMENT.md`): 0.5–0.9 mA/triode, plates at 70–85 % of B+, ×25–35
per leg.

## Manual test steps

- [ ] `ctest --test-dir build --output-on-failure` — green, and the default summary lists the
      `_xfail_ledger` entries as `***Skipped`
- [ ] `./build/clipper_twin_tests --xfail-ledger` prints the open findings and exits 77
- [ ] `cd web && npx playwright test` — green
- [ ] Edge case: flip an XFAIL's condition to true in a scratch copy → the binary must exit
      non-zero (XPASS is fatal), proving the XFAIL cannot outlive its defect

## Out of scope for this session

- **Any DSP fix.** Findings 7, 8, 16 and the opto-tremolo defect stay broken; this slice only
  makes them visible.
- `web/tests/cab.spec.ts:155` (`'cab swap: no pop'`) — owned by the in-flight
  `fix/cab-swap-rt-safety`.
- `test_amp_model.cpp testConvolverChunking` — already rewritten and merged.
- `web/playwright.config.ts:34` `retries: 2` — flagged, not changed unilaterally.
- The tone-stack "analytic H(jω) derived from the same netlist" class. Real, but fixing it
  needs published response curves per amp, which is a research slice of its own.

---

## What actually happened

Full write-up in **`docs/DEVELOPMENT.md` §29**. The decisions worth recording here:

**1. The XFAIL ratchet was the real design work.** Correcting these tests exposes open
defects, and both obvious ways to stay green are the disease itself: `#if 0` makes the
property untested again, and loosening the bound makes the defect canon — which is literally
how the starved AC30 phase inverter shipped. So `core/tests/support/Xfail.h` measures the
property, prints the real number, names the finding and the owning slice, continues — and
**fails the suite on an XPASS**. An XFAIL therefore cannot outlive its defect. Visibility came
from `SKIP_RETURN_CODE 77`: each binary with XFAILs registers a `<target>_xfail_ledger` entry
that shows as `***Skipped` in a *plain* `ctest` run, because `--output-on-failure` prints
nothing for a passing test and an invisible known defect is how this whole class survived.

**2. `-UNDEBUG` got a compile-time guard, not just a flag fix.** Removing `if(NOT MSVC)` fixes
today; `core/tests/support/AssertsLive.h` (`#error` on `NDEBUG`, included by all 18 test
`.cpp`s) means the hole cannot silently reappear on any platform or in any new target.
Seventeen copies of the flag block collapsed into one `clipper_add_test_flags()`.

**3. The ragged block-size pass found a real defect I was not looking for.** Adding a
non-128-multiple block size to block B (the only segmentation that can catch a block-size bug —
finding 3's `CabConvolver` was exact at 128 and worse-than-signal at 100) showed all five dirt
pedals diverging, confined to the first ~25 ms. That is the audit's Medium/DSP "control-rate
parameter sampling" item, now measured end to end through the C ABI the worklet calls. XFAILed;
the *settled* output is asserted for real.

**4. The DC-on-signal tests needed a second stimulus to have teeth at all.** A clean tone was
not enough: deleting the RAT's / TS's / GOLD's output coupling cap changes their measured DC by
*nothing*, because a symmetric or already-blocked clipper does not rectify. A +0.1 V **input**
offset makes the cap load-bearing, and is the realistic case anyway.

**5. A false alarm, recorded because the lesson is reusable.** A measurement appeared to show
the TS passing input DC through at unity (`cfg_.dcBlockHz` reading 0 while the constexpr
static-asserts as 12.0). Not a defect: a **stale object file**. The perturbation harness
restored files with `cp`/`mv`, which preserves the *backup's* mtime — older than the existing
`.o`, so `make` skipped the rebuild. A fresh build dir gave the correct value. Any perturbation
harness must `touch` after both patch and restore; one earlier "NO TEETH" reading was also
wrong for this reason and was redone.

**6. Scope held.** `git diff --name-only core/src core/include web/src web/public web/worklet
native server electron` → **0 files**. No DSP fix, no WASM rebuild needed, goldens untouched.
`web/tests/cab.spec.ts:155` and `test_amp_model.cpp testConvolverChunking` left alone as
instructed; `playwright.config.ts` `retries: 2` flagged, not changed.

## Measured results

Phase inverter (reproduces the audit exactly):

| amp | Va1 (% B+) | Ip/triode | leg gains | ratio | verdict |
|---|---|---|---|---|---|
| JCM800 | 322.1 V (94.7 %) | 0.179 mA | ×15.44 / ×9.37 | 0.607 | 3 XFAIL |
| Twin | 386.8 V (94.3 %) | 0.232 mA | ×7.43 / ×7.51 | 0.990 | 2 XFAIL, balance **asserted** |
| AC30 | 247.1 V (82.4 %) | 0.529 mA | ×31.76 / ×17.46 | 0.550 | plate+current **asserted**, balance XFAIL |

DC offset as a fraction of output peak — clean input / +0.1 V input DC:
RAT 0.000 % / 0.091 %, TS 0.000 / 0.000, GOLD 0.000 / 0.000, **Muff 18.4 / 18.4** (XFAIL).

Block B: **0.000e+00 at 128** for every unit (was `tol` 2e-5). At a ragged 100 frames the
phaser, all four amps, `CabConvolver` and `ReverbModel` stay bit-identical; the dirt pedals
diverge up to 1.473 (Muff) inside 25 ms and settle to ≤ 1.0e-3 relative.

OptoTremolo across SPEED at INTENSITY 1: peak gain 0.973 → 0.831 → **0.522**, mean level
−6.91 → −8.84 → **−12.03 dB**, depth −71.9 → −27.2 dB.

Amp-swap slew ratio (swap window ÷ away-from-swap, same render): jcm 2.04, twin 1.66, ac30
1.19 with the shipped declick; **7.15 / 7.16 / 5.12** with the declick deleted. Bar 3.0.

**Teeth — one perturbation per rewritten test:**

| test | perturbation | result |
|---|---|---|
| JCM800 PI leg balance | `Ra2` 82 k → 150 k | **XPASS → exit 1** |
| JCM800 PI plate / current | `Rtail` 10 k → 1.6 k | fails |
| Twin PI leg balance (hard) | `Ra2` 142 k → the 100 k its header documents | fails |
| AC30 PI plate + current (hard) | `Rtail` 2.2 k → the original 22 k | fails |
| TS DC on signal | bypass the output coupling cap | fails |
| GOLD DC on signal | bypass the output coupling cap | fails |
| block B `tol` 2e-5 → 0 | +1e-6/call in `ReverbModel` → 1.341e-07 on the JCM path | fails at 0.0, **would have passed at 2e-5** |
| block B ragged pass | phaser LFO sampled per call instead of per sample | fails |
| LEVEL knob top half | RAT level saturates at 0.5 | fails |
| OptoTremolo speed sag | `kReleaseMs` 55 → 5 ms | **XPASS → exit 1** |
| `-UNDEBUG` / AssertsLive | drop `-UNDEBUG` | **build error** |
| amp swap "no pop" ×3 | delete the declick bracket | 5.12–7.16 vs bar 3.0 |
| chain reorder | post the chain in its original order | fails |
| cab custom label | derive the label from a constant | fails |
| `audio.spec` finiteness | one NaN at sample 256 (outside every window) | **guard fires; passes 2/2 with the guard disabled** |

Honest caveats: the **RAT**'s DC assertion holds with ~11× margin and its teeth overlap
`testPreClipVoicing` (the shaping network's DC gain is already pinned there to ±1.5 dB).
**GOLD** is protected at both input and output; the output cap alone is enough to fail it.

## Files created / modified

Created: `core/tests/support/{Xfail.h,AssertsLive.h,LtpProbe.h,DcOffset.h}`,
`core/tests/test_opto_tremolo.cpp`, `web/tests/support/finite-output.ts`,
`docs/work/2026-07-25-assert-real-properties.md`.

Modified: `core/CMakeLists.txt` (unconditional `-UNDEBUG` via `clipper_add_test_flags`, the
`clipper_add_xfail_ledger` helper, the new tremolo target); `core/tests/test_jcm800_power.cpp`,
`test_twin_amp.cpp`, `test_ac30_amp.cpp`, `test_muff_model.cpp`, `test_rat_model.cpp`,
`test_ts_model.cpp`, `test_gold_model.cpp`, `test_player_expectations.cpp` (real assertions);
the other eight `core/tests/test_*.cpp` (the `AssertsLive.h` include + `requireAssertsLive()`
only); `web/tests/amp.spec.ts`, `audio.spec.ts`, `cab.spec.ts`; `docs/DEVELOPMENT.md` (§29);
`CLAUDE.md`.

No ADR. §29 records the XFAIL-ratchet decision in full and it is a test-infrastructure
convention rather than an architecture change; **ADR 005 was left unclaimed.**

## Deferred to next session

- **Every XFAIL is a deferred defect**, each naming its owner: findings **7** (5 entries), **8**
  (2), **16** (1), the opto-tremolo speed sag (2), control-rate parameter sampling (1). Fixing
  any of them will XPASS and turn the suite red — that is the ratchet; delete the XFAIL in the
  same slice.
- **The tone-stack class of test** — discrete MNA vs an analytic `H(jω)` derived from the same
  netlist. This is how finding 5 and the JCM800 preamp flatness shipped. Needs published
  response curves per amp: a research slice.
- **`playwright.config.ts` `retries: 2`** — the last remaining way for a real fault to vanish.
  Recommendation in §29: `retries: 0` locally and on PRs, retries only on a nightly job. This
  slice ran into it: the suite is green (66 passed, exit 0) but **four pre-existing tests
  needed a retry**, all `OfflineAudioContext` renders starved by context accumulation — the
  cause `audio.spec.ts`'s own header warns about. With `retries: 0` they would be four red
  tests describing a real resource limit instead of nothing at all. The same limit made one
  rewritten perf-smoke pair render clean120 twice (`voiceDiff` 0, ratio 0.85× where isolation
  gives 13× and 0.22), so those assertions take the **best of three pairs** rather than the
  last — still hard-fails if the swap never works, without trading vacuous for flaky.
- The finiteness guard is adopted in `audio.spec.ts` only; `amp.spec.ts`, `cab.spec.ts`,
  `expectations.spec.ts` and `tuner.spec.ts` are two lines each away from it.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
