// Clipper — portable DSP core (M6.3).
//
// ChorusModel: the JC-120's chorus/vibrato — a single modulated (BBD-style)
// delay line that splits a mono signal into a STEREO pair. This is the block
// that turns the amp stereo: in the real JC-120 the post-preamp/reverb signal
// forks into two power-amp + speaker paths, one dry and one through a
// clock-swept bucket-brigade delay, and the famous "chorus" is the ACOUSTIC sum
// of the two speakers in the room. We model the fork, not the room.
//
//   OFF      (mode 0): outL == outR == input, bit-exact (delay line untouched).
//   CHORUS   (mode 1): outL = dry input, outR = modulated wet. The stereo bloom
//                      is the dry-L / wet-R split; comb filtering happens when a
//                      listener's ears (or a room) sum the two sides.
//   VIBRATO  (mode 2): outL == outR == modulated wet (no dry reference), so you
//                      hear pure pitch wobble, not chorus. Both sides identical.
//
// Interpolation: 4-point Lagrange (cubic) fractional delay. Chosen over all-pass
// interpolation because the read position sweeps CONTINUOUSLY under the LFO and
// reverses direction twice per LFO cycle: all-pass interpolation is recursive
// (has state), so a fast delay-modulation reversal rings its internal filter and
// smears transients; Lagrange is stateless, so every sample is interpolated from
// scratch with flat-ish group delay and no reversal artifact. Cubic (not linear)
// keeps the HF loss of the moving read tap inaudible.
//
// LFO: a pure SINE. The BBD clock in a real JC sweeps triangle-ish, but a
// triangle delay sweep makes the PITCH deviation a square wave — it snaps between
// +Δ and −Δ cents with an abrupt reversal you hear as a chirp. A sine delay sweep
// gives a smooth cosine pitch deviation: musical, and what the ear reads as "the
// JC chorus." (Documented tradeoff; see docs/DEVELOPMENT.md §M6.3.)
//
// Delay / depth / speed (tuned in the .cpp, see the constants there):
//   base delay  ~5 ms  (decorrelates the wet side for the stereo bloom)
//   depth 0..1  -> sine sweep amplitude 0 .. ~1.5 ms peak
//   speed 0..1  -> LFO rate, LOG mapped ~0.15 .. 8 Hz
// Peak pitch deviation scales with depth AND rate (physically: faster sweep of
// the same excursion = larger Δf); the ~1.5 ms cap keeps a full-depth, mid-rate
// vibrato lush (~30-40 cents) without turning seasick.
//
// Smoothing: depth (sweep amplitude) and the LFO rate are one-pole smoothed so a
// knob move never clicks. Mode is a hard switch (like the pedal/amp bypass in
// this codebase — a deliberate footswitch action).
//
// Platform-free C++17 (no OS/browser/Emscripten includes). Same call-shape family
// as AmpModel; owned BY AmpModel (which routes its chorus params here).

#ifndef CLIPPER_DSP_CHORUS_MODEL_H
#define CLIPPER_DSP_CHORUS_MODEL_H

#include <memory>

namespace clipper::dsp {

class ChorusModel {
public:
    enum Mode : int {
        MODE_OFF = 0,
        MODE_CHORUS = 1,
        MODE_VIBRATO = 2,
    };

    // LFO clock waveform. SINE is the JC-120 voicing and the DEFAULT, so an owner
    // that never calls setWaveform() renders bit-identically to every build before
    // this seam existed (docs §62; proven by render hash and by the
    // clean120_chorus golden reading ±0.00).
    //
    // TRIANGLE exists because the Boss CE-1 — literally this same circuit in a
    // floor box — sweeps its BBD clock with a TRIANGLE in CHORUS mode and a sine
    // in VIBRATO mode, which is a sourced, load-bearing difference rather than a
    // preference. Note what it does: for delay(t) = D0 + A·tri(ωt) the pitch
    // deviation is −d(delay)/dt, and the derivative of a triangle is a SQUARE
    // wave — so a triangle sweep does not "wobble", it alternates between two
    // FIXED detunings of ∓4·A·f. The header note below calls that out as the
    // reason the JC voicing chose sine; the CE-1 is the box where the real
    // circuit does it anyway, and its chorus mode is heard as a comb rather than
    // as pitch, which is why it gets away with it.
    enum Waveform : int {
        WAVE_SINE = 0,
        WAVE_TRIANGLE = 1,
    };

    ChorusModel();
    ~ChorusModel();
    ChorusModel(const ChorusModel&) = delete;
    ChorusModel& operator=(const ChorusModel&) = delete;

    // Configure for a sample rate; allocates the delay buffer, resets state, and
    // snaps the depth/rate smoothers to their current targets.
    void prepare(double sampleRate);

    // Reset the delay line + LFO phase to silence/zero without reallocating.
    void reset();

    // speed / depth are 0..1 knob positions (clamped). mode is 0/1/2 (clamped to
    // the valid range).
    void setSpeed(float knob01);
    void setDepth(float knob01);
    void setMode(int mode);

    int mode() const;

    // --- Direct voicing seam (docs §62) -------------------------------------
    //
    // setSpeed/setDepth above carry the JC-120's OWN knob tapers (log 0.15..8 Hz,
    // and a squared depth taper into a fixed 5 ms base). A different box built on
    // this same circuit has different tapers AND a different delay window, so it
    // sets the physical quantities directly and does its own knob mapping. These
    // are voicing setters, not knobs: the values are milliseconds and hertz.
    //
    // Every one of them feeds the SAME smoother the knob path feeds, so a moving
    // base delay or sweep is click-free on exactly the same ~8 ms ramp. An owner
    // that never calls them keeps the JC defaults, which is why this is additive.
    //
    // The delay buffer is sized for the JC's own worst case (base + max sweep +
    // 3 ms of guard); base+sweep beyond that is clamped per sample by the
    // existing interpolator floor/ceiling rather than reallocating.
    void setBaseDelayMs(double ms);
    void setSweepMs(double ms);
    void setRateHz(double hz);
    void setWaveform(int w);

    // --- Measurement hooks (NOT user controls) ------------------------------
    // The current LFO rate in Hz, AFTER smoothing. A test measuring "what rate
    // does this knob give" should measure the RENDERED pitch track, not this —
    // this exists so a harness can confirm the smoother has settled first.
    double currentRateHz() const;
    // Largest absolute value in the delay line. The ring is a pure FIFO of the
    // input (no recursion into it), so on silence it reaches EXACTLY 0.0 after
    // one buffer length — asserted rather than assumed (ADR 006 scope rule).
    double maxAbsDelayLine() const;

    // Split numFrames of mono audio into a stereo pair. in may alias neither out
    // (outL/outR must be distinct buffers). In OFF mode this is a bit-exact copy
    // of in into both outputs.
    void processStereo(const float* in, float* outL, float* outR, int numFrames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_CHORUS_MODEL_H
