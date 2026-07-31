// Clipper — portable DSP core (M10.3, docs §57).
//
// OrangePowerAmp: the back half of an early-70s Orange OR120 — the DC-coupled
// driver + CATHODYNE phase inverter, FOUR EL34s in push-pull, the output
// transformer, global negative feedback with the HF DRIVE control, and the B+
// supply. ~120 W.
//
// The EL34 device law, the per-tube plate-load Newton, the coupling/blocking grid
// solve, the OT bandwidth pair and the rail/screen sag integrator are REUSED from
// Jcm800PowerAmp (docs §18) — same fit, same numerics, no new device model. What
// is different is everything around them, and that is the point of the voice.
//
// ===========================================================================
// 1. THE INVERTER IS A CATHODYNE, NOT A LONG-TAILED PAIR
// ===========================================================================
// Sourced, and per the players the single biggest reason an Orange does not sound
// like a Marshall: "the cathodyne phase inverter into EL34 output tubes is key to
// the Orange sound, and it distorts differently than the long-tail pair of most
// other guitar amps ... stiffer, punchier, fuzzier."
//
//   driver V2A          cathodyne V2B
//   grid <- preamp      grid <- V2A PLATE, DIRECT-COUPLED (sourced)
//   Ra 300k             Ra = Rk = 180k (equal split loads), B+ 400 V
//   Rk 1.5k  <- NFB     plate out = B+ - Vk   (anti-phase)
//                       cathode out = Vk      (in phase)
//
// Consequences the model gets FOR FREE, which an LTP has to be fitted for:
//   * The two legs are EXACTLY anti-phase and EXACTLY equal amplitude, because
//     both are read off ONE plate current through TWO equal resistors. There is
//     no Ra1/Ra2 balance constant to calibrate (contrast Jcm800PowerAmp's Ra2,
//     which took audit finding 8 and a sweep to land at 120 k). The leg-balance
//     probe is asserted at ~1.000 as a TOPOLOGICAL property.
//   * The stage has voltage gain < 1, so all the drive has to come from the
//     driver — hence its unusually large 300 k plate load.
//   * It clips on COMPLIANCE: Vk cannot go below 0 nor much above B+/2, and the
//     two limits are reached asymmetrically. That is the "fuzzier" clip, and it
//     falls out of the solve rather than being shaped.
//
// The driver plate node IS the cathodyne grid node (direct coupling), so the two
// tubes are solved TOGETHER as one 3x3 nodal Newton in (Vpd, Vkd, Vkc) — the
// same shape as the LTP's, different circuit.
//
// DOCUMENTED SIMPLIFICATION: cathodyne GRID CONDUCTION is not modelled (the
// dominant clip here is compliance, and the EL34 grids carry the blocking
// mechanism as on the 2204). Named follow-up in docs §57.
//
// ===========================================================================
// 2. FOUR EL34s, FIXED BIAS, SOLID-STATE RECTIFIER
// ===========================================================================
// Two tubes per side. Each side's reflected load is Raa/4, shared by two tubes, so
// each TUBE sees Raa/2 (kRppPerTube) and the differential primary voltage is
// (2*Ip_up - 2*Ip_down)*(Raa/4). Raa = 1.7 k for a quad EL34 at this rail.
//
// The rectifier is a solid-state BRIDGE (sourced), not a valve, so the supply is
// STIFF: kRsupply 70 ohm behind a 100 uF reservoir, against the 2204's 150 ohm /
// 50 uF. Double the tubes pulling through half the impedance is what "stiff and
// punchy" means as a number; the sag that remains is real but modest, and it is
// NOT where this amp's compression comes from (the cathodyne and the EL34 grids
// are).
//
// ===========================================================================
// 3. GLOBAL NFB RETURNS TO THE DRIVER'S CATHODE — AND HF DRIVE LIVES THERE
// ===========================================================================
// Sourced: "on Orange amps that use a cathodyne phase inverter this point is at
// the top of the cathode resistor of the tube BEFORE the phase inverter. The
// feedback tap is connected to the presence control."
//
// So the loop encloses the driver AND the inverter (the 2204's loop starts at the
// PI's second grid and encloses only the PI). Injection is at the cathode node
// through kRfb into kRkDriver: raising the cathode lowers Vgk, which opposes a
// positive grid signal — negative feedback, and the closed-vs-open-loop gain
// assert in the test is what pins the sign.
//
// HF DRIVE (0..1) is this amp's presence: it REDUCES the HF content of the
// feedback, so the highs come up. beta(f) = beta*[(1-p) + p*L(f)], L a one-pole
// low-pass at kHfDriveHz. p = 0 is full (dark) feedback, p = 1 lifts the top.
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
// CathodyneInverter — the DC-coupled driver + split-load pair, solved as one
// 3x3 nodal Newton in (Vpd, Vkd, Vkc). Grids are driven voltages: vg is the
// preamp signal at the driver grid, vfb is the (already presence-shaped) global
// feedback voltage presented to kRfb.
//
// Residuals:
//   r1: (B+d - Vpd)/Rad - Ipd                              [driver plate node]
//   r2: Ipd - Vkd/Rkd - (Vkd - vfb)/Rfb                    [driver cathode node]
//   r3: Ipc - Vkc/R                                        [cathodyne cathode]
// with Ipd = koren(Vpd - Vkd, vg - Vkd) and
//      Ipc = koren(B+c - 2*Vkc, Vpd - Vkc)   [plate node = B+c - Ipc*R = B+c - Vkc]
//
// The cathodyne grid draws no current (see the header's documented
// simplification), which is what keeps the driver plate node a two-element KCL.
// ---------------------------------------------------------------------------
class CathodyneInverter {
public:
    struct Config {
        double bPlusDriver = 320.0;   // driver plate supply (V)
        double Rad = 300.0e3;         // driver plate load (Ohm) — large, because the
                                      // cathodyne has no gain and the driver must
                                      // also sit LOW enough to DC-couple to it
        double Rkd = 1.5e3;           // driver cathode resistor (Ohm)
        double Rfb = 27.0e3;          // global feedback return into that cathode
        double bPlusCathodyne = 400.0;  // split-load supply (V)
        double Rsplit = 180.0e3;      // Ra == Rk, the equal split loads (Ohm)
        TriodeStage::KorenParams tube{};  // 12AX7, both triodes
    };

    void configure(const Config& c) { cfg_ = c; }
    const Config& config() const { return cfg_; }
    void prepare();   // solve + park the DC operating point (grid and vfb at 0)
    void reset() { vpd_ = vpdq_; vkd_ = vkdq_; vkc_ = vkcq_; }

    // One sample. Returns the two anti-phase ABSOLUTE node voltages:
    //   vPlate = the cathodyne PLATE  (B+c - Vkc)
    //   vCath  = the cathodyne CATHODE (Vkc)
    void processSample(double vg, double vfb, double& vPlate, double& vCath);

    double quiescentDriverPlate() const { return vpdq_; }
    double quiescentDriverCathode() const { return vkdq_; }
    double quiescentDriverCurrent() const { return ipdq_; }
    double quiescentCathodyneCathode() const { return vkcq_; }
    double quiescentCathodynePlate() const { return cfg_.bPlusCathodyne - vkcq_; }
    double quiescentCathodyneCurrent() const { return ipcq_; }
    // The LIVE driver plate node (== the cathodyne grid, they are the same node).
    // Exposed so a test can separate the driver's gain from the split load's,
    // which is the property that says "this really is a cathodyne".
    double driverPlateNow() const { return vpd_; }

private:
    Config cfg_{};
    double vpd_ = 0, vkd_ = 0, vkc_ = 0;      // live node state (warm start)
    double vpdq_ = 0, vkdq_ = 0, vkcq_ = 0;   // quiescent
    double ipdq_ = 0, ipcq_ = 0;
};

// ---------------------------------------------------------------------------
// The composed OR120 power section.
// ---------------------------------------------------------------------------
class OrangePowerAmp {
public:
    enum ParamId : int {
        PARAM_HF_DRIVE = 0,  // 0..1 — presence (HF feedback reduction / lift)
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
    double screenNow() const { return vScreen_; }
    double lastOutputPeak() const { return lastOutPeak_; }
    const El34Params& el34() const { return el34_; }
    const CathodyneInverter& inverter() const { return inv_; }

    // Documented constants (cited in the tests / docs §57).
    static constexpr double kVbias = -48.0;         // EL34 fixed bias (V)
    static constexpr double kRaa = 1700.0;          // OT plate-to-plate load (Ohm)
    // Two tubes share each side's Raa/4, so per TUBE the reflected load is Raa/2.
    static constexpr double kRppPerTube = kRaa / 2.0;
    static constexpr int kTubes = 4;                // the quad
    static constexpr double kVsupply = 510.0;       // B+ Thevenin no-load (V)
    static constexpr double kRsupply = 70.0;        // SOLID-STATE bridge: stiff
    static constexpr double kCreservoir = 100.0e-6;
    static constexpr double kRscreen = 470.0;
    static constexpr double kCscreen = 47.0e-6;
    // A 120 W output transformer has far more primary inductance than the 2204's
    // 50 W unit, so its LF corner sits LOWER (45 Hz vs 75 Hz) — one of the circuit
    // reasons for the thick bottom end, and measured as such in the tests.
    static constexpr double kOtLfHz = 45.0;
    static constexpr double kOtHfHz = 14000.0;
    static constexpr double kCoupCc = 22.0e-9;      // inverter -> EL34 coupling
    static constexpr double kCoupRg = 220.0e3;      // EL34 grid leak
    static constexpr double kHfDriveHz = 2200.0;    // HF DRIVE shelf corner (Hz)
    // Secondary volts that map to 1.0 full scale. DERIVED by measurement on the
    // composed amp (docs §57, the §23 convention: every voice is normalized to its
    // own cranked peak so the voices stay level-comparable). Cranked (VOLUME 1.0,
    // 0.50 V peak in, 220 Hz) the model's secondary reaches 45.65 V peak = 130 W
    // into 8 ohm, i.e. the OR120 genuinely makes its rated 120 W; 45.65 / 0.90 =
    // 50.7, and the cranked peak measures 0.9004 at that value. The NFB tap uses
    // the REAL secondary volts, never this, so the loop gain is independent of it.
    static constexpr double kFullScaleSecV = 50.7;

    static double otTurnsRatio() { return 14.5774; }  // sqrt(kRaa/8)
    // The feedback divider, for reporting: the real loop is the cathode-node solve
    // (kRfb into kRkDriver), not this scalar.
    double feedbackDivider() const {
        const auto& c = inv_.config();
        return fbEnabled_ ? c.Rkd / (c.Rkd + c.Rfb) : 0.0;
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
    double vRailIdle_ = 500.0, vScreenIdle_ = 493.0;
    double vRail_ = 500.0, vScreen_ = 493.0;
    double gRes_ = 0.0, gScr_ = 0.0;

    double vCcUp_ = 0.0, vCcDown_ = 0.0;
    double vgUp_ = kVbias, vgDown_ = kVbias;
    double gCc_ = 0.0, gRg_ = 0.0;
    double gridRgk_ = 1500.0, gridVgn_ = 0.5;

    double otHpA_ = 0.0, otHpS_ = 0.0;
    double otLpA_ = 0.0, otLpS_ = 0.0;
    double hfLpA_ = 0.0, hfLpS_ = 0.0;
    double fbDelay_ = 0.0;

    double lastOutPeak_ = 0.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_ORANGE_POWER_AMP_H
