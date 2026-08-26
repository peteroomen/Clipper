// Clipper — Jcm800Amp (M9.3). Composition of the M9.2 preamp and the M9.3 power
// section. See Jcm800Amp.h for the topology and the interstage-level rationale.

#include "clipper/dsp/Jcm800Amp.h"

#include <algorithm>

namespace clipper::dsp {

Jcm800Amp::Jcm800Amp() : reverb_(std::make_unique<ReverbModel>()) {}
Jcm800Amp::~Jcm800Amp() = default;

// ONE shared oversampling domain (2026-08-26), the change §63.14 made on the
// Rockerverb. Both halves are prepared AT the oversampled rate with their OWN
// resamplers at 1x — Oversampler.h's documented exact pass-through — so
// Jcm800Preamp, Jcm800PowerAmp and TriodeStage need no edit at all: they simply
// run at 4x the base rate and band-limit nothing internally.
//
// What it costs and what it buys is in docs; the headline is 360 -> 72 samples of
// latency (7.50 -> 1.50 ms) because five independent domains became one.
void Jcm800Amp::rebuild() {
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

void Jcm800Amp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    reverb_->prepare(sampleRate_);
    rebuild();
    prepared_ = true;
}

void Jcm800Amp::setOversampling(int factor) {
    oversampling_ = factor;
    // The factor sets the rate every stage is discretized at, so the whole cascade
    // re-prepares (and its DC operating points re-settle). Before prepare() this
    // only records the choice.
    if (prepared_) rebuild();
}

void Jcm800Amp::reset() {
    // The shared domain's halfband delay lines are recursive state and must be
    // cleared too, or a NaN survives in the resampler after the models are reparked
    // (audit finding 1 / docs §28: reset() is the recovery seam).
    os_.reset();
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
        const int m = n * os_.factor();
        // ONE band-limiting in, one out (2026-08-26). `os_.buffer()` holds the
        // upsampled block; the preamp writes the interstage scratch, the power amp
        // writes back into the oversampler's own buffer to be decimated.
        os_.upsample(in + off, n);
        float* w = buf_.data();
        preamp_.process(os_.buffer(), w, m);
        // Preamp volts → PI grid drive. The master-volume node emits real volts;
        // trim the handoff so the POWER TUBES see a realistic drive.
        //
        // 0.25 -> 0.16 (2026-07-29, docs §42). 0.25 was fitted around the STARVED phase
        // inverter of audit finding 7 (×15.4 per leg, idling at 0.18 mA/triode with its
        // plates at 95 % of B+): it was absorbing the PI gain the model was missing, so
        // when the tail reference landed the inverter on its textbook operating point
        // (×29.7 per leg) the whole voice arrived 4-6 dB hot and clipped past full scale.
        // Un-fitted here in the same slice as the fix (the ADR 008 precedent), and
        // re-derived BY MEASUREMENT rather than by dividing by the PI's gain ratio — the
        // global NFB absorbs about half of that ratio, so the number that matters is the
        // power section's CLOSED-LOOP gain, normalized-out per volt at the PI grid:
        //   f0 Hz      82      110     220     440     880    1760    3520
        //   before  0.1129  0.1208  0.1288  0.1321  0.1369  0.1460  0.1530
        //   after   0.1890  0.1938  0.1980  0.2017  0.2116  0.2336  0.2566
        //   ratio    1.674   1.604   1.537   1.527   1.545   1.600   1.677
        // Mean over the guitar fundamental range (82-880 Hz) is 1.577, so the trim that
        // restores the documented power-tube grid drive is 0.25 / 1.577 = 0.1585 -> 0.16
        // (1 % of drive, +0.08 dB, from exact). The spread is the NFB loop's own
        // frequency shaping and cannot be removed by a broadband trim.
        // Below clipping the model's level is therefore unchanged by finding 7; what DID
        // change is the ceiling, and that belongs to kFullScaleSecV (see its comment).
        for (int i = 0; i < m; ++i)
            w[i] = static_cast<float>(w[i] * kInterstageScale);
        power_.process(w, os_.buffer(), m);
        os_.downsample(out + off, n);
        off += n;
    }
    // M10.1: mono spring reverb AFTER the power amp, BEFORE the C ABI dual-mono
    // split (a usability convenience — the real 2204 has none). Mix 0 == bit-exact
    // passthrough, so a reverb-off JCM is unchanged sample-for-sample.
    reverb_->process(out, out, numFrames);
}

}  // namespace clipper::dsp
