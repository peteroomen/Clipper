// Clipper — DelayModel (M13.4). The "Echoman" BBD analog delay.
// See DelayModel.h for the three things that had to be modelled rather than
// approximated, and docs/DEVELOPMENT.md §60 for the research record (sources AND
// the explicit list of what could not be sourced from this container).
//
// ---------------------------------------------------------------------------
// THE SIGNAL PATH, and where each number comes from.
//
//   in --> [input HP 60 Hz] --+--> COMPRESSOR (NE570, 2:1)
//                             ^            |
//                             |            v
//                        FEEDBACK    [anti-alias LPF, FIXED 9.9 kHz, 4-pole]
//                             ^            |
//                             |            v
//                             |     [BBD soft compliance limit]
//                             |            |
//                             |            v
//                             |     [S/H at f_clk] -> [charge smear] -> [delay
//                             |      = 4096 / f_clk seconds]
//                             |            |
//                             |            v
//                             |     [reconstruction LPF, FIXED 9.5 kHz, 4-pole]
//                             |            |
//                             |            v
//                             +----- EXPANDER (NE570, 1:2) --> [output HP] --> wet
//
//   out = dry + BLEND * wet          (dry stays at BASE rate and is untouched)
//
// 1. THE DEVICE. Two MN3005s in series. The MN3005 is a 4096-STAGE BBD and a
//    charge packet advances one stage per clock HALF-cycle, so the delay of an
//    N-stage device is N/(2*f_clk) — i.e. the device holds N/2 SAMPLE SLOTS
//    clocked at f_clk. Two of them is kStages = 8192 stages = kSlots = 4096
//    slots, and the delay is 4096/f_clk.
//
//    That formula is not asserted, it is CHECKED against the datasheet's own
//    endpoints: Panasonic/Xvive quote the MN3005 at 20.48-204.8 ms over a
//    10-100 kHz clock, and 4096/(2*10e3) = 204.8 ms, 4096/(2*100e3) = 20.48 ms.
//    Both, exactly.
//
// 2. THE CLOCK RANGE IS OUTSIDE THE DATASHEET AT BOTH ENDS, and that is
//    reported rather than clamped away. EHX publish 30-550 ms for the Deluxe
//    Memory Man; with 4096 slots that is f_clk = 136.5 kHz down to 7.45 kHz,
//    against the MN3005's rated 100 kHz / 10 kHz. The real pedal genuinely runs
//    its BBDs past both ends of the spec sheet; the published TIME range is the
//    stronger source, so it wins and the excursion is documented here.
//
// 3. THE DELAY KNOB IS LINEAR IN TIME, DERIVED. The CD4047 astable's frequency
//    is f = 1/(4.4*R*C), so f_clk is inversely proportional to the timing
//    resistance and the DELAY TIME is directly proportional to it. A linear pot
//    in series with a minimum resistor therefore gives a delay time LINEAR in
//    rotation. That is what ships; it is a consequence of the oscillator, not a
//    taste choice.
//
// 4. THE CHARGE SMEAR IS THE DARKENING, AND IT IS DERIVED FROM THE DEVICE.
//    A BBD stage does not transfer all of its charge: a fraction eps is left
//    behind and mixes into the following packet. One stage is
//        y[n] = (1-eps) x[n] + eps y[n-1]
//    and N of them cascade to H(z) = [(1-eps)/(1-eps z^-1)]^N. For small eps,
//        log H = N[log(1-eps) - log(1-eps z^-1)] ~= -N*eps*(1 - z^-1)
//    so  H(z) ~= exp(-mu (1 - z^-1)) with mu = N*eps in STAGE-TRANSFER steps,
//    i.e. mu/2 SAMPLE SLOTS (two transfers per slot). That is a Poisson kernel
//    h[k] = e^-mu mu^k / k! — a real, physically-shaped charge lag, applied ONCE
//    PER SLOT, i.e. AT THE CLOCK. Its corner is therefore a fixed FRACTION of
//    f_clk, so it falls in Hz exactly as the clock slows: -3 dB at 0.2254*f_clk
//    (solve exp(-2 mu_slot (1-cos w)) = 1/2 for mu_slot = 0.4096).
//
//    eps could NOT be sourced for the MN3005 specifically. The BBD literature
//    reachable from here gives "transfer efficiencies of 0.9998 at 5 MHz, and at
//    lower rates an efficiency of 0.99995 is not uncommon"; audio BBDs run at
//    10-200 kHz, i.e. squarely in the "lower rates" band. kCte = 0.9999 is the
//    commonly quoted mid-range figure and is a STATED MODELLING PARAMETER, not a
//    fit to a sound. Its consequence is measured and reported in docs §60.4.
//
// 5. THE FILTERS ARE FIXED AND THEIR CUTOFFS ARE BORROWED, WHICH IS A GAP.
//    The DMM's own filter schematic could not be read (every schematic host 403s
//    from this container — §60.1). The cutoffs used are the ones Holters & Parker
//    published for the JUNO-60 chorus's BBD filter pair (9900 Hz in / 9500 Hz
//    out), read out of the chowdsp_utils implementation of their model, which
//    clones from GitHub. They are a real BBD design point from a real instrument,
//    they are NOT the Memory Man's, and nothing here was tuned to them.
//    4th-order Butterworth (two cascaded biquads) because the real filters are
//    multi-pole active sections, not RC cascades.
//
// 6. THE COMPANDER IS THE NE570's ACTUAL LAW. The gain cell sits in the op-amp
//    feedback path for the compressor and in the forward path for the expander,
//    which makes the pair a square-root / square:
//        compressor  env_out = sqrt(Vref * env_in)     (2:1 in dB)
//        expander    env_out = env_in^2 / Vref         (1:2 in dB)
//    Composed with the same envelope they are EXACTLY unity at every level — the
//    algebra is the datasheet's claim, and the test suite checks it. Each half
//    has its own full-wave rectifier averaged by R*C; the NE570's rectifier
//    resistor is the internal 10 kOhm and the application notes' typical C_RECT
//    is 1 uF, so tau = 10 ms. THE TWO RECTIFIERS SEE DIFFERENT SIGNALS (the
//    expander's is the delayed, degraded one) and that is the breathing.
//
// 7. WHAT BOUNDS THE FEEDBACK LOOP IS THE DEVICE, NOT A LIMITER. The BBD's
//    bucket has finite capacity; past it the packet clips. kBbdClipVolts is that
//    compliance, applied as a smooth tanh at the BBD input. At FEEDBACK max the
//    loop gain is under unity AND the device saturates, so the 30 s peak is
//    bounded — measured in docs §60.6.
// ---------------------------------------------------------------------------

#include "clipper/dsp/DelayModel.h"

#include "clipper/dsp/Biquad.h"
#include "clipper/dsp/DelayLine.h"
#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/ParamGuard.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace clipper::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

// --- The device -------------------------------------------------------------
// Two MN3005s in series. 4096 stages each; a packet moves one stage per clock
// half-cycle, so the pair holds kSlots = kStages/2 samples at f_clk.
constexpr int kStages = 8192;
constexpr int kSlots = kStages / 2;  // 4096

// Charge transfer efficiency per stage (see banner note 4). NOT sourced for the
// MN3005 specifically; the literature's "lower rate" band is 0.9998..0.99995.
constexpr double kCte = 0.9999;
constexpr double kEps = 1.0 - kCte;
// Poisson mean of the charge-lag kernel, in SAMPLE SLOTS.
constexpr double kSmearMu = kStages * kEps * 0.5;  // 0.4096
constexpr int kSmearTaps = 8;  // e^-mu mu^7/7! < 1e-8 at mu = 0.41 — well past exact

// --- The knob travel --------------------------------------------------------
// EHX's published Deluxe Memory Man spec. Both ends are outside the MN3005's own
// datasheet clock range; see banner note 2.
constexpr double kMinDelaySec = 0.030;
constexpr double kMaxDelaySec = 0.550;

// --- The fixed analog filters (banner note 5) -------------------------------
constexpr double kAntiAliasHz = 9900.0;
constexpr double kReconstructHz = 9500.0;
// 4th-order Butterworth section Qs.
constexpr double kButterQ1 = 0.541196100146197;
constexpr double kButterQ2 = 1.306562964876377;

// Input / output coupling high-passes. The DMM's delay path is AC coupled at
// several points; one high-pass per side at 60 Hz stands for the lot, and it is
// what makes the repeats thin out at the bottom the way the real ones do.
constexpr double kPathHpHz = 60.0;

// --- The compander (banner note 6) -----------------------------------------
// NE570 rectifier: internal 10 kOhm with the application notes' typical 1 uF.
constexpr double kRectTauSec = 0.010;
// Unity level. The compander pair is unity at EVERY level by construction (the
// sqrt and the square compose to identity), so this only sets WHERE the BBD's
// nominal drive lands: at kCompRefVolts in, the device sees kCompRefVolts. It is
// set to a normal single-coil pluck peak so the shipped pedal drives the bucket
// at about a third of its compliance, leaving the pick attack room.
constexpr double kCompRefVolts = 0.15;
// The gain cell's compliance. A real NE570 cannot make infinite gain on silence
// (I_B = 140 uA sets the ceiling) and cannot make zero on the way out.
constexpr double kCompGainMax = 10.0;   // +20 dB
constexpr double kExpGainMin = 0.01;    // -40 dB
constexpr double kExpGainMax = 4.0;

// --- The bucket's compliance (banner note 7) -------------------------------
constexpr double kBbdClipVolts = 0.45;

// --- Loop and mix ----------------------------------------------------------
// Below unity by construction: the pedal is a delay, not an oscillator, and the
// brief's bar is that FEEDBACK max stays bounded over 30 s.
constexpr double kFeedbackMax = 0.92;
// Wet trim at BLEND 1. The expander returns the level the compressor took away,
// so the wet path is already near unity; this is the pedal's own wet pot ceiling.
constexpr double kWetGain = 1.0;

// Knob smoothing. DELAY smooths the CLOCK, not the delay time, because the clock
// is what the pot physically moves — and because a clock that glides is what
// makes a swept delay bend pitch like tape. 25 ms is longer than the house 5-8 ms
// because the quantity behind it is a FREQUENCY spanning 18:1: at the house
// constant a fast knob move slews the read pointer hard enough to chirp. Measured
// against the signal's own slew in `testNoZipperOnDelaySweep`.
constexpr double kClockSmoothSec = 0.025;
constexpr double kMixSmoothSec = 0.008;

// OVERSAMPLING IS SET BY THE DEVICE, NOT BY TASTE, and it is 8x rather than the
// house 4x. The BBD's clock reaches 136.53 kHz at the 30 ms end (banner note 2),
// so the internal rate has to clear 2 x 136.53 = 273.07 kHz or the DEVICE'S OWN
// clock images fold inside the oversampled domain before the reconstruction
// filter can reach them. 4x at 44.1 kHz is 176.4 kHz and does NOT clear it; 8x is
// 352.8 kHz and does. Measured, at DELAY 0.0 on a 3 kHz tone: the worst
// non-harmonic product is -65.5 dB at 4x and -122.1 dB at 8x, for 2.9 % -> 5.4 %
// of one 48 kHz stream (docs §60.5). The dry path is untouched either way, so
// this costs no latency at all.
constexpr int kDefaultOversampling = 8;
// The delay line is sized once, in prepare(), for the longest delay at the
// HIGHEST factor this instance could ever be switched to. setOversampling() is
// then allocation-free, and so is every DELAY move.
constexpr int kMaxOversampling = 8;

// Knob 0..1 -> delay seconds. Linear (banner note 3).
double knobToDelaySec(double k) {
    return kMinDelaySec + clampParam01(k) * (kMaxDelaySec - kMinDelaySec);
}

// Delay seconds -> BBD clock. The device's own law.
double delaySecToClockHz(double sec) {
    return static_cast<double>(kSlots) / sec;
}

// Two-biquad 4th-order Butterworth lowpass in DOUBLE, with a WHOLE-STATE
// denormal flush.
//
// docs §56.4b: the house one-liner `z = flushDenormal(z)` guards only the newest
// tap and does NOT converge for a recursion of order >= 2 — the older state gets
// re-injected and the filter parks above the floor forever. A TDF2 biquad is
// exactly that shape (z1 and z2 feed each other through y), so each section
// tests BOTH taps and zeroes them as a unit. It also carries `double` state, for
// §56's other reason: these filters sit inside a FEEDBACK loop at up to 768 kHz,
// where a float direct form's own state rounding becomes an audible floor.
struct Butter4 {
    struct Sec {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;
        inline double process(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            const double m = std::max(std::fabs(z1), std::fabs(z2));
            if (m < static_cast<double>(kDenormalFloor)) { z1 = 0.0; z2 = 0.0; }
            return y;
        }
        void reset() { z1 = z2 = 0.0; }
        double maxState() const { return std::max(std::fabs(z1), std::fabs(z2)); }
    };

    Sec s1, s2;

    void design(double fc, double fs) {
        setLp(s1, fc, kButterQ1, fs);
        setLp(s2, fc, kButterQ2, fs);
    }
    void designHighpass(double fc, double fs) {
        // One 2nd-order Butterworth section is enough for the coupling network;
        // the second section is made a pass-through so the struct stays one type.
        setHp(s1, fc, 0.70710678118654752, fs);
        s2 = Sec{};
    }
    inline double process(double x) { return s2.process(s1.process(x)); }
    void reset() { s1.reset(); s2.reset(); }
    double maxState() const { return std::max(s1.maxState(), s2.maxState()); }

private:
    static void setLp(Sec& s, double fc, double q, double fs) {
        const double f = std::min(fc, 0.45 * fs);
        const double w0 = 2.0 * kPi * f / fs;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        const double b0 = (1.0 - cw) * 0.5, b1 = 1.0 - cw, b2 = (1.0 - cw) * 0.5;
        const double a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha;
        s.b0 = b0 / a0; s.b1 = b1 / a0; s.b2 = b2 / a0;
        s.a1 = a1 / a0; s.a2 = a2 / a0;
    }
    static void setHp(Sec& s, double fc, double q, double fs) {
        const double f = std::min(fc, 0.45 * fs);
        const double w0 = 2.0 * kPi * f / fs;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        const double b0 = (1.0 + cw) * 0.5, b1 = -(1.0 + cw), b2 = (1.0 + cw) * 0.5;
        const double a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha;
        s.b0 = b0 / a0; s.b1 = b1 / a0; s.b2 = b2 / a0;
        s.a1 = a1 / a0; s.a2 = a2 / a0;
    }
};

}  // namespace

struct DelayModel::Impl {
    double sampleRate = 44100.0;
    int maxBlock = 128;

    Oversampler os;
    DelayLine line;

    // The dry signal, captured before anything writes to `out` — the C ABI allows
    // `in` and `out` to ALIAS, and the wet path downsamples straight into `out`.
    std::vector<float> dry;

    // Knob state. The clock is what is smoothed (see kClockSmoothSec).
    // DELAY and FEEDBACK are advanced PER OVERSAMPLED SAMPLE (they are inside the
    // loop), BLEND per base sample (it is the output mix).
    OnePoleSmootherD clock;   // Hz
    OnePoleSmootherD feedback;
    OnePoleSmootherD blend;
    double delayKnob = 0.35;

    // Fixed analog filters, both at the OVERSAMPLED rate.
    Butter4 antiAlias;     // before the device
    Butter4 reconstruct;   // after the device
    Butter4 inputHp;
    Butter4 outputHp;

    // Compander envelopes (full-wave rectifier + R*C average). Both rest at 0.
    double envIn = 0.0;
    double envOut = 0.0;
    double rectCoef = 0.0;
    double curCompGain = 1.0;
    double curExpGain = 1.0;

    // The BBD clock: a phase accumulator, a sample-and-hold, and the charge-lag
    // kernel evaluated once per slot.
    double clockPhase = 0.0;
    double held = 0.0;
    double tickHist[kSmearTaps] = {0.0};
    int tickPos = 0;
    double smearKernel[kSmearTaps] = {0.0};

    double fbSample = 0.0;  // one internal sample of loop latency

    double curClockHz = 0.0;

    void designFilters() {
        const double fsi = sampleRate * static_cast<double>(os.factor());
        antiAlias.design(kAntiAliasHz, fsi);
        reconstruct.design(kReconstructHz, fsi);
        inputHp.designHighpass(kPathHpHz, fsi);
        outputHp.designHighpass(kPathHpHz, fsi);
        rectCoef = 1.0 - std::exp(-1.0 / (kRectTauSec * fsi));
    }

    void buildSmearKernel() {
        // Poisson h[k] = e^-mu mu^k / k!, normalised so DC gain is exactly 1 (the
        // truncation at kSmearTaps otherwise loses ~1e-8 of the charge).
        double sum = 0.0;
        double term = std::exp(-kSmearMu);
        for (int k = 0; k < kSmearTaps; ++k) {
            smearKernel[k] = term;
            sum += term;
            term *= kSmearMu / static_cast<double>(k + 1);
        }
        for (int k = 0; k < kSmearTaps; ++k) smearKernel[k] /= sum;
    }

    // Smooth compliance limit of the bucket. Odd, so it adds no DC.
    static inline double bucketLimit(double v) {
        return kBbdClipVolts * std::tanh(v / kBbdClipVolts);
    }
};

DelayModel::DelayModel() : impl_(std::make_unique<Impl>()) {}
DelayModel::~DelayModel() = default;

void DelayModel::prepare(double sampleRate, int maxBlockSize) {
    Impl& d = *impl_;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    d.maxBlock = maxBlockSize > 0 ? maxBlockSize : 128;

    d.os.prepare(d.maxBlock);
    d.os.setFactor(kDefaultOversampling);

    // THE ONE ALLOCATION. Sized for the longest delay at the highest factor this
    // instance could ever be switched to, so setOversampling() and every DELAY
    // move afterwards touch nothing but a read offset. A little headroom past
    // kMaxDelaySec covers the interpolator and the clock smoother's overshoot-free
    // approach from a longer setting.
    d.line.prepare(kMaxDelaySec * 1.05,
                   d.sampleRate * static_cast<double>(kMaxOversampling));

    d.dry.assign(static_cast<size_t>(d.maxBlock), 0.0f);

    d.clock.prepare(kClockSmoothSec, d.sampleRate * kDefaultOversampling);
    d.feedback.prepare(kMixSmoothSec, d.sampleRate * kDefaultOversampling);
    d.blend.prepare(kMixSmoothSec, d.sampleRate);
    d.clock.setImmediate(delaySecToClockHz(knobToDelaySec(d.delayKnob)));
    d.feedback.setImmediate(0.0);
    d.blend.setImmediate(0.0);

    d.designFilters();
    d.buildSmearKernel();
    reset();
}

void DelayModel::reset() {
    Impl& d = *impl_;
    d.line.reset();
    d.os.reset();
    d.antiAlias.reset();
    d.reconstruct.reset();
    d.inputHp.reset();
    d.outputHp.reset();
    d.envIn = 0.0;
    d.envOut = 0.0;
    d.curCompGain = 1.0;
    d.curExpGain = 1.0;
    d.clockPhase = 0.0;
    d.held = 0.0;
    for (int i = 0; i < kSmearTaps; ++i) d.tickHist[i] = 0.0;
    d.tickPos = 0;
    d.fbSample = 0.0;
    // Re-snap the knob smoothers: a poisoned smoother value never recovers on its
    // own and would immediately re-poison the line we just cleared (finding 1).
    d.clock.reset();
    d.feedback.reset();
    d.blend.reset();
}

void DelayModel::setOversampling(int factor) {
    Impl& d = *impl_;
    const int before = d.os.factor();
    d.os.setFactor(factor);
    if (d.os.factor() != before) {
        // Everything in the wet path runs at the oversampled rate, so the filters
        // and the rectifier constant are redesigned. The delay line is NOT
        // resized — prepare() already sized it for kMaxOversampling.
        const double fsi = d.sampleRate * static_cast<double>(d.os.factor());
        const double fbTarget = d.feedback.target();
        d.clock.prepare(kClockSmoothSec, fsi);
        d.feedback.prepare(kMixSmoothSec, fsi);
        d.clock.setImmediate(delaySecToClockHz(knobToDelaySec(d.delayKnob)));
        d.feedback.setImmediate(fbTarget);
        d.designFilters();
        reset();
    }
}

int DelayModel::oversampling() const { return impl_->os.factor(); }

// See the header: the DRY path never enters the oversampled domain.
int DelayModel::latencySamples() const { return 0; }

void DelayModel::setParameter(int paramId, float value) {
    Impl& d = *impl_;
    switch (paramId) {
        case PARAM_DELAY: {
            d.delayKnob = clampParam01(static_cast<double>(value));
            d.clock.setTarget(delaySecToClockHz(knobToDelaySec(d.delayKnob)));
            break;
        }
        case PARAM_FEEDBACK:
            d.feedback.setTarget(clampParam01(static_cast<double>(value)) * kFeedbackMax);
            break;
        case PARAM_BLEND:
            d.blend.setTarget(clampParam01(static_cast<double>(value)) * kWetGain);
            break;
        default:
            break;
    }
}

double DelayModel::clockHz() const {
    const Impl& d = *impl_;
    return d.curClockHz > 0.0 ? d.curClockHz : d.clock.value();
}

double DelayModel::delaySeconds() const {
    const double f = clockHz();
    return f > 0.0 ? static_cast<double>(kSlots) / f : 0.0;
}

double DelayModel::minDelaySeconds() { return kMinDelaySec; }
double DelayModel::maxDelaySeconds() { return kMaxDelaySec; }
int DelayModel::bbdSlots() { return kSlots; }

double DelayModel::compressorGain() const { return impl_->curCompGain; }
double DelayModel::expanderGain() const { return impl_->curExpGain; }

double DelayModel::chargeSmearCornerHz() const {
    // -3 dB of exp(-2 mu (1 - cos w)) — the closed form in banner note 4.
    const double c = 1.0 - std::log(2.0) / (2.0 * kSmearMu);
    if (c <= -1.0) return 0.5 * clockHz();  // never reaches -3 dB below Nyquist
    const double w = std::acos(c);
    return w / (2.0 * kPi) * clockHz();
}

double DelayModel::maxAbsRestingState() const {
    const Impl& d = *impl_;
    double m = d.line.maxAbsState();
    m = std::max(m, d.antiAlias.maxState());
    m = std::max(m, d.reconstruct.maxState());
    m = std::max(m, d.inputHp.maxState());
    m = std::max(m, d.outputHp.maxState());
    m = std::max(m, std::fabs(d.envIn));
    m = std::max(m, std::fabs(d.envOut));
    m = std::max(m, std::fabs(d.held));
    m = std::max(m, std::fabs(d.fbSample));
    for (int i = 0; i < kSmearTaps; ++i) m = std::max(m, std::fabs(d.tickHist[i]));
    return m;
}

void DelayModel::process(const float* in, float* out, int numFrames) {
    Impl& d = *impl_;
    if (numFrames <= 0) return;

    // The Oversampler's scratch is sized for maxBlock in prepare(); a host handing
    // us more is chunked here. Chunking is exact — every stage is sample-recursive.
    if (numFrames > d.maxBlock) {
        int off = 0;
        while (off < numFrames) {
            const int n = std::min(d.maxBlock, numFrames - off);
            process(in + off, out + off, n);
            off += n;
        }
        return;
    }

    const int factor = d.os.factor();
    const double fsi = d.sampleRate * static_cast<double>(factor);
    const int n = numFrames * factor;

    // `in` and `out` may ALIAS, and the wet path downsamples straight into `out`,
    // so the dry signal is captured first. This copy is also what makes BLEND 0
    // return the caller's own samples bit-for-bit.
    for (int i = 0; i < numFrames; ++i) d.dry[static_cast<size_t>(i)] = in[i];

    // --- WET PATH (oversampled) ---------------------------------------------
    d.os.upsample(in, numFrames);
    float* w = d.os.buffer();

    for (int i = 0; i < n; ++i) {
        const double x = static_cast<double>(w[i]);

        // Clock first: the DELAY pot moves this and NOTHING else in the path.
        const double fclk = d.clock.next();
        d.curClockHz = fclk;

        // Feedback re-enters at the compressor's input, so every repeat is
        // companded, band-limited and charge-smeared AGAIN — the degradation is
        // cumulative by construction, not by a filter placed in the loop.
        const double fb = d.feedback.next();
        const double drive = d.inputHp.process(x + fb * d.fbSample);

        // --- Compressor (NE570, 2:1). Its own rectifier, on its own input.
        d.envIn = flushDenormal(d.envIn + d.rectCoef * (std::fabs(drive) - d.envIn));
        double gc = std::sqrt(kCompRefVolts / std::max(d.envIn, 1e-9));
        if (gc > kCompGainMax) gc = kCompGainMax;
        d.curCompGain = gc;

        // --- Fixed anti-alias filter, then the bucket's compliance.
        const double pre = Impl::bucketLimit(d.antiAlias.process(drive * gc));

        // --- The BBD. A sampled device: the input is held at the clock, the held
        // value carries the charge-lag kernel, and the delay is 4096 slots at
        // whatever the clock currently is.
        d.clockPhase += fclk / fsi;
        while (d.clockPhase >= 1.0) {
            d.clockPhase -= 1.0;
            d.tickPos = (d.tickPos + 1) & (kSmearTaps - 1);
            d.tickHist[d.tickPos] = pre;
            double s = 0.0;
            for (int k = 0; k < kSmearTaps; ++k) {
                s += d.smearKernel[k] * d.tickHist[(d.tickPos - k) & (kSmearTaps - 1)];
            }
            d.held = flushDenormal(s);
        }

        // Read BEFORE write (DelayLine's documented order). The read distance is
        // the device's own law expressed in internal samples, so it is exact and
        // fractional and it GLIDES when the clock does — which is why sweeping
        // DELAY bends pitch, exactly as a real BBD (or a tape machine) does.
        double delaySamples = static_cast<double>(kSlots) / fclk * fsi;
        const double dMax = d.line.maxDelaySamples();
        if (delaySamples > dMax) delaySamples = dMax;
        const double bbdOut = static_cast<double>(d.line.readCubic(delaySamples));
        d.line.write(static_cast<float>(d.held));

        // --- Fixed reconstruction filter, then the expander (NE570, 1:2). ITS
        // rectifier sees the DELAYED, degraded signal — the two envelopes cannot
        // agree, and that disagreement is the breathing.
        const double post = d.reconstruct.process(bbdOut);
        d.envOut = flushDenormal(d.envOut + d.rectCoef * (std::fabs(post) - d.envOut));
        double ge = d.envOut / kCompRefVolts;
        if (ge < kExpGainMin) ge = kExpGainMin;
        if (ge > kExpGainMax) ge = kExpGainMax;
        d.curExpGain = ge;

        const double wet = d.outputHp.process(post * ge);
        // BOTH the loop tap and the emitted sample take the FLUSHED value, and the
        // second half of that is not cosmetic — `clipper_denormal_tests` caught it.
        // A ring-down that has fallen to ~1e-40 is a perfectly NORMAL double, so
        // nothing upstream flushes it, but `float(1e-40)` is SUBNORMAL and this is
        // where it leaves the model and enters the rest of the chain. Measured
        // before the fix: 5395 subnormal output samples over a 200 s tail at
        // DELAY 1.0 / FEEDBACK 0.8; after it, 0. (ADR 006 / docs §33.)
        d.fbSample = flushDenormal(wet);
        w[i] = static_cast<float>(d.fbSample);
    }

    d.os.downsample(out, numFrames);

    // --- MIX (base rate) ----------------------------------------------------
    // The dry signal never entered the oversampled domain, so BLEND 0 returns the
    // input BIT-IDENTICALLY (`b == 0` skips the add entirely, and the smoother is
    // parked at exactly 0 once settled). That is the transparency contract, and
    // `clipper_delay_tests` proves it by render hash.
    //
    // The mixed branch is FLUSHED on the way out, and that is the pedal's last
    // line of anti-denormal defence rather than a belt-and-braces guard. The
    // model's own states are all flushed, so the wet stream that enters the
    // oversampler is either exactly 0 or at least ~1e-30 — but the halfband's tap
    // sums CANCEL, and the difference of two ~1e-30 floats that agree to within a
    // ULP is ~6e-42, i.e. subnormal. Measured over a 200 s tail at DELAY 1.0 /
    // FEEDBACK 0.8: 1666 subnormal output samples without this flush, 0 with it.
    // The dry branch is deliberately NOT flushed — it is the caller's own signal
    // and BLEND 0 must return it untouched.
    for (int i = 0; i < numFrames; ++i) {
        const double b = d.blend.next();
        const float dry = d.dry[static_cast<size_t>(i)];
        out[i] = (b == 0.0) ? dry
                            : flushDenormal(static_cast<float>(
                                  static_cast<double>(dry) +
                                  b * static_cast<double>(out[i])));
    }
}

}  // namespace clipper::dsp
