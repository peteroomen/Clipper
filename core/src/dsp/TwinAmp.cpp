// Clipper — TwinAmp (M10.1). Composition of the TwinPreamp, the mono spring
// ReverbModel, the OptoTremolo, and the TwinPowerAmp in the authentic AB763
// signal order. See TwinAmp.h for the topology and the headroom rationale.

#include "clipper/dsp/TwinAmp.h"

#include <algorithm>

namespace clipper::dsp {

TwinAmp::TwinAmp()
    : reverb_(std::make_unique<ReverbModel>()),
      tremolo_(std::make_unique<OptoTremolo>()) {}
TwinAmp::~TwinAmp() = default;

void TwinAmp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    buf_.assign(static_cast<size_t>(maxBlockSize_), 0.0f);
    preamp_.prepare(sampleRate_, maxBlockSize_);
    power_.prepare(sampleRate_, maxBlockSize_);
    reverb_->prepare(sampleRate_);
    tremolo_->prepare(sampleRate_);
    setOversampling(oversampling_);
}

void TwinAmp::setOversampling(int factor) {
    oversampling_ = factor;
    preamp_.setOversampling(factor);
    power_.setOversampling(factor);
}

void TwinAmp::setParameter(int paramId, float value) {
    switch (paramId) {
        case PARAM_VOLUME: preamp_.setParameter(TwinPreamp::PARAM_VOLUME, value); break;
        case PARAM_BASS: preamp_.setParameter(TwinPreamp::PARAM_BASS, value); break;
        case PARAM_MID: preamp_.setParameter(TwinPreamp::PARAM_MID, value); break;
        case PARAM_TREBLE: preamp_.setParameter(TwinPreamp::PARAM_TREBLE, value); break;
        case PARAM_BRIGHT: preamp_.setParameter(TwinPreamp::PARAM_BRIGHT, value); break;
        case PARAM_REVERB: reverb_->setMix(value); break;
        case PARAM_SPEED: tremolo_->setSpeed(value); break;
        case PARAM_INTENSITY: tremolo_->setIntensity(value); break;
        default: break;
    }
}

void TwinAmp::process(const float* in, float* out, int numFrames) {
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        float* w = buf_.data();
        // Preamp: V1 → Fender TMB → V2 → volume/bright (real volts at the volume node).
        preamp_.process(in + off, w, n);
        // AB763 order: reverb blended AFTER the recovery/volume, BEFORE tremolo+PI.
        reverb_->process(w, w, n);
        // Optical tremolo ("vibrato" on the panel) — amplitude modulation.
        tremolo_->process(w, w, n);
        // Preamp volts → PI grid drive (memoryless trim, documented constant).
        for (int i = 0; i < n; ++i)
            w[i] = static_cast<float>(w[i] * kInterstageScale);
        power_.process(w, out + off, n);
        off += n;
    }
}

}  // namespace clipper::dsp
