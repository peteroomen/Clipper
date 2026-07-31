// Clipper — portable DSP core (M10.3, docs §57; SCHEMATIC CORRECTION 2026-07-31).
//
// OrangePowerAmp: the back half of an early-1970s Orange OR120 — the driver
// triode, the CATHODYNE phase inverter it is AC-COUPLED to, FOUR EL34s in
// push-pull, the output transformer, global negative feedback from the 16 ohm tap
// and the H.F. Boost network that sits in the driver's cathode. ~120 W.
//
// The EL34 device law, the per-tube plate-load Newton, the coupling/blocking grid
// solve, the OT bandwidth pair and the rail sag integrator are REUSED from
// Jcm800PowerAmp (docs §18) — same fit, same numerics, no new device model.
//
// ===========================================================================
// 1. THE INVERTER IS A CATHODYNE — AND IT IS AC-COUPLED (defect #7)
// ===========================================================================
// Transcribed, output-amp sheet:
//
//   DRIVER (1/2 ECC83)            CATHODYNE (1/2 ECC83)
//     grid <- loop return 100k      grid <- driver plate through 68n (1M leak)
//     Ra 100k to C+, 1n across it   Ra 100k to C+ ; Rk 100k  (EQUAL split loads)
//     cathode: 1k5 + 220k           plate  -> 68n -> 2k4 -> EL34 grids (pair 1)
//       + the H.F. BOOST network    cathode-> 68n -> 2k4 -> EL34 grids (pair 2)
//       + the NFB injection (15k)
//
// The first release DC-coupled the two (driver plate node == cathodyne grid node)
// and solved them as ONE 3x3 Newton. The early-70s amp puts a 68n coupling cap
// and a 1M grid leak between them, so this build solves them SEPARATELY: a 2x2
// Newton for the driver (Vpd, Vkd) and a 1-D Newton for the cathodyne (Vkc), with
// the 68n/1M network carrying the signal across. That is not a refactor — it
// changes what the driver's plate node is loaded by, and it un-pins the
// cathodyne's DC from the driver's.
//
// What does NOT change, and must not: the split loads stay EQUAL (Ra == Rk ==
// 100k), so the two legs are read off ONE current through TWO equal resistors and
// are anti-phase and equal BY TOPOLOGY. The leg-balance probe still measures
// 0.999965, against the 2204 LTP's 0.988 that took audit finding 8 and a resistor
// sweep (§45). It still clips on COMPLIANCE (Vkc pinned to [0, C+/2]).
//
// WHAT DID CHANGE, and the first release's own claim that it refutes: that build
// said the compliance clip was "asymmetric by construction" (-132.7 / +67.3 V),
// because the DC-coupled driver's plate pinned the cathodyne's grid off-centre.
// The AC-coupled stage is biased at the CENTRE of its compliance, so the two
// limits measure -104.23 / +103.97 V — symmetric to 0.25 %. The test asserts what
// is now true, which is a stronger check than the old one, not a weaker one.
//
// THE ONE VALUE THE SHEET DOES NOT CARRY, named rather than hidden: the sheet
// records "1M grid leak" on the cathodyne's grid but not the node that leak
// RETURNS to. It cannot be ground — measured, a 100k/100k cathodyne with its grid
// at 0 V idles at tens of microamps and cannot swing anywhere near the 48 V the
// transcribed EL34 fixed bias needs (docs §57.3). The only arrangement that keeps
// the transcribed EQUAL split loads is the textbook one: the leak returns to a tap
// in the cathode leg. The tap is DERIVED, not fitted to a tone — it is placed so
// the stage idles at the CENTRE of its own compliance (Vkc = C+/4, i.e. Vak =
// C+/2), which is the cathodyne's published design rule, and the resulting swing
// is then checked against an ABSOLUTE number from the same schematic: it must
// exceed the EL34s' 48 V of fixed bias.
//
// ===========================================================================
// 2. THE H.F. BOOST IS AN INDUCTOR (defect #8)
// ===========================================================================
// Transcribed: "1k LIN pot + a 2 mH CHOKE + 0.47uF to ground, with 100k", at the
// DRIVER'S CATHODE. That is a series R-L-C branch shunting the cathode resistor,
// resonant at 1/(2*pi*sqrt(L*C)) = 5.19 kHz: at resonance the reactances cancel,
// the cathode's degeneration collapses and the driver's gain rises there. The
// pot sets the series damping (rheostat: 1k at minimum, 0 at maximum boost).
//
// The first release had NO inductor anywhere and modelled "presence" as a
// one-pole shaping of the feedback voltage. That is a different circuit with a
// different shape (a shelf, not a resonance) in a different place (the loop, not
// the cathode).
//
// ===========================================================================
// 3. GLOBAL NFB — FROM THE 16 OHM TAP, THROUGH 15k, INTO THE DRIVER'S CATHODE
// ===========================================================================
// Transcribed: "F.B from the OT 16 ohm tap -> 15k -> driver cathode node". So the
// loop encloses the driver AND the inverter (the 2204's loop starts at the PI's
// second grid and encloses only the PI), and the injection resistor is 15k, not
// the reconstruction's 27k. The tap matters too: this model's `vSec` is the 8 ohm
// tap (otTurnsRatio = sqrt(Raa/8)), so the 16 ohm tap is sqrt(2) higher and the
// loop is correspondingly deeper.
//
// ===========================================================================
// 4. FOUR EL34s, FIXED BIAS, SOLID-STATE BRIDGE, A+ BEFORE THE CHOKE
// ===========================================================================
// Two tubes per side; each side's reflected load is Raa/4 shared by two tubes, so
// each TUBE sees Raa/2 (kRppPerTube). Transcribed from the power-supply sheet:
// 8x 1N4005 bridge -> 1A HT fuse -> A+ (2x 100uF in series = 50uF) which is the
// OT CENTRE TAP; A+ -> CHOKE -> B+ (2x 32uF in series = 16uF) which feeds the
// SCREENS through the transcribed per-tube 1k (R35-R38, 1K 5W); B+ -> 33K -> C+
// (the driver and cathodyne); C+ -> 33K -> D+ (V1A and V1B).
//
// The choke's DCR and inductance are NOT on the sheet, so the model does not
// split A+ from B+: the choke is treated as ideal at DC, which makes the two
// reservoirs one 66 uF node and leaves the transcribed per-tube 1k as the only
// screen impedance. Named in docs §57.1; it removes the screen-node time constant
// the reconstruction had (the screens now track the rail).
//
// Convention: real circuit VOLTS internally; process() output normalized so
// 1.0f == full scale (kFullScaleSecV). Platform-free C++17.

#ifndef CLIPPER_DSP_ORANGE_POWER_AMP_H
#define CLIPPER_DSP_ORANGE_POWER_AMP_H

#include <vector>

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/Jcm800PowerAmp.h"  // El34Params + the shared EL34 device law
#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/TriodeStage.h"

namespace clipper::dsp {

// ---------------------------------------------------------------------------
// CathodyneInverter — the AC-COUPLED driver + split-load pair.
//
//   driver:    2x2 nodal Newton in (Vpd, Vkd)
//     plate    : (C+ - Vpd)/Rad + i(Cad) - Ipd - i(coupling) = 0
//     cathode  : Ipd - Vkd/Rk_total - (Vkd - vfb)/Rfb - i(boost RLC) = 0
//   coupling:  68n from Vpd into a 1M leak returning to the cathodyne bias tap
//              (linear, folded into the driver's plate node as one conductance
//              plus a history current, so the Newton stays 2x2)
//   cathodyne: 1-D Newton in Vkc
//     Ipc(C+ - 2*Vkc, Vgc - Vkc) - Vkc/Rsplit = 0
//
// The cathodyne grid draws no current (documented simplification, docs §57.13),
// which is what keeps the coupling network linear.
// ---------------------------------------------------------------------------
class CathodyneInverter {
public:
    struct Config {
        // Both plate loads return to the SAME supply node, C+ (transcribed).
        double bPlus = 434.0;
        double Rad = 100.0e3;          // driver plate load (transcribed)
        double Cad = 1.0e-9;           // "1n across it" (transcribed)
        double Rkd = 1.5e3;            // driver cathode resistor (transcribed)
        double RkdShunt = 220.0e3;     // the 220k also on that node (transcribed)
        double Rfb = 15.0e3;           // NFB injection from the 16 ohm tap
        double Rsplit = 100.0e3;       // Ra == Rk, the EQUAL split loads
        double Ccoup = 68.0e-9;        // driver plate -> cathodyne grid
        double Rgc = 1.0e6;            // cathodyne grid leak
        // H.F. Boost: 1k LIN pot + 2 mH choke + 0.47 uF to ground, with 100k.
        double RboostPot = 1.0e3;
        double Lboost = 2.0e-3;
        double Cboost = 0.47e-6;
        double RboostBleed = 100.0e3;
        // The 2 mH choke's winding resistance is not on the sheet; a real iron
        // cored 2 mH part measures single-digit ohms. Named in docs §57.1.
        double RchokeDcr = 8.0;
        TriodeStage::KorenParams tube{};  // 12AX7, both triodes
    };

    void configure(const Config& c) { cfg_ = c; }
    const Config& config() const { return cfg_; }
    // osRate is the OVERSAMPLED rate: the reactive companions live in here.
    void prepare(double osRate);
    void reset();

    void setBoost(double p) { boost_ = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p); }

    // One sample. Returns the two anti-phase ABSOLUTE node voltages:
    //   vPlate = the cathodyne PLATE  (C+ - Vkc)
    //   vCath  = the cathodyne CATHODE (Vkc)
    void processSample(double vg, double vfb, double& vPlate, double& vCath);

    double quiescentDriverPlate() const { return vpdq_; }
    double quiescentDriverCathode() const { return vkdq_; }
    double quiescentDriverCurrent() const { return ipdq_; }
    double quiescentCathodyneCathode() const { return vkcq_; }
    double quiescentCathodynePlate() const { return cfg_.bPlus - vkcq_; }
    double quiescentCathodyneCurrent() const { return ipcq_; }
    double quiescentCathodyneGrid() const { return vTap_; }
    // The portion of the 100k cathode leg ABOVE the grid-leak tap — the derived
    // bias element (see the header banner). Rsplit - this is the portion below.
    double biasTapOhms() const { return rkTap_; }
    // The LIVE driver plate node.
    double driverPlateNow() const { return vpd_; }
    // The LIVE cathodyne grid node (after the 68n / 1M coupling).
    double cathodyneGridNow() const { return vgc_; }

    // Anti-denormal diagnostic (docs §33, ADR 006). TWO states in this class rest
    // at zero — the H.F. Boost branch's current and its choke voltage — and those
    // are the two that are guarded. Everything else in here rests at a real
    // operating point and is deliberately NOT flushed:
    //   * the 1n across the plate load        rests at C+ - Vpdq
    //   * the 68n coupling cap                rests at Vpdq - Vtap
    //   * the boost branch's 0.47 uF          rests at the DRIVER'S CATHODE
    //     voltage (measured 1.9454 V), because the branch carries no DC current
    //     and the cap therefore stands at the whole node voltage. That one looks
    //     like a zero-resting state and is not.
    double maxAbsRestingState() const { return maxAbsState(ib_, vL_); }
    double boostCapRestVolts() const { return vC_; }

private:
    void solveDc();

    Config cfg_{};
    double osRate_ = 176400.0;
    double boost_ = 0.5;

    double vpd_ = 0, vkd_ = 0, vkc_ = 0, vgc_ = 0;   // live node state
    double vpdq_ = 0, vkdq_ = 0, vkcq_ = 0;          // quiescent
    double ipdq_ = 0, ipcq_ = 0;
    double vTap_ = 0.0, rkTap_ = 0.0, vgkcq_ = 0.0;

    // Reactive companions.
    double geqAd_ = 0.0, vAd_ = 0.0, iAd_ = 0.0;     // 1n across the plate load
    double geqCc_ = 0.0, vCc_ = 0.0, iCc_ = 0.0;     // 68n coupling
    double rL_ = 0.0, rC_ = 0.0;                     // boost L and C companions
    double ib_ = 0.0, vL_ = 0.0, vC_ = 0.0;          // boost branch state
};

// ---------------------------------------------------------------------------
// The composed OR120 power section.
// ---------------------------------------------------------------------------
class OrangePowerAmp {
public:
    enum ParamId : int {
        PARAM_HF_DRIVE = 0,  // 0..1 — the H.F. BOOST pot (1k lin, in the cathode)
        PARAM_DRIVE = 1,     // 0..1 — input trim into the driver grid
        PARAM_COUNT = 2,
    };

    OrangePowerAmp();

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);
    int oversampling() const { return os_.factor(); }
    int latencySamples() const { return os_.latencySamples(); }
    void setParameter(int paramId, float value);
    void reset();

    void setFeedbackEnabled(bool on) { fbEnabled_ = on; }
    bool feedbackEnabled() const { return fbEnabled_; }

    // Process mono audio, in -> out (may alias). Input = driver grid drive in
    // volts; output normalized (1.0 == full scale).
    void process(const float* in, float* out, int numFrames);

    // --- Introspection (measurement / tests) --------------------------------
    double el34QuiescentPlateCurrent() const { return iqTube_; }
    double el34QuiescentScreenCurrent() const { return ig2qTube_; }
    double railIdle() const { return vRailIdle_; }
    double screenIdle() const { return vScreenIdle_; }
    double railNow() const { return vRail_; }
    double screenNow() const { return vScreenUp_; }
    // C+ — the driver/cathodyne supply, after the transcribed 33K from B+.
    double cPlusIdle() const { return vCplusIdle_; }
    double lastOutputPeak() const { return lastOutPeak_; }
    const El34Params& el34() const { return el34_; }
    const CathodyneInverter& inverter() const { return inv_; }

    // --- Documented constants (cited in the tests / docs §57) ----------------
    static constexpr double kVbias = -48.0;         // EL34 fixed bias (V)
    static constexpr double kRaa = 1700.0;          // OT plate-to-plate load (Ohm)
    // Two tubes share each side's Raa/4, so per TUBE the reflected load is Raa/2.
    static constexpr double kRppPerTube = kRaa / 2.0;
    static constexpr int kTubes = 4;                // the quad
    static constexpr double kVsupply = 510.0;       // B+ Thevenin no-load (V)
    static constexpr double kRsupply = 70.0;        // SOLID-STATE bridge: stiff
    // 2x 100uF in series at A+ (50uF) plus 2x 32uF in series at B+ (16uF); with
    // the choke ideal at DC the two are one node. See the header banner.
    static constexpr double kCreservoir = 66.0e-6;
    // TRANSCRIBED: R35-R38 = 1K 5W wirewound, one per EL34 screen (pin 4). There
    // is no screen bypass cap on the sheet, so the drop is algebraic.
    static constexpr double kRscreen = 1000.0;
    // TRANSCRIBED: B+ -> 33K 2W -> C+ -> 33K 2W -> D+.
    static constexpr double kRdropCplus = 33.0e3;
    // A 120 W output transformer has far more primary inductance than the 2204's
    // 50 W unit, so its LF corner sits LOWER. NOT on the sheet (the sheet gives
    // the 16/8/4 ohm taps and nothing else about the OT) — a documented
    // reconstruction, unchanged by this pass.
    static constexpr double kOtLfHz = 45.0;
    static constexpr double kOtHfHz = 14000.0;
    // TRANSCRIBED: cathodyne legs -> 68n -> 2k4 -> EL34 grids; grid leaks
    // 220K + 220K to N.B, i.e. two 220k per SIDE = 110k at the modelled node.
    static constexpr double kCoupCc = 68.0e-9;
    static constexpr double kCoupRg = 110.0e3;
    // The 16 ohm feedback tap, relative to this model's 8 ohm secondary.
    static constexpr double kFbTapRatio = 1.4142135623730951;  // sqrt(16/8)
    // Secondary volts that map to 1.0 full scale. DERIVED by measurement on the
    // CORRECTED circuit (docs §57.3, the §23 convention: every voice is
    // normalized to its own cranked peak). The NFB tap uses the REAL secondary
    // volts, never this, so the loop gain is independent of it.
    static constexpr double kFullScaleSecV = 43.086;

    static double otTurnsRatio() { return 14.5774; }  // sqrt(kRaa/8)
    // The feedback divider, for reporting: the real loop is the cathode-node
    // solve (Rfb into the driver's cathode network), not this scalar.
    double feedbackDivider() const {
        const auto& c = inv_.config();
        const double rk = 1.0 / (1.0 / c.Rkd + 1.0 / c.RkdShunt + 1.0 / c.RboostBleed);
        return fbEnabled_ ? rk / (rk + c.Rfb) : 0.0;
    }

private:
    void solveOperatingPoint();
    void parkState();
    inline float processSampleOS(float x);
    inline double solveTubePlate(double vg1k, double vg2, double rail, double& vpOut,
                                 double& baseOut) const;
    inline double solveEl34Grid(double vDriveAC, double vDriveQ, double& vCc,
                                double& vgWarm) const;

    double sampleRate_ = 44100.0;
    double osRate_ = 176400.0;
    int maxBlockSize_ = 128;

    Oversampler os_;
    CathodyneInverter inv_;
    El34Params el34_{};

    double hfDrive_ = 0.5;
    double drive_ = 1.0;
    bool fbEnabled_ = true;

    double iqTube_ = 0.0, ig2qTube_ = 0.0;
    double vRailIdle_ = 500.0, vScreenIdle_ = 493.0, vCplusIdle_ = 434.0;
    double vRail_ = 500.0;
    double vScreenUp_ = 493.0, vScreenDown_ = 493.0;
    double gRes_ = 0.0;

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

#endif  // CLIPPER_DSP_ORANGE_POWER_AMP_H
