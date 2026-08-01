// Clipper — ChorusModel (M6.3). JC-120-style chorus/vibrato modulated delay.
// See ChorusModel.h for the overview and the interpolation/LFO rationale.
//
// ---------------------------------------------------------------------------
// Chosen numbers and why (mirrors the AmpModel.cpp documentation style):
//
//   BASE DELAY  kBaseMs = 5.0 ms. The dry (L) and wet (R) sides differ by ~5 ms
//     of pure delay in CHORUS mode; that offset is what decorrelates the two
//     speakers into the stereo bloom (and, summed acoustically, combs around
//     ~100 Hz and its odd multiples). 5 ms is the classic short-chorus figure —
//     long enough to widen, short enough not to read as a slapback echo.
//
//   DEPTH -> SWEEP. depth 0..1 maps with a SQUARED taper to a sine sweep
//     amplitude of 0 .. kMaxSweepMs (3.5 ms) PEAK: A = depth^2 * 3.5 ms. At
//     full depth the wet delay swings 5 ± 3.5 ms (1.5 .. 8.5 ms) — still
//     comfortably positive and well above the interpolator's floor. The wide
//     excursion came from field feedback ("chorus/vibrato needs to be
//     stronger"); the squared taper keeps the knob's lower half in
//     subtle-shimmer territory so the extra range costs no resolution.
//
//   SPEED -> RATE. speed 0..1 maps LOG to kMinHz .. kMaxHz (0.15 .. 8 Hz):
//     rate = kMinHz * (kMaxHz/kMinHz)^speed. Log so the musical slow-to-medium
//     range (a JC chorus lives ~0.5-3 Hz) occupies most of the knob and the fast
//     end is reachable but compressed.
//
//   PEAK PITCH DEVIATION. For delay(t) = D0 + A*sin(w t) (A in seconds, w=2*pi*f),
//     the instantaneous fractional pitch shift is -d(delay)/dt = -A*w*cos(w t),
//     so the PEAK deviation is |A*w| = A * 2*pi*f, and in cents
//        peak_cents ~= (1200/ln2) * A * 2*pi*f  ~= 38.1 * depth^2 * f_hz
//     (using A = depth^2 * 3.5 ms). Examples: depth 1 @ 2 Hz ~= 76 cents;
//     depth 1 @ 5 Hz ~= 190 cents (full seasick, by request); depth 0.5 @
//     2 Hz ~= 19 cents. Deviation grows with BOTH depth and rate — the
//     physical truth of a fixed-excursion swept delay.
//
//   INTERPOLATION: 4-point Lagrange (cubic). Stateless, so the twice-per-cycle
//     sweep reversal leaves no ringing (an all-pass interpolator would).
//
// Smoothing: depth (as sweep-in-samples) and rate (Hz) feed OnePoleSmoothers
// (~8 ms), advanced per sample; the LFO phase is continuous so a rate change
// never clicks. Mode is a hard switch.
// ---------------------------------------------------------------------------

#include "clipper/dsp/ChorusModel.h"

#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/ParamGuard.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace clipper::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586;

constexpr double kBaseMs = 5.0;      // base wet delay
constexpr double kMaxSweepMs = 3.5;  // sine sweep PEAK amplitude at depth = 1
constexpr double kMinHz = 0.15;      // LFO rate at speed = 0
constexpr double kMaxHz = 8.0;       // LFO rate at speed = 1
constexpr double kGuardMs = 3.0;     // extra delay-buffer headroom past max delay
constexpr double kSmoothSeconds = 0.008;

// NaN-rejecting knob clamp (ParamGuard.h) — audit finding 1. A NaN sweep depth
// would write NaN into the delay line and it would never wash out.
float clamp01(float v) { return clampParam01(v); }

// speed knob 0..1 -> LFO frequency (log map kMinHz..kMaxHz).
double speedToHz(float knob) {
    const double k = clamp01(knob);
    return kMinHz * std::pow(kMaxHz / kMinHz, k);
}
}  // namespace

struct ChorusModel::Impl {
    double sampleRate = 44100.0;

    // Delay line (mono ring buffer of the amp's post-volume signal).
    std::vector<float> buf;
    int bufLen = 0;
    int writeIdx = 0;

    // Base delay in SAMPLES. A double smoother, not a plain double, because the
    // CE-1 voicing (docs §62) moves the base with its DEPTH knob and an unsmoothed
    // base delay is a pitch step. A settled OnePoleSmootherT<double> returns its
    // target BIT-EXACTLY (value_ == target_ short-circuits in next()), so an owner
    // that sets it once — every JC-120 caller — is unchanged to the last mantissa
    // bit. That is what keeps the clean120_chorus golden at ±0.00.
    OnePoleSmootherD baseSamples;
    OnePoleSmoother sweepSamples;  // depth -> sweep amplitude in samples
    OnePoleSmoother rateHz;        // speed -> LFO Hz
    double lfoPhase = 0.0;         // radians, [0, 2*pi)

    int mode = MODE_OFF;

    // Waveform as a SMOOTHED BLEND: 0.0 = sine, 1.0 = triangle.
    //
    // It has to be smoothed, and the reason was found by measurement rather than
    // reasoned about in advance (docs §62.8). The two shapes share their zero
    // crossings and their sign, so switching AT a zero crossing would be
    // seamless — but they do NOT agree anywhere in between: at phase pi/4 sine is
    // 0.7071 and triangle is 0.5000, so flipping the flag mid-cycle JUMPS the
    // read pointer by 0.207*A, which at a 1.4 ms sweep is ~14 samples at 48 kHz.
    // That is an audible click, and the CE-1's mode switch changes the waveform.
    // Blending on the same ~8 ms constant as everything else makes the delay
    // continuous through the change.
    //
    // The blend is a DOUBLE smoother so that the sine end is EXACT: an owner that
    // never asks for triangle holds blend == 0.0 and takes the `b <= 0.0` branch,
    // which is std::sin(phase) and nothing else. That is what keeps every JC-120
    // caller bit-identical — a `(1-b)*sin + b*tri` evaluated unconditionally
    // would also be exact at b == 0, but it would pay for the triangle on every
    // sample of every amp render for nothing.
    OnePoleSmootherD waveBlend;

    // Triangle normalised to +/-1 with sine's zero crossings and sign:
    // 0 at phase 0, +1 at pi/2, 0 at pi, -1 at 3pi/2. Its delay sweep makes the
    // PITCH deviation a SQUARE wave — two fixed detunings, not a wobble.
    static double triangle(double phase) {
        const double p = phase * (1.0 / kTwoPi);  // [0, 1)
        if (p < 0.25) return 4.0 * p;
        if (p < 0.75) return 2.0 - 4.0 * p;
        return 4.0 * p - 4.0;
    }

    static double lfo(double blend, double phase) {
        if (blend <= 0.0) return std::sin(phase);
        if (blend >= 1.0) return triangle(phase);
        return (1.0 - blend) * std::sin(phase) + blend * triangle(phase);
    }

    // 4-point Lagrange (cubic) fractional-delay read at delay D (in samples,
    // D >= 2 guaranteed by the base + sweep design). tap(k) reads the sample
    // written k samples ago relative to the CURRENT write index.
    float readCubic(double D) const {
        int id = static_cast<int>(std::floor(D));
        const double a = D - id;  // fractional part in [0, 1)
        auto tap = [&](int k) -> double {
            int idx = writeIdx - k;
            idx %= bufLen;
            if (idx < 0) idx += bufLen;
            return static_cast<double>(buf[static_cast<size_t>(idx)]);
        };
        const double xm1 = tap(id - 1);  // delay id-1 (newer)
        const double x0 = tap(id);       // delay id
        const double x1 = tap(id + 1);   // delay id+1 (older)
        const double x2 = tap(id + 2);   // delay id+2
        // Lagrange weights for evaluation point 'a' in [0,1] between x0 and x1.
        const double cm1 = -a * (a - 1.0) * (a - 2.0) / 6.0;
        const double c0 = (a + 1.0) * (a - 1.0) * (a - 2.0) / 2.0;
        const double c1 = -(a + 1.0) * a * (a - 2.0) / 2.0;
        const double c2 = (a + 1.0) * a * (a - 1.0) / 6.0;
        return static_cast<float>(cm1 * xm1 + c0 * x0 + c1 * x1 + c2 * x2);
    }
};

ChorusModel::ChorusModel() : impl_(std::make_unique<Impl>()) {}
ChorusModel::~ChorusModel() = default;

void ChorusModel::prepare(double sampleRate) {
    Impl& d = *impl_;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;

    const double maxDelayMs = kBaseMs + kMaxSweepMs + kGuardMs;
    d.bufLen = static_cast<int>(std::ceil(maxDelayMs * d.sampleRate / 1000.0)) + 4;
    if (d.bufLen < 8) d.bufLen = 8;
    d.buf.assign(static_cast<size_t>(d.bufLen), 0.0f);
    d.writeIdx = 0;
    d.lfoPhase = 0.0;

    d.baseSamples.prepare(kSmoothSeconds, d.sampleRate);
    d.waveBlend.prepare(kSmoothSeconds, d.sampleRate);
    d.sweepSamples.prepare(kSmoothSeconds, d.sampleRate);
    d.rateHz.prepare(kSmoothSeconds, d.sampleRate);
    // Defaults: the JC's fixed 5 ms base, no sweep, a gentle rate (all settable
    // before/after prepare). setImmediate snaps, so nothing ramps from a stale
    // value at (re)prepare time.
    d.baseSamples.setImmediate(kBaseMs * d.sampleRate / 1000.0);
    d.waveBlend.setImmediate(0.0);  // SINE — the JC-120 voicing
    d.sweepSamples.setImmediate(0.0f);
    d.rateHz.setImmediate(static_cast<float>(speedToHz(0.3f)));
}

void ChorusModel::reset() {
    Impl& d = *impl_;
    std::fill(d.buf.begin(), d.buf.end(), 0.0f);
    d.writeIdx = 0;
    d.lfoPhase = 0.0;
    // Snap the two control smoothers onto their targets too — a poisoned smoother
    // value never recovers on its own, and a NaN sweep would immediately re-poison
    // the delay line we just cleared (audit finding 1).
    d.baseSamples.reset();
    d.waveBlend.reset();
    d.sweepSamples.reset();
    d.rateHz.reset();
}

void ChorusModel::setSpeed(float knob01) {
    impl_->rateHz.setTarget(static_cast<float>(speedToHz(knob01)));
}

void ChorusModel::setDepth(float knob01) {
    // Squared taper: the knob's lower half stays in subtle-shimmer territory
    // while the upper half opens into the strong JC wobble (field feedback:
    // a linear map into the wider sweep made low settings too hot).
    const double k = clamp01(knob01);
    const double sweep = k * k * kMaxSweepMs * impl_->sampleRate / 1000.0;
    impl_->sweepSamples.setTarget(static_cast<float>(sweep));
}

void ChorusModel::setMode(int mode) {
    impl_->mode = mode < MODE_OFF ? MODE_OFF : (mode > MODE_VIBRATO ? MODE_VIBRATO : mode);
}

int ChorusModel::mode() const { return impl_->mode; }

// --- Direct voicing seam (docs §62) -----------------------------------------
// Milliseconds and hertz, not knob positions. Non-finite values are rejected
// outright (ParamGuard convention): a NaN base delay would index the ring buffer
// with a NaN and poison every subsequent read, and unlike a knob there is no
// meaningful clamp target for "not a number".

void ChorusModel::setBaseDelayMs(double ms) {
    if (!std::isfinite(ms)) return;
    if (ms < 0.0) ms = 0.0;
    impl_->baseSamples.setTarget(ms * impl_->sampleRate / 1000.0);
}

void ChorusModel::setSweepMs(double ms) {
    if (!std::isfinite(ms)) return;
    if (ms < 0.0) ms = 0.0;
    impl_->sweepSamples.setTarget(static_cast<float>(ms * impl_->sampleRate / 1000.0));
}

void ChorusModel::setRateHz(double hz) {
    if (!std::isfinite(hz)) return;
    if (hz < 0.0) hz = 0.0;
    impl_->rateHz.setTarget(static_cast<float>(hz));
}

void ChorusModel::setWaveform(int w) {
    impl_->waveBlend.setTarget(w == WAVE_TRIANGLE ? 1.0 : 0.0);
}

double ChorusModel::currentRateHz() const {
    return static_cast<double>(impl_->rateHz.value());
}

double ChorusModel::maxAbsDelayLine() const {
    double m = 0.0;
    for (float v : impl_->buf) {
        const double a = std::fabs(static_cast<double>(v));
        if (a > m) m = a;
    }
    return m;
}

void ChorusModel::processStereo(const float* in, float* outL, float* outR, int numFrames) {
    Impl& d = *impl_;

    if (d.mode == MODE_OFF) {
        // Bit-exact passthrough to both sides; leave the delay line untouched so
        // OFF is deterministic and L == R exactly. (We still keep the ring buffer
        // primed for a click-reduced re-engage by writing the input through it.)
        for (int i = 0; i < numFrames; ++i) {
            d.buf[static_cast<size_t>(d.writeIdx)] = in[i];
            if (++d.writeIdx >= d.bufLen) d.writeIdx = 0;
            outL[i] = in[i];
            outR[i] = in[i];
        }
        // Advance smoothers so a later engage starts from the right depth/rate.
        for (int i = 0; i < numFrames; ++i) {
            d.baseSamples.next();
            d.sweepSamples.next();
            d.rateHz.next();
            d.waveBlend.next();
        }
        return;
    }

    const bool dryLeft = (d.mode == MODE_CHORUS);  // CHORUS: dry L / wet R
    for (int i = 0; i < numFrames; ++i) {
        // Write the current input into the ring buffer.
        d.buf[static_cast<size_t>(d.writeIdx)] = in[i];

        const double base = d.baseSamples.next();
        const double sweep = d.sweepSamples.next();
        const double rate = d.rateHz.next();

        // Modulated delay in samples. lfo(phase) in [-1,1]; base ± sweep keeps it
        // well inside the buffer and >= a couple samples (interpolator floor).
        double delay = base + sweep * Impl::lfo(d.waveBlend.next(), d.lfoPhase);
        if (delay < 2.0) delay = 2.0;
        if (delay > d.bufLen - 3) delay = d.bufLen - 3;

        const float wet = d.readCubic(delay);

        if (dryLeft) {
            outL[i] = in[i];  // dry
            outR[i] = wet;    // modulated
        } else {              // VIBRATO: both sides the modulated signal
            outL[i] = wet;
            outR[i] = wet;
        }

        // Advance write pointer and LFO phase.
        if (++d.writeIdx >= d.bufLen) d.writeIdx = 0;
        d.lfoPhase += kTwoPi * rate / d.sampleRate;
        if (d.lfoPhase >= kTwoPi) d.lfoPhase -= kTwoPi;
    }
}

}  // namespace clipper::dsp
