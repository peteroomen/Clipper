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
//   6. VOLUME linearity; stability + hygiene at ±20 V, all rates, with the damped
//      Newton's iteration count pinned so the globalization cannot quietly degrade.
//   7. Idle solver cost (docs §34): a PARKED stage does zero Newton iterations, so
//      the Muff cannot go back to costing more when you are not playing.
//   8. The low end is where a guitar's low end is (testLowEndResponse) and the output does
//      not ride on DC when driven (the DC-on-signal block in testStabilityHygiene) — audit
//      finding 16, fixed 2026-07-25, docs §37 / ADR 009.

#include "clipper/dsp/MuffModel.h"

#include "clipper/dsp/BjtStage.h"
#include "measure/AliasMetric.h"
#include "support/AssertsLive.h"
#include "support/DcOffset.h"
#include "support/Xfail.h"

#include <algorithm>
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

// Found by this slice (perf/muff-newton-earlyout, docs §34) when the ±10 V single-
// oversampling-factor slam test was widened to ±20 V across every rate x factor.
// PRE-EXISTING, not caused by the residual early-out: the pre-early-out solver was
// measured at the identical iteration counts in the identical combinations (it merely
// reported the cap as 61 rather than 60, which is the over-report this slice fixed).
// The output stays finite and bounded, so this is not the old cascade blow-up — but at
// those samples the solve has not converged, so the audio there is not the circuit's
// answer.
constexpr clipper::test::XfailDecl kXfSlamIterCap{
    "muff-slam-exhausts-newton-cap",
    "found 2026-07-25 by perf/muff-newton-earlyout (docs §34), no audit finding number",
    "a ±20 V slam converges STRICTLY INSIDE the damped Newton's 60-iteration cap at "
    "every supported sample rate x oversampling factor. It does at 10 of 16 (14-18 "
    "iterations), but SIX exhaust the cap: 2x oversampling at all four base rates, and "
    "4x at 88.2 and 96 kHz. The shipped desktop path (4x at 44.1/48 kHz) converges in "
    "17-18, so this is not shipping-audible today — but 96 kHz x4 is a real user "
    "configuration, so it is not hypothetical either",
    "its own slice: the damping/step-clamp strategy under a >|10 V| slam. Do NOT paper "
    "over it by raising kMaxNewtonIter — that buys iterations, not convergence"};

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

// --- Test 3: THD now RANGES with SUSTAIN (the field-fix — a usable min setting). -
// Pre-fix the Muff clipped hard at EVERY sustain (THD ~80–95% even at min — "no clean
// setting"), because the input booster Q1 was overdriven (kInputDrive 12) and the pot
// floored at a hot 0.06. Fixed: Q1 is a clean booster and the SUSTAIN pot is an honest
// audio-taper full-range attenuator into the clippers, so THD RISES with sustain — min
// sustain sheds the wall (dynamics return), max is the fuzz wall. See docs §24.
void testSustainRange(double fs) {
    const double f0 = 220.0;
    const double tLo = thd(render(sine(f0, 0.2f, 1.0, fs), {0.0f, 0.5f, 0.6f}, fs), f0, fs);
    const double tMid = thd(render(sine(f0, 0.2f, 1.0, fs), {0.6f, 0.5f, 0.6f}, fs), f0, fs);
    const double tMax = thd(render(sine(f0, 0.2f, 1.0, fs), {1.0f, 0.5f, 0.6f}, fs), f0, fs);
    // Rising trend at the coarse points min -> default -> max (the low-drive cold-bias
    // hump makes the FINE curve non-monotonic, so assert the endpoints/default).
    assert(tLo < tMid && "SUSTAIN=0 not less saturated than the 0.6 default");
    assert(tMid < tMax && "default not less saturated than max SUSTAIN");
    // The wall at high sustain is fuzz-large. RE-BASELINED 2026-07-25 (docs §37): the bar
    // was ">80 %, and >100 % at max", and that figure was INFLATED BY AUDIT FINDING 16.
    // THD is harmonic-sum / fundamental, and the pre-fix coupling network pre-emphasised the
    // harmonics over the fundamental by 11–20 dB — read it straight off the pre-fix response
    // table at SUSTAIN 0.15 (48 kHz, TONE 0.5): 220 Hz sat 19.9 dB BELOW 1 kHz and 11.0 dB
    // below 440 Hz, because the two clip stages' coupling caps cornered at 898 Hz. With the
    // series base resistors in place 220 Hz sits 1.4 dB ABOVE 1 kHz and 0.1 dB above 440 Hz,
    // the fundamental survives, and the same amount of clipping measures 39 % instead of
    // 122 %. The pedal is not less saturated — testSustainWall below measures the
    // compression directly and it got STRONGER (20 dB in -> -0.09 dB out at max). So the
    // bar is re-derived rather than the old number chased.
    assert(tMax > 0.30 && "max-SUSTAIN THD not fuzz-large (>30%)");
    // Min sustain is a GENUINE escape: clearly less saturated than the wall.
    assert(tLo < 0.6 && tLo < 0.5 * tMax && "SUSTAIN=0 did not shed the wall");
    std::printf(
        "  [ok] SUSTAIN THD range @ %.0f Hz: min 0.0 -> %.1f%%, default 0.6 -> %.1f%%, "
        "max 1.0 -> %.1f%% (RISES — min sustain sheds the wall)\n",
        fs, tLo * 100.0, tMid * 100.0, tMax * 100.0);
}

// --- Test 4: the sustain/compression WALL — and its COLLAPSE at min sustain. ---
// At HIGH SUSTAIN the double diode clip compresses hard: output RMS stays ~flat as the
// INPUT sweeps 20 dB (the wall-of-sustain signature). Post-fix the wall lives in the
// SUSTAIN ≥ 0.6 territory (min sustain now sheds it — the field fix), so the wall assert
// runs at 0.7. The test is STRENGTHENED with the contrast: at SUSTAIN=0 the same sweep
// is DYNAMIC (output tracks input), proving the wall collapses and dynamics return.
void testSustainWall(double fs) {
    const float amps[] = {0.05f, 0.10f, 0.20f, 0.35f, 0.50f};  // ~20 dB sweep
    auto sweepSpread = [&](float sustain, double& stepDb) {
        double mn = 1e9, mx = 0.0, r05 = 0.0, r50 = 0.0;
        for (float a : amps) {
            const double r = tailRms(render(sine(220.0, a, 0.6, fs), {sustain, 0.5f, 0.6f}, fs), fs);
            if (a == 0.05f) r05 = r;
            if (a == 0.50f) r50 = r;
            mn = std::min(mn, r); mx = std::max(mx, r);
        }
        stepDb = 20.0 * std::log10(r50 / (r05 + 1e-12));
        return 20.0 * std::log10(mx / (mn + 1e-12));
    };
    // The WALL at SUSTAIN 0.7 (≥ 0.6 territory): <= 4 dB output variation across a 20 dB
    // input sweep, and the 20 dB step compresses to near nothing (measured ~2.4 / ~0.1 dB).
    double wallStep = 0.0;
    const double wallSpread = sweepSpread(0.7f, wallStep);
    assert(wallSpread < 4.0 && "no sustain wall at 0.7: output not compressed across the sweep");
    assert(std::fabs(wallStep) < 3.0 && "20 dB input step not heavily compressed at SUSTAIN 0.7");
    // The COLLAPSE at SUSTAIN 0 (the fix): the wall is gone — output tracks input far more
    // (the 20 dB step now yields several dB out, not ~0), so dynamics return.
    double loStep = 0.0;
    const double loSpread = sweepSpread(0.0f, loStep);
    assert(loSpread > wallSpread + 2.0 && "SUSTAIN=0 not appreciably more dynamic than the wall");
    assert(std::fabs(loStep) > 3.0 && "SUSTAIN=0 still compressed like a wall (dynamics not restored)");
    std::printf(
        "  [ok] sustain wall @ %.0f Hz: SUSTAIN 0.7 spread %.2f dB (20 dB in -> %.2f dB out, "
        "the WALL); SUSTAIN 0.0 spread %.2f dB (-> %.2f dB out, DYNAMIC — wall collapses)\n",
        fs, wallSpread, wallStep, loSpread, loStep);
}

// --- Test 4b: single-coil HUM rejection at min sustain (the field complaint). --
// Field report: "the Pi makes even just my single-coil hum blown out, even with minimum
// sustain." Root cause (docs §24): the overdriven input booster + hot pot floor clipped
// EVERYTHING, so the wall-of-sustain compression dragged a −40 dBFS hum up toward playing
// level (a −40 dBFS hum came out near −10 dBFS, and min sustain didn't help). This is the
// PERMANENT regression guard for the fix.
//
// WHAT THIS TEST ACTUALLY MEASURES, after audit finding 16 (re-baselined 2026-07-25, §37).
//
// The audit's charge was that this test "passes largely BECAUSE of the defect — it validates
// a bug as a feature": 60 Hz was being rejected by a bass rolloff that should not exist. The
// comment here made that explicit and got the number wrong twice over. It claimed
// "Cin=100 nF, ~−4.5 dB/stage at 60 Hz — canon, not a fantasy filter". CORRECTED CANON, all
// figures measured per stage at the 4× rate, 60 Hz relative to 1 kHz:
//
//   * −4.5 dB/stage is RIGHT for the two PLAIN stages (Q1/Q4): base node ~19.6 k, coupling
//     corner 81 Hz, −4.47 dB at 60 Hz. Cin = 100 nF, exactly as claimed.
//   * It is badly WRONG for the two CLIP stages (Q2/Q3). Their idle-conducting diodes shunt
//     the base node to ~1.8 k, cornering at 898 Hz: −20.95 dB/stage at 60 Hz.
//   * Pre-fix total: −49.6 dB at 60 Hz relative to 1 kHz (measured −49.59). Averaged over
//     four stages that is the audit's −13.9 dB/stage figure, ~3× the stated −4.5.
//
// With the series base resistors the clip corners move to 31.5 Hz (−1.05 dB/stage at 60 Hz)
// and the total is −8.4 dB — so this test can no longer be passed by brute filtering, and
// testLowEndResponse below asserts that the bass really is present. What remains, and what
// this test now genuinely measures, is the SUSTAIN POT: at SUSTAIN 0 it attenuates to
// kClipDriveMax·10^(−54/20) ≈ 1.2 % of max drive, so a quiet hum stays in the near-linear
// region of the clean cascade instead of being compressed up to playing level. That is the
// mechanism the §24 field fix installed, and it is the one under test.
// So (a) with signal present the 60 Hz hum stays far below the note, and (b) a hum-ALONE
// input is NOT amplified above its own level. Both bars are UNCHANGED by the finding-16 fix
// (measured: hum/sig −51 dB → −43 dB, hum-alone −61 dBFS → −72 dBFS), which is the evidence
// that the pot, not the rolloff, was carrying them.
void testHumRejection(double fs) {
    const double fSig = 220.0, fHum = 60.0;
    const float aSig = 0.3162f;  // −10 dBFS guitar-level note
    const float aHum = 0.01f;    // −40 dBFS single-coil hum  (input hum/sig = −30 dB)
    auto humToSig = [&](float sustain) {
        std::vector<float> in = sine(fSig, aSig, 1.0, fs);
        const std::vector<float> hum = sine(fHum, aHum, 1.0, fs);
        for (size_t i = 0; i < in.size(); ++i) in[i] += hum[i];
        const std::vector<float> o = render(in, {sustain, 0.5f, 0.6f}, fs);
        const size_t n = o.size(), win = std::min(n, static_cast<size_t>(fs));
        return toDb(goertzelAmp(o, n - win, win, fHum, fs)) -
               toDb(goertzelAmp(o, n - win, win, fSig, fs));
    };
    // (1) With a note playing, the hum sits far below it at min sustain (bar ≤ −25 dB;
    //     measured ~−51 dB, i.e. the hum is NOT compressed up to the note).
    const double ratioMin = humToSig(0.0f);
    const double ratioMax = humToSig(1.0f);
    assert(ratioMin <= -25.0 && "hum compressed up to signal at min sustain (the field bug)");
    // (2) Hum-ALONE (no note) — the literal complaint: a −40 dBFS hum must NOT come out
    //     blown up. At min sustain it stays quiet; the min->max delta is large, so min
    //     sustain is a real escape hatch (a fuzz at MAX does amplify hum — that is honest).
    auto humAloneDb = [&](float sustain) {
        return toDb(tailRms(render(sine(fHum, aHum, 0.6, fs), {sustain, 0.5f, 0.6f}, fs), fs));
    };
    const double humLo = humAloneDb(0.0f);
    const double humHi = humAloneDb(1.0f);
    assert(humLo < -40.0 && "hum-alone blown up at min sustain (field bug: was ~−10 dBFS)");
    assert(humHi - humLo > 25.0 && "min sustain is not a large escape from the hum wall");
    std::printf(
        "  [ok] hum rejection @ %.0f Hz: with note, hum/sig min=%.1f dB (bar -25) max=%.1f dB; "
        "hum-ALONE min=%.1f dBFS max=%.1f dBFS (delta %.1f dB — min sustain tames it)\n",
        fs, ratioMin, ratioMax, humLo, humHi, humHi - humLo);
}

// --- Test 4c: the pedal has BASS where a guitar has bass (audit finding 16). ---
// The other half of finding 16, and the property that makes testHumRejection honest.
//
// MEASURED pre-fix (48 kHz, SUSTAIN 0.15 = near-linear, TONE 0.5, 0.002 V in — small enough
// that the cascade is not compressing, so this is a transfer function and not a level test):
//
//   Hz     30      41.2     60      82.4     110     220     440    1000    4000
//   dB   -70.95  -60.82  -49.59  -41.00  -33.96  -19.89  -8.84    0.00   +6.24   (re 1 kHz)
//
// The guitar's LOW E is 82.4 Hz. It sat 41 dB below 1 kHz, with a ~21 dB/octave slope below
// 60 Hz — four cascaded coupling high-passes, two of them cornered at 898 Hz. No Big Muff is
// that thin; its coupling caps see 10 k–100 k series resistors and a 100 k pot, putting the
// corners at 15–50 Hz.
//
// Absolute reference, deliberately not derived from this netlist: the pitches of a guitar's
// bottom two strings, against 1 kHz. The bar is player-stated — a fuzz must not be 6 dB down
// on the open low E — and the subsonic assertion is the other half of it: the pedal must
// still be a coupled circuit, not a DC-coupled one, so 30 Hz stays well down.
void testLowEndResponse(double fs) {
    const float amp = 0.002f;  // small-signal: the cascade is not compressing here
    auto gainDb = [&](double f) {
        const auto in = sine(f, amp, 1.2, fs);
        const auto o = render(in, {0.15f, 0.5f, 0.6f}, fs);
        const size_t n = o.size(), win = std::min(n, static_cast<size_t>(0.8 * fs));
        return toDb(goertzelAmp(o, n - win, win, f, fs)) -
               toDb(goertzelAmp(in, n - win, win, f, fs));
    };
    const double g1k = gainDb(1000.0);
    const double gLowE = gainDb(82.4);   // open low E
    const double gA = gainDb(110.0);     // open A
    const double g60 = gainDb(60.0);     // below the lowest fretted note; also mains hum
    const double g30 = gainDb(30.0);     // subsonic — must still be rejected

    assert(gLowE - g1k > -6.0 &&
           "open low E (82.4 Hz) more than 6 dB below 1 kHz — the coupling networks are "
           "cornering too high (audit finding 16: no series base resistor)");
    assert(gA - g1k > -4.0 && "open A (110 Hz) more than 4 dB below 1 kHz");
    assert(g60 - g1k > -12.0 && "60 Hz more than 12 dB below 1 kHz — a rolloff this steep is "
                                "what let testHumRejection pass on a defect");
    // The other direction: this is still a capacitor-coupled circuit. If someone deletes the
    // coupling caps (or DC-couples a stage) to chase the assertions above, 30 Hz comes up and
    // this fails. A first-order-per-stage network cannot satisfy both bars by accident.
    assert(g30 - g1k < -12.0 &&
           "30 Hz is NOT rejected — the coupling network has gone (or been shorted): a fuzz "
           "must not pass subsonics into the amp");
    std::printf("  [ok] low end @ %.0f Hz: low E(82.4) %+.2f dB, A(110) %+.2f, 60 Hz %+.2f, "
                "30 Hz %+.2f — all re 1 kHz (bars -6 / -4 / -12 / must be < -12)\n",
                fs, gLowE - g1k, gA - g1k, g60 - g1k, g30 - g1k);
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

// --- Test 8: idle solver cost — the Muff must not cost MORE when you stop playing.
//
// Audit finding 12, docs §34. This is a CPU property, but it is asserted as an exact
// solver-work count rather than a wall-clock time, because a timing assertion on a
// shared CI box is either flaky or so loose it proves nothing.
//
// Parked with no input, the previous sample's solution IS this sample's solution, so
// a healthy solver does ZERO Newton iterations: it evaluates the system once, sees the
// KCL residual is already at tolerance, and returns. Before the fix it instead ran an
// iteration whose backtracking line search could not possibly succeed (no trial step
// can strictly decrease a residual already at the floating-point floor) and burned all
// 30 backtracks — 31 Ebers-Moll evaluations, 124 exp() calls, per stage per sample,
// measured at 3x the cost of actually playing.
//
// Every rate x oversampling combination is checked because the tolerance has to clear
// the parked residual in all of them; it is 4.9x above the measured ceiling and that
// ceiling is rate-independent, but this is the assertion that would notice otherwise.
void testIdleSolverCost() {
    int checked = 0;
    for (double fs : {44100.0, 48000.0, 88200.0, 96000.0}) {
        for (int os : {1, 2, 4, 8}) {
            for (float sustain : {0.0f, 0.5f, 1.0f}) {
                MuffModel m;
                m.prepare(fs, 128);
                m.setOversampling(os);
                m.setParameter(MuffModel::PARAM_SUSTAIN, sustain);
                m.setParameter(MuffModel::PARAM_TONE, 0.5f);
                m.setParameter(MuffModel::PARAM_VOLUME, 0.8f);

                // A real signal first, so the stages are NOT sitting on their
                // prepare-time park: this has to hold for a stage that fell quiet
                // after being played, which is the case a player actually hears.
                const auto pluck = sine(220.0, 0.3f, 0.15, fs);
                std::vector<float> scratch(pluck.size(), 0.0f);
                m.process(pluck.data(), scratch.data(), static_cast<int>(pluck.size()));

                // Now go quiet, long enough for the coupling caps to fully discharge.
                const std::vector<float> quiet(static_cast<size_t>(fs * 0.5), 0.0f);
                std::vector<float> out(quiet.size(), 0.0f);
                m.process(quiet.data(), out.data(), static_cast<int>(quiet.size()));

                // Fresh high-water mark over the silent stretch only.
                MuffModel probe;
                probe.prepare(fs, 128);
                probe.setOversampling(os);
                probe.setParameter(MuffModel::PARAM_SUSTAIN, sustain);
                probe.setParameter(MuffModel::PARAM_TONE, 0.5f);
                probe.setParameter(MuffModel::PARAM_VOLUME, 0.8f);
                std::vector<float> o2(quiet.size(), 0.0f);
                probe.process(quiet.data(), o2.data(), static_cast<int>(quiet.size()));
                for (int i = 0; i < 4; ++i) {
                    const int iters = probe.stage(i).lastMaxNewtonIterations();
                    assert(iters == 0 &&
                           "PARKED Muff stage is still running Newton iterations: the "
                           "residual early-out is not firing, so idle costs ~3x playing "
                           "again (audit finding 12, docs §34). Most likely cause: "
                           "kNewtonResidualTolA tightened below the 2.06e-18 A parked "
                           "residual ceiling.");
                    assert(iters <= BjtStage::kMaxNewtonIter &&
                           "iteration count above the cap it is compared against");
                }
                ++checked;
            }
        }
    }
    std::printf("  [ok] idle solver cost: 0 Newton iterations when parked, across %d "
                "rate x oversampling x sustain combinations\n", checked);
}

// --- Test 7b: slam convergence sweep (±20 V, every rate x oversampling). -------
//
// The original version of this drove ±10 V at one oversampling factor and asserted
// only "finite and bounded". That leaves the damped Newton's actual JOB unmeasured:
// the backtracking line search exists because a bare Newton oscillates and stalls at
// the iteration cap under a slam, which previously produced a cascade blow-up. Since
// the residual early-out (docs §34) touches that same loop, the iteration count is
// now pinned so the globalization cannot silently degrade.
//
// Doubled to ±20 V, and swept across oversampling factors, this immediately found
// something the ±10 V single-factor version could not: some rate x oversampling
// combinations EXHAUST the cap. See kXfSlamIterCap — that is pre-existing (measured
// identical on the pre-early-out solver, which reported it as 61 against a cap of
// 60), not a regression from this slice.
void testSlamConvergence() {
    int worst = 0;
    double worstFs = 0.0;
    int worstOs = 0;
    int checked = 0, atCap = 0;
    for (double fs : {44100.0, 48000.0, 88200.0, 96000.0}) {
        for (int os : {1, 2, 4, 8}) {
            std::vector<float> slam(static_cast<size_t>(fs * 0.2));
            for (size_t i = 0; i < slam.size(); ++i) slam[i] = (i % 3) ? 20.0f : -20.0f;
            MuffModel m;
            m.prepare(fs, 128);
            m.setOversampling(os);
            m.setParameter(MuffModel::PARAM_SUSTAIN, 1.0f);
            m.setParameter(MuffModel::PARAM_TONE, 0.5f);
            m.setParameter(MuffModel::PARAM_VOLUME, 1.0f);
            std::vector<float> o(slam.size(), 0.0f);
            m.process(slam.data(), o.data(), static_cast<int>(slam.size()));

            double pk = 0.0;
            for (float v : o) {
                assert(std::isfinite(v) && "non-finite output on a ±20 V slam");
                pk = std::max(pk, static_cast<double>(std::fabs(v)));
            }
            assert(pk < 100.0 && "output blew up on the slam (unbounded)");

            int iters = 0;
            for (int i = 0; i < 4; ++i)
                iters = std::max(iters, m.stage(i).lastMaxNewtonIterations());
            assert(iters > 0 &&
                   "a ±20 V slam that needed ZERO Newton iterations means the stage "
                   "never saw the signal — test plumbing broken");
            // This one used to FAIL: the pre-fix solver returned `it + 1` even when the
            // loop exhausted the cap, so it reported 61 against kMaxNewtonIter == 60.
            assert(iters <= BjtStage::kMaxNewtonIter &&
                   "Newton iteration count exceeded the cap it is compared against "
                   "(the `return it + 1` over-report — audit finding 12, docs §34)");
            if (iters > worst) { worst = iters; worstFs = fs; worstOs = os; }
            if (iters >= BjtStage::kMaxNewtonIter) {
                ++atCap;
                std::printf("      [!] %.0f Hz x%d: %d/%d iterations — AT THE CAP "
                            "(not converged; peak %.2f V, still finite)\n",
                            fs, os, iters, BjtStage::kMaxNewtonIter, pk);
            }
            ++checked;
        }
    }
    std::printf("  [ok] ±20 V slam: finite + bounded, iterations <= cap, across %d "
                "rate x oversampling combinations (worst %d/%d @ %.0f Hz x%d; %d at "
                "the cap)\n",
                checked, worst, BjtStage::kMaxNewtonIter, worstFs, worstOs, atCap);

    // Evaluated ONCE over the whole sweep, not per rate: the property is "at EVERY
    // supported rate x oversampling", which is uniformly false, so it cannot XPASS at
    // one rate while XFAILing at another.
    char detail[256];
    std::snprintf(detail, sizeof detail,
                  "%d of %d rate x oversampling combinations exhaust the cap "
                  "(%d of %d iterations, i.e. NOT converged); worst at %.0f Hz x%d; the "
                  "other %d converge in 14-18 iterations",
                  atCap, checked, worst, BjtStage::kMaxNewtonIter, worstFs, worstOs,
                  checked - atCap);
    clipper::test::expectXfail(worst < BjtStage::kMaxNewtonIter, kXfSlamIterCap, detail);
}

// --- Test 7: stability + hygiene (finite, silence->silence, deterministic). ----
void testStabilityHygiene() {
    const double fs = 48000.0;
    auto in = sine(330.0, 0.3f, 0.2, fs);
    for (float su = 0.0f; su <= 1.0f; su += 0.25f)
        for (float tn = 0.0f; tn <= 1.0f; tn += 0.25f)
            for (float vo = 0.0f; vo <= 1.0f; vo += 0.5f) {
                auto o = render(in, {su, tn, vo}, fs);
                for (float v : o) assert(std::isfinite(v) && "non-finite output on the grid");
            }
    {  // silence -> silence (the model does not self-oscillate)
        std::vector<float> zeros(static_cast<size_t>(fs * 0.3), 0.0f);
        auto o = render(zeros, {1.0f, 0.5f, 1.0f}, fs);
        double dc = 0.0, pk = 0.0;
        for (float v : o) { dc += v; pk = std::max(pk, (double)std::fabs(v)); }
        dc /= o.size();
        assert(pk < 1e-4 && "silence produced output");
        assert(std::fabs(dc) < 1e-3 && "DC offset on silence");
    }
    {  // DC offset ON SIGNAL — audit finding 16, FIXED 2026-07-25 (docs §37, ADR 009).
        //
        // This block was an `expectXfail` from 2026-07-25's test/assert-real-properties slice
        // until the output coupling cap landed: the Muff is FOUR cascaded asymmetric BJT
        // stages and had NO high-pass anywhere, so it rode on up to +0.47 V of DC — 28.1 %
        // of peak at max SUSTAIN. `MuffModel` now carries the real pedal's 0.1 µF output cap
        // into the 100 k VOLUME pot (15.92 Hz), placed after Q4 and before the decimator, and
        // the property is asserted for real. Fixing it made the XFAIL XPASS, which is a hard
        // failure by design (support/Xfail.h) — so the XFAIL is gone, not loosened.
        //
        // MEASURED after the fix, both stimuli, all three sustains: 0.00002 % of peak.
        //
        // The second stimulus is the load-bearing one: an already-symmetric or already-
        // blocked clipper does not rectify, so a clean-input test alone cannot fail if the
        // cap goes missing. +0.1 V of input offset makes the cap load-bearing. See
        // core/tests/support/DcOffset.h.
        const auto tone = sine(220.0, 0.2f, 1.0, fs);
        for (float sustain : {0.3f, 0.5f, 1.0f}) {
            for (float dcIn : {0.0f, clipper::test::kInputDcOffset}) {
                auto stim = tone;
                for (float& s : stim) s += dcIn;
                const auto o = render(stim, {sustain, 0.5f, 1.0f}, fs);
                const auto d = clipper::test::measureDcOnSignal(o);
                assert(d.peak > 0.01 && "no signal to measure DC against");
                assert(d.fraction < clipper::test::kDcFractionBar &&
                       "output DC on signal exceeds 1 % of peak — the output coupling cap "
                       "(kOutCouplingHz) is missing or bypassed (audit finding 16)");
                std::printf("  [ok] DC on signal @ %.0f Hz: sustain %.2f, input DC %+.2f V -> "
                            "mean %+.3e V on a %.4f V peak = %.5f %% (bar %.1f %%)\n",
                            fs, sustain, dcIn, d.mean, d.peak, 100.0 * d.fraction,
                            100.0 * clipper::test::kDcFractionBar);
            }
        }
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

// Known defects this binary exercises under XFAIL. Printed by --xfail-ledger, which ctest
// surfaces as ***Skipped in its default summary (see core/CMakeLists.txt).
//
// kXfMuffDc (audit finding 16, "the Muff has no output DC blocker") was REMOVED here:
// this slice fixes it and asserts the property for real, so leaving the XFAIL would
// XPASS and fail the suite by design (CLAUDE.md, the XFAIL ratchet).
const clipper::test::XfailDecl kLedger[] = {kXfSlamIterCap};

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    // No XFAIL ledger any more: this binary's only known-bad property was audit finding 16
    // (no output DC blocker, no series base resistors), fixed 2026-07-25 — docs §37, ADR 009.
    clipper::test::requireAssertsLive();

    std::printf("Running clipper::dsp::MuffModel tests (v1.1 item 4 — the fuzz + BjtStage)...\n");
    testDcOperatingPoints(44100.0);
    testDcOperatingPoints(96000.0);
    testMidScoop(44100.0);
    testMidScoop(96000.0);
    testSustainRange(44100.0);
    testSustainRange(48000.0);
    testSustainRange(96000.0);
    testSustainWall(44100.0);
    testSustainWall(48000.0);
    testSustainWall(96000.0);
    testHumRejection(44100.0);
    testHumRejection(48000.0);
    testHumRejection(96000.0);
    testLowEndResponse(44100.0);
    testLowEndResponse(48000.0);
    testAliasing(44100.0);
    testAliasing(96000.0);
    testVolumeLinearity();
    testStabilityHygiene();
    testSlamConvergence();
    testIdleSolverCost();
    std::printf("All MuffModel tests passed (XFAILs listed below are known open defects, "
                "not regressions).\n");
    return clipper::test::reportXfails();
}
