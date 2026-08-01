// Clipper — portable DSP core.
//
// GateModel implementation. Sources, gaps and every derived number are in
// docs §61; this file carries the component values and the discretization.
//
// ============================================================================
//  THE RECONSTRUCTION THIS FILE ENCODES  (docs §61.1 — read the gap list)
// ============================================================================
// Sourced (Boss's own product copy, the NS-2 owner's manual, and one forum
// circuit description):
//
//   * a VCA plus a "high-speed envelope-detecting circuit", with the expander
//     acting when the instrument's volume falls BELOW the threshold level;
//   * TWO knobs — THRESHOLD and DECAY — plus a MODE switch that changes what the
//     footswitch does rather than what the audio does (docs §61.3);
//   * the sidechain is a GAIN STAGE feeding "various stages with diodes" whose
//     control signal reaches the VCA's control pin. So the detector really is a
//     diode rectifier behind a gain stage, which is exactly what M13.1's
//     detector is.
//
// NOT sourced, and therefore chosen here under stated constraints:
//
//   * every component value (no schematic was reachable);
//   * the threshold range in dBV, the decay range in seconds, the VCA's
//     off-isolation, and whether the reference implements hysteresis at all.
//     Hysteresis is in the model because the GATE LITERATURE is unanimous that a
//     single threshold chatters, and its −6 dB nominal is that literature's own
//     recommended starting point; the fixed 30 ms hold is likewise the published
//     designer practice (20–30 ms). Both are cited in docs §61.1.
//
// The device cards (2N3904, 1N4148) are NOT chosen here — they are the shared
// published cards in `SidechainDetector.h`, the same ones M13.1 uses.

#include "clipper/dsp/GateModel.h"

#include <algorithm>
#include <cmath>

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/ParamGuard.h"

namespace clipper::dsp {
namespace {

constexpr double kPi = 3.14159265358979323846;

// --- The detector's parts ----------------------------------------------------
// Same topology as M13.1's, different values — which is the whole claim of the
// shared detector, expressed as a diff rather than a second model.
constexpr double kVsupply = 9.0;      // the pedal's rail
constexpr double kCclamp = 10.0e-9;   // per-leg DC-restorer cap
constexpr double kRclamp = 1.0e6;     // per-leg node resistor to ground
// The envelope pair. M13.1's is 10 µF × 150 kΩ = 1.5 s, because a compressor's
// release IS its envelope. A gate's audible release is the DECAY knob on the VCA
// ramp, so the detector's own envelope only has to be quick enough to see a note
// stop: 47 nF × 220 kΩ = 10.34 ms, which is the "high-speed envelope detector"
// the reference advertises.
constexpr double kCenv = 47.0e-9;
constexpr double kRenv = 220.0e3;
// The sidechain rectifier is driven by an OP-AMP, not by M13.1's phase splitter,
// so BOTH legs see the same source impedance and the full-wave rectifier is
// symmetric. One series resistor per leg.
constexpr double kRsrcLeg = 1.0e3;

// --- The control stage's parts -----------------------------------------------
// The sidechain amp's fixed gain, ahead of the THRESHOLD attenuator. Chosen so
// that the most sensitive end of the knob puts the detector's turn-on at a
// realistic guitar NOISE floor rather than at a note (docs §61.4 has the
// measured table).
constexpr double kSidechainGain = 400.0;
// A log (audio-taper) threshold pot: dB-linear attenuation across the travel.
constexpr double kThresholdRangeDb = 40.0;
// The comparator's reference, as a fraction of the rail. The envelope node rests
// AT the rail with no signal and is pulled down by the detector, so the
// reference has to sit below it; 0.80 puts the trip where the detector's
// transfer is steepest, which is what makes the threshold sharp.
constexpr double kRefFraction = 0.80;
// Hysteresis: the gate literature's own recommended starting point.
constexpr double kHysteresisDb = 6.0;
// VCA control-node time constants. ATTACK fast enough to keep a pick transient
// (the literature's 0.1–1 ms for percussive guitar); HOLD the published fixed
// 20–30 ms designers build in so a gate does not chase individual cycles of a
// low note; DECAY the knob, spanning a fast chop to a long natural fade.
constexpr double kAttackSeconds = 0.0004;
constexpr double kHoldSeconds = 0.030;
constexpr double kDecayMinSeconds = 0.020;
constexpr double kDecayMaxSeconds = 2.000;
// The VCA's off-isolation. A real gain cell does not reach zero; this is the
// ceiling on the gate's noise reduction and the test reports it as such.
constexpr double kClosedGain = 1.0e-4;  // −80 dB

// Coupling caps. Both corners are deliberately WELL below the guitar's range:
// an open gate has to be transparent, and this pedal's whole claim is that the
// signal path never touches the detector.
constexpr double kInCouplingHz = 7.234;
constexpr double kOutCouplingHz = 7.234;

// One-pole coefficient for a time constant, at a rate.
double poleCoef(double tauSeconds, double rate) {
    if (tauSeconds <= 0.0) return 1.0;
    return 1.0 - std::exp(-1.0 / (tauSeconds * rate));
}

}  // namespace

GateModel::GateModel() {
    det_.clampCapF = kCclamp;
    det_.clampResOhms = kRclamp;
    det_.envCapF = kCenv;
    det_.envResOhms = kRenv;
    det_.supplyV = kVsupply;
    det_.vceSatV = 0.1;
    det_.srcEmitterOhms = kRsrcLeg;
    det_.srcCollectorOhms = kRsrcLeg;

    ctl_.sidechainGain = kSidechainGain;
    ctl_.thresholdRangeDb = kThresholdRangeDb;
    ctl_.refFraction = kRefFraction;
    ctl_.hysteresisDb = kHysteresisDb;
    ctl_.attackSeconds = kAttackSeconds;
    ctl_.holdSeconds = kHoldSeconds;
    ctl_.decayMinSeconds = kDecayMinSeconds;
    ctl_.decayMaxSeconds = kDecayMaxSeconds;
    ctl_.closedGain = kClosedGain;

    sc_.configure(det_, npn_, diode_);
    vRef_ = ctl_.refFraction * det_.supplyV;
    hystFactor_ = std::pow(10.0, ctl_.hysteresisDb / 20.0);
    gain_ = ctl_.closedGain;
}

void GateModel::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;

    // Knob smoothing: the house ~6 ms, applied per sample (never per block).
    threshWiper_.prepare(0.006, sampleRate_);
    decayWiper_.prepare(0.006, sampleRate_);
    // Construction defaults mirror the shipped knob defaults (web
    // GATE_KNOB_DEFAULTS / the native APVTS). Note the Muff trap docs §34
    // records: a model whose level smoother defaults to 0 renders digital
    // silence and makes a whole test sweep vacuous. A gate has no level knob, so
    // the equivalent hazard is a THRESHOLD default that gates everything.
    threshWiper_.setImmediate(0.35f);
    decayWiper_.setImmediate(0.5f);

    rebuildRates();
    reset();
    snapPending_ = true;
}

void GateModel::rebuildRates() {
    sc_.configure(det_, npn_, diode_);
    sc_.setRate(sampleRate_);

    // Base-rate coupling high-passes: y = R*(y1 + x - x1).
    inHpR_ = std::exp(-2.0 * kPi * kInCouplingHz / sampleRate_);
    outHpR_ = std::exp(-2.0 * kPi * kOutCouplingHz / sampleRate_);

    attackCoef_ = poleCoef(ctl_.attackSeconds, sampleRate_);
    holdSamples_ = static_cast<int>(ctl_.holdSeconds * sampleRate_ + 0.5);

    refreshKnobs(static_cast<double>(threshWiper_.value()),
                 static_cast<double>(decayWiper_.value()));
}

void GateModel::refreshKnobs(double thresholdKnob, double decayKnob) {
    // THRESHOLD: a LOG pot, so the attenuation is dB-linear in rotation. Fully
    // counter-clockwise passes the sidechain amp's whole gain (the most sensitive
    // setting = the LOWEST threshold); clockwise attenuates, so the input has to
    // be louder to trip.
    scAtten_ = std::pow(10.0, -ctl_.thresholdRangeDb * thresholdKnob / 20.0);
    // DECAY: log-spaced between the two endpoints, so the knob is even in
    // perceived time rather than crowding the fast end.
    const double tau = ctl_.decayMinSeconds *
                       std::pow(ctl_.decayMaxSeconds / ctl_.decayMinSeconds,
                                decayKnob);
    decayCoef_ = poleCoef(tau, sampleRate_);
}

void GateModel::reset() {
    inHpX1_ = inHpY1_ = 0.0;
    outHpX1_ = outHpY1_ = 0.0;
    // Re-park at the CACHED quiescent point: with no signal the detector's
    // envelope cap charges to the rail through R_env and nothing discharges it,
    // so the rail IS the quiescent point — an exact number, not a solve.
    sc_.reset(det_.supplyV);
    open_ = false;
    holdRemaining_ = 0;
    gain_ = ctl_.closedGain;
    threshWiper_.reset();
    decayWiper_.reset();
    refreshKnobs(static_cast<double>(threshWiper_.value()),
                 static_cast<double>(decayWiper_.value()));
}

void GateModel::setOversampling(int factor) {
    // Accepted and IGNORED — see the header. The gate's signal path is linear, so
    // there is nothing for an oversampler to band-limit, and the measurement says
    // so (docs §61.7). Kept as an ABI no-op exactly like `phaser_set_oversampling`
    // so the worklet and the native engine can drive every pedal identically.
    (void)factor;
}

void GateModel::setParameter(int paramId, float value) {
    // ParamGuard, never a hand-rolled clamp: std::clamp and the ternary both pass
    // NaN straight through, and one NaN latches forever in recursive state.
    const float v = clampParam01(value);
    switch (paramId) {
        case PARAM_THRESHOLD: threshWiper_.setTarget(v); break;
        case PARAM_DECAY: decayWiper_.setTarget(v); break;
        default:
            // Slot 1 is carried and unused (the reference gate has two knobs).
            break;
    }
}

double GateModel::openTripVolts() const { return vRef_; }

double GateModel::closeTripVolts() const {
    // The two trip points are the SAME comparator reference; what moves between
    // them is the sidechain gain (the positive-feedback path). Expressed in
    // envelope volts they are therefore identical — the hysteresis lives in the
    // level that reaches the detector, which is exactly why the gap is a defined
    // number of dB. See docs §61.5.
    return vRef_;
}

double GateModel::statedThresholdDbV(double knob) const {
    // The pedal's LABEL — what a printed knob scale would say — computed from the
    // pot law and the sidechain amp alone, with no rendered signal and no
    // detector in the arithmetic.
    //
    // `kStatedTripVolts` is the ONE calibration constant in this file: the
    // sidechain drive at which the detector's mean collector current reaches
    // (V+ − vRef)/R_env = 8.30 µA and the comparator trips. That is a
    // conduction-angle integral over the clamp network with no closed form, so it
    // is quoted from a measurement of the detector rather than derived — and the
    // test's teeth are deliberately NOT in this offset. What the test checks is
    // the SHAPE: that the measured threshold tracks the pot's dB-linear law
    // across the whole travel, which is a real prediction because the detector
    // between the pot and the comparator is exponential. Perturb the pot to
    // linear and the shape fails while this constant is untouched.
    constexpr double kStatedTripVolts = 0.4458;
    const double atten = std::pow(10.0, -ctl_.thresholdRangeDb * knob / 20.0);
    const double inputPeak = kStatedTripVolts / (ctl_.sidechainGain * atten);
    return 20.0 * std::log10(inputPeak);
}

void GateModel::process(const float* in, float* out, int numFrames) {
    int done = 0;
    while (done < numFrames) {
        const int n = std::min(numFrames - done, maxBlockSize_);
        processChunk(in + done, out + done, n);
        done += n;
    }
}

void GateModel::processChunk(const float* in, float* out, int numFrames) {
    if (snapPending_) {
        // Deferred snap (docs §35): the C ABI's prepare-then-set order would
        // otherwise ramp every knob from its construction default on the first
        // block.
        threshWiper_.setImmediate(threshWiper_.target());
        decayWiper_.setImmediate(decayWiper_.target());
        refreshKnobs(static_cast<double>(threshWiper_.value()),
                     static_cast<double>(decayWiper_.value()));
        snapPending_ = false;
    }

    // Stage 1 (base rate): the input coupling cap.
    for (int i = 0; i < numFrames; ++i) {
        const double x = static_cast<double>(in[i]);
        const double y = inHpR_ * (inHpY1_ + x - inHpX1_);
        inHpX1_ = flushDenormal(x);
        inHpY1_ = flushDenormal(y);
        out[i] = static_cast<float>(y);
    }

    // The VCA and the sidechain, at BASE RATE. There is no oversampling domain
    // here and that is measured, not skipped — see the header.
    for (int i = 0; i < numFrames; ++i) {
        // The knobs advance once per sample, as the house requires.
        refreshKnobs(static_cast<double>(threshWiper_.next()),
                     static_cast<double>(decayWiper_.next()));

        const double x = static_cast<double>(out[i]);
        const double y = x * gain_;

        // --- The sidechain. FEED-FORWARD: it tastes the pedal's INPUT. -------
        // The `detectorFromOutput_` branch is a measurement hook, never a
        // product mode: a gate that tastes its own output latches shut the first
        // time it closes, and `clipper_gate_tests` shows that rather than
        // arguing it.
        const double tap = detectorFromOutput_ ? y : x;
        // Positive feedback: while the gate is OPEN the sidechain sees
        // `hysteresisDb` more level, so the input must fall that far below the
        // OPEN threshold before the comparator will let go.
        const double drive =
            tap * ctl_.sidechainGain * scAtten_ * (open_ ? hystFactor_ : 1.0);
        sc_.advanceEnvelope(sc_.detect(drive), 0.0);

        // --- The comparator. The envelope node rests HIGH and signal pulls it
        // DOWN, so "below the reference" means "loud enough".
        const bool wasOpen = open_;
        open_ = sc_.envelopeVolts() < vRef_;
        if (wasOpen && !open_) holdRemaining_ = holdSamples_;

        // --- The VCA's control ramp: attack, then HOLD, then decay.
        double target;
        double coef;
        if (open_) {
            holdRemaining_ = 0;
            target = 1.0;
            coef = attackCoef_;
        } else if (holdRemaining_ > 0) {
            --holdRemaining_;
            target = 1.0;
            coef = attackCoef_;
        } else {
            target = ctl_.closedGain;
            coef = decayCoef_;
        }
        // NO flushDenormal: `gain_` rests at `closedGain` (1e-4), a real
        // operating point 296 decades above the subnormal range, so a guard here
        // would be unreachable code in the hottest loop (ADR 006's scope rule).
        gain_ += coef * (target - gain_);

        out[i] = static_cast<float>(y);
    }

    // Stage 3 (base rate): the output coupling cap.
    for (int i = 0; i < numFrames; ++i) {
        const double x = static_cast<double>(out[i]);
        const double y = outHpR_ * (outHpY1_ + x - outHpX1_);
        outHpX1_ = flushDenormal(x);
        outHpY1_ = flushDenormal(y);
        out[i] = static_cast<float>(y);
    }
}

double GateModel::maxAbsRestingState() const {
    // Every recursive accumulator that genuinely rests at ZERO, and nothing else
    // — the four coupling-cap states. Adding a state that rests elsewhere would
    // make this assertion untestable instead of stricter.
    //
    // Deliberately excluded, each MEASURED rather than assumed (docs §61.8):
    //  * the detector's envelope node — rests at the 9.0000 V rail.
    //  * `gain_` — rests at the VCA's off-isolation, 1.0000e-04.
    //  * the detector's clamp caps and Newton warm starts — floored by the
    //    solver's residual tolerance, not by physics (SidechainDetector::leg()).
    return maxAbsState(inHpX1_, inHpY1_, outHpX1_, outHpY1_);
}

}  // namespace clipper::dsp
