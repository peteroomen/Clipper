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

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/ParamGuard.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace clipper::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586;

// ~5 ms glide, matches the RAT/M0 smoothers (shared across the family).
constexpr double kSmoothSeconds = 0.005;

// NaN-rejecting knob clamp (ParamGuard.h) — audit finding 1.
float clamp01(float v) { return clampParam01(v); }

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
    tonePot_.prepare(kSmoothSeconds, sampleRate_);
    level_.prepare(kSmoothSeconds, sampleRate_);

    // Stage 3 — the pedal's own tone network at the base rate (docs §65), then
    // the output coupling cap. Both are LINEAR, so they commute; the order here
    // follows the circuit (the tone stage's op-amp output drives C6, not the
    // other way round).
    tone_.prepare(cfg_.tone, sampleRate_);
    tone_.setPot(tonePot_.value());
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

void OverdriveEngine::reset() {
    // Smoothers first: a poisoned smoother value never recovers on its own.
    driveK_.reset();
    tonePot_.reset();
    level_.reset();
    tone_.reset();
    tone_.setPot(tonePot_.value());
    dcX1_ = 0.0f;
    dcY1_ = 0.0f;
    os_.reset();
    // Re-derives the oversampled-section coefficients at the CURRENT rate/factor and
    // resets midLpState_ / the op-amp / the clipper. Allocation-free (no DC solve).
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
            // The knob IS the pot's wiper fraction: 0 = wiper at the op-amp's
            // non-inverting node (dark), 1 = at the inverting node (bright).
            // Both transcriptions mark the pot LINEAR (docs §65.1 records that the
            // real taper letter could not be sourced).
            tonePot_.setTarget(static_cast<double>(knob));
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
        // Anti-denormal (Denormal.h): flush the one-pole state at -600 dB so a
        // silent tail can never park it in the float subnormal range.
        midLpState_ = flushDenormal(midLpState_ + hc * (x - midLpState_));
        const float hp = x - midLpState_;
        float u = K * hp;                       // amplified feedback drive
        if (!idealOpAmp_) u = opAmp_.processSample(u);  // 4558 BW + slew
        const float vfb = (clipMode_ == CLIP_NAIVE) ? clip_.processSampleNaive(u)
                                                    : clip_.processSampleADAA(u);
        w[i] = x + vfb;                          // clean pedestal + soft-clipped fb
    }
    os_.downsample(out, numFrames);

    // --- Stage 3 (base rate, linear): tone network -> DC block -> level. ---
    // The tone network's coefficients are rebuilt only while the TONE smoother is
    // still moving (docs §65.6: ~5 std::exp per sample for the ~5 ms of a knob
    // move, and nothing at all once parked — OnePoleSmootherT::settled()).
    for (int i = 0; i < numFrames; ++i) {
        if (!tonePot_.settled()) tone_.setPot(tonePot_.next());
        const float v = tone_.processSample(out[i]);
        // One-pole DC blocker (output coupling cap, ~12 Hz).
        // Anti-denormal (Denormal.h): on silence this degenerates to y = dcR_*dcY1_
        // with dcR_ ~= 0.9984, and in the subnormal range that product ROUNDS BACK
        // TO ITSELF — the state never reaches zero and every later sample pays the
        // microcode penalty forever. Measured 1.15x (SD-1) / 1.13x (Screamer) on
        // silence before the flush; audit finding 11, docs §33. dcX1_ needs no guard:
        // it is an INPUT history (assigned, never fed back), so it is whatever the
        // signal is — never an asymptote.
        const float y = v - dcX1_ + static_cast<float>(dcR_) * dcY1_;
        dcX1_ = v;
        dcY1_ = flushDenormal(y);
        out[i] = y * level_.next();
    }
}

}  // namespace clipper::dsp
