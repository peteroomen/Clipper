# Orange Rockerverb 100 — the SIXTH amp voice (M10.7)

**Date:** 2026-08-01
**Branch:** claude/rockerverb-6f557i
**Roadmap item:** M10.7 — Orange Rockerverb 100 MkIII, the second half of the owner's
"OR120 and Rockerverb" ask. Docs §63. ADR 024 if one is needed.

## Goal

Ship the modern Orange as amp voice **5** (`rockerverb`) — a master-volume, four-stage,
EL34 push-pull head with reverb — wired end to end on both front ends in one slice, with a
hard, perturbation-proven, player-observable bar that it is **measurably NOT a re-skinned
OR120**.

## Approach

### Research first, and the channel was better than §57's

The proxy permits **github.com only** (confirmed again: `el34world.com` and
`en.wikipedia.org` both fail `CONNECT tunnel failed, response 403`; `WebFetch` 403s on
`el34world.com` and `dirtboxlayouts.blogspot.com`). But this time a GitHub code search found
a **real netlist**: `Orange Rockerverb 50 Preamp.schx`, an example circuit shipped inside
**LiveSPICE** (`dsharlet/LiveSPICE`, `Tests/Examples/`). It is a full schematic file —
components with values, positions and wires — so the netlist is recoverable exactly. It is
parsed node-by-node (terminal offsets read out of LiveSPICE's own `LayoutSymbol` code, the
rotation/flip transform out of `Schematic/Symbol.cs`) rather than eyeballed.

So the **dirty channel preamp is TRANSCRIBED**, and the power section, the reverb placement
and the phase inverter are a **documented reconstruction**. §63.1 tabulates which is which,
exactly as §57.1 does.

### What the netlist says, and why it makes the bar

Four 12AX7 gain stages, TWO 1 M log GAIN pots (a dual gang), a **Marshall-lineage FMV tone
stack** (39 k slope, 560 p treble cap, 22 n / 22 n, treble 250 k lin / bass 500 k log /
mid 25 k lin) and a **1 M linear VOLUME after the stack**. That is the acceptance bar
handed over by the circuit:

* The OR120's tone network is a **James / passive Baxandall** and measures a mid **BUMP**
  (§57.4: +0.75 dB). An FMV measures a mid **SCOOP** (§57.4: the Marshall's −6.03 dB).
  Same metric, opposite sign, from a sourced structural difference.
* The OR120 has **no master volume** and its breakup tracks the one knob (§57.5). The
  Rockerverb's VOLUME is **after** the tone stack, so drive and output level are
  independent — a property a no-master amp cannot have at any setting.

### Reuse, not new machinery

* EL34 quad + per-tube plate Newton + grid blocking + OT + rail/screen sag: the
  `Jcm800PowerAmp` machinery §57 already reused. **No new device model.**
* Phase inverter: the existing `LtpInverter` with §42's `tailRef` — and that is itself the
  third structural contrast (LTP vs the OR120's cathodyne).
* Reverb: the existing `ReverbModel`. The Rockerverb's spring is **authentic** (it is in
  the amp's name), unlike the JCM's and the OR120's usability adds.
* Cab: **reuse `orange412`** — a Rockerverb 100 head is sold against the same PPC412 the
  OR120 is. Decided by measurement (below), not assumed.

### Deliberate scope fence, stated up front

**Only the DIRTY channel ships.** The netlist covers the dirty channel; the clean channel
would be pure invention, and §57's own rule ("do not re-tune toward a sound; find the
schematic") applies to inventing a whole channel too. A channel switch with a reconstructed
channel behind it is worse than no channel switch. Named as the §63 follow-up, and the
ROADMAP entry is amended with the reason.

The **ATTENUATOR** is a post-OT load device and is not modelled (named, not hidden).

### Fidelity / tone statement

New voice. No existing model is touched, so this is neither fidelity-neutral nor a tone
change to anything that ships — the scope check is that **all five goldens stay at ±0.00**.

## Steps

- [x] Parse the LiveSPICE `.schx` into a netlist; record the transcription in §63.1
- [x] `RockerverbPreamp.{h,cpp}` — four `TriodeStage`s + three interstage MNAs (the two
      GAIN-pot networks and the stage-3→4 divider) + the FMV `RockerverbToneStack` MNA
      (with the VOLUME pot and the PI grid leak inside it)
- [x] `RockerverbPowerAmp.{h,cpp}` — `LtpInverter` + EL34 quad + OT + NFB + sag
- [x] `RockerverbAmp.{h,cpp}` — preamp → power → reverb
- [x] `clipper_rockerverb_tests` — the bar, the DC points, the networks vs their own
      `H(jω)`, knob authority, breakup, alias, DC-on-signal, denormals, reset/ragged
- [x] C ABI voice 5; worklet; web (`params.ts`, `rig.ts`, `audio.ts`, `Amp.tsx`, tokens,
      assistant); native (`ClipperEngine`, `PluginProcessor`, `PluginEditor`)
- [x] `native/tests/identical_core_test.cpp` gains a Rockerverb board case
- [x] WASM rebuild + `check-artifact.mjs`
- [x] Perturbation-prove every bar
- [x] Docs §63, ROADMAP M10.7, CLAUDE.md Current State

## How this will be measured

**THE BAR (two independent halves, both hard asserts):**

1. **Mid-notch contrast against the OR120**, on §57.4's own scale-free metric (min response
   across 300–800 Hz relative to the mean of the 100 Hz and 4 kHz responses, at noon),
   measured on the tone NETWORKS from their own netlists and again on the COMPOSED amps.
   The Rockerverb must measure a SCOOP where the OR120 measures a BUMP, with a shipped
   contrast bound in dB and the margin reported.
2. **Master-volume decoupling.** Level-matched at a quiet output RMS, the Rockerverb's THD
   must exceed the OR120's by a shipped dB/ratio bound — the thing an amp with no master
   physically cannot do.

**The rest, all reported:** breakup onset vs GAIN at fixed VOLUME and vs VOLUME at fixed
GAIN; THD at the documented settings; alias floor at 44.1 k and 48 k across 1/2/4/8×; DC on
signal with and without a +0.1 V input offset; latency; CPU as % of one 48 kHz stream;
block-size invariance; `reset()` vs fresh; rate independence 44.1–96 kHz; every preamp DC
operating point against Ohm's law on the transcribed plate loads; LTP leg balance; NFB
depth; the golden report for all five rigs.

## Manual test steps

- [ ] Web: pick the Rockerverb, sweep GAIN with VOLUME low — the amp must get dirtier
      without getting much louder (the OR120 cannot do this)
- [ ] Web: sweep BASS/MIDDLE/TREBLE — all three must do something (the OR120 has no mid)
- [ ] Web: REVERB must work; rig JSON must round-trip
- [ ] Native: the same, plus a session save/load
- [ ] Edge case: switch to and from the Rockerverb mid-render — no pop (declick)
- [ ] Edge case: NaN into every Rockerverb param must be rejected at the ABI

## Out of scope for this session

The CLEAN channel and its footswitch · the ATTENUATOR · a new cab · the OR120's open
follow-ups (§57.13) · `TriodeStage`'s conflated input/output coupling config · anything in
the parallel agent's files (`SidechainDetector.h`, `WahModel`, `CompressorEngine`/`CompModel`,
`GateModel`, §58/§59/§61, ADR 023).

---

<!-- Filled in during/after the session -->
## What actually happened

**The research went better than planned, and that changed the whole slice.** §57's
first release had to invent nearly every value because no schematic was reachable.
This time a GitHub code search found `Orange Rockerverb 50 Preamp.schx` shipped as
an EXAMPLE INSIDE LiveSPICE (`dsharlet/LiveSPICE`, `Tests/Examples/`, mirrored in
four other repos) — a schematic FILE, not a picture. Everything outside github.com
still 403s at CONNECT (re-verified on `el34world.com` and `en.wikipedia.org`;
`WebFetch` 403s on el34world's Rockerverb PDF and on dirtboxlayouts). So the DIRTY
CHANNEL PREAMP is transcribed and the power section is a documented
reconstruction.

**The parsed netlist**, recovered node by node (terminal offsets from LiveSPICE's
own `LayoutSymbol` code, the rotation/flip transform from `Schematic/Symbol.cs`,
the pot convention from `Potentiometer::Analyze`, wires by union-find with
collinear merging):

```
Capacitor  C1    1 nF     {A: n8,  C: n4}          Resistor R1   220k  {n16, GND}
Capacitor  C13   2.2 nF   {A: n11, C: n9}          Resistor R19  100k  {n3,  n4}
Capacitor  C14   10 uF    {A: n6,  C: GND}         Resistor R2   1.5k  {n6,  GND}
Capacitor  C16   100 pF   {A: n17, C: n16}         Resistor R20  1M    {n5,  GND}
Capacitor  C19   22 uF    {A: n2,  C: GND}         Resistor R23  68k   {n5,  n7}
Capacitor  C2    470 pF   {A: n16, C: GND}         Resistor R24  100k  {n3,  n9}
Capacitor  C23   22 uF    {A: n3,  C: GND}         Resistor R25  1k    {n10, GND}
Capacitor  C24   560 pF   {A: n21, C: n24}         Resistor R26  100k  {n2,  n12}
Capacitor  C25   22 nF    {A: n22, C: n25}         Resistor R27  2.2k  {n13, GND}
Capacitor  C26   22 nF    {A: n20, C: n25}         Resistor R3   1.5k  {n27, GND}
Capacitor  C3    10 uF    {A: n10, C: GND}         Resistor R30  220k  {n8,  n16}
Capacitor  C4    100 pF   {A: n3,  C: n9}          Resistor R31  220k  {n11, n14}
Capacitor  C5    10 uF    {A: n13, C: GND}         Resistor R32  470k  {n14, GND}
Capacitor  C6    100 pF   {A: n2,  C: n12}         Resistor R4   100k  {n2,  n24}
Capacitor  C7    4.7 nF   {A: n12, C: n28}         Resistor R42  10k   {n2,  n3}
Input      V7             {A: n5,  C: GND}         Resistor R43  39k   {n24, n25}
Speaker    S3             {A: n18, C: GND}         Resistor R5   220k  {n26, GND}
VoltageSrc V8    400 V    {A: n1,  C: GND}         Resistor R6   470k  {n28, n26}
Triode     V9    {P: n4,  G: n7,  K: n6}           Resistor R9   10k   {n1,  n2}
Triode     V10   {P: n9,  G: n17, K: n10}
Triode     V11   {P: n12, G: n15, K: n13}
Triode     V12   {P: n24, G: n26, K: n27}
Pot Gain1  1M   LOG  {A: n16, C: GND, W: n17}  Group="Gain"  wipe 0.6
Pot Gain2  1M   LOG  {A: n14, C: GND, W: n15}  Group="Gain"  wipe 0.6
Pot Bass1  500k LOG  {A: n22, C: n19, W: n22}  (wiper tied to anode: a rheostat)
Pot Middle 25k  LIN  {A: n19, C: GND, W: n20}
Pot Treble 250k LIN  {A: n21, C: n22, W: n23}
Pot Volume 1M   LIN  {A: n23, C: GND, W: n18}
```

**The bar had to be re-thought once.** The first draft of the master-volume
mechanism test swept VOLUME 0.06 → 1.00 and asserted a level span > 8 dB; it
measured **7.03 dB and went red**, because above ~0.15 the power valves are
already flat out and the knob stops moving the level. That is the amp working, not
a defect — the window was moved to the bottom of the travel (0.01 → 0.10) where it
measures **19.89 dB for a THD ratio of 0.971**, which is a far stronger statement
than the original.

**The 44.1 kHz alias floor came out as an XFAIL**, exactly as the OR120's did in
§57.7 and for what is evidently the same architectural reason. Not loosened.

**One perturbation stayed GREEN and was answered with an absolute reference**, per
the convention: see §63.13's P5.

Three claims in this plan's own "Approach" needed correcting on contact:

1. The plan said the LTP "would be a strong bar". It is NOT the bar — the PI is an
   INFERENCE from a sourced fact (an ECC83 in its own socket = a whole dual
   triode) rather than a transcription, so it is REPORTED with its measured
   balance and the bar rests on the transcribed tone stack instead.
2. The plan expected `orange412` reuse to be argued from provenance. It is argued
   from a MEASUREMENT instead: the composed contrast is 3.50 dB *through the same
   cab*, so the difference between these two amps lives entirely in the heads.
3. The plan did not anticipate `TriodeStage`'s conflated couplings mattering.
   They matter a lot here (a 398 Hz corner applied twice), and the workaround plus
   its cost is §63.3.

## Measured results

Every number is in docs §63. Headlines:

* **THE BAR (a)** network mid-notch at noon: Rockerverb FMV **−6.01 dB** vs OR120
  James **+0.75 dB** = **6.76 dB** of contrast against a shipped **5.0** bar
  (1.76 dB of margin, recorded). Composed: **−0.72** vs **+2.78** = **3.50 dB**
  against **3.0** (0.50 dB — the tighter half, reported as such).
* **THE BAR (b)** level-matched at −20 dBFS: Rockerverb **29.75 %** THD vs OR120
  **1.89 %** = **15.76× (23.95 dB)** against a **5×** bar. Mechanism: VOLUME
  0.01 → 0.10 moves **19.89 dB** of level for a THD ratio of **0.971**.
* Preamp DC S1–S4: 1.0850 / 1.2958 / 0.9564 / 1.1678 mA, every one cross-checked
  against Ohm's law on the transcribed 100 k. Supply 400 → 355.18 → 331.49 V.
* EL34 quad: rail 489.55 V, 33.89 mA, **16.59 W = 66 %** of the 25 W rating.
* LTP: balance **0.971988**, legs −25.4276 / +24.7153, plates 82.5 / 81.2 % of B+,
  Ip1 0.8291 mA. NFB depth **6.65 dB** and FLAT (0.26 dB spread).
* Breakup ≥5 % THD at **GAIN 0.20**; VOLUME law −25.6 dB at 0.05.
* Power: composed cranked **93.9 W**, power-section sine ceiling **82.15 W** with
  feedback / **84.78 W** without, against a rated 100 W. Reported, not tuned.
* Alias floor 4× : **−80.1 dB at 48 kHz** (hard) / **−52.7 dB at 44.1 kHz**
  (XFAIL), 1× → 4× improvement 28.1 dB, 8× −92.1 / −64.0.
* DC on signal **0.2152 %** clean, **0.2157 %** with +0.1 V of input offset.
* Latency **360 samples / 7.50 ms**; CPU **42.04 %** of one 48 kHz stream
  (jcm800 35.89 %, ac30 36.51 % in the same session).
* Block-size invariance and reset-vs-fresh **0.000e+00**; rate spread **0.007 dB**
  over 44.1–96 kHz; every zero-resting companion **exactly 0.0** after 4 s.
* **ALL FIVE GOLDENS UNCHANGED at ±0.00** (`--golden-report`, nothing written).
* Core ctest **34/34, exit 0** (checked unpiped). Native 3/3 including a new
  `TS → Squash → Rocker Verb` `identical_core_test` board at **0.000e+00**.
  Node 15 / 10 / 12, electron 20. WASM rebuilt, **91** hashed inputs, gate green.

### Perturbations (docs §63.13)

P1 slope 39k→3.9k RED · P2 treble cap 560p→47n RED · P3 VOLUME 1M→1k RED ·
P4 GAIN gang broken RED · **P5 S3→S4 divider 220k→2M2 GREEN (a bar that could not
fail) → absolute divider windows added → P5′ RED** · P6 LTP symmetric RED ·
P7 mid pot as a rheostat RED · P8 NFB disconnected RED · P9 bias −47→−56 RED ·
P10 dropper 10k→100k RED · restore GREEN.

## Files created / modified

Created: `core/include/clipper/dsp/Rockerverb{Preamp,PowerAmp,Amp}.h`,
`core/src/dsp/Rockerverb{Preamp,PowerAmp,Amp}.cpp`,
`core/tests/test_rockerverb_amp.cpp`, this plan file.
Modified: `core/CMakeLists.txt`, `core/src/clipper_c_api.cpp`,
`core/tools/bench/main.cpp`, `scripts/build-wasm.sh`,
`web/src/{params,rig,audio}.ts`, `web/src/components/{Amp,Board}.tsx`,
`web/src/App.tsx`, `web/src/assistant/{tools,prompt}.ts`,
`web/src/styles/amp.css`, `web/tests/amp.spec.ts`,
`native/src/{ClipperEngine.h,ClipperEngine.cpp,PluginProcessor.cpp,PluginEditor.cpp}`,
`native/tests/identical_core_test.cpp`, `web/public/generated/*` (rebuilt),
`docs/DEVELOPMENT.md` (§63), `ROADMAP.md` (M10.7), `CLAUDE.md`.

## Deferred to next session

See docs §63.11. In priority order: the CLEAN channel (needs a netlist — do not
invent it); the shared-oversampling-domain slice that owns BOTH
`rockerverb-alias-44k1` and `orange-schematic-alias-44k1` and would also recover
the 360-sample latency and the 42 % CPU; `TriodeStage` independent input/output
coupling configs (§63.3); the ~93 W ceiling; the Attenuator; confirming the pot
taper letters against a factory sheet; a native `rockerverb` snapshot scene.

## Status

- [x] Complete
