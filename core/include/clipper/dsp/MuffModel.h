// Clipper — portable DSP core.
//
// MuffModel (ROADMAP v1.1 item 4): a four-transistor "wall of sustain" fuzz — a
// trademark-safe homage to the early-70s "ram's head"-family Big Muff Pi. It is
// the FIRST BJT-based voice in the core, and its real product is the reusable
// device machinery underneath: BjtStage (BjtStage.h), an Ebers-Moll NPN gain stage
// solved per-sample with nodal Newton. The Fuzz Face and the RG100 solid-state
// preamp on the roadmap reuse BjtStage with a new Config; MuffModel is its first
// consumer and proof.
//
// ---------------------------------------------------------------------------
// Topology (documented canon: early-70s "ram's head" family, 9 V, 2N5088-class).
// ---------------------------------------------------------------------------
//   in ─► [Q1 input boost] ─► SUSTAIN pot ─► [Q2 clip] ─► [Q3 clip] ─►
//         ─► mid-scoop TONE stack ─► [Q4 recovery] ─► VOLUME ─► out
//
// - **4-stage BJT cascade** (Q1..Q4), each a BjtStage (Ebers-Moll NPN, collector-
//   feedback bias 470 k / 10 k / 390 Ω, 9 V). The cascade IS the fuzz.
// - **2 clipping stages** — Q2 and Q3 carry an antiparallel 1N4148 diode pair
//   across the base↔collector feedback (a SYMMETRIC soft clip). The DOUBLE clip is
//   WHY a Muff compresses into that endless wall of sustain: each stage's diodes
//   clip near idle (the collector-feedback bias sits the collector barely above the
//   base — almost no clean headroom, see BjtStage.h), so by the second stage the
//   signal is a hard-limited, hugely-compressed square-ish wave regardless of how
//   hard you pick. That compression is the sustain.
// - **The famous mid-scoop TONE stack** — a passive network with a high-pass leg
//   and a low-pass leg blended by the TONE pot (MuffToneStack below): at noon both
//   legs sum with a NOTCH between their corners (~1 kHz), the scooped-mids voice
//   that lets a Muff sit under a vocal; TONE to 0 = dark (low-pass only), TONE to
//   1 = bright/buzzy (high-pass only).
// - **SUSTAIN / TONE / VOLUME** — the three real knobs. SUSTAIN (a level pot after
//   Q1) sets how hard the clip stages are driven → how much fuzz + sustain; TONE is
//   the scoop tilt; VOLUME is output level (a Muff makes FAR more than unity gain).
//
// Params: slot 0 = SUSTAIN, slot 1 = TONE, slot 2 = VOLUME — matching the shared
// three-knob PedalParams shape (so the worklet/ABI/serializer stay pedal-agnostic).
//
// ---------------------------------------------------------------------------
// Antialiasing (M2 infra). Two cascaded clip stages generate MASSIVE harmonics, so
// the whole nonlinear cascade (Q1..Q4 + the tone stack, run together to stay
// sample-aligned) runs inside the shared Oversampler at 4× by default. Aliasing is
// measured at max sustain against the −60 dB M2 bar (see the tests). Convention:
// 1.0f == 1.0 V. Zero platform/OS/browser deps (portable C++17). Heavy state lives
// behind a pimpl.

#ifndef CLIPPER_DSP_MUFF_MODEL_H
#define CLIPPER_DSP_MUFF_MODEL_H

#include <memory>

namespace clipper::dsp {

class BjtStage;  // fwd (introspection accessor below)

// The Big-Muff-family mid-scoop TONE stack, as a reusable one-pole blend. A
// low-pass leg (R_lp·C, the bass/dark side) and a high-pass leg (R_hp·C, the
// treble/buzz side) are mixed by the TONE pot; at noon the two legs sum to a
// scooped NOTCH between their corners. Split out so the scoop can be validated
// against its own analytic H(s) in isolation (the transistor stages surround it).
//
//   H(jω, tone) = (1−tone)·H_LP(jω) + tone·H_HP(jω)
//   H_LP = 1/(1 + jω/ω_lp),   H_HP = (jω/ω_hp)/(1 + jω/ω_hp)
//
// Documented canon: C = 0.01 µF; R_lp = 22 kΩ (f_lp = 723 Hz), R_hp = 12 kΩ
// (f_hp = 1326 Hz) → the noon notch sits near √(f_lp·f_hp) ≈ 980 Hz (the ~1 kHz
// mid-scoop). MuffModel runs it at the OVERSAMPLED rate between Q3 and Q4.
struct MuffToneStack {
    static constexpr double kFlpHz = 723.0;   // low-pass leg corner (22 k / 0.01 µF)
    static constexpr double kFhpHz = 1326.0;  // high-pass leg corner (12 k / 0.01 µF)

    void prepare(double sampleRate);
    void setTone(float tone01);   // 0 = dark (LP), 0.5 = scoop, 1 = bright (HP)
    float processSample(float x);
    void reset();

    // Analytic |H(jω, tone)| in dB — the reference the scoop test validates the
    // rendered response against. Pure function of the documented corners above.
    static double analyticMagDb(double freqHz, double tone);

    double aLp_ = 0.0, aHp_ = 0.0;  // one-pole coeffs at the prepared rate
    float lpState_ = 0.0f;          // LP leg state
    float hpLpState_ = 0.0f;        // HP leg's internal low-pass state (HP = x − LP)
    float tone_ = 0.5f;
};

class MuffModel {
public:
    // Ids match the shared PedalParams triple positionally (0/1/2) so the chain ABI
    // is pedal-agnostic: for the Muff they READ as Sustain / Tone / Volume.
    enum ParamId : int {
        PARAM_SUSTAIN = 0,  // drive into the clip stages (fuzz + sustain)
        PARAM_TONE = 1,     // mid-scoop tilt (0 dark .. 0.5 scoop .. 1 bright)
        PARAM_VOLUME = 2,   // output level
        PARAM_COUNT
    };

    // Oversampling A/B hook: NAIVE == 1× (no oversampling) for the aliasing contrast;
    // the shipped path is the 4× oversampled cascade. (No ADAA here — the nonlinear
    // element is the transistor solve, not a static shaper.)
    enum ClipMode : int { CLIP_OVERSAMPLED = 0, CLIP_NAIVE = 1 };

    MuffModel();
    ~MuffModel();

    MuffModel(const MuffModel&) = delete;
    MuffModel& operator=(const MuffModel&) = delete;

    void prepare(double sampleRate, int maxBlockSize);

    void setOversampling(int factor);
    int oversampling() const;
    int latencySamples() const;

    void setClipMode(int mode);
    int clipMode() const;

    void setParameter(int paramId, float value);

    // Process numFrames of mono audio, in -> out (may alias). 1.0f == 1.0 V.
    void process(const float* in, float* out, int numFrames);

    // Recovery seam (audit finding 1). Re-park all four BJT stages at their
    // already-solved DC operating points and clear the tone stack, the two knob
    // smoothers and the oversampler histories. Does NOT re-solve: each stage's
    // damped Newton + up to 12·Rf·Cin of settling stays as prepared. Knobs survive;
    // allocation-free.
    void reset();

    // --- Introspection (measurement / tests) --------------------------------
    // The four cascade stages Q1..Q4 (index 0..3), for per-stage DC op-point
    // validation against the analytic bias math.
    const BjtStage& stage(int index) const;
    // Fixed input-drive gain into Q1 and the internal output trim, so the test can
    // reproduce the analytic signal levels if needed.
    static double inputDriveGain();
    static double outputTrim();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_MUFF_MODEL_H
