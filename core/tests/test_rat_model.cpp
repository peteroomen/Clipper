// Plain-assert tests for clipper::dsp::RatModel (M1). No framework: int main +
// <cassert>. Frequency content is measured with a hand-rolled Goertzel (no FFT
// dependency). Each assert is designed to FAIL if the corresponding stage is
// broken (verified during development by temporarily bypassing stages).

#include "clipper/dsp/RatModel.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::RatModel;

// --- Hand-rolled Goertzel: amplitude estimate at frequency f (Hann-windowed).
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

double toDb(double amp) { return 20.0 * std::log10(amp + 1e-12); }

struct Params {
    float distortion, filter, level;
};

// Render a mono buffer through a fresh model.
std::vector<float> render(const std::vector<float>& in, Params p, double fs) {
    RatModel m;
    m.prepare(fs, 128);
    m.setParameter(RatModel::PARAM_DISTORTION, p.distortion);
    m.setParameter(RatModel::PARAM_FILTER, p.filter);
    m.setParameter(RatModel::PARAM_LEVEL, p.level);
    std::vector<float> out(in.size(), 0.0f);
    if (!in.empty())
        m.process(in.data(), out.data(), static_cast<int>(in.size()));
    return out;
}

std::vector<float> sine(double f, float amp, double secs, double fs) {
    const int n = static_cast<int>(secs * fs);
    std::vector<float> s(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        s[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(kTwoPi * f * i / fs));
    return s;
}

// Steady-state peak/RMS over the tail (skip the smoothing transient).
double tailPeak(const std::vector<float>& x, double fs) {
    const size_t skip = std::min(x.size(), static_cast<size_t>(0.2 * fs));
    double pk = 0.0;
    for (size_t i = skip; i < x.size(); ++i)
        pk = std::max(pk, static_cast<double>(std::fabs(x[i])));
    return pk;
}
double tailRms(const std::vector<float>& x, double fs) {
    const size_t skip = std::min(x.size(), static_cast<size_t>(0.2 * fs));
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = skip; i < x.size(); ++i) { acc += double(x[i]) * x[i]; ++n; }
    return n ? std::sqrt(acc / n) : 0.0;
}

// --- Test 1: harmonic generation. Runs at the given sample rate. -------------
void testHarmonics(double fs) {
    const double f0 = 220.0;
    const float amp = 0.10f;
    const double secs = 1.0;
    // FILTER = 0 (bright) so odd harmonics pass; measure the tail 1 s.
    auto harmEnergy = [&](float dist) {
        auto out = render(sine(f0, amp, secs, fs), {dist, 0.0f, 1.0f}, fs);
        const size_t n = out.size();
        const size_t win = std::min(n, static_cast<size_t>(fs));
        const size_t start = n - win;
        double e = 0.0;
        for (int h : {3, 5, 7, 9, 11}) {
            double a = goertzelAmp(out, start, win, f0 * h, fs);
            e += a * a;
        }
        return std::sqrt(e);
    };

    // 3rd/5th within 40 dB of the fundamental at high distortion.
    {
        auto out = render(sine(f0, amp, secs, fs), {0.9f, 0.0f, 1.0f}, fs);
        const size_t n = out.size();
        const size_t win = std::min(n, static_cast<size_t>(fs));
        const size_t start = n - win;
        const double f1 = toDb(goertzelAmp(out, start, win, f0, fs));
        const double d3 = toDb(goertzelAmp(out, start, win, f0 * 3, fs));
        const double d5 = toDb(goertzelAmp(out, start, win, f0 * 5, fs));
        const double d2 = toDb(goertzelAmp(out, start, win, f0 * 2, fs));  // noise floor ref
        assert(f1 - d3 < 40.0 && "3rd harmonic not within 40 dB of fundamental");
        assert(f1 - d5 < 40.0 && "5th harmonic not within 40 dB of fundamental");
        // Odd harmonics stand well above the (nearly absent) even harmonic.
        assert(d3 - d2 > 20.0 && "3rd harmonic not above noise/even-harmonic floor");
        assert(d5 - d2 > 20.0 && "5th harmonic not above noise/even-harmonic floor");
    }

    // Monotonic growth of harmonic energy as distortion rises.
    const double e2 = harmEnergy(0.2f);
    const double e5 = harmEnergy(0.5f);
    const double e9 = harmEnergy(0.9f);
    assert(e2 < e5 && "harmonic energy did not grow 0.2 -> 0.5");
    assert(e5 < e9 && "harmonic energy did not grow 0.5 -> 0.9");

    std::printf("  [ok] harmonic generation @ %.0f Hz (odd harmonics, monotonic)\n", fs);
}

// --- Test 2: clipping ceiling. ----------------------------------------------
void testClippingCeiling() {
    const double fs = 48000.0;
    const double f0 = 220.0;
    // High distortion, bright filter, unity level. Doubling input barely moves peak.
    auto peakAt = [&](float amp) {
        return tailPeak(render(sine(f0, amp, 0.5, fs), {0.9f, 0.0f, 1.0f}, fs), fs);
    };
    const double p1 = peakAt(0.3f);
    const double p2 = peakAt(0.6f);
    assert(p1 > 1e-3 && "no output");
    const double ratio = p2 / p1;
    assert(ratio < 1.3 && "output peak did not saturate (clipper bypassed?)");
    std::printf("  [ok] clipping ceiling: 2x input -> %.3fx peak\n", ratio);
}

// --- Test 3: filter behavior. Runs at the given sample rate. -----------------
void testFilter(double fs) {
    const double fLo = 220.0, fHi = 5000.0;
    const float amp = 0.05f;  // small: keep the through-path near-linear
    const double secs = 0.5;
    // Signal with both a low and a high tone.
    auto lo = sine(fLo, amp, secs, fs);
    auto hi = sine(fHi, amp, secs, fs);
    std::vector<float> mix(lo.size());
    for (size_t i = 0; i < mix.size(); ++i) mix[i] = lo[i] + hi[i];

    auto measure = [&](float filt, double* out5k, double* out220) {
        auto o = render(mix, {0.3f, filt, 1.0f}, fs);
        const size_t n = o.size();
        const size_t win = std::min(n, static_cast<size_t>(0.3 * fs));
        const size_t start = n - win;
        *out5k = goertzelAmp(o, start, win, fHi, fs);
        *out220 = goertzelAmp(o, start, win, fLo, fs);
    };
    double h0, l0, h5, l5, h1, l1;
    measure(0.0f, &h0, &l0);
    measure(0.5f, &h5, &l5);
    measure(1.0f, &h1, &l1);

    // 5 kHz decreases monotonically as FILTER goes 0 -> 0.5 -> 1 (darker).
    assert(h0 > h5 && "5 kHz did not drop from FILTER 0 -> 0.5");
    assert(h5 > h1 && "5 kHz did not drop from FILTER 0.5 -> 1");
    // 220 Hz fundamental barely affected (< 1.5 dB across the sweep).
    assert(std::fabs(toDb(l0) - toDb(l1)) < 1.5 && "220 Hz moved too much with filter");
    std::printf("  [ok] filter @ %.0f Hz: 5kHz %.1f -> %.1f -> %.1f dB, 220Hz ~flat\n",
                fs, toDb(h0), toDb(h5), toDb(h1));
}

// --- Test 4: level linearity. -----------------------------------------------
void testLevelLinearity() {
    const double fs = 48000.0;
    auto in = sine(220.0, 0.3f, 0.5, fs);
    auto rmsAt = [&](float lvl) {
        return tailRms(render(in, {0.7f, 0.3f, lvl}, fs), fs);
    };
    const double r25 = rmsAt(0.25f);
    const double r50 = rmsAt(0.50f);
    const double r100 = rmsAt(1.0f);
    // mapped gain == knob (identity), so RMS should scale linearly.
    assert(std::fabs(r50 / r25 - 2.0) < 0.05 && "RMS not linear 0.25 -> 0.5");
    assert(std::fabs(r100 / r50 - 2.0) < 0.05 && "RMS not linear 0.5 -> 1.0");
    std::printf("  [ok] level linearity: r50/r25=%.3f  r100/r50=%.3f\n",
                r50 / r25, r100 / r50);
}

// --- Test 5: hygiene (no NaN/inf, silence->silence, determinism). -----------
void testHygiene() {
    const double fs = 48000.0;
    // Parameter grid sweep: assert all-finite output.
    auto in = sine(330.0, 0.4f, 0.2, fs);
    for (float d = 0.0f; d <= 1.0f; d += 0.25f)
        for (float fl = 0.0f; fl <= 1.0f; fl += 0.25f)
            for (float lv = 0.0f; lv <= 1.0f; lv += 0.5f) {
                auto o = render(in, {d, fl, lv}, fs);
                for (float v : o) assert(std::isfinite(v) && "non-finite output");
            }

    // Silence in -> silence out (no DC pumping).
    {
        std::vector<float> zeros(static_cast<size_t>(fs * 0.3), 0.0f);
        auto o = render(zeros, {0.9f, 0.5f, 1.0f}, fs);
        double dc = 0.0, pk = 0.0;
        for (float v : o) { dc += v; pk = std::max(pk, (double)std::fabs(v)); }
        dc /= o.size();
        assert(pk < 1e-6 && "silence produced output");
        assert(std::fabs(dc) < 1e-3 && "DC offset on silence");
    }

    // Determinism: two runs bit-identical.
    {
        auto a = render(in, {0.6f, 0.4f, 0.8f}, fs);
        auto b = render(in, {0.6f, 0.4f, 0.8f}, fs);
        assert(a.size() == b.size());
        assert(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0 &&
               "processing is not deterministic");
    }
    std::printf("  [ok] hygiene: finite grid, silence->silence, deterministic\n");
}

}  // namespace

int main() {
    std::printf("Running clipper::dsp::RatModel tests...\n");
    testHarmonics(44100.0);
    testHarmonics(96000.0);   // Test 6: SR robustness for test 1
    testClippingCeiling();
    testFilter(44100.0);
    testFilter(96000.0);      // Test 6: SR robustness for test 3
    testLevelLinearity();
    testHygiene();
    std::printf("All RatModel tests passed.\n");
    return 0;
}
