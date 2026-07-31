// Clipper — WahModel (post-v1.1: the first FILTER pedal). See WahModel.h for the
// topology and docs §58 for the research, the sources and every derivation.
//
// ---------------------------------------------------------------------------
// Chosen numbers and why (house documentation style — every constant either
// comes off a published component list, or is pinned to a published measurement,
// or is measured from the model itself; there is exactly ONE fitted number in the
// whole file and it is a pot taper, whose fitted value is checked against the
// published pot-taper spec).
//
//   kInductanceH = 0.5 H, kTankCapF = 10 nF
//     The published GCB-95 tank: "inductance values between 200mH to 1H (500mH
//     typ.) can be used" and C = 0.01 uF (ElectroSmash). NOT fitted. Their
//     resonance
//         kBareTankHz = 1/(2*pi*sqrt(L*C)) = 2250.7908 Hz
//     is the TOE end of the sweep (the bootstrap feeds back nothing there), and
//     it lands +1.57 % from the CCRMA MEASURED toe of 2216.06 Hz. Two independent
//     routes — a component list and a measurement of a real pedal — agreeing to
//     1.6 % is the evidence that the mechanism below is the right one.
//
//   kHeelHz = 450 Hz -> kApparentCapSpan = (kBareTankHz/450)^2 - 1 = 24.0175762
//     The published heel-down centre; ElectroSmash, Catalinbread and Dunlop's own
//     Cry Baby From Hell spec (440 Hz) all agree within 2 %. It fixes the span of
//     the apparent-capacitance multiplication: heel multiplies C by 25.018x.
//
//   kTaperBeta = 23.537247      <-- THE ONE FITTED NUMBER
//     The pot's wiper law, u(p) = (beta^(1-p) - 1)/(beta - 1), least-squared in
//     ln(f) against the CCRMA measured fit over the whole travel. Its honesty
//     check: u(0.5) = 0.17090, i.e. 17.1 % of full pot resistance at half
//     rotation — inside the documented audio/log-taper spec (an A-taper reads
//     ~10 % at 50 %; the GCB-95's "Hot Potz" is a log / custom-audio part). The
//     one fitted number came out being a pot taper. rms error 0.46 %, worst
//     +1.57 %.
//
//   kTankLoadOhms = 20273.5, kR7StockOhms = 33 k
//     Q = Rp/(2*pi*f0*L) with Rp FIXED and C varying — the parallel-RLC law, which
//     gives BW ~ f0^2 (exponent exactly 2.000 against the reference's measured
//     1.870) and Q ~ 1/f0. Fitting the single constant Q*f0 to the reference gives
//     Rp = 12558.32 ohm at the stock R7 = 33 k, hence the rest of the loading,
//     kTankLoadOhms = (1/12558.32 - 1/33000)^-1 = 20273.5 ohm. The +-11 % Q error
//     at the two ends is the exponent difference and is REPORTED (docs §58.3),
//     not fitted away.
//
//   kR7LoOhms/kR7HiOhms = 10.89 k / 100 k
//     ElectroSmash's documented "Vocal Mod" as a knob (R7 33 k -> 39 k/68 k/100 k
//     for a more vocal peak; lower spreads the bell). Log-centred so VOICE 0.5 is
//     EXACTLY the stock 33.000 k. Moves width only.
//
//   kPeakBoostDb = 18 dB
//     "These frequencies are boosted at somewhere around 18dB while everything
//     above and below is rolled off in a bell curve" — quoted at BOTH ends of the
//     sweep, which is also what this topology predicts (the resonant gain is set
//     by resistors that do not move). Constant peak height is a TEST BAR here,
//     not an assumption.
//
//   BjtStage config: Vcc 9 V, Rc 22 k, Re 390, Rf 470 k, Rbg 82 k, Cin 10 nF
//     The GCB-95's MPSA18-class common-emitter output stage (values from the
//     ElectroSmash designator sets; the letters differ between mirrors, the values
//     do not — docs §58.1). betaF 500 is the MPSA18's published high-hFE class.
//     No feedback cap: none is documented on this stage.
//
//   kTankDivider = 10^(kPeakBoostDb/20) / G0        <-- MEASURED, not fitted
//     G0 is the transistor stage's own small-signal gain, measured from THIS model
//     in prepare() with a 1 kHz probe. The divider is then forced by the published
//     closed-loop +18 dB, so the pedal's small-signal resonant boost is the
//     published figure BY CONSTRUCTION and where the stage starts to bark is a
//     prediction rather than a knob.
//
//   kEnvAttackSec = 0.010, kEnvReleaseSec = 1/6
//     Geofex (Mark Hammer), "The Technology of Auto-Wahs": a typical detector
//     yields "an attack time of about 10 ms and decay of around 500 ms", and
//     commercial units "respond with maximum swing over a period of 50 msec or
//     less, and drift back to baseline over a period of 500 msec or less".
//     3*tau = 500 ms => tau = 166.7 ms.
//
//   kEnvRefVolts = 0.25 V
//     The envelope is normalised through a fixed reference so a normally picked
//     note opens the filter usefully; 0.25 V sits between the house clean probe
//     (0.10 V) and the house loud probe (0.30 V) — docs §11.1 input calibration.
//     The acceptance number is the MEASURED octave excursion of a real pluck
//     (docs §58.6), not this constant.
// ---------------------------------------------------------------------------

#include "clipper/dsp/WahModel.h"

#include "clipper/dsp/BjtStage.h"
#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/ParamGuard.h"

#include <algorithm>
#include <cmath>

namespace clipper::dsp {

namespace {
constexpr double kPi = 3.141592653589793;

// --- The tank (published component values; NOT fitted) ----------------------
constexpr double kInductanceH = 0.5;      // 500 mH — GCB-95 typ.
constexpr double kTankCapF = 1.0e-8;      // 0.01 uF

// --- The sweep law ----------------------------------------------------------
constexpr double kHeelHz = 450.0;         // published heel-down centre
constexpr double kTaperBeta = 23.537247;  // THE one fitted number (a pot taper)

// --- The Q law --------------------------------------------------------------
constexpr double kTankLoadOhms = 20273.5079;  // the non-R7 loading (derived)
constexpr double kR7StockOhms = 33.0e3;       // published stock value
constexpr double kR7HiOhms = 100.0e3;         // published "vocal mod" ceiling
// Log-centred on the stock value: sqrt(kR7LoOhms*kR7HiOhms) == kR7StockOhms.
constexpr double kR7LoOhms = kR7StockOhms * kR7StockOhms / kR7HiOhms;  // 10.89 k

// --- Level ------------------------------------------------------------------
constexpr double kPeakBoostDb = 18.0;

// --- The envelope follower --------------------------------------------------
constexpr double kEnvAttackSec = 0.010;
constexpr double kEnvReleaseSec = 1.0 / 6.0;   // 3*tau = 500 ms
constexpr double kEnvRefVolts = 0.25;

// --- Housekeeping -----------------------------------------------------------
constexpr double kSmoothSeconds = 0.008;        // knob smoothing (house ~5-8 ms)
constexpr double kPositionPoleSeconds = 0.004;  // x2 cascaded -> ~8 ms overall
constexpr double kDefaultPosition = 0.35;
constexpr int kDefaultOversampling = 4;
constexpr double kProbeHz = 1000.0;        // small-signal gain probe
constexpr double kProbeVolts = 1.0e-3;     // 1 mV — deep in the linear region

// The bare LC resonance of the published tank (Hz). constexpr-friendly form is
// not available for sqrt in C++17 on every compiler, so it is a function.
double bareTankHzImpl() {
    return 1.0 / (2.0 * kPi * std::sqrt(kInductanceH * kTankCapF));
}

// The pot's normalised wiper law: 1 at the heel, 0 at the toe. Standard log-pot
// family; kTaperBeta is the fitted exponent (see the banner).
double wiperLawImpl(double p) {
    const double q = clampParam01(p);
    return (std::pow(kTaperBeta, 1.0 - q) - 1.0) / (kTaperBeta - 1.0);
}

// Apparent-capacitance multiplication, M(p) = Ceff/C.
double apparentCapMult(double p) {
    const double fLC = bareTankHzImpl();
    const double A = (fLC / kHeelHz) * (fLC / kHeelHz) - 1.0;
    return 1.0 + A * wiperLawImpl(p);
}
}  // namespace

// ---------------------------------------------------------------------------
// The derived laws, exposed statically so the tests compare RENDERS against them
// rather than against a copy of them.
// ---------------------------------------------------------------------------

double WahModel::bareTankHz() { return bareTankHzImpl(); }

double WahModel::wiperLaw(double position01) { return wiperLawImpl(position01); }

double WahModel::positionToHz(double position01) {
    return bareTankHzImpl() / std::sqrt(apparentCapMult(position01));
}

double WahModel::voiceToR7Ohms(double voice01) {
    const double v = clampParam01(voice01);
    return kR7LoOhms * std::pow(kR7HiOhms / kR7LoOhms, v);
}

double WahModel::resonanceQ(double position01, double voice01) {
    const double r7 = voiceToR7Ohms(voice01);
    const double rp = (r7 * kTankLoadOhms) / (r7 + kTankLoadOhms);
    const double f0 = positionToHz(position01);
    return rp / (2.0 * kPi * f0 * kInductanceH);
}

double WahModel::peakBoostDb() { return kPeakBoostDb; }

// The CCRMA / Julius Smith digitised CryBaby (Faust vaeffects.lib `crybaby`),
// fitted to three MEASURED GCB-95 responses. Stated here ONCE so a test can
// validate against it without restating it, and so a future re-derivation cannot
// silently drift away from what it was checked against.
double WahModel::referenceHz(double position01) {
    return 450.0 * std::pow(2.0, 2.3 * clampParam01(position01));
}
double WahModel::referenceQ(double position01) {
    return std::pow(2.0, 2.0 * (1.0 - clampParam01(position01)) + 1.0);
}

// ---------------------------------------------------------------------------

struct WahModel::Impl {
    double sampleRate = 44100.0;
    int maxBlock = 128;

    // POSITION is smoothed by TWO cascaded 4 ms one-poles (same ~8 ms overall
    // response, 2nd-order stop-band). WHY, honestly: it was added on the
    // hypothesis that the −75.0 dB far-field floor a fast sweep leaves was the
    // host's per-BLOCK control staircase feeding the smoother a 750 Hz-stepped
    // target. THE MEASUREMENT REFUTED THAT — re-aiming POSITION every 64 samples,
    // every 8, and every SINGLE sample all floor at −68.8/−75.0/−79.0/−81.3 dB in
    // the 3-6/6-10/10-14/14-20 kHz bands, identically. The floor is the discrete
    // resonator's own time-varying residual (it decays with frequency, and drops
    // 8.7-14.4 dB at 96 kHz — both signatures of discretisation, neither of
    // stepping), not the control path. The second pole is KEPT anyway because it
    // does improve the pathological per-block 0<->1 slam by 23.7 dB (−80.6 →
    // −104.3) for one extra multiply-add. A static POSITION floors at −322.9 dB,
    // which is what proves none of this is the resonator at rest.
    // SENSITIVITY and VOICE keep one pole — neither modulates the resonance
    // sharply enough to matter.
    OnePoleSmootherD position;     // treadle position (0..1), pole 1 of 2
    OnePoleSmootherD position2;    // pole 2 of 2
    OnePoleSmootherD sensitivity;  // 0 = manual pedal
    OnePoleSmootherD voice;        // the "vocal mod" width knob

    // --- TPT state-variable resonator (docs §58.4). The two integrator states
    // ARE the physical tank variables; both rest at ZERO, so both are guarded,
    // and because this is a SECOND-order recursion the flush is applied to the
    // WHOLE STATE as a unit (docs §56.4b — the house one-liner does not converge
    // above first order).
    double s1 = 0.0, s2 = 0.0;

    // --- Envelope follower (its own; NOT shared with the parallel compressor
    // slice's — see docs §58.5).
    double env = 0.0;          // rests at zero -> guarded
    double envAttackCoef = 0.0;
    double envReleaseCoef = 0.0;

    // --- Output stage ---------------------------------------------------------
    BjtStage stage;
    Oversampler os;
    double stageGain = 1.0;    // G0, MEASURED in prepare()
    double tankDivider = 1.0;  // 10^(18/20)/G0

    // Live readouts for the measurement hooks.
    double curPosition = 0.0;
    double curCentreHz = kHeelHz;

    double peakGainLin() const { return std::pow(10.0, kPeakBoostDb / 20.0); }

    // Configure + DC-solve the transistor stage at the current oversampled rate,
    // MEASURE its small-signal gain, and derive the tank's insertion divider from
    // it. Split out of prepare() so a later setOversampling() can redo exactly
    // this WITHOUT snapping the user's knobs back to their defaults.
    void prepareStage() {
        BjtStage::Config cfg;
        cfg.Vcc = 9.0;
        cfg.Rc = 22.0e3;
        cfg.Re = 390.0;
        cfg.Rf = 470.0e3;
        cfg.Rbg = 82.0e3;
        // In the real pedal the tank's OWN capacitor DC-blocks the base — there is
        // no second coupling high-pass between filter and transistor. BjtStage's
        // topology requires a Cin, so it is set two decades below the band and out
        // of the way. This is NOT cosmetic: at the first value tried (10 nF) the
        // stage's input network measured a ~1.1 kHz corner sitting in the MIDDLE of
        // the sweep, which tilted the pedal's resonant peak by 9.6 dB across the
        // travel (12.18 dB at the heel, 21.74 at the toe) and pushed the measured
        // centre frequency +2.82 % out at the toe. 1 µF puts the corner at ~11 Hz;
        // the constant-peak-height bar in the tests is what holds this honest.
        cfg.Cin = 1.0e-6;
        cfg.Cf = 0.0;           // no feedback cap is documented on this stage
        cfg.Rs = 0.0;
        cfg.bjt.betaF = 500.0;  // MPSA18 published high-hFE class
        stage.configure(cfg);

        const double osRate = sampleRate * static_cast<double>(os.factor());
        stage.prepare(osRate);

        // MEASURE the stage's small-signal gain, then hand the result to the
        // divider. 1 mV at 1 kHz is deep inside the linear region; two cycles
        // settle the coupling cap's transient, four more are measured.
        const int perCycle = std::max(4, static_cast<int>(osRate / kProbeHz));
        const int warm = 2 * perCycle;
        const int meas = 4 * perCycle;
        double peak = 0.0;
        for (int i = 0; i < warm + meas; ++i) {
            const double t = static_cast<double>(i) / osRate;
            const float y = stage.processSample(
                static_cast<float>(kProbeVolts * std::sin(2.0 * kPi * kProbeHz * t)));
            if (i >= warm) peak = std::max(peak, std::fabs(static_cast<double>(y)));
        }
        stageGain = peak > 0.0 ? peak / kProbeVolts : 1.0;
        stage.reset();
        tankDivider = peakGainLin() / stageGain;
    }

    // Both integrator states flushed as a UNIT: if either has decayed below the
    // floor while the other has not, zeroing only one lets the recursion re-inject
    // the survivor (docs §56.4b).
    void flushResonatorState() {
        const double m = std::max(std::fabs(s1), std::fabs(s2));
        if (m < static_cast<double>(kDenormalFloor)) {
            s1 = 0.0;
            s2 = 0.0;
        }
    }

    // One TPT-SVF sample. Returns the UNITY-PEAK bandpass output (2R*bp), so the
    // peak height is decoupled from Q by construction.
    double resonate(double x, double g, double twoR) {
        const double denom = 1.0 + twoR * g + g * g;
        const double hp = (x - (twoR + g) * s1 - s2) / denom;
        const double bp = g * hp + s1;
        s1 = g * hp + bp;
        const double lp = g * bp + s2;
        s2 = g * bp + lp;
        flushResonatorState();
        return twoR * bp;
    }
};

WahModel::WahModel() : impl_(std::make_unique<Impl>()) {}
WahModel::~WahModel() = default;

void WahModel::prepare(double sampleRate, int maxBlockSize) {
    Impl& d = *impl_;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    d.maxBlock = maxBlockSize > 0 ? maxBlockSize : 128;

    d.position.prepare(kPositionPoleSeconds, d.sampleRate);
    d.position2.prepare(kPositionPoleSeconds, d.sampleRate);
    d.sensitivity.prepare(kSmoothSeconds, d.sampleRate);
    d.voice.prepare(kSmoothSeconds, d.sampleRate);
    d.position.setImmediate(kDefaultPosition);
    d.position2.setImmediate(kDefaultPosition);
    d.sensitivity.setImmediate(0.0);
    d.voice.setImmediate(0.5);

    d.envAttackCoef = 1.0 - std::exp(-1.0 / (kEnvAttackSec * d.sampleRate));
    d.envReleaseCoef = 1.0 - std::exp(-1.0 / (kEnvReleaseSec * d.sampleRate));

    d.os.prepare(d.maxBlock);
    d.os.setFactor(kDefaultOversampling);

    // The GCB-95's common-emitter output stage. Collector feedback (Rf) plus the
    // base-to-ground leg (Rbg) is what puts the collector at a real mid-rail
    // operating point instead of the ~1 V that Rf alone would give.
    d.prepareStage();

    reset();
}

void WahModel::reset() {
    Impl& d = *impl_;
    d.position.reset();
    d.position2.setImmediate(d.position.value());
    d.sensitivity.reset();
    d.voice.reset();
    d.s1 = 0.0;
    d.s2 = 0.0;
    d.env = 0.0;
    d.os.setFactor(d.os.factor());  // clears every halfband delay line
    d.stage.reset();
    d.curPosition = d.position.value();
    d.curCentreHz = positionToHz(d.curPosition);
}

void WahModel::setPosition(float knob01) {
    impl_->position.setTarget(static_cast<double>(clampParam01(knob01)));
}
void WahModel::setSensitivity(float knob01) {
    impl_->sensitivity.setTarget(static_cast<double>(clampParam01(knob01)));
}
void WahModel::setVoice(float knob01) {
    impl_->voice.setTarget(static_cast<double>(clampParam01(knob01)));
}

void WahModel::setParameter(int paramId, float value) {
    switch (paramId) {
        case PARAM_POSITION: setPosition(value); break;
        case PARAM_SENSITIVITY: setSensitivity(value); break;
        case PARAM_VOICE: setVoice(value); break;
        default: break;
    }
}

void WahModel::setOversampling(int factor) {
    Impl& d = *impl_;
    const int before = d.os.factor();
    d.os.setFactor(factor);
    if (d.os.factor() != before) {
        // The transistor stage runs at the OVERSAMPLED rate, so a factor change
        // re-prepares that stage (and therefore re-measures its gain). It does NOT
        // touch the knobs — that is why prepareStage() is split out of prepare().
        d.prepareStage();
        reset();
    }
}

int WahModel::latencySamples() const { return impl_->os.latencySamples(); }

double WahModel::currentPosition() const { return impl_->curPosition; }
double WahModel::currentCentreHz() const { return impl_->curCentreHz; }
double WahModel::currentEnvelope() const { return impl_->env; }
double WahModel::outputStageGain() const { return impl_->stageGain; }
double WahModel::tankDivider() const { return impl_->tankDivider; }

double WahModel::maxAbsRestingState() const {
    const Impl& d = *impl_;
    double m = std::fabs(d.s1);
    m = std::max(m, std::fabs(d.s2));
    m = std::max(m, std::fabs(d.env));
    return m;
}

void WahModel::process(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;
    if (numFrames <= 0) return;

    // The Oversampler's scratch is sized for maxBlock in prepare(), so a host (or
    // an offline render) handing us more than that is chunked here rather than
    // asserting. Chunking is exact — every stage is sample-recursive.
    if (numFrames > d.maxBlock) {
        int off = 0;
        while (off < numFrames) {
            const int n = std::min(d.maxBlock, numFrames - off);
            process(in + off, out + off, n);
            off += n;
        }
        return;
    }

    const double fs = d.sampleRate;
    const double nyq = 0.45 * fs;
    const int factor = d.os.factor();

    // Pass 1 (BASE RATE): the envelope follower and the linear resonator. The
    // coefficients are rebuilt EVERY SAMPLE from the smoothed knobs and the
    // envelope — no block stepping, so a fast sweep leaves a clean floor.
    for (int i = 0; i < numFrames; ++i) {
        const double x = static_cast<double>(in[i]);

        // --- Envelope follower: one-pole peak detector on |x|, asymmetric.
        const double rect = std::fabs(x);
        const double coef = (rect > d.env) ? d.envAttackCoef : d.envReleaseCoef;
        d.env = flushDenormal(d.env + coef * (rect - d.env));

        // --- Control law: SENS = 0 makes this EXACTLY the manual pedal.
        d.position2.setTarget(d.position.next());
        const double pos = d.position2.next();
        const double sens = d.sensitivity.next();
        const double voice = d.voice.next();
        const double envNorm = std::min(1.0, d.env / kEnvRefVolts);
        double posEff = pos + (1.0 - pos) * sens * envNorm;
        posEff = posEff < 0.0 ? 0.0 : (posEff > 1.0 ? 1.0 : posEff);
        d.curPosition = posEff;

        double f0 = positionToHz(posEff);
        if (f0 > nyq) f0 = nyq;
        d.curCentreHz = f0;

        const double q = resonanceQ(posEff, voice);
        const double g = std::tan(kPi * f0 / fs);
        const double twoR = 1.0 / q;

        // Unity-peak bandpass -> the tank's insertion divider. The transistor
        // stage's own measured gain G0 then brings the resonance back up to
        // EXACTLY the published +18 dB, because kTankDivider = 10^(18/20)/G0.
        out[i] = static_cast<float>(d.resonate(x, g, twoR) * d.tankDivider);
    }

    // Pass 2 (OVERSAMPLED): the transistor. Nonlinear, so it lives inside the
    // oversampled domain like every other nonlinear stage in the project.
    d.os.upsample(out, numFrames);
    float* w = d.os.buffer();
    const int n = numFrames * factor;
    for (int i = 0; i < n; ++i) w[i] = d.stage.processSample(w[i]);
    d.os.downsample(out, numFrames);
}

}  // namespace clipper::dsp
