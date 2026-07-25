// Plain-assert tests for clipper::dsp::PhaserModel (v1.1 item 3 — the script-era
// 4-stage phaser, "Ninety"). No framework: int main + <cassert>. Frequency
// content via a hand-rolled windowed Goertzel; the analytic reference is the
// EXACT discrete transfer function of the four first-order allpasses summed 50/50
// with the dry, built from the model's own published coefficient recipe (the
// static accessors) so the reference cannot drift from the implementation.
//
// Headline measurements (see docs/DEVELOPMENT.md §20):
//   (a) STATIC: at frozen LFO positions the RENDERED notch frequencies match the
//       analytic 4-stage allpass response (±6%); EXACTLY 2 notches in-band; and
//       the pair sits at ≈0.414·fc / ≈2.414·fc (the −8·atan physics).
//   (b) SWEEP: the notches TRACK the LFO over a cycle — measured at the quarter-
//       cycle points they rise 200→2000 Hz corner (trough→peak) and return.
//   (c) SPEED map endpoints: knob 0 → 0.06 Hz, knob 1 → 8 Hz, monotonic.
//   (d) MIX/DEPTH: the composite notch is DEEPER than 20 dB even WITH the
//       per-stage detune on (an allpass sum is never flat, so we assert depth,
//       not flatness).
//   (e) NO ZIPPER: a fast (8 Hz) sweep on a 1 kHz sine leaves the far-field
//       spectral floor (5–11 kHz) >80 dB down — smooth per-sample coefficients,
//       no turnaround tick.
//   (f) BYPASS/LEVEL sanity: silence → silence; unity sine stays bounded (≤~1.02
//       peak, allpass sum has unity-ish gain); a 0 dB response peak exists.
//   All at 44.1 k, 48 k, 96 k where a rate is swept.

#include "clipper/dsp/PhaserModel.h"

#include "support/AssertsLive.h"

#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::PhaserModel;

// Windowed Goertzel amplitude of a real tone at f over x[start, start+n).
double goertzelAmp(const std::vector<float>& x, size_t start, size_t n, double f,
                   double fs) {
    const double w = kTwoPi * f / fs;
    const double cw = std::cos(w), sw = std::sin(w), coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0, winSum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double win = 0.5 * (1.0 - std::cos(kTwoPi * i / (n - 1)));
        winSum += win;
        const double s0 = x[start + i] * win + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * sw;
    return 2.0 * std::sqrt(re * re + im * im) / winSum;
}

std::vector<float> sine(double f, float amp, double secs, double fs) {
    const int n = static_cast<int>(secs * fs);
    std::vector<float> s(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        s[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(kTwoPi * f * i / fs));
    return s;
}

// |H(f)| of the phaser FROZEN at a given LFO phase, measured by rendering a probe
// sine and reading its settled amplitude (the model is LTI while frozen).
double measMag(double f, double fs, double phase01, bool detune) {
    PhaserModel m;
    m.prepare(fs);
    m.setDetune(detune);
    m.setFrozen(true, phase01);
    const float amp = 0.25f;
    auto in = sine(f, amp, 0.4, fs);
    std::vector<float> out(in.size(), 0.0f);
    m.process(in.data(), out.data(), static_cast<int>(in.size()));
    const size_t n = out.size(), win = std::min(n, static_cast<size_t>(0.2 * fs));
    return goertzelAmp(out, n - win, win, f, fs) / amp;
}

// Analytic |Hmix(f)| for the frozen config: Hmix = 0.5·(1 + Π_k AP_k), each
// AP_k = (a_k + z⁻¹)/(1 + a_k z⁻¹), a_k = (t−1)/(t+1), t = tan(π·fc·detune_k/fs).
// Built from the model's own static recipe, so it can't drift from the code.
double analyticMag(double f, double fs, double phase01, bool detune) {
    const double fc = PhaserModel::cornerHzAtPhase(phase01);
    const std::complex<double> zinv = std::exp(std::complex<double>(0.0, -kTwoPi * f / fs));
    std::complex<double> prod(1.0, 0.0);
    for (int s = 0; s < PhaserModel::numStages(); ++s) {
        const double fcs = detune ? fc * PhaserModel::stageDetune(s) : fc;
        const double t = std::tan(kTwoPi * 0.5 * fcs / fs);
        const double a = (t - 1.0) / (t + 1.0);
        prod *= (a + zinv) / (1.0 + a * zinv);
    }
    return std::abs(0.5 * (std::complex<double>(1.0, 0.0) + prod));
}

double toDb(double m) { return 20.0 * std::log10(m + 1e-12); }

struct Notch { double freq, depthDb; };

// Find in-band notches of a magnitude function magAt(f): scan a log grid, take
// local minima below −6 dB, refine the frequency parabolically (in log-f).
std::vector<Notch> findNotches(double fs, double phase01, bool detune, bool analytic) {
    const double fLo = 40.0, fHi = std::min(9000.0, 0.45 * fs);
    const int N = 900;
    std::vector<double> lf(N), db(N);
    for (int i = 0; i < N; ++i) {
        const double t = static_cast<double>(i) / (N - 1);
        lf[i] = std::log(fLo) + t * (std::log(fHi) - std::log(fLo));
        const double f = std::exp(lf[i]);
        db[i] = toDb(analytic ? analyticMag(f, fs, phase01, detune)
                              : measMag(f, fs, phase01, detune));
    }
    std::vector<Notch> out;
    for (int i = 1; i < N - 1; ++i) {
        if (db[i] < db[i - 1] && db[i] <= db[i + 1] && db[i] < -6.0) {
            const double y0 = db[i - 1], y1 = db[i], y2 = db[i + 1];
            const double denom = (y0 - 2.0 * y1 + y2);
            const double delta = std::fabs(denom) > 1e-9 ? 0.5 * (y0 - y2) / denom : 0.0;
            const double dlog = lf[1] - lf[0];
            out.push_back({std::exp(lf[i] + delta * dlog), y1});
        }
    }
    return out;
}

// --- (a) static notches match the analytic response; exactly 2 in-band. -------
void testStaticNotches(double fs) {
    const double phases[] = {0.15, 0.25, 0.40};  // trough-ish, center, peak-ish
    for (double ph : phases) {
        auto meas = findNotches(fs, ph, /*detune=*/false, /*analytic=*/false);
        auto anal = findNotches(fs, ph, /*detune=*/false, /*analytic=*/true);
        assert(anal.size() == 2 && "analytic 4-stage sum must have EXACTLY 2 notches");
        assert(meas.size() == 2 && "rendered response must show exactly 2 notches");
        for (int k = 0; k < 2; ++k) {
            const double dev = std::fabs(meas[k].freq - anal[k].freq) / anal[k].freq;
            std::printf("  [static fs=%.0f ph=%.2f] notch%d meas=%.1f Hz anal=%.1f Hz (%.2f%%)\n",
                        fs, ph, k + 1, meas[k].freq, anal[k].freq, 100.0 * dev);
            assert(dev < 0.06 && "measured notch deviates >6% from analytic");
        }
        // Physics check: the pair sits at ≈0.414·fc and ≈2.414·fc.
        const double fc = PhaserModel::cornerHzAtPhase(ph);
        assert(std::fabs(anal[0].freq / fc - 0.4142) < 0.06 && "notch1 != ~0.414*fc");
        assert(std::fabs(anal[1].freq / fc - 2.4142) < 0.12 && "notch2 != ~2.414*fc");
    }
}

// --- (b) sweep: notches track the LFO across a cycle (quarter-cycle points). ---
void testSweepTracking(double fs) {
    // Upper notch at the four quarter-cycle phases. corner: 200 (trough) at ph0,
    // 632 (center) at ph0.25 & ph0.75, 2000 (peak) at ph0.5.
    auto upperNotch = [&](double ph) {
        auto n = findNotches(fs, ph, /*detune=*/false, /*analytic=*/false);
        assert(n.size() == 2);
        return n[1].freq;
    };
    const double n0 = upperNotch(0.0);    // corner 200
    const double n25 = upperNotch(0.25);  // corner 632
    const double n50 = upperNotch(0.50);  // corner 2000
    const double n75 = upperNotch(0.75);  // corner 632 (returning)
    std::printf("  [sweep fs=%.0f] upper notch ph0=%.0f ph.25=%.0f ph.5=%.0f ph.75=%.0f Hz\n",
                fs, n0, n25, n50, n75);
    assert(n0 < n25 && n25 < n50 && "upper notch must rise trough->center->peak");
    assert(std::fabs(n25 - n75) / n25 < 0.03 && "quarter points (same corner) must match");
    // Each tracks the LFO corner: notch2 ≈ 2.414·cornerHzAtPhase(ph).
    assert(std::fabs(n0 / (2.4142 * PhaserModel::cornerHzAtPhase(0.0)) - 1.0) < 0.08);
    assert(std::fabs(n50 / (2.4142 * PhaserModel::cornerHzAtPhase(0.5)) - 1.0) < 0.10);
}

// --- (c) SPEED map endpoints + monotonic. -------------------------------------
void testSpeedMap() {
    const double lo = PhaserModel::speedKnobToHz(0.0);
    const double hi = PhaserModel::speedKnobToHz(1.0);
    const double mid = PhaserModel::speedKnobToHz(0.5);
    std::printf("  [speed] knob0=%.3f Hz knob.5=%.3f Hz knob1=%.3f Hz\n", lo, mid, hi);
    assert(std::fabs(lo - 0.06) < 1e-6 && "speed knob 0 must map to 0.06 Hz");
    assert(std::fabs(hi - 8.0) < 1e-6 && "speed knob 1 must map to 8 Hz");
    assert(mid > lo && mid < hi && "speed map must be monotonic");
    // Log map: the geometric mean lands at the knob centre.
    assert(std::fabs(mid - std::sqrt(lo * hi)) < 1e-6 && "speed map must be log");
}

// --- (d) composite notch depth > 20 dB WITH detune on. ------------------------
void testNotchDepth(double fs) {
    const double phases[] = {0.1, 0.25, 0.45};
    for (double ph : phases) {
        auto n = findNotches(fs, ph, /*detune=*/true, /*analytic=*/false);
        assert(n.size() == 2 && "detuned response still has exactly 2 notches");
        for (int k = 0; k < 2; ++k) {
            std::printf("  [depth fs=%.0f ph=%.2f] notch%d = %.1f Hz depth %.1f dB\n",
                        fs, ph, k + 1, n[k].freq, n[k].depthDb);
            assert(n[k].depthDb < -20.0 && "notch depth must exceed 20 dB even detuned");
        }
    }
}

// --- (e) no zipper: fast sweep on a pure tone leaves a clean HF floor. ---------
void testNoZipper(double fs) {
    PhaserModel m;
    m.prepare(fs);
    m.setSpeed(1.0f);  // 8 Hz — the fastest, hardest-swept case
    const double fCar = 1000.0;
    const float amp = 0.3f;
    auto in = sine(fCar, amp, 1.0, fs);
    std::vector<float> out(in.size(), 0.0f);
    m.process(in.data(), out.data(), static_cast<int>(in.size()));
    // The model is LINEAR (allpass sum): a pure 1 kHz input can only produce
    // energy AT the carrier and its close modulation sidebands. Any appreciable
    // energy far above the carrier is coefficient-stepping zipper. Probe 5–11 kHz.
    const size_t n = out.size(), win = std::min(n, static_cast<size_t>(0.5 * fs));
    const double car = goertzelAmp(out, n - win, win, fCar, fs);
    const double probes[] = {5000.0, 7000.0, 9000.0, 11000.0};
    for (double f : probes) {
        if (f > 0.45 * fs) continue;
        const double a = goertzelAmp(out, n - win, win, f, fs);
        const double rel = toDb(a / (car + 1e-12));
        std::printf("  [zipper fs=%.0f] %.0f Hz floor = %.1f dB rel carrier\n", fs, f, rel);
        assert(rel < -80.0 && "HF spectral floor too high — coefficient zipper");
    }
}

// --- (f) bypass/level sanity. -------------------------------------------------
void testLevelSanity(double fs) {
    // Silence -> silence.
    {
        PhaserModel m;
        m.prepare(fs);
        m.setSpeed(0.5f);
        std::vector<float> in(static_cast<size_t>(0.1 * fs), 0.0f), out(in.size(), 0.0f);
        m.process(in.data(), out.data(), static_cast<int>(in.size()));
        for (float v : out) assert(std::fabs(v) < 1e-9 && "silence must stay silent");
    }
    // A unity-ish sine stays bounded (allpass sum: |Hmix| <= 1), and a 0 dB
    // response PEAK exists somewhere (the comb's peaks are unity).
    {
        double tailPeak = 0.0, maxMag = 0.0;
        PhaserModel m;
        m.prepare(fs);
        m.setFrozen(true, 0.25);
        // The mid comb PEAK sits at total phase −360°, i.e. exactly f = fc (the
        // corner), which is a ~0 dB point; include it so maxMag sees a peak.
        const double fcMid = PhaserModel::cornerHzAtPhase(0.25);
        for (double f : {60.0, 120.0, 200.0, 400.0, fcMid, 800.0, 1500.0, 3000.0, 6000.0}) {
            auto in = sine(f, 0.5f, 0.3, fs);
            std::vector<float> out(in.size(), 0.0f);
            m.process(in.data(), out.data(), static_cast<int>(in.size()));
            // Peak over the SETTLED tail (past the allpass onset overshoot) reflects
            // the steady comb gain; the transient onset is not a level fault.
            const size_t skip = std::min(out.size(), static_cast<size_t>(0.15 * fs));
            for (size_t i = skip; i < out.size(); ++i)
                tailPeak = std::max(tailPeak, static_cast<double>(std::fabs(out[i])));
            maxMag = std::max(maxMag, measMag(f, fs, 0.25, true));
        }
        std::printf("  [level fs=%.0f] tailPeak=%.4f (0.5 in) maxMag=%.4f\n", fs, tailPeak, maxMag);
        assert(tailPeak < 0.52 && "steady output must not exceed the ~unity allpass-sum gain");
        assert(maxMag > 0.95 && "a ~0 dB comb peak must exist");
    }
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();
    std::printf("PhaserModel tests\n");
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        std::printf("--- fs = %.0f Hz ---\n", fs);
        testStaticNotches(fs);
        testSweepTracking(fs);
        testNotchDepth(fs);
        testNoZipper(fs);
        testLevelSanity(fs);
    }
    testSpeedMap();
    std::printf("All PhaserModel tests passed.\n");
    return 0;
}
