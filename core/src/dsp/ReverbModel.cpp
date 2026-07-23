// Clipper — ReverbModel (M6.7). Spring-FLAVORED algorithmic reverb in the JC-120's
// spring-tank position. See ReverbModel.h for the overview and scope.
//
// ---------------------------------------------------------------------------
// Topology (mono), in signal order:
//
//   in -> predelay(10 ms) -> [ 4 parallel damped combs ] --sum--> * combScale
//      -> [ 2 series Schroeder allpass diffusers ]
//      -> [ 4 short "spring chirp" allpasses ]
//      -> band-limit: 2x highpass @150 Hz  +  2x lowpass @4.5 kHz  -> wet
//
//   out = cos(mix*pi/2)*dry + sin(mix*pi/2)*(wetGain*wet)     [equal-power mix]
//
// WHY these blocks (spring flavor, not a bright plate — documented choices):
//
//   PREDELAY 10 ms. A short gap before the tail, like the pickup-to-first-return
//     travel down the tank. Long enough to separate the dry pluck from the wet
//     bloom, short enough to still read as "reverb", not "delay".
//
//   4 DAMPED COMBS (Schroeder/Moorer parallel bank, Freeverb-style feedback with a
//     one-pole lowpass IN the loop). The comb delays are mutually prime-ish
//     (1116/1188/1277/1356 @44.1k, scaled by fs) so their modes interleave into a
//     dense, non-periodic tail rather than a flutter. The in-loop lowpass ("damp")
//     makes the HIGH frequencies decay FASTER than the lows — springs are dark and
//     get darker as they ring, the opposite of a bright plate.
//     DECAY is FIXED (the knob is MIX, per the real amp): the feedback is tuned for
//     a broadband RT60 ~= 1.5 s (kCombFeedback below), squarely in the 1.2..2.0 s
//     design window. See the RT60 derivation in the test.
//
//   2 SERIES ALLPASS DIFFUSERS (Schroeder). They smear the comb output into a
//     diffuse tail (raise echo density) without changing its magnitude spectrum —
//     the classic reverb "make it dense" stage.
//
//   4 SHORT "SPRING CHIRP" ALLPASSES. A cascade of SHORT allpasses has strongly
//     frequency-dependent group delay: different frequencies exit at slightly
//     different times, which is a small TASTE of the dispersive "boing/chirp" a
//     real spring makes when a transient travels its length (highs and lows
//     arriving at different times). This is a FLAVOR, explicitly NOT the true
//     dispersive-waveguide spring — that physics (chirped echo trains, dual
//     detuned springs) is M6.7-2, which replaces this whole file behind the same
//     one-knob interface. The short delays (67/97/131/173 @44.1k) and moderate
//     gain give audible dispersion without ringing into a separate echo.
//
//   BAND-LIMIT (2x HP @150 Hz + 2x LP @4.5 kHz, i.e. 4th-order skirts). A real
//     spring reverb is a pair of narrowband electromechanical transducers: it
//     passes roughly 150 Hz .. 4.5 kHz and rolls off steeply outside. Cutting the
//     sub-150 Hz keeps the tail from booming and the >4.5 kHz keeps it from
//     fizzing/sounding like a plate. This is THE line between "spring-ish" and
//     "generic digital reverb", so it is a 4th-order (steep) cut, not a gentle one.
//
// EQUAL-POWER MIX. dryGain = cos(mix*pi/2), wetGain = sin(mix*pi/2): constant total
//   power across the sweep. At mix == 0, cos(0)==1 and sin(0)==0 EXACTLY (IEEE), so
//   out == in bit-for-bit — and we additionally SKIP the whole network when the mix
//   is idle at 0 (fast path + guaranteed bit-exact passthrough).
//
// Deterministic + allocation-free in process(). A tiny anti-denormal offset is fed
// into each comb's damping store so a decaying-to-silence tail never drops into
// denormal arithmetic (a CPU cliff); at ~1e-20 it is >100 dB below any audible
// tail and does not affect the decay/spectral measurements.
// ---------------------------------------------------------------------------

#include "clipper/dsp/ReverbModel.h"

#include "clipper/dsp/Biquad.h"
#include "clipper/dsp/OnePoleSmoother.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace clipper::dsp {

namespace {
constexpr double kHalfPi = 1.5707963267948966;
constexpr double kSmoothSeconds = 0.010;  // ~10 ms mix ramp (click-free)

// Predelay.
constexpr double kPredelayMs = 10.0;

// Parallel comb bank (delays in samples @ 44.1 kHz; scaled by fs/44100).
constexpr int kNumCombs = 4;
constexpr int kCombLen44k[kNumCombs] = {1116, 1188, 1277, 1356};
// Fixed feedback -> a broadband RT60 ~= 1.5 s (see the test's RT60 derivation:
// RT60 ~= -3*D/(fs*log10(g)); at D=1116, fs=44100, g=0.89 => ~1.5 s). "Dwell" is
// fixed on purpose — the single knob is a MIX, like the real JC REVERB control.
constexpr float kCombFeedback = 0.89f;
// In-loop damping (one-pole lowpass): highs decay faster than lows (spring-dark).
constexpr float kCombDamp = 0.28f;
// Sum of 4 combs is scaled back so the wet level is musical against the dry.
constexpr float kCombScale = 0.25f;

// Series Schroeder allpass diffusers (delays @44.1k, standard Freeverb figures).
constexpr int kNumDiffusers = 2;
constexpr int kDiffLen44k[kNumDiffusers] = {556, 441};
constexpr float kDiffuserGain = 0.5f;

// Short "spring chirp" allpass cascade (dispersion FLAVOR). Short delays + moderate
// gain give frequency-dependent group delay (a taste of the spring boing) without
// ringing into a discrete echo.
constexpr int kNumChirp = 4;
constexpr int kChirpLen44k[kNumChirp] = {67, 97, 131, 173};
constexpr float kChirpGain = 0.6f;

// Transducer band-limit (the spring passband). Steep (4th-order) so the tail is
// clearly NOT a full-range plate.
constexpr double kHighpassHz = 150.0;
constexpr double kLowpassHz = 4500.0;
constexpr double kFilterQ = 0.7071067811865476;  // Butterworth per stage

// Overall wet trim after band-limiting (keeps a full-wet mix comparable in level
// to the dry so the knob feels balanced; not load-bearing for the tests).
constexpr float kWetGain = 0.6f;

// Anti-denormal offset fed into each comb damping store (see header note).
constexpr float kAntiDenormal = 1e-20f;

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Scale a 44.1k-referenced delay length to the engine rate, min 1 sample.
int scaleLen(int len44k, double fs) {
    int n = static_cast<int>(std::lround(len44k * fs / 44100.0));
    return n < 1 ? 1 : n;
}

// A fixed-length delay line (integer taps; no interpolation needed — all delays
// here are constant). Ring buffer, allocation-free in process.
struct DelayLine {
    std::vector<float> buf;
    int size = 0;
    int idx = 0;
    void prepare(int n) {
        size = n < 1 ? 1 : n;
        buf.assign(static_cast<size_t>(size), 0.0f);
        idx = 0;
    }
    void reset() {
        std::fill(buf.begin(), buf.end(), 0.0f);
        idx = 0;
    }
    // Read the oldest sample (delay == size), then write x in its place.
    inline float process(float x) {
        float y = buf[static_cast<size_t>(idx)];
        buf[static_cast<size_t>(idx)] = x;
        if (++idx >= size) idx = 0;
        return y;
    }
};

// Freeverb-style lowpass-damped comb: y = buf[idx]; store = lp(y); buf[idx] = x +
// store*feedback. The damping store is a one-pole lowpass in the feedback path.
struct Comb {
    std::vector<float> buf;
    int size = 0;
    int idx = 0;
    float store = 0.0f;
    float feedback = 0.0f;
    float damp = 0.0f;
    void prepare(int n) {
        size = n < 1 ? 1 : n;
        buf.assign(static_cast<size_t>(size), 0.0f);
        idx = 0;
        store = 0.0f;
    }
    void reset() {
        std::fill(buf.begin(), buf.end(), 0.0f);
        idx = 0;
        store = 0.0f;
    }
    inline float process(float x) {
        const float y = buf[static_cast<size_t>(idx)];
        store = y * (1.0f - damp) + store * damp + kAntiDenormal;
        buf[static_cast<size_t>(idx)] = x + store * feedback;
        if (++idx >= size) idx = 0;
        return y;
    }
};

// Schroeder allpass: out = -x + buf; buf_new = x + buf*gain.
struct Allpass {
    std::vector<float> buf;
    int size = 0;
    int idx = 0;
    float gain = 0.5f;
    void prepare(int n) {
        size = n < 1 ? 1 : n;
        buf.assign(static_cast<size_t>(size), 0.0f);
        idx = 0;
    }
    void reset() {
        std::fill(buf.begin(), buf.end(), 0.0f);
        idx = 0;
    }
    inline float process(float x) {
        const float bufout = buf[static_cast<size_t>(idx)];
        const float out = -x + bufout;
        buf[static_cast<size_t>(idx)] = x + bufout * gain;
        if (++idx >= size) idx = 0;
        return out;
    }
};
}  // namespace

struct ReverbModel::Impl {
    double sampleRate = 44100.0;

    OnePoleSmoother mix;  // 0..1 wet/dry knob

    DelayLine predelay;
    Comb combs[kNumCombs];
    Allpass diffusers[kNumDiffusers];
    Allpass chirp[kNumChirp];
    Biquad hp1, hp2, lp1, lp2;  // 4th-order band-limit (spring passband)

    // One wet sample from a single dry input sample (the whole network).
    inline float processSample(float dry) {
        float x = predelay.process(dry);

        // Parallel comb bank.
        float sum = 0.0f;
        for (int c = 0; c < kNumCombs; ++c) sum += combs[c].process(x);
        float w = sum * kCombScale;

        // Series diffusers, then the short chirp cascade.
        for (int d = 0; d < kNumDiffusers; ++d) w = diffusers[d].process(w);
        for (int a = 0; a < kNumChirp; ++a) w = chirp[a].process(w);

        // Transducer band-limit (steep HP + LP -> the spring passband).
        w = hp1.process(w);
        w = hp2.process(w);
        w = lp1.process(w);
        w = lp2.process(w);

        return w * kWetGain;
    }
};

ReverbModel::ReverbModel() : impl_(std::make_unique<Impl>()) {}
ReverbModel::~ReverbModel() = default;

void ReverbModel::prepare(double sampleRate) {
    Impl& d = *impl_;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;

    d.mix.prepare(kSmoothSeconds, d.sampleRate);
    d.mix.setImmediate(0.0f);  // default fully dry (bit-exact passthrough)

    d.predelay.prepare(scaleLen(static_cast<int>(std::lround(kPredelayMs * 44100.0 / 1000.0)),
                                d.sampleRate));
    for (int c = 0; c < kNumCombs; ++c) {
        d.combs[c].prepare(scaleLen(kCombLen44k[c], d.sampleRate));
        d.combs[c].feedback = kCombFeedback;
        d.combs[c].damp = kCombDamp;
    }
    for (int i = 0; i < kNumDiffusers; ++i) {
        d.diffusers[i].prepare(scaleLen(kDiffLen44k[i], d.sampleRate));
        d.diffusers[i].gain = kDiffuserGain;
    }
    for (int i = 0; i < kNumChirp; ++i) {
        d.chirp[i].prepare(scaleLen(kChirpLen44k[i], d.sampleRate));
        d.chirp[i].gain = kChirpGain;
    }

    const auto hp = rbj::highPass(kHighpassHz, kFilterQ, d.sampleRate);
    const auto lp = rbj::lowPass(kLowpassHz, kFilterQ, d.sampleRate);
    d.hp1.setCoeffs(hp);
    d.hp2.setCoeffs(hp);
    d.lp1.setCoeffs(lp);
    d.lp2.setCoeffs(lp);
    d.hp1.reset();
    d.hp2.reset();
    d.lp1.reset();
    d.lp2.reset();
}

void ReverbModel::reset() {
    Impl& d = *impl_;
    d.predelay.reset();
    for (int c = 0; c < kNumCombs; ++c) d.combs[c].reset();
    for (int i = 0; i < kNumDiffusers; ++i) d.diffusers[i].reset();
    for (int i = 0; i < kNumChirp; ++i) d.chirp[i].reset();
    d.hp1.reset();
    d.hp2.reset();
    d.lp1.reset();
    d.lp2.reset();
}

void ReverbModel::setMix(float knob01) { impl_->mix.setTarget(clamp01(knob01)); }

void ReverbModel::process(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;

    // Idle fast path: mix parked at 0 -> bit-exact dry passthrough and the network
    // stays silent (no input fed), so there is nothing to advance. This is what
    // makes reverb == 0 leave the rig unchanged sample-for-sample.
    if (d.mix.value() == 0.0f && d.mix.target() == 0.0f) {
        if (out != in) std::copy(in, in + numFrames, out);
        return;
    }

    for (int i = 0; i < numFrames; ++i) {
        const float dry = in[i];
        const float m = d.mix.next();
        const float wet = d.processSample(dry);
        const float wg = static_cast<float>(std::sin(m * kHalfPi));
        const float dg = static_cast<float>(std::cos(m * kHalfPi));
        out[i] = dg * dry + wg * wet;
    }

    // When the knob has ramped back down to (essentially) 0, snap the smoother to
    // exactly 0 so the next block takes the free bit-exact passthrough again.
    if (d.mix.target() == 0.0f && d.mix.value() < 1e-6f) {
        d.mix.setImmediate(0.0f);
        reset();  // flush the (now inaudible) tail so a re-engage starts clean
    }
}

}  // namespace clipper::dsp
