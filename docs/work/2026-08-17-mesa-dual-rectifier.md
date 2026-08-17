# Mesa/Boogie Dual Rectifier Solo Head — the SIXTH amp voice (M10.4)

**Date:** 2026-08-17
**Branch:** claude/mesa-drop-pedal-setup-9lxs5g
**Roadmap item:** M10.4 — Mesa Dual Rectifier ("the ask is grunge and 90s metal")

## Goal

Ship the Mesa Dual Rectifier Solo Head as amp voice **6** (`mesa`), wired end to end
(core, C ABI, worklet, web, native), **transcribed from a real schematic** rather than
reconstructed — the owner supplied the `mbdr` sheet set, so §57's "find the schematic"
rule is satisfied for the first time on an amp voice since §63's Rockerverb.

Second half of the owner's ask (a DigiTech Drop-style polyphonic pitch shifter) is a
SEPARATE slice — see "Out of scope".

## The research channel — this is the good case, not the §57 case

The owner supplied `recto.zip` via Drive: **11 GIF sheets** of the
`MESA/BOOGIE DUAL RECTIFIER SOLO HEAD` drawing set (`mbdr1`…`mbdr8`, plus `mbdr1mod`
and `mbdr3'` variants). Every sheet is legible at 2× LANCZOS upscale and carries
component values AND marked DC node voltages.

This matters because the two channels available in-container are both dead:

- `WebFetch` is **EGRESS_BLOCKED** — verified on `warpedmusician.wordpress.com` and
  `www.prowessamplifiers.com`. Same as §57/§59/§60/§63.
- GitHub **code search is repository-scoped** this session (the §63 trick is out), and
  `dsharlet/LiveSPICE` was cloned and checked: **no Mesa example** (its "Rectifier"
  files are literal diode-bridge test circuits).

So without the owner's zip this would have been another §57-style documented
reconstruction. It is not. **It is a transcription.**

### Sheet inventory

| Sheet | Contents | Used |
| --- | --- | --- |
| `mbdr1` | PREAMP — V1A…V3B, both gain pots, both tone stacks, presence | ✅ transcribed |
| `mbdr2` | POWER AMP — 12AX7 LTP, 4×6L6, OT, NFB/presence, output taps | ✅ transcribed |
| `mbdr3` | EFFECTS LOOP — V4B cathode follower, V4A recovery | ⚠️ bypass path only |
| `mbdr4` | POWER SUPPLY — rect select, spongy/bold, rail ladder, bias | ✅ transcribed |
| `mbdr5` | SWITCHING MATRIX pt1 — RY1/RY2, RED/ORANGE busses | ✅ (semantics) |
| `mbdr6` | SWITCHING MATRIX pt2 — LDR banks, bias select, loop select | ✅ (semantics) |
| `mbdr7` | **TRUTH TABLE** — every LDR × every mode | ✅ transcribed verbatim |
| `Mbdr8`, `mbdr8'''`, `mbdr1mod`, `mbdr3'` | wide sheet + variants | not yet read |

## Transcribed netlist

### Supply rail ladder (`mbdr4`)

Marked node voltages, straight off the sheet:

```
rect ──220u+220u (series, 150k/150k balancing)── Stdby ── A 460V
A ──choke── B 454V ──2k7── C 422V ──15k── D 406V ──22k── E 402V
4 × 30u decoupling on B, C, D, E; 47n across A
```

Preamp stages tap this ladder exactly as the preamp sheet's node letters say:
**V1A ← E**, **V2A ← D**, **V2B ← D**, **V3A ← C**, **V3B ← C**.

### The rectifier select — the signature control (`mbdr4`)

Two independent switches, and **they are commonly conflated**:

1. **`rect select`** — `4 × 1N4007` silicon bridge **vs** `2 × 5U4` valve rectifiers.
   The two paths tap **different HT winding taps** (the silicon leg takes the
   350 V + 50 V (`blu`) tap; the 5U4 leg takes the plain 350 V), so silicon is both
   *stiffer* AND *higher B+*. A dedicated 5 V winding heats the 5U4s.
2. **`SPONGY` / `BOLD`** — a **mains-primary-side** switch inserting series impedance
   before the power transformer. Nothing to do with the rectifier valves.

This is the reason the roadmap's cost estimate dropped after §55: `Ac30PowerAmp`
already models a real Thévenin HT source + reservoir droop, so both switches land as
`kRsupply` / `kVsupply` changes on machinery that exists.

### Preamp (`mbdr1`) — five triodes to the tone stack

```
In ──1M grid leak── V1A grid
V1A: Ra 220k from E, Vp 200V, Rk 1k8 (Vk 1.6V)
     bypass = 1u in series with (47k ∥ LDR3b)   ← LDR3b SHORTS the 47k when ON
V1A plate ──20n── node X1
X1 ──2M2── gnd                    (load)
X1 ──(680k ∥ 2n)── LDR4 ── gnd    ("V1A OUTPUT PAD")
X1 ──82p──  LDR1 ──┐              ("RD input sel")
X1 ──2M2──  LDR2 ──┤              ("OR input select")
   RED path  → 1n → 1M RD GAIN POT → wiper → LDR5 ─┐
   ORANGE path→ 1n → 1M OR GAIN POT → wiper → LDR6 ─┴─ 470k ── V2A grid (20p to gnd)

V2A: Ra 100k from D, Vp 280V, Rk 1k8 (Vk 2V), bypass 1u + (47k ∥ LDR3a)
V2A plate ──20n──►──470k(series)── V2B grid, 1M grid leak to gnd
V2B: Ra 100k from D, Vp 384V, Rk 39k UNBYPASSED (Vk 5V)   ← degenerated, high headroom
V2B plate ──20n── node ──330k── gnd, ──220k(stopper)── V3A grid
V3A: Ra 220k from C, Vp 213V, Rk 1k8 (Vk 1.6V), bypass 1u + (47k ∥ LDR10)
V3A plate → X → tone stacks
V3B: Ra 100k from C, Vp 415V/216V — post-stack recovery
```

Note the **cathode-bypass idiom**: every bypass cap is `1u` in series with a `47k`
that an LDR **shorts out**. Bypass ON = full `1u` across `1k8`; bypass OFF = `1u + 47k`,
which is negligible bypass. So a "cathode cap" LDR is really a gain switch.

### Both tone stacks (`mbdr1`) — FMV, differing in ONE cap

Both are standard FMV, driven through their own LDR, joined by a 1M bleeder:

| Element | RED | ORANGE |
| --- | --- | --- |
| Treble cap | **680p** | **500p** |
| Treble pot | 220k | 220k |
| Slope resistor | 47k | 47k |
| Bass cap / pot | 20n / 1M | 20n / 1M |
| Mid cap / pot | 20n / 25k | 20n / 25k |

ORANGE additionally carries `82p`, `22k`, `3n`, `82k`, and a `100k` + **LDR7 treble
roll-off** leg; PRESENCE is a 25k pot with 22k/3n.

### Power amp (`mbdr2`)

```
V5 12AX7 LTP: Ra(V5B) 82k, Ra(V5A) 90k   ← DELIBERATELY UNEQUAL (the house Ra2 story)
              120p across each plate load, 1M grid leaks, 470R + 10k, 4k7 tail
V5 plates ──47n── 220k grid leaks ──1k5 grid stoppers── 4 × 6L6 (V6..V9)
Screens 1k/2W each. Bias F = −51V (6L6) / −39V (EL34), "Bias select" 1/2 DPDT
OT: taps 8–16 Ω and 4 Ω; slave out via 6k8 + 10k pot
NFB from secondary: 100n, 47k ∥ 47k with LDR19 ("feedback") / LDR20 ("more feedback")
ORANGE PRESENCE 25k + 100n in the loop
```

### The truth table (`mbdr7`) — verbatim

| LDR | Device | OR NORM | OR CLN | OR MOD | RED NORM | RED VINT |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | INPUT SELECT | OFF | OFF | OFF | ON | ON |
| 2 | INPUT SELECT | ON | ON | ON | OFF | OFF |
| 3a | V2A CATHODE CAP | ON | OFF | ON | ON | ON |
| 3b | V1A CATHODE CAP | ON | OFF | ON | ON | ON |
| 4 | V1A OUTPUT PAD | ON | OFF | ON | ON | ON |
| 5 | RD GAIN POT | OFF | OFF | OFF | ON | ON |
| 6 | OR GAIN POT | ON | ON | ON | OFF | OFF |
| 7 | TREBLE ROLL OFF | ON | ON | **OFF** | **OFF** | ON |
| 8 | RD TONE CAPS | OFF | OFF | OFF | ON | ON |
| 9 | OR TONE CAPS | ON | ON | ON | OFF | OFF |
| 10 | V3 CATHODE CAP | ON | OFF | ON | ON | ON |
| 14 | RD MASTER POT | OFF | OFF | OFF | ON | ON |
| 15 | OR MASTER POT | ON | ON | ON | OFF | OFF |
| 16 | OR MASTER BYPASS | OFF | ON | OFF | OFF | OFF |
| 19 | FEEDBACK | ON | ON | **OFF** | **OFF** | ON |
| 20 | MORE FEEDBACK | OFF | **ON** | OFF | OFF | OFF |

(11/12/13 are the FX loop send/return/bypass and follow the selected mode.)

## The acceptance bar

Two independent halves, both hard asserts, both **structural** rather than EQ curves —
and both are read straight off the truth table, so neither is fitted:

**(a) MODERN RUNS OPEN-LOOP.** `OR MOD` and `RED NORM` switch LDR19 *and* LDR20 off:
the amp has **no global negative feedback at all** in its two modern modes. Every other
amp in this lineup has a permanently-wired loop (JCM 6.37 dB, OR120 7.81 dB,
Rockerverb 6.65 dB). The bar: measured loop depth must be **> 4 dB in RED VINT and
≈ 0 dB in RED NORM**, and the same power stage must therefore show materially more
compression / lower damping in the modern modes. This is the "loose, saturated Recto
low end" as a topology, not a voicing.

**(b) THE RECTIFIER SELECT MOVES SAG, NOT LEVEL ALONE.** Silicon vs 5U4 changes both
the source impedance and the tap, so the bar is a *ratio*: the rail droop under a
sustained cranked chord must be **≥ 3× larger** on the tube setting than on silicon,
while the clean-signal small-signal gain differs by less than the droop does.

Third contrast, **reported not asserted** (it is an inference): the unequal LTP plate
loads (82k/90k) predict the leg balance without a resistor sweep, unlike §45's JCM
(which needed one) — worth measuring against the OR120's topological cathodyne.

## Steps

- [ ] Transcribe the remaining sheets (`Mbdr8`, `mbdr1mod`, `mbdr3'`) to confirm no
      revision conflicts with `mbdr1`
- [ ] `MesaPreamp.{h,cpp}` — V1A…V3B cascade, both gain pots, both FMV stacks as MNAs,
      the LDR truth table as a mode enum
- [ ] `MesaPowerAmp.{h,cpp}` — reuse `Tube6L6Params`/`to6L6()` from `TwinPowerAmp.h`
      and `LtpInverter` with §42's `tailRef`; unequal plate loads; switchable NFB
- [ ] Supply: Thévenin source + reservoir on the §55 `Ac30PowerAmp` pattern, with
      `rect select` and `spongy/bold` as constants, not knobs-into-a-fit
- [ ] `MesaAmp.{h,cpp}` — compose; ONE shared oversampling domain (§63.14's lesson,
      NOT per-triode — that is what opened the two alias XFAILs)
- [ ] C ABI voice 6 + `mesa_*` params; worklet; web face/params/assistant; native
      engine + APVTS + editor
- [ ] `core/tests/test_mesa_amp.cpp` + a case in `identical_core_test`'s plugin driver
- [ ] `bash scripts/build-wasm.sh`, commit the artifact

## How this will be measured

- **Loop depth per mode** (the bar (a) number), measured by the shared LTP probe
- **Rail droop ratio** silicon vs 5U4 under a cranked sustained chord (bar (b))
- Mid-notch metric on §57.4's scale-free scale vs JCM/Rockerverb — reported, since
  all three are FMV-lineage and this one is NOT claimed to be different there
- Preamp DC operating points vs the sheet's own marked voltages (200/280/384/213 V) —
  an **absolute** reference, the strongest kind available (§57.10's lesson)
- Alias floor at 4× at 44.1 and 48 kHz, with the ≥12 dB 1×→4× clause
- `--golden-report`: all five goldens must be UNCHANGED (a new voice cannot need a bless)
- CPU (× realtime, % of one 48 kHz stream), latency, DC-on-signal, ragged blocks,
  `reset()`, NaN recovery, rate spread

## Manual test steps

- [ ] Web: add Mesa, sweep all five modes, confirm RED NORM is looser/darker-bottomed
      than RED VINT at the same knobs
- [ ] Flip rect select on a held chord — expect bloom/droop on tube, tighter on silicon
- [ ] Edge case: MODE + rectifier switched mid-render must not click (declick bracket)
- [ ] Edge case: NaN into the engine, then `reset()` → 0 non-finite samples

## Out of scope for this session

- **The DigiTech Drop-style pitch shifter** — its own slice, its own plan file. It is
  a new DSP family (no circuit, no schematic, so a completely different acceptance
  shape: cents accuracy, latency, artifact floor) and bundling it would break the
  one-slice rule.
- The **effects loop** as a user-visible feature (V4A/V4B are modelled only as the
  bypass path).
- The EL34 bias-select option (the sheet supports it; we ship 6L6).
- A new cab — reuse `brit412` / `orange412` unless measurement says otherwise.

---

<!-- Fill in below during/after the session -->

## What actually happened

(in progress)

## Measured results

(in progress)

## Files created / modified

(in progress)

## Deferred to next session

(in progress)

## Status

- [x] In progress
- [ ] Complete
- [ ] Partial — see deferred
