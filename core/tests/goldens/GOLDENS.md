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
