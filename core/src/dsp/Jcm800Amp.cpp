// Clipper — Jcm800Amp (M9.3). Composition of the M9.2 preamp and the M9.3 power
// section. See Jcm800Amp.h for the topology and the interstage-level rationale.

#include "clipper/dsp/Jcm800Amp.h"

#include <algorithm>

namespace clipper::dsp {

Jcm800Amp::Jcm800Amp() : reverb_(std::make_unique<ReverbModel>()) {}
Jcm800Amp::~Jcm800Amp() = default;

void Jcm800Amp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    buf_.assign(static_cast<size_t>(maxBlockSize_), 0.0f);
    preamp_.prepare(sampleRate_, maxBlockSize_);
    power_.prepare(sampleRate_, maxBlockSize_);
    reverb_->prepare(sampleRate_);
    setOversampling(oversampling_);
}

void Jcm800Amp::setOversampling(int factor) {
    oversampling_ = factor;
    preamp_.setOversampling(factor);
    power_.setOversampling(factor);
}

void Jcm800Amp::reset() {
    preamp_.reset();
    power_.reset();
    reverb_->reset();
}

void Jcm800Amp::setParameter(int paramId, float value) {
    switch (paramId) {
        case PARAM_GAIN: preamp_.setParameter(Jcm800Preamp::PARAM_GAIN, value); break;
        case PARAM_MASTER: preamp_.setParameter(Jcm800Preamp::PARAM_MASTER, value); break;
        case PARAM_BASS: preamp_.setParameter(Jcm800Preamp::PARAM_BASS, value); break;
        case PARAM_MID: preamp_.setParameter(Jcm800Preamp::PARAM_MID, value); break;
        case PARAM_TREBLE: preamp_.setParameter(Jcm800Preamp::PARAM_TREBLE, value); break;
        case PARAM_PRESENCE:
            power_.setParameter(Jcm800PowerAmp::PARAM_PRESENCE, value); break;
        case PARAM_REVERB: reverb_->setMix(value); break;
        default: break;
    }
}

void Jcm800Amp::process(const float* in, float* out, int numFrames) {
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        float* w = buf_.data();
        preamp_.process(in + off, w, n);
        // Preamp volts → PI grid drive. The master-volume node emits real volts;
        // trim the handoff so the phase inverter sees a realistic drive.
        for (int i = 0; i < n; ++i)
            w[i] = static_cast<float>(w[i] * kInterstageScale);
        power_.process(w, out + off, n);
        off += n;
    }
    // M10.1: mono spring reverb AFTER the power amp, BEFORE the C ABI dual-mono
    // split (a usability convenience — the real 2204 has none). Mix 0 == bit-exact
    // passthrough, so a reverb-off JCM is unchanged sample-for-sample.
    reverb_->process(out, out, numFrames);
}

}  // namespace clipper::dsp
