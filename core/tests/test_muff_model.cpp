// Plain-assert tests for clipper::dsp::MuffModel (v1.1 item 4 — the four-transistor
// "wall of sustain" fuzz) and its reusable clipper::dsp::BjtStage (Ebers-Moll NPN).
// No framework: int main + <cassert>. Frequency content via a hand-rolled Goertzel;
// aliasing via the shared measure/AliasMetric.h. Mirrors test_ts_model.cpp's shape.
//
// The headline measurements (see docs/DEVELOPMENT.md §24):
//   1. Per-stage DC operating point vs an INDEPENDENT analytic bias solve (±10%).
//   2. The famous mid-scoop TONE stack: notch depth + center (~1 kHz) and the tilt
//      extremes match the analytic H(s) (MuffToneStack::analyticMagDb).
//   3. THD at max SUSTAIN is MASSIVE (fuzz, not overdrive) — documented %.
//   4. Sustain/compression wall: output RMS is ~flat across a 20 dB input sweep at
//      high sustain (the wall-of-sustain assert).
//   5. Aliasing: shipped 4× at max sustain below the M2 −60 dB bar; naive far worse.
//   6. VOLUME linearity; stability + hygiene at ±10 V, all rates.

#include "clipper/dsp/MuffModel.h"

#include "clipper/dsp/BjtStage.h"
#include "measure/AliasMetric.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::BjtStage;
using clipper::dsp::MuffModel;
using clipper::dsp::MuffToneStack;

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
double tailRms(const std::vector<float>& x, double fs) {
    const size_t skip = std::min(x.size(), static_cast<size_t>(0.2 * fs));
    double acc = 0.0; size_t n = 0;
    for (size_t i = skip; i < x.size(); ++i) { acc += double(x[i]) * x[i]; ++n; }
    return n ? std::sqrt(acc / n) : 0.0;
}

struct Params { float sustain, tone, volume; };
std::vector<float> render(const std::vector<float>& in, Params p, double fs,
                          int os = 4, int clipMode = MuffModel::CLIP_OVERSAMPLED) {
    MuffModel m;
    m.prepare(fs, 128);
    m.setOversampling(os);
    m.setClipMode(clipMode);
    m.setParameter(MuffModel::PARAM_SUSTAIN, p.sustain);
    m.setParameter(MuffModel::PARAM_TONE, p.tone);
    m.setParameter(MuffModel::PARAM_VOLUME, p.volume);
    std::vector<float> out(in.size(), 0.0f);
    if (!in.empty()) m.process(in.data(), out.data(), static_cast<int>(in.size()));
    return out;
}

// THD (fundamental f0, harmonics 2..hi) over the signal tail.
double thd(const std::vector<float>& o, double f0, double fs, int hi = 12) {
    const size_t n = o.size(), win = std::min(n, static_cast<size_t>(fs));
    const double f1 = goertzelAmp(o, n - win, win, f0, fs);
    double e = 0.0;
    for (int h = 2; h <= hi; ++h) {
        const double a = goertzelAmp(o, n - win, win, f0 * h, fs);
        e += a * a;
    }
    return std::sqrt(e) / (f1 + 1e-12);
}

// --- INDEPENDENT analytic DC bias solve (fresh code, not the model's solver). ---
// Same collector-feedback KCL as BjtStage, but written here from scratch and solved
// by a damped 3-node Newton, so agreement with the model's quiescents genuinely
// cross-validates the device model + solver. Diodes optional (clip stages).
struct DcOp { double Vb, Vc, Ve, Ic; };
DcOp analyticBias(const BjtStage::Config& c) {
    const BjtStage::EbersMoll& p = c.bjt;
    const double gRc = 1.0 / c.Rc, gRf = 1.0 / c.Rf, gRe = 1.0 / c.Re;
    auto id = [&](double v) {  // antiparallel diode pair
        if (!c.diodes.present) return 0.0;
        return c.diodes.Isd * (std::exp(std::min(v / c.diodes.nVt, 60.0)) -
                               std::exp(std::min(-v / c.diodes.nVt, 60.0)));
    };
    double Vb = 0.85, Vc = 1.3, Ve = 0.28;
    for (int it = 0; it < 500; ++it) {
        const double Vbe = Vb - Ve, Vbc = Vb - Vc;
        const double Ic = BjtStage::ebersMollIc(Vbe, Vbc, p);
        const double Ib = BjtStage::ebersMollIb(Vbe, Vbc, p);
        const double Ie = Ic + Ib;
        const double Id = id(Vc - Vb);
        const double r1 = (Vc - Vb) * gRf + Id - Ib;
        const double r2 = (c.Vcc - Vc) * gRc + (Vb - Vc) * gRf - Ic - Id;
        const double r3 = Ie - Ve * gRe;
        // Numerical Jacobian (independent of the model's analytic one).
        const double h = 1e-6;
        auto res = [&](double b, double cc, double e, double r[3]) {
            const double vbe = b - e, vbc = b - cc;
            const double ic = BjtStage::ebersMollIc(vbe, vbc, p);
            const double ib = BjtStage::ebersMollIb(vbe, vbc, p);
            const double idd = id(cc - b);
            r[0] = (cc - b) * gRf + idd - ib;
            r[1] = (c.Vcc - cc) * gRc + (b - cc) * gRf - ic - idd;
            r[2] = (ic + ib) - e * gRe;
        };
        double J[3][3], rb[3], base[3] = {r1, r2, r3};
        res(Vb + h, Vc, Ve, rb); for (int k = 0; k < 3; ++k) J[k][0] = (rb[k] - base[k]) / h;
        res(Vb, Vc + h, Ve, rb); for (int k = 0; k < 3; ++k) J[k][1] = (rb[k] - base[k]) / h;
        res(Vb, Vc, Ve + h, rb); for (int k = 0; k < 3; ++k) J[k][2] = (rb[k] - base[k]) / h;
        // Solve 3x3 (Cramer).
        const double det =
            J[0][0]*(J[1][1]*J[2][2]-J[1][2]*J[2][1]) -
            J[0][1]*(J[1][0]*J[2][2]-J[1][2]*J[2][0]) +
            J[0][2]*(J[1][0]*J[2][1]-J[1][1]*J[2][0]);
        if (std::fabs(det) < 1e-30) break;
        const double b[3] = {-r1, -r2, -r3};
        double dx[3];
        for (int col = 0; col < 3; ++col) {
            double m[3][3];
            for (int r = 0; r < 3; ++r) { m[r][0]=J[r][0]; m[r][1]=J[r][1]; m[r][2]=J[r][2]; }
            for (int r = 0; r < 3; ++r) m[r][col] = b[r];
            const double d =
                m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1]) -
                m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0]) +
                m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
            dx[col] = d / det;
        }
        for (double& v : dx) v = v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v);
        Vb += dx[0]; Vc += dx[1]; Ve += dx[2];
        if (std::fabs(dx[0]) < 1e-11 && std::fabs(dx[1]) < 1e-11 && std::fabs(dx[2]) < 1e-11) break;
    }
    return {Vb, Vc, Ve, BjtStage::ebersMollIc(Vb - Ve, Vb - Vc, p)};
}

// --- Test 1: per-stage DC operating point vs the analytic bias (±10%). --------
void testDcOperatingPoints(double fs) {
    MuffModel m;
    m.prepare(fs, 128);
    m.setOversampling(4);
    BjtStage::Config nodi;             // Q1/Q4 (no diodes)
    BjtStage::Config di = nodi; di.diodes.present = true;  // Q2/Q3 (clip)
    const DcOp aNo = analyticBias(nodi);
    const DcOp aDi = analyticBias(di);
    const bool clip[4] = {false, true, true, false};
    for (int i = 0; i < 4; ++i) {
        const BjtStage& q = m.stage(i);
        const DcOp& a = clip[i] ? aDi : aNo;
        const double vc = q.quiescentCollectorVoltage();
        // Physical: collector strictly inside the rails; active (not saturated flat).
        assert(vc > 0.2 && vc < 9.0 && "collector out of rails");
        assert(q.quiescentCollectorCurrent() > 1e-5 && "stage not biased on");
        const double relErr = std::fabs(vc - a.Vc) / std::fabs(a.Vc);
        assert(relErr < 0.10 && "stage DC collector Vc deviates >10% from analytic bias");
        std::printf(
            "  [ok] Q%d DC @ %.0f Hz: model Vc=%.3f V (analytic %.3f, %.1f%%), "
            "Vb=%.3f Ve=%.3f Ic=%.3f mA%s\n",
            i + 1, fs, vc, a.Vc, relErr * 100.0, q.quiescentBaseVoltage(),
            q.quiescentEmitterVoltage(), q.quiescentCollectorCurrent() * 1e3,
            clip[i] ? " [clip diodes]" : "");
    }
}

// --- Test 2: the mid-scoop TONE stack vs analytic H(s). -----------------------
// Validated in ISOLATION (the transistor stages surround it in the pedal), at the
// OVERSAMPLED rate the model runs it. Assert: (a) rendered == analytic within
// tolerance across the band; (b) at TONE=0.5 there is a NOTCH near ~1 kHz (a local
// minimum below both a low and a high shoulder); (c) the tilt extremes — TONE 0 is
// DARK (HF cut), TONE 1 is BRIGHT (LF cut).
void testMidScoop(double fs) {
    const double osr = fs * 4.0;  // the rate MuffModel prepares the tone stack at
    auto measDb = [&](double f, float tone) {
        MuffToneStack ts; ts.prepare(osr); ts.setTone(tone);
        auto in = sine(f, 0.1f, 0.25, osr);
        std::vector<float> out(in.size());
        for (size_t i = 0; i < in.size(); ++i) out[i] = ts.processSample(in[i]);
        const size_t n = out.size(), win = std::min(n, static_cast<size_t>(0.12 * osr));
        return toDb(goertzelAmp(out, n - win, win, f, osr)) -
               toDb(goertzelAmp(in, n - win, win, f, osr));
    };
    // (a) rendered matches analytic H(s) at TONE=0.5.
    const double probes[] = {100, 300, 700, 980, 1500, 3000, 6000};
    double worst = 0.0;
    for (double f : probes) {
        const double dev = std::fabs(measDb(f, 0.5f) - MuffToneStack::analyticMagDb(f, 0.5));
        worst = std::max(worst, dev);
        assert(dev < 0.6 && "tone-stack rendered response deviates from analytic H(s)");
    }
    // (b) the noon NOTCH: scan the analytic response for the minimum, assert it is a
    // real dip sitting in the ~1 kHz region (700..1300 Hz).
    double notchF = 0.0, notchDb = 1e9, lowShoulder = MuffToneStack::analyticMagDb(120, 0.5),
           highShoulder = MuffToneStack::analyticMagDb(6000, 0.5);
    for (double f = 200; f <= 4000; f += 5) {
        const double db = MuffToneStack::analyticMagDb(f, 0.5);
        if (db < notchDb) { notchDb = db; notchF = f; }
    }
    assert(notchF > 700.0 && notchF < 1300.0 && "mid-scoop notch not in the ~1 kHz region");
    assert(notchDb < lowShoulder - 1.0 && notchDb < highShoulder - 1.0 &&
           "no mid-scoop: notch is not below both shoulders");
    // (c) tilt extremes: at 5 kHz, TONE 1 (bright/HP) is far louder than TONE 0 (dark/LP);
    //     at 120 Hz the reverse.
    const double hiToneAtHf = measDb(5000, 1.0f), loToneAtHf = measDb(5000, 0.0f);
    const double hiToneAtLf = measDb(120, 1.0f), loToneAtLf = measDb(120, 0.0f);
    assert(hiToneAtHf > loToneAtHf + 12.0 && "TONE=1 not brighter than TONE=0 at HF");
    assert(loToneAtLf > hiToneAtLf + 12.0 && "TONE=0 not darker/bassier than TONE=1 at LF");
    std::printf(
        "  [ok] mid-scoop @ %.0f Hz: notch %.2f dB @ %.0f Hz (shoulders %.2f/%.2f dB); "
        "analytic match worst %.2f dB; tilt HF t1=%.1f/t0=%.1f LF t1=%.1f/t0=%.1f dB\n",
        fs, notchDb, notchF, lowShoulder, highShoulder, worst, hiToneAtHf, loToneAtHf,
        hiToneAtLf, loToneAtLf);
}

// --- Test 3: THD at max SUSTAIN is MASSIVE (fuzz, not overdrive). --------------
void testMassiveThd(double fs) {
    const double f0 = 220.0;
    const double tMax = thd(render(sine(f0, 0.2f, 1.0, fs), {1.0f, 0.5f, 0.6f}, fs), f0, fs);
    const double tLo = thd(render(sine(f0, 0.2f, 1.0, fs), {0.1f, 0.5f, 0.6f}, fs), f0, fs);
    // A fuzz, not an overdrive: THD is ENORMOUS at EVERY sustain setting — the Muff
    // has no clean setting (the double diode clip slams even at low sustain). Both
    // RAT/SD-1/TS top out ~20-30%; the Muff sits ~80-95%.
    assert(tMax > 0.5 && tLo > 0.5 && "Muff THD not fuzz-huge (>50%) at both extremes");
    std::printf("  [ok] massive THD @ %.0f Hz: SUSTAIN 0.1 -> %.1f%%, 1.0 -> %.1f%% (FUZZ, no clean setting)\n",
                fs, tLo * 100.0, tMax * 100.0);
}

// --- Test 4: the sustain/compression WALL. ------------------------------------
// At high SUSTAIN the double diode clip compresses hard: output RMS stays ~flat as
// the INPUT sweeps 20 dB (the wall-of-sustain signature — the pedal makes the same
// wall whether you pick soft or hard). Contrast with a low-sustain setting, which
// tracks the input far more.
void testSustainWall(double fs) {
    const float amps[] = {0.05f, 0.10f, 0.20f, 0.35f, 0.50f};  // ~20 dB sweep
    double hiMin = 1e9, hiMax = 0.0;
    for (float a : amps) {
        const double r = tailRms(render(sine(220.0, a, 0.6, fs), {1.0f, 0.5f, 0.6f}, fs), fs);
        hiMin = std::min(hiMin, r); hiMax = std::max(hiMax, r);
    }
    const double hiSpreadDb = 20.0 * std::log10(hiMax / (hiMin + 1e-12));
    // Wall: <= 4 dB output variation across a 20 dB input sweep (measured ~1 dB).
    assert(hiSpreadDb < 4.0 && "no sustain wall: output not compressed across the input sweep");
    // Sanity that it IS compression: a 20 dB input step yields far less than 20 dB out.
    const double loR = tailRms(render(sine(220.0, 0.05f, 0.6, fs), {1.0f, 0.5f, 0.6f}, fs), fs);
    const double hiR = tailRms(render(sine(220.0, 0.50f, 0.6, fs), {1.0f, 0.5f, 0.6f}, fs), fs);
    const double outStepDb = 20.0 * std::log10(hiR / (loR + 1e-12));
    assert(std::fabs(outStepDb) < 6.0 && "20 dB input step not heavily compressed at max sustain");
    std::printf(
        "  [ok] sustain wall @ %.0f Hz: outRMS spread %.2f dB across a 20 dB input sweep "
        "(20 dB in -> %.2f dB out)\n",
        fs, hiSpreadDb, outStepDb);
}

// --- Test 5: aliasing. Shipped 4× at max SUSTAIN below the M2 −60 dB bar. ------
void testAliasing(double fs) {
    using clipper::measure::measureAliasing;
    const double f0 = 4186.0;  // C8
    auto in = sine(f0, 0.2f, 1.0, fs);
    const double w4 = measureAliasing(render(in, {1.0f, 0.5f, 0.6f}, fs, 4), fs, f0).worstAliasDb;
    assert(w4 < -60.0 && "Muff 4x worst-alias at max sustain not >= 60 dB below fundamental");
    const double wNaive =
        measureAliasing(render(in, {1.0f, 0.5f, 0.6f}, fs, 1, MuffModel::CLIP_NAIVE), fs, f0)
            .worstAliasDb;
    assert(w4 < wNaive - 30.0 && "4x did not dramatically improve on naive (double-clip is harsh)");
    std::printf("  [ok] aliasing @ %.0f Hz base: 4x worst-alias %.1f dB (bar -60); naive %.1f dB\n",
                fs, w4, wNaive);
}

// --- Test 6: VOLUME linearity. ------------------------------------------------
void testVolumeLinearity() {
    const double fs = 48000.0;
    auto in = sine(220.0, 0.2f, 0.5, fs);
    auto rmsAt = [&](float v) { return tailRms(render(in, {0.8f, 0.5f, v}, fs), fs); };
    const double r25 = rmsAt(0.25f), r50 = rmsAt(0.50f), r100 = rmsAt(1.0f);
    assert(std::fabs(r50 / r25 - 2.0) < 0.06 && "VOLUME not linear 0.25 -> 0.5");
    assert(std::fabs(r100 / r50 - 2.0) < 0.06 && "VOLUME not linear 0.5 -> 1.0");
    std::printf("  [ok] VOLUME linearity: r50/r25=%.3f r100/r50=%.3f\n", r50 / r25, r100 / r50);
}

// --- Test 7: stability + hygiene (finite, silence->silence, deterministic). ----
void testStabilityHygiene() {
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        // ±10 V square slam: finite + bounded at every rate.
        std::vector<float> slam(static_cast<size_t>(fs * 0.2));
        for (size_t i = 0; i < slam.size(); ++i) slam[i] = (i % 3) ? 10.0f : -10.0f;
        auto o = render(slam, {1.0f, 0.5f, 1.0f}, fs);
        double pk = 0.0;
        for (float v : o) { assert(std::isfinite(v) && "non-finite on ±10 V slam"); pk = std::max(pk, (double)std::fabs(v)); }
        assert(pk < 100.0 && "output blew up on the slam (unbounded)");
        std::printf("  [ok] slam @ %.0f Hz: finite, bounded (peak %.2f V)\n", fs, pk);
    }
    const double fs = 48000.0;
    auto in = sine(330.0, 0.3f, 0.2, fs);
    for (float su = 0.0f; su <= 1.0f; su += 0.25f)
        for (float tn = 0.0f; tn <= 1.0f; tn += 0.25f)
            for (float vo = 0.0f; vo <= 1.0f; vo += 0.5f) {
                auto o = render(in, {su, tn, vo}, fs);
                for (float v : o) assert(std::isfinite(v) && "non-finite output on the grid");
            }
    {  // silence -> silence (no DC pumping / turn-on thump)
        std::vector<float> zeros(static_cast<size_t>(fs * 0.3), 0.0f);
        auto o = render(zeros, {1.0f, 0.5f, 1.0f}, fs);
        double dc = 0.0, pk = 0.0;
        for (float v : o) { dc += v; pk = std::max(pk, (double)std::fabs(v)); }
        dc /= o.size();
        assert(pk < 1e-4 && "silence produced output");
        assert(std::fabs(dc) < 1e-3 && "DC offset on silence");
    }
    {  // determinism
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
    std::printf("Running clipper::dsp::MuffModel tests (v1.1 item 4 — the fuzz + BjtStage)...\n");
    testDcOperatingPoints(44100.0);
    testDcOperatingPoints(96000.0);
    testMidScoop(44100.0);
    testMidScoop(96000.0);
    testMassiveThd(44100.0);
    testMassiveThd(48000.0);
    testSustainWall(48000.0);
    testAliasing(44100.0);
    testAliasing(96000.0);
    testVolumeLinearity();
    testStabilityHygiene();
    std::printf("All MuffModel tests passed.\n");
    return 0;
}
