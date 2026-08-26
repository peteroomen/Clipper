// Clipper — Ac30Amp (M10.2). Composition of the Ac30Preamp, the Ac30PowerAmp, and a
// mono spring ReverbModel (usability add). See Ac30Amp.h for the topology and the
// VOLUME-is-the-overdrive rationale.

#include "clipper/dsp/Ac30Amp.h"

#include <algorithm>

namespace clipper::dsp {

Ac30Amp::Ac30Amp() : reverb_(std::make_unique<ReverbModel>()) {}
Ac30Amp::~Ac30Amp() = default;

// ONE shared oversampling domain (2026-08-26) — §63.14's change, applied here.
// Both halves prepare AT the oversampled rate with their own resamplers at 1x
// (Oversampler.h's documented exact pass-through), so Ac30Preamp, Ac30PowerAmp and
// TriodeStage need no edit. Note the ORDER: Ac30Preamp::prepare re-applies its own
// stored factor to every stage, so setOversampling(1) must come FIRST.
void Ac30Amp::rebuild() {
    os_.prepare(maxBlockSize_);
    os_.setFactor(oversampling_);
    const int f = os_.factor();
    const double osRate = sampleRate_ * static_cast<double>(f);
    const int osBlock = maxBlockSize_ * f;
    preamp_.setOversampling(1);
    preamp_.prepare(osRate, osBlock);
    power_.setOversampling(1);
    power_.prepare(osRate, osBlock);
    buf_.assign(static_cast<size_t>(osBlock), 0.0f);
}

void Ac30Amp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    reverb_->prepare(sampleRate_);
    rebuild();
    prepared_ = true;
}

void Ac30Amp::setOversampling(int factor) {
    oversampling_ = factor;
    if (prepared_) rebuild();
}

void Ac30Amp::reset() {
    // Shared-domain halfband state is recursive: part of the recovery seam.
    os_.reset();
    preamp_.reset();
    power_.reset();
    reverb_->reset();
}

void Ac30Amp::setParameter(int paramId, float value) {
    switch (paramId) {
        case PARAM_VOLUME: preamp_.setParameter(Ac30Preamp::PARAM_VOLUME, value); break;
        case PARAM_BASS: preamp_.setParameter(Ac30Preamp::PARAM_BASS, value); break;
        case PARAM_TREBLE: preamp_.setParameter(Ac30Preamp::PARAM_TREBLE, value); break;
        case PARAM_TOPCUT: power_.setParameter(Ac30PowerAmp::PARAM_TOPCUT, value); break;
        case PARAM_REVERB: reverb_->setMix(value); break;
        default: break;
    }
}

void Ac30Amp::process(const float* in, float* out, int numFrames) {
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        const int m = n * os_.factor();
        // ONE band-limiting in, one out (2026-08-26). The tank stays at BASE rate,
        // after the decimation — the same place in the signal path it always was.
        os_.upsample(in + off, n);
        float* w = buf_.data();
        // Preamp: V1 → top-boost stack → VOLUME (real volts at the volume node).
        preamp_.process(os_.buffer(), w, m);
        // Preamp volts → hot PI grid drive (memoryless trim, documented constant).
        for (int i = 0; i < m; ++i)
            w[i] = static_cast<float>(w[i] * kInterstageScale);
        power_.process(w, os_.buffer(), m);
        os_.downsample(out + off, n);
        // Spring reverb (usability add) AFTER the power amp, mono; mix 0 = passthrough.
        reverb_->process(out + off, out + off, n);
        off += n;
    }
}

}  // namespace clipper::dsp
