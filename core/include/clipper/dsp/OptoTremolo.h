// Clipper — portable DSP core (M10.1).
//
// OptoTremolo: the Fender AB763 "vibrato" (a famous misnomer — it is a
// TREMOLO: amplitude modulation, not pitch). The AB763 circuit is an OPTICAL
// tremolo: a low-frequency oscillator (LFO) drives a neon lamp, and the lamp's
// light falls on a photocell (LDR, the "roach") whose resistance shunts the
// signal to ground. When the lamp brightens the LDR conducts and the signal
// level DIPS; when it darkens the level recovers.
//
// The character of the classic soft "throb" comes from the LDR's ASYMMETRIC
// lag: a photocell DARKENS FAST when lit (resistance drops in a few ms) but
// RECOVERS SLOWLY when the light is removed (tens of ms). So the gain envelope
// is NOT the pure sine of the LFO — its dips are sharper than its recoveries,
// a soft-sawtooth-ish throb. We model that with an opto-cell state that follows
// the lamp with a fast ATTACK time constant and a slow RELEASE time constant.
//
// This is built as a small REUSABLE class (future amps — Princeton, Vibrolux,
// the brownface bias-vary trem — all want an opto tremolo). Mono, linear-time-
// varying (a per-sample gain multiply): no oversampling needed (the LFO is
// ~1–10 Hz, so the amplitude-modulation sidebands sit within a few Hz of each
// partial — no aliasing to guard).
//
//   SPEED (0..1)      -> LFO rate, LOG mapped kMinHz..kMaxHz (~1..10 Hz)
//   INTENSITY (0..1)  -> modulation DEPTH (0 = no throb, 1 = full amplitude dip)
//
// intensity == 0 is a bit-exact passthrough (the gain stays 1.0 — the LFO still
// runs so a knob move mid-note doesn't jump phase, but the multiply is unity).
//
// Platform-free C++17 (no OS/browser/Emscripten includes). Same call-shape
// family as ChorusModel/ReverbModel; owned BY TwinAmp (which routes SPEED /
// INTENSITY here).

#ifndef CLIPPER_DSP_OPTO_TREMOLO_H
#define CLIPPER_DSP_OPTO_TREMOLO_H

namespace clipper::dsp {

class OptoTremolo {
public:
    // Speed knob log map (documented; reproduced in the tests).
    static constexpr double kMinHz = 1.0;
    static constexpr double kMaxHz = 10.0;
    // Opto-cell (LDR) asymmetric lag. Photocells darken fast, recover slow —
    // the source of the soft-throb asymmetry (docs §20). Chosen constants:
    static constexpr double kAttackMs = 5.0;    // lamp brightening -> LDR drops fast
    static constexpr double kReleaseMs = 55.0;  // lamp darkening -> LDR recovers slow

    OptoTremolo();

    // Configure for a sample rate; resets LFO phase and the opto-cell state and
    // snaps the smoothed rate/depth to their current targets.
    void prepare(double sampleRate);

    // Reset the LFO phase + opto cell to the idle (lamp-dark, gain=1) state.
    void reset();

    void setSpeed(float knob01);      // 0..1, log map to kMinHz..kMaxHz
    void setIntensity(float knob01);  // 0..1, modulation depth

    // TREMOLO ENABLE (M10.1 amendment — the Fender "trem on/off", docs §20). When
    // DISABLED the effect gates OUT: a short linear enable-ramp carries the depth to
    // EXACTLY 0, so a settled-off tremolo is a BIT-EXACT passthrough (gain held at 1.0,
    // out == in), while the ramp keeps the toggle CLICK-FREE (no jump from a mid-dip
    // gain to unity). The LFO + opto cell keep running while disabled so re-enabling
    // does not jump phase. Default DISABLED (matches AMP_KNOB_DEFAULTS chorusMode 0).
    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    // Process numFrames of MONO audio, in -> out (may alias). With intensity == 0
    // this is a bit-exact copy of in into out (gain held at unity).
    void process(const float* in, float* out, int numFrames);

    // --- Introspection (measurement / tests) --------------------------------
    // Current LFO frequency in Hz (from the smoothed speed target).
    double rateHz() const;
    // The instantaneous opto gain multiplier applied to the LAST processed
    // sample (1.0 = no attenuation, dips toward 1-depth at a lamp peak).
    double lastGain() const { return lastGain_; }
    // Enable-ramp state (measurement/tests): 0 = fully bypassed, 1 = fully engaged.
    double enableRamp() const { return enableRamp_; }

private:
    // Advance the LFO + opto cell one sample, returning the gain multiplier.
    double tick();

    double sampleRate_ = 44100.0;
    double speed_ = 0.5;      // knob (smoothed target below)
    double intensity_ = 0.0;  // knob (smoothed target below)
    bool enabled_ = false;    // trem on/off (default OFF — old rigs unaffected)
    double enableRamp_ = 0.0; // 0 bypass .. 1 engaged (short LINEAR ramp, reaches 0/1 exactly)
    double enableStep_ = 1.0; // per-sample ramp increment (set in prepare)

    // Smoothed control values (one-pole, per-sample) so knob moves never click.
    double rateSmoothed_ = 0.0;    // Hz
    double depthSmoothed_ = 0.0;   // 0..1
    double smoothA_ = 0.0;         // control smoother coefficient

    double phase_ = 0.0;           // LFO phase [0,1)
    double cell_ = 0.0;            // opto-cell state (0 = dark, 1 = fully lit)
    double attackA_ = 0.0;         // per-sample attack coefficient
    double releaseA_ = 0.0;        // per-sample release coefficient
    double lastGain_ = 1.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_OPTO_TREMOLO_H
