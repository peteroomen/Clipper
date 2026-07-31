# Orange OR120 — correcting the model against the REAL schematics (§57 amendment)

**Date:** 2026-07-31
**Branch:** claude/orange-schematic-correction-6f557i
**Roadmap item:** §57 follow-up. The OR120 shipped (PR #41) as an explicit
**documented reconstruction** — no schematic was reachable from the container
(`WebFetch` 403'd every host including `example.com`). §57.1 said in terms:
*"Do not re-tune any of them toward a sound; find the schematic."*

**The owner has now supplied the schematics.** This slice replaces the
reconstruction with the circuit.

## Sources (owner-supplied 2026-07-31, transcribed below — the images are NOT in the repo)

1. **`OR120 GRAPHIC MkII — Early 1970's`**, a 2004 redraw by B. Hickmott of the
   1972 factory schematics, in three sheets (Preamp / Output-amp / Power supply).
   **This is the primary source**: a complete, unambiguous netlist.
2. **`ORANGE GRAPHIC MkII` post-'74 factory sheet** with a full parts list
   (R1–R38, C1–C26) — used as the cross-check and for the later-era values.
3. **Orange Amp Field Guide, OR120 HEAD** — confirms *"Phase Inverter: Cathodyne
   type: 1/2 x 12ax7"*, fixed bias, solid-state rectifier, 4× EL34, 1.5× 12AX7
   preamp, and the panel layout **Input – F.A.C. – Bass – Treble – H.F.Boost –
   Gain – Reverb Send – Reverb Return**.

**Era decision: model the EARLY 1970s version** (source 1). It is the one with a
complete drawn netlist, it is the "picture graphics" amp §57 targeted, and it
carries the **1n5 treble cap** — which was the slice's ONE sourced component value
and is why owners say the early treble knob "brings high mids up with it". Source 2
is the post-'74 amp (330 pF treble) and is the cross-check, not the target.

## What the reconstruction got RIGHT (keep, do not disturb)

Confirmed by the schematics: the **cathodyne** phase inverter; **global NFB into
the driver's cathode**; the **James / passive-Baxandall** BASS+TREBLE stack with no
mid; **F.A.C. as a six-way series coupling cap**; **solid-state bridge**; **fixed
bias**; **no master volume**; 4× EL34 + 2× ECC83.

## THE NETLIST (transcribed from source 1 — this is the spec)

### Preamp sheet

```
2 input jacks -> 68k grid stopper each -> V1A grid;  1M grid leak to ground
V1A  (1/2 ECC83)   Rk 2k2 bypassed by 50uF ;  Ra 220k to D+
V1A plate -> 68n -> TONE STACK input

TONE STACK (James / passive Baxandall):
  from the 68n:  100k in series to the BASS pot top, and the treble branch
  BASS   1M LOG ; 2n2 across the upper section ; 22n across the lower section
                ; 22k from the bottom of the pot to ground
  100k between the BASS wiper and the TREBLE pot
  TREBLE 1M LOG ; 1n5 at the top ; 10n at the bottom to ground

GAIN  1M LOG        (the panel calls this GAIN — it is the volume control)
GAIN wiper -> 330p -> V1B grid
V1B  (1/2 ECC83)   Rk 2k2 bypassed by 50uF ;  Ra 220k to D+
V1B plate -> 68n -> F.A.C. rotary -> OUTPUT AMP

F.A.C.  6-way series coupling cap, in order:
        [straight through / no cap], 4n7, 4n7, 2n2, 1n, 330p
        (post-'74 list agrees on the five caps: C16 330p, C17 1000p,
         C18 2200p, C19 4700p, C20 4700p)
```

### Output-amp sheet

```
Effects loop: REVERB SEND / RETURN jacks with 100k resistors; PREAMP feed via 100k

DRIVER (1/2 ECC83)
  grid from the loop return (100k)
  Ra 100k to C+ , with 1n across it
  cathode node: 1K5 + 220k , PLUS the BOOST network , PLUS the NFB injection
  NFB:  F.B from the OT 16 ohm tap -> 15k -> driver cathode node
  BOOST ("H.F. Boost"): 1k LIN pot + a 2 mH CHOKE + 0.47uF to ground, with 100k

  driver plate -> 68n -> CATHODYNE grid  (1M grid leak)     <-- AC-COUPLED

CATHODYNE (1/2 ECC83)
  Ra 100k to C+   ;   Rk 100k        (EQUAL split loads)
  plate   -> 68n -> 2k4 -> EL34 grids (pair 1)
  cathode -> 68n -> 2k4 -> EL34 grids (pair 2)
  EL34 grid leaks 220K + 220K to N.B (negative bias)

4x EL34: 2k4 grid stoppers, 1k screen resistors (pin 4), cathodes grounded
OT taps 16 / 8 / 4 ohm ; A+ = centre tap
```

### Power-supply sheet

```
mains taps 240/220/200/115 V, 3A fuse
8x 1N4005 BRIDGE
 -> 1A HT fuse -> A+   (2x 100uF 450V in series, 100K 2W balancers)  [OT centre tap]
 A+ -> CHOKE -> B+     (2x 32uF 450V, 100K 2W balancers)             [EL34 screens]
 B+ -> 33K 2W -> C+    (16uF 450V)                                   [driver + cathodyne]
 C+ -> 33K 2W -> D+    (16uF 450V)                                   [V1A + V1B]
negative bias: 1N4005 half-wave, 2x 22uF 63V, 22K, 1k -> N.B
6.3 V filaments
```

**Note the supply ordering** — `A+` (the OT centre tap) is taken from the reservoir
**BEFORE** the choke; the screens (`B+`) come **after** it. This is the same
structural question docs §55 had to settle for the AC30, and here it is explicit.

### Post-'74 sheet — cross-check values (source 2)

`VC1/VC2/VC3 = 1M log (BASS/TREBLE/VOLUME)`, `VC4 = 1k lin (BOOST)`,
`VR1 100K preset (SET BIAS)`, `VR2 100R (SET MIN HUM)`, `SW2 = 2-pole 6-way FAC`,
`D1–9 = 1N4005`, `L1 = HT choke`, `R35–R38 = 1K 5W wirewound` (screens),
`R24–R27 = 2K2` (EL34 grid stoppers), `C25/C26 = 100uF 450V`,
`C13/C14 = 330pF` (the LATER treble cap — the early amp's is 1n5).
80 W version = identical but V3 & V6, R24, R27, R35, R38 omitted.

## The defects to fix

| # | Item | Model today | Schematic |
| --- | --- | --- | --- |
| 1 | **Signal order** | `V1A → VOLUME → V1B → F.A.C. → James stack` | **`V1A → James stack → GAIN → V1B → F.A.C. → power amp`** |
| 2 | V1A/V1B plate load | 100k | **220k** |
| 3 | V1A/V1B cathode | 820 Ω ∥ 25 µF | **2k2 ∥ 50 µF** |
| 4 | F.A.C. ladder | 47n·22n·10n·4n7·1n5·330p | **through·4n7·4n7·2n2·1n·330p** |
| 5 | Cathodyne split loads | 180k / 180k | **100k / 100k** |
| 6 | Driver plate load | 300k | **100k**, with 1n across it |
| 7 | Driver→cathodyne | **DC**-coupled (joint 3×3 Newton) | **AC**-coupled: 68n + 1M grid leak |
| 8 | H.F. Boost | "presence in the NFB loop" | 1k lin + **2 mH choke** + 0.47 µF at the driver cathode |
| 9 | Tone-stack values | reconstructed | as transcribed above |
| 10 | EL34 screen R | — | **1k** |

**#1 is the structural one.** The stack is not a terminal EQ: it sits between V1A
and the GAIN pot, so it is driven by V1A's 220k plate and its insertion loss is made
up by V1B. Turning GAIN up therefore drives V1B harder *with an already-EQ'd
signal*, and the F.A.C. feeds the **power amp**, not the stack. That is a response
difference, not just a frequency-response difference.

**#4 matters more than it looks:** §57 cited a forum's *"330 p to .047"* for the
F.A.C. span. The real ladder tops out at **4n7** — a **10× error** at the fat end —
plus a straight-through position.

**#7 is an era inconsistency, not simply a bug:** the early-70s amp is AC-coupled
here; the post-'74 parts list has exactly four 0.068 caps, all accounted for
elsewhere, which implies the LATER amp is DC-coupled. The model is currently an
early-70s amp wearing post-'74 coupling. Modelling the early amp means **splitting
the joint 3×3 Newton** into an AC-coupled driver + cathodyne.

## Approach

Deliberate fidelity correction. Replace reconstructed constants with the transcribed
ones and re-order the preamp. **Do not re-tune anything toward a sound** — if a
measurement moves, report it.

The §57 acceptance bar (**mid-forward vs the JCM800**) must SURVIVE, because it is
topological: a James stack has no mid notch and an FMV does. Expect the *numbers*
to move (the stack is now driven from a 220k plate and loaded by the GAIN pot).
**If the bar fails, that is a finding to report, not a bar to loosen.**

## Steps

- [ ] Read `docs/DEVELOPMENT.md` §57 in full, plus §19/§22 (JCM800 machinery),
      §46 (no-master-volume breakup onset), §42 (PI), §55 (supply)
- [ ] Re-order `OrangePreamp`: V1A → stack → GAIN → V1B → F.A.C. → out
- [ ] Replace every value in the table above with the schematic's
- [ ] Split the driver/cathodyne into the AC-coupled pair (68n + 1M grid leak);
      keep the cathodyne's equal 100k/100k split loads
- [ ] Model the H.F. Boost as the real 1k lin + 2 mH choke + 0.47 µF network at the
      driver cathode (an INDUCTOR — the model currently has none here)
- [ ] Re-check the supply topology against the transcription (A+ before the choke,
      screens after, 33K droppers to C+ and D+)
- [ ] Re-derive `kInterstageScale` / `kFullScaleSecV` by the §42 rated-power
      criterion on the CORRECTED circuit — do not carry the old values over
- [ ] Re-run every §57 bar; update the tables in §57 **in place**, marked as
      measured against the real schematic
- [ ] Perturbation-prove the bars again (`touch` after BOTH patch and restore)
- [ ] `--golden-report`: **all five UNCHANGED** — the Orange is in no golden rig, so
      any movement is a SCOPE FAILURE to report, not a bless candidate
- [ ] WASM rebuild + artifacts; full core ctest; native build + tests; web build +
      Playwright; node suites
- [ ] §57 amendment: rewrite §57.1 so the sourced/reconstructed table becomes a
      **transcribed parts list citing the schematics**, and record the era choice
- [ ] CLAUDE.md Current State entry; fill this plan's bottom sections
- [ ] ONE commit on this branch, `fix: …`, tables in the body; NO push, NO PR

## How this will be measured

The §57 bar (mid-forward contrast vs the JCM800, tone-network and composed);
breakup onset vs VOLUME/GAIN; the F.A.C. span across the corrected ladder;
cathodyne leg balance (must stay ~1.000 — it is topological); DC operating points
against the real supply rails; alias floor; DC on signal; five goldens unchanged.

## Manual test steps

- [ ] Owner: A/B the Orange against the JCM800 at matched level — still obviously a
      different, mid-forward amp
- [ ] F.A.C. through→330p thins and quietens monotonically across all six
- [ ] H.F. Boost sweeps HF without changing the midrange
- [ ] Edge: GAIN 1.0 finite/bounded, reset clean, 44.1/96 k

## Out of scope

The post-'74 variant as a second voice, the Rockerverb (M10.7), every other amp and
pedal, the cab (unchanged), any golden re-bless.
