# Golden renders — changelog

The `.wav` files beside this one are the M11 "first five minutes" reference renders
(`core/tests/test_player_expectations.cpp`, block C). They are the project's only
defence against voicing drift, and they are binary: a reviewer looking at a PR diff
can see that `rat_jcm800.wav` changed but not *how*. This file is the "how".

**Every change to a golden `.wav` must add an entry here, in the same commit.**
`scripts/update-goldens.sh` writes the entry for you and stages it alongside the
goldens; CI's `goldens` job fails any PR that changes a `.wav` in this directory
without touching this file.

Re-blessing is a deliberate act. To do it:

```bash
bash scripts/update-goldens.sh -m "why the new render is the correct one"
```

which will refuse to run on a dirty tree, print the per-golden third-octave band
deltas in dB against the *previous* goldens, require a confirmation typed at a
terminal, and require the justification above. If you find yourself re-blessing to
make a red test green, stop — that is the exact failure mode the gate exists to
prevent (CLAUDE.md → "Don't re-bless goldens to make a failing test pass").

Reading an entry: **UNCHANGED** means the fresh render matched the old golden to
within the 16-bit storage floor (0.15 dB) — those goldens were rewritten only
because the run rewrites all five. **CHANGED** is a real voicing difference, and
the worst-band number is the one to argue about; the gate itself trips at 1.5 dB.

## The five rigs

| Golden | Rig |
| --- | --- |
| `rat_jcm800.wav` | RAT → JCM800 2204 + brit412 cab |
| `sd1_twin_reverb.wav` | SD-1 → blackface Twin + clean212 cab + spring reverb 0.25 |
| `muff_twin.wav` | Muff Pi → clean Twin + clean212 cab (the Gilmour move) |
| `ts_ac30.wav` | Screamer → AC30 top boost + clean212 cab |
| `clean120_chorus.wav` | Clean 120 + chorus + clean212 cab (stereo, stored as the mono mix) |

All five: 2.0 s standard pluck, 16-bit mono 48 kHz.

---

<!-- New entries go directly below this line, newest first. -->

## 2026-07-25 — 1 golden(s) re-blessed

- **Blessed by:** Claude
- **On top of:** `4b557f7` build: rebuild the WASM artifact for the corrected diode ideality

| Golden | Status | Broadband RMS Δ | Worst third-octave band Δ | Bands |
| --- | --- | --- | --- | --- |
| `rat_jcm800` | CHANGED | -0.15 dB | 6.50 dB @ 800 Hz | 10 |
| `sd1_twin_reverb` | UNCHANGED | +0.00 dB | 0.07 dB @ 317 Hz | 13 |
| `muff_twin` | UNCHANGED | +0.00 dB | 0.00 dB @ 252 Hz | 13 |
| `ts_ac30` | UNCHANGED | +0.00 dB | 0.00 dB @ 1008 Hz | 7 |
| `clean120_chorus` | UNCHANGED | -0.00 dB | 0.11 dB @ 252 Hz | 7 |

**Justification:** Audit finding 15: RatModel's WDF clipper carried the 1N4148's Is=2.52nA from its SPICE card but with the ideality factor dropped to 1.0, where the same card reads N=1.752. The pair therefore clipped at 0.32-0.43 V instead of 0.6-0.7 V - germanium territory, 5-6 dB low - while three places in the tree documented +/-0.6 V. Measured clipping node after the fix: 0.727/0.737 V peak with a 0.664 V soft-knee toe, inside the 1N4148 datasheet's 0.60-0.85 V forward-drop band; before it measured 0.381 V at the shipped default DISTORTION 0.7. The OLD golden encoded the defect, so this render is the correct one. Broadband RMS moves only -0.15 dB (the JCM800 is already compressing) but the energy redistributes: +6.50 dB @ 800 Hz, -5.85 dB @ 1008 Hz, +4.89 dB @ 1600 Hz, +2.96 dB @ 2540 Hz. The other four goldens are UNCHANGED (<= 0.11 dB), which is the scope check. Approved by the project owner on 2026-07-25 after reviewing this per-band table; the confirmation prompt was answered via a pty on that explicit authorization. Docs section 36, ADR 008.


## 2026-07-25 — changelog opened (no goldens changed)

- **Blessed by:** n/a — this entry records the state the changelog starts from.
- **On top of:** the `chore/artifact-staleness` slice.

No `.wav` was rewritten. The five goldens beside this file are the ones blessed
with the M11 Player Expectations Suite and last verified by the
`fix/cab-block-size` slice, which kept the 128-aligned convolver path
bit-identical precisely so they would not need re-blessing.

Recorded here because before this slice `--update-goldens` compared each fresh
render against **the file it had just written**, so every historical re-bless
reported a worst-band delta of ≤0.11 dB no matter what actually changed. Those
numbers, wherever they were quoted, measured 16-bit quantisation and nothing else.
Deltas from this point on are measured against the previous golden.
