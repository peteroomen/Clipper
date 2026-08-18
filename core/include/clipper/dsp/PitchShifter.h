// Clipper — portable DSP core (M13.10, docs §70).
//
// PitchShifter: a POLYPHONIC pitch shifter built as two crossfaded taps sweeping
// a delay line. It is the second consumer of `DelayLine` (M13.4's primitive), and
// like that one it is deliberately IGNORANT of the pedal wrapped round it — it
// knows nothing about semitone selectors, dry blends or footswitches, so a future
// flanger, detune or harmoniser can hold one without inheriting a drop pedal.
//
// ===========================================================================
// THE MECHANISM, AND WHY IT IS POLYPHONIC WITHOUT TRYING
// ===========================================================================
// To shift by ratio `r`, read the delay line at a position that advances at `r`
// samples per written sample. That is RESAMPLING: it stretches the waveform in
// time and therefore scales EVERY frequency in it by exactly `r`. It does not
// know or care how many notes are present.
//
// **That is the whole reason this shape was chosen over a phase vocoder.** A
// tracker-based shifter has to decide on one f0 and shifts everything by that,
// so a chord comes out wrong; this cannot make that mistake because it never
// forms an opinion about pitch. The polyphony bar in the test suite is a
// consequence of the algorithm, not a tuning achievement — and it is asserted
// anyway, because an IMPLEMENTATION error (a wrong ratio, a mis-wrapped tap)
// would still break it.
//
// For a DOWNWARD shift (`r < 1`) the read falls behind the write at a rate of
// `(1 - r)` samples per sample, so a tap cannot run forever: it walks the whole
// window and must be re-seated. THE RE-SEAT IS THE ONLY HARD PART, and how it is
// done is the difference between a shifter that works and one that does not.
//
// **ONE TAP IS LIVE MOST OF THE TIME.** For the first `1 - kCrossfade` of the
// window there is a SINGLE tap at unity gain — no second read, so no comb and no
// modulation at all. Only during the last `kCrossfade` of the window does an
// incoming tap fade in, seated at `dMin`, while the outgoing one runs out at
// `dMin + W`:
//
//     p < 1 - x   : one tap, delay = dMin + p*W, gain 1        (CLEAN)
//     p >= 1 - x  : u = (p - (1-x)) / x
//                   out  delay = dMin + p*W          gain cos(pi*u/2)
//                   in   delay = dMin + u*x*W        gain sin(pi*u/2)
//     at p == 1   : wrap to p = x — the incoming tap IS the new tap, already at
//                   dMin + x*W, so the handover is continuous in both delay and
//                   rate.
//
// The gain pair is EQUAL-POWER (cos^2 + sin^2 = 1), so the crossfade does not
// pump. Do NOT replace it with a linear pair to "simplify": linear sums to 1 in
// AMPLITUDE, which for two decorrelated taps is a 3 dB dip in POWER halfway
// through every crossfade — an audible periodic suckout at the grain rate.
//
// **THE SPLICE POINT IS CHOSEN BY CROSS-CORRELATION (SOLA), AND THAT IS NOT AN
// OPTIMISATION — WITHOUT IT THE PITCH IS SIMPLY WRONG.** A fixed splice jumps the
// read position by a FIXED number of samples, which for any given input period is
// a FIXED fraction of a cycle. So every grain slips the phase by the same amount
// in the same direction, and a constant phase slip per unit time IS a frequency
// offset: measured on a 220 Hz tone with a fixed splice, the average frequency
// came out `r_eff = 0.9697*r + 0.0303` — +3.1 cents at a semitone and **+51.7
// cents at an octave**, with the exact target sitting 60 dB below the spectral
// peak. The resampling itself was proven exact to 0.000 cents in the same run, so
// the splice was the whole error.
//
// SOLA fixes it by searching a window of candidate splice positions for the one
// whose waveform best matches what the outgoing tap is playing, so the handover
// lands in phase instead of a fixed fraction out. **It stays polyphonic**: a
// correlation search forms no opinion about pitch, it only asks "where does this
// waveform repeat", which a chord answers as readily as a note.
//
// **THE OBVIOUS ALTERNATIVE WAS TRIED FIRST AND FAILED, TOO.** The textbook two-tap arrangement — taps a HALF
// window apart, crossfaded sin/cos across the whole cycle — is what this class
// was written as originally. It is wrong, and not subtly: both taps sit at ~0.707
// gain for most of the cycle with a fixed W/2 delay between them, so the output
// is a permanent deep comb rather than a shifted signal. Measured on a 220 Hz
// tone at a semitone down: 96 % of the output energy landed OFF the harmonics,
// and the apparent pitch was up to 150 cents wrong at an octave because the
// modulation sidebands were larger than the fundamental. The fix is not a tuning
// change, it is this structure: keep the second tap OFF except during the
// handover. See docs §70.2.
//
// ===========================================================================
// THE COST, NAMED UP FRONT
// ===========================================================================
// During the crossfade — and only then — two taps read the same signal at delays
// `(1 - x)*W` apart, which is a comb. That recurs at the GRAIN RATE,
// `(1 - r) / W` Hz: 1.1 Hz at a semitone with a 50 ms window, 10 Hz at an octave.
// So the artifact is (a) present for only `kCrossfade` of the time and (b) far
// rarer at small shifts than at large ones — which matters, because a semitone
// down is what this pedal is mostly bought for.
//
// This is not a defect to be tuned away, it is what this algorithm IS, and the
// test suite measures it per shift position rather than asserting it inaudible —
// a claim this shape cannot honestly make. See docs §70.
//
// ===========================================================================
// WHAT THIS IS MEASURED TO DO, AND THE ONE CASE IT DOES NOT
// ===========================================================================
// Measured at 48 kHz with the shipped constants (docs §70.4):
//
//     single note, -1 .. -12 semitones      0.006 .. 0.084 cents
//     E5 power chord (-1 / -2)              0.00 / 0.25 cents
//     E5 + octave                           0.00 cents
//     E major triad (-1 / -2)               0.50 / 1.25 cents
//     mean algorithmic latency              36 ms
//
// The triad row is the one that took work. At the first shipped triple it read
// 9.75 / 22.75 cents, and the cause was structural rather than a tuning miss:
// SOLA aligns to where the waveform REPEATS, and a 4:5:6 triad only repeats at
// its composite period (~48 ms on E2), which a 17.5 ms search could not reach.
// Widening the search fixed it — see kCrossfade for how that was bought without
// paying latency for it.
//
// A frequency-domain shifter (FFT.h) would have no splice and so none of this
// trade, at the cost of transient smearing. The plan said to build one only when
// a measurement demanded it; after the fix above, none does.
//
// Convention: 1.0f == 1.0 V. Platform-free C++17.

#ifndef CLIPPER_DSP_PITCH_SHIFTER_H
#define CLIPPER_DSP_PITCH_SHIFTER_H

#include <cmath>

#include "clipper/dsp/DelayLine.h"
#include "clipper/dsp/Denormal.h"

namespace clipper::dsp {

class PitchShifter {
public:
    // The read never gets closer to the write head than this. 2 samples keeps the
    // cubic interpolator's 4-point kernel inside the ring's guard (DelayLine
    // clamps at 1.0, so this is one sample of margin on top).
    static constexpr double kMinDelaySamples = 2.0;

    void prepare(double windowSeconds, double sampleRate) {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        windowSeconds_ = windowSeconds > 0.0 ? windowSeconds : 0.05;
        windowSamples_ = windowSeconds_ * sampleRate_;
        // The line must hold the largest delay a tap can reach, plus the minimum,
        // plus DelayLine's own guard. Ask for double the window so a later
        // setRatio() cannot outgrow the allocation on the audio thread.
        line_.prepare(2.0 * windowSeconds_ + 0.005, sampleRate_);
        reset();
    }

    void reset() {
        line_.reset();
        phase_ = kCrossfade;
        spliceDelay_ = kMinDelaySamples;
        inCrossfade_ = false;
    }

    // `r` is the frequency ratio: 0.5 == an octave down, 1.0 == no shift.
    // Values > 1 (upward) are accepted by the arithmetic but this pedal never
    // asks for them, and the wrap direction has NOT been measured for them —
    // see docs §70.7 before using this class for an upward shift.
    void setRatio(double r) {
        ratio_ = r > 0.0 ? r : 1.0;
        // Phase advance per sample. At r == 1 this is exactly zero, so the taps
        // freeze and the shifter becomes a fixed two-tap comb — which is why the
        // pedal SKIPS this class entirely at unity rather than calling it with
        // r = 1 (see DropModel).
        phaseInc_ = (1.0 - ratio_) / windowSamples_;
    }

    double ratio() const { return ratio_; }
    double windowSamples() const { return windowSamples_; }

    // Mean algorithmic delay: the live tap sweeps dMin+x*W .. dMin+W, so its
    // average read position is dMin + (1+x)/2 * W. Reported, NOT compensated:
    // the delay is a sawtooth by construction, so there is no single group delay
    // to remove — which is why DropModel reports 0 latency and says why.
    double meanDelaySamples() const {
        return kMinDelaySamples + 0.5 * (1.0 + kCrossfade) * windowSamples_;
    }
    static constexpr double crossfadeFraction() { return kCrossfade; }

    inline float process(float x) {
        line_.write(x);
        const double dOut = kMinDelaySamples + phase_ * windowSamples_;
        double y;
        if (phase_ < kCrossfadeStart) {
            // ONE tap, unity gain. No second read, so nothing to comb with.
            y = static_cast<double>(line_.readCubic(dOut));
            inCrossfade_ = false;
        } else {
            if (!inCrossfade_) {
                // Entering the handover: pick WHERE to re-seat, once per grain.
                spliceDelay_ = findSplice(dOut);
                inCrossfade_ = true;
            }
            const double u = (phase_ - kCrossfadeStart) / kCrossfade;
            const double dIn = spliceDelay_ + u * kCrossfade * windowSamples_;
            const double gOut = std::cos(0.5 * kPi * u);
            const double gIn = std::sin(0.5 * kPi * u);
            y = gOut * static_cast<double>(line_.readCubic(dOut)) +
                gIn * static_cast<double>(line_.readCubic(dIn));
        }

        phase_ += phaseInc_;
        if (phase_ >= 1.0) {
            // The incoming tap is now the live one, at spliceDelay_ + x*W. Set the
            // phase FROM the chosen splice rather than by a fixed subtraction, so a
            // correlation-shifted handover stays continuous in delay and in rate.
            inCrossfade_ = false;
            phase_ = (spliceDelay_ + kCrossfade * windowSamples_ - kMinDelaySamples) /
                     windowSamples_;
            if (phase_ >= 1.0) phase_ = 1.0 - 1e-9;
            if (phase_ < 0.0) phase_ = 0.0;
        }
        return static_cast<float>(y);
    }

    // ADR 006: the ring is a pure FIFO with no recursion into it, and `phase_` is
    // a bounded accumulator that never decays toward zero — so nothing here can
    // asymptote into the subnormal range. `DelayLine::write` already flushes what
    // it stores. Measured in the test suite rather than assumed.
    double maxAbsRestingState() const { return 0.0; }

private:
    // SOLA search, run ONCE per grain (tens of times a second), never per sample.
    // Finds the delay near kMinDelaySamples whose recent waveform best matches
    // what the outgoing tap is about to play, so the two are in phase when they
    // cross-fade. Normalised cross-correlation, so it is level-independent.
    //
    // The search span must cover at least one period of the lowest note the pedal
    // will see; a detuned low B is ~57 Hz, so kSearchSpan is set from that at the
    // prepared rate rather than as a magic sample count.
    double findSplice(double dOut) const {
        const int span = static_cast<int>(kSearchSeconds * sampleRate_);
        const int len = static_cast<int>(kCorrSeconds * sampleRate_);
        double bestScore = -2.0;
        double bestDelay = kMinDelaySamples;
        double refEnergy = 0.0;
        for (int i = 0; i < len; ++i) {
            const double v = line_.readCubic(dOut + static_cast<double>(i));
            refEnergy += v * v;
        }
        if (refEnergy <= 1e-20) return kMinDelaySamples;  // silence: nothing to align
        for (int k = 0; k <= span; ++k) {
            const double cand = kMinDelaySamples + static_cast<double>(k);
            double num = 0.0, den = 0.0;
            for (int i = 0; i < len; i += kCorrStride) {
                const double a = line_.readCubic(dOut + static_cast<double>(i));
                const double b = line_.readCubic(cand + static_cast<double>(i));
                num += a * b;
                den += b * b;
            }
            if (den <= 1e-20) continue;
            const double score = num / std::sqrt(den);
            if (score > bestScore) {
                bestScore = score;
                bestDelay = cand;
            }
        }
        return bestDelay;
    }

    static constexpr double kPi = 3.14159265358979323846;
    // The SOLA search span. It must reach the COMPOSITE period of the material,
    // not just one note's: a 2:3 power chord repeats at its lower note's period
    // (~12 ms on a low E) but a 4:5:6 triad only repeats at ~48 ms. 50 ms covers
    // both, and that is the whole reason triads work — see kCrossfade above.
    static constexpr double kSearchSeconds = 0.050;
    static constexpr double kCorrSeconds = 0.010;
    static constexpr int kCorrStride = 2;
    // Fraction of each window spent handing over, and it is DERIVED (docs §70.5)
    // by a route that is worth knowing, because the obvious reading is backwards.
    //
    // A short crossfade looks risky — less smearing of the splice — but SOLA
    // ALIGNS the splice in phase, so there is little left to smear, and the
    // crossfade's real job becomes hiding the residual rather than masking a
    // discontinuity. What a short crossfade BUYS is search span: the splice may
    // land anywhere in [dMin, dMin + span] and the buffer must still reset, which
    // requires span < (1 - kCrossfade) * W. So dropping x from 0.25 to 0.10 raises
    // the affordable span from 0.75*W to 0.90*W at NO latency cost.
    //
    // That is what closed the triad gap. Measured, E major triad at -1 / -2:
    //     x 0.25, span 17.5 ms, W 50 ms -> latency 31 ms, 9.75 / 22.75 cents
    //     x 0.10, span 50.0 ms, W 60 ms -> latency 33 ms, 0.50 /  1.25 cents
    //     x 0.10, span 50.0 ms, W 65 ms -> latency 36 ms, 0.50 /  1.25 cents
    // The shipped triple is the last one: the same accuracy as the 60 ms window
    // with 8.5 ms of margin on the span constraint instead of 4.
    static constexpr double kCrossfade = 0.10;
    static constexpr double kCrossfadeStart = 1.0 - kCrossfade;

    DelayLine line_;
    double sampleRate_ = 44100.0;
    double windowSeconds_ = 0.05;
    double windowSamples_ = 2205.0;
    double ratio_ = 1.0;
    double phaseInc_ = 0.0;
    double phase_ = kCrossfade;
    double spliceDelay_ = kMinDelaySamples;
    bool inCrossfade_ = false;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_PITCH_SHIFTER_H
