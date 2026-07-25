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
