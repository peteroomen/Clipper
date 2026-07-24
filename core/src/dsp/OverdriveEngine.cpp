// Clipper — shared Tube-Screamer-family overdrive engine (v1.1).
// See OverdriveEngine.h for the topology overview; the circuit derivations live
// in the wrapper models (SdModel.cpp / TsModel.cpp) that supply each config.
//
// This file is the M8 SdModel processing path, extracted VERBATIM and
// parameterized by OverdriveConfig. With the SD-1 config it computes exactly what
// the pre-refactor in-line SdModel::Impl did (same math, same evaluation order,
// same control-rate sampling), so the M8 test suite passes byte-for-byte
// unchanged; the TS config swaps only the diode knees and the DRIVE plateau.

#include "clipper/dsp/OverdriveEngine.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace clipper::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586;

// ~5 ms glide, matches the RAT/M0 smoothers (shared across the family).
constexpr double kSmoothSeconds = 0.005;

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float onePoleCoeff(double cutoffHz, double sampleRate) {
    const double a = 1.0 - std::exp(-kTwoPi * cutoffHz / sampleRate);
    return static_cast<float>(std::clamp(a, 0.0, 1.0));
}

// DRIVE knob (0..1) -> feedback gain K (= plateau_gain - 1), linear-in-dB over
// [driveMinDb, driveMaxDb].
float driveKnobToK(float knob, float minDb, float maxDb) {
    const float db = minDb + (maxDb - minDb) * clamp01(knob);
    return std::pow(10.0f, db / 20.0f) - 1.0f;
}

// TONE knob (0..1) -> treble tilt LINEAR gain (1.0 == flat at knob 0.5).
float toneKnobToTilt(float knob, float maxTiltDb) {
    const float db = (clamp01(knob) - 0.5f) * 2.0f * maxTiltDb;
    return std::pow(10.0f, db / 20.0f);
}
}  // namespace

void OverdriveEngine::applyKnees() {
    if (kneeOverride_) {
        clip_.setKnees(ovVp_, ovVn_);
    } else if (symmetric_) {
        const double v = 0.5 * (cfg_.diodeVp + cfg_.diodeVn);
        clip_.setKnees(v, v);
    } else {
        clip_.setKnees(cfg_.diodeVp, cfg_.diodeVn);
    }
}

void OverdriveEngine::reprepareStage2() {
    os_.setFactor(osFactor_);
    const double osRate = sampleRate_ * os_.factor();
    midHumpCoef_ = onePoleCoeff(cfg_.midHumpHz, osRate);
    midLpState_ = 0.0f;
    opAmp_.prepare(osRate, cfg_.opAmpGbwHz, cfg_.opAmpSlewVoltsPerSec);
    applyKnees();
    clip_.reset();
}

void OverdriveEngine::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;

    driveK_.prepare(kSmoothSeconds, sampleRate_);
    toneTilt_.prepare(kSmoothSeconds, sampleRate_);
    level_.prepare(kSmoothSeconds, sampleRate_);

    toneCoef_ = onePoleCoeff(cfg_.tonePivotHz, sampleRate_);
    toneLpState_ = 0.0f;
    dcR_ = std::exp(-kTwoPi * cfg_.dcBlockHz / sampleRate_);
    dcX1_ = 0.0f;
    dcY1_ = 0.0f;

    os_.prepare(maxBlockSize_);
    reprepareStage2();
}

void OverdriveEngine::setOversampling(int factor) {
    osFactor_ = factor;
    reprepareStage2();
}

void OverdriveEngine::setClipMode(int mode) {
    clipMode_ = (mode == CLIP_NAIVE) ? CLIP_NAIVE : CLIP_ADAA;
    clip_.reset();
}

void OverdriveEngine::setIdealOpAmp(bool ideal) {
    idealOpAmp_ = ideal;
    opAmp_.reset();
}

void OverdriveEngine::setSymmetric(bool symmetric) {
    symmetric_ = symmetric;
    kneeOverride_ = false;  // an explicit symmetric choice clears any knee override
    applyKnees();
}

void OverdriveEngine::setDiodeKnees(double vp, double vn) {
    kneeOverride_ = true;
    ovVp_ = vp;
    ovVn_ = vn;
    applyKnees();
}

void OverdriveEngine::setParameter(int paramId, float value) {
    const float knob = clamp01(value);
    switch (paramId) {
        case PARAM_DRIVE:
            driveK_.setTarget(driveKnobToK(knob, cfg_.driveMinDb, cfg_.driveMaxDb));
            break;
        case PARAM_TONE:
            toneTilt_.setTarget(toneKnobToTilt(knob, cfg_.toneMaxTiltDb));
            break;
        case PARAM_LEVEL:
            level_.setTarget(knob);  // identity linear map, as the RAT
            break;
        default:
            break;
    }
}

void OverdriveEngine::process(const float* in, float* out, int numFrames) {
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        processChunk(in + off, out + off, n);
        off += n;
    }
}

void OverdriveEngine::processChunk(const float* in, float* out, int numFrames) {
    assert(numFrames <= maxBlockSize_ && "chunk exceeds maxBlockSize");

    // Advance the DRIVE smoother across the chunk; use the chunk value as the
    // feedback gain K for the oversampled section (control-rate, like the RAT's
    // op-amp corner and AmpModel's coeffs — the 5 ms glide makes it click-free).
    float K = driveK_.value();
    for (int i = 0; i < numFrames; ++i) K = driveK_.next();
    const float noiseGain = K + 1.0f;  // non-inverting closed-loop noise gain
    if (!idealOpAmp_) opAmp_.setNoiseGain(noiseGain);

    // --- Stage 1+2 (oversampled): V_out = V_in + f(K * HP720(V_in)). ---
    // Upsample the RAW input; the clean pedestal V_in and the clipped feedback
    // are summed at the oversampled rate so they stay sample-aligned. os.upsample
    // copies `in` internally first, so in/out may alias.
    os_.upsample(in, numFrames);
    float* w = os_.buffer();
    const int osN = os_.bufferLength();
    const float hc = midHumpCoef_;
    for (int i = 0; i < osN; ++i) {
        const float x = w[i];
        // Mid-hump high-pass: hp = x - LP720(x)  (unity at HF, 0 at DC).
        midLpState_ += hc * (x - midLpState_);
        const float hp = x - midLpState_;
        float u = K * hp;                       // amplified feedback drive
        if (!idealOpAmp_) u = opAmp_.processSample(u);  // 4558 BW + slew
        const float vfb = (clipMode_ == CLIP_NAIVE) ? clip_.processSampleNaive(u)
                                                    : clip_.processSampleADAA(u);
        w[i] = x + vfb;                          // clean pedestal + soft-clipped fb
    }
    os_.downsample(out, numFrames);

    // --- Stage 3 (base rate, linear): DC block -> tone tilt -> level. ---
    for (int i = 0; i < numFrames; ++i) {
        const float v = out[i];
        // One-pole DC blocker (output coupling cap, ~12 Hz).
        const float y = v - dcX1_ + static_cast<float>(dcR_) * dcY1_;
        dcX1_ = v;
        dcY1_ = y;
        // Treble tilt: scale the HF half (y - LP_pivot) by the tilt gain.
        toneLpState_ += toneCoef_ * (y - toneLpState_);
        const float hpTone = y - toneLpState_;
        const float toned = toneLpState_ + toneTilt_.next() * hpTone;
        out[i] = toned * level_.next();
    }
}

}  // namespace clipper::dsp
