# Rockerverb field report — "tinny / squashed / too much gain / too noisy"

**Date:** 2026-08-01
**Branch:** claude/rockerverb-field-report-6f557i
**Roadmap item:** owner field report on M10.7 (docs §63, PR #52) + the §63.11 /
§57.13 named follow-up "one shared oversampling domain"

## The report, verbatim

> "the rockverb is a bit tinny/squashed compared to the sunshine stack/orange clone in logic pro"
> "a touch brittle almost. it does have orange characteristics overall"
> "the gain needs scaling back too 100 is totally unusable"
> "too noisy, too much gain."

**RULE ZERO: the Sunshine Stack is another vendor's model, not a reference unit.**
It is used here only as a description of a symptom. Every change below is
justified by the circuit, by the netlist, or by a measured defect — §57's rule.

## Goal

Answer each of the four complaints with a measurement, fix what has a defensible
circuit- or architecture-level cause, and report plainly where the answer is
"this is what the netlist does".

## Hypotheses, and what the measurement said

Measured FIRST, before any code (all figures 48 kHz unless stated, 220 Hz /
0.15 V unity-trim probe, tone knobs noon — the §51 anchor).

### H1 — "brittle" and "noisy" are the 44.1 kHz alias floor — **SUPPORTED, TAKEN**

`rockerverb-alias-44k1` (§63.8). Re-measured on the house composed probe
(4186 Hz / 0.3 V):

| setting | 44.1 kHz 4x | 48 kHz 4x |
| --- | --- | --- |
| cranked G1.0 V1.0 | **−52.7 dB** | −80.1 dB |
| realistic G0.5 V0.10 | **−61.1 dB** | −71.5 dB |
| realistic G0.3 V0.10 | −104.3 dB | −114.9 dB |

This project synthesises no noise anywhere (§59), so "too noisy" can only be
foldover, unbounded HF products, or amplified input noise. −52.7 dB of
non-harmonic foldover is audible hash, and it is 27 dB worse at 44.1 kHz — the
rate Logic defaults to.

**Fix: ONE shared oversampling domain around the whole preamp + power cascade** —
the fix §63.8 and §57.13 both name, never a lower bar. Also cuts latency
(five 4x domains at 72 samples each) and is the same fix the OR120 needs.

### H2 — "squashed" is §63.3's un-modelled preamp grid blocking — **REFUTED**

Dynamic range, 220 Hz, VOLUME 0.10, input swept 0.02 → 0.40 V (a 26.0 dB span):

| GAIN | output span |
| --- | --- |
| 0.30 | **25.52 dB** |
| 0.50 | 10.45 dB |
| 0.70 | **1.01 dB** |
| 1.00 | **−0.04 dB** |

The amp has essentially perfect touch sensitivity at GAIN 0.30 and none at all at
0.70. **The squash is not a missing mechanism — it is where the knob puts you.**
Adding preamp grid blocking would put a bloom-and-recover on top of a brick wall;
it would not restore 25 dB of range. `TriodeStage` is therefore NOT touched, and
the four valve goldens are not at risk.

### H3 — "too much gain / 100 unusable" is the knob law — **CONFIRMED as a symptom, REFUTED as a taper fix**

Measured in WIPER space (taper-independent), 220 Hz / 0.15 V, VOLUME 0.10:

| wiper | knob (k=4) | THD % | RMS dBFS | dyn range (26 dB in) |
| --- | --- | --- | --- | --- |
| 0.002 | 0.026 | 1.06 | −72.50 | 25.99 |
| 0.030 | 0.240 | 4.89 | −25.38 | 26.33 |
| 0.080 | 0.416 | 15.86 | −10.22 | 17.02 |
| 0.120 | 0.501 | 26.41 | −9.00 | 10.33 |
| 0.200 | 0.615 | 29.57 | −8.33 | 2.63 |
| 0.500 | 0.831 | 32.36 | −8.34 | 0.04 |
| 1.000 | 1.000 | 34.31 | −8.55 | −0.04 |

So the useful wiper span is 0.002…0.20 and **everything above wiper 0.20 moves the
output 0.22 dB** — with the shipped k = 4 audio taper that is knob 0.615 → 1.00,
i.e. **38 % of the travel is dead**, and by the 1 dB level-ceiling criterion it is
dead from knob 0.50 (**50 %**).

**But a taper cannot fix it, and that is the refutation:**

* The house audio law `(e^{kx}−1)/(e^k−1)` puts the wiper at `1/(e^{k/2}+1)` at
  half rotation. k = 4 → **11.9 %**, inside the documented audio-taper spec
  (10–20 %) that §58 established as this repo's reference. k = 8 → **1.8 %**,
  which is not an audio pot at all. The most the spec allows is k = 4.394 (10.0 %),
  which moves the level ceiling from knob 0.500 to **0.537** — nothing.
* The geometry is fixed: clean→onset is 23.5 dB of the useful span and
  onset→saturated is 16.5 dB, so ANY monotone taper that puts saturation at the
  top of the travel puts the ≥5 % THD onset at ~59 % of it. Measured: k = 8 gives
  onset **0.56**, k = 10 gives **0.65**. A Rockerverb dirty channel that is clean
  at GAIN 6 is a worse model than one that is dirty at 3.

**What IS wrong, and is fixed: the shipped DEFAULT knob positions.** This voice
inherited `AMP_KNOB_DEFAULTS`' JCM values wholesale — GAIN 0.5 / master 0.4 —
even though §63.5 documents that its VOLUME is a **linear** master whose useful
range is 5–15, and the table above shows GAIN 0.5 is already 26 % THD with 10 dB
of dynamic range left. **The amp opened at the wall.** Per-voice defaults are not
a circuit change and are stated as such.

### H4 — the mid scoop / "tinny" — **REFUTED as a treble excess, REPORTED as a bass deficit**

Composed spectral tilt, clean level, tone knobs noon, dB re each amp's own 660 Hz:

| f | Rockerverb | OR120 | JCM800 |
| --- | --- | --- | --- |
| 82.41 Hz | **−14.79** | −23.33 | **−3.36** |
| 220 Hz | −3.09 | −11.13 | −2.66 |
| 2.2 kHz | +6.05 | +3.15 | +7.15 |
| 4.4 kHz | **+8.37** | +2.81 | **+10.15** |
| 6 kHz | +8.88 | +1.96 | +10.64 |

**The Rockerverb is LESS bright than the JCM800 at the top** (+8.37 vs +10.15 at
4.4 kHz) and **11.4 dB thinner at low E**. "Tinny" is a bass deficit, not a treble
excess. Preamp-only measurement puts all of it in the preamp (−14.82 dB at 82 Hz
at GAIN 0.3), i.e. the transcribed 1 n / 2n2 / 4n7 interstage cascade (398 / 134 /
49 Hz corners). **The netlist was re-verified against the source file this slice**
(see below): C1 really is 1 nF. Not touched; the BASS knob has +9.77 dB of
authority at 82 Hz to answer it (§63.7), and the default moves to use it.

### The netlist re-verification (H4's research question)

`dsharlet/LiveSPICE` was re-cloned and `Tests/Examples/Orange Rockerverb 50
Preamp.schx` re-parsed independently of §63's pass:

* **C1 = 1 nF — CONFIRMED.** The single largest audible consequence in the voice
  is transcribed correctly.
* **The MIDDLE pot is NOT a rheostat — CONFIRMED** by re-deriving the terminal
  geometry (potentiometer terminals are anode (−10,−20), cathode (−10,+20), wiper
  (+10,0); `Middle` sits at (470,100) with `Rotation="-10"` ≡ 180° and
  `Flip="true"`, which resolves its anode to the BASS pot's cathode, its cathode
  to the ground/output-reference node and its **wiper** to C26). §63.2's
  transcription stands. Consequence, newly measured and REPORTED: because
  C→GND is 25 k at every position rather than a canonical FMV's rheostat, the
  stack's mid-band insertion loss is **−13.2 dB at 1 kHz** where a canonical
  Marshall FMV at noon is ~−20 dB. That is ~7 dB of "too much gain" and it is the
  netlist's, not a fit.
* **Both GAIN pots carry `Wipe="0.6"`** in the file (every other pot is 0.5) —
  the file's own saved position, noted as weak evidence, not used.
* **Still open and unresolvable from here:** the file is a Rockerverb **50**
  preamp and this voice ships as a **100**. No source reachable from this
  container settles whether the two preamps are identical. Recorded, not guessed.

## Approach

1. **`RockerverbAmp` gets ONE `Oversampler`.** The preamp and the power section
   are prepared at the OVERSAMPLED rate with their own resamplers set to 1x, so
   the existing classes are reused unchanged and the change is confined to
   `RockerverbAmp.{h,cpp}`. `TriodeStage` is not touched.
2. **The same for `OrangeAmp`**, if and only if its own XFAIL then XPASSes.
3. **Per-voice amp knob defaults** (`AMP_KNOB_DEFAULTS` gains a per-type override
   map; native mirrors it). Not a circuit change.

Fidelity: (1) is a **deliberate tone change** confined to two voices — one
band-limiting instead of five changes the audio. (3) is a default, not a change to
any rendered signal at a given knob position.

## How this will be measured

* alias floor at 44.1 and 48 kHz × 1/2/4/8x, cranked and realistic
  (`clipper_rockerverb_tests`, `clipper_orange_tests`)
* `latencySamples()` before → after
* `clipper-bench` interleaved same-machine A/B (§35)
* `--golden-report`: ALL FIVE goldens must read ±0.00
* the XFAIL ledger: `rockerverb-alias-44k1` must XPASS → be deleted → be asserted
  hard

## Manual test steps

- [ ] Web: pick Rockerverb, confirm it opens at the new defaults and is not at the wall
- [ ] Web: the delivery-path spec still measures > 6 dB from param id 12
- [ ] Edge: `setOversampling(1)` still renders finite audio and reports latency 0

## Out of scope

* `TriodeStage` independent input/output coupling (§63.11) — refuted as the cause
  of "squashed"
* the Rockerverb clean channel, the attenuator, the ~93 W ceiling
* `RatModel` / `DiodeClipperADAA` / §66 / ADR 027 / the `rat_jcm800` golden
  (a parallel slice owns those)

---

## What actually happened

The full record is **docs §63.14** (amended into §63 in place, per the brief), plus
a §57.7 / §57.13 amendment for the OR120. Summary of the surprises:

1. **The shared oversampling domain needed no change to any shared class.** The
   trick is that `Oversampler`'s `factor() == 1` is a documented exact
   pass-through, so preparing `RockerverbPreamp` / `RockerverbPowerAmp` at
   `sampleRate * 4` with their own resamplers at 1× runs the *existing* code
   inside one outer domain. `TriodeStage` was never opened. That is also why all
   five goldens are unchanged by construction rather than by luck.
2. **The same change makes the OR120 WORSE** (−50.8 → −48.7 dB at 44.1 kHz,
   −73.0 → −67.8 at 48 kHz), so that half was reverted and §57.7's "it is the
   architecture speaking" was corrected. Mechanism in §63.14.2.
3. **H2 (grid blocking) was refuted before any code was written** — the amp
   reproduces 25.52 dB of a 26.0 dB input span at GAIN 0.30.
4. **H3 was confirmed as a symptom and refuted as a taper fix** — k = 4 is already
   inside §58's 10–20 % audio-taper spec and the useful-span geometry forbids any
   monotone taper from fixing it. The real defect there is the DEFAULTS, which are
   named as their own slice because `setAmpType` deliberately does not reset knobs
   and the change would touch the rig-JSON Playwright fixtures.
5. **H4 was refuted as a treble excess** — the amp is *less* bright than the JCM800
   at 4.4 kHz; "tinny" is 11.4 dB of missing low E from the transcribed
   1 n / 2n2 / 4n7 cascade. The netlist was re-cloned and re-parsed to be sure.

## Measured results

See §63.14 for the full tables. Headlines:

| | before | after |
| --- | --- | --- |
| composed cranked 4× alias, 44.1 kHz | −52.7 dB | **−72.3 dB** |
| composed cranked 4× alias, 48 kHz | −80.1 dB | **−84.9 dB** |
| realistic (G 0.50 / V 0.10) 4×, 44.1 kHz | −61.1 dB | **−115.1 dB** |
| realistic (G 0.50 / V 0.10) 4×, 48 kHz | −71.5 dB | **−96.2 dB** |
| latency @ 4× | 360 samples | **72 samples** |
| goldens | — | **all five ±0.00** |
| `clipper_rockerverb_tests` ledger | 1 entry | **none** |

## Files created / modified

* `core/include/clipper/dsp/RockerverbAmp.h`, `core/src/dsp/RockerverbAmp.cpp` —
  the shared oversampling domain
* `core/include/clipper/dsp/OrangeAmp.h` — banner only; the refutation is recorded
  there and `OrangeAmp.cpp` is byte-identical to `HEAD`
* `core/tests/test_rockerverb_amp.cpp` — XFAIL deleted, the −56 dB bar hard at both
  rates, a new absolute −80 dB realistic-setting bar, new `testOneOversamplingDomain`
* `core/tests/test_orange_amp.cpp` — the XFAIL's `fix` string re-owned to the cathodyne
* `core/CMakeLists.txt` — `clipper_add_xfail_ledger(clipper_rockerverb_tests)` removed
* `web/src/assistant/prompt.ts` — the GAIN geometry, measured
* `docs/DEVELOPMENT.md` — §63.14 (new), §63.8 / §63.11 / §57.7 / §57.13 amended
* `CLAUDE.md`, this plan file, the rebuilt WASM artifact

## Deferred to next session

* **Per-voice amp knob defaults** — the real fix for "too much gain" as *experienced*
  (§63.11, §63.14.4). Numbers to use are in §63.14's wiper table.
* **The OR120's `orange-schematic-alias-44k1`**, re-owned to the cathodyne's
  compliance clip. A shared domain would still buy it 216 → 72 samples of latency
  *if* the alias cost is answered first.
* **`TriodeStage` independent input/output coupling** — a fidelity item, not a
  field-report item.
* The Rockerverb 50-vs-100 preamp question (no reachable source).

## Status

- [x] Complete
