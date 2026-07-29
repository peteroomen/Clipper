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

void TwinAmp::reset() {
    preamp_.reset();
    reverb_->reset();
    tremolo_->reset();
    power_.reset();
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
        case PARAM_TREMOLO_ENABLE: tremolo_->setEnabled(value >= 0.5f); break;
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
        //
        // 0.16 -> 0.107 (2026-07-29, docs §42). 0.16 was fitted around the STARVED phase
        // inverter of audit finding 7 (×7.4 per leg, 0.23 mA/triode, plates at 94 % of
        // B+), so it was absorbing the PI gain the model was missing and the corrected
        // inverter arrived 3.4-4.9 dB hot. Un-fitted in the same slice as the fix (the
        // ADR 008 precedent) and re-derived BY MEASUREMENT — not by the PI's gain ratio,
        // which the global NFB absorbs half of. The measured quantity is the power
        // section's CLOSED-LOOP gain, normalized-out per volt at the PI grid:
        //   f0 Hz      82      110     220     440     880
        //   before  0.0761  0.0777  0.0792  0.0796  0.0797
        //   after   0.1150  0.1163  0.1175  0.1179  0.1179
        //   ratio    1.511   1.497   1.484   1.480   1.480   (mean 1.490)
        // 0.16 / 1.490 = 0.1074 -> 0.107, so the 6L6 grids see the drive they were
        // calibrated with at every knob position below clipping. The ceiling moved too,
        // and that is kFullScaleSecV's job (see its comment in TwinPowerAmp.h).
        for (int i = 0; i < n; ++i)
            w[i] = static_cast<float>(w[i] * kInterstageScale);
        power_.process(w, out + off, n);
        off += n;
    }
}

}  // namespace clipper::dsp
