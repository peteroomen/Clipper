// Plain-assert tests for clipper::dsp::Jcm800Preamp (M9.2) — the composed JCM800
// 2204 preamp (4x TriodeStage + passive Marshall TMB tone stack). No framework:
// int main + <cassert>. Frequency content is measured with a hand-rolled Goertzel
// (no FFT dependency). Every assert compares a MEASURED number against an ANALYTIC
// target derived IN THE TEST — per-stage load-line bisection, central-difference
// small-signal params, the complex cathode-bypass transfer, and a complex nodal
// solve of the tone-stack netlist — so the suite pins the composition against the
// physics, not against itself. Same measurement discipline as the M1/M2/M9.1 suites.

#include "clipper/dsp/Jcm800Preamp.h"

#include "measure/AliasMetric.h"

#include "support/AssertsLive.h"

#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::Jcm800Preamp;
using clipper::dsp::MarshallToneStack;
using clipper::dsp::TriodeStage;
using clipper::measure::measureAliasing;

const TriodeStage::KorenParams kTube{};  // default 12AX7
constexpr double kBplus = 320.0;

double koren(double Va, double Vgk) {
    return TriodeStage::korenPlateCurrent(Va, Vgk, kTube);
}

// --- signal + spectrum helpers ----------------------------------------------
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
double toDb(double a) { return 20.0 * std::log10(a + 1e-12); }

std::vector<float> sine(double f, float amp, double secs, double fs) {
    const int n = static_cast<int>(secs * fs);
    std::vector<float> s(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        s[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(kTwoPi * f * i / fs));
    return s;
}

// ---------------------------------------------------------------------------
// Analytic references (independent of the solver).
// ---------------------------------------------------------------------------

// Common-cathode DC operating current from the load line by bisection:
//   g(Ip) = Koren(B+ - Ip*Ra, -Ip*Rk) - Ip = 0   (self-bias, grid at 0 V DC).
double analyticCCcurrent(double Ra, double Rk) {
    auto g = [&](double Ip) { return koren(kBplus - Ip * Ra, -Ip * Rk) - Ip; };
    double lo = 1e-7, hi = 5e-3;  // brackets the root: g(lo)>0, g(hi)<0
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (g(mid) > 0.0) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

// Direct-coupled cathode-follower cathode voltage by bisection (plate = B+):
//   f(Vk) = Koren(B+ - Vk, gridBias - Vk) - Vk/Rk = 0  (grid current negligible).
double analyticFollowerVk(double gridBias, double Rk) {
    auto f = [&](double Vk) { return koren(kBplus - Vk, gridBias - Vk) - Vk / Rk; };
    double lo = 1.0, hi = kBplus;  // f(lo)>0 (grid drives hard), f(hi)<0 (Va->0)
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (f(mid) > 0.0) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

// Small-signal device params at (Va, Vgk) by central difference of the Koren law.
struct SmallSignal { double gm, rp, mu; };
SmallSignal smallSignal(double Va, double Vgk) {
    const double h = 1e-5;
    const double gm = (koren(Va, Vgk + h) - koren(Va, Vgk - h)) / (2 * h);
    const double go = (koren(Va + h, Vgk) - koren(Va - h, Vgk)) / (2 * h);
    const double rp = 1.0 / go;
    return {gm, rp, gm * rp};
}

// Complex magnitude of a common-cathode gain with cathode impedance Zk =
// Rk/(1+jw*Rk*Ck) (Ck=0 -> Zk=Rk, unbypassed):  |mu*RL / (RL+rp+(mu+1)*Zk)|.
double ccGainMag(const SmallSignal& ss, double Ra, double Rgl, double Rk,
                 double Ck, double f) {
    const double RL = Ra * Rgl / (Ra + Rgl);
    const double w = kTwoPi * f;
    const std::complex<double> Zk =
        (Ck > 0.0) ? std::complex<double>(Rk, 0.0) /
                         std::complex<double>(1.0, w * Rk * Ck)
                   : std::complex<double>(Rk, 0.0);
    const std::complex<double> den = RL + ss.rp + (ss.mu + 1.0) * Zk;
    return ss.mu * RL / std::abs(den);
}

// Analytic tone-stack transfer H(jw) = Vout/Vsource by complex nodal analysis of
// the SAME FMV/TMB netlist the runtime discretizes (documented in Jcm800Preamp.h).
std::complex<double> toneAnalytic(double f, double treble, double mid, double bass,
                                  double Rs) {
    using cd = std::complex<double>;
    const double w = kTwoPi * f;
    const cd jw(0.0, w);
    const double R1 = MarshallToneStack::kRslope, RT = MarshallToneStack::kRT,
                 RB = MarshallToneStack::kRB, RM = MarshallToneStack::kRM;
    const double Ct = MarshallToneStack::kCt, Cb = MarshallToneStack::kCb,
                 Cm = MarshallToneStack::kCm;
    auto cl = [](double v) { return v < 1e-3 ? 1e-3 : (v > 1 - 1e-3 ? 1 - 1e-3 : v); };
    const double t = cl(treble), m = cl(mid), l = cl(bass);
    constexpr int N = 5;
    enum { IN = 0, N2 = 1, N3 = 2, N4 = 3, OUT = 4 };
    cd G[N][N] = {}, b[N] = {};
    auto sr = [&](int a, int bb, double R) {
        cd g = 1.0 / (R < 1 ? 1 : R);
        G[a][a] += g; G[bb][bb] += g; G[a][bb] -= g; G[bb][a] -= g;
    };
    auto sc = [&](int a, int bb, double C) {
        cd y = jw * C;
        G[a][a] += y; G[bb][bb] += y; G[a][bb] -= y; G[bb][a] -= y;
    };
    cd gs = 1.0 / (Rs < 1 ? 1 : Rs);
    G[IN][IN] += gs; b[IN] += gs * cd(1.0, 0.0);
    sc(IN, N2, Ct); sr(IN, N3, R1); sr(N2, OUT, (1 - t) * RT); sr(OUT, N3, t * RT);
    sr(N3, N4, l * RB); sc(N3, N4, Cb);
    { cd g = 1.0 / ((m * RM) < 1 ? 1 : m * RM); G[N4][N4] += g; }
    G[N4][N4] += jw * Cm;
    // Gaussian elimination
    for (int c = 0; c < N; ++c) {
        int p = c;
        for (int r = c + 1; r < N; ++r)
            if (std::abs(G[r][c]) > std::abs(G[p][c])) p = r;
        for (int j = 0; j < N; ++j) std::swap(G[p][j], G[c][j]);
        std::swap(b[p], b[c]);
        cd d = G[c][c];
        for (int j = c; j < N; ++j) G[c][j] /= d;
        b[c] /= d;
        for (int r = 0; r < N; ++r) {
            if (r == c) continue;
            cd f2 = G[r][c];
            for (int j = c; j < N; ++j) G[r][j] -= f2 * G[c][j];
            b[r] -= f2 * b[c];
        }
    }
    return b[OUT];
}

// Steady-state |output amplitude| of the preamp at frequency f for a given input.
double chainAmpAt(Jcm800Preamp& pre, double f, float amp, double fs) {
    auto in = sine(f, amp, 1.0, fs);
    std::vector<float> out(in.size(), 0.0f);
    pre.process(in.data(), out.data(), static_cast<int>(in.size()));
    const size_t n = out.size(), win = n / 2, start = n - win;
    return goertzelAmp(out, start, win, f, fs);
}

// ---------------------------------------------------------------------------
// Test 1 — per-stage DC operating points vs load-line analysis (+/-5%).
// ---------------------------------------------------------------------------
void testOperatingPoints(double fs) {
    Jcm800Preamp pre;
    pre.prepare(fs, 128);

    struct Ref { const char* name; int idx; double Ra, Rk; };
    const Ref ccs[] = {
        {"V1A", Jcm800Preamp::V1A, 100.0e3, 820.0},
        {"V1B", Jcm800Preamp::V1B, 82.0e3, 10.0e3},
        {"V2A", Jcm800Preamp::V2A, 100.0e3, 820.0},
    };
    for (const Ref& r : ccs) {
        const double Iq = pre.stageQuiescentCurrent(r.idx);
        const double Va = pre.stageQuiescentPlate(r.idx);
        const double IqA = analyticCCcurrent(r.Ra, r.Rk);
        const double VaA = kBplus - IqA * r.Ra;
        assert(std::fabs(Va - VaA) / VaA < 0.05 && "CC stage Va off analytic load line >5%");
        assert(std::fabs(Iq - IqA) / IqA < 0.05 && "CC stage Iq off analytic load line >5%");
        std::printf("  [ok] %s DC @ %.0f Hz: Va=%.1f V (analytic %.1f), Iq=%.3f mA "
                    "(analytic %.3f), Vk=%.2f V\n",
                    r.name, fs, Va, VaA, Iq * 1e3, IqA * 1e3, pre.stageQuiescentCathode(r.idx));
    }

    // Direct-coupled follower: grid DC = V2A quiescent plate; solve Vk on the 100k
    // cathode load. Assert both the shared bias and the follower cathode DC.
    const double gridBias = pre.followerGridBias();
    assert(std::fabs(gridBias - pre.stageQuiescentPlate(Jcm800Preamp::V2A)) < 1e-6 &&
           "follower grid bias != V2A quiescent plate (direct coupling broken)");
    const double VkA = analyticFollowerVk(gridBias, 100.0e3);
    const double Vk = pre.stageQuiescentCathode(Jcm800Preamp::V2B);
    assert(std::fabs(Vk - VkA) / VkA < 0.05 && "follower cathode DC off analytic >5%");
    // Follower runs hotter than the gain stages (large cathode load, low Vgk).
    assert(pre.stageQuiescentCurrent(Jcm800Preamp::V2B) > 1.0e-3 &&
           "follower current implausibly low");
    std::printf("  [ok] V2B follower DC @ %.0f Hz: gridBias=%.1f V, Vk(out)=%.1f V "
                "(analytic %.1f), Iq=%.3f mA, Rout=%.0f ohm\n",
                fs, gridBias, Vk, VkA, pre.stageQuiescentCurrent(Jcm800Preamp::V2B) * 1e3,
                pre.followerSourceImpedance());
}

// ---------------------------------------------------------------------------
// Test 2 — small-signal chain gain at low GAIN vs the analytic product of the
// per-stage gains x interstage attenuations x tone-stack response (+/-2 dB).
// ---------------------------------------------------------------------------
void testChainGain(double fs) {
    const double f = 1000.0;
    const double gainKnob = 0.03, masterKnob = 1.0;  // low gain -> whole chain linear
    Jcm800Preamp pre;
    pre.prepare(fs, 128);
    pre.setOversampling(4);
    pre.setParameter(Jcm800Preamp::PARAM_GAIN, static_cast<float>(gainKnob));
    pre.setParameter(Jcm800Preamp::PARAM_MASTER, static_cast<float>(masterKnob));
    pre.setParameter(Jcm800Preamp::PARAM_BASS, 0.5f);
    pre.setParameter(Jcm800Preamp::PARAM_MID, 0.5f);
    pre.setParameter(Jcm800Preamp::PARAM_TREBLE, 0.5f);

    const float amp = 0.005f;
    const double measG = chainAmpAt(pre, f, amp, fs) / amp;

    // Analytic per-stage small-signal gains at each solved op point.
    auto ss = [&](int idx, double VgkSign) {
        const double Va = pre.stageQuiescentPlate(idx);
        const double Vk = pre.stageQuiescentCathode(idx);
        return smallSignal(Va, VgkSign * Vk);
    };
    const SmallSignal s1a = ss(Jcm800Preamp::V1A, -1.0);
    const SmallSignal s1b = ss(Jcm800Preamp::V1B, -1.0);
    const SmallSignal s2a = ss(Jcm800Preamp::V2A, -1.0);
    const double gV1A = ccGainMag(s1a, 100.0e3, 1.47e6, 820.0, 0.68e-6, f);
    const double gV1B = ccGainMag(s1b, 82.0e3, 470.0e3, 10.0e3, 0.0, f);
    const double gV2A = ccGainMag(s2a, 100.0e3, 1.0e6, 820.0, 0.68e-6, f);
    // Follower small-signal buffer gain gm*Rk/(1+gm*Rk).
    const double Vkf = pre.stageQuiescentCathode(Jcm800Preamp::V2B);
    const SmallSignal sf = smallSignal(kBplus - Vkf, pre.followerGridBias() - Vkf);
    const double gFol = sf.gm * 100.0e3 / (1.0 + sf.gm * 100.0e3);
    // Interstage: the GAIN network — since docs §47 a frequency-dependent divider
    // (series 470 k + 1 M pot + the 2204's 470 pF bright cap, top lug -> wiper).
    // |H(jw)| from the SAME nodal solve the runtime uses: H = (n0+jwn1)/(d0+jwd1),
    // H(0) = kGainDivider·wiper (the old scalar), H(inf) = Rl/(Rl+Rs).
    const double gainScale = [&] {
        const double wiper = Jcm800Preamp::gainTaper(gainKnob);  // GAIN law (§51)
        const double Rl = std::max(wiper, 1.0e-4) * Jcm800Preamp::kGainPotOhms;
        const double Ru = (1.0 - wiper) * Jcm800Preamp::kGainPotOhms;
        const double Rs = Jcm800Preamp::kGainSeriesOhms;
        const double C = Jcm800Preamp::kBrightCapF;
        const double w = 2.0 * M_PI * f;
        const std::complex<double> num(Rl, w * C * Ru * Rl);
        const std::complex<double> den(Rl + Rs + Ru, w * C * Ru * (Rl + Rs));
        return std::abs(num / den);
    }();
    const double htone = std::abs(toneAnalytic(f, 0.5, 0.5, 0.5, pre.followerSourceImpedance()));
    const double master = pre.masterScale();

    const double predG = gV1A * gainScale * gV1B * gV2A * gFol * htone * master;

    assert(std::fabs(toDb(measG) - toDb(predG)) < 2.0 &&
           "small-signal chain gain off analytic product >2 dB");
    std::printf("  [ok] chain gain @ %.0f Hz (gain=%.2f): measured %.2f dB, analytic %.2f dB "
                "[V1A %.1fx V1B %.1fx V2A %.1fx fol %.3f gainScale %.4f Htone %.2f dB]\n",
                fs, gainKnob, toDb(measG), toDb(predG), gV1A, gV1B, gV2A, gFol,
                gainScale, toDb(htone));
}

// ---------------------------------------------------------------------------
// Test 3 — passive tone stack vs analytic H(s), scooped (1,0,1) & flat
// (.5,.5,.5), at 100/650/3000 Hz (+/-1.5 dB); classic mid notch present.
// The tone stack is measured in isolation (its own low-Z source), decoupled
// from the tube nonlinearity, and compared to the complex nodal H(jw).
// ---------------------------------------------------------------------------
void testToneStack(double fs) {
    const double Rs = 371.0;  // representative follower output impedance
    struct Set { const char* name; double bass, mid, treble; };
    const Set sets[] = {
        {"scooped(1,0,1)", 1.0, 0.0, 1.0},
        {"flat(.5,.5,.5)", 0.5, 0.5, 0.5},
    };
    const double freqs[] = {100.0, 650.0, 3000.0};
    for (const Set& st : sets) {
        MarshallToneStack ts;
        ts.prepare(fs);
        ts.setSourceImpedance(Rs);
        ts.setKnobs(st.bass, st.mid, st.treble);
        for (double f : freqs) {
            ts.reset();
            auto in = sine(f, 0.1f, 1.0, fs);
            std::vector<float> out(in.size(), 0.0f);
            ts.process(in.data(), out.data(), static_cast<int>(in.size()));
            const size_t n = out.size(), win = n / 2, start = n - win;
            const double meas = toDb(goertzelAmp(out, start, win, f, fs) / 0.1);
            const double ana = toDb(std::abs(toneAnalytic(f, st.treble, st.mid, st.bass, Rs)));
            assert(std::fabs(meas - ana) < 1.5 &&
                   "tone-stack response off analytic H(s) >1.5 dB");
            std::printf("  [ok] tone %-14s @ %.0f Hz: %.0f Hz meas %.2f dB (analytic %.2f)\n",
                        st.name, fs, f, meas, ana);
        }
    }
    // Classic Marshall mid notch present in the scooped setting (fine analytic scan).
    double minDb = 1e9, minF = 0.0;
    for (double f = 100.0; f < 2000.0; f *= 1.005) {
        const double db = toDb(std::abs(toneAnalytic(f, 1.0, 0.0, 1.0, Rs)));
        if (db < minDb) { minDb = db; minF = f; }
    }
    assert(minF > 300.0 && minF < 800.0 && "scoop mid-notch not in the 300-800 Hz Marshall band");
    assert(minDb < -12.0 && "scoop mid-notch not deep enough (<-12 dB)");
    std::printf("  [ok] tone scoop mid-notch @ %.0f Hz base: %.1f dB @ %.0f Hz (Marshall band)\n",
                fs, minDb, minF);
}

// ---------------------------------------------------------------------------
// Test 4 — gain character: THD grows monotonically with the GAIN knob; at high
// gain the output clips asymmetrically (crunch); and (white-box) V1B's cold
// grid is driven beyond its linear window (peak grid drive > its cathode bias).
// ---------------------------------------------------------------------------
void testGainCharacter(double fs) {
    const double f0 = 1000.0;
    auto measure = [&](double gainKnob, double& thd, double& asym, double& v1bPk,
                       double& outPk) {
        Jcm800Preamp pre;
        pre.prepare(fs, 128);
        pre.setOversampling(4);
        pre.setParameter(Jcm800Preamp::PARAM_GAIN, static_cast<float>(gainKnob));
        pre.setParameter(Jcm800Preamp::PARAM_MASTER, 0.5f);
        auto in = sine(f0, 0.1f, 0.5, fs);
        std::vector<float> out(in.size(), 0.0f);
        pre.process(in.data(), out.data(), static_cast<int>(in.size()));
        const size_t n = out.size(), win = n / 2, start = n - win;
        const double f1 = goertzelAmp(out, start, win, f0, fs);
        double h = 0.0;
        for (int k = 2; k <= 6; ++k) {
            const double a = goertzelAmp(out, start, win, k * f0, fs);
            h += a * a;
        }
        thd = std::sqrt(h) / f1;
        double pp = 0.0, pn = 0.0;
        for (size_t i = n / 2; i < n; ++i) {
            pp = std::max(pp, static_cast<double>(out[i]));
            pn = std::max(pn, -static_cast<double>(out[i]));
        }
        asym = std::fabs(pp - pn) / std::max(pp, pn);
        v1bPk = pre.lastV1bGridDrivePeak();
        outPk = pre.lastOutputPeak();
    };

    double thd[4], asym[4], v1bPk[4], outPk[4];
    const double knobs[4] = {0.1, 0.3, 0.6, 1.0};
    for (int i = 0; i < 4; ++i) measure(knobs[i], thd[i], asym[i], v1bPk[i], outPk[i]);

    for (int i = 1; i < 4; ++i)
        assert(thd[i] > thd[i - 1] && "THD not monotonic with the GAIN knob");
    // Asymmetric clip at high gain (grid conduction / cutoff act on opposite peaks).
    assert(asym[3] > 0.08 && "high-gain output not asymmetric (crunch missing)");
    // White-box: V1B's cold grid is driven past its linear window at high gain.
    // The window before cutoff/grid conduction ~ its cathode self-bias voltage.
    Jcm800Preamp probe;
    probe.prepare(fs, 128);
    const double v1bBias = probe.stageQuiescentCathode(Jcm800Preamp::V1B);
    assert(v1bPk[3] > v1bBias &&
           "V1B grid drive did not exceed its linear window at high gain");
    assert(v1bPk[0] < v1bBias &&
           "V1B already nonlinear at low gain (expected linear)");
    std::printf("  [ok] gain character @ %.0f Hz: THD %.1f/%.1f/%.1f/%.1f%% (gain .1/.3/.6/1), "
                "asym@max %.1f%%, V1B gridPk %.2f>%.2f V (bias window)\n",
                fs, thd[0] * 100, thd[1] * 100, thd[2] * 100, thd[3] * 100, asym[3] * 100,
                v1bPk[3], v1bBias);
}

// ---------------------------------------------------------------------------
// Test 5 — blocking / recovery on an overdrive burst (the cascade's coupling
// RCs) plus a +/-10 V slam: bounded recovery, all finite, stable.
// ---------------------------------------------------------------------------
void testBlockingStability(double fs, int os) {
    Jcm800Preamp pre;
    pre.prepare(fs, 128);
    pre.setOversampling(os);
    pre.setParameter(Jcm800Preamp::PARAM_GAIN, 0.9f);
    pre.setParameter(Jcm800Preamp::PARAM_MASTER, 0.6f);

    const int nB = static_cast<int>(0.15 * fs);  // 150 ms hard burst
    const int nS = static_cast<int>(0.50 * fs);  // 500 ms silence (recovery + wobble)
    const int n = nB + nS;
    std::vector<float> in(static_cast<size_t>(n), 0.0f), out(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < nB; ++i) {
        double s = 1.5 * std::sin(kTwoPi * 180.0 * i / fs);
        if (i % 4000 < 20) s += (i & 1) ? 10.0 : -10.0;  // +/-10 V slams inside the burst
        in[static_cast<size_t>(i)] = static_cast<float>(s);
    }
    pre.process(in.data(), out.data(), n);

    double pk = 0.0;
    for (float v : out) {
        assert(std::isfinite(v) && "non-finite output under overdrive/slam");
        pk = std::max(pk, static_cast<double>(std::fabs(v)));
    }
    assert(pk < 1000.0 && "output not bounded under overdrive/slam");
    // Bounded recovery: after the burst the cascade's coupling RCs recover (with a
    // decaying low-frequency blocking wobble as the stages re-bias); the final
    // window settles far below the driven level. No latch-up / runaway.
    auto tailRms = [&](int a, int b) {
        double e = 0.0;
        for (int i = a; i < b; ++i) e += static_cast<double>(out[i]) * out[i];
        return std::sqrt(e / std::max(1, b - a));
    };
    const double drivenRms = tailRms(nB - static_cast<int>(0.05 * fs), nB);
    const double recovRms = tailRms(n - static_cast<int>(0.10 * fs), n);
    assert(recovRms < 0.01 * drivenRms + 1e-4 &&
           "preamp did not recover after the overdrive burst");
    std::printf("  [ok] blocking/stability @ %.0f Hz os=%d: finite, peak %.2f, "
                "driven rms %.3f -> recovered rms %.5f\n",
                fs, os, pk, drivenRms, recovRms);
}

// ---------------------------------------------------------------------------
// Test 6 — aliasing of the whole cascade (M2 sweep). Drive a high sine hard;
// the shipped 4x must clear a wide audibility margin and improve on 2x.
// ---------------------------------------------------------------------------
void testAliasing(double fs, bool strict) {
    const double f0 = 4186.0;
    auto in = sine(f0, 0.1f, 1.0, fs);
    double worst[4];
    int idx = 0;
    for (int os : {1, 2, 4, 8}) {
        Jcm800Preamp pre;
        pre.prepare(fs, 128);
        pre.setOversampling(os);
        pre.setParameter(Jcm800Preamp::PARAM_GAIN, 1.0f);  // MAX gain (cranked)
        pre.setParameter(Jcm800Preamp::PARAM_MASTER, 0.5f);
        std::vector<float> out(in.size(), 0.0f);
        pre.process(in.data(), out.data(), static_cast<int>(in.size()));
        worst[idx++] = measureAliasing(out, fs, f0).worstAliasDb;
    }
    const double w1 = worst[0], w2 = worst[1], w4 = worst[2], w8 = worst[3];
    assert(w2 < w1 - 8.0 && "2x not >=8 dB better than 1x");
    // At a 44.1k base 2x is still above the floor, so 4x strictly improves; at a
    // 96k base 2x already bottoms out at the cascade's alias floor (w4 ~ w2 ~ w8).
    if (strict) assert(w4 < w2 - 10.0 && "shipped 4x not >=10 dB better than 2x (44.1k)");
    // The compound cascade's alias FLOOR: 4x reaches it and 8x buys nothing (w8 ~
    // w4), so 4x is the measured requirement. It clears the M2 audibility bar
    // (worst-alias >= 60 dB below the fundamental) with margin at MAX gain.
    assert(w4 < -60.0 && "shipped 4x worst-alias not >=60 dB below fundamental (M2 bar)");
    assert(w8 < w4 + 3.0 && "8x materially better than 4x (4x not at the floor)");
    std::printf("  [ok] aliasing @ %.0f Hz base (high gain): 1x=%.1f 2x=%.1f 4x=%.1f 8x=%.1f "
                "dB (shipped 4x; 8x adds ~%.0f dB)\n",
                fs, w1, w2, w4, w8, w4 - w8);
}

}  // namespace

// ---------------------------------------------------------------------------
// The 470 pF bright-cap shelf (docs §47): the gain network's HF tilt at mid gain
// matches the analytic H of the real parts, and vanishes fully open. Measured as a
// RATIO OF RATIOS — (5 kHz / 110 Hz) at GAIN 0.5 over the same at GAIN 1.0 — so the
// tone stack, follower and triode responses cancel and only the gain network's
// frequency dependence is left. Perturbation-proven: kBrightCapF = 0 (or the old
// flat-scalar loop) measures ~0 dB of tilt and fails the >= 4 dB bar outright.
// ---------------------------------------------------------------------------
void testBrightCap(double fs) {
    auto respDb = [&](float gainKnob, double f) {
        Jcm800Preamp pre;
        pre.prepare(fs, 128);
        pre.setOversampling(4);
        pre.setParameter(Jcm800Preamp::PARAM_GAIN, gainKnob);
        pre.setParameter(Jcm800Preamp::PARAM_BASS, 0.5f);
        pre.setParameter(Jcm800Preamp::PARAM_MID, 0.5f);
        pre.setParameter(Jcm800Preamp::PARAM_TREBLE, 0.6f);
        const float amp = 0.002f;
        return toDb(chainAmpAt(pre, f, amp, fs) / amp);
    };
    auto tiltAt = [&](float g) {
        return (respDb(g, 5000.0) - respDb(g, 110.0)) -
               (respDb(1.0f, 5000.0) - respDb(1.0f, 110.0));
    };
    auto analyticTilt = [&](float g) {
        auto H = [&](double f) {
            const double wiper = Jcm800Preamp::gainTaper(g);  // GAIN law (§51)
            const double Rl = std::max(wiper, 1.0e-4) * Jcm800Preamp::kGainPotOhms;
            const double Ru = (1.0 - wiper) * Jcm800Preamp::kGainPotOhms;
            const double Rs = Jcm800Preamp::kGainSeriesOhms;
            const double C = Jcm800Preamp::kBrightCapF;
            const double w = 2.0 * M_PI * f;
            const std::complex<double> num(Rl, w * C * Ru * Rl);
            const std::complex<double> den(Rl + Rs + Ru, w * C * Ru * (Rl + Rs));
            return std::abs(num / den);
        };
        return toDb(H(5000.0)) - toDb(H(110.0));
    };
    const double t05 = tiltAt(0.5f), a05 = analyticTilt(0.5f);
    const double t07 = tiltAt(0.7f), a07 = analyticTilt(0.7f);
    std::printf("  [ok] bright cap @ %.0f Hz: gain-network HF tilt (5k vs 110, rel GAIN 1.0) "
                "gain .5 = %.1f dB (analytic %.1f), gain .7 = %.1f dB (analytic %.1f)\n",
                fs, t05, a05, t07, a07);
    std::fflush(stdout);
    assert(std::fabs(t05 - a05) < 2.0 && std::fabs(t07 - a07) < 2.0 &&
           "bright-cap tilt off the analytic H of the real parts by > 2 dB");
    assert(t05 > 4.0 &&
           "bright-cap tilt at mid gain under 4 dB — the 470 pF network is gone "
           "(flat scalar again? docs §47)");
    assert(t05 > t07 &&
           "bright-cap tilt does not GROW as the gain knob comes down — wrong law");
}

int main() {
    clipper::test::requireAssertsLive();
    std::printf("Running clipper::dsp::Jcm800Preamp (M9.2) tests...\n");
    testOperatingPoints(44100.0);
    testOperatingPoints(96000.0);
    testChainGain(44100.0);
    testBrightCap(44100.0);
    testBrightCap(48000.0);
    testChainGain(96000.0);
    testToneStack(44100.0);
    testToneStack(96000.0);
    testGainCharacter(44100.0);
    testGainCharacter(96000.0);
    testBlockingStability(44100.0, 4);
    testBlockingStability(96000.0, 4);
    testBlockingStability(44100.0, 8);
    testAliasing(44100.0, /*strict=*/true);
    testAliasing(96000.0, /*strict=*/false);
    std::printf("All Jcm800Preamp tests passed.\n");
    return 0;
}
