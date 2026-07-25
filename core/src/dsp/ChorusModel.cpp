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

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

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

    double baseSamples = 0.0;     // kBaseMs in samples
    OnePoleSmoother sweepSamples;  // depth -> sweep amplitude in samples
    OnePoleSmoother rateHz;        // speed -> LFO Hz
    double lfoPhase = 0.0;         // radians, [0, 2*pi)

    int mode = MODE_OFF;

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

    d.baseSamples = kBaseMs * d.sampleRate / 1000.0;
    const double maxDelayMs = kBaseMs + kMaxSweepMs + kGuardMs;
    d.bufLen = static_cast<int>(std::ceil(maxDelayMs * d.sampleRate / 1000.0)) + 4;
    if (d.bufLen < 8) d.bufLen = 8;
    d.buf.assign(static_cast<size_t>(d.bufLen), 0.0f);
    d.writeIdx = 0;
    d.lfoPhase = 0.0;

    d.sweepSamples.prepare(kSmoothSeconds, d.sampleRate);
    d.rateHz.prepare(kSmoothSeconds, d.sampleRate);
    // Defaults: no sweep, a gentle rate (settable before/after prepare).
    d.sweepSamples.setImmediate(0.0f);
    d.rateHz.setImmediate(static_cast<float>(speedToHz(0.3f)));
}

void ChorusModel::reset() {
    Impl& d = *impl_;
    std::fill(d.buf.begin(), d.buf.end(), 0.0f);
    d.writeIdx = 0;
    d.lfoPhase = 0.0;
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
            d.sweepSamples.next();
            d.rateHz.next();
        }
        return;
    }

    const bool dryLeft = (d.mode == MODE_CHORUS);  // CHORUS: dry L / wet R
    for (int i = 0; i < numFrames; ++i) {
        // Write the current input into the ring buffer.
        d.buf[static_cast<size_t>(d.writeIdx)] = in[i];

        const double sweep = d.sweepSamples.next();
        const double rate = d.rateHz.next();

        // Modulated delay in samples. sin(phase) in [-1,1]; base ± sweep keeps it
        // well inside the buffer and >= a couple samples (interpolator floor).
        double delay = d.baseSamples + sweep * std::sin(d.lfoPhase);
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
