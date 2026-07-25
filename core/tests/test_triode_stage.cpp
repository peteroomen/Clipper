// Plain-assert tests for clipper::dsp::TriodeStage (M9.1) — a validated 12AX7
// common-cathode gain stage. No framework: int main + <cassert>. Frequency
// content is measured with a hand-rolled Goertzel (no FFT dependency). Every
// assert compares a MEASURED number against an ANALYTIC target derived, in the
// test, from the Koren device law and the load-line / small-signal / shelf
// algebra — the same measurement discipline as the M1/M2 RAT suite.
//
// Analytic references are computed independently here (bisection on the load
// line, central-difference small-signal params, complex cathode-shelf transfer)
// so the tests pin the SOLVER against the physics, not against itself.

#include "clipper/dsp/TriodeStage.h"

#include "measure/AliasMetric.h"

#include "support/AssertsLive.h"

#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::TriodeStage;
using clipper::measure::measureAliasing;

// --- Hand-rolled Goertzel amplitude estimate at frequency f (Hann-windowed). --
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

std::vector<float> sine(double f, float amp, double secs, double fs) {
    const int n = static_cast<int>(secs * fs);
    std::vector<float> s(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        s[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(kTwoPi * f * i / fs));
    return s;
}

// Render a sine through a fresh stage; return the steady-state |gain| at f.
double gainAt(double f, float amp, double Ck, double fs, int os) {
    TriodeStage s;
    TriodeStage::Config c;
    c.Ck = Ck;
    s.configure(c);
    s.prepare(fs, 128);
    s.setOversampling(os);
    auto in = sine(f, amp, 1.0, fs);
    std::vector<float> out(in.size(), 0.0f);
    s.process(in.data(), out.data(), static_cast<int>(in.size()));
    const size_t n = out.size(), win = n / 2, start = n - win;
    return goertzelAmp(out, start, win, f, fs) / amp;
}

// ---------------------------------------------------------------------------
// Analytic references (independent of the solver).
// ---------------------------------------------------------------------------
const TriodeStage::KorenParams kTube{};  // default 12AX7
constexpr double kBplus = 320.0, kRa = 100.0e3, kRk = 820.0, kRgl = 1.0e6;

// Solve the DC operating point from the load line by bisection on Ip:
//   g(Ip) = Koren(B+ - Ip*Ra, -Ip*Rk) - Ip = 0.
// Self-bias: grid at 0 V (DC), so Vgk = -Vk = -Ip*Rk. Returns Ip (A); Va = B+-Ip*Ra.
double analyticOperatingCurrent() {
    auto g = [](double Ip) {
        const double Va = kBplus - Ip * kRa;
        return TriodeStage::korenPlateCurrent(Va, -Ip * kRk, kTube) - Ip;
    };
    double lo = 1e-6, hi = 4e-3;  // 0..4 mA brackets the root (g(lo)>0, g(hi)<0)
    for (int i = 0; i < 100; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (g(mid) > 0.0) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

// Small-signal device params at (Va, Vgk) by central difference of the Koren law.
struct SmallSignal { double gm, rp, mu; };
SmallSignal smallSignal(double Va, double Vgk) {
    const double h = 1e-5;
    const double gm = (TriodeStage::korenPlateCurrent(Va, Vgk + h, kTube) -
                       TriodeStage::korenPlateCurrent(Va, Vgk - h, kTube)) / (2 * h);
    const double go = (TriodeStage::korenPlateCurrent(Va + h, Vgk, kTube) -
                       TriodeStage::korenPlateCurrent(Va - h, Vgk, kTube)) / (2 * h);
    const double rp = 1.0 / go;
    return {gm, rp, gm * rp};
}

// ---------------------------------------------------------------------------
// Test 1 — DC operating point vs the analytic load line.
// The stage self-biases through Rk = 820 with B+ = 320, Ra = 100 k. The
// quiescent plate sits in the ~170-200 V region and the current near ~1.3 mA
// (the JCM800 first stage runs a hot bias). Assert the SOLVER's operating point
// matches the independent load-line bisection within 5 % (Va) and lands in the
// documented physical band.
// ---------------------------------------------------------------------------
void testOperatingPoint(double fs) {
    TriodeStage s;
    s.prepare(fs, 128);
    const double Iq = s.quiescentCurrent();
    const double Va = s.quiescentPlateVoltage();

    const double IqA = analyticOperatingCurrent();
    const double VaA = kBplus - IqA * kRa;

    assert(std::fabs(Va - VaA) / VaA < 0.05 && "quiescent Va off analytic load line >5%");
    assert(std::fabs(Iq - IqA) / IqA < 0.05 && "quiescent Iq off analytic load line >5%");
    // Physical bands (documented): Va 170-200 V, Iq ~1.0-1.6 mA (12AX7 hot bias).
    assert(Va > 170.0 && Va < 200.0 && "quiescent Va outside 170-200 V");
    assert(Iq > 1.0e-3 && Iq < 1.6e-3 && "quiescent Iq outside 1.0-1.6 mA band");
    std::printf("  [ok] DC op point @ %.0f Hz: Va=%.1f V (analytic %.1f), Iq=%.3f mA "
                "(analytic %.3f), Vk=%.3f V\n",
                fs, Va, VaA, Iq * 1e3, IqA * 1e3, s.quiescentCathodeVoltage());
}

// ---------------------------------------------------------------------------
// Test 2 — small-signal gain vs analytic (bypassed & unbypassed cathode).
// Mid-band voltage gain of a common cathode loaded by RL = Ra || Rgl (the plate
// drives the next 1 M grid leak through the output coupling):
//   fully bypassed:  A = -gm * (RL || rp)            (Rk shorted at AC)
//   unbypassed:      A = -mu * RL / (RL + rp + (mu+1)*Rk)   (local feedback)
// gm, rp, mu come from the Koren law linearised at the solved operating point.
// Target: bypassed ~ -64x (36 dB), unbypassed reduced by the feedback term;
// measured within +/-1.5 dB.
// ---------------------------------------------------------------------------
void testSmallSignalGain(double fs) {
    TriodeStage s;
    s.prepare(fs, 128);
    const double Va = s.quiescentPlateVoltage(), Vk = s.quiescentCathodeVoltage();
    const SmallSignal ss = smallSignal(Va, -Vk);
    const double RL = kRa * kRgl / (kRa + kRgl);  // 100k || 1M = 90.9 k
    const double gBypA = ss.gm * (RL * ss.rp / (RL + ss.rp));
    const double gUnbA = ss.mu * RL / (RL + ss.rp + (ss.mu + 1.0) * kRk);

    // Measure at 1 kHz, 10 mV grid (small-signal), fully bypassed (22 uF) vs none.
    const double gByp = gainAt(1000.0, 0.010f, 22.0e-6, fs, 4);
    const double gUnb = gainAt(1000.0, 0.010f, 0.0, fs, 4);

    assert(std::fabs(toDb(gByp) - toDb(gBypA)) < 1.5 &&
           "bypassed small-signal gain off analytic >1.5 dB");
    assert(std::fabs(toDb(gUnb) - toDb(gUnbA)) < 1.5 &&
           "unbypassed small-signal gain off analytic >1.5 dB");
    assert(gByp > gUnb * 1.3 && "bypass did not raise gain over unbypassed");
    // Sanity: bypassed lands in the JCM800 first-stage window (~ -60..-67x).
    assert(gByp > 58.0 && gByp < 70.0 && "bypassed gain outside the 12AX7 window");
    std::printf("  [ok] small-signal gain @ %.0f Hz: bypassed %.1fx (%.2f dB, analytic %.2f), "
                "unbypassed %.1fx (%.2f dB, analytic %.2f)  [gm=%.3f mS rp=%.1f k mu=%.1f]\n",
                fs, gByp, toDb(gByp), toDb(gBypA), gUnb, toDb(gUnb), toDb(gUnbA),
                ss.gm * 1e3, ss.rp / 1e3, ss.mu);
}

// ---------------------------------------------------------------------------
// Test 3 — transfer shape: asymmetric soft clip, 2nd-harmonic dominance at
// moderate drive, monotonic THD growth. Grid conduction squashes the positive
// grid peak (plate-side negative), so even harmonics dominate — the "warm" tube
// signature, unlike the RAT's symmetric odd-harmonic hard clip.
// ---------------------------------------------------------------------------
void testTransferShape(double fs) {
    const double f0 = 1000.0;
    auto harmonics = [&](float amp, double& h2dbc, double& h3dbc, double& thd) {
        TriodeStage s;
        TriodeStage::Config c; c.Ck = 0.68e-6; s.configure(c);
        s.prepare(fs, 128); s.setOversampling(4);
        auto in = sine(f0, amp, 0.5, fs);
        std::vector<float> out(in.size(), 0.0f);
        s.process(in.data(), out.data(), static_cast<int>(in.size()));
        const size_t n = out.size(), win = n / 2, start = n - win;
        const double f1 = goertzelAmp(out, start, win, f0, fs);
        const double a2 = goertzelAmp(out, start, win, 2 * f0, fs);
        const double a3 = goertzelAmp(out, start, win, 3 * f0, fs);
        const double a4 = goertzelAmp(out, start, win, 4 * f0, fs);
        const double a5 = goertzelAmp(out, start, win, 5 * f0, fs);
        h2dbc = toDb(a2) - toDb(f1);
        h3dbc = toDb(a3) - toDb(f1);
        thd = std::sqrt(a2 * a2 + a3 * a3 + a4 * a4 + a5 * a5) / f1;
    };

    double h2, h3, thd02, thd05, thd10;
    double dummy2, dummy3;
    harmonics(0.2f, h2, h3, thd02);
    // Moderate drive: 2nd harmonic dominant AND in a sane dBc window.
    assert(h2 > h3 + 6.0 && "2nd harmonic not dominant over 3rd at moderate drive");
    assert(h2 < -30.0 && h2 > -55.0 && "2nd-harmonic level outside the -30..-55 dBc window");
    harmonics(0.5f, dummy2, dummy3, thd05);
    harmonics(1.0f, dummy2, dummy3, thd10);
    // Monotonic THD growth with drive.
    assert(thd02 < thd05 && thd05 < thd10 && "THD did not grow monotonically with drive");
    // Asymmetry: at hard drive the two output polarities compress unequally
    // (cutoff on one side, saturation + grid conduction on the other) — the
    // one-sided soft clip a symmetric diode clipper cannot produce.
    {
        TriodeStage s;
        TriodeStage::Config c; c.Ck = 0.68e-6; s.configure(c);
        s.prepare(fs, 128); s.setOversampling(4);
        auto in = sine(f0, 3.0f, 0.5, fs);
        std::vector<float> out(in.size(), 0.0f);
        s.process(in.data(), out.data(), static_cast<int>(in.size()));
        double pkPos = 0.0, pkNeg = 0.0;
        for (size_t i = out.size() / 2; i < out.size(); ++i) {
            pkPos = std::max(pkPos, (double)out[i]);
            pkNeg = std::max(pkNeg, -(double)out[i]);
        }
        const double asym = std::fabs(pkPos - pkNeg) / std::max(pkPos, pkNeg);
        assert(asym > 0.10 && "transfer is symmetric (grid-conduction asymmetry missing)");
        std::printf("  [ok] transfer shape @ %.0f Hz: 2nd=%.1f dBc 3rd=%.1f dBc (0.2Vpk), "
                    "THD 0.2/0.5/1.0Vpk=%.2f/%.2f/%.2f%%, asym=%.1f%%\n",
                    fs, h2, h3, thd02 * 100, thd05 * 100, thd10 * 100, asym * 100);
    }
}

// ---------------------------------------------------------------------------
// Test 4 — blocking distortion. A hard overdrive burst drives the grid into
// conduction; the grid current charges the input coupling cap (Cc = 22 n), which
// then recovers through the grid leak (Rgl = 1 M) with tau = Rgl*Cc = 22 ms. So
// a burst shifts the bias toward cutoff and recovers over ~one RC after it ends.
// Measure the coupling-cap voltage (the bias-shift state) sample by sample; assert
// the shift exists and its post-burst 1/e recovery time is ~22 ms.
// ---------------------------------------------------------------------------
void testBlockingDistortion(double fs) {
    TriodeStage s;
    TriodeStage::Config c; c.Ck = 0.68e-6; s.configure(c);
    s.prepare(fs, 128); s.setOversampling(4);
    const int nB = static_cast<int>(0.10 * fs);  // 100 ms burst
    const int nS = static_cast<int>(0.15 * fs);  // 150 ms silence
    const int n = nB + nS;
    std::vector<float> in(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < nB; ++i)
        in[static_cast<size_t>(i)] = 2.0f * static_cast<float>(std::sin(kTwoPi * 200.0 * i / fs));

    std::vector<double> vcc(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        float o;
        s.process(&in[static_cast<size_t>(i)], &o, 1);
        vcc[static_cast<size_t>(i)] = s.couplingCapVoltage();
    }
    // Bias shift accumulated during the burst.
    double peak = 0.0;
    for (int i = 0; i < nB; ++i) peak = std::max(peak, std::fabs(vcc[static_cast<size_t>(i)]));
    assert(peak > 0.1 && "no blocking bias shift on overdrive (grid conduction missing?)");

    // Post-burst 1/e recovery time.
    const double v0 = std::fabs(vcc[static_cast<size_t>(nB)]);
    const double target = v0 / std::exp(1.0);
    int trec = -1;
    for (int i = nB; i < n; ++i)
        if (std::fabs(vcc[static_cast<size_t>(i)]) <= target) { trec = i - nB; break; }
    assert(trec > 0 && "coupling cap never recovered after the burst");
    const double tauMeas = trec * 1000.0 / fs;      // ms
    const double tauRC = kRgl * c.Cc * 1000.0;       // 22 ms
    // Recovery within +/-25% of the RC constant (the discharge is slightly faster
    // than pure RC while residual conduction lingers early in the tail).
    assert(std::fabs(tauMeas / tauRC - 1.0) < 0.25 &&
           "blocking recovery time not ~ the coupling RC (22 ms)");
    std::printf("  [ok] blocking distortion @ %.0f Hz: bias shift %.3f V, 1/e recovery "
                "%.1f ms (RC = Rgl*Cc = %.1f ms)\n", fs, peak, tauMeas, tauRC);
}

// ---------------------------------------------------------------------------
// Test 5 — stability: white noise + DC steps + +/-10 V slam at 44.1k & 96k, at
// 4x and 8x. No NaN/inf, bounded output, Newton iterations under the cap.
// ---------------------------------------------------------------------------
void testStability(double fs, int os) {
    TriodeStage s;
    s.prepare(fs, 128);
    s.setOversampling(os);
    const int n = static_cast<int>(0.3 * fs);
    std::vector<float> in(static_cast<size_t>(n)), out(static_cast<size_t>(n), 0.0f);
    unsigned seed = 22222u;
    for (int i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const double r = (seed >> 8) / static_cast<double>(1 << 24) - 0.5;  // +/-0.5
        const double dc = (i % 5000 < 2500) ? 3.0 : -3.0;                    // DC steps
        const double slam = (i % 9000 < 40) ? ((i & 1) ? 10.0 : -10.0) : 0.0;  // +/-10 V slam
        in[static_cast<size_t>(i)] = static_cast<float>(2.0 * r + dc + slam);
    }
    s.process(in.data(), out.data(), n);
    double pk = 0.0;
    for (float v : out) {
        assert(std::isfinite(v) && "non-finite output under slam");
        pk = std::max(pk, static_cast<double>(std::fabs(v)));
    }
    assert(pk < 1000.0 && "output not bounded under slam");
    assert(s.lastMaxNewtonIterations() < TriodeStage::kMaxNewtonIter &&
           "Newton hit the iteration cap (non-convergence)");
    std::printf("  [ok] stability @ %.0f Hz os=%d: finite, peak %.1f V, max Newton iters %d "
                "(cap %d)\n", fs, os, pk, s.lastMaxNewtonIterations(), TriodeStage::kMaxNewtonIter);
}

// ---------------------------------------------------------------------------
// Test 6 — cathode bypass frequency response (0.68 uF). The bypass cap forms a
// gain shelf: below the corner the cathode is degenerated (unbypassed gain),
// above it the cap shorts Rk (bypassed gain). Analytic complex transfer with a
// frequency-dependent cathode impedance Zk = Rk / (1 + jw*Rk*Ck):
//   |A(f)| = mu * RL / | RL + rp + (mu+1)*Zk |,  RL = Ra || Rgl.
// The shelf zero sits at 1/(2*pi*Rk*Ck). Assert the measured 0.68 uF response
// matches this analytic shelf within +/-1.5 dB across the corner.
// ---------------------------------------------------------------------------
void testCathodeBypass(double fs) {
    TriodeStage s;
    s.prepare(fs, 128);
    const double Va = s.quiescentPlateVoltage(), Vk = s.quiescentCathodeVoltage();
    const SmallSignal ss = smallSignal(Va, -Vk);
    const double RL = kRa * kRgl / (kRa + kRgl);
    const double Ck = 0.68e-6;
    auto analyticGain = [&](double f) {
        const double w = kTwoPi * f;
        const std::complex<double> Zk(kRk, 0.0);
        const std::complex<double> Zc = Zk / std::complex<double>(1.0, w * kRk * Ck);
        const std::complex<double> den = RL + ss.rp + (ss.mu + 1.0) * Zc;
        return ss.mu * RL / std::abs(den);
    };

    const double probes[] = {50.0, 100.0, 200.0, 285.0, 500.0, 1000.0, 3000.0};
    double worst = 0.0;
    for (double f : probes) {
        const double meas = gainAt(f, 0.010f, Ck, fs, 4);
        const double want = analyticGain(f);
        const double err = std::fabs(toDb(meas) - toDb(want));
        worst = std::max(worst, err);
        assert(err < 1.5 && "cathode-bypass shelf deviates >1.5 dB from analytic");
    }
    // The shelf really is a shelf: low end ~ unbypassed, high end ~ bypassed.
    const double gLo = gainAt(50.0, 0.010f, Ck, fs, 4);
    const double gHi = gainAt(3000.0, 0.010f, Ck, fs, 4);
    assert(gHi > gLo * 1.4 && "cathode bypass shelf did not lift the highs");
    const double fz = 1.0 / (kTwoPi * kRk * Ck);
    std::printf("  [ok] cathode bypass @ %.0f Hz (0.68 uF): shelf %.1fx(50Hz)->%.1fx(3kHz), "
                "zero 1/(2pi*Rk*Ck)=%.0f Hz, worst dev %.2f dB\n",
                fs, gLo, gHi, fz, worst);
}

// ---------------------------------------------------------------------------
// Test 7 — aliasing of the stage alone at 4x vs 8x (M2 sweep discipline). Drive
// a high sine (f0 = 4186 Hz) hard so its harmonics fold; measure worst-alias vs
// the fundamental at os = 1/2/4/8. Oversampling must drop it monotonically; the
// shipped 4x must clear a wide audibility margin (measured ~ -140 dB — the smooth
// triode transfer + reactive band-limiting alias far less than a hard clipper).
// ---------------------------------------------------------------------------
void testAliasing(double fs, bool strict) {
    const double f0 = 4186.0;
    auto in = sine(f0, 1.5f, 1.0, fs);
    double worst[4];
    int idx = 0;
    for (int os : {1, 2, 4, 8}) {
        TriodeStage s;
        TriodeStage::Config c; c.Ck = 0.68e-6; s.configure(c);
        s.prepare(fs, 128); s.setOversampling(os);
        std::vector<float> out(in.size(), 0.0f);
        s.process(in.data(), out.data(), static_cast<int>(in.size()));
        worst[idx++] = measureAliasing(out, fs, f0).worstAliasDb;
    }
    const double w1 = worst[0], w2 = worst[1], w4 = worst[2], w8 = worst[3];
    assert(w2 < w1 - 15.0 && "2x not >=15 dB better than 1x");
    // Shipped 4x (and 8x) must clear a wide audibility margin at both base rates.
    assert(w4 < -70.0 && "shipped 4x worst-alias not >=70 dB below fundamental");
    assert(w8 < -70.0 && "8x worst-alias not >=70 dB below fundamental");
    if (strict) {
        // 44.1 kHz base: 2x is still above the floor, so 4x strictly improves.
        assert(w4 < w2 - 10.0 && "4x did not improve >=10 dB on 2x (44.1k)");
    }
    // At 96 kHz base 2x/4x already bottom out at the numerical floor (w4 ~ w2).
    std::printf("  [ok] aliasing @ %.0f Hz base: worst-alias 1x=%.1f 2x=%.1f 4x=%.1f 8x=%.1f "
                "dB (shipped 4x)\n", fs, w1, w2, w4, w8);
}

// ---------------------------------------------------------------------------
// Test 8 — hygiene: silence->silence, determinism, all-finite parameter grid.
// ---------------------------------------------------------------------------
void testHygiene() {
    const double fs = 48000.0;
    // Silence in -> silence out (bias-only, no output — the output coupling blocks DC).
    {
        TriodeStage s; s.prepare(fs, 128); s.setOversampling(4);
        std::vector<float> zeros(static_cast<size_t>(fs * 0.2), 0.0f);
        std::vector<float> out(zeros.size(), 0.0f);
        s.process(zeros.data(), out.data(), static_cast<int>(zeros.size()));
        double pk = 0.0;
        for (float v : out) pk = std::max(pk, (double)std::fabs(v));
        assert(pk < 1e-3 && "silence produced output");
    }
    // Determinism: two runs bit-identical.
    {
        auto in = sine(220.0, 0.5f, 0.2, fs);
        TriodeStage a, b;
        a.prepare(fs, 128); a.setOversampling(4);
        b.prepare(fs, 128); b.setOversampling(4);
        std::vector<float> oa(in.size(), 0.f), ob(in.size(), 0.f);
        a.process(in.data(), oa.data(), (int)in.size());
        b.process(in.data(), ob.data(), (int)in.size());
        assert(std::memcmp(oa.data(), ob.data(), oa.size() * sizeof(float)) == 0 &&
               "processing is not deterministic");
    }
    // Finite across a drive/oversampling grid.
    {
        auto in = sine(330.0, 1.0f, 0.1, fs);
        for (int os : {1, 2, 4, 8})
            for (float amp : {0.1f, 1.0f, 5.0f}) {
                TriodeStage s; s.prepare(fs, 128); s.setOversampling(os);
                std::vector<float> scaled(in.size()), out(in.size(), 0.f);
                for (size_t i = 0; i < in.size(); ++i) scaled[i] = in[i] * amp;
                s.process(scaled.data(), out.data(), (int)in.size());
                for (float v : out) assert(std::isfinite(v) && "non-finite in grid");
            }
    }
    std::printf("  [ok] hygiene: silence->silence, deterministic, finite grid\n");
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();
    std::printf("Running clipper::dsp::TriodeStage (M9.1) tests...\n");
    testOperatingPoint(44100.0);
    testOperatingPoint(96000.0);
    testSmallSignalGain(44100.0);
    testSmallSignalGain(96000.0);
    testTransferShape(44100.0);
    testTransferShape(96000.0);
    testBlockingDistortion(44100.0);
    testBlockingDistortion(96000.0);
    testStability(44100.0, 4);
    testStability(44100.0, 8);
    testStability(96000.0, 4);
    testStability(96000.0, 8);
    testCathodeBypass(44100.0);
    testCathodeBypass(96000.0);
    testAliasing(44100.0, /*strict=*/true);
    testAliasing(96000.0, /*strict=*/false);
    testHygiene();
    std::printf("All TriodeStage tests passed.\n");
    return 0;
}
