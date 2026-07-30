# The JCM800's 470 pF gain-pot bright cap — the chug-flab fix

**Date:** 2026-07-30
**Branch:** claude/amps-pedals-fixes-6f557i
**Roadmap item:** slice 4 of the field-report round ("still feels flabby, especially with chugs/palm
mutes") — a new circuit finding from the 2026-07-30 diagnosis, not on the audit ledger

## Goal

The 2204's 470 pF bright cap across the gain pot exists in the model: at mid-gain settings the
clipping stages receive the real amp's HF-tilted spectrum instead of today's frequency-flat one
(measured gap: 10–18 dB of relative sub-200 Hz excess into V1B at GAIN 0.5–0.7), and mid-gain
palm mutes tighten accordingly.

## Approach

Deliberate tone change, derived entirely from the real parts. The model's gain network is a
frequency-flat scalar (`gainInterstageScale() = kGainDivider · audioTaper(gain)`, applied
per-sample in `Jcm800Preamp::process`). The real 2204 hangs 470 pF from the gain pot's top lug
to its wiper, so the network is a frequency-dependent divider:

    H(s) = w · (1 + sτ) / (1 + w·sτ),   τ = (1−w) · Rpot · C,   Rpot = 1 M, C = 470 pF

— LF gain w (exactly today's wiper scale), HF gain 1 (the cap shorts the top segment), zero at
1/(2πτ), pole at 1/(2π·w·τ). At w = 0.119 (GAIN 0.5): zero ≈ 385 Hz, relative HF lift +18.5 dB.
At w = 1 the network collapses to today's flat unity — **fully-open GAIN is unchanged by
construction**. The shelf multiplies the existing `kGainDivider` series loss, which is
frequency-flat and stays.

Implementation: a one-pole/one-zero shelf inside the per-sample gain loop, coefficients derived
from the *smoothed* wiper value at the tone-stack pattern's 32-sample control rate (recomputing
two exp-free bilinear coefficients; the smoother's `settled()` gates recomputation), state
`flushDenormal`-guarded (rests at zero), included in `reset()`. The shelf lives at base rate
(linear network, pre-V1B) exactly where the scalar multiply lives today.

## Steps

- [ ] Implement the shelf in `Jcm800Preamp` (state + coeffs + reset + denormal guard); keep
      `gainInterstageScale()` reporting the LF scale (its meaning today)
- [ ] Measure: relative HF-vs-LF drive into V1B at GAIN 0.3/0.5/0.7/1.0 against the analytic
      H(s) from the same parts (discretization check) AND the +18.5/+10.8 dB diagnosis figures;
      full-amp response at 75/220/880 Hz per GAIN; palm-mute burst spectrum before/after
- [ ] New test: the bright-cap property — perturbation-proven (C = 0 restores flat and fails);
      re-derive any preamp gain test that assumed the flat scalar
- [ ] Full-amp THD-vs-GAIN re-measure (the 220 Hz floor will drop further at mid gain — the
      probe sits below the shelf zero); A2/A3 JCM reference rows re-baselined as needed
- [ ] `--golden-report`: `rat_jcm800` WILL move brighter (the RAT drives the JCM at GAIN 0.7,
      w = 0.288, +10.8 dB HF tilt) — expect outside the gates → owner bless with the table
- [ ] Aliasing re-measure at max gain (more HF into the clippers; at GAIN 1.0 the shelf is
      unity so the max-gain alias corner should be unmoved — verify, don't assume)
- [ ] Full core ctest; WASM rebuild; web build + Playwright (drift guard may need the JCM row
      re-centred — measure); node suites
- [ ] Docs §46, CLAUDE.md, this file; commit, push, PR

## How this will be measured

The V1B-drive spectrum tilt at mid gain (the 10–18 dB figure closing to the analytic H), the
discrete-vs-analytic shelf comparison (< 0.5 dB to Nyquist/4), the palm-mute burst spectrum
(sub-200 Hz relative energy into the clippers down ~10 dB at GAIN 0.5), the full-amp 75-vs-880 Hz
output tilt (was +4.6 dB LF-heavy at GAIN 0.5), and the golden table for the owner.

## Manual test steps

- [ ] Native/web at trim 33, GAIN 40–60: palm mutes tighter, less woofy; open chords get the
      classic cutting JCM top instead of dark mush
- [ ] GAIN 100: character unchanged (the shelf is unity fully open)
- [ ] Sweep GAIN slowly 20 → 80 while playing: no zipper (coefficients follow the smoothed
      wiper), brightness rises as gain falls — the real amp's behavior
- [ ] Edge: NaN to PARAM_GAIN rejected; reset clean (nan-guard block C covers new state)

## Out of scope for this session

The GAIN-pot taper decision (owner re-tests after this lands), the push-pull residual XFAIL,
the JCM latency, every other amp/pedal.

---

<!-- Fill in below during/after the session -->

## What actually happened

## Measured results

## Files created / modified

## Deferred to next session

## Status

- [ ] In progress
- [ ] Complete
- [ ] Partial — see deferred
