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

## 2026-07-31 — 1 golden(s) re-blessed

- **Blessed by:** Claude
- **On top of:** `bed7722` fix: re-derive the JCM800 GAIN pot taper so breakup lands at 30, not 20

| Golden | Status | Broadband RMS Δ | Worst third-octave band Δ | Bands |
| --- | --- | --- | --- | --- |
| `rat_jcm800` | CHANGED | -1.08 dB | 4.50 dB @ 2016 Hz | 11 |
| `sd1_twin_reverb` | UNCHANGED | +0.00 dB | 0.02 dB @ 3200 Hz | 12 |
| `muff_twin` | UNCHANGED | +0.00 dB | 0.01 dB @ 252 Hz | 13 |
| `ts_ac30` | UNCHANGED | +0.00 dB | 0.00 dB @ 2016 Hz | 8 |
| `clean120_chorus` | UNCHANGED | -0.00 dB | 0.11 dB @ 252 Hz | 7 |

**Justification:** The JCM800 GAIN taper re-derivation (docs §51): the owner's round-3 report ("20 sounds like what I want 30 to sound like; 100 is perfect right where it is") became the design equation — gainTaper(0.30) == the old audioTaper(0.20), k 4 → 5.052 on the GAIN pot only. The rig renders at GAIN 0.7, which now delivers 2.6 dB less drive (the power section returns about half), hence −1.08 dB RMS with the worst band 4.50 dB @ 2016 Hz — the intended knob-feel change at the golden's fixed knob position, not a voicing drift; GAIN 1.0 is bit-identical by render hash and MASTER is byte-identical. The other four goldens are UNCHANGED (scope check). Owner authorized blessing on 2026-07-31 ("let's merge 29,30 and bless") with the table presented in PR #29 and in-chat; same hand-run ritual as the earlier 2026-07-31 entries (no /dev/tty — --golden-report verified against the approved figures before --update-goldens).

- **Blessed by:** Claude
- **On top of:** `8809984` fix: the Muff clip stages' series base resistors — finding 16's bass half (docs §49)

| Golden | Status | Broadband RMS Δ | Worst third-octave band Δ | Bands |
| --- | --- | --- | --- | --- |
| `muff_twin` | CHANGED | -3.06 dB | 3.76 dB @ 4032 Hz | 13 |
| `rat_jcm800` | UNCHANGED | +0.00 dB | 0.00 dB @ 4032 Hz | 11 |
| `sd1_twin_reverb` | UNCHANGED | +0.00 dB | 0.02 dB @ 3200 Hz | 12 |
| `ts_ac30` | UNCHANGED | +0.00 dB | 0.00 dB @ 2016 Hz | 8 |
| `clean120_chorus` | UNCHANGED | -0.00 dB | 0.11 dB @ 252 Hz | 7 |

**Justification:** The Muff clip stages' series base resistors (docs §49, finding 16's bass half): without the schematic's 10 k series base resistance the feedback diodes could not form their limiting divider, so the stages blew past the ±0.6 V clamp and preferentially amplified each other's distortion products over the note. The -3.06 dB RMS / 3.76 dB @ 4032 Hz is the un-blown-out default voice: less high-band hash (the 470 pF Miller cap finally forms its ~1.2 kHz anti-harshness rolloff against the new base impedance) and the honest level of a stage that now clips where the circuit says it should. At max sustain the audible change is much larger (150 → 40 % THD at unchanged level — the wall became articulate); the default-knob golden moves modestly because the default was only partially blown out. The other four goldens are byte-identical (Rs = 0 reduces exactly to the stock solver, hash-verified — RAT/GOLD/SD/TS untouched). Owner authorized blessing on 2026-07-31 with this table presented; same hand-run ritual as the entries below (no /dev/tty in this environment — table measured via --golden-report against the previous goldens before writing).

## 2026-07-31 — 1 golden(s) re-blessed

- **Blessed by:** Claude
- **On top of:** `d6a2ea2` fix: halve the spring reverb's wet trim — the "twice as strong" report (docs §48)

| Golden | Status | Broadband RMS Δ | Worst third-octave band Δ | Bands |
| --- | --- | --- | --- | --- |
| `sd1_twin_reverb` | CHANGED | -0.83 dB | 6.02 dB @ 504 Hz | 13 |
| `rat_jcm800` | UNCHANGED | +0.00 dB | 0.00 dB @ 4032 Hz | 11 |
| `muff_twin` | UNCHANGED | +0.00 dB | 0.00 dB @ 5080 Hz | 13 |
| `ts_ac30` | UNCHANGED | +0.00 dB | 0.00 dB @ 2016 Hz | 8 |
| `clean120_chorus` | UNCHANGED | -0.00 dB | 0.11 dB @ 252 Hz | 7 |

**Justification:** The spring reverb wet trim halved (docs §48): the owner's field report "the spring reverb is about twice as strong" measured true — wet reached parity with dry at knob 0.40 and sat +6.5 dB OVER it at 0.60 — and was answered with exactly -6 dB (kWetGain 3.0 → 1.5, knob law untouched, parity now ≈ 0.60). sd1_twin_reverb is the only golden rendering reverb (its rig sets 0.25); the -0.83 dB RMS / 6.02 dB @ 504 Hz is the wet component stepping back to the intended balance, concentrated in the spring's low-mid dwell. Every other rig renders reverb 0 and is byte-identical (scope check). Owner authorized blessing on 2026-07-31 with this table presented; same hand-run ritual as the rat_jcm800 entry below (no /dev/tty in this environment — table measured via --golden-report against the previous goldens before writing).

## 2026-07-31 — 1 golden(s) re-blessed

- **Blessed by:** Claude
- **On top of:** `0b81c0e` feat: the JCM800 gain-pot bright cap — the 470 pF the model never had (docs §47)

| Golden | Status | Broadband RMS Δ | Worst third-octave band Δ | Bands |
| --- | --- | --- | --- | --- |
| `rat_jcm800` | CHANGED | -0.44 dB | 6.73 dB @ 1008 Hz | 11 |
| `sd1_twin_reverb` | UNCHANGED | +0.00 dB | 0.02 dB @ 317 Hz | 13 |
| `muff_twin` | UNCHANGED | +0.00 dB | 0.00 dB @ 5080 Hz | 13 |
| `ts_ac30` | UNCHANGED | +0.00 dB | 0.00 dB @ 2016 Hz | 8 |
| `clean120_chorus` | UNCHANGED | -0.00 dB | 0.11 dB @ 252 Hz | 7 |

**Justification:** The JCM800 gain-pot bright cap (docs §47): the 2204's 470 pF top-lug-to-wiper cap now exists, tilting the drive spectrum into V1B by the measured +7.8 dB at GAIN 0.5 / +5.6 dB at 0.7 (analytic 7.5/5.5). rat_jcm800's -0.44 dB RMS / 6.73 dB @ 1008 Hz is the mid-gain drive path getting the brightness the real amp has; GAIN 1.0 is bit-identical by construction and settled levels are unchanged (H(0) equals the pre-fix scalar). The other four goldens are UNCHANGED — byte-identical rewrites (scope check). Owner authorized blessing on 2026-07-31 with this table presented; the script's /dev/tty confirmation is unavailable in this environment, so its gates were satisfied by hand on that authorization: clean tree, the table above measured against the previous goldens via --golden-report before writing, justification recorded here.

## 2026-07-31 — 2 golden(s) re-blessed

- **Blessed by:** Claude
- **On top of:** `e0b5081` feat: complete the AC30 top-boost channel — gain structure + stack (docs §46, ADR 015)

| Golden | Status | Broadband RMS Δ | Worst third-octave band Δ | Bands |
| --- | --- | --- | --- | --- |
| `ts_ac30` | CHANGED | -4.94 dB | 17.96 dB @ 800 Hz | 7 |
| `rat_jcm800` | CHANGED | +0.14 dB | 0.26 dB @ 1008 Hz | 11 |
| `sd1_twin_reverb` | UNCHANGED | +0.00 dB | 0.02 dB | 13 |
| `muff_twin` | UNCHANGED | +0.00 dB | 0.00 dB | 13 |
| `clean120_chorus` | UNCHANGED | -0.00 dB | 0.11 dB @ 252 Hz | 7 |

**Justification:** The AC30 gain-structure slice (docs §46, ADR 015): the completed top-boost channel (missing V2 gain stage + cathode follower), the corrected tone stack (audit finding 5's series-Cb + wiper load, plus the in-slice-found slope-resistor placement), and the re-derived volume law. The +17.96 dB at 800 Hz IS the fix — the structural mid notch filling back in; the −4.94 dB broadband is the honest kFullScaleSecV re-derivation from the amp's real cranked swing (the finding-7 Twin precedent: honest level, un-compensated). Owner authorized blessing ts_ac30 on 2026-07-31 with this table presented. rat_jcm800's +0.14/0.26 is NOT this slice: it is the §45-documented finding-8 drift (inside the gates, deliberately left unblessed there) being absorbed at this bless so the golden matches the shipped render again.

## 2026-07-30 — 2 golden(s) re-blessed

- **Blessed by:** Claude
- **On top of:** `4e8d338` fix: move the Twin VOLUME pot to the AB763 position — headroom back (docs §44)

| Golden | Status | Broadband RMS Δ | Worst third-octave band Δ | Bands |
| --- | --- | --- | --- | --- |
| `sd1_twin_reverb` | CHANGED | +2.18 dB | 5.00 dB @ 2540 Hz | 13 |
| `muff_twin` | CHANGED | +5.85 dB | 4.82 dB @ 4032 Hz | 12 |
| `rat_jcm800` | UNCHANGED | +0.00 dB | 0.00 dB | 11 |
| `ts_ac30` | UNCHANGED | +0.00 dB | 0.00 dB | 7 |
| `clean120_chorus` | UNCHANGED | -0.00 dB | 0.11 dB @ 252 Hz | 7 |

**Justification:** The Twin VOLUME pot moved to the AB763 position (docs §44): it was after the V2 recovery stage, so V2's drive was volume-independent and the pedal-driven Twin rigs — exactly these two goldens — were being permanently compressed by V2 at their drive levels. Louder (+2.18/+5.85 dB) and brighter (worst bands at 2.5/4 kHz) is that compression removed, not a drift: the amp-alone level is unchanged to 0.08 dB at every knob position and VOL 1.0 is bit-identical by construction. rat_jcm800 and ts_ac30 are byte-identical (scope check). Owner authorized blessing both on 2026-07-30 with this table presented.

## 2026-07-30 — 4 golden(s) re-blessed

- **Blessed by:** Claude
- **On top of:** `e4b07a6` build: rebuild the WASM artifact for the finding-7 core changes

| Golden | Status | Broadband RMS Δ | Worst third-octave band Δ | Bands |
| --- | --- | --- | --- | --- |
| `rat_jcm800` | CHANGED | -1.77 dB | 1.80 dB @ 200 Hz | 11 |
| `sd1_twin_reverb` | CHANGED | -4.89 dB | 5.00 dB @ 317 Hz | 13 |
| `muff_twin` | CHANGED | -4.74 dB | 4.92 dB @ 800 Hz | 12 |
| `ts_ac30` | CHANGED | +0.44 dB | 1.22 dB @ 800 Hz | 7 |
| `clean120_chorus` | UNCHANGED | -0.00 dB | 0.11 dB @ 252 Hz | 7 |

**Justification:** Audit finding 7 (docs §42, ADR 014): the PI tail reference. rat_jcm800 -1.77 dB and sd1_twin_reverb/muff_twin -4.9/-4.7 dB are the JCM/Twin spanning their real rated power for the first time (kFullScaleSecV re-derived on its own cranked-swing convention); ts_ac30 +0.44 dB is the AC30 PI balance fix (0.550 -> 0.912). clean120_chorus unchanged (scope check). Owner authorized blessing all four on 2026-07-30 with the per-band tables presented.


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
- **On top of:** `f8ca424` wip: output coupling cap only (pre-bless checkpoint)

| Golden | Status | Broadband RMS Δ | Worst third-octave band Δ | Bands |
| --- | --- | --- | --- | --- |
| `rat_jcm800` | UNCHANGED | +0.00 dB | 0.00 dB @ 1008 Hz | 10 |
| `sd1_twin_reverb` | UNCHANGED | +0.00 dB | 0.07 dB @ 317 Hz | 13 |
| `muff_twin` | CHANGED | -0.03 dB | 0.80 dB @ 5080 Hz | 13 |
| `ts_ac30` | UNCHANGED | +0.00 dB | 0.00 dB @ 1008 Hz | 7 |
| `clean120_chorus` | UNCHANGED | -0.00 dB | 0.11 dB @ 252 Hz | 7 |

**Justification:** Audit finding 16, DC half only. The Muff had NO output high-pass at all: BjtStage returns Vc - vcQ_, which removes the QUIESCENT DC but not the dynamic DC four asymmetrically-clipping common-emitter stages rectify. Measured before: up to +0.47 V, 28 percent of peak, on signal. This adds the component the real pedal has and the model did not - 0.1 uF into the 100 k VOLUME pot, f = 15.92 Hz - placed after Q4 and before the VOLUME multiply, inside the oversampled domain. Measured after: 0.00000-0.00003 percent of peak across three sustain settings and a +0.1 V input-offset case, against a 1 percent bar. The golden moves only -0.03 dB broadband with a 0.80 dB worst band at 5080 Hz, i.e. removing DC from a fuzz is nearly inaudible in the spectrum; the audible win is that the output no longer rides on an offset. Deliberately does NOT include the series base resistors that fix the same finding's BASS half - that is a separate slice because its value departs from the schematic to compensate for a clip-stage base-node impedance this model measures at ~1.8 k against the real stage's ~4 k (ADR 009). The bass defect remains measured and named by XFAIL finding16-muff-almost-no-bass at low E -41.14 dB. Owner authorised the split and the DC half landing on 2026-07-25; prompt answered via pty on that authorisation.


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
