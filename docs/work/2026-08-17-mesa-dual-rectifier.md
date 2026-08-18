# Mesa/Boogie Dual Rectifier Solo Head — the SEVENTH amp voice (M10.4)

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

---

## What actually happened

**The research channel inverted the whole slice.** The plan was written expecting a
§57-style documented reconstruction, because all three normally-available channels
were checked first and all three are dead in this container: `WebFetch` is
`EGRESS_BLOCKED` (verified on `warpedmusician.wordpress.com` and
`www.prowessamplifiers.com`), GitHub code search is repository-scoped this session,
and `dsharlet/LiveSPICE` was cloned and confirmed to carry no Mesa example. **The
owner then supplied `recto.zip`** — the full `mbdr` factory drawing set — which
turned the slice into a transcription AND handed it something no previous amp slice
has had: the manufacturer's own marked DC node voltages, i.e. an absolute external
reference.

**The plan's acceptance bar (b) was set at ≥3× before anything was measured; it
lands at 2.24×.** Shipped at a 2.0× bar with the measurement recorded, per the
house rule that margins are recorded rather than snugged.

**The plan's step list assumed a `MesaPowerAmp` reconstruction.** It is transcribed
too — sheets `mbdr2` and `mbdr4` carry the LTP's (unequal) plate loads, the tail,
the couplings, the grid leaks and stoppers, the screens, the bias, both feedback
resistors, the presence network, the rectifier select and the whole rail ladder.
Only the device cards, the OT and `kFullScaleSecV` are reconstruction.

**Two things went wrong and were fixed rather than bounded**, and one trap bit that
this repo has already recorded twice:

1. `reset()` did not reproduce a fresh model (**4.699e-01**). Bisected to
   `prepare()`, not `reset()` — see §68.9. Now **0.000e+00**.
2. `kRaa` was set to 3800 Ω, a *pair's* reflected load, where the house value for
   the same 6L6 quad is 2000 Ω. Found by measuring output power.
3. `build-wasm.sh`'s emcc source list is EXPLICIT, and the first artifact rebuild
   failed at `wasm-ld` with undefined `MesaAmp` symbols — exactly as §60 and §64
   record for the delay and the opto.

**§ number:** CLAUDE.md says section numbers are assigned centrally. §68 was taken
as the next free number (§67 is the Uni-Vibe, the highest in `DEVELOPMENT.md`). If
that collides with a parallel slice, this one renumbers.

## Measured results

| property | result |
| --- | --- |
| V2B plate vs sheet | 384.04 V vs 384 (**0.01 %**) |
| V1A / V2A / V3A plate | 1.69 % / 1.25 % / 3.98 % |
| V3B cathode | 222.42 V vs 216 (2.97 %) |
| idle rail (silicon) | 460.66 V vs marked A = 460 (**0.14 %**) |
| loop depth per mode | OR CLN 9.27 · OR NORM 6.19 · RED VINT 6.19 · **OR MOD 0.00 · RED MOD 0.00 dB** |
| rectifier droop | silicon 25.34 V · 5U4 56.66 V = **2.24×** (bar 2.0×) |
| spongy (silicon) | 65.13 V |
| input-select tilt | RED +22.23 dB vs ORANGE −1.07 dB = **23.29 dB** |
| latency | **72 samples / 1.50 ms** (one shared OS domain) |
| ragged blocks | **0.000e+00** |
| `reset()` vs fresh | **0.000e+00** (was 4.699e-01) |
| NaN recovery | 0 / 5120 non-finite after `reset()` |
| DC on signal | 0.92 … 1.60 % of peak across all five modes |
| resting state | plateaus 2.40e-08 / 1.04e-08; **0 subnormal output samples** |
| cranked peak | 0.6759 … 0.9013 (§23's normalization convention) |
| quad idle | 30.34 mA / 13.97 W = 47 % of a 6L6GC's 30 W (a cool bias) |
| output power | **~48 W against a rated 100 — open, attributed, NOT fitted** |
| goldens | **all five UNCHANGED** |
| WASM artifact | rebuilt, 103 hashed inputs |

**The screen-resistor perturbation, which REFUTED the obvious attribution:**

| kRscreen | max power |
| --- | --- |
| 1000 Ω (shipped) | 47.99 W |
| 470 Ω | 52.26 W |
| 100 Ω | 55.55 W |
| 1 Ω | 56.46 W |

## Files created / modified

Core: `MesaPreamp.{h,cpp}`, `MesaPowerAmp.{h,cpp}`, `MesaAmp.{h,cpp}`,
`tests/test_mesa_amp.cpp`, `CMakeLists.txt`, `src/clipper_c_api.cpp`.
Web: `params.ts`, `rig.ts`, `App.tsx`, `components/Amp.tsx`, `styles/tokens.css`,
`styles/amp.css`, `assistant/tools.ts`, `assistant/prompt.ts`.
Native: `ClipperEngine.{h,cpp}`.
Build/docs: `scripts/build-wasm.sh`, `web/public/generated/*` (artifact + stamp),
`docs/DEVELOPMENT.md` (§68), `ROADMAP.md`, `CLAUDE.md`.

## Deferred to next session

1. **THE DROP PEDAL** — the second half of the owner's ask, deliberately not
   bundled. New DSP family; `FFT.h` and `DelayLine.h` are the starting points, and
   the acceptance shape is cents accuracy / latency / artifact floor rather than a
   measured contrast against a sibling.
2. A **Mesa oversized 4×12 IR** (the voice reuses `brit412`).
3. The **effects loop** as a feature; the **EL34 bias option**.
4. The **power shortfall** (§68.7).
5. A **series-R field on `TriodeStage`'s cathode network** — the same shared-class
   change §63.3 already names for the coupling caps.
6. **Native build not verified in this container** — no JUCE tree exists here and
   fetching it was out of budget. The edits follow the Rockerverb's pattern exactly
   and the core they call is green, but `identical_core_test` has NOT been run
   against this voice, and no `identical_core_test` case was added for it. That is
   the honest gap; CI's native job will exercise the build.
7. ~~The Playwright suite~~ — **RESOLVED in-slice.** The first full run had 5 real
   failures, all of them the LITERAL rig round-trip fixtures: adding three fields
   to `AmpParams` changes the serialized shape, exactly the trap §57 recorded for
   the OR120's `fac`. Fixtures updated; re-run is **52 passed, 1 flaky**
   (`amp.spec.ts:120`, unrelated, passes on retry, and matches
   `playwright.config.ts`'s own documented Chromium flake). Node suites and
   electron (20/20) green.

## Status

- [ ] In progress
- [ ] Complete
- [x] Partial — core (37/37), C ABI, web (52 passed), node, electron, docs and the
      WASM artifact all done and green. The ONE unverified piece is the NATIVE
      build: no JUCE tree exists in this container and fetching it was out of
      budget, so `identical_core_test` has not been run against this voice and no
      case was added for it (see Deferred 6).
