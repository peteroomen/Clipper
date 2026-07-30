// Plain-assert tests for clipper::dsp::GoldModel (v1.1 item 6 — the GOLD
// "transparent" overdrive: parallel clean/dirt blend + germanium WDF clipper).
// No framework: int main + <cassert>. Frequency content via a hand-rolled Goertzel;
// aliasing via the shared measure/AliasMetric.h. Mirrors test_ts_model.cpp's shape.
//
// The headline measurements (see docs/DEVELOPMENT.md §27):
//   1. TRANSPARENCY at GAIN 0 — flat within a documented tolerance and honestly
//      clean (THD ~0), the trait the whole pedal is famous for.
//   2. THE CROSSFADE — one ganged knob moves the mix from all-clean to
//      clipped-through-a-clean-core: THD rises monotonically AND the clipped
//      share of the output rises, while the clean core never leaves.
//   3. GERMANIUM vs SILICON — the soft knee, measured: germanium starts bending
//      at a LOWER input and its THD-vs-level curve is GENTLER than the silicon
//      counterfactual driven identically.
//   4. TREBLE moves the spectrum in the documented (normal, clockwise-brightens)
//      direction, and its tilt matches the analytic +/-12 dB shelf.
//   5. Aliasing: shipped 4x at max GAIN below the M2 -60 dB bar; 1x far worse.
//   6. HEADROOM: the charge-pump rails never engage at guitar levels (the diodes
//      are the only clipper), and OUTPUT is linear.
//   7. Stability + hygiene at +/-10 V, all rates.

#include "clipper/dsp/GoldModel.h"

#include "measure/AliasMetric.h"

#include "support/AssertsLive.h"
#include "support/DcOffset.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::GoldModel;

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
double toDb(double amp) { return 20.0 * std::log10(amp + 1e-15); }

std::vector<float> sine(double f, float amp, double secs, double fs) {
    const int n = static_cast<int>(secs * fs);
    std::vector<float> s(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        s[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(kTwoPi * f * i / fs));
    return s;
}

double tailRms(const std::vector<float>& x, double fs) {
    const size_t skip = std::min(x.size(), static_cast<size_t>(0.2 * fs));
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = skip; i < x.size(); ++i) { acc += double(x[i]) * x[i]; ++n; }
    return n ? std::sqrt(acc / n) : 0.0;
}

struct Params {
    float gain, treble, output;
};

std::vector<float> render(const std::vector<float>& in, Params p, double fs, int os = 4,
                          int diode = GoldModel::DIODE_GERMANIUM, bool cleanBlend = true) {
    GoldModel m;
    m.prepare(fs, 128);
    m.setOversampling(os);
    m.setDiodeType(diode);
    m.setCleanBlendEnabled(cleanBlend);
    m.setParameter(GoldModel::PARAM_GAIN, p.gain);
    m.setParameter(GoldModel::PARAM_TREBLE, p.treble);
    m.setParameter(GoldModel::PARAM_OUTPUT, p.output);
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

// Small-signal gain (dB) at frequency f, measured through the model.
double gainDbAt(double f, Params p, double fs, float amp = 0.02f) {
    const auto in = sine(f, amp, 0.5, fs);
    const auto out = render(in, p, fs);
    const size_t n = out.size(), win = std::min(n, static_cast<size_t>(0.25 * fs));
    return toDb(goertzelAmp(out, n - win, win, f, fs)) -
           toDb(goertzelAmp(in, n - win, win, f, fs));
}

// Summed harmonic energy in [1 kHz, 10 kHz] relative to the fundamental, in dB —
// the same "what a tone knob does to a nonlinear device" metric the M11 harness
// uses (a plain centroid is fundamental-dominated and barely moves).
double hfHarmonicDb(const std::vector<float>& o, double f0, double fs) {
    const size_t n = o.size(), win = std::min(n, static_cast<size_t>(fs));
    const double f1 = goertzelAmp(o, n - win, win, f0, fs);
    double hh = 0.0;
    for (int k = 2; k * f0 < std::min(10000.0, fs * 0.45); ++k) {
        if (k * f0 < 1000.0) continue;
        const double a = goertzelAmp(o, n - win, win, k * f0, fs);
        hh += a * a;
    }
    return 10.0 * std::log10(hh / (f1 * f1 + 1e-30) + 1e-30);
}

// --- Test 1: TRANSPARENCY at GAIN 0 (the pedal's whole reputation). ----------
// With the ganged pot at minimum the clipped half is switched out entirely, so the
// box is its input buffer + summing amp + output pot: FLAT and CLEAN. OUTPUT at
// noon (0.5) is the calibration point — kSumGain 2.0 x 0.5 == exactly unity.
void testTransparency(double fs) {
    const Params p{0.0f, 0.5f, 0.5f};  // GAIN 0, TREBLE flat, OUTPUT noon
    const double probes[] = {60, 100, 220, 440, 1000, 2200, 5000, 8000, 10000};
    double worst = 0.0, worstF = 0.0;
    for (double f : probes) {
        if (f > fs * 0.45) continue;
        const double dev = std::fabs(gainDbAt(f, p, fs));
        if (dev > worst) { worst = dev; worstF = f; }
    }
    // Flat to a QUARTER dB across the guitar band — this is a buffer, not a voice.
    assert(worst < 0.25 && "GAIN=0 response is not flat within 0.25 dB (not transparent)");
    // And honestly clean: a hot 0.3 V note at GAIN 0 makes essentially no harmonics.
    const double t = thd(render(sine(220.0, 0.3f, 1.0, fs), p, fs), 220.0, fs);
    assert(t < 0.005 && "GAIN=0 is not clean (THD >= 0.5%)");
    // Unity at OUTPUT noon (the documented calibration point).
    const double unityDb = gainDbAt(1000.0, p, fs);
    assert(std::fabs(unityDb) < 0.1 && "OUTPUT at noon is not unity gain");
    std::printf(
        "  [ok] transparency @ %.0f Hz: GAIN=0 flat within %.3f dB (worst @ %.0f Hz), "
        "THD %.4f%%, unity at OUTPUT noon %.3f dB\n",
        fs, worst, worstF, t * 100.0, unityDb);
}

// --- Test 2: THE CROSSFADE — one ganged knob, two paths. ---------------------
// (a) THD rises monotonically with GAIN (from literally zero);
// (b) the CLIPPED share of the output rises with GAIN — measured by rendering the
//     same setting with the clean half forced OFF (the counterfactual hook) and
//     comparing RMS;
// (c) the clean core NEVER leaves: even at max GAIN the clean-only render is a
//     substantial part of the output (the reason notes stay defined).
void testCrossfade(double fs) {
    const double f0 = 220.0;
    const auto in = sine(f0, 0.2f, 1.0, fs);
    const float gains[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    double prevThd = -1.0, prevShare = -1.0;
    double th[5], share[5];
    for (int i = 0; i < 5; ++i) {
        const Params p{gains[i], 0.5f, 0.7f};
        const auto full = render(in, p, fs);
        const auto dirtOnly = render(in, p, fs, 4, GoldModel::DIODE_GERMANIUM, false);
        th[i] = thd(full, f0, fs);
        share[i] = tailRms(dirtOnly, fs) / (tailRms(full, fs) + 1e-12);
        assert(th[i] >= prevThd - 1e-9 && "THD not non-decreasing across the GAIN sweep");
        assert(share[i] >= prevShare - 1e-9 && "clipped share not rising with GAIN");
        prevThd = th[i];
        prevShare = share[i];
    }
    assert(th[0] < 1e-4 && "GAIN 0 is not exactly clean (the clipped half is not out)");
    assert(th[4] > 0.10 && "max GAIN does not saturate (>10% THD)");
    assert(share[0] < 1e-4 && "clipped path audible at GAIN 0");
    assert(share[4] > 0.5 && "clipped path does not dominate at max GAIN");
    // §50: the clean core doesn't just survive — per the real gang-2 divider it
    // stays FLAT (the "clean fades out" folklore is only relative to the dirt).
    assert(std::fabs(GoldModel::cleanBlendAt(1.0) - 1.0) < 1e-9 &&
           GoldModel::cleanBlendAt(0.3) == GoldModel::cleanBlendAt(0.9) &&
           "clean blend law drifted from the §50 flat clean feed");
    // §50: the schematic gang-1 law, asserted at both ends (the perturbation that
    // restores the pre-§50 linear 1 + 66.7g law fails BOTH):
    assert(std::fabs(GoldModel::driveGainAt(0.0) - 4.6068) < 1e-3 &&
           std::fabs(GoldModel::driveGainAt(1.0) - 25.8235) < 1e-3 &&
           "drive gain law drifted from A = 1 + 422k/((1-g)*100k + 17k)");
    std::printf(
        "  [ok] ganged crossfade @ %.0f Hz: THD %.2f%% -> %.2f%% -> %.2f%% -> %.2f%% -> "
        "%.2f%%; clipped share %.2f -> %.2f -> %.2f -> %.2f -> %.2f (clean feed flat, "
        "A 4.61x -> 25.82x)\n",
        fs, th[0] * 100, th[1] * 100, th[2] * 100, th[3] * 100, th[4] * 100, share[0],
        share[1], share[2], share[3], share[4]);
}

// --- Test 3: the GERMANIUM soft knee, distinguished from silicon. ------------
// Identical circuit, identical drive; ONLY the diode parameters change. Two
// measurable consequences of the germanium pair's lower, softer knee:
//   (a) EARLIER onset — at a small input the germanium is already generating
//       harmonics the silicon pair has barely started on;
//   (b) GENTLER slope — across a 20 dB input sweep the germanium's THD grows by a
//       SMALLER factor (it bent early and keeps bending gradually), while the
//       silicon curve stays near-clean and then takes off.
void testGermaniumKnee(double fs) {
    const double f0 = 220.0;
    // Probed in the region where the knee IS the story: a 20 dB input sweep whose
    // diode-node drive straddles the germanium knee. §50 RE-DERIVED PROBE: the
    // drive path now attenuates before the diodes (kDrivePreScale + the 600 Hz
    // corner ≈ 0.22x at 220 Hz), so the old probe (A = 7.7, 0.01–0.1 V) no longer
    // reached the knee at all. At max gain (A = 25.82) the node sees
    // 25.82·0.22·(0.025..0.25 V) ≈ 0.14..1.4 V — onset-to-deep across the sweep,
    // which is the straddle the two properties below need.
    const Params base{1.0f, 0.5f, 0.7f};
    const float lo = 0.025f, hi = 0.25f;  // 20 dB input sweep
    auto thdAt = [&](float amp, int diode) {
        return thd(render(sine(f0, amp, 1.0, fs), base, fs, 4, diode), f0, fs);
    };
    const double geLo = thdAt(lo, GoldModel::DIODE_GERMANIUM);
    const double geHi = thdAt(hi, GoldModel::DIODE_GERMANIUM);
    const double siLo = thdAt(lo, GoldModel::DIODE_SILICON);
    const double siHi = thdAt(hi, GoldModel::DIODE_SILICON);
    // Onset: at the quiet end the germanium is already making measurable harmonics
    // while the silicon pair is still essentially linear.
    //
    // RE-BASELINED 2026-07-25 (audit finding 15, docs §36). The silicon
    // counterfactual had its ideality factor dropped to 1.0, which pulled its knee
    // DOWN toward the germanium's and made this test's job easier than it should
    // have been: the onset ratio measured ~26x and the slope ratio ~0.032. With the
    // real 1N4148 device (n = 1.752) the two diodes are properly far apart and both
    // ratios improve by an order of magnitude — measured onset **189x** and slope
    // ratio **0.0063**.
    //
    // Both bounds below are therefore TIGHTER than they were (5x -> 20x, 0.25x ->
    // 0.05x): as assertions they are strictly harder to satisfy. What widened is the
    // MARGIN between bound and measurement (5.2x of headroom -> 9.4x), which is
    // deliberate — these are the "these are clearly different devices" floor, not a
    // fit to the measurement. Neither assertion was pinning the bug; both were genuine
    // properties that the bug made harder to satisfy. Do not read this as loosening a
    // bound to go green (CLAUDE.md forbids that, and rightly).
    assert(geLo > siLo * 20.0 && "germanium does not bend earlier than silicon");
    // Slope: across the same 20 dB the germanium THD grows by a far smaller factor
    // (measured ~23x vs silicon's ~3672x) — that is the soft knee, as a number.
    const double geSlope = geHi / (geLo + 1e-12);
    const double siSlope = siHi / (siLo + 1e-12);
    assert(geSlope < 0.05 * siSlope &&
           "germanium THD-vs-level slope is not markedly gentler than silicon");
    std::printf(
        "  [ok] germanium knee @ %.0f Hz: Ge THD %.2f%% -> %.2f%% (x%.2f over 20 dB), "
        "Si %.4f%% -> %.2f%% (x%.2f) — Ge bends EARLIER (x%.0f) and GENTLER (x%.4f)\n",
        fs, geLo * 100, geHi * 100, geSlope, siLo * 100, siHi * 100, siSlope,
        geLo / (siLo + 1e-12), geSlope / siSlope);
}

// --- Test 3b: the germanium-vs-silicon LEVEL contrast, absolutely. -----------
// The knee test above measures the SHAPE difference (onset + slope) and passes on
// ratios. It passed at ~26x onset even while the two diodes' clipping ceilings sat
// only ~1 dB apart, because a dropped ideality factor moves a knee without changing
// the shape of the comparison much (audit finding 15). So it never noticed that the
// silicon "counterfactual" had stopped being silicon.
//
// This asserts the thing the A/B is FOR, against an external reference: a 1N34A
// point-contact germanium pair and a 1N4148 silicon pair clip roughly a factor of
// two in voltage apart — the datasheet forward drops are ~0.3 V and ~0.65 V, i.e.
// ~6 dB. The clean half is switched OUT so this reads the clipper alone, and the
// measurement is taken well past both knees so it reads the CEILINGS, not the toe.
// Measured 5.88-6.11 dB across the drive range; it was 0.96-1.56 dB before.
void testDiodeLevelContrast(double fs) {
    const double f0 = 220.0;
    std::printf("  [--] Ge-vs-Si ceiling contrast @ %.0f Hz (clean half OUT):\n", fs);
    // §50 probe re-derivation: the drive path now attenuates (~0.22x at 220 Hz) and
    // A tops at 25.8x, so the old rows (gain 0.10 at 0.3 V) never reached EITHER
    // ceiling — the comparison read the linear path, not the clippers. The property
    // is the CEILINGS ("well past both knees" above), so the probe must get there:
    // 0.6 V at gain >= 0.35 puts the node at 0.8-3.4 V, past both.
    for (float g : {0.35f, 0.60f, 1.00f}) {
        const Params p{g, 0.5f, 0.7f};
        const auto in = sine(f0, 0.6f, 0.6, fs);
        const double ge = tailRms(render(in, p, fs, 4, GoldModel::DIODE_GERMANIUM, false), fs);
        const double si = tailRms(render(in, p, fs, 4, GoldModel::DIODE_SILICON, false), fs);
        assert(ge > 1e-4 && si > 1e-4 && "a diode option produced no output");
        const double contrast = toDb(si) - toDb(ge);
        // A real 1N34A-vs-1N4148 pair is ~6-7 dB apart. Bracket generously but
        // nowhere near the ~1 dB the dropped ideality factor produced.
        assert(contrast > 4.0 &&
               "silicon barely clips above germanium — is the 1N4148 ideality factor "
               "missing again? (audit finding 15, docs §36)");
        assert(contrast < 9.0 && "silicon clips implausibly far above germanium");
        std::printf("       gain %.2f: Ge rms %.4f, Si rms %.4f -> %+.2f dB "
                    "(4.0-9.0 dB band)\n", g, ge, si, contrast);
    }
}

// --- Test 4: the TREBLE control (normal sense: clockwise BRIGHTENS). ---------
void testTreble(double fs) {
    // (a) analytic tilt: +/-12 dB on the HF half about the ~1 kHz pivot.
    const double atHiBright = gainDbAt(6000.0, {0.0f, 1.0f, 0.5f}, fs);
    const double atHiDark = gainDbAt(6000.0, {0.0f, 0.0f, 0.5f}, fs);
    // Measured 44.1 k: +11.43 / -9.42 dB (the first-order tilt is asymptotic, so the
    // cut side reaches its -12 dB shelf a little higher up than the boost side).
    assert(atHiBright > 9.0 && atHiBright < 13.0 && "treble max is not ~+12 dB at 6 kHz");
    assert(atHiDark < -8.5 && atHiDark > -13.0 && "treble min is not ~-12 dB at 6 kHz");
    // Pivot region barely moves (it is a tilt, not a volume control).
    const double atLoBright = gainDbAt(120.0, {0.0f, 1.0f, 0.5f}, fs);
    assert(std::fabs(atLoBright) < 1.0 && "treble tilt is moving the low end");
    // (b) on the harmonics the pedal itself makes (the M11 metric).
    const auto drive = sine(220.0, 0.15f, 1.0, fs);
    const double hfDark = hfHarmonicDb(render(drive, {1.0f, 0.0f, 0.7f}, fs), 220.0, fs);
    const double hfBright = hfHarmonicDb(render(drive, {1.0f, 1.0f, 0.7f}, fs), 220.0, fs);
    assert(hfBright > hfDark + 6.0 &&
           "TREBLE extremes do not move the high-band harmonic energy >= 6 dB");
    std::printf(
        "  [ok] treble @ %.0f Hz: 6 kHz %.2f dB (dark) -> %+.2f dB (bright), 120 Hz %+.2f dB; "
        "HF harmonics %.1f -> %.1f dB\n",
        fs, atHiDark, atHiBright, atLoBright, hfDark, hfBright);
}

// --- Test 5: aliasing. Shipped 4x at max GAIN below the M2 -60 dB bar. -------
void testAliasing(double fs) {
    using clipper::measure::measureAliasing;
    const double f0 = 4186.0;  // C8
    const auto in = sine(f0, 0.2f, 1.0, fs);
    const Params hot{1.0f, 1.0f, 0.9f};  // max gain, brightest treble
    const double w4 = measureAliasing(render(in, hot, fs, 4), fs, f0).worstAliasDb;
    const double w1 = measureAliasing(render(in, hot, fs, 1), fs, f0).worstAliasDb;
    const double w8 = measureAliasing(render(in, hot, fs, 8), fs, f0).worstAliasDb;
    assert(w4 < -60.0 && "4x worst-alias at max GAIN not >= 60 dB below fundamental");
    assert(w4 < w1 - 20.0 && "4x did not clearly improve on 1x");
    std::printf("  [ok] aliasing @ %.0f Hz: 1x %.1f dB, 4x %.1f dB (bar -60), 8x %.1f dB\n",
                fs, w1, w4, w8);
}

// --- Test 6: HEADROOM (the charge pump) + OUTPUT linearity. ------------------
// The +/-9 V rails exist so the germanium pair is the ONLY clipper: at a very hot
// 1.0 V input, wide open, the summing node must stay well inside the rails.
void testHeadroomAndOutput(double fs) {
    const auto hotIn = sine(220.0, 1.0f, 0.5, fs);
    const auto o = render(hotIn, {1.0f, 0.5f, 1.0f}, fs);
    double pk = 0.0;
    for (float v : o) pk = std::max(pk, static_cast<double>(std::fabs(v)));
    assert(pk < 3.0 && "output peak approaches the charge-pump rails (they should never clip)");
    // A 1 V input at GAIN 0 is still CLEAN — the headroom statement, measured.
    const double tClean = thd(render(hotIn, {0.0f, 0.5f, 0.5f}, fs), 220.0, fs);
    assert(tClean < 0.005 && "the clean path distorts at 1 V (no headroom)");
    // OUTPUT is a linear pot (house convention).
    const auto in = sine(220.0, 0.2f, 0.5, fs);
    auto rmsAt = [&](float v) { return tailRms(render(in, {0.6f, 0.5f, v}, fs), fs); };
    const double r25 = rmsAt(0.25f), r50 = rmsAt(0.50f), r100 = rmsAt(1.0f);
    assert(std::fabs(r50 / r25 - 2.0) < 0.06 && "OUTPUT not linear 0.25 -> 0.5");
    assert(std::fabs(r100 / r50 - 2.0) < 0.06 && "OUTPUT not linear 0.5 -> 1.0");
    std::printf(
        "  [ok] headroom @ %.0f Hz: 1 V in wide open -> peak %.3f V (rails 8.6 V never "
        "touched), clean-path THD %.4f%%; OUTPUT r50/r25=%.3f r100/r50=%.3f\n",
        fs, pk, tClean * 100.0, r50 / r25, r100 / r50);
}

// --- Test 7: analytic laws (the ganged pot, measured against the formulas). ---
void testAnalyticLaws(double fs) {
    // Drive-amp gain law (§50, the schematic): A = 1 + 422k/((1-g)*100k + 17k).
    assert(std::fabs(GoldModel::driveGainAt(0.0) - (1.0 + 422.0e3 / 117.0e3)) < 1e-6);
    assert(std::fabs(GoldModel::driveGainAt(1.0) - (1.0 + 422.0e3 / 17.0e3)) < 1e-6);
    // Measured small-signal gain at GAIN 0 == the summing amp alone (x2 = +6 dB)
    // at OUTPUT wide open; at OUTPUT noon it is exactly unity (tested above).
    const double g0 = gainDbAt(1000.0, {0.0f, 0.5f, 1.0f}, fs);
    assert(std::fabs(g0 - 6.02) < 0.15 && "GAIN 0 / OUTPUT 1 is not the +6 dB summing amp");
    // The drive path's pre-clip high-pass keeps the lows out of the clipper: with
    // the clean half forced off, 60 Hz must be far below 1 kHz.
    // Probed with a TINY input so the diodes stay linear and the measurement is the
    // FILTER, not the clipper (at 0.001 V x A(0.5)=34.3 the diode node sees ~34 mV,
    // far below the germanium knee).
    auto dirtOnlyDb = [&](double f) {
        const auto in = sine(f, 0.001f, 0.5, fs);
        const auto out = render(in, {0.5f, 0.5f, 0.7f}, fs, 4, GoldModel::DIODE_GERMANIUM,
                                false);
        const size_t n = out.size(), win = std::min(n, static_cast<size_t>(0.25 * fs));
        return toDb(goertzelAmp(out, n - win, win, f, fs)) -
               toDb(goertzelAmp(in, n - win, win, f, fs));
    };
    const double d60 = dirtOnlyDb(60.0), d1k = dirtOnlyDb(1000.0);
    assert(d1k - d60 > 4.0 && "the drive path's pre-clip high-pass is missing (lows get clipped)");
    std::printf(
        "  [ok] analytic laws @ %.0f Hz: A(0)=1.00 A(1)=%.2f; GAIN0/OUT1 %.2f dB (analytic "
        "6.02); drive-path 60 Hz %.1f dB vs 1 kHz %.1f dB (lows skip the clipper)\n",
        fs, GoldModel::driveGainAt(1.0), g0, d60, d1k);
}

// --- Test 8: stability + hygiene (finite, silence->silence, deterministic). ---
void testStabilityHygiene() {
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        std::vector<float> slam(static_cast<size_t>(fs * 0.2));
        for (size_t i = 0; i < slam.size(); ++i) slam[i] = (i % 3) ? 10.0f : -10.0f;
        const auto o = render(slam, {1.0f, 1.0f, 1.0f}, fs);
        double pk = 0.0;
        for (float v : o) {
            assert(std::isfinite(v) && "non-finite on +/-10 V slam");
            pk = std::max(pk, static_cast<double>(std::fabs(v)));
        }
        assert(pk < 100.0 && "output blew up on the slam (unbounded)");
        std::printf("  [ok] slam @ %.0f Hz: finite, bounded (peak %.2f V)\n", fs, pk);
    }
    const double fs = 48000.0;
    const auto in = sine(330.0, 0.3f, 0.2, fs);
    for (float g = 0.0f; g <= 1.0f; g += 0.25f)
        for (float t = 0.0f; t <= 1.0f; t += 0.25f)
            for (float v = 0.0f; v <= 1.0f; v += 0.5f) {
                const auto o = render(in, {g, t, v}, fs);
                for (float s : o) assert(std::isfinite(s) && "non-finite output on the grid");
            }
    {  // silence -> silence (the model does not self-oscillate)
        std::vector<float> zeros(static_cast<size_t>(fs * 0.3), 0.0f);
        const auto o = render(zeros, {1.0f, 1.0f, 1.0f}, fs);
        double dc = 0.0, pk = 0.0;
        for (float v : o) { dc += v; pk = std::max(pk, static_cast<double>(std::fabs(v))); }
        dc /= static_cast<double>(o.size());
        assert(pk < 1e-4 && "silence produced output");
        assert(std::fabs(dc) < 1e-3 && "DC offset on silence");
    }
    {  // DC offset ON SIGNAL — the property that matters (audit finding 16).
        //
        // 2026-07-25 (test/assert-real-properties): the silence check above was the only
        // DC assertion here and it is trivially true. GOLD is the most exposed of the four:
        // its germanium branch is DELIBERATELY asymmetric (that asymmetry IS the voice),
        // and it sums a clipped path with a clean path — so a DC step in the dirt leg would
        // ride straight through the blend. `kOutHpHz` (~8 Hz) is what stops it.
        //
        // TWO stimuli: the clean tone catches a rectifying clipper, and the +0.1 V input
        // offset makes the coupling cap itself load-bearing (with a clean input, deleting
        // the cap changes nothing — verified). See core/tests/support/DcOffset.h.
        const auto tone = sine(220.0, 0.2f, 1.0, fs);
        for (float gain : {0.5f, 1.0f}) {
            for (float dcIn : {0.0f, clipper::test::kInputDcOffset}) {
                auto stim = tone;
                for (float& s : stim) s += dcIn;
                const auto o = render(stim, {gain, 1.0f, 1.0f}, fs);
                const auto d = clipper::test::measureDcOnSignal(o);
                std::printf("  [ok] DC on SIGNAL (gain %.2f, input DC %+.2f V): mean %+.6f V, "
                            "peak %.4f V -> %.4f %% of peak (bar %.1f %%)\n",
                            gain, dcIn, d.mean, d.peak, 100.0 * d.fraction,
                            100.0 * clipper::test::kDcFractionBar);
                assert(d.peak > 0.01 && "no signal to measure DC against");
                assert(d.fraction < clipper::test::kDcFractionBar &&
                       "DC offset ON SIGNAL exceeds 1 % of peak — the output coupling cap is "
                       "missing or mistuned, or the clipper is rectifying");
            }
        }
    }
    {  // determinism
        const auto a = render(in, {0.6f, 0.4f, 0.8f}, fs);
        const auto b = render(in, {0.6f, 0.4f, 0.8f}, fs);
        assert(a.size() == b.size());
        assert(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0 &&
               "processing is not deterministic");
    }
    std::printf("  [ok] hygiene: finite grid, silence->silence, deterministic\n");
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();
    std::printf("Running clipper::dsp::GoldModel tests (v1.1 item 6 — the clean-blend "
                "germanium overdrive)...\n");
    testTransparency(44100.0);
    testTransparency(48000.0);
    testTransparency(96000.0);
    testCrossfade(44100.0);
    testCrossfade(48000.0);
    testCrossfade(96000.0);
    testGermaniumKnee(44100.0);
    testGermaniumKnee(96000.0);
    testDiodeLevelContrast(48000.0);
    testTreble(44100.0);
    testTreble(96000.0);
    testAliasing(44100.0);
    testAliasing(96000.0);
    testHeadroomAndOutput(44100.0);
    testHeadroomAndOutput(96000.0);
    testAnalyticLaws(48000.0);
    testStabilityHygiene();
    std::printf("All GoldModel tests passed.\n");
    return 0;
}
