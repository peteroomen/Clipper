#include "ClipperEngine.h"

#include <algorithm>
#include <cmath>

namespace clipper::native {

namespace {
// Mirrors web/src/params.ts INPUT_TRIM_MIN_DB / MAX_DB.
constexpr float kTrimMinDb = -12.0f;
constexpr float kTrimMaxDb = 24.0f;
constexpr int   kCabPartition = 128;  // == worklet render quantum / cab partition
}  // namespace

float trimKnobToGain(float knob01) {
    const float k = std::min(1.0f, std::max(0.0f, knob01));
    const float db = kTrimMinDb + (kTrimMaxDb - kTrimMinDb) * k;
    return std::pow(10.0f, db / 20.0f);
}

ClipperEngine::ClipperEngine() = default;

void ClipperEngine::applyParamsToModels() {
    const Params& p = params_;

    // Dirt pedals: id 0/1/2 map to (dist/drive, filter/tone, level) — identical
    // positional ABI in RatModel and SdModel.
    rat_.setParameter(clipper::dsp::RatModel::PARAM_DISTORTION, p.ratDist);
    rat_.setParameter(clipper::dsp::RatModel::PARAM_FILTER, p.ratFilter);
    rat_.setParameter(clipper::dsp::RatModel::PARAM_LEVEL, p.ratLevel);

    sd_.setParameter(clipper::dsp::SdModel::PARAM_DRIVE, p.sdDrive);
    sd_.setParameter(clipper::dsp::SdModel::PARAM_TONE, p.sdTone);
    sd_.setParameter(clipper::dsp::SdModel::PARAM_LEVEL, p.sdLevel);

    // Amp tone stack + volume + bright, then the routed chorus params.
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_VOLUME, p.volume);
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_BASS, p.bass);
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_MIDDLE, p.middle);
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_TREBLE, p.treble);
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_BRIGHT, p.bright ? 1.0f : 0.0f);
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_CHORUS_SPEED, p.chorusSpeed);
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_CHORUS_DEPTH, p.chorusDepth);
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_CHORUS_MODE,
                      static_cast<float>(p.chorusMode));
}

void ClipperEngine::setParams(const Params& p) {
    params_ = p;
    applyParamsToModels();
}

void ClipperEngine::updateParams(const Params& p) {
    using clipper::dsp::AmpModel;
    using clipper::dsp::RatModel;
    using clipper::dsp::SdModel;
    const Params& o = params_;  // old snapshot

    // Dirt-pedal knobs (only the changed ones — never re-seed a steady smoother).
    if (p.ratDist != o.ratDist)     rat_.setParameter(RatModel::PARAM_DISTORTION, p.ratDist);
    if (p.ratFilter != o.ratFilter) rat_.setParameter(RatModel::PARAM_FILTER, p.ratFilter);
    if (p.ratLevel != o.ratLevel)   rat_.setParameter(RatModel::PARAM_LEVEL, p.ratLevel);
    if (p.sdDrive != o.sdDrive)     sd_.setParameter(SdModel::PARAM_DRIVE, p.sdDrive);
    if (p.sdTone != o.sdTone)       sd_.setParameter(SdModel::PARAM_TONE, p.sdTone);
    if (p.sdLevel != o.sdLevel)     sd_.setParameter(SdModel::PARAM_LEVEL, p.sdLevel);

    // Amp knobs + toggles.
    if (p.volume != o.volume) amp_.setParameter(AmpModel::PARAM_VOLUME, p.volume);
    if (p.bass != o.bass)     amp_.setParameter(AmpModel::PARAM_BASS, p.bass);
    if (p.middle != o.middle) amp_.setParameter(AmpModel::PARAM_MIDDLE, p.middle);
    if (p.treble != o.treble) amp_.setParameter(AmpModel::PARAM_TREBLE, p.treble);
    if (p.bright != o.bright) amp_.setParameter(AmpModel::PARAM_BRIGHT, p.bright ? 1.0f : 0.0f);
    if (p.chorusSpeed != o.chorusSpeed) amp_.setParameter(AmpModel::PARAM_CHORUS_SPEED, p.chorusSpeed);
    if (p.chorusDepth != o.chorusDepth) amp_.setParameter(AmpModel::PARAM_CHORUS_DEPTH, p.chorusDepth);
    if (p.chorusMode != o.chorusMode)
        amp_.setParameter(AmpModel::PARAM_CHORUS_MODE, static_cast<float>(p.chorusMode));

    // Oversampling change: reset only the pedals' OS filter state (like the web
    // worklet's per-node setOversampling), not a full re-prepare.
    if (p.oversampling != o.oversampling) {
        rat_.setOversampling(p.oversampling);
        sd_.setOversampling(p.oversampling);
    }

    // The remaining fields (inputTrim gain, ratOn/sdOn/ampOn/cab) are consumed
    // directly in process()/latencySamples() from params_, so just store them.
    params_ = p;
}

void ClipperEngine::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_ = std::max(1, maxBlockSize);

    // Push targets first, then prepare so each model snaps its smoothers to them.
    applyParamsToModels();

    rat_.prepare(sampleRate_, maxBlock_);
    sd_.prepare(sampleRate_, maxBlock_);
    amp_.prepare(sampleRate_, maxBlock_);

    rat_.setOversampling(params_.oversampling);
    sd_.setOversampling(params_.oversampling);

    const std::vector<float> ir = clipper::dsp::generateDefaultCab2x12IR(sampleRate_);
    cabL_.prepare(sampleRate_, ir.data(), static_cast<int>(ir.size()), sampleRate_,
                  kCabPartition);
    cabR_.prepare(sampleRate_, ir.data(), static_cast<int>(ir.size()), sampleRate_,
                  kCabPartition);

    limiter_.prepare(sampleRate_);

    bufA_.assign(static_cast<size_t>(maxBlock_), 0.0f);
    bufB_.assign(static_cast<size_t>(maxBlock_), 0.0f);
}

void ClipperEngine::process(const float* in, float* outL, float* outR,
                            int numFrames) {
    if (numFrames <= 0) return;
    if (numFrames > maxBlock_) {
        // Chunk oversized blocks so scratch never overflows (mirrors the core's own
        // internal chunking contract).
        int off = 0;
        while (off < numFrames) {
            const int chunk = std::min(maxBlock_, numFrames - off);
            process(in + off, outL + off, outR + off, chunk);
            off += chunk;
        }
        return;
    }

    const Params& p = params_;

    // 1. Input trim into ping-pong buffer A.
    const float g = trimKnobToGain(p.inputTrim);
    float* cur = bufA_.data();
    float* other = bufB_.data();
    for (int i = 0; i < numFrames; ++i) cur[i] = in[i] * g;

    // 2. Fixed pedal chain: RAT then SD-1, each only if engaged. Ping-pong so a
    // bypassed pedal is a true pass-through (its buffer is simply not swapped in).
    if (p.ratOn) {
        rat_.process(cur, other, numFrames);
        std::swap(cur, other);
    }
    if (p.sdOn) {
        sd_.process(cur, other, numFrames);
        std::swap(cur, other);
    }

    // 3. Amp stage. Powered off => stereo passthrough of the mono chain signal.
    if (!p.ampOn) {
        for (int i = 0; i < numFrames; ++i) {
            outL[i] = cur[i];
            outR[i] = cur[i];
        }
    } else {
        amp_.processStereo(cur, outL, outR, numFrames);
        if (p.cab) {
            cabL_.process(outL, outL, numFrames);  // in-place ok
            cabR_.process(outR, outR, numFrames);
        }
    }

    // 4. Final safety limiter (stereo, one shared gain — image-preserving).
    limiter_.processStereo(outL, outR, numFrames);
}

int ClipperEngine::latencySamples() const {
    const Params& p = params_;
    int n = 0;
    if (p.ratOn) n += rat_.latencySamples();
    if (p.sdOn) n += sd_.latencySamples();
    if (p.ampOn && p.cab) n += kCabPartition;      // cab adds one partition (128)
    n += limiter_.latencySamples();                // lookahead (64)
    return n;
}

}  // namespace clipper::native
