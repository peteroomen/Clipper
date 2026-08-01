// Clipper — portable DSP core.
//
// OptoModel implementation. The sources, the SOURCED/RECONSTRUCTED table and
// every measured number live in docs §64; this file carries the component values
// and the discretization, with the arithmetic in the open.
//
// Things worth knowing before touching a constant here:
//
//  * `kSeriesOhms` is DERIVED from two published figures (over 40 dB of gain
//    reduction, at a lit-cell resistance under 1 kΩ), not chosen. Changing it
//    changes the pedal's maximum gain reduction and nothing else.
//
//  * The GAIN pot is an OUTPUT ATTENUATOR after the sidechain tap, which is the
//    reference's arrangement and is a testable property: make-up gain must move
//    the output and NOT the gain reduction.
//
//  * The loop carries exactly one sample of delay (the cell resistance used to
//    divide this sample was computed from the last one). That is unavoidable in
//    a feed-back loop and it is placed where the physical lag already is: the
//    cell's own attack is 10 ms, about 480 samples at 48 kHz.
//
//  * OVERSAMPLED at 4x, and the whole LOOP is inside the one domain — the cell
//    closes around the divider, so splitting them would change the loop. The
//    decision was measured before it was made (docs §64.7): the divider is a
//    multiply by an audio-rate signal, so it folds, and the floor moves 28 dB
//    with the rate.

#include "clipper/dsp/OptoModel.h"

#include <algorithm>
#include <cmath>

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/ParamGuard.h"

namespace clipper::dsp {
namespace {

constexpr double kPi = 3.14159265358979323846;

// ============================================================================
//  COMPONENT VALUES
// ============================================================================
// SOURCED (docs §64.1): the cell's dark/light resistances and the ~90 V panel
// ceiling; the published "0 to 40 dB gain limiting"; the two device exponents;
// the 2.2 MΩ bleeder across the EL panel; the LIMIT/COMPRESS sidechain blend.
// Everything else is a stated reconstruction.

// The gain-reduction divider's top leg. DERIVED: the published maximum gain
// reduction is "over 40 dB" and the published lit-cell resistance is "under
// 1 kΩ", so Rs = 1 kΩ·(10^(40/20) − 1) = 99 kΩ → the 100 kΩ standard value,
// which puts this model's own ceiling at 20·log10(1 + 100k/1k) = 40.09 dB.
constexpr double kSeriesOhms = 100.0e3;

// The leveling amplifier's fixed voltage gain (+20 dB). RECONSTRUCTED: the
// reference is a two-stage tube amplifier followed by a cathode follower and an
// output transformer, and this model does not carry tube stages (a documented
// simplification — docs §64.6). +20 dB puts a normally played note comfortably
// inside the swing below and makes GAIN's mid-travel unity.
constexpr double kAmpGain = 10.0;

// The amplifier's output swing, as a SOFT (ADAA) ceiling. RECONSTRUCTED. The
// reference's published distortion is under 0.5 % THD, i.e. it is not meant to
// be driven here; this exists so a slammed input degrades gracefully rather than
// hard-clipping, and it is the pedal's only fast nonlinearity.
constexpr double kHeadroomV = 6.0;

// The GAIN pot: an output ATTENUATOR spanning 40 dB, dB-linear in rotation (an
// audio-taper part), 0 dB at the top. RECONSTRUCTED span.
constexpr double kGainRangeDb = 40.0;

// Coupling corners. RECONSTRUCTED, chosen to sit below the guitar's low E so the
// pedal is not a high-pass — the reference's published response is +0/−1 dB from
// 30 Hz, which these two together satisfy (−0.55 dB at 30 Hz).
constexpr double kInCouplingHz = 8.0;
constexpr double kOutCouplingHz = 8.0;

// The sidechain's total voltage gain (amplifier × step-up) at PEAK REDUCTION
// 1.0, and the pot's span. RECONSTRUCTED: 90 makes the panel reach its sourced
// ~90 V ceiling on a 1 V node, and 40 dB of pot takes the pedal from
// "essentially uncompressed" to "slammed" across the travel.
constexpr double kSidechainMaxGain = 90.0;
constexpr double kPeakRangeDb = 40.0;

// The panel drive ceiling. SOURCED: the EL panel is driven "to no more than
// ~90 VAC peak".
constexpr double kPanelClampV = 90.0;

// The bleeder across the EL panel is a SOURCED 2.2 MΩ; the panel's own
// capacitance is RECONSTRUCTED at 20 nF (a small powder-EL panel), which puts
// the corner at 1/(2π·2.2M·20n) = 3.62 Hz. It is below the band by design — a
// bleeder's job is to keep DC off the panel, which is exactly what it does here:
// without it, any DC offset in the loop would light the cell forever.
constexpr double kPanelBleedHz = 3.62;

// MODE = LIMIT mixes this much of the pedal's INPUT into the sidechain, the rest
// coming from the output node. SOURCED, from a schematic reading published on a
// forum (docs §64.1 — it is the one component-level statement obtained about the
// switch, and it is a forum reading rather than a factory sheet).
constexpr double kLimitFeedForward = 1.0 / 25.0;

OptoVoice makeVoice() {
    OptoVoice v{};
    v.seriesOhms = kSeriesOhms;
    v.ampGain = kAmpGain;
    v.headroomV = kHeadroomV;
    v.gainRangeDb = kGainRangeDb;
    v.inCouplingHz = kInCouplingHz;
    v.outCouplingHz = kOutCouplingHz;
    v.sidechainMaxGain = kSidechainMaxGain;
    v.peakRangeDb = kPeakRangeDb;
    v.panelClampV = kPanelClampV;
    v.panelBleedHz = kPanelBleedHz;
    v.limitFeedForward = kLimitFeedForward;
    return v;
}

}  // namespace

OptoModel::OptoModel() : voice_(makeVoice()) {
    cell_.configure(OptoCellSpec{});
    // The shipped knob defaults (web OPTO_KNOB_DEFAULTS / the native APVTS) are
    // set in the CONSTRUCTOR rather than in prepare(), deliberately: a caller who
    // writes parameters BEFORE prepare() (which is what
    // `native/tests/identical_core_test.cpp`'s reference chain does) must keep
    // them. `OnePoleSmootherT::prepare` snaps value to TARGET, so it preserves
    // whatever was set; `setImmediate` in prepare() would overwrite it. Note the
    // Muff trap docs §34 records — a model whose output smoother defaults to 0
    // renders digital silence and makes a whole test sweep vacuous.
    peakWiper_.setImmediate(0.5f);
    gainWiper_.setImmediate(0.62f);
}

void OptoModel::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;

    os_.prepare(maxBlockSize_);
    os_.setFactor(osFactor_);

    // Knob smoothing: the house ~6 ms, applied per sample (never per block).
    // `OnePoleSmootherT::prepare` snaps value to TARGET, so a parameter written
    // BEFORE prepare() survives — which is why the defaults live in the
    // constructor and are deliberately NOT re-applied here.
    peakWiper_.prepare(0.006, sampleRate_);
    gainWiper_.prepare(0.006, sampleRate_);

    headroom_.setKnees(voice_.headroomV, voice_.headroomV);

    rebuildRates();
    reset();
    snapPending_ = true;
}

void OptoModel::rebuildRates() {
    osRate_ = sampleRate_ * static_cast<double>(os_.factor());
    // The cell lives INSIDE the oversampled domain, because the loop does.
    cell_.setRate(osRate_);
    // Coupling high-passes: y = R*(y1 + x - x1). Both are base-rate — they are
    // linear, so there is nothing for them to fold.
    inHpR_ = std::exp(-2.0 * kPi * voice_.inCouplingHz / sampleRate_);
    outHpR_ = std::exp(-2.0 * kPi * voice_.outCouplingHz / sampleRate_);
    // The panel bleeder is inside the loop, hence at the oversampled rate.
    panelHpR_ = std::exp(-2.0 * kPi * voice_.panelBleedHz / osRate_);
}

void OptoModel::reset() {
    inHpX1_ = inHpY1_ = 0.0;
    outHpX1_ = outHpY1_ = 0.0;
    panelHpX1_ = panelHpY1_ = 0.0;
    cell_.reset();
    // Re-park at the cell's DARK resistance — the cached quiescent point, and
    // there is nothing to solve for it.
    rCell_ = cell_.resistanceOhms();
    vPanel_ = 0.0;
    headroom_.reset();
    os_.reset();
    osPhase_ = 0;
    peakWiper_.reset();
    gainWiper_.reset();
    refreshKnobs(static_cast<double>(peakWiper_.value()),
                 static_cast<double>(gainWiper_.value()));
}

void OptoModel::setOversampling(int factor) {
    os_.setFactor(factor);
    osFactor_ = os_.factor();
    rebuildRates();
    // The loop's states are rate-dependent; clear them rather than leaving
    // history that belonged to another rate.
    panelHpX1_ = panelHpY1_ = 0.0;
    cell_.reset();
    rCell_ = cell_.resistanceOhms();
    headroom_.reset();
    osPhase_ = 0;
}

int OptoModel::oversampling() const { return os_.factor(); }
int OptoModel::latencySamples() const { return os_.latencySamples(); }

void OptoModel::setParameter(int paramId, float value) {
    // ParamGuard, never a hand-rolled clamp: std::clamp and the ternary both
    // pass NaN straight through, and one NaN latches forever in recursive state.
    const float v = clampParam01(value);
    switch (paramId) {
        case PARAM_PEAK_REDUCTION:
            peakWiper_.setTarget(v);
            break;
        case PARAM_MODE:
            // DISCRETE, like the CE-1's chorus/vibrato switch: the real control
            // is a two-position switch, so a smoothed blend would be a knob that
            // lies. The chain layer declicks a topology change.
            limitMode_ = v >= 0.5f;
            break;
        case PARAM_GAIN:
            gainWiper_.setTarget(v);
            break;
        default:
            break;
    }
}

void OptoModel::refreshKnobs(double peakKnob, double gainKnob) {
    // PEAK REDUCTION is a LOG (audio-taper) part in the sidechain, so its
    // attenuation is dB-linear in rotation: full gain at the top of the travel.
    scGain_ = voice_.sidechainMaxGain *
              std::pow(10.0, (peakKnob - 1.0) * voice_.peakRangeDb / 20.0);
    // GAIN is the same kind of part used as an output attenuator, 0 dB at 1.0.
    makeup_ = std::pow(10.0, (gainKnob - 1.0) * voice_.gainRangeDb / 20.0);
}

double OptoModel::gainReductionDb() const {
    const double div = rCell_ / (voice_.seriesOhms + rCell_);
    return -20.0 * std::log10(div > 1e-12 ? div : 1e-12);
}

void OptoModel::process(const float* in, float* out, int numFrames) {
    int done = 0;
    while (done < numFrames) {
        const int n = std::min(numFrames - done, maxBlockSize_);
        processChunk(in + done, out + done, n);
        done += n;
    }
}

void OptoModel::processChunk(const float* in, float* out, int numFrames) {
    if (snapPending_) {
        // Deferred snap (docs §35): the C ABI's prepare-then-set order would
        // otherwise ramp every knob from its construction default on the first
        // block.
        peakWiper_.setImmediate(peakWiper_.target());
        gainWiper_.setImmediate(gainWiper_.target());
        refreshKnobs(static_cast<double>(peakWiper_.value()),
                     static_cast<double>(gainWiper_.value()));
        snapPending_ = false;
    }

    // Stage 1 (base rate): input coupling. Linear, so nothing to fold.
    for (int i = 0; i < numFrames; ++i) {
        const double xin = static_cast<double>(in[i]);
        const double x = inHpR_ * (inHpY1_ + xin - inHpX1_);
        inHpX1_ = flushDenormal(xin);
        inHpY1_ = flushDenormal(x);
        out[i] = static_cast<float>(x);
    }

    // Stages 2..5 run inside ONE oversampling domain. The cell closes AROUND the
    // divider, so the sidechain is inside the same loop — splitting them would
    // change the loop, exactly as CompressorEngine's detector is inside its own.
    os_.upsample(out, numFrames);
    float* w = os_.buffer();
    const int f = os_.factor();
    const int n = numFrames * f;
    osPhase_ = 0;
    for (int i = 0; i < n; ++i) {
        // One knob advance per BASE sample (see the members' declarations).
        if (osPhase_ == 0) {
            refreshKnobs(static_cast<double>(peakWiper_.next()),
                         static_cast<double>(gainWiper_.next()));
        }
        if (++osPhase_ >= f) osPhase_ = 0;
        const double x = static_cast<double>(w[i]);

        // --- The gain-reduction divider: the T4B cell is the BOTTOM leg ------
        const double div = rCell_ / (voice_.seriesOhms + rCell_);
        const double reduced = x * div;

        // --- The leveling amplifier, and its own swing ----------------------
        const double amp = static_cast<double>(
            headroom_.processSampleADAA(static_cast<float>(reduced * voice_.ampGain)));

        // --- The sidechain taps HERE, before the GAIN pot -------------------
        // COMPRESS: the sidechain IS the amplifier node (100 % feed-back).
        // LIMIT: a linear combination of that node and the pedal's own input, so
        // under gain reduction the sidechain still sees the un-reduced peak.
        const double scIn =
            limitMode_ ? (1.0 - voice_.limitFeedForward) * amp +
                             voice_.limitFeedForward * x * voice_.ampGain
                       : amp;

        // The 2M2 bleeder keeps DC off the panel (3.62 Hz — below the band).
        const double drive = scIn * scGain_;
        const double vp = panelHpR_ * (panelHpY1_ + drive - panelHpX1_);
        panelHpX1_ = flushDenormal(drive);
        panelHpY1_ = flushDenormal(vp);
        // The step-up cannot deliver more than the sourced ~90 V peak.
        vPanel_ = vp > voice_.panelClampV
                      ? voice_.panelClampV
                      : (vp < -voice_.panelClampV ? -voice_.panelClampV : vp);

        // --- The cell. The panel is the rectifier; the cell is the integrator.
        rCell_ = cell_.process(vPanel_);

        w[i] = static_cast<float>(amp);
    }
    os_.downsample(out, numFrames);

    // Stage 6 (base rate): GAIN (an output attenuator, AFTER the sidechain tap)
    // and the output coupling cap.
    for (int i = 0; i < numFrames; ++i) {
        const double y = static_cast<double>(out[i]) * makeup_;
        const double yo = outHpR_ * (outHpY1_ + y - outHpX1_);
        outHpX1_ = flushDenormal(y);
        outHpY1_ = flushDenormal(yo);
        out[i] = static_cast<float>(yo);
    }
}

double OptoModel::maxAbsRestingState() const {
    // Every recursive accumulator that genuinely rests at ZERO, and nothing
    // else: the three coupling high-passes and the cell's three states.
    //
    // Deliberately EXCLUDED, measured rather than assumed:
    //  * `rCell_` — rests at the cell's DARK resistance (1.0e7 Ω). A real
    //    operating point; it can never be subnormal (ADR 006's scope rule).
    //  * the ADAA clipper's history — rests at exactly 0 but is owned by
    //    `AsymSoftClipper`, which every other consumer already covers.
    const double lin = maxAbsState(inHpX1_, inHpY1_, outHpX1_, outHpY1_,
                                   panelHpX1_, panelHpY1_);
    const double cellState = cell_.maxAbsRestingState();
    return cellState > lin ? cellState : lin;
}

}  // namespace clipper::dsp
