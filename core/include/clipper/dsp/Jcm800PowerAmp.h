// Clipper — portable DSP core (M9.3).
//
// Jcm800PowerAmp: the Marshall JCM800 2204's 50 W push-pull EL34 POWER SECTION —
// the phase inverter, the EL34 output pair, the output transformer, global
// negative feedback + presence, and the B+ SUPPLY SAG. This is where a cranked
// Marshall's "responsive" character lives: the feedback loop, the presence lift,
// the power-tube crossover/saturation, and the rail droop under pick attack. All
// of it is modelled from circuit physics and MEASURED (docs §18), not vibed.
//
// The composed guitar-in-to-speaker chain (Jcm800Amp) is preamp → this. Convention
// throughout: real circuit VOLTS internally; the process() output is normalized so
// 1.0f == full scale (see kFullScaleSecV — a full-power sine peaks ~0.9). Platform-
// free C++17, zero web/server/electron deps.
//
// ===========================================================================
// 1. PHASE INVERTER — 12AX7 long-tailed pair (LtpInverter)
// ===========================================================================
// The 2204's PI is a 12AX7 long-tailed pair (LTP), NOT expressible by the M9.1
// TriodeStage (which models a single triode with its own cathode — it cannot share
// a tail between two devices). So this is a dedicated `LtpInverter` reusing the
// SAME Koren 12AX7 device law (TriodeStage::korenPlateCurrent) — no new device fit.
//
//   V3A grid ← input (master-volume drive)      Ra1 = 100 k  ┐ asymmetric plate
//   V3B grid ← global feedback (NFB + presence)  Ra2 =  82 k  ┘ loads balance the
//   shared cathodes → Rtail = 10 k to ground (the "long tail")   two output legs
//   B+_PI ≈ 340 V (post-dropping-resistor node, below the main rail)
//
// The tail resistor makes the joined cathodes a soft current source: the two plate
// currents swing ANTI-PHASE, so Va1 and Va2 are the two out-of-phase drives for the
// EL34 grids. As drive rises, ONE triode is steered to cutoff while the other takes
// the whole tail current → the PI's own asymmetric SOFT CLIP, an audible part of the
// cranked-Marshall sound (the 100k/82k imbalance is deliberate — it evens the two
// legs' large-signal gain). Solved per (oversampled) sample as a 3×3 nodal Newton
// (unknowns Va1, Va2, Vk_tail; grids are driven voltages) with an analytic Jacobian
// from the Koren derivatives. PI grid-current blocking is DEFERRED (documented
// simplification): the dominant PI clip is the tail-steering cutoff, which the LTP
// solve captures without it; the modelled blocking mechanism lives on the EL34 grids.
//
// ===========================================================================
// 2. EL34 PUSH-PULL PAIR — Koren pentode
// ===========================================================================
// Koren pentode plate/screen law (Norman Koren, "Improved SPICE models for
// vacuum-tube amplifiers", 1996 — the pentode form):
//
//   E1  = (Vg2/kp)·ln(1 + exp(kp·(1/mu + Vg1k/Vg2)))
//   Ip  = (2/kg1)·E1^ex·atan(Vp/kvb)          [(1+sign)=2 for E1>0; atan = the knee]
//   Ig2 = (2/kg2)·E1^ex                        [screen current, rises with drive]
//
// EL34 parameter fit (DOCUMENTED FIT — a widely-circulated Koren EL34 set; pentode
// fits are looser than triodes, hence the ±10 % validation band):
//   mu = 11 (the g1→g2 amplification factor), ex = 1.35, kg1 = 650, kp = 60,
//   kvb = 24 (plate-knee scale), kg2 = 4200 (screen).
//
// Fixed bias Vg1k_q = −43 V, cathodes grounded (the 2204 is a FIXED-bias amp). With
// this fit that yields ~38 mA/tube quiescent at a ~465 V loaded rail / ~460 V screen
// — 17 W plate dissipation, ~67 % of the EL34's 25 W max: the 2204's design centre.
// (The nominal "−38 V" 2204 figure assumes a slightly different device fit; −43 V is
// what THIS Koren fit needs for the same operating point — derived + asserted in the
// tests, not asserted against a datasheet number.)
//
// Class AB: both tubes idle at 38 mA (not cutoff) so a CROSSOVER region exists and
// is measurable at low drive (the smooth Koren turn-on leaves a slight compression
// through the zero crossing). The up tube sees Vg1k = −43 + Vac, the down tube
// −43 − Vac (anti-phase from the PI). Idiff = Ip_up − Ip_down = f(Vac) − f(−Vac) is
// an ODD function of Vac for a MATCHED pair, so EVEN harmonics cancel — the push-pull
// signature (validated against a single-ended reference). Grid CONDUCTION on the EL34
// grids (soft clamp for Vg1k ≳ 0) charges the PI→EL34 coupling caps and shifts the
// bias toward cutoff on overdrive — the BLOCKING mechanism of M9.1, recovering
// through the 220 k grid leak (τ = Rg·Cc = 4.8 ms): the cranked-Marshall
// "compression/fart". Present and measured.
//
// Power-tube SATURATION: each tube's plate voltage swings Vp = Vrail − (Ip−Iq)·Rpp,
// Rpp = Raa/4 (reflected per-plate load). When the up tube swings its plate down
// toward the knee (kvb), atan(Vp/kvb) collapses → the current compresses → the
// power stage clips. Solved per tube as a 1-D Newton (Ip ↔ Vp coupled through Rpp).
//
// ===========================================================================
// 3. OUTPUT TRANSFORMER — linear v1
// ===========================================================================
// Raa = 3.4 k plate-to-plate reflected load (EL34 pair into the rated speaker load).
// The differential primary voltage is Vpri = Idiff·(Raa/4)·... → the secondary is
// Vpri / n, n = √(Raa/Rload) = √(3400/8) ≈ 20.6. Bandwidth as two documented first-
// order corners: LF ~75 Hz (primary magnetizing inductance high-passes the bottom)
// and HF ~12 kHz (leakage inductance low-passes the top). OT CORE SATURATION is
// EXPLICITLY DEFERRED (documented) — v1 is linear; the nonlinearity lives in the
// tubes. Output normalized to 1.0f == full scale via kFullScaleSecV (full-power
// sine → ~0.9 peak); the NFB tap uses the real secondary volts, not the normalized.
//
// ===========================================================================
// 4. NEGATIVE FEEDBACK + PRESENCE
// ===========================================================================
// Global NFB from the OT secondary back to the PI's V3B grid. The feedback is
// injected as −β·V_secondary at V3B, whose forward path to the secondary is NON-
// inverting (V3A is the inverting input), so the loop OPPOSES the output — negative
// feedback (validated: closed-loop gain is LOWER than open-loop). β is the 2204's
// feedback divider 5k/(5k+47k) = 0.0962 (documented in kFeedbackBeta). Because β
// acts on the REAL secondary VOLTS, the loop gain is β·A_real where A_real is the
// forward VOLTS gain PI-grid→secondary (≈5). Closed-loop gain = A_real/(1+β·A_real),
// a reduction of 20·log10(1+β·A_real) ≈ 3–4 dB vs open loop — asserted in-test with
// A_real MEASURED open-loop (= normalized gain × kFullScaleSecV). The feedback
// carries a UNIT DELAY at the oversampled rate (explicit-loop simplification — the
// loop is decoupled so each sample's forward path solves independently; at OS rate
// the delay is a fraction of a µs and the low loop gain makes it accurate +
// unconditionally stable).
//
// PRESENCE (0..1): the 2204's presence pot + 0.68 µF cap in the feedback leg REDUCE
// the HF feedback → LIFT the highs. Modelled as β(f) = β·[(1−p) + p·L(f)], L = a
// one-pole low-pass (corner ~1.5 kHz): at LF L→1 (feedback = β, presence-independent);
// at HF L→0 (feedback → β(1−p), so p=1 removes HF feedback → max HF lift). The HF
// shelf boost vs the flat (presence-0) response is 20·log10[(1+Aβ)/(1+Aβ(1−p))],
// validated ±1.5 dB against that analytic form.
//
// ===========================================================================
// 5. SAG — the B+ supply rail (the centrepiece)
// ===========================================================================
// The rail is a Thévenin source Vsupply ≈ 480 V (no-load) behind Rsupply ≈ 150 Ω
// (effective supply impedance) with a reservoir cap Creservoir ≈ 50 µF, plus a
// slower screen node (Rscreen 1 k, Cscreen 22 µF). Total plate+screen current draw
// pulls the rail down; the tubes' gain and headroom FOLLOW the rail (and, strongly,
// the screen). A loud burst after silence → an initial peak, then COMPRESSION that
// settles over ~one reservoir RC (τ = Rsupply·Creservoir = 7.5 ms — the "bloom"),
// then RECOVERY after release with the same RC. Rail/screen are integrated backward-
// Euler per oversampled sample from the current draw (the ms time constants dwarf the
// µs step, so the tubes see the previous sample's rail — an accurate decoupling). Sag
// DEPTH at full power lands in the documented 2–6 dB window: the 2204 is a fixed-bias
// solid-state-rectified amp — a comparatively TIGHT amp — so the sag is deliberately
// modest, not overcooked.

#ifndef CLIPPER_DSP_JCM800_POWER_AMP_H
#define CLIPPER_DSP_JCM800_POWER_AMP_H

#include <vector>

#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/TriodeStage.h"

namespace clipper::dsp {

// ---------------------------------------------------------------------------
// EL34 Koren pentode device law. Static so the tests derive the analytic bias /
// crossover / saturation independently (same discipline as korenPlateCurrent).
// ---------------------------------------------------------------------------
struct El34Params {
    double mu = 11.0;     // g1→g2 amplification factor
    double ex = 1.35;     // exponent
    double kg1 = 650.0;   // plate scaling
    double kp = 60.0;     // grid-drive knee
    double kvb = 24.0;    // plate-knee scale (atan)
    double kg2 = 4200.0;  // screen scaling
};

// Plate current (A). Vp = plate-cathode V, Vg1k = control-grid-cathode V, Vg2 =
// screen-cathode V. Cutoff (Ip=0) for E1<=0; atan(Vp/kvb) is the pentode knee.
double el34PlateCurrent(double Vp, double Vg1k, double Vg2, const El34Params& p);
// Screen current (A) — rises with drive; feeds the screen-node sag.
double el34ScreenCurrent(double Vg1k, double Vg2, const El34Params& p);

// ---------------------------------------------------------------------------
// LtpInverter — 12AX7 long-tailed-pair phase inverter (Koren device law).
// Per-sample 3×3 nodal Newton (Va1, Va2, Vk_tail); grids are driven voltages.
// Public + introspectable so the tests validate its DC point and anti-phase gain.
// ---------------------------------------------------------------------------
class LtpInverter {
public:
    struct Config {
        double bPlus = 340.0;   // PI plate supply (V)
        double Ra1 = 100.0e3;   // V3A plate load (Ω)
        double Ra2 = 82.0e3;    // V3B plate load (Ω) — asymmetric for balance
        double Rtail = 10.0e3;  // shared tail resistor to ground (Ω)
        TriodeStage::KorenParams tube{};  // 12AX7
    };

    void configure(const Config& c) { cfg_ = c; }
    const Config& config() const { return cfg_; }
    void prepare();  // solve + park the DC operating point (grids at 0)
    // One sample: grid drives vg1 (input), vg2 (feedback). Fills the two anti-phase
    // plate voltages (absolute volts). Warm-started from the previous sample.
    void processSample(double vg1, double vg2, double& va1, double& va2);

    double quiescentPlate1() const { return va1q_; }
    double quiescentPlate2() const { return va2q_; }
    double quiescentTail() const { return vkq_; }
    double quiescentCurrent1() const { return ip1q_; }

private:
    Config cfg_{};
    double va1_ = 0, va2_ = 0, vk_ = 0;       // live node state (warm start)
    double va1q_ = 0, va2q_ = 0, vkq_ = 0;    // quiescent
    double ip1q_ = 0;
};

// ---------------------------------------------------------------------------
// The composed power section.
// ---------------------------------------------------------------------------
class Jcm800PowerAmp {
public:
    enum ParamId : int {
        PARAM_PRESENCE = 0,   // presence knob (0..1) — HF feedback reduction / lift
        PARAM_DRIVE = 1,      // PI input drive trim (0..1, maps to a gain) — how hard
                              // the master-volume signal hits the phase inverter
        PARAM_COUNT = 2,
    };

    Jcm800PowerAmp();

    // Configure for a sample rate and max block size. Solves the whole DC operating
    // point (PI, EL34 bias, rail/screen idle) and parks all state there.
    void prepare(double sampleRate, int maxBlockSize);

    // Oversampling factor for the power section (1/2/4/8). Default 4 (measured, §18).
    void setOversampling(int factor);
    int oversampling() const { return os_.factor(); }

    void setParameter(int paramId, float value);

    // Enable/disable global NFB (open-loop A/B for the closed-vs-open gain test).
    void setFeedbackEnabled(bool on) { fbEnabled_ = on; }
    bool feedbackEnabled() const { return fbEnabled_; }

    // Process mono audio, in -> out (may alias). Input = PI grid drive in volts
    // (the preamp/master-volume output). Output = normalized (1.0 == full scale).
    void process(const float* in, float* out, int numFrames);

    // --- Introspection (measurement / tests) --------------------------------
    double el34QuiescentPlateCurrent() const { return iqTube_; }  // A / tube
    double el34QuiescentScreenCurrent() const { return ig2qTube_; }
    double railIdle() const { return vRailIdle_; }
    double screenIdle() const { return vScreenIdle_; }
    double railNow() const { return vRail_; }        // live rail (sag readout)
    double screenNow() const { return vScreen_; }
    double lastOutputPeak() const { return lastOutPeak_; }  // normalized
    const El34Params& el34() const { return el34_; }
    const LtpInverter& inverter() const { return ltp_; }

    // Documented constants (cited in the tests / docs §18).
    static constexpr double kVbias = -43.0;        // EL34 fixed bias (V)
    static constexpr double kRaa = 3400.0;         // OT plate-to-plate load (Ω)
    static constexpr double kRppReflected = kRaa / 4.0;  // per-plate reflected (Ω)
    static constexpr double kVsupply = 480.0;      // B+ Thévenin no-load (V)
    static constexpr double kRsupply = 150.0;      // supply impedance (Ω)
    static constexpr double kCreservoir = 50.0e-6; // reservoir cap (F)
    static constexpr double kRscreen = 1000.0;     // screen dropping R (Ω)
    static constexpr double kCscreen = 22.0e-6;    // screen filter cap (F)
    static constexpr double kOtLfHz = 75.0;        // OT LF corner (Hz)
    static constexpr double kOtHfHz = 12000.0;     // OT HF corner (Hz)
    static constexpr double kCoupCc = 22.0e-9;     // PI→EL34 coupling cap (F)
    static constexpr double kCoupRg = 220.0e3;     // EL34 grid leak (Ω)
    // Effective NFB factor: the 2204's feedback divider — the 47 k feedback
    // resistor from the OT secondary tap into the PI, with the 5 k presence pot to
    // ground: β = 5k/(5k+47k) = 0.0962. The loop gain is β·A_real (A_real = the
    // grid→secondary forward VOLTS gain, ≈ 5), giving ~3–4 dB of global feedback —
    // the 2204 is a lightly-fed-back amp, so the loop is modest by design.
    static constexpr double kFeedbackBeta = 0.0962;  // 5k / (5k + 47k)
    static constexpr double kPresenceHz = 1500.0;  // presence shelf corner (Hz)
    // Secondary volts that map to 1.0 full scale. Calibrated (docs §18) against the
    // COMPOSED amp: fully cranked (GAIN 1, MASTER 1, hot input) a power sine peaks
    // ~0.9, a normal full-send (MASTER 0.7) ~0.78 — headroom to 1.0 for transients.
    // The OT secondary reaches ~23 V peak at full cranked output into the rated load.
    static constexpr double kFullScaleSecV = 26.0;

    // Analytic helpers exposed for the tests.
    static double otTurnsRatio() { return 20.616; }  // √(Raa/8)
    double feedbackBeta() const { return fbEnabled_ ? kFeedbackBeta : 0.0; }

private:
    void solveOperatingPoint();
    inline float processSampleOS(float x);
    // Per-tube plate-load Newton: Ip with Vp = rail − (Ip−Iq)·Rpp. Returns Ip;
    // outputs the resolved plate voltage in vpOut.
    inline double solveTubePlate(double vg1k, double vg2, double rail, double& vpOut) const;
    // EL34 grid-node solve (coupling cap + grid leak to bias + grid conduction).
    // vpPlateAC = PI plate AC drive; state vCc updated. Returns Vg1k (grid-cathode).
    inline double solveEl34Grid(double vpPlateAC, double vpPlateQ, double& vCc) const;

    double sampleRate_ = 44100.0;
    double osRate_ = 176400.0;
    int maxBlockSize_ = 128;

    Oversampler os_;
    LtpInverter ltp_;
    El34Params el34_{};

    double presence_ = 0.5;
    double drive_ = 1.0;      // PI input trim gain (mapped from PARAM_DRIVE)
    bool fbEnabled_ = true;

    // Operating point.
    double iqTube_ = 0.0, ig2qTube_ = 0.0;
    double vRailIdle_ = 465.0, vScreenIdle_ = 458.0;

    // Live sag state (backward Euler).
    double vRail_ = 465.0, vScreen_ = 458.0;
    double gRes_ = 0.0, gScr_ = 0.0;  // C/T companion conductances at OS rate

    // PI→EL34 coupling-cap blocking state (per tube) + grid conduction params.
    double vCcUp_ = 0.0, vCcDown_ = 0.0;
    double gCc_ = 0.0, gRg_ = 0.0;
    double gridRgk_ = 1500.0, gridVgn_ = 0.5;  // EL34 grid conduction (harder knee)

    // OT one-pole corners (TPT).
    double otHpA_ = 0.0, otHpS_ = 0.0;   // high-pass coeff + state
    double otLpA_ = 0.0, otLpS_ = 0.0;   // low-pass coeff + state
    // Presence low-pass (feedback shaping) + feedback unit-delay.
    double presLpA_ = 0.0, presLpS_ = 0.0;
    double fbDelay_ = 0.0;               // secondary volts, previous OS sample

    double lastOutPeak_ = 0.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_JCM800_POWER_AMP_H
