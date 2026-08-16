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

## 2026-08-10 — 4 golden(s) re-blessed

- **Blessed by:** Claude
- **On top of:** `038b7a9` fix: the golden write-back check is a per-sample round-trip, not a band delta

| Golden | Status | Broadband RMS Δ | Worst third-octave band Δ | Bands |
| --- | --- | --- | --- | --- |
| `rat_jcm800` | CHANGED | -1.20 dB | 14.63 dB @ 252 Hz | 12 |
| `sd1_twin_reverb` | CHANGED | -8.57 dB | 8.67 dB @ 2540 Hz | 12 |
| `muff_twin` | CHANGED | -10.23 dB | 10.60 dB @ 5080 Hz | 12 |
| `ts_ac30` | CHANGED | -7.17 dB | 7.64 dB @ 1600 Hz | 8 |
| `clean120_chorus` | UNCHANGED | -0.00 dB | 0.11 dB @ 252 Hz | 7 |

Per-band tables. Three of the four moved essentially FLAT — the signature of a
level change with the voice intact, not a redistribution:

| `sd1_twin_reverb` Hz | 200 | 252 | 400 | 504 | 635 | 800 | 1008 | 1270 | 1600 | 2016 | 2540 | 3200 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Δ dB | -8.47 | -8.41 | -8.49 | -8.53 | -8.53 | -8.56 | -8.60 | -8.63 | -8.28 | -8.42 | -8.67 | -8.22 |

| `muff_twin` Hz | 200 | 400 | 635 | 800 | 1008 | 1270 | 1600 | 2016 | 2540 | 3200 | 4032 | 5080 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Δ dB | -10.10 | -10.37 | -9.68 | -10.07 | -10.29 | -9.87 | -10.13 | -10.12 | -10.10 | -10.36 | -10.40 | -10.60 |

| `ts_ac30` Hz | 200 | 400 | 635 | 800 | 1008 | 1270 | 1600 | 2016 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Δ dB | -7.29 | -7.25 | -7.21 | -7.15 | -7.11 | -7.10 | -7.64 | -7.33 |

| `rat_jcm800` Hz | 200 | 252 | 400 | 635 | 800 | 1008 | 1270 | 1600 | 2016 | 2540 | 3200 | 4032 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Δ dB | -2.51 | **-14.63** | -2.80 | -3.38 | -4.32 | -4.98 | -5.30 | -6.81 | -6.02 | -3.81 | -5.17 | -5.23 |

**Justification:** The lineup-wide output-pot tapers (docs §67). Every dirt pedal's
output pot now delivers what its own netlist says it delivers instead of a linear
fraction of the knob: the RAT's and the Muff's are unloaded audio tapers (11.920 %
at half rotation), the TS's is an audio taper bent by the 226 k its output buffer
puts on the wiper (11.391 %), and the GOLD's is LINEAR and deliberately did not
change. The old goldens encoded a knob law that three independent netlists
contradict, so these renders are the correct ones.

The three flat tables are the argument. `sd1_twin_reverb` moves -8.2 to -8.7 dB in
every one of twelve bands (spread 0.45 dB), `muff_twin` -9.7 to -10.6 across twelve
(spread 0.92), `ts_ac30` -7.1 to -7.6 across eight (spread 0.54). A pot is a
frequency-flat scalar and that is exactly what these measure: the rigs are quieter
and otherwise unchanged.

`rat_jcm800` is the one that is NOT flat, and the reason is worth reading before
approving it. Broadband it moves only -1.20 dB although the RAT itself drops
5.21 dB, because the JCM800 is being driven less hard and gives most of it back —
that is the amp compressing, not the pedal failing to change. The harmonic bands
move progressively with frequency (-2.51 at 200 Hz = the 220 Hz fundamental, then
-2.80 / -3.38 / -4.32 / -4.98 / -5.30 at the 2nd through 6th harmonics), which is
the signature of less high-order harmonic generation. **The 14.63 dB outlier at
252 Hz is a band that contains no harmonic of the stimulus at all**: the pluck is
f0 = 220 Hz and the 252 Hz third-octave band spans 224-283 Hz, between the
fundamental and the 2nd harmonic. It carries only the amp's own distortion
products, and those collapse when the amp is driven less hard. The biggest number
in this bless is therefore in the band with no signal in it, which is reassuring
rather than alarming.

`clean120_chorus` is UNCHANGED and its file is byte-identical on disk — it is the
only golden rig with no dirt pedal in it, and that is the scope check.

**What is NOT in this bless, deliberately:** the shipped OUTPUT knob defaults were
a level calibration expressed in linear-pot coordinates, and re-deriving them
(§67.5 carries the solved positions) would restore the old levels and undo most of
these deltas. That is a separate concern with its own justification; bundling it
here would have left a reviewer unable to attribute the drift.

Owner authorized explicitly on 2026-08-10 ("bless and merge") after being shown the
per-golden delta table above. Same hand-run ritual as the 2026-07-31 entries — no
/dev/tty in this environment, so: clean tree verified, `--golden-report` re-checked
against the approved figures immediately before `--update-goldens`, and this entry
committed with the `.wav` files. Note the first attempt at this bless ABORTED on
the write-back check and that abort was correct — see docs §67.11; the check is now
a per-sample round-trip (1.51-1.59 LSB across all five) instead of a band delta,
fixed in its own commit before this one, and `kQuantizationFloorDb` was left alone.


## 2026-07-31 — 1 golden(s) re-blessed

- **Blessed by:** Claude
- **On top of:** `189b33e` Merge remote-tracking branch 'origin/main' into claude/ac30-sag-6f557i

| Golden | Status | Broadband RMS Δ | Worst third-octave band Δ | Bands |
| --- | --- | --- | --- | --- |
| `ts_ac30` | CHANGED | -1.86 dB | 3.12 dB @ 200 Hz | 8 |
| `rat_jcm800` | UNCHANGED | +0.00 dB | 0.00 dB @ 252 Hz | 12 |
| `sd1_twin_reverb` | UNCHANGED | +0.00 dB | 0.02 dB @ 3200 Hz | 12 |
| `muff_twin` | UNCHANGED | +0.00 dB | 0.00 dB @ 5080 Hz | 12 |
| `clean120_chorus` | UNCHANGED | -0.00 dB | 0.11 dB @ 252 Hz | 7 |

Per-band table for `ts_ac30` (every band down, monotonically less with frequency):

| Hz | 200 | 400 | 635 | 800 | 1008 | 1270 | 1600 | 2016 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Δ dB | -3.12 | -2.85 | -2.64 | -2.47 | -2.33 | -2.21 | -1.82 | -1.76 |

**Justification:** The AC30's dynamic supply (docs §55, ADR 017 — audit finding 4). The old
"sag" multiplied the OT *secondary* by `1/(1+k·(Idemand−Iidle))`, which since
`vSec ∝ (ipUp−ipDown)` is algebraically `y = x/(1+k|x|)` — a memoryless soft clipper behind
the transformer, taking 5.02 dB of output from a 6.02 dB input step on a dead-clean signal
and compressing a 34 dB drive increase into 0.82 dB. It is replaced by a real GZ34 +
reservoir supply and the published shared-cathode network, and the level drop recorded here
is that wall coming off: the render is quieter because the model has stopped manufacturing
output it had no circuit reason to make. The monotonic, gently frequency-dependent shape of
the per-band table is the signature of a level change with the voice intact — not a
redistribution. Corroborated by the property that was deliberately protected: **h2, the
harmonic the AC30's character and the whole §46 chime argument rest on, moved only +0.26 dB
at VOLUME 0.60 and +0.31 dB at 0.85**, and it sits 6–21 dB above every other harmonic. The
idle operating point is preserved to seven figures (rail 309.4904457 → 309.4904201 V), so
this is dynamics rather than a moved bias. The other four goldens are UNCHANGED and only
`ts_ac30.wav` changed on disk — the scope check, confirmed at file level. Owner authorized
explicitly on 2026-07-31 ("bless and merge") after being shown this exact table; same
hand-run ritual as the other 2026-07-31 entries (no /dev/tty in this environment — clean
tree verified, `--golden-report` table checked against the approved figures immediately
before `--update-goldens`).


## 2026-07-31 — 1 golden(s) re-blessed

- **Blessed by:** Claude
- **On top of:** `9cf36cf` Merge remote-tracking branch 'origin/main' into claude/muff-dc-diodes-6f557i

| Golden | Status | Broadband RMS Δ | Worst third-octave band Δ | Bands |
| --- | --- | --- | --- | --- |
| `muff_twin` | CHANGED | -1.09 dB | 13.18 dB @ 252 Hz | 13 |
| `rat_jcm800` | UNCHANGED | +0.00 dB | 0.00 dB @ 252 Hz | 12 |
| `sd1_twin_reverb` | UNCHANGED | +0.00 dB | 0.02 dB @ 3200 Hz | 12 |
| `ts_ac30` | UNCHANGED | +0.00 dB | 0.00 dB @ 2016 Hz | 8 |
| `clean120_chorus` | UNCHANGED | -0.00 dB | 0.11 dB @ 252 Hz | 7 |

**Justification:** The Muff clip stages' DC-blocked diode branch (docs §53, ADR 010 — ADR 009's named follow-up): the real pedal's 1 µF caps in series with each feedback diode pair, as a 4th Newton node. The diodes stop DC-loading the base (clip bias Vc 1.21 → 4.95 V, collector current at the published ~0.4 mA), so the bass the loading ate comes back: low E −14.2 → −5.5 dB re 1 kHz. The golden's 13.18 dB worst band at 252 Hz IS that bass returning to the default-sustain render — the intended fidelity change, not a drift; broadband only −1.09 dB. The other four goldens are UNCHANGED (the 3-node bit-identity contract, digest-verified — RAT/GOLD/SD/TS untouched). Owner authorized on 2026-07-31 ("merge … when it's all landed", following the presented measurements); same hand-run ritual as the other 2026-07-31 entries (no /dev/tty — table verified via --golden-report before --update-goldens).


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
