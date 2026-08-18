// Clipper — MesaAmp (M10.4, docs §68). Composition of the Dual Rectifier's
// preamp and power section in ONE shared oversampling domain. See MesaAmp.h for
// the domain argument, the interstage-scale derivation and the list of what this
// voice deliberately does not model.

#include "clipper/dsp/MesaAmp.h"

#include "clipper/dsp/ReverbModel.h"

#include <algorithm>

namespace clipper::dsp {

MesaAmp::MesaAmp() : reverb_(std::make_unique<ReverbModel>()) {
    preamp_.setMode(mode_);
    power_.setMode(mode_);
}
MesaAmp::~MesaAmp() = default;

// Both halves prepared AT THE OVERSAMPLED RATE with their own resamplers at 1x
// (an exact pass-through — Oversampler.h). One band-limiting in, one out.
void MesaAmp::rebuild() {
    os_.prepare(maxBlockSize_);
    os_.setFactor(oversampling_);
    const int f = os_.factor();
    const double osRate = sampleRate_ * static_cast<double>(f);
    const int osBlock = maxBlockSize_ * f;
    power_.setOversampling(1);
    power_.prepare(osRate, osBlock);
    preamp_.setOversampling(1);
    preamp_.prepare(osRate, osBlock);
    buf_.assign(static_cast<size_t>(osBlock), 0.0f);
}

void MesaAmp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    reverb_->prepare(sampleRate_);
    rebuild();
    prepared_ = true;
}

void MesaAmp::setOversampling(int factor) {
    oversampling_ = factor;
    // The factor sets the rate every stage is discretized at, so the whole
    // cascade re-prepares (and its DC operating points re-settle). Before
    // prepare() this only records the choice.
    if (prepared_) rebuild();
}

void MesaAmp::reset() {
    os_.reset();
    preamp_.reset();
    power_.reset();
    reverb_->reset();
}

void MesaAmp::setMode(MesaMode m) {
    const int i = static_cast<int>(m);
    if (i < 0 || i >= static_cast<int>(MesaMode::MESA_MODE_COUNT)) return;
    if (m == mode_) return;
    mode_ = m;
    preamp_.setMode(m);   // input select, cathode caps, pad, tone-stack cap
    power_.setMode(m);    // the feedback depth, and NOTHING else
}

void MesaAmp::setRectifier(MesaRectifier r) { power_.setRectifier(r); }
void MesaAmp::setPowerMode(MesaPowerMode p) { power_.setPowerMode(p); }

void MesaAmp::setParameter(int paramId, float value) {
    switch (paramId) {
        case PARAM_GAIN: preamp_.setParameter(MesaPreamp::PARAM_GAIN, value); break;
        case PARAM_MASTER: preamp_.setParameter(MesaPreamp::PARAM_MASTER, value); break;
        case PARAM_BASS: preamp_.setParameter(MesaPreamp::PARAM_BASS, value); break;
        case PARAM_MID: preamp_.setParameter(MesaPreamp::PARAM_MID, value); break;
        case PARAM_TREBLE: preamp_.setParameter(MesaPreamp::PARAM_TREBLE, value); break;
        case PARAM_PRESENCE:
            power_.setParameter(MesaPowerAmp::PARAM_PRESENCE, value);
            break;
        case PARAM_REVERB: reverb_->setMix(value); break;
        default: break;
    }
}

void MesaAmp::process(const float* in, float* out, int numFrames) {
    const int f = os_.factor();
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        const int m = n * f;
        os_.upsample(in + off, n);
        float* w = os_.buffer();
        preamp_.process(w, buf_.data(), m);
        if (kInterstageScale != 1.0) {
            for (int i = 0; i < m; ++i)
                buf_[static_cast<size_t>(i)] =
                    static_cast<float>(buf_[static_cast<size_t>(i)] * kInterstageScale);
        }
        power_.process(buf_.data(), w, m);
        os_.downsample(out + off, n);
        off += n;
    }
    // Carried for lineup parity only — a Solo Head has no reverb tank. Mix 0 is
    // a bit-exact pass-through, so the default costs nothing. See MesaAmp.h.
    reverb_->process(out, out, numFrames);
}

}  // namespace clipper::dsp
