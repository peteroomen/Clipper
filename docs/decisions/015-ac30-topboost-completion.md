# ADR 015: The AC30 top-boost channel is completed, and its probes move to where the physics lives

Date: 2026-07-30
Status: Accepted

## Context

Finding 7 (ADR 014) balanced the AC30's phase inverter and thereby removed the even-harmonic
leakage that had been serving as the amp's "chime" and mid-volume "breakup". Five §23 voicing
bars became XFAILs owned by a future gain-structure slice. The owner's field report confirmed
the deficit ("not voxy, not jangly, duller than the Twin, doesn't break up with volume
correctly"), and audit finding 5 (the tone stack's structural ~−36 dB mid notch) was measured
to be inseparable from it: adding gain without fixing the stack leaves the voice notched, and
fixing them serially means re-deriving the staging constants and re-blessing the golden twice.

## Decision

1. **The top-boost channel is modeled whole**: V1 → VOLUME → V2 (the top-boost triode the
   model never had) → direct-coupled cathode follower → the corrected tone network. The
   owner chose the VOLUME position (between V1 and V2 — the historic top-boost-kit insertion
   point, making breakup track the knob) over the literal AC30/6 post-stack order, which
   would have left V2's drive volume-independent (the Twin's §44 defect, mirrored).
2. **The stack netlist is corrected in three places**: Cb in series with the bass rheostat
   and a 500 k wiper load (finding 5's two errors), plus the slope resistor moved to the
   treble pot's top — a third structural error not in the audit, found when the corrected
   stack under low-impedance drive measured an inverted treble knob. The bass rheostat is
   knob-mapped by a square law (the real pot is log; a linear map leaves the Vox "V" in the
   last 5 % of rotation).
3. **Constants re-derived to their §42.6 conventions**: volume taper k = 8 and
   kInterstageScale = 0.03 chosen jointly by a parameter search whose acceptance criteria
   are the five ledgered voicing bars simultaneously; kFullScaleSecV = 12.2 from the
   measured cranked swing alone.
4. **Two probes are re-derived because the defect they compensated for is gone** (the §42.9
   discipline, bars unchanged): the chime comparison moves from the power sections alone
   (where balanced push-pull pairs rightly cancel evens) to the composed amps at their
   documented settings (where the single-ended V2 makes them); the character guard moves
   from a 3 kHz/1 kHz ratio whose reference sat inside the notch to mid-fullness-vs-Twin +
   band-flatness + mids-over-bass.

## Consequences

The AC30 finally behaves like the amp §23 describes: clean jangle low, knob-tracking class-A
breakup from mid-volume, dialable scoop, +10.8 dB more 2nd harmonic than the Twin at matched
settings. All five XFAILs are deleted and their bars hard; the AC30 suite carries zero known
defects. Costs: ~+50 % relative CPU on the AC30 (still under the JCM800) and +144 samples of
latency from two more per-stage oversampling domains — consolidating the preamp cascade into
one shared domain remains the named follow-up. The `ts_ac30` golden moved (−4.94 dB RMS,
+18 dB at 800 Hz) and was owner-blessed; the level precedent is finding 7's Twin (honest
normalization, un-compensated). The old netlist's treble-knob behavior existed only because
the stack was driven from V1's plate — any future source-impedance change must re-run the
stack's knob-authority measurements, which is why they are asserted, not assumed.
