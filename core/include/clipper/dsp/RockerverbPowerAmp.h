// Clipper — portable DSP core (M10.7, docs §63).
//
// RockerverbPowerAmp: the back half of an Orange Rockerverb 100 — a 12AX7
// LONG-TAILED PAIR phase inverter, FOUR EL34s in fixed-bias push-pull, the output
// transformer, a plain global feedback loop and the B+ sag. ~100 W.
//
// EVERYTHING here reuses machinery that already shipped: the `LtpInverter` with
// audit finding 7's tail reference (docs §42), the Koren EL34 fit, the per-tube
// plate-load Newton, the coupling/blocking grid solve, the OT bandwidth pair and
// the rail/screen sag integrator (docs §18, reused by the OR120 in §57). NO NEW
// DEVICE MODEL was fitted for this voice.
//
// ===========================================================================
// PROVENANCE — this half is a RECONSTRUCTION, and it says so (docs §63.1)
// ===========================================================================
// The netlist this voice is transcribed from (LiveSPICE's own
// `Orange Rockerverb 50 Preamp.schx`) is a PREAMP ONLY. No Orange factory sheet
// was reachable: the proxy in this container permits github.com only, and
// el34world.com / orangeamps.com / Wikipedia all fail at CONNECT with 403.
//
// SOURCED (search-result text, named in §63.1):
//   * FOUR EL34s in the 100 W head, four 12AX7 preamp valves and two 12AT7s for
//     the reverb and the effects loop;
//   * the phase inverter valve is an ECC83 (12AX7) in its own socket — a WHOLE
//     dual triode dedicated to the PI. That is the reason this model uses a
//     long-tailed pair: the OR120's Field Guide entry says in terms
//     "Cathodyne type: 1/2 x 12AX7" (half a valve), and a cathodyne cannot use
//     the other half. This is an INFERENCE from a sourced fact, not a
//     transcription, and it is labelled as such.
//   * the front panel: Clean (Volume/Bass/Treble), Dirty (Gain/Volume/Bass/
//     Middle/Treble), shared Reverb + a footswitchable Attenuator. There is NO
//     PRESENCE control, which is why this power section has no presence knob and
//     its feedback loop carries no shaping filter.
//
// RECONSTRUCTED (every constant below): the LTP's plate loads / tail / tail
// reference, the EL34 fixed-bias voltage, Raa, the OT corners, the supply
// Thévenin source + reservoir, the screen network, the PI->EL34 coupling and the
// feedback divider. Each is chosen against a PHYSICAL constraint (the triodes in
// the project's 0.5-0.9 mA window with plates at 70-85 % of B+, the EL34s inside
// their 25 W plate rating, the amp reaching its rated power) — never against a
// tone. §57's rule applies verbatim: do not re-tune any of them toward a sound;
// find the schematic.
//
// THE ATTENUATOR IS NOT MODELLED. It is a post-OT load device (the MkIII's
// half-power/attenuated modes), so it belongs after this stage, not inside it.
// Named rather than hidden.
//
// Convention: real circuit VOLTS internally; process() output normalized so
// 1.0f == full scale (kFullScaleSecV). Platform-free C++17.

#ifndef CLIPPER_DSP_ROCKERVERB_POWER_AMP_H
#define CLIPPER_DSP_ROCKERVERB_POWER_AMP_H

#include <vector>

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/Jcm800PowerAmp.h"  // El34Params, the EL34 law, LtpInverter
#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/TriodeStage.h"

namespace clipper::dsp {

class RockerverbPowerAmp {
public:
    enum ParamId : int {
        PARAM_DRIVE = 0,  // 0..1 — input trim into the PI grid
        PARAM_COUNT = 1,
    };

    RockerverbPowerAmp();

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);
    int oversampling() const { return os_.factor(); }
    int latencySamples() const { return os_.latencySamples(); }
    void setParameter(int paramId, float value);
    void reset();

    void setFeedbackEnabled(bool on) { fbEnabled_ = on; }
    bool feedbackEnabled() const { return fbEnabled_; }

    // Process mono audio, in -> out (may alias). Input = PI grid drive in volts;
    // output normalized (1.0 == full scale).
    void process(const float* in, float* out, int numFrames);

    // --- Introspection (measurement / tests) --------------------------------
    double el34QuiescentPlateCurrent() const { return iqTube_; }
    double el34QuiescentScreenCurrent() const { return ig2qTube_; }
    double railIdle() const { return vRailIdle_; }
    double screenIdle() const { return vScreenIdle_; }
    double railNow() const { return vRail_; }
    double screenNow() const { return vScreen_; }
    double lastOutputPeak() const { return lastOutPeak_; }
    const El34Params& el34() const { return el34_; }
    const LtpInverter& inverter() const { return ltp_; }

    // --- Documented constants (all RECONSTRUCTION — see the banner) ---------
    // EL34 fixed bias. Derived, not chosen: the value that lands the quad at
    // ~68 % of the EL34's 25 W plate rating on this Koren fit and this rail —
    // the same design centre §18 derived for the 2204 and §57 for the OR120.
    static constexpr double kVbias = -47.0;
    // Plate-to-plate reflected load for a quad-EL34 100 W head, as §57's.
    static constexpr double kRaa = 1700.0;
    // Two tubes share each side's Raa/4, so per TUBE the reflected load is Raa/2.
    static constexpr double kRppPerTube = kRaa / 2.0;
    static constexpr int kTubes = 4;
    // Solid-state rectified (a modern head), so the supply is stiff.
    static constexpr double kVsupply = 500.0;
    static constexpr double kRsupply = 70.0;
    static constexpr double kCreservoir = 100.0e-6;
    // Per-tube screen resistor + a shared screen filter cap. Unlike the OR120's
    // transcribed unbypassed 1k (§57.3), a modern amp filters its screen node;
    // this one does, and the consequence is measured in §63.6 rather than
    // asserted.
    static constexpr double kRscreen = 1000.0;
    static constexpr double kCscreen = 47.0e-6;
    // The PI's own supply, behind a dropping resistor from the main rail.
    static constexpr double kRdropPi = 10.0e3;
    // A 100 W output transformer: the same corners §57 reconstructed for the
    // OR120's, because it is the same class of part.
    static constexpr double kOtLfHz = 45.0;
    static constexpr double kOtHfHz = 14000.0;
    // PI -> EL34 coupling and grid leaks (two 220k per side at the modelled node
    // would be the OR120's arrangement; a modern head runs one 220k per grid).
    static constexpr double kCoupCc = 22.0e-9;
    static constexpr double kCoupRg = 220.0e3;
    // Global feedback: 47k from the 8 ohm tap into the PI's second grid leg with
    // 4k7 to ground. There is NO presence pot in that leg — the Rockerverb's
    // panel has no presence control — so the loop is FLAT with frequency and
    // this model carries no shaping filter in it. That is a structural
    // difference from BOTH siblings (the 2204's presence pot, the OR120's H.F.
    // Boost R-L-C), and it is reported in §63.6, not asserted as the bar.
    static constexpr double kFeedbackBeta = 0.0909;  // 4.7k / (4.7k + 47k)
    // Secondary volts that map to 1.0 full scale. DERIVED by measurement on the
    // composed amp (the §23 convention: every voice is normalized to its own
    // cranked peak). The NFB tap uses the REAL secondary volts, never this.
    static constexpr double kFullScaleSecV = 42.99;

    static double otTurnsRatio() { return 14.5774; }  // sqrt(kRaa/8)
    double feedbackBeta() const { return fbEnabled_ ? kFeedbackBeta : 0.0; }

    // NO maxAbsRestingState() here, deliberately (ADR 006). Every state in this
    // stage idles at a real operating point — rail, screen, the coupling caps'
    // blocking voltages, the grid warm starts and the LTP's three node voltages —
    // except otHpS_/otLpS_/fbDelay_, which are flushed in processSampleOS and
    // whose floor is a push-pull residual, not zero. An accessor here would buy
    // an assertion with no teeth; see Jcm800PowerAmp.h's identical note.

private:
    void solveOperatingPoint();
    void parkState();
    inline float processSampleOS(float x);
    inline double solveTubePlate(double vg1k, double vg2, double rail, double& vpOut,
                                 double& baseOut) const;
    inline double solveEl34Grid(double vpPlateAC, double vpPlateQ, double& vCc,
                                double& vgWarm) const;

    double sampleRate_ = 44100.0;
    double osRate_ = 176400.0;
    int maxBlockSize_ = 128;

    Oversampler os_;
    LtpInverter ltp_;
    El34Params el34_{};

    double drive_ = 1.0;
    bool fbEnabled_ = true;

    double iqTube_ = 0.0, ig2qTube_ = 0.0;
    double vRailIdle_ = 480.0, vScreenIdle_ = 473.0;
    double vRail_ = 480.0, vScreen_ = 473.0;
    double gRes_ = 0.0, gScr_ = 0.0;

    double vCcUp_ = 0.0, vCcDown_ = 0.0;
    double vgUp_ = kVbias, vgDown_ = kVbias;
    double gCc_ = 0.0, gRg_ = 0.0;
    double gridRgk_ = 1500.0, gridVgn_ = 0.5;

    double otHpS_ = 0.0, otHpA_ = 0.0;
    double otLpS_ = 0.0, otLpA_ = 0.0;
    double fbDelay_ = 0.0;

    double lastOutPeak_ = 0.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_ROCKERVERB_POWER_AMP_H
