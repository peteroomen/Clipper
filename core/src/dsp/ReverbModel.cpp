// Clipper — ReverbModel (M6.7-2). TRUE DISPERSIVE SPRING reverb in the JC-120's
// spring-tank position. See ReverbModel.h for the overview and scope.
//
// ---------------------------------------------------------------------------
// Why this replaced M6.7's Schroeder/Moorer "spring-flavored" network:
//
//   M6.7 was a bank of 4 short parallel feedback combs + a steep 4th-order 4.5 kHz
//   in-loop lid. Two classic failure modes fell straight out of that topology:
//     * "TOO METALLIC" — 4 short combs ring on HARMONICALLY-spaced modes (each comb
//       resonates at integer multiples of 1/delay). Evenly-spaced modes fuse into a
//       pitched, clangy "sproing". A real spring does NOT do this.
//     * "UNDERWATER"    — a steep 4th-order 4.5 kHz lowpass INSIDE the feedback loop
//       plus an allpass smear produced a dull, phasey wash with no air.
//
// The fix is to model the actual physics. A spring reverb is a helical spring whose
// wave speed is FREQUENCY-DEPENDENT (dispersive): a transient injected at one end
// arrives smeared into a DOWNWARD-swept chirp ("boing"), and each round trip emits
// the next chirp. Because the round-trip delay VARIES WITH FREQUENCY, the resonant
// modes are STRETCHED (inharmonic), so they never stack into a metallic pitch. That
// dispersion is exactly what a long cascade of first-order allpass filters gives.
//
// ---------------------------------------------------------------------------
// Topology (mono), in signal order:
//
//   in ─► transducer band-limit (2nd-order HP 150 Hz + 2nd-order LP 5.2 kHz)
//      ─► [ spring 1 ]  +  [ spring 2 ]   (0.5·sum; two detuned springs)
//      ─► transducer band-limit (HP 150 Hz + LP 5.2 kHz)  ─► ×wetGain  ─► wet
//
//   out = cos(mix²·π/2)·dry + sin(mix²·π/2)·wet   [equal-power, squared taper]
//
//   Each spring is a FEEDBACK LOOP:
//
//     loopIn ─► bulk delay (Dₖ samples) ─► [ N first-order dispersion allpasses ]
//            ─► in-loop damping (gentle HF high-shelf + 120 Hz low-cut) ─► loopOut
//     feedback = g · loopOut ;  loopIn = dry_band + feedback
//
// WHY each block:
//
//   DISPERSION ALLPASS CASCADE (the heart). Each section is a FIRST-ORDER allpass in
//     the DOWNWARD form  H(z) = (z⁻¹ − a)/(1 − a·z⁻¹)  (pole at +a, |a|<1). Its group
//     delay DECREASES with frequency: at DC ≈ (1+a)/(1−a) samples, at Nyquist ≈
//     (1−a)/(1+a). So LOW frequencies are delayed MORE than highs → a broadband
//     impulse exits as a HIGH→LOW descending chirp. Cascading N of them accumulates
//     the sweep into an audible "boing", and — crucially — makes the loop's total
//     round-trip delay frequency-dependent, which STRETCHES the mode spacing and
//     kills the harmonic (metallic) stacking of M6.7. a = 0.74/0.728 sits in the
//     documented 0.6–0.75 window; at a≈0.74 each section carries a lot of dispersion,
//     so ~32 sections do the work a lower-coefficient 100–200-section chain would —
//     the deliberate chirp-rate/CPU tradeoff (see docs §16 and the perf test).
//
//   BULK DELAY (Dₖ samples, ~26 ms @44.1k). Sets the round-trip ECHO PERIOD together
//     with the cascade's low-frequency group delay (total ≈ 30 ms round trip, scaled
//     by fs so it holds in ms). This is the SHORT end of the spring range on purpose:
//     lengthening the loop toward 40–56 ms let the (constant) bulk delay dominate the
//     (fixed-length) dispersion, and the modes collapsed back to a HARMONIC comb
//     (metallic — measured mode-spacing stretch fell from ~3.3× to ~1.0×, the very
//     failure this milestone fixes). At ~30 ms the dispersion stays a large enough
//     fraction of the loop to keep the modes stretched, and successive ~12 ms chirps
//     overlap into a wash rather than discrete pings. See docs §16.
//
//   TWO DETUNED SPRINGS (loop lengths differing ~6%, coefficients 0.74 vs 0.728),
//     summed. The slight detune gives the dual-spring shimmer/beating of a real tank
//     and further de-correlates the two mode series.
//
//   IN-LOOP DAMPING — GENTLE. A one-pole-ish high-SHELF (−2.5 dB above ~6 kHz) so the
//     spring loses a little top per pass (springs get darker as they ring), plus a
//     120 Hz low-cut so the tail does not boom. This is deliberately NOT M6.7's steep
//     4.5 kHz in-loop lid — that lid is what made M6.7 sound "underwater".
//
//   TRANSDUCER BAND-LIMIT (2nd-order HP 150 Hz + 2nd-order LP 5.2 kHz at input AND
//     output). A real spring's pickup/driver transducers pass a band ~150 Hz..5.2 kHz.
//     Gentler (2nd-order) and higher (5.2 vs 4.5 kHz) than M6.7's 4th-order lid, so
//     the tail keeps some air (>6 kHz is down but NOT dead) instead of sounding dull.
//
// EQUAL-POWER MIX with a SQUARED KNOB TAPER: dryGain = cos(mix²·π/2), wetGain =
//   sin(mix²·π/2). The raw equal-power curve reached 38% wet gain at a quarter
//   turn (field feedback: "saturates a little fast"); squaring the knob keeps the
//   lower half gradual while the endpoints are untouched. At mix == 0,
//   cos(0)==1 and sin(0)==0 EXACTLY (IEEE), and the whole network is SKIPPED (fast
//   path), so reverb == 0 is a BIT-EXACT dry passthrough.
//
// Deterministic + allocation-free in process(). A tiny (1e-20) anti-denormal offset
// is injected at each spring's loop input so a tail decaying to silence never drops
// into denormal arithmetic (a CPU cliff); it is removed by the 120 Hz / 150 Hz
// high-passes (no audible DC) and sits >100 dB below any audible tail.
// ---------------------------------------------------------------------------

#include "clipper/dsp/ReverbModel.h"

#include "clipper/dsp/Biquad.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/ParamGuard.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace clipper::dsp {

namespace {
constexpr double kHalfPi = 1.5707963267948966;
constexpr double kSmoothSeconds = 0.010;  // ~10 ms mix ramp (click-free)

// Two detuned springs. Section counts are FIXED across sample rate (the dispersion
// is defined in samples); the bulk delays scale with fs so the echo period holds in
// ms. Spring 2's loop is ~6% longer with a slightly lower allpass coefficient — the
// dual-spring detune.
constexpr int kNumSprings = 2;
constexpr int kSections[kNumSprings] = {32, 34};       // dispersion allpass sections
constexpr float kAllpassA[kNumSprings] = {0.740f, 0.728f};
constexpr int kBulk44k[kNumSprings] = {1150, 1219};    // bulk delay @44.1k (samples)
constexpr float kLoopGain = 0.900f;                    // -> RT60 ~1.7 s (see test)

// In-loop damping: gentle HF high-shelf + a low-cut. NOT M6.7's steep 4.5 kHz lid.
constexpr double kShelfHz = 6000.0;
constexpr double kShelfDb = -2.5;
constexpr double kShelfS = 0.5;
constexpr double kLoopLowCutHz = 120.0;

// Transducer band-limit (spring passband): 2nd-order HP + LP, at input and output.
constexpr double kBandHpHz = 150.0;
constexpr double kBandLpHz = 5200.0;
constexpr double kFilterQ = 0.7071067811865476;  // Butterworth

// Overall wet trim so a full-wet mix sits musically against the dry.
constexpr float kWetGain = 3.0f;

// Anti-denormal offset injected at each spring's loop input (see header note).
constexpr float kAntiDenormal = 1e-20f;

// NaN-rejecting knob clamp (ParamGuard.h) — audit finding 1. Load-bearing here:
// a NaN MIX would circulate through the spring delay lines forever.
float clamp01(float v) { return clampParam01(v); }

// Scale a 44.1k-referenced delay length to the engine rate, min 1 sample.
int scaleLen(int len44k, double fs) {
    int n = static_cast<int>(std::lround(len44k * fs / 44100.0));
    return n < 1 ? 1 : n;
}

// A fixed-length integer delay line (ring buffer, allocation-free in process).
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

// One dispersive spring: bulk delay + N first-order downward-dispersion allpasses in
// a feedback loop, with gentle in-loop damping. The allpass state is two parallel
// arrays (x1/y1 per section) sized in prepare() — allocation-free in process().
struct Spring {
    DelayLine bulk;
    std::vector<float> apX1;  // previous input  per section
    std::vector<float> apY1;  // previous output per section
    int sections = 0;
    float a = 0.74f;
    float g = 0.9f;
    float fb = 0.0f;
    Biquad hfShelf;  // gentle HF loss per pass
    Biquad lowCut;   // 120 Hz in-loop low-cut

    void prepare(int bulkLen, int nSections, float allpassA, float gain,
                 const BiquadCoeffs& shelf, const BiquadCoeffs& lc) {
        bulk.prepare(bulkLen);
        sections = nSections;
        apX1.assign(static_cast<size_t>(sections), 0.0f);
        apY1.assign(static_cast<size_t>(sections), 0.0f);
        a = allpassA;
        g = gain;
        fb = 0.0f;
        hfShelf.setCoeffs(shelf);
        lowCut.setCoeffs(lc);
        hfShelf.reset();
        lowCut.reset();
    }
    void reset() {
        bulk.reset();
        std::fill(apX1.begin(), apX1.end(), 0.0f);
        std::fill(apY1.begin(), apY1.end(), 0.0f);
        fb = 0.0f;
        hfShelf.reset();
        lowCut.reset();
    }
    inline float process(float x) {
        // Feedback loop input (+ anti-denormal keeps the loop off the denormal cliff;
        // it is removed by the 120 Hz low-cut so there is no audible DC).
        float y = bulk.process(x + fb + kAntiDenormal);
        // Dispersion cascade: first-order allpass, downward form
        //   y = -a*x + x1 + a*y1  ==  H(z)=(z^-1 - a)/(1 - a z^-1).
        for (int s = 0; s < sections; ++s) {
            const float in = y;
            const float out = -a * in + apX1[static_cast<size_t>(s)] +
                              a * apY1[static_cast<size_t>(s)];
            apX1[static_cast<size_t>(s)] = in;
            apY1[static_cast<size_t>(s)] = out;
            y = out;
        }
        y = hfShelf.process(y);
        y = lowCut.process(y);
        fb = g * y;
        return y;
    }
};
}  // namespace

struct ReverbModel::Impl {
    double sampleRate = 44100.0;

    OnePoleSmoother mix;  // 0..1 wet/dry knob

    Biquad inHp, inLp;    // transducer band-limit (input)
    Spring springs[kNumSprings];
    Biquad outHp, outLp;  // transducer band-limit (output)

    inline float processSample(float dry) {
        float in = inHp.process(dry);
        in = inLp.process(in);
        float w = 0.0f;
        for (int s = 0; s < kNumSprings; ++s) w += springs[s].process(in);
        w *= 0.5f;
        w = outHp.process(w);
        w = outLp.process(w);
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

    const auto bandHp = rbj::highPass(kBandHpHz, kFilterQ, d.sampleRate);
    const auto bandLp = rbj::lowPass(kBandLpHz, kFilterQ, d.sampleRate);
    d.inHp.setCoeffs(bandHp);
    d.inLp.setCoeffs(bandLp);
    d.outHp.setCoeffs(bandHp);
    d.outLp.setCoeffs(bandLp);
    d.inHp.reset();
    d.inLp.reset();
    d.outHp.reset();
    d.outLp.reset();

    const auto shelf = rbj::highShelf(kShelfHz, kShelfDb, kShelfS, d.sampleRate);
    const auto lc = rbj::highPass(kLoopLowCutHz, kFilterQ, d.sampleRate);
    for (int s = 0; s < kNumSprings; ++s) {
        d.springs[s].prepare(scaleLen(kBulk44k[s], d.sampleRate), kSections[s],
                             kAllpassA[s], kLoopGain, shelf, lc);
    }
}

void ReverbModel::reset() {
    Impl& d = *impl_;
    // Snap the MIX smoother onto its target: a poisoned smoother value can never
    // climb back out on its own (audit finding 1), and the idle fast path below
    // keys off mix == 0 exactly.
    d.mix.reset();
    d.inHp.reset();
    d.inLp.reset();
    d.outHp.reset();
    d.outLp.reset();
    for (int s = 0; s < kNumSprings; ++s) d.springs[s].reset();
}

void ReverbModel::setMix(float knob01) { impl_->mix.setTarget(clamp01(knob01)); }

void ReverbModel::process(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;

    // Idle fast path: mix parked at 0 -> bit-exact dry passthrough and the network
    // stays silent (nothing fed), so reverb == 0 leaves the rig unchanged sample-
    // for-sample.
    if (d.mix.value() == 0.0f && d.mix.target() == 0.0f) {
        if (out != in) std::copy(in, in + numFrames, out);
        return;
    }

    for (int i = 0; i < numFrames; ++i) {
        const float dry = in[i];
        const float m = d.mix.next();
        const float wet = d.processSample(dry);
        // Squared knob taper UNDER the equal-power law (field feedback: the raw
        // equal-power curve "saturates a little fast" — 38% wet gain at a quarter
        // turn). m^2 keeps the knob gradual and controllable in its lower half
        // while 0 stays exactly 0 (bit-exact dry) and 1 stays exactly full wet.
        const float m2 = m * m;
        const float wg = static_cast<float>(std::sin(m2 * kHalfPi));
        const float dg = static_cast<float>(std::cos(m2 * kHalfPi));
        out[i] = dg * dry + wg * wet;
    }

    // When the knob has ramped back to (essentially) 0, snap the smoother to exactly
    // 0 so the next block takes the free bit-exact passthrough again, and flush the
    // (now inaudible) tail so a re-engage starts clean.
    if (d.mix.target() == 0.0f && d.mix.value() < 1e-6f) {
        d.mix.setImmediate(0.0f);
        reset();
    }
}

}  // namespace clipper::dsp
