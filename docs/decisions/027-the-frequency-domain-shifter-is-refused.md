# ADR 027: The frequency-domain shifter is refused; the Cellar keeps SOLA

Date: 2026-08-27
Status: Accepted

## Context

M13.10 (docs §70) shipped the "Cellar" drop-tune on a time-domain SOLA pitch
shifter and left one XFAIL, `drop-triad-spread-at-minus-2`, whose ledger entry
named its own fix: *"a frequency-domain shifter (`FFT.h` exists) and it owns the
entry."* Two candidate fixes inside the SOLA family had already been built and
refuted in that slice (a wider correlation span; sub-sample lag refinement), so
the frequency-domain route was the last one standing.

ADR 023 and ADR 025 set the procedure for exactly this shape of question: **build
the substitution and measure it** — "both blocks do X" is neither a reason to
share nor a reason to refuse.

## Decision

**Build it, measure it head to head, and refuse it.** The Cellar keeps
`PitchShifter` unchanged. The candidate is kept as
`tools/measure/PhaseVocoderShifter.h` — outside `core/include/`, off every audio
path, and outside the WASM artifact's hash closure — together with the
head-to-head that refused it, so a later slice does not rebuild it to reach the
same answer.

## Consequences

**§70's diagnosis was right and is recorded as right.** A phase vocoder has no
splice, so the mechanism behind the XFAIL genuinely disappears: at N = 8192 an
E major triad measures 0.000 / 0.000 / 0.002 / 0.001 cents at −1 / −2 / −5 / −12
where the shipped SOLA measures 0.615 / 1.163 / 4.124 / 12.516.

**It is refused on cost, and the cost is measured, not argued.** That accuracy
requires N = 8192, i.e. **171.6 ms of flat latency** (envelope-measured) against
SOLA's 8.83 ms onset / 35.8 ms mean, plus **112.5 ms of transient rise at the
octave** against SOLA's 27.9 — on a pedal whose owner opened this round of work
with "it's too laggy", and whose §70 acceptance bar is that the pick attack
survives. At the largest affordable size, N = 4096 (83.7 ms), it is **worse than
the shipped shifter on the very bar it exists to fix** (rich triad at −2: 14.6
cents against 1.5). There is no window at which it both fixes the defect and costs
less than the defect.

**The structural reason, which generalises beyond this pedal.** A low-register
triad's partials are ~20 Hz apart, and a DOWNWARD shift relocates bin `k` to
`k·r`, halving that spacing again. So the resolution a drop pedal needs is twice
what the input alone implies, and resolution in an STFT is bought only with
window, which is bought only with latency. **A frequency-domain shifter is the
wrong family for a DOWNWARD polyphonic shift of low-register material at
playable latency**, and that is a property of the algorithm, not of this
implementation.

**What this makes harder.** The XFAIL now has no owner. Its `fix` field says
`unknown` and names both refuted candidates with their numbers, which is honest
but leaves the defect standing. The only route the diagnosis still points at — and
it is untried — is a **multi-lag** splice: "one lag cannot align three partials"
is a statement about the number of lags, not about the domain.

**What this makes easier.** A future slice reaching for a phase vocoder — for a
harmoniser, a detune, an upward shift, or a formant-preserving effect — has a
working, phase-locked implementation with a validated measurement battery beside
it, and a written record of the one case it must not be used for. An UPWARD shift
does not suffer the bin-spacing halving at all, so this refusal does not transfer
to one.

**One tool became shared infrastructure.** The per-partial frequency estimator
moved to `core/tests/support/PartialFreq.h`, alongside `LtpProbe.h` and
`DcOffset.h`, because a bar set by an unvalidated measurement is worse than no
bar: the test file's own Hann-peak helper reads this slice's E major triad at −5
as 5.25 cents where the validated estimator reads 4.53, which would have failed
correct code against a 5-cent bar.
