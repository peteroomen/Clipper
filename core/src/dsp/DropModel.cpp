// Clipper — DropModel (M13.10, docs §70). See DropModel.h for the provenance
// banner, the faithful-controls decision and why latency is reported as zero.

#include "clipper/dsp/DropModel.h"

#include "clipper/dsp/ParamGuard.h"

#include <cmath>

namespace clipper::dsp {

namespace {
constexpr double kDrySmoothSeconds = 0.010;
}  // namespace

int DropModel::positionFor(double knob01) {
    const double v = clampParam01(knob01);
    int idx = static_cast<int>(v * static_cast<double>(POSITION_COUNT - 1) + 0.5);
    if (idx < 0) idx = 0;
    if (idx > POSITION_COUNT - 1) idx = POSITION_COUNT - 1;
    return idx;
}

void DropModel::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    shifter_.prepare(kWindowSeconds, sampleRate_);
    drySm_.prepare(kDrySmoothSeconds, sampleRate_);
    buf_.assign(static_cast<size_t>(maxBlockSize > 0 ? maxBlockSize : 128), 0.0f);
    applyPosition();
    drySm_.reset();  // snap: prepare must not ramp the dry sum in from zero
}

void DropModel::setOversampling(int) {
    // Accepted and IGNORED. Linear time-varying, no nonlinearity — the phaser's
    // and chorus's arrangement (§12). The suite proves the alias floor does not
    // move with the factor rather than taking this on trust.
}

void DropModel::reset() {
    shifter_.reset();
    drySm_.reset();
}

void DropModel::setParameter(int paramId, float value) {
    switch (paramId) {
        case PARAM_AMOUNT: {
            amount_ = clampParam01(static_cast<double>(value));
            applyPosition();
            break;
        }
        default: break;  // slots 1 and 2 are carried and unused (see the header)
    }
}

void DropModel::applyPosition() {
    position_ = positionFor(amount_);
    // The ratio is arithmetic, not a fit: 2^(-n/12) exactly.
    shifter_.setRatio(std::pow(2.0, -static_cast<double>(semitonesFor(position_)) / 12.0));
    drySm_.setTarget(blendsDryAt(position_) ? 1.0 : 0.0);
}

void DropModel::process(const float* in, float* out, int numFrames) {
    if (numFrames <= 0) return;
    if (static_cast<int>(buf_.size()) < numFrames)
        buf_.assign(static_cast<size_t>(numFrames), 0.0f);

    for (int n = 0; n < numFrames; ++n) {
        const float x = in[n];
        const float wet = shifter_.process(x);
        // The dry sum exists at exactly ONE selector position. Everywhere else
        // this multiplies by an exact 0, so the pedal is 100 % wet — which is
        // what the reference does and is not an oversight.
        const double dry = drySm_.next();
        out[n] = static_cast<float>(static_cast<double>(wet) +
                                    dry * static_cast<double>(x));
    }
}

}  // namespace clipper::dsp
