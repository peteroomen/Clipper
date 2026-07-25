// Plain-assert tests for clipper::dsp::RatModel (M1). No framework: int main +
// <cassert>. Frequency content is measured with a hand-rolled Goertzel (no FFT
// dependency). Each assert is designed to FAIL if the corresponding stage is
// broken (verified during development by temporarily bypassing stages).

#include "clipper/dsp/RatModel.h"

#include "clipper/dsp/DiodeClipperADAA.h"
#include "clipper/dsp/LM308Stage.h"
#include "measure/AliasMetric.h"

#include "support/AssertsLive.h"
#include "support/DcOffset.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <complex>
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

// --- Test 1b: pre-clip voicing (M6.1) matches the real RAT gain stage. --------
// The DISTORTION knob's frequency shaping is the ProCo RAT LM308 non-inverting
// stage: A(s) = 1 + Rf/Zg, Zg = (560R+4.7uF) || (47R+2.2uF) to ground, Rf=100k.
// Normalized to the HF plateau it rises through two corners (~60.5 Hz, ~1539 Hz)
// and falls toward unity at DC — NOT the old fixed 320 Hz / -10.5 dB shelf.
//
// Measure the small-signal shape in isolation: DISTORTION=0 => pre-gain is unity
// (0 dB), so stage 1 is JUST the shaping filter; tiny input keeps the diodes
// linear; FILTER=0 (bright) keeps stage 3 wide open. Normalize the measured
// response to its 3 kHz value and compare to the analytical A(f)/A(3kHz) target,
// which must match within +/-1.5 dB at every probe frequency.
void testPreClipVoicing(double fs) {
    // Analytical target: |A(f)| for the RAT network (dB, relative to 3 kHz).
    auto analyticalDbRel3k = [](double f) {
        const double Rf = 100e3, R1 = 560.0, C1 = 4.7e-6, R2 = 47.0, C2 = 2.2e-6;
        auto magA = [&](double fr) {
            const double w = kTwoPi * fr;
            // Z = R + 1/(jwC) -> 1/Z = jwC/(1+jwRC); Y = Y1 + Y2; A = 1 + Rf*Y.
            auto Y = [&](double R, double C) {
                // jwC / (1 + jwRC): return (re, im).
                const double a = 0.0, b = w * C;            // numerator jwC
                const double c = 1.0, dd = w * R * C;        // denom 1 + jwRC
                const double den = c * c + dd * dd;
                return std::complex<double>((a * c + b * dd) / den, (b * c - a * dd) / den);
            };
            std::complex<double> A = std::complex<double>(1.0, 0.0) + Rf * (Y(R1, C1) + Y(R2, C2));
            return std::abs(A);
        };
        return 20.0 * std::log10(magA(f) / magA(3000.0));
    };

    const double f0Ref = 3000.0;
    const float amp = 1e-3f;  // tiny: stay linear through the diode stage
    auto shapeDbRel3k = [&](double f) {
        // DISTORTION=0 (unity pre-gain), FILTER=0 (bright), LEVEL=1.
        auto out = render(sine(f, amp, 0.5, fs), {0.0f, 0.0f, 1.0f}, fs);
        auto outRef = render(sine(f0Ref, amp, 0.5, fs), {0.0f, 0.0f, 1.0f}, fs);
        const size_t n = out.size(), win = std::min(n, static_cast<size_t>(0.3 * fs));
        const size_t start = n - win;
        return toDb(goertzelAmp(out, start, win, f, fs)) -
               toDb(goertzelAmp(outRef, start, win, f0Ref, fs));
    };

    const double probes[] = {82.4, 220.0, 320.0, 500.0, 1000.0, 1539.0, 5000.0};
    double worst = 0.0;
    for (double f : probes) {
        const double meas = shapeDbRel3k(f);
        const double want = analyticalDbRel3k(f);
        const double err = std::fabs(meas - want);
        worst = std::max(worst, err);
        assert(err < 1.5 && "pre-clip voicing deviates >1.5 dB from the RAT analytical target");
    }
    // The two documented corners bracket the rise; HF plateau ~0 dB (rel 3 kHz).
    // NB (M6.5): this measures at DISTORTION=0, where the LM308 closed-loop corner
    // is ~GBW/1 = 1 MHz (far above these <=5 kHz probes — transparent), so the
    // two-corner voicing is measured cleanly in the high-bandwidth regime. The
    // COMPLEMENTARY property — the corner COLLAPSING as gain rises — is asserted
    // by testClosedLoopBandwidth below. Together they pin both halves of the M6.5
    // gain stage.
    assert(std::fabs(shapeDbRel3k(5000.0)) < 1.0 && "HF plateau not ~unity (rel 3 kHz)");
    assert(shapeDbRel3k(82.4) < -12.0 && "low E not shelved (voicing missing?)");
    std::printf(
        "  [ok] pre-clip voicing @ %.0f Hz (BW high): worst dev %.2f dB (82Hz=%.1f "
        "320Hz=%.1f 1539Hz=%.1f dB rel 3k)\n",
        fs, worst, shapeDbRel3k(82.4), shapeDbRel3k(320.0), shapeDbRel3k(1539.0));
}

// --- Test 1c: LM308 closed-loop bandwidth tracks the noise gain (M6.5). --------
// The op-amp's closed-loop -3 dB corner is f_c = GBW / A_noise: it COLLAPSES as
// DISTORTION (the noise gain) rises. GBW = 1.0 MHz (see RatModel.cpp).
//
// Measurement: the LM308 low-pass sits BEFORE the diode clamp, so ratio the
// small-signal response at a high-gain setting against DISTORTION=0 (corner ~1
// MHz, LP inactive). Everything gain-independent — the two-corner shaping, the
// WDF shunt-cap corner, the FILTER stage, the linear diode slope — CANCELS in the
// ratio, leaving exactly the one-pole LP. Extract f_c from the rolloff and
// compare to the analytic GBW/A at two DISTORTION settings.
void testClosedLoopBandwidth(double fs) {
    const double GBW = 1.0e6;
    const float amp = 1e-5f;  // deeply linear through the diode at ALL gains here
    // Small-signal output level (dB) at frequency f for a DISTORTION setting.
    // render() uses the default 4x oversampling with the LM308 modelled (on).
    auto respDb = [&](float dist, double f) {
        auto out = render(sine(f, amp, 0.5, fs), {dist, 0.0f, 1.0f}, fs);
        const size_t n = out.size(), win = std::min(n, static_cast<size_t>(0.3 * fs));
        return toDb(goertzelAmp(out, n - win, win, f, fs));
    };
    // LP attenuation (dB) at f, isolated by ratioing vs dist=0 and normalizing to
    // a low reference frequency well below the corner.
    auto extractFc = [&](float dist, double f, double fRef) {
        const double L = (respDb(dist, f) - respDb(0.0f, f)) -
                         (respDb(dist, fRef) - respDb(0.0f, fRef));
        const double r = std::pow(10.0, -L / 10.0) - 1.0;  // (f/fc)^2
        return f / std::sqrt(r > 1e-9 ? r : 1e-9);
    };
    // dist 1.0 => A ~ 1995 => fc ~ 501 Hz;  dist 0.7 => A ~ 204 => fc ~ 4898 Hz.
    const double A_hi = std::pow(10.0, 66.0 * 1.0 / 20.0);
    const double A_md = std::pow(10.0, 66.0 * 0.7 / 20.0);
    const double fcHiAnalytic = GBW / A_hi;
    const double fcMdAnalytic = GBW / A_md;
    // Average a couple of probes bracketing each corner for robustness.
    const double fcHi = 0.5 * (extractFc(1.0f, 1200.0, 100.0) + extractFc(1.0f, 1800.0, 100.0));
    const double fcMd = 0.5 * (extractFc(0.7f, 4000.0, 500.0) + extractFc(0.7f, 6000.0, 500.0));

    // Corner within 20% of analytic GBW/A at both gains (measured ~2%).
    assert(std::fabs(fcHi / fcHiAnalytic - 1.0) < 0.20 &&
           "closed-loop corner at +66 dB is not ~ GBW/A (~500 Hz)");
    assert(std::fabs(fcMd / fcMdAnalytic - 1.0) < 0.20 &&
           "closed-loop corner at dist 0.7 is not ~ GBW/A (~4.9 kHz)");
    // Tracking: the corner scales inversely with gain — higher gain => lower fc.
    assert(fcHi < fcMd * 0.3 &&
           "closed-loop corner did not COLLAPSE as gain rose (BW not gain-tracking)");
    std::printf(
        "  [ok] closed-loop BW @ %.0f Hz: fc(+66dB)=%.0f Hz (GBW/A=%.0f), "
        "fc(dist0.7)=%.0f Hz (GBW/A=%.0f)\n",
        fs, fcHi, fcHiAnalytic, fcMd, fcMdAnalytic);
}

// --- Test 1d: LM308 slew-rate limiter clamps dV/dt to ~0.3 V/us (M6.5). --------
// Unit-test the LM308Stage in isolation: a big step / square must produce a max
// |dV/dt| equal to the configured slew rate (0.3 V/us), rate-independently. (The
// slew limiter lives inside oversampling before the diode clamp; testing the
// stage directly avoids the diode+FILTER masking the op-amp node, and confirms
// the clamp math at both 44.1k and 96k.)
void testSlewRate(double fs) {
    const double GBW = 1.0e6, SR = 0.3e6;  // V/s
    clipper::dsp::LM308Stage s;
    s.prepare(fs, GBW, SR);
    s.setNoiseGain(1.0f);  // corner near Nyquist; isolate the slew clamp
    auto maxSlopeVpUs = [&](const std::vector<float>& in) {
        s.reset();
        double mx = 0.0;
        float prev = 0.0f;
        for (size_t i = 0; i < in.size(); ++i) {
            const float y = s.processSample(in[i]);
            if (i) mx = std::max(mx, std::fabs(static_cast<double>(y) - prev) * fs);
            prev = y;
        }
        return mx / 1e6;
    };
    // A big step (100 V op-amp scale) guarantees the clamp engages at any rate.
    std::vector<float> step(static_cast<size_t>(0.01 * fs), 100.0f);
    step[0] = 0.0f;
    const double srStep = maxSlopeVpUs(step);
    // A large 1 kHz square (both edges slew-limited).
    std::vector<float> sq(static_cast<size_t>(0.02 * fs));
    for (size_t i = 0; i < sq.size(); ++i)
        sq[i] = (std::sin(kTwoPi * 1000.0 * i / fs) >= 0.0 ? 100.0f : -100.0f);
    const double srSq = maxSlopeVpUs(sq);

    assert(std::fabs(srStep / 0.3 - 1.0) < 0.02 && "step slew-rate not ~0.3 V/us");
    assert(std::fabs(srSq / 0.3 - 1.0) < 0.02 && "square slew-rate not ~0.3 V/us");
    std::printf("  [ok] slew @ %.0f Hz: step %.4f V/us, square %.4f V/us (target 0.300)\n",
                fs, srStep, srSq);
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

    // Silence in -> silence out (the model does not self-oscillate).
    {
        std::vector<float> zeros(static_cast<size_t>(fs * 0.3), 0.0f);
        auto o = render(zeros, {0.9f, 0.5f, 1.0f}, fs);
        double dc = 0.0, pk = 0.0;
        for (float v : o) { dc += v; pk = std::max(pk, (double)std::fabs(v)); }
        dc /= o.size();
        assert(pk < 1e-6 && "silence produced output");
        assert(std::fabs(dc) < 1e-3 && "DC offset on silence");
    }

    // DC offset ON SIGNAL — the property that actually matters (audit finding 16).
    //
    // 2026-07-25 (test/assert-real-properties): the silence check above used to be the
    // ONLY DC assertion here, and it is trivially true. The RAT clips ASYMMETRICALLY
    // (LM308 + a silicon pair around the feedback path), so its DC comes from SIGNAL, and
    // a lost output blocker would sail past a silent-input test. `OverdriveEngine` carries
    // `dcBlockHz = 12.0` precisely for this; that constant is what this now pins.
    //
    // Player-observable: DC eats the headroom of the amp, cab and limiter downstream and
    // thumps when the note stops.
    //
    // TWO stimuli. The clean tone catches a clipper that RECTIFIES; the +0.1 V input offset
    // is what makes the coupling cap itself load-bearing — with a clean input, deleting the
    // cap changes the measured DC by nothing (verified). See core/tests/support/DcOffset.h.
    {
        const auto tone = sine(220.0, 0.2f, 1.0, fs);
        for (float dist : {0.5f, 1.0f}) {
            for (float dcIn : {0.0f, clipper::test::kInputDcOffset}) {
                auto stim = tone;
                for (float& s : stim) s += dcIn;
                const auto o = render(stim, {dist, 0.5f, 1.0f}, fs);
                const auto d = clipper::test::measureDcOnSignal(o);
                std::printf("  [ok] DC on SIGNAL (dist %.2f, input DC %+.2f V): mean %+.6f V, "
                            "peak %.4f V -> %.4f %% of peak (bar %.1f %%)\n",
                            dist, dcIn, d.mean, d.peak, 100.0 * d.fraction,
                            100.0 * clipper::test::kDcFractionBar);
                assert(d.peak > 0.01 && "no signal to measure DC against");
                assert(d.fraction < clipper::test::kDcFractionBar &&
                       "DC offset ON SIGNAL exceeds 1 % of peak — the output coupling cap is "
                       "missing or mistuned, or the clipper is rectifying");
            }
        }
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

// ===========================================================================
// M2 — antialiasing tests.
// ===========================================================================

using clipper::measure::AliasReport;
using clipper::measure::measureAliasing;
// NB: the file already defines a local goertzelAmp with the same signature as
// clipper::measure::goertzelAmp; the local one is used below.

// Render through a fresh model with a chosen oversampling factor / stage-2 mode.
std::vector<float> renderOS(const std::vector<float>& in, Params p, double fs,
                            int os, int stage2 = RatModel::STAGE2_WDF,
                            bool idealOpAmp = false) {
    RatModel m;
    m.prepare(fs, 128);
    m.setOversampling(os);
    m.setStage2Mode(stage2);
    m.setIdealOpAmp(idealOpAmp);
    m.setParameter(RatModel::PARAM_DISTORTION, p.distortion);
    m.setParameter(RatModel::PARAM_FILTER, p.filter);
    m.setParameter(RatModel::PARAM_LEVEL, p.level);
    std::vector<float> out(in.size(), 0.0f);
    if (!in.empty())
        m.process(in.data(), out.data(), static_cast<int>(in.size()));
    return out;
}

// --- Test M2-1: oversampling reduces aliasing monotonically. -----------------
// f0 = 4186 Hz (C8): its odd harmonics above Nyquist fold to predictable
// inharmonic bins (see AliasMetric.h). Drive hard (dist 0.9, bright) so the
// clipper generates lots of high harmonics. At 44.1 kHz base the full 1x->8x
// chain must improve strictly; at 96 kHz the higher OS factors bottom out at the
// numerical floor, so only the weaker guarantees are asserted there.
// NOTE (M6.5): measured with the op-amp IDEALIZED (setIdealOpAmp). This test is
// the M2 regression for the *oversampled diode clipper* — that oversampling
// reduces the clipper's aliasing monotonically. The M6.5 LM308 slew limiter is a
// SEPARATE nonlinearity that itself aliases at LOW oversampling (measured: at 2x
// it can be a few dB WORSE than 1x), so a strict 1x->2x->4x->8x chain does not
// hold for the full LM308 path — an expected, documented tradeoff. Crucially the
// SHIPPED factor is 4x, where the real (LM308-on) path is EXCELLENT (worst-alias
// ~ -89 dB at dist 0.9, ~ -88 dB at the +66 dB max) — asserted separately by
// testAliasingAtMaxGain. Idealizing the op-amp here keeps this a faithful,
// meaningful M2 oversampler regression (identical numbers to pre-M6.5).
void testAliasingMonotonic(double fs, bool strict) {
    const double f0 = 4186.0;
    auto in = sine(f0, 0.3f, 1.0, fs);
    double worst[4];
    int idx = 0;
    for (int os : {1, 2, 4, 8}) {
        auto out = renderOS(in, {0.9f, 0.0f, 0.9f}, fs, os, RatModel::STAGE2_WDF,
                            /*idealOpAmp=*/true);
        worst[idx++] = measureAliasing(out, fs, f0).worstAliasDb;
    }
    const double w1 = worst[0], w2 = worst[1], w4 = worst[2], w8 = worst[3];

    // 4x must be >= 20 dB better than 1x (explicit requirement).
    assert(w4 < w1 - 20.0 && "4x oversampling not >=20 dB better than 1x");
    // 8x worst-alias at least 60 dB below the fundamental.
    assert(w8 < -60.0 && "8x worst-alias not >=60 dB below fundamental");
    // 2x improves on 1x.
    assert(w2 < w1 - 3.0 && "2x did not improve on 1x");

    if (strict) {
        // Full strict monotonic chain (44.1 kHz base, all above the floor).
        assert(w4 < w2 - 3.0 && "4x did not improve on 2x");
        assert(w8 < w4 - 1.0 && "8x did not improve on 4x");
    } else {
        // 96 kHz: 4x/8x hit the numerical floor; just require both very low.
        assert(w4 < -60.0 && w8 < -60.0 && "4x/8x not well below fundamental");
    }
    std::printf(
        "  [ok] aliasing @ %.0f Hz base: worst-alias 1x=%.1f 2x=%.1f 4x=%.1f 8x=%.1f dB\n",
        fs, w1, w2, w4, w8);
}

// --- Test M6.5: aliasing at the +66 dB max gain, SHIPPED (LM308-on) path. -----
// M2 measured the aliasing bar at dist 0.9; M6.1 raised the max pre-clip gain to
// +66 dB (dist 1.0), pushing ~12 dB more HF into the clipper. Re-assert the M2
// audibility bar (worst-alias >= 60 dB below the fundamental) at the SHIPPED 4x,
// with the full LM308 path engaged (the BW/slew help — the clipper input is
// band-limited). Measured comfortably past the bar (~ -88 dB @ 44.1k).
void testAliasingAtMaxGain(double fs) {
    const double f0 = 4186.0;
    auto in = sine(f0, 0.3f, 1.0, fs);
    const double w4 = measureAliasing(renderOS(in, {1.0f, 0.0f, 0.9f}, fs, 4), fs, f0)
                          .worstAliasDb;
    assert(w4 < -60.0 &&
           "4x worst-alias at +66 dB (dist 1.0) not >=60 dB below fundamental");
    std::printf(
        "  [ok] aliasing @ +66 dB (dist 1.0), %.0f Hz base: 4x worst-alias %.1f dB "
        "(bar -60)\n",
        fs, w4);
}

// --- Test M2-2: passband integrity — OS removes aliases, not tone. -----------
void testPassbandIntegrity() {
    const double fs = 44100.0;
    // (a) A clean-ish 1 kHz tone at low distortion: 4x amplitude within ~0.2 dB
    //     of 1x.
    {
        auto in = sine(1000.0, 0.05f, 0.5, fs);
        auto o1 = renderOS(in, {0.2f, 0.0f, 1.0f}, fs, 1);
        auto o4 = renderOS(in, {0.2f, 0.0f, 1.0f}, fs, 4);
        const size_t n = o1.size(), win = n / 2, start = n - win;
        const double a1 = toDb(goertzelAmp(o1, start, win, 1000.0, fs));
        const double a4 = toDb(goertzelAmp(o4, start, win, 1000.0, fs));
        assert(std::fabs(a4 - a1) < 0.2 && "1 kHz passband amplitude shifted >0.2 dB at 4x");
        std::printf("  [ok] passband: 1 kHz 1x=%.4f dB 4x=%.4f dB (delta %.4f dB)\n",
                    a1, a4, a4 - a1);
    }
    // (b) Harmonic character at moderate drive: 3rd/5th ratios within a few dB of
    //     1x (oversampling must not change the tone).
    {
        auto in = sine(220.0, 0.1f, 1.0, fs);
        auto o1 = renderOS(in, {0.6f, 0.0f, 1.0f}, fs, 1);
        auto o4 = renderOS(in, {0.6f, 0.0f, 1.0f}, fs, 4);
        const size_t n = o1.size(), win = n / 2, start = n - win;
        auto ratio = [&](const std::vector<float>& o, double h) {
            return toDb(goertzelAmp(o, start, win, 220.0 * h, fs)) -
                   toDb(goertzelAmp(o, start, win, 220.0, fs));
        };
        const double r3_1 = ratio(o1, 3), r3_4 = ratio(o4, 3);
        const double r5_1 = ratio(o1, 5), r5_4 = ratio(o4, 5);
        assert(std::fabs(r3_4 - r3_1) < 3.0 && "3rd harmonic ratio moved >3 dB at 4x");
        assert(std::fabs(r5_4 - r5_1) < 3.0 && "5th harmonic ratio moved >3 dB at 4x");
        std::printf("  [ok] passband: 3rd %.2f->%.2f  5th %.2f->%.2f dB (1x->4x)\n",
                    r3_1, r3_4, r5_1, r5_4);
    }
}

// --- Test M2-3: factor-1 regression — os=1 reproduces the M1 path. -----------
// Golden samples captured from the RatModel os=1 path rendering a 987 Hz sine
// (amp 0.25) at dist 0.8 / filter 0.35 / level 0.9. os=1 is a pure pass-through
// around the WDF stage, so it must match bit-closely. Regenerated for M6.5 (the
// LM308 op-amp model now sits in the signal path at every factor) — the guard
// pins the current single-rate path (shaping + LM308 + clip) against future drift.
void testFactorOneRegression() {
    const double fs = 44100.0, f0 = 987.0;
    const int N = 4096;
    std::vector<float> in(N);
    for (int i = 0; i < N; ++i)
        in[i] = 0.25f * static_cast<float>(std::sin(kTwoPi * f0 * i / fs));
    auto out = renderOS(in, {0.8f, 0.35f, 0.9f}, fs, 1);
    struct G { int i; float v; };
    const G golden[] = {
        {128, -1.448836029e-01f}, {256, -2.423077524e-01f}, {512, -2.132538408e-01f},
        {1024, -1.587393880e-01f}, {2048, -3.484802246e-01f}, {3000, 3.611189127e-01f},
        {4095, -3.536947966e-01f},
    };
    double maxDiff = 0.0;
    for (const auto& g : golden)
        maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(out[g.i] - g.v)));
    assert(maxDiff < 1e-5 && "os=1 output diverged from the M1 golden reference");
    std::printf("  [ok] factor-1 regression: max |os1 - M1 golden| = %.2e\n", maxDiff);
}

// --- Test M2-4: ADAA effectiveness on the memoryless clipper. -----------------
// Build a naive (aliasing) reference and an ADAA version from the SAME transfer
// curve (DiodeClipperADAA) and drive them with a high sine. f0 = 12 kHz so its
// odd harmonics fold aggressively; ADAA must beat naive by >= 12 dB.
void testAdaaEffectiveness() {
    const double fs = 44100.0, f0 = 12000.0;
    const int N = static_cast<int>(fs);
    std::vector<float> in(N), naive(N), adaa(N);
    for (int i = 0; i < N; ++i)
        in[i] = 1.5f * static_cast<float>(std::sin(kTwoPi * f0 * i / fs));
    clipper::dsp::DiodeClipperADAA a, b;
    a.reset();
    b.reset();
    for (int i = 0; i < N; ++i) naive[i] = a.processSampleNaive(in[i]);
    for (int i = 0; i < N; ++i) adaa[i] = b.processSampleADAA(in[i]);
    const AliasReport rn = measureAliasing(naive, fs, f0);
    const AliasReport ra = measureAliasing(adaa, fs, f0);
    const double worstImpr = rn.worstAliasDb - ra.worstAliasDb;
    const double sumImpr = rn.sumAliasDb - ra.sumAliasDb;
    assert(worstImpr >= 12.0 && "ADAA worst-alias not >=12 dB better than naive");
    assert(sumImpr >= 12.0 && "ADAA summed alias not >=12 dB better than naive");
    std::printf(
        "  [ok] ADAA: worst %.1f->%.1f (%.1f dB), sum %.1f->%.1f (%.1f dB) vs naive\n",
        rn.worstAliasDb, ra.worstAliasDb, worstImpr, rn.sumAliasDb, ra.sumAliasDb,
        sumImpr);
}

// --- Test M2-5: perf sanity — 8x processes 1 s well under real time. ---------
void testPerfSanity() {
    const double fs = 44100.0;
    auto in = sine(4186.0, 0.3f, 1.0, fs);
    RatModel m;
    m.prepare(fs, 128);
    m.setOversampling(8);
    m.setParameter(RatModel::PARAM_DISTORTION, 0.9f);
    m.setParameter(RatModel::PARAM_FILTER, 0.3f);
    m.setParameter(RatModel::PARAM_LEVEL, 0.9f);
    std::vector<float> out(in.size(), 0.0f);
    // Process in 128-frame blocks like a real host would.
    const auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t off = 0; off < in.size(); off += 128) {
        const int n = static_cast<int>(std::min<size_t>(128, in.size() - off));
        m.process(in.data() + off, out.data() + off, n);
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    assert(ms < 500.0 && "8x processing not comfortably faster than real time");
    // Sanity: output is finite and non-trivial.
    double pk = 0.0;
    for (float v : out) { assert(std::isfinite(v)); pk = std::max(pk, (double)std::fabs(v)); }
    assert(pk > 1e-3 && "8x produced no output");
    std::printf("  [ok] perf: 1 s @ 8x (128-frame blocks) in %.1f ms (< 500 ms)\n", ms);
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();
    std::printf("Running clipper::dsp::RatModel tests...\n");
    testHarmonics(44100.0);
    testHarmonics(96000.0);   // Test 6: SR robustness for test 1
    testPreClipVoicing(44100.0);
    testPreClipVoicing(96000.0);
    testClosedLoopBandwidth(44100.0);
    testClosedLoopBandwidth(96000.0);
    testSlewRate(44100.0);
    testSlewRate(96000.0);
    testClippingCeiling();
    testFilter(44100.0);
    testFilter(96000.0);      // Test 6: SR robustness for test 3
    testLevelLinearity();
    testHygiene();
    std::printf("Running M2 (antialiasing) tests...\n");
    testAliasingMonotonic(44100.0, /*strict=*/true);
    testAliasingMonotonic(96000.0, /*strict=*/false);
    testAliasingAtMaxGain(44100.0);
    testAliasingAtMaxGain(96000.0);
    testPassbandIntegrity();
    testFactorOneRegression();
    testAdaaEffectiveness();
    testPerfSanity();
    std::printf("All RatModel tests passed.\n");
    return 0;
}
