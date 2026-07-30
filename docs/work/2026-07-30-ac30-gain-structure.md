# AC30 gain structure — the missing top-boost stage, the stack correction, the VOLUME re-taper

**Date:** 2026-07-30
**Branch:** claude/amps-pedals-fixes-6f557i
**Roadmap item:** the five `finding7-ac30-*` XFAILs (docs §42.8) + audit finding 5; owner field
report 2026-07-30 ("the vox isn't voxy at all — not jangly, duller than the Twin, and it doesn't
break up with volume correctly")

## Goal

The composed AC30 breaks up on the VOLUME knob ahead of the Twin with a class-A h2 chime and a
bright, dialable top-boost voice: the five ledgered XFAILs resolved (XPASS→hardened or honestly
re-owned), the structural mid notch gone (dialable instead of hardwired), measured against an
external reference where one exists.

## Approach

Deliberate TONE change — the completion of the top-boost channel. Owner decisions locked
2026-07-30 (AskUserQuestion): **finding 5's stack correction is IN this slice** (same physical
circuit; splitting means re-deriving the staging constants and re-blessing `ts_ac30` twice), and
**VOLUME sits between V1 and V2** (the historic top-boost-kit insertion point — breakup tracks
the knob, pluck/hot separation naturally right; measured best in the planning prototype).

New topology: V1 → VOLUME (re-tapered) → V2 (12AX7 CC, 100k/1.5k‖25µ, the canonical top-boost
values) → cathode follower (Rk 100k, direct-coupled) → corrected `TopBoostToneStack` (**Cb in
series with the bass rheostat** + the **volume-pot load stamped at OUT** — the two errors of
finding 5) → post-stack pad → `kInterstageScale` → power amp. Constants re-derived by
measurement, each to its own convention (§42.6): staging from PI drive, `kFullScaleSecV` from
the measured cranked swing. The (taper k, pad) pair is chosen by an in-slice parameter search
whose acceptance bars are the five XFAIL properties + the §23 clean bar simultaneously.

Planning-session prototype evidence (scratchpad `ac30_probe{,2,3}.cpp`): three-stage preamp
+47.3 dB at 220 Hz with the corrected stack (vs −0.5 dB shipped at 1 kHz); composed h2 at
VOLUME 0.6 reachable at −16 dBc (+8.4 dB over the Twin); CF rout 226 Ω; V2 op point
Va 202.4 / Vk 1.46; CPU ~+51 % relative (still under the JCM); latency 72 → 216 samples.

## Steps

- [ ] `TopBoostToneStack`: 6-node MNA (series Cb, 500 k pot load), knob-sense re-map, smoothing
      kept, denormal flushes + `maxAbsRestingState()` extended; validate vs analytic H of the
      NEW netlist and record the external-curve position honestly
- [ ] `Ac30Preamp`: V2 + CF stages (Jcm800Preamp wiring pattern), `setSourceImpedance(cfRout)`,
      volume moved between V1/V2, `latencySamples()` = sum of three stages, reset/park via the
      existing stage loop (nan-guard block C must stay green)
- [ ] Parameter search: (volume taper k, post-stack pad) → acceptance = the five bars +
      clean-jangle + monotonicity + cranked peak window
- [ ] Re-derive `kInterstageScale` and `kFullScaleSecV` by measurement; document both
- [ ] XFAILs: delete + harden the XPASSing bars; re-derive the chime probe to the composed amps
      (physics named: the single-ended top-boost triode + cathode-bias dynamics make the evens;
      the balanced PI rightly cancels them; bar +3 dB over the Twin unchanged, checked on both);
      `-breakup-vs-twin` stays ledgered against the Twin volume-placement… (note: the Twin's own
      fix LANDED in §44, so re-measure — it may be reachable now)
- [ ] Re-derive `testCharacterGuard` (its 1 kHz reference sat inside the notch — multi-point
      tilt with perturbation-proven teeth), `testProduct` windows, the cathode-bloom probe, the
      stack analytic test; prove every rewritten bar by perturbation (touch after patch AND
      restore)
- [ ] Aliasing at max volume (new nonlinear stage, per-stage OS domain — M2 bar re-measured);
      interleaved same-machine CPU A/B; latency end-to-end
- [ ] `--golden-report`: `ts_ac30` moves (owner bless in the morning — hold the goldens commit);
      other four byte-stable (scope check); web amp-level drift guard re-centred for the AC30
      row only, with the decision comment
- [ ] Full core ctest; WASM rebuild + artifact; web build + Playwright; node suites
- [ ] Docs §46 + ADR 015 (topology completion + probe re-derivations); CLAUDE.md; this file;
      commit voicing change, push; PR after the morning bless

## How this will be measured

- THD vs VOLUME (220 Hz, 0.316 V): onset ≤ 0.65, ≥ 8 % and ≥ 1.8× Twin at 0.6, monotonic
- Composed h2 at matched output: AC30 ≥ Twin + 3 dB (re-derived chime probe)
- Small-signal tilt: 5 kHz rel 82 Hz from −2.6 dB toward the Twin's +9.2 (brighter, dialable)
- Stack |H|: the 880 Hz hole from −35.8 dB hardwired to knob-dialable; worst-case error vs the
  analytic H of the new netlist < 1.5 dB (discretization), external-curve delta reported
- Cranked peak ≈ 0.9 after `kFullScaleSecV` re-derivation; aliasing 4× ≥ 60 dB
- CPU interleaved A/B (expect ~31 % → ~48 % of one stream); latency +144 samples reported

## Manual test steps

- [ ] Web/native: sweep VOLUME 0.2 → 0.9 with a hot pickup — clean jangle low, real class-A
      breakup arriving mid-knob, no click/zipper through the sweep
- [ ] A/B vs the Twin at 0.6: the AC30 is audibly brighter and dirtier
- [ ] BASS/TREBLE now audibly reshape the mids (the scoop dials); CUT still darkens the top only
- [ ] Edge: NaN rejected; reset clean; VOLUME 1.0 pluck finite/bounded; 44.1/96 k spot-check

## Out of scope for this session

Finding 4 (the sag saturator), consolidating the per-stage oversampling domains (the CPU
follow-up), the JCM bright cap (parked), native tuner, everything else in the round.

---

<!-- Fill in below during/after the session -->

## What actually happened

As planned, plus one discovery that grew the slice a third head: the stack netlist had a
THIRD structural error beyond finding 5's two — the slope resistor fed the treble pot's
BOTTOM, which inverted the treble knob (−1.9 dB the wrong way) the moment the corrected
stack was driven from the follower's 226 Ω instead of V1's 45 k plate. R1 belongs on the
pot top with Ct; fixed in netlist + analytic, documented in §46.1. The bass rheostat also
gained a square-law knob map (real pot is log; linear parked the whole Vox "V" in the last
5 % of travel).

The parameter search ran on (taper k × interstage s) jointly as planned; k = 8 / s = 0.03
won on all bars at once. `kFullScaleSecV` re-derived on the product-probe convention
(first derivation used the wrong probe and measured 1.29 cranked — caught by the product
test, re-derived to 12.2). Probes re-derived per §42.9: chime → composed amps (physics
named), character guard → mid-fullness/flatness/mids-over-bass (its old 1 kHz reference
sat inside the notch). Web swap spec's level-based "swap landed" detector re-derived to a
harmonic-signature detector at volume 0.75 (the honest re-normalization made levels
coincide at the old probe). One process wobble, honestly recorded: a stray `git checkout`
reverted the test-file surgery mid-slice; all edits were re-applied scripted and the suite
re-verified green.

## Measured results

Docs §46.3 table. Headlines: onset ≈ 0.5–0.6 tracking the knob (Twin 0.8), 10.8 % at the
documented 0.6 (3.6× Twin), composed h2 +10.8 dB over the Twin, clean 0.24 % at 0.35,
880 Hz hole → flat within 1.4 dB, treble knob +8.3 dB authority, cranked 0.898. All five
XFAILs XPASSed → hardened; AC30 suite zero known defects (repo ledgers 5 → 4, ctest 26 →
25 entries). CPU interleaved A/B 30.1/30.4 → 46.5/47.2 % (< JCM 54.5/55.8). Latency
72 → 216 samples. Perturbations: old netlist red, k = 4 red (§46.4). `ts_ac30` −4.94 dB /
+17.96 dB @ 800 Hz, owner-blessed; Playwright 71/71; node+electron green.

## Files created / modified

- `core/include/clipper/dsp/Ac30Preamp.h` + `core/src/dsp/Ac30Preamp.cpp` (V2, CF,
  6-node corrected stack, volume position, taper, latency)
- `core/include/clipper/dsp/Ac30Amp.h` (kInterstageScale), `Ac30PowerAmp.h` (kFullScaleSecV)
- `core/tests/test_ac30_amp.cpp` (analytic re-derivation, probe re-derivations, five
  XFAILs deleted + hardened), `core/CMakeLists.txt` (ledger entry removed)
- `web/tests/amp.spec.ts` (swap-landed detector re-derived), `web/public/generated/*`
- `core/tests/goldens/` (ts_ac30 + GOLDENS.md), `docs/DEVELOPMENT.md` §46,
  `docs/decisions/015-ac30-topboost-completion.md`, this file, `CLAUDE.md`

## Deferred to next session

- Consolidating the AC30 preamp's three per-stage oversampling domains into one (CPU +
  latency follow-up, named in ADR 015)
- Finding 4 (the sag static saturator) — unchanged by this slice
- The external published-curve check for the stack (research slice; §46 argues topology
  against the published schematic, response bars are vs the new netlist's analytic)

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
