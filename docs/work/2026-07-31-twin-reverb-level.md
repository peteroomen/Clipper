# The spring reverb's wet trim — "about twice as strong" measured and halved

**Date:** 2026-07-31 (overnight)
**Branch:** claude/twin-reverb-6f557i (stacked on claude/jcm-bright-cap-6f557i)
**Roadmap item:** owner field report at unity trim ("the reverb is still about twice as strong
as I'd expect, at least on the twin sixty five") — confirmed trim-independent

## Goal

The Twin's reverb knob sits where a blackface mix feels: wet at parity with the dry around
knob 0.6 instead of 0.4, the golden's 0.25 setting clearly *under* the note.

## Approach

Control-law calibration, one constant: `ReverbModel`'s `kWetGain` 3.0 → 1.5 (−6.02 dB of wet
everywhere — exactly the owner's "about twice as strong", inverted). The equal-power squared
knob law is untouched: 0 stays bit-exact dry, the whole wet curve shifts down 6 dB. kWetGain
was a documented taste constant ("sits musically against the dry"), so the owner's calibrated
ears at unity trim are the correct derivation source.

## Measured (decaying 220 Hz note through the composed TwinAmp, 48 kHz)

| knob | wet under note, before | after |
|---|---|---|
| 0.15 | −17.5 dB | −23.5 |
| 0.25 (golden) | −8.6 | **−14.6** |
| 0.40 | **−0.4 (parity!)** | −6.2 |
| 0.60 | **+6.5 (wet OVER dry)** | +1.0 (parity) |
| 1.00 | +13.1 | +8.5 |

## Steps

- [x] kWetGain 3.0 → 1.5 with the measured rationale in the comment
- [x] Suites: twin (mix-0 bit-exactness + wet-presence bar), amp-model, param-smoothing — green
- [x] Golden report: `sd1_twin_reverb` (reverb 0.25 in-rig) **−0.83 dB RMS / 6.02 dB @ 504 Hz**
      — bless HELD for the owner (draft PR); other rigs render reverb 0 → byte-stable
- [x] WASM rebuild; docs §48; CLAUDE.md
- [ ] Owner bless + merge (morning)

## Manual test steps

- [ ] Twin at the same reverb knob position as the report: about half the reverb — present,
      springy, under the guitar
- [ ] Reverb 0: bit-exact dry (unchanged guarantee); full-wet still drippy, 6 dB politer
- [ ] Clean 120's reverb (shared model) halves the same way — same knob feel across amps

## Out of scope

The spring dispersion/decay character (unchanged); a per-amp wet trim (one shared constant is
the model's design; if the owner wants the Clean 120 hotter it's its own decision).

## Status

- [x] Complete (pending the owner's golden bless)
