// Clipper — portable DSP core (M10.1).
//
// TwinPowerAmp: the Fender blackface AB763 (Twin-style) ~85 W push-pull POWER
// SECTION — a 12AT7 long-tailed-pair phase inverter, FOUR 6L6GC output tubes, the
// output transformer, global negative feedback (NO presence control), and a LIGHT
// B+ supply sag. This is the clean-headroom king's back end: huge, stiff, and
// mostly linear until pushed hard, where the breakup arrives from the PI + power
// tubes (not a voiced preamp). Modelled from circuit physics and MEASURED
// (docs §20), same discipline as the M9.3 Marshall power section. Convention:
// real circuit VOLTS internally; process() output normalized so 1.0f == full
// scale (see kFullScaleSecV). Platform-free C++17, zero web/server/electron deps.
//
// ===========================================================================
// 1. PHASE INVERTER — 12AT7 long-tailed pair (reuses LtpInverter)
// ===========================================================================
// The AB763 PI is a long-tailed pair, exactly the topology of the M9.3
// LtpInverter — so we REUSE that class, only swapping the tube: a 12AT7 (a
// LOWER-mu, higher-current triode than the 12AX7 the JCM PI uses). Parameter
// source: a widely-circulated Koren 12AT7 fit (the modeling-community parameter
// table in the Koren 1996 form): mu = 60, ex = 1.35, kg1 = 460, kp = 300,
// kvb = 300. Balanced 100k/100k plate loads (Fender's PI is more symmetric than
// the JCM's deliberate 100k/82k imbalance), 10k shared tail, B+_PI ≈ 410 V.
//
// ===========================================================================
// 2. 6L6GC PUSH-PULL QUAD — Koren pentode
// ===========================================================================
// Koren pentode plate/screen law (Norman Koren, "Improved SPICE models for
// vacuum-tube amplifiers", 1996 — the pentode form), same equations as the M9.3
// EL34. 6L6GC parameter fit (DOCUMENTED FIT — a widely-circulated Koren 6L6GC
// set; pentode fits are looser than triodes, hence the ±10 % validation band):
//   mu = 8.7, ex = 1.35, kg1 = 1460, kp = 48, kvb = 12, kg2 = 4500.
//
// Four tubes: TWO per phase, paralleled — modelled as a push-pull PAIR of
// "super-tubes," each super-tube = kTubesPerSide (2) paralleled 6L6GC (so plate
// current and supply draw scale by 2 per side, and the per-tube reflected plate
// load is Raa/2, see the OT note). Fixed bias Vg1k_q = −47 V (the value THIS
// Koren fit needs for the Twin's ~32 mA/tube operating point at a ~460 V rail;
// the nominal Twin figure is ≈ −52 V — derived + asserted in the tests, not
// against a datasheet). Class AB: idle ~32 mA/tube so a crossover region exists.
// The matched pair's Idiff = f(Vac) − f(−Vac) is ODD → EVEN harmonics cancel.
// 6L6 grid conduction charges the PI→grid coupling caps → BLOCKING (τ = Rg·Cc =
// 22 ms) on overdrive. Plate-load saturation via a per-tube 1-D Newton.
//
// ===========================================================================
// 3. OUTPUT TRANSFORMER — linear v1
// ===========================================================================
// Raa = 2 k plate-to-plate reflected load (the 6L6 quad into the rated 8 Ω).
// Turns ratio n = √(Raa/Rload) = √(2000/8) ≈ 15.8. Two documented corners: a
// LOW LF corner ~40 Hz (the Twin's big OT extends the bottom) and HF ~14 kHz.
// Core saturation EXPLICITLY DEFERRED — v1 is linear, the nonlinearity lives in
// the tubes. Output normalized to 1.0f == full scale via kFullScaleSecV.
//
// ===========================================================================
// 4. NEGATIVE FEEDBACK — NO presence control (authenticity)
// ===========================================================================
// Global NFB from the OT secondary tap back to the PI's cold-side grid via the
// AB763's 820 Ω feedback resistor (with the 100 Ω leg to ground): β_eff referred
// to the modelled secondary (kFeedbackBeta). The loop OPPOSES the output (closed-
// loop gain LOWER than open-loop — validated). The blackface Twin has NO PRESENCE
// control (per the visual/authenticity doctrine — presence is a Marshall/tweed-
// bandmaster feature, not on the AB763 vibrato channel), so the feedback is FLAT:
// no HF shelf shaping. This is one reason the Twin sounds so tight and controlled.
// The feedback carries a unit delay at the oversampled rate (explicit-loop
// decoupling, same as M9.3).
//
// ===========================================================================
// 5. SAG — LIGHT (the Twin is famously stiff)
// ===========================================================================
// SS-rectifier supply: a low Thévenin impedance (Rsupply ≈ 80 Ω) behind a large
// reservoir (Creservoir ≈ 100 µF), plus a stiff screen node. The rail barely
// droops under load — sag DEPTH lands in a LIGHT ~1–2 dB window, deliberately
// SMALLER than the JCM800's 2–6 dB (asserted in the tests). Backward-Euler rail +
// screen integration per oversampled sample, same structure as M9.3.

#ifndef CLIPPER_DSP_TWIN_POWER_AMP_H
#define CLIPPER_DSP_TWIN_POWER_AMP_H

#include <vector>

#include "clipper/dsp/Jcm800PowerAmp.h"  // El34Params/pentode law + LtpInverter (reused)
#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/TriodeStage.h"

namespace clipper::dsp {

// 6L6GC Koren pentode parameters (documented fit; see header). Reuses the same
// El34Params-shaped struct + el34PlateCurrent/el34ScreenCurrent evaluators (the
// Koren pentode form is device-agnostic — only the six constants differ).
struct Tube6L6Params {
    double mu = 8.7;
    double ex = 1.35;
    double kg1 = 1460.0;
    double kp = 48.0;
    double kvb = 12.0;
    double kg2 = 4500.0;
};

// Bridge the 6L6 fit into the shared El34Params-typed evaluators.
inline El34Params to6L6(const Tube6L6Params& p) {
    El34Params e;
    e.mu = p.mu; e.ex = p.ex; e.kg1 = p.kg1; e.kp = p.kp; e.kvb = p.kvb; e.kg2 = p.kg2;
    return e;
}

class TwinPowerAmp {
public:
    enum ParamId : int {
        PARAM_DRIVE = 0,   // PI input drive trim (0..1) — how hard the volume signal
                           // hits the phase inverter. No presence (the Twin has none).
        PARAM_COUNT = 1,
    };

    TwinPowerAmp();

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);
    int oversampling() const { return os_.factor(); }
    int latencySamples() const { return os_.latencySamples(); }

    void setParameter(int paramId, float value);

    void setFeedbackEnabled(bool on) { fbEnabled_ = on; }
    bool feedbackEnabled() const { return fbEnabled_; }

    void process(const float* in, float* out, int numFrames);

    // --- Introspection (measurement / tests) --------------------------------
    double tubeQuiescentPlateCurrent() const { return iqTube_; }  // A / tube
    double tubeQuiescentScreenCurrent() const { return ig2qTube_; }
    double railIdle() const { return vRailIdle_; }
    double screenIdle() const { return vScreenIdle_; }
    double railNow() const { return vRail_; }
    double screenNow() const { return vScreen_; }
    double lastOutputPeak() const { return lastOutPeak_; }
    const El34Params& tube() const { return tube6L6_; }
    const LtpInverter& inverter() const { return ltp_; }

    // Documented constants (cited in the tests / docs §20).
    static constexpr double kVbias = -50.5;          // 6L6 fixed bias (V) — this fit
    static constexpr double kRaa = 2000.0;           // OT plate-to-plate load (Ω)
    static constexpr int kTubesPerSide = 2;          // 4 tubes: 2 paralleled per phase
    // Per-tube effective reflected plate load. A PP pair sees Raa/4 per side; with
    // kTubesPerSide paralleled tubes carrying the side current, each tube's own
    // current sees Raa/4 * kTubesPerSide = Raa/2 (derived in the header).
    static constexpr double kRppReflected = kRaa / 2.0;   // = 1000 Ω per tube
    static constexpr double kVsupply = 470.0;        // B+ Thévenin no-load (V)
    static constexpr double kRsupply = 80.0;         // stiff SS-rectified supply (Ω)
    static constexpr double kCreservoir = 100.0e-6;  // big reservoir (F) — stiff
    static constexpr double kRscreen = 470.0;        // screen dropping R (Ω)
    static constexpr double kCscreen = 47.0e-6;      // screen filter cap (F)
    static constexpr double kOtLfHz = 40.0;          // OT LF corner (Hz) — big OT
    static constexpr double kOtHfHz = 14000.0;       // OT HF corner (Hz)
    static constexpr double kCoupCc = 100.0e-9;      // PI→6L6 coupling cap (F) 0.1uF
    static constexpr double kCoupRg = 220.0e3;       // 6L6 grid leak (Ω) — τ=22 ms
    // AB763 feedback: the 820 Ω resistor from the OT tap + 100 Ω leg to ground,
    // referred to the modelled 8 Ω secondary volts. β_eff chosen so the loop gain
    // β·A_real lands the ~4–5 dB of global feedback the Twin runs (tighter, cleaner
    // than the lightly-fed-back JCM). Flat (no presence shaping).
    static constexpr double kFeedbackBeta = 0.16;    // effective NFB divider
    // Secondary volts that map to 1.0 full scale. Calibrated (docs §20) against the
    // COMPOSED amp: fully cranked (VOLUME 1, hot input) a power sine peaks ~0.9, a
    // normal full-send ~0.78 — headroom to 1.0 for transients. The modelled OT
    // secondary saturates near ~28 V at the clipping ceiling.
    static constexpr double kFullScaleSecV = 24.0;

    static double otTurnsRatio() { return 15.811; }  // √(Raa/8) = √250
    double feedbackBeta() const { return fbEnabled_ ? kFeedbackBeta : 0.0; }

private:
    void solveOperatingPoint();
    inline float processSampleOS(float x);
    // Plate-load Newton with the hoisted Koren base (Ip = base·atan(Vp/kvb), exact
    // dIp/dVp); baseOut feeds the shared-E1 screen current (§25).
    inline double solveTubePlate(double vg1k, double vg2, double rail, double& vpOut,
                                 double& baseOut) const;
    // Grid-node solve; vgWarm carries the previous sample's solution (§25).
    inline double solveTubeGrid(double vpPlateAC, double vpPlateQ, double& vCc,
                                double& vgWarm) const;

    double sampleRate_ = 44100.0;
    double osRate_ = 176400.0;
    int maxBlockSize_ = 128;

    Oversampler os_;
    LtpInverter ltp_;
    Tube6L6Params tube6L6cfg_{};
    El34Params tube6L6_{};   // the 6L6 fit routed through the pentode evaluators

    double drive_ = 1.0;
    bool fbEnabled_ = true;

    double iqTube_ = 0.0, ig2qTube_ = 0.0;
    double vRailIdle_ = 460.0, vScreenIdle_ = 452.0;

    double vRail_ = 460.0, vScreen_ = 452.0;
    double gRes_ = 0.0, gScr_ = 0.0;

    double vCcUp_ = 0.0, vCcDown_ = 0.0;
    // Previous-sample grid solutions (Newton warm start, §25).
    double vgUp_ = kVbias, vgDown_ = kVbias;
    double gCc_ = 0.0, gRg_ = 0.0;
    double gridRgk_ = 1500.0, gridVgn_ = 0.5;

    double otHpA_ = 0.0, otHpS_ = 0.0;
    double otLpA_ = 0.0, otLpS_ = 0.0;
    double fbDelay_ = 0.0;   // secondary volts, previous OS sample (flat NFB)

    double lastOutPeak_ = 0.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_TWIN_POWER_AMP_H
