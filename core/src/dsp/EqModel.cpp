// EqModel — the "Decade" ten-band graphic EQ. See EqModel.h for the topology,
// the sourced/derived/reconstructed boundary, and the named XFAIL.

#include "clipper/dsp/EqModel.h"

#include <algorithm>
#include <cmath>

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/ParamGuard.h"

namespace clipper::dsp {
namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr double kPi = 3.141592653589793;

// --- Transcribed from the GE-7 netlist (docs §72.1, S5/S6) --------------------
// The summing stage's input and feedback resistors. Equal, so the stage is a
// unity inverter with every slider centred.
constexpr double kRin = 10.0e3;
constexpr double kRf = 10.0e3;
// The band slider: 10 kΩ LINEAR, per the netlist's own annotation. NOT an audio
// taper, and that is sourced rather than assumed — a graphic EQ's slider is
// linear in dB-ish boost by virtue of the topology, so a log taper here would be
// wrong twice over.
constexpr double kRpot = 10.0e3;
// The netlist adds a 1 Ω floor to each half of every pot so a slider at an
// endpoint is not a mathematical short. Carried across verbatim: it is also
// exactly the guard this implementation needs against a divide by zero.
constexpr double kPotFloorOhms = 1.0;

// --- RECONSTRUCTED (§57's rule applies — see the header) ----------------------
// Pinned to the published ±12 dB range.
constexpr double kLegLossOhms = 3215.504;
// Pinned to the published ~1/3-octave width at full boost.
constexpr double kLegZ0Ohms = 26665.5;

// The ten ISO centres the reference's published 31.25 Hz…16 kHz range implies.
constexpr double kBandHz[EqModel::kNumBands] = {31.25, 62.5,  125.0,  250.0,   500.0,
                                                1000.0, 2000.0, 4000.0, 8000.0, 16000.0};

// GAIN and VOLUME: RECONSTRUCTED as ±12 dB dB-linear sliders with 0.5 == unity,
// matching the band range so the whole faceplate reads in one unit. The
// reference's own slider laws are not reachable.
constexpr double kLevelRangeDb = 12.0;

inline double levelKnobToGain(double knob01) {
    return std::pow(10.0, ((knob01 - 0.5) * 2.0 * kLevelRangeDb) / 20.0);
}

// One band leg: C1 in series with the gyrator-simulated L and the lumped loss,
// from the pot wiper to the summing node's virtual ground.
//
// Trapezoidal companions, in DOUBLE. The 31.25 Hz leg at 96 kHz sits at a pole
// radius over 0.998, which is §56.4's exact shape — the float direct-form state
// that put an audible −73 dBFS hiss floor on the GOLD's drive divider in code
// that looked fine and was invisible to every single-bin bar in that suite.
// `testNumericalFloor` measures this against the project's own −120 dBFS gate
// rather than trusting the choice.
struct BandLeg {
    // Fixed by the sample rate and the band's L / C1.
    double zc = 0.0;    // T / (2·C1)
    double zl = 0.0;    // 2·L / T
    double yz = 0.0;    // 1 / (zc + zl + Rs)
    double eCoef = 0.0; // zc - zl

    // Fixed by the slider position.
    double ga = 0.0, gb = 0.0, invD = 0.0;
    double kA = 0.0;    // yz·ga/D — this leg's contribution to the numerator
    double kC = 0.0;    // yz·gb/D — ... and to the denominator
    double kE = 0.0;    // (yz/D)·(ga+gb) — its history term's weight

    // Recursive state. ALL of it rests at exactly zero on silence, so all of it
    // is flushed as a UNIT (§56.4b: the scalar one-liner guards only the newest
    // tap and does not converge above first order).
    double vC = 0.0, vL = 0.0, iPrev = 0.0;

    void reset() { vC = vL = iPrev = 0.0; }

    void setRate(double centreHz, double T) {
        // FREQUENCY PREWARP. The companions below are trapezoidal, i.e. the
        // bilinear map, which compresses the frequency axis toward Nyquist: an
        // analog resonance at f appears at a LOWER digital frequency. Untreated,
        // the 16 kHz band at 48 kHz lands near 12.4 kHz and measures +0.95 dB at
        // its own nominal centre instead of +12 (the first build of this model
        // did exactly that, and BAR 2 caught it).
        //
        // Prewarping is legitimate here rather than a fudge because the per-band
        // L and C1 are DERIVED, not transcribed — the design intent is "resonate
        // at f0", and this is the value of L·C1 that delivers it. Z0 = √(L/C1)
        // is untouched, so the leg's Q is unchanged.
        const double wp = (2.0 / T) * std::tan(kPi * centreHz * T);
        const double L = kLegZ0Ohms / wp;
        const double C1 = 1.0 / (wp * kLegZ0Ohms);
        zc = T / (2.0 * C1);
        zl = 2.0 * L / T;
        yz = 1.0 / (zc + zl + kLegLossOhms);
        eCoef = zc - zl;
    }

    // x is the slider fraction: 0 = wiper at Vout (cut), 1 = wiper at Vin (boost).
    void setPosition(double x) {
        ga = 1.0 / ((1.0 - x) * kRpot + kPotFloorOhms);  // toward Vin
        gb = 1.0 / (x * kRpot + kPotFloorOhms);          // toward Vout
        const double D = ga + gb + yz;
        invD = 1.0 / D;
        kA = yz * ga * invD;
        kC = yz * gb * invD;
        kE = yz * invD * (ga + gb);
    }
};

}  // namespace

struct EqModel::Impl {
    double sr = 48000.0;
    double T = 1.0 / 48000.0;

    BandLeg legs[kNumBands];

    // Slider smoothers. A graphic EQ's sliders move under a mouse and under the
    // assistant, and the leg coefficients are rebuilt from them, so they get the
    // house treatment: smoothed and applied per sample.
    OnePoleSmootherT<double> band[kNumBands];
    OnePoleSmootherT<double> gain, volume;
    // levelKnobToGain is a pow(); these two sliders are parked almost always, so
    // the gain is cached and only recomputed on a move. (Measured in
    // testCpuCost: recomputing both per sample costs ~2.5x the whole model.)
    double gainLin = 1.0, volumeLin = 1.0;

    // Cached node sums, rebuilt only when a slider actually moves.
    double sumA = 0.0, sumC = 0.0;
    bool coeffsDirty = true;

    void rebuild() {
        sumA = 1.0 / kRin;
        sumC = 1.0 / kRf;
        for (int k = 0; k < kNumBands; ++k) {
            sumA += legs[k].kA;
            sumC += legs[k].kC;
        }
        coeffsDirty = false;
    }
};

EqModel::EqModel() : impl_(new Impl) {
    // Defaults in the CONSTRUCTOR, not in prepare(). §64 records the bug this
    // avoids: setting them in prepare() clobbers a setParameter made BEFORE
    // prepare, which is exactly what identical_core_test's reference chain does.
    for (int k = 0; k < kNumBands; ++k) impl_->band[k].setTarget(0.5);
    impl_->gain.setTarget(0.5);
    impl_->volume.setTarget(0.5);
}

EqModel::~EqModel() = default;

void EqModel::prepare(double sampleRate) {
    Impl& d = *impl_;
    d.sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    d.T = 1.0 / d.sr;

    const double smoothSec = 0.008;
    for (int k = 0; k < kNumBands; ++k) {
        d.legs[k].setRate(kBandHz[k], d.T);
        d.legs[k].reset();
        d.band[k].prepare(d.sr, smoothSec);
        d.legs[k].setPosition(sliderToX(d.band[k].value()));
    }
    d.gain.prepare(d.sr, smoothSec);
    d.volume.prepare(d.sr, smoothSec);
    d.gainLin = levelKnobToGain(d.gain.value());
    d.volumeLin = levelKnobToGain(d.volume.value());
    d.coeffsDirty = true;
    d.rebuild();
}

void EqModel::reset() {
    Impl& d = *impl_;
    for (int k = 0; k < kNumBands; ++k) d.legs[k].reset();
}

void EqModel::setBand(int index, float knob01) {
    if (index < 0 || index >= kNumBands) return;
    impl_->band[index].setTarget(clampParam01(knob01));
}

void EqModel::setParameter(int paramId, float value) {
    if (paramId == PARAM_GAIN) {
        impl_->gain.setTarget(clampParam01(value));
    } else if (paramId == PARAM_VOLUME) {
        impl_->volume.setTarget(clampParam01(value));
    } else if (paramId >= PARAM_BAND_BASE && paramId < PARAM_COUNT) {
        setBand(paramId - PARAM_BAND_BASE, value);
    }
    // PARAM_UNUSED_1 and anything out of range: ignored, as the phaser ignores
    // its carried slots.
}

void EqModel::setOversampling(int /*factor*/) {
    // Linear and time-invariant — nothing to oversample. Accepted and ignored so
    // the chain can drive every pedal identically (the phaser precedent, §12).
}

int EqModel::latencySamples() const { return 0; }

double EqModel::bandCentreHz(int index) {
    if (index < 0 || index >= kNumBands) return 0.0;
    return kBandHz[index];
}
double EqModel::legZ0Ohms() { return kLegZ0Ohms; }
double EqModel::legLossOhms() { return kLegLossOhms; }

double EqModel::sliderToX(double knob01) {
    // The slider IS the pot fraction: 10 kΩ linear, per the netlist annotation.
    // No taper is applied and none should be — see the header.
    return knob01 < 0.0 ? 0.0 : (knob01 > 1.0 ? 1.0 : knob01);
}

void EqModel::process(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;
    for (int n = 0; n < numFrames; ++n) {
        // Per-sample slider smoothing; the leg coefficients follow.
        bool moved = false;
        for (int k = 0; k < kNumBands; ++k) {
            if (!d.band[k].settled()) {
                d.legs[k].setPosition(sliderToX(d.band[k].next()));
                moved = true;
            }
        }
        if (moved) d.rebuild();

        if (!d.gain.settled()) d.gainLin = levelKnobToGain(d.gain.next());
        if (!d.volume.settled()) d.volumeLin = levelKnobToGain(d.volume.next());
        const double vin = static_cast<double>(in[n]) * d.gainLin;

        // Each leg's history term, and the summed history contribution.
        double bHist = 0.0;
        double eCache[kNumBands];
        for (int k = 0; k < kNumBands; ++k) {
            BandLeg& g = d.legs[k];
            const double e = g.vC + g.iPrev * g.eCoef - g.vL;
            eCache[k] = e;
            bHist += g.kE * e;
        }

        // One node equation for the summing amp (derivation in the header).
        const double vout = (-vin * d.sumA + bHist) / d.sumC;

        // Push each leg forward from the solved node voltages.
        for (int k = 0; k < kNumBands; ++k) {
            BandLeg& g = d.legs[k];
            const double e = eCache[k];
            const double w = (vin * g.ga + vout * g.gb + e * g.yz) * g.invD;
            const double i = (w - e) * g.yz;
            const double vCNew = g.vC + g.zc * (i + g.iPrev);
            const double vLNew = g.zl * (i - g.iPrev) - g.vL;
            // Whole-state flush: every one of these rests at zero, and a partial
            // guard does not converge above first order (§56.4b).
            g.vC = flushDenormal(vCNew);
            g.vL = flushDenormal(vLNew);
            g.iPrev = flushDenormal(i);
        }

        // The stage inverts; undo it so the pedal is phase-transparent in the
        // chain (a real one has a second inverting stage after it — the GE-7's
        // own output buffer — and a pedal that flips polarity when engaged would
        // be a defect on a board where anything can be reordered).
        out[n] = static_cast<float>(-vout * d.volumeLin);
    }
}

double EqModel::maxAbsRestingState() const {
    const Impl& d = *impl_;
    double m = 0.0;
    for (int k = 0; k < kNumBands; ++k) {
        const BandLeg& g = d.legs[k];
        m = std::max(m, std::abs(g.vC));
        m = std::max(m, std::abs(g.vL));
        m = std::max(m, std::abs(g.iPrev));
    }
    return m;
}

}  // namespace clipper::dsp
