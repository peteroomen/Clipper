// Clipper — portable DSP core (M10.4, docs §68).
//
// MesaPowerAmp: the Dual Rectifier's power section — a 12AX7 long-tailed pair
// with DELIBERATELY UNEQUAL plate loads, four 6L6GC in push-pull, a SWITCHABLE
// global feedback loop with three depths, and a SELECTABLE RECTIFIER.
//
//   PI grid <- the preamp's master
//     -> V5 12AX7 LTP   Ra(V5B) 82k, Ra(V5A) 90k, 120p across each,
//                       1M grid leaks, 470R + 10k, 4k7 tail
//     -> 47n coupling -> 220k grid leaks -> 1k5 grid stoppers
//     -> 4 x 6L6GC, 1k/2W screens, fixed bias -51 V
//     -> OT, taps 8-16 ohm and 4 ohm
//     -> global NFB: 47k (LDR19) || 47k (LDR20) into 4k7, with the 25k
//        PRESENCE pot + 100n shunting the lower leg
//
// ===========================================================================
// PROVENANCE — this half is TRANSCRIBED too (sheets `mbdr2` and `mbdr4`)
// ===========================================================================
// Unlike RockerverbPowerAmp (whose whole power section is a documented
// reconstruction, because the netlist that existed was a preamp-only file), the
// Dual Rectifier's power amp AND its supply are both on the owner-supplied
// sheets. Transcribed: the LTP's two plate loads, its tail, the 120p plate
// snubbers, the 47n couplings, the 220k grid leaks, the 1k5 grid stoppers, the
// 1k screens, the -51 V bias, both feedback resistors, the presence pot and cap,
// the rectifier select, the reservoir pair and the whole dropper ladder.
//
// RECONSTRUCTION, named: the 6L6GC device card (the shared Koren fit from
// TwinPowerAmp.h), the OT's corner frequencies and reflected load (no transformer
// spec is on the sheet), and kFullScaleSecV (this project's own normalization).
//
// ===========================================================================
// (a) THE ACCEPTANCE BAR: THE MODERN MODES RUN OPEN-LOOP
// ===========================================================================
// Sheet `mbdr7` switches LDR19 ("FEEDBACK") and LDR20 ("MORE FEEDBACK") per
// mode, and the two MODERN modes turn BOTH off:
//
//     mode        LDR19  LDR20   feedback resistance   beta
//     OR CLN       ON     ON     47k || 47k = 23.5k    0.1667
//     OR NORM      ON     OFF    47k                   0.0909
//     RED VINT     ON     OFF    47k                   0.0909
//     OR MOD       OFF    OFF    open                  0.0
//     RED NORM     OFF    OFF    open                  0.0
//
// beta = 4k7 / (4k7 + Rfb), straight off the drawing. So this amp's two most
// popular modes have NO GLOBAL NEGATIVE FEEDBACK AT ALL — the power stage runs
// open-loop, with the higher output impedance, lower damping factor and softer,
// later-onset compression that implies. Every other amp in this lineup has a
// permanently wired loop (JCM 6.37 dB, OR120 7.81 dB, Rockerverb 6.65 dB).
//
// THIS IS WHY A RECTO'S LOW END IS "LOOSE" AND IT IS A TOPOLOGY, NOT AN EQ
// CURVE. Do not "fix" it with a low shelf, and do not give the modern modes a
// small non-zero beta to tidy the code — the drawing says open, and the bar in
// docs §68.4 measures it.
//
// ===========================================================================
// (b) THE OTHER BAR: THE RECTIFIER SELECT MOVES SAG, NOT LEVEL ALONE
// ===========================================================================
// Sheet `mbdr4` carries TWO independent switches that are commonly conflated:
//
//   1. `rect select` — 4 x 1N4007 SILICON vs 2 x 5U4 VALVE rectifiers. They tap
//      DIFFERENT HT winding taps: the silicon leg takes 350 V + the 50 V (`blu`)
//      boost tap, the valve leg takes the plain 350 V. So silicon is BOTH higher
//      B+ AND stiffer, and the open-circuit ratio 350/400 = 0.875 is transcribed
//      rather than chosen.
//   2. `SPONGY` / `BOLD` — a MAINS-PRIMARY-SIDE switch inserting series
//      impedance ahead of the power transformer. Nothing to do with the valves.
//
// Modelled on §55's machinery: a Thevenin HT source behind kRsupply charging
// kCreservoir, exactly as Ac30PowerAmp does. The valve rectifier's source
// resistance is the one RECONSTRUCTED number here (a 5U4GB drops ~50 V at
// 250 mA => ~200 ohm; two in parallel => ~100 ohm), and it is stated as such.
//
// Convention: 1.0f == 1.0 V in, normalized (1.0 == full scale) out.

#ifndef CLIPPER_DSP_MESA_POWER_AMP_H
#define CLIPPER_DSP_MESA_POWER_AMP_H

#include "clipper/dsp/Jcm800PowerAmp.h"  // El34Params, the pentode law, LtpInverter
#include "clipper/dsp/MesaPreamp.h"      // MesaMode
#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/TwinPowerAmp.h"    // Tube6L6Params, to6L6()

namespace clipper::dsp {

// The two front-panel supply switches, as their own types so a caller cannot
// pass one where the other belongs.
enum class MesaRectifier : int { Silicon = 0, Valve5U4 = 1 };
enum class MesaPowerMode : int { Bold = 0, Spongy = 1 };

class MesaPowerAmp {
public:
    enum ParamId : int {
        PARAM_PRESENCE = 0,  // the transcribed 25k pot in the feedback leg
        PARAM_DRIVE = 1,     // 0..1 — input trim into the PI grid
        PARAM_COUNT = 2,
    };

    MesaPowerAmp();

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);
    int oversampling() const { return os_.factor(); }
    int latencySamples() const { return os_.latencySamples(); }
    void setParameter(int paramId, float value);
    void reset();

    // The mode drives the feedback depth and NOTHING else in this class.
    void setMode(MesaMode m);
    MesaMode mode() const { return mode_; }
    void setRectifier(MesaRectifier r);
    MesaRectifier rectifier() const { return rect_; }
    void setPowerMode(MesaPowerMode p);
    MesaPowerMode powerMode() const { return powerMode_; }

    void process(const float* in, float* out, int numFrames);

    // --- Introspection (measurement / tests) --------------------------------
    double tubeQuiescentPlateCurrent() const { return iqTube_; }
    double tubeQuiescentScreenCurrent() const { return ig2qTube_; }
    double railIdle() const { return vRailIdle_; }
    double screenIdle() const { return vScreenIdle_; }
    double railNow() const { return vRail_; }
    double screenNow() const { return vScreen_; }
    double lastOutputPeak() const { return lastOutPeak_; }
    const El34Params& tube() const { return tube6L6_; }
    const LtpInverter& inverter() const { return ltp_; }

    // --- TRANSCRIBED (sheet `mbdr2`) ----------------------------------------
    // 6L6 fixed bias. The sheet states it outright: "6L6 -51V" (and "-39V" for
    // the EL34 option this model does not ship). NOT derived, NOT fitted.
    static constexpr double kVbias = -51.0;
    static constexpr int kTubes = 4;
    static constexpr double kRscreen = 1000.0;   // 1k/2W per tube
    static constexpr double kCoupCc = 47.0e-9;   // PI -> grid coupling
    static constexpr double kCoupRg = 220.0e3;   // per-grid leak
    // The LTP's plate loads are UNEQUAL ON THE DRAWING: 82k on V5B, 90k on V5A.
    // That is worth stating loudly, because audit finding 8 (docs §45) spent a
    // whole slice sweeping Ra2 on the JCM to balance its legs, and §63 did it
    // again for the Rockerverb. Here the factory already did it, so the value is
    // TRANSCRIBED and the resulting balance is a PREDICTION this model reports
    // rather than a calibration it performs.
    static constexpr double kRa1 = 82.0e3;
    static constexpr double kRa2 = 90.0e3;
    static constexpr double kRtail = 4.7e3;
    // Feedback: 47k with LDR19, a second 47k paralleled by LDR20, into 4k7.
    static constexpr double kRfbSingle = 47.0e3;
    static constexpr double kRfbDouble = 23.5e3;  // the two 47k in parallel
    static constexpr double kRfbLower = 4.7e3;
    // PRESENCE: 25k pot + 100n shunting the lower leg. The lift corner is where
    // the cap's reactance meets the lower leg: 1/(2*pi*4k7*100n) = 338.7 Hz at
    // full presence. DERIVED from the transcribed parts, not chosen.
    static constexpr double kPresencePot = 25.0e3;
    static constexpr double kPresenceCap = 100.0e-9;
    static constexpr double kPresenceCornerHz = 338.7;

    // --- TRANSCRIBED (sheet `mbdr4`) — the two supply switches ---------------
    // The silicon leg takes the 350 V winding PLUS the 50 V `blu` boost tap; the
    // valve leg takes the plain 350 V. The ratio is the drawing's, not a choice.
    static constexpr double kHtTapSilicon = 400.0;
    static constexpr double kHtTapValve = 350.0;
    // Open-circuit Thevenin source on the silicon setting, derived so the rail
    // lands on the sheet's own marked A = 460 V at the idle draw this model
    // computes. The valve setting scales by the TAP RATIO above.
    static constexpr double kVsupplySilicon = 468.0;
    static constexpr double kVsupplyValve =
        kVsupplySilicon * (kHtTapValve / kHtTapSilicon);
    // Source resistance. Silicon: the PT secondary alone (1N4007 drops ~1 V).
    // Valve: RECONSTRUCTED — a 5U4GB drops ~50 V at 250 mA (~200 ohm), two in
    // parallel ~100 ohm, plus the same secondary.
    static constexpr double kRptSecondary = 50.0;
    static constexpr double kRrect5U4Pair = 100.0;
    // SPONGY adds primary-side series impedance; referred to the secondary it is
    // a further series resistance. RECONSTRUCTED (the sheet gives the switch and
    // the part but no value), and named as the weakest constant in this file.
    static constexpr double kRspongyExtra = 120.0;
    // Two 220u in SERIES with 150k/150k balancing resistors = 110u effective.
    static constexpr double kCreservoir = 110.0e-6;

    // --- RECONSTRUCTION (no transformer spec is on the sheet) ---------------
    // Plate-to-plate reflected load. RECONSTRUCTION (no transformer spec is on
    // the sheet), but not a free choice: TwinPowerAmp.h carries 2 k for the SAME
    // 6L6 QUAD into the same 8 ohm, and a quad's Raa is half a pair's. A first
    // draft here used 3800 (a PAIR's load) and the composed amp topped out at
    // 52 W against a rated 100 -- measured, then corrected to the house value.
    static constexpr double kRaa = 2000.0;
    static constexpr double kRppPerTube = kRaa / 2.0;
    static constexpr double kOtLfHz = 40.0;
    static constexpr double kOtHfHz = 13000.0;
    static constexpr double kRdropPi = 10.0e3;
    // Secondary volts mapping to 1.0 full scale. DERIVED by measurement on the
    // COMPOSED amp, per the §23 convention that every voice is normalized to its
    // own cranked peak: the loudest mode (OR MOD) peaked at 0.6933 with a
    // provisional 45.0, so 45.0 * 0.6933 / 0.90 lands the cranked peak on 0.90.
    // The NFB tap uses the REAL secondary volts, never this.
    static constexpr double kFullScaleSecV = 34.66;

    static double otTurnsRatio() { return 15.8114; }  // sqrt(kRaa/8)

    // beta for the CURRENT mode, straight off the truth table.
    double feedbackBeta() const;
    double supplyOpenCircuit() const;
    double supplyResistance() const;

private:
    void solveOperatingPoint();
    void parkState();
    inline double solveTubePlate(double vg1k, double vg2, double rail, double& vpOut,
                                 double& baseOut) const;
    inline double solveGrid(double vpPlateAC, double vpPlateQ, double& vCc,
                            double& vgWarm) const;
    inline float processSampleOS(float x);

    double sampleRate_ = 44100.0;
    double osRate_ = 176400.0;
    int maxBlockSize_ = 128;
    Oversampler os_;
    LtpInverter ltp_;

    Tube6L6Params tube6L6cfg_{};
    El34Params tube6L6_{};

    MesaMode mode_ = MesaMode::RedModern;
    MesaRectifier rect_ = MesaRectifier::Silicon;
    MesaPowerMode powerMode_ = MesaPowerMode::Bold;

    double drive_ = 1.0;
    double presence_ = 0.5;

    double gRes_ = 0.0, gScr_ = 0.0, gCc_ = 0.0, gRg_ = 0.0;
    double otHpA_ = 0.0, otLpA_ = 0.0;
    double presA_ = 0.0;
    double gridRgk_ = 1.5e3;   // the transcribed 1k5 grid stoppers
    double gridVgn_ = 0.4;

    double vRailIdle_ = 460.0, vScreenIdle_ = 455.0;
    double iqTube_ = 0.0, ig2qTube_ = 0.0;
    double vRail_ = 460.0, vScreen_ = 455.0;
    double vCcUp_ = 0.0, vCcDown_ = 0.0;
    double vgUp_ = kVbias, vgDown_ = kVbias;
    double otHpS_ = 0.0, otLpS_ = 0.0;
    double presS_ = 0.0;
    double fbDelay_ = 0.0;
    double lastOutPeak_ = 0.0;
    static constexpr double kCscreen = 47.0e-6;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_MESA_POWER_AMP_H
