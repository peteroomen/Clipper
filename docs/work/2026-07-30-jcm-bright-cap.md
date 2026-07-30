# JCM800 gain-pot bright cap — the 470 pF the model never had

**Date:** 2026-07-30 (overnight)
**Branch:** claude/jcm-bright-cap-6f557i
**Roadmap item:** the "flabby chugs" half of the JCM field report (slice 4 of the round); a new
circuit finding from the 2026-07-30 palm-mute diagnosis (no audit number — the audit missed it)

## Goal

The 2204's 470 pF bright cap across the gain pot exists in the model: at mid gain the clipping
stages receive the real amp's HF-tilted drive (measured +6–9 dB of 5 kHz-vs-110 Hz tilt at
playable gains) instead of the flat spectrum that made 90–110 Hz palm mutes clip full-bandwidth
("flabby chugs"), with GAIN 1.0 bit-identical by construction.

## Approach

Deliberate tone change at low/mid gain; a circuit component the model lacked. The gain network
(V1A → coupling → 470 k series → 1 M pot with the 470 pF top-lug→wiper cap → V1B's grid) is
solved exactly as a 2-node network: H(s) = (n0 + n1·s)/(d0 + d1·s) with n0 = Rl,
n1 = C·Ru·Rl, d0 = Rl+Rs+Ru, d1 = C·Ru·(Rl+Rs) (Rl = w·1M, Ru = (1−w)·1M, Rs = 470 k).
H(0) = kGainDivider·w — EXACTLY the old scalar, so settled levels are unchanged and the change
is purely spectral. H(∞) = Rl/(Rl+Rs). Bilinear one-pole/one-zero, coefficients re-derived at a
32-sample control rate from the smoothed wiper (finding-6 discipline), denormal-guarded state.
At wiper ≥ 0.9995 the network collapses to the DC divider (the cap bridges a shorted segment)
and the code takes the exact pre-slice scalar path — which is also what makes GAIN 1.0
bit-identical (verified by render hash).

**Honesty note:** the 2026-07-30 diagnosis quoted +10–18 dB of tilt from a pot-only law
(HF → 1/w). The full network — the series 470 k loads the shorted-cap divider — gives +8.0 dB
at GAIN 0.5 and +5.7 dB at 0.7. The exact solve ships; the diagnosis figure is corrected in §47.

## Steps

- [x] Implement the shelf in `Jcm800Preamp` (`rebuildBrightCap`, per-sample loop, reset/park)
- [x] Teach the chain-gain analytic test the network's |H(f)| (same nodal solve)
- [x] New perturbation-proven property test (`testBrightCap`): ratio-of-ratios tilt vs analytic
      (< 2 dB), ≥ 4 dB at mid gain, growing as the knob comes down; kBrightCapF = 0 fails it
- [x] GAIN 1.0 bit-identity: render hash equal to pre-slice build
- [x] Full core ctest in the worktree; aliasing + chunk-equivalence rows (green except the
      held golden gate; A3 JCM row re-baselined 9.3 → 10.8 mid — brighter drive spectrum)
- [x] `--golden-report`: rat_jcm800 measured **−0.44 dB RMS / worst band 6.73 dB @ 1008 Hz** —
      outside the ±1.5 band gate → **bless HELD for the owner** (the PR ships as draft with a
      red core job until the morning authorization; finding-15 precedent)
- [x] WASM rebuild in-worktree; web build + Playwright 71/71
- [x] Docs §47, CLAUDE.md Current State, this file; commit, push, DRAFT PR stacked per the
      owner's overnight instruction

## How this will be measured

The gain-network tilt table (5 kHz vs 110 Hz, rel GAIN 1.0): measured 7.8/5.6 dB at gain
0.5/0.7 vs analytic 7.5/5.5 (± 0.3 dB). GAIN 1.0 render hash identical. The golden report row
for `rat_jcm800` (its rig renders at GAIN 0.7 — the brighter drive re-voices the RAT rig's
spectrum; the owner judges with the table + ears).

## Manual test steps

- [ ] Native/web at trim 33, gain 40–60: palm mutes tighter/punchier, less woofy low-mid;
      open chords slightly brighter/cuttier
- [ ] GAIN at 100: identical to before (bit-identical by construction)
- [ ] Sweep the gain knob: no zipper (coefficients follow the smoothed wiper)
- [ ] Edge: NaN rejected at ABI; reset clean (nan-guard block C)

## Out of scope for this session

The GAIN-knob taper decision (re-measure after this + Ra2 both land and the owner re-tests);
the push-pull residual XFAIL; the master-volume network's own parasitics.

---

<!-- Fill in below during/after the session -->

## What actually happened

As planned. The exact 2-node solve replaced the diagnosis's pot-only law (honesty note above);
GAIN 1.0 bit-identity held by render hash; the chain-gain analytic test learned the same |H(f)|.
The A3 JCM mid-gain THD reference moved 9.3 → 10.8 % (brighter drive = more measured harmonic
energy at the 220 Hz probe) and was re-baselined. The rat_jcm800 bless is HELD for the owner —
draft PR with the golden gate red by design.

## Measured results

Tilt into V1B (5 kHz vs 110 Hz rel GAIN 1.0): 7.8 dB @ gain .5 (analytic 7.5), 5.6 @ .7
(analytic 5.5), 0.0 @ 1.0 (hash-identical). Perturbation kBrightCapF=0: red at the ≥4 dB bar.
rat_jcm800: −0.44 dB RMS / 6.73 dB @ 1008 Hz (bless pending). Playwright 71/71.

## Files created / modified

## Deferred to next session

## Status

- [ ] In progress
- [x] Complete (pending the owner's golden bless)
- [ ] Partial — see deferred
