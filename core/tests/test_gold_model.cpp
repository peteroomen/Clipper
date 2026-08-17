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

#include "clipper/dsp/OutputPotTaper.h"

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

// --- docs §54: THE TWO §52 XFAILs ARE GONE, AND THAT IS THE RATCHET WORKING ---------
// §52 left two known-bad properties here (`gold-summing-rails-engage` and its shadow
// `gold-summing-alias-at-treble-max`), both naming ONE fix: the diode node ran hot, so
// the schematic's correct summing weight was multiplying a node that was ~2.1x too
// loud and the tone stage's +/-8.6 V rail clamp engaged at max TREBLE. §54 fixed that
// node (the reference implementation's measured clipper fit, Is = 15 uA / Vt =
// 25.85 mV, behind the schematic's R13 = 1 k). BOTH XPASSED, so per the XFAIL ratchet
// they are deleted and asserted for real, in the same slice:
//   * the rail clamp: 8.624 V -> 1.274 V at max GAIN + max TREBLE (testHeadroom...)
//   * the alias floor at max TREBLE: -26.5 dB -> -92.9 dB at 4x/44.1 k, and it MOVES
//     with the oversampling factor again (1x -27.3 / 4x -92.9 / 8x -106.8), i.e. it
//     is measuring aliasing rather than a base-rate clip (testAliasing).
// This file has no XFAIL ledger. Do not add one back without a measured property.

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

// |H_pre(j2*pi*f)| for the GOLD's drive-path INPUT DIVIDER, written here from the
// reference implementation's component values (KlonCentaur/ChowCentaur/
// GainStageProcessors/PreAmpStage.{h,cpp}: C3 0.1 uF, C5 68 nF, C16 1 uF, R6 10 k,
// R7 1.5 k, `ResVs Vbias2` = R19 15 k, `ResVs Vbias` = the GAIN pot's other gang
// half = g * 100 k), NOT read back out of GoldModel.cpp. The topology:
//     Zs1 = (R6 || 1/sC5) + g*R_pot        Zs2 = R7 + (R19 || 1/sC16)
//     H_pre = Zp / (Zc3 + Zp),  Zp = Zs1 || Zs2
// Complex arithmetic done by hand so this file keeps its no-<complex> shape.
double refPreAmpMag(double f, double g) {
    const double w = kTwoPi * f;
    const double C3 = 0.1e-6, C5 = 68.0e-9, C16 = 1.0e-6;
    const double R6 = 10.0e3, R7 = 1.5e3, R19 = 15.0e3, Rg = g * 100.0e3;
    // (a + jb) / (c + jd)
    auto div = [](double a, double b, double c, double d, double& re, double& im) {
        const double q = c * c + d * d;
        re = (a * c + b * d) / q;
        im = (b * c - a * d) / q;
    };
    // Zs1 = R6/(1 + jw C5 R6) + Rg
    double z1r, z1i;
    div(R6, 0.0, 1.0, w * C5 * R6, z1r, z1i);
    z1r += Rg;
    // Zs2 = R7 + R19/(1 + jw C16 R19)
    double z2r, z2i;
    div(R19, 0.0, 1.0, w * C16 * R19, z2r, z2i);
    z2r += R7;
    // Zp = Zs1*Zs2 / (Zs1 + Zs2)
    const double nr = z1r * z2r - z1i * z2i, ni = z1r * z2i + z1i * z2r;
    double zpr, zpi;
    div(nr, ni, z1r + z2r, z1i + z2i, zpr, zpi);
    // H = Zp / (Zc3 + Zp), Zc3 = 1/(jw C3) = -j/(w C3)
    double hr, hi;
    div(zpr, zpi, zpr, zpi - 1.0 / (w * C3), hr, hi);
    return std::sqrt(hr * hr + hi * hi);
}

// --- Test 1: TRANSPARENCY at GAIN 0 (the pedal's whole reputation). ----------
// With the ganged pot at minimum the clipped half is switched out entirely, so the
// box is its input buffer + summing amp + output pot: FLAT and CLEAN. OUTPUT at
// noon (0.5) is the calibration point — kSumGain 2.0 x 0.5 == exactly unity.
// SPLIT INTO TWO BARS 2026-08-10 (docs §68). This used to measure ONE thing —
// |gain in dB| at each probe frequency, against 0.25 dB — which silently
// conflated FLATNESS (is this a buffer or a voice?) with LEVEL (is it unity?).
// They coincided only because the OUTPUT pot was an identity map and
// kSumGain 2.0 x 0.5 came out exactly 1.0. With the pot's real network
// (R25 560 R + 10 k + R28 100 k) noon delivers 0.489758 rather than 0.500, and
// the conflated bar measured 0.3181 dB and failed — on a 0.18 dB LEVEL move, with
// the flatness it was written to protect completely unchanged.
//
// So the two are now separate, and the flatness half is NOT loosened: it keeps
// the same 0.25 dB bound and measures 0.1384 dB, having lost whatever help the
// level term was giving it. Only the level half moved, and it moved by a DERIVED
// amount, not a fitted one.
void testTransparency(double fs) {
    const Params p{0.0f, 0.5f, 0.5f};  // GAIN 0, TREBLE flat, OUTPUT noon
    const double probes[] = {60, 100, 220, 440, 1000, 2200, 5000, 8000, 10000};
    const double ref = gainDbAt(1000.0, p, fs);
    double worst = 0.0, worstF = 0.0;
    for (double f : probes) {
        if (f > fs * 0.45) continue;
        // FLATNESS, relative to 1 kHz — the property this bar is actually for.
        // The OUTPUT pot is a frequency-flat scalar, so this number is untouched
        // by the taper slice and stays on its original 0.25 dB bound.
        const double dev = std::fabs(gainDbAt(f, p, fs) - ref);
        if (dev > worst) { worst = dev; worstF = f; }
    }
    // Flat to a QUARTER dB across the guitar band — this is a buffer, not a voice.
    assert(worst < 0.25 && "GAIN=0 response is not flat within 0.25 dB (not transparent)");
    // And honestly clean: a hot 0.3 V note at GAIN 0 makes essentially no harmonics.
    const double t = thd(render(sine(220.0, 0.3f, 1.0, fs), p, fs), 220.0, fs);
    assert(t < 0.005 && "GAIN=0 is not clean (THD >= 0.5%)");
    // LEVEL at OUTPUT noon, DERIVED from two sourced numbers rather than assumed:
    //   * the summing amp's gain kSumGain = 2.0 (docs §52 — R20·G_clean measures
    //     1.82-2.28 across the band, so the clean side was already right); and
    //   * the output network's delivered gain at noon, 0.489758 (docs §68.3, out
    //     of R25 = 560 R, the 10 k pot and R28 = 100 k).
    // => 20·log10(2.0 × 0.489758) = -0.1798 dB, and unity now lands at OUTPUT
    // 0.5105 rather than exactly noon. The bar is 0.25 dB; the measured -0.1798
    // leaves 0.07 dB of margin, which is RECORDED not snugged. It still fails if
    // kSumGain or the output network moves materially — the old 0.1 dB bound was
    // pinning an arithmetic coincidence of the linear map, not a circuit fact.
    const double unityDb = ref;
    assert(std::fabs(unityDb) < 0.25 &&
           "GAIN=0 / OUTPUT noon is not within 0.25 dB of unity — kSumGain or the "
           "output network has moved (docs §68.3 derives the -0.18 dB)");
    std::printf(
        "  [ok] transparency @ %.0f Hz: GAIN=0 FLAT within %.3f dB re 1 kHz "
        "(worst @ %.0f Hz, bar 0.25), THD %.4f%%; LEVEL at OUTPUT noon %.3f dB "
        "(derived -0.180 from kSumGain 2.0 x law(0.5) 0.489758, bar 0.25)\n",
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
    // §52: the DIRT SUMMING WEIGHT, from the schematic's summing network rather than a
    // fit. Every path reaches the summing amp's virtual ground through its own resistor
    // and shares one transimpedance R20 = 392 k, so the dirt path's weight is
    //   R20 / R16                 = 392k/47k = 8.3404  (R16 = the gain stage's own
    //                                                   summing resistor; C10 = 1 uF in
    //                                                   front of it is a 3.4 Hz HP, so
    //                                                   there is NO in-band post-diode
    //                                                   attenuation to divide out)
    // and this model carries the clean path's transimpedance in kSumGain = 2.0 (the
    // schematic's own R20*G_clean measures 1.96 at 110 Hz / 2.28 at 220 / 1.82 at 1 kHz
    // — an independent check that the 2.0 was already right), so
    //   kClipBlendWeight        = 8.3404 / 2.0 = 4.1702.
    // Two properties, both perturbation-proven against the pre-§52 0.65 (which fails
    // the first by 6.42x and is NOT a rounding difference):
    constexpr double kDerivedClipWeight = 392.0e3 / (47.0e3 * 2.0);
    assert(std::fabs(GoldModel::clipBlendAt(1.0) - kDerivedClipWeight) < 1e-9 &&
           "dirt summing weight drifted from the schematic's R20/(R16*kSumGain) "
           "= 392k/(47k*2) — do NOT re-fit it (docs §52)");
    // ... and it is FIXED across the travel above the contract fade (the knob changes
    // DRIVE, not mix), while the fade still holds clipBlend(0) = 0 exactly.
    assert(GoldModel::clipBlendAt(0.2) == GoldModel::clipBlendAt(1.0) &&
           GoldModel::clipBlendAt(0.6) == GoldModel::clipBlendAt(1.0) &&
           "dirt summing weight is not FIXED across the knob (the §50/§52 gang law)");
    assert(GoldModel::clipBlendAt(0.0) == 0.0 &&
           "clipBlend(0) != 0 — the bit-exact-clean GAIN-0 product contract is broken");
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
    // drive path now attenuates before the diodes (≈ 0.22x at 220 Hz), so the old
    // probe (A = 7.7, 0.01–0.1 V) no longer reached the knee at all. §56 replaced
    // that stand-in with the reference's own divider, which is 0.2027x at 220 Hz —
    // the same attenuation to within 0.8 dB, so the probe still lands.
    // §54 RE-CHECK (no change needed): the drive amp is now the real C7/C8 network, so
    // at 220 Hz and max gain it delivers 47.6x rather than the DC law's 25.82x, and
    // the germanium knee moved DOWN to 0.109 V. The sweep below puts the node's source
    // at 0.27..2.7 V, which solves to a node of 0.069..0.128 V — straddling the
    // 0.109 V knee from the toe to well past it, which is exactly the straddle the two
    // properties need. Left as it is.
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
    // ratio **0.0063**. (§52 re-baseline: the derived summing weight raises the dirt's
    // share of the output, so these THDs are absolutely larger — Ge 2.87 % -> 26.90 %,
    // Si 0.0180 % -> 23.72 % — while the two RATIOS the properties are about are
    // essentially unmoved: onset **160x**, slope ratio **0.0071**. Bounds unchanged;
    // the probe still straddles the knee, so no re-derivation was needed.)
    // (§54 re-baseline: the reference's germanium fit clips lower and softer, so the
    // Ge curve is flatter across the sweep — Ge 7.68 % -> 14.59 %, Si 0.0551 % ->
    // 18.53 %. Onset **139x** (bar 20x) and slope ratio **0.00565** (bar 0.05): both
    // ratios still clear their bounds by 7x and 8.8x. Bounds unchanged again.)
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
    // (measured ~9.4x vs silicon's ~1319x) — that is the soft knee, as a number.
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
// This asserts the thing the A/B is FOR, against external references: the two device
// models' own knees. The clean half is switched OUT so this reads the clipper alone,
// and the measurement is taken well past both knees so it reads the CEILINGS.
//
// §54 RE-DERIVED THIS PROBE'S BAND, and the reason is a change of reference, not a
// change of bound. Until 2026-07-31 this model's germanium was a datasheet-shaped
// guess (Is = 200 nA, n = 1.3 -> 0.286 V at 1 mA), and the band 4.0-9.0 dB came from
// the datasheet argument "1N34A ~0.3 V vs 1N4148 ~0.65 V, i.e. ~6 dB". §54 replaced
// the germanium with the reference implementation's FIT TO A REAL UNIT's clipper
// (Is = 15 uA, Vt = 25.85 mV, n = 1), whose knee is 0.1086 V at 1 mA. The silicon
// counterfactual is unchanged and still externally anchored — the 1N4148 SPICE card
// `IS=2.52n N=1.752` gives 1.752*25.85m*ln(1mA/2.52nA) = 0.5839 V at 1 mA, which the
// datasheet's ~0.62 V typical corroborates. So the two knees are now a factor of
// 5.376 apart == 14.61 dB, and that analytic figure — computed from two independently
// sourced device models, neither of them fitted here — is what the band brackets.
// Measured through the whole dirt path: 12.70-14.29 dB across 0.3-3.0 V of input at
// GAIN 0.35 and 1.00 (it was 5.84-6.12 dB with the old germanium, 0.96-1.56 dB when
// the SILICON ideality was also wrong).
//
// THE BAR STILL CATCHES THE BUG IT WAS BUILT FOR. Audit finding 15 (§36) was the
// 1N4148's ideality dropped to 1.0, which moves the silicon knee to 0.333 V and the
// analytic contrast to 9.73 dB; the perturbation measures 8.51-10.22 dB through the
// model and fails the 11.0 dB floor below. (testGermaniumKnee catches it too, and
// harder — its onset ratio collapses 139x -> 3.8x against a 20x bar — so the
// regression now has two independent bars, not one.) Perturbation transcript in
// docs/work/2026-07-31-gold-clipping-fidelity.md.
void testDiodeLevelContrast(double fs) {
    const double f0 = 220.0;
    std::printf("  [--] Ge-vs-Si ceiling contrast @ %.0f Hz (clean half OUT):\n", fs);
    // §50 probe re-derivation, still valid: the drive path attenuates (~0.22x at
    // 220 Hz) and A tops at 25.8x DC, so the pre-§50 rows (gain 0.10 at 0.3 V) never
    // reached EITHER ceiling — the comparison read the linear path. 0.6 V at
    // gain >= 0.35 puts the node's source at 1.4-10.6 V, far past both knees.
    for (float g : {0.35f, 0.60f, 1.00f}) {
        const Params p{g, 0.5f, 0.7f};
        const auto in = sine(f0, 0.6f, 0.6, fs);
        const double ge = tailRms(render(in, p, fs, 4, GoldModel::DIODE_GERMANIUM, false), fs);
        const double si = tailRms(render(in, p, fs, 4, GoldModel::DIODE_SILICON, false), fs);
        assert(ge > 1e-4 && si > 1e-4 && "a diode option produced no output");
        const double contrast = toDb(si) - toDb(ge);
        // Bracketed around the two device models' 14.61 dB knee ratio, wide enough
        // that the composed path's own shaping cannot push it out, tight enough that
        // restoring the §36 bug (kSiIdeality = 1.0 -> ~9 dB) fails it.
        assert(contrast > 11.0 &&
               "silicon does not clip far enough above germanium — is the 1N4148 "
               "ideality factor missing again? (audit finding 15, docs §36/§54)");
        assert(contrast < 18.0 && "silicon clips implausibly far above germanium");
        std::printf("       gain %.2f: Ge rms %.4f, Si rms %.4f -> %+.2f dB "
                    "(11.0-18.0 dB band, analytic knee ratio 14.61)\n", g, ge, si, contrast);
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
//
// §52 split this probe in two because the derived summing weight made the tone
// stage's base-rate rail clamp engage at max TREBLE, so the max-treble number stopped
// measuring aliasing at all (1x -20.3 / 4x -26.5 / 8x -26.4 dB — flat in the OS
// factor). §54 fixed the node the weight was multiplying and BOTH stimuli are hard
// again: at max TREBLE the floor is back to -92.9 dB at 4x/44.1 k and it MOVES with
// the factor (1x -27.3 / 8x -106.8), which is what "this measures aliasing" looks
// like. The split is kept, because two stimuli with two independent bars is strictly
// more than the one this file used to have.
void testAliasing(double fs) {
    using clipper::measure::measureAliasing;
    const double f0 = 4186.0;  // C8
    const auto in = sine(f0, 0.2f, 1.0, fs);

    // (a) THE CLIPPER'S OWN ALIASING — TREBLE at noon, so the tone stage's rail clamp
    //     is nowhere near engaging and the only nonlinearity in the path is the
    //     oversampled diode pair. This is the M2 bar, hard.
    const Params clipperOnly{1.0f, 0.5f, 0.9f};
    const double c4 = measureAliasing(render(in, clipperOnly, fs, 4), fs, f0).worstAliasDb;
    const double c1 = measureAliasing(render(in, clipperOnly, fs, 1), fs, f0).worstAliasDb;
    const double c8 = measureAliasing(render(in, clipperOnly, fs, 8), fs, f0).worstAliasDb;
    assert(c4 < -60.0 && "4x worst-alias at max GAIN not >= 60 dB below fundamental");
    assert(c4 < c1 - 20.0 && "4x did not clearly improve on 1x");
    std::printf("  [ok] aliasing @ %.0f Hz (TREBLE noon, clipper isolated): 1x %.1f dB, "
                "4x %.1f dB (bar -60), 8x %.1f dB\n", fs, c1, c4, c8);

    // (b) MAX TREBLE — §52's XFAIL `gold-summing-alias-at-treble-max`, XPASSED by §54
    //     and therefore asserted for real. Two bars, because the defect it replaces
    //     was NOT "the number is too high" but "the number stopped depending on the
    //     oversampling factor": the -60 dB M2 bar, AND a 20 dB improvement from 1x to
    //     4x that a base-rate clip cannot produce. Measured 44.1 k: 1x -27.3,
    //     4x -92.9, 8x -106.8 dB (it was 1x -20.3 / 4x -26.5 / 8x -26.4 pre-§54).
    const Params hot{1.0f, 1.0f, 0.9f};  // max gain, brightest treble
    const double w4 = measureAliasing(render(in, hot, fs, 4), fs, f0).worstAliasDb;
    const double w1 = measureAliasing(render(in, hot, fs, 1), fs, f0).worstAliasDb;
    const double w8 = measureAliasing(render(in, hot, fs, 8), fs, f0).worstAliasDb;
    assert(w4 < -60.0 &&
           "4x worst-alias at max GAIN + max TREBLE not >= 60 dB below fundamental "
           "(docs §54 — this was XFAIL gold-summing-alias-at-treble-max)");
    assert(w4 < w1 - 20.0 &&
           "the max-TREBLE alias floor does not improve with oversampling — it is "
           "measuring a BASE-RATE nonlinearity (a rail clip), not aliasing");
    std::printf("  [ok] aliasing @ %.0f Hz (TREBLE max, was XFAIL): 1x %.1f dB, "
                "4x %.1f dB (bar -60), 8x %.1f dB\n", fs, w1, w4, w8);
}

// --- Test 6: HEADROOM (the charge pump) + OUTPUT linearity. ------------------
// The +/-9 V rails exist so the germanium pair is the ONLY clipper: at a very hot
// 1.0 V input, wide open, the summing node must stay well inside the rails.
void testHeadroomAndOutput(double fs) {
    const auto hotIn = sine(220.0, 1.0f, 0.5, fs);
    const auto o = render(hotIn, {1.0f, 0.5f, 1.0f}, fs);
    double pk = 0.0;
    for (float v : o) pk = std::max(pk, static_cast<double>(std::fabs(v)));
    // The PROPERTY is the physical one: the +/-8.6 V charge-pump rails must not clip,
    // because the germanium pair is meant to be the only clipper in the box. §27
    // measured 1.60 V here and guarded at 3.0 — a snug drift guard around that
    // measurement, not the property. §52's derived summing weight (R20/(R16*kSumGain),
    // 6.42x the old fit) put it at 4.302 V and §54's diode-node fix brings it back to
    // 2.967 V, but the bound stays stated in the units it is actually about (the
    // rail), not re-tightened around whichever measurement is current.
    assert(pk < 6.0 && "output peak approaches the charge-pump rails (they should never clip)");
    {
        // Max GAIN + max TREBLE at an ordinary 0.2 V pick: does the tone stage's rail
        // clamp engage? Measured at its INPUT (the clamp sits before the OUTPUT pot,
        // so divide the rendered peak by the pot setting to read the clamped node).
        //
        // This was §52's XFAIL `gold-summing-rails-engage` (8.624 V against the 8.600 V
        // rail). §54's diode node XPASSED it — 1.274 V at 44.1 k / 1.166 V at 96 k —
        // so it is a hard assertion again, with the bar left at the rail itself.
        const auto pick = sine(4186.0, 0.2f, 0.5, fs);
        auto clampNodePeak = [&](float treble) {
            const float outPot = 0.9f;
            const auto r = render(pick, {1.0f, treble, outPot}, fs);
            double p = 0.0;
            for (float v : r) p = std::max(p, static_cast<double>(std::fabs(v)));
            return p / outPot;
        };
        const double noon = clampNodePeak(0.5f), bright = clampNodePeak(1.0f);
        assert(bright < 8.6 * 0.999 &&
               "the tone stage's charge-pump rail clamp ENGAGES at max GAIN + max "
               "TREBLE — the germanium pair is supposed to be the only clipper in the "
               "box (docs §27/§54; do NOT answer this by raising kRailVolts)");
        std::printf("  [ok] rails idle @ %.0f Hz (was XFAIL): tone-stage node peaks "
                    "%.3f V at TREBLE noon, %.3f V at TREBLE max (rail 8.600)\n",
                    fs, noon, bright);
    }
    // A 1 V input at GAIN 0 is still CLEAN — the headroom statement, measured.
    const double tClean = thd(render(hotIn, {0.0f, 0.5f, 0.5f}, fs), 220.0, fs);
    assert(tClean < 0.005 && "the clean path distorts at 1 V (no headroom)");
    // --- THE OUTPUT POT: the one of five that is NOT an audio taper. ----------
    //
    // REWRITTEN 2026-08-10 (docs §68). This used to read "OUTPUT is a linear pot
    // (house convention)" and assert exactly that. The lineup-wide taper slice
    // went looking for the convention's justification and found a SOURCE:
    //
    //   * the reference's own output stage (jatinchowdhury18/KlonCentaur,
    //     `OutputStageProcessor.cpp`) is R25 = 560 R in series + a 10 k pot +
    //     C15 = 4.7 uF, with its `OutputStageWDF.h` sibling naming the wiper's
    //     load as R28 = 100 k; and
    //   * the published analyses of the original say its Output pot is a LINEAR
    //     (B) taper and that an audio taper is a popular MOD — search-summary
    //     grade, labelled as such (the §66.4 precedent), corroborated
    //     independently by the reference mapping its control straight to the
    //     wiper position.
    //
    // So the law stayed linear-shaped, and these bars are here because it stayed
    // that way FOR A REASON. **Do not "tidy" this pedal into the same audioTaper
    // call the other four use** — bar (c) catches exactly that, and it is the
    // difference between a derived lineup and one house constant applied five
    // times (§63.14's lesson).
    const auto in = sine(220.0, 0.2f, 0.5, fs);
    auto rmsAt = [&](float v) { return tailRms(render(in, {0.6f, 0.5f, v}, fs), fs); };
    const double r100 = rmsAt(1.0f), r75 = rmsAt(0.75f);
    const double r50 = rmsAt(0.50f), r25 = rmsAt(0.25f);

    // (a) The NETWORK is modelled, and it measures within 0.5 dB of the identity
    //     map it replaced at every position (worst 0.180 dB, at knob 0.60). That
    //     is the honest headline: this pedal's old "house convention" map was
    //     already right, and is now right for a SOURCED reason.
    for (int i = 1; i <= 20; ++i) {
        const double k = i / 20.0;
        const double devDb = 20.0 * std::log10(clipper::dsp::pot::goldOutput(k) / k);
        assert(std::fabs(devDb) < 0.5 &&
               "the OUTPUT network moved more than 0.5 dB from a plain linear pot "
               "— R25/R28 cannot do that; something else changed");
    }

    // (b) It is still recognisably a LINEAR pot at the listening end: half
    //     rotation delivers ~49 %, nowhere near the 10-20 % an audio taper gives.
    const double halfFraction = r50 / r100;
    assert(halfFraction > 0.40 &&
           "OUTPUT at half rotation has dropped into audio-taper territory — the "
           "sources say this pedal's pot is LINEAR (B)");

    // (c) THE CONTRAST BAR, and the reason this pedal has one of its own: its dB
    //     per quarter turn is strongly UNEVEN (2.64 / 3.56 / 5.96 dB -> 2.258),
    //     a linear pot's signature. Its four siblings measure 1.23-1.27 against
    //     a < 1.60 bar. Fold the GOLD into the house audioTaper and this ratio
    //     collapses to ~1.27 and this assert goes red.
    const double s1 = std::fabs(toDb(r75 / r100));
    const double s2 = std::fabs(toDb(r50 / r75));
    const double s3 = std::fabs(toDb(r25 / r50));
    const double stepRatio =
        std::max(s1, std::max(s2, s3)) / std::min(s1, std::min(s2, s3));
    assert(stepRatio > 2.00 &&
           "OUTPUT now spends its dB evenly across the travel — that is an AUDIO "
           "taper's signature, and the sources say this pot is linear. If an "
           "audio taper was applied here because the other four have one, revert "
           "it: docs §68.5");

    // (d) The top of the knob does not move: R25's fixed 0.519 dB insertion loss
    //     is normalised out on purpose (a LEVEL fact, not a LAW fact — §68.3), so
    //     OUTPUT 1.0 renders bit-identically to the pre-slice build.
    assert(clipper::dsp::pot::goldOutput(1.0) == 1.0 &&
           "OUTPUT 1.0 is no longer exactly unity — the network's insertion loss "
           "has leaked into the law");
    assert(clipper::dsp::pot::goldOutput(0.0) == 0.0 && "OUTPUT 0 is not silence");

    std::printf(
        "  [ok] headroom @ %.0f Hz: 1 V in wide open -> peak %.3f V (rails 8.6 V never "
        "touched), clean-path THD %.4f%%\n"
        "  [ok] OUTPUT pot law (10k LINEAR + R25 560R + R28 100k, netlist-sourced — "
        "the ONE pedal of five with no audio taper): half rotation %.2f %% "
        "(siblings 11.4-11.9), steps %.2f/%.2f/%.2f dB ratio %.3f (bar > 2.00; "
        "siblings measure 1.23-1.27)\n",
        fs, pk, tClean * 100.0, 100.0 * halfFraction, s1, s2, s3, stepRatio);
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

// --- Test 9: THE CLIPPING-STAGE TRIO (docs §54). -----------------------------
// One bar per fix, each perturbation-proven against reverting THAT fix alone (the
// transcripts are in docs/work/2026-07-31-gold-clipping-fidelity.md). All four are
// measured through the whole model with the clean half switched OUT, so they read the
// dirt path end-to-end rather than a constant.
void testClippingStageFidelity(double fs) {
    const double f0 = 220.0;
    // Small-signal dirt-path gain at f, with the clean half OUT. 0.2 mV is small
    // enough that the diode pair is at its ZERO-BIAS incremental resistance
    // (rd = Vt/(2*Is) = 862 Ohm — note the reference fit has no truly linear region,
    // which is the germanium bloom as a number), so this reads the dirt path's
    // FILTERS times one frequency-independent constant. Every bar below is a RATIO
    // across frequency or across the knob, in which that constant cancels exactly.
    auto dirtDb = [&](double f, float gain) {
        const auto in = sine(f, 0.0002f, 0.5, fs);
        const auto o = render(in, {gain, 0.5f, 0.7f}, fs, 4, GoldModel::DIODE_GERMANIUM,
                              false);
        const size_t n = o.size(), win = std::min(n, static_cast<size_t>(0.25 * fs));
        return toDb(goertzelAmp(o, n - win, win, f, fs)) -
               toDb(goertzelAmp(in, n - win, win, f, fs));
    };

    // (1) THE DIODE NODE — the germanium ceiling, referred back to the node.
    // The dirt path's output is kSumGain * clipBlend(1) * LP495(V_node), and
    // kSumGain * clipBlend(1) is exactly R20/R16 = 8.3404, so dividing the rendered
    // peak by 8.3404 reads the clamped node (times the pole's 0.914 at 220 Hz, i.e.
    // 8.6 % low — the bar below is stated on the measured quantity, uncorrected).
    // Driven far past the knee (1 V in at max GAIN puts ~10.6 V on the node's source),
    // so this reads the CEILING, not the toe. The reference implementation's own
    // clipper, solved statically at that drive, clamps at 0.168 V; this model measures
    // 0.148 V through the pole (0.162 V node-referred). The pre-§54 pair (Is = 200 nA,
    // n = 1.3 behind Rs = 2.2 k) clamps at 0.336 V — that is the perturbation.
    {
        const auto in = sine(f0, 1.0f, 0.5, fs);
        const auto o = render(in, {1.0f, 0.5f, 1.0f}, fs, 4, GoldModel::DIODE_GERMANIUM,
                              false);
        double pk = 0.0;
        for (float v : o) pk = std::max(pk, static_cast<double>(std::fabs(v)));
        const double node = pk / (392.0e3 / 47.0e3);
        assert(node > 0.10 && node < 0.22 &&
               "the germanium clipping node's ceiling has left the reference "
               "implementation's measured fit (Is = 15 uA, Vt = 25.85 mV behind "
               "R13 = 1 k -> ~0.17 V); docs §54");
        std::printf("  [ok] diode node @ %.0f Hz: 1 V in wide open clamps at %.4f V "
                    "node-referred (reference static solve 0.168 V, band 0.10-0.22)\n",
                    fs, node);
    }

    // (2) THE SUMMING POLE — R20 || C13 = 495 Hz, on the dirt branch.
    // A one-pole low-pass at 495 Hz costs 3 kHz 12.7 dB more than 500 Hz
    // (20*log10(sqrt((1+(3000/495)^2)/(1+(500/495)^2)))), so the dirt path's
    // 3 kHz-vs-500 Hz tilt has to move by about that much. Measured -13.92 dB with the
    // pole in; removing the pole alone (and nothing else) leaves -4.9 dB. (It read
    // -17.6 dB until docs §56 replaced the drive path's input stand-in with the
    // reference divider, which is +3.7 dB brighter over that same pair — the pole did
    // not move, the signal reaching it did.)
    {
        const double t = dirtDb(3000.0, 0.5f) - dirtDb(500.0, 0.5f);
        assert(t < -12.0 &&
               "the 495 Hz summing pole (R20 || C13) is missing from the dirt path — "
               "this is the filter that makes a clipped Klon creamy (docs §54)");
        std::printf("  [ok] summing pole @ %.0f Hz: dirt path 3 kHz is %.2f dB below "
                    "500 Hz (bar -12; without the pole it is -4.9)\n", fs, t);
    }

    // (3) THE DRIVE AMP'S C7 — the ground-leg cap that makes the gain KNOB-dependent
    // in frequency, not just in level. Without C7 the drive amp is a flat A(g), so
    // the dirt path's 500-vs-110 Hz tilt is IDENTICAL at every knob position (every
    // other filter in that path is knob-independent). With C7 across R10b the zero
    // walks from 19 Hz (knob closed) to 970 Hz (knob open) and the tilt opens up.
    // Measured 7.90 dB at g = 0.15 -> 15.97 dB at g = 1.00, a spread of 8.07 dB;
    // with C7 removed (and nothing else) the four knob positions measure 7.27 /
    // 7.26 / 7.24 / 7.23 dB — a spread of -0.04 dB, i.e. flat to the measurement.
    {
        const double tiltLo = dirtDb(500.0, 0.15f) - dirtDb(110.0, 0.15f);
        const double tiltHi = dirtDb(500.0, 1.0f) - dirtDb(110.0, 1.0f);
        assert(tiltHi - tiltLo > 5.0 &&
               "the drive amp's gain is not KNOB-dependent in frequency — C7 (82 nF "
               "across R10b) is missing, so the stage is a flat A(g) (docs §54)");
        std::printf("  [ok] drive C7 @ %.0f Hz: 500-vs-110 Hz tilt %.2f dB at GAIN 0.15 "
                    "-> %.2f dB at GAIN 1.00 (spread %.2f dB, bar 5; flat A gives -0.04)\n",
                    fs, tiltLo, tiltHi, tiltHi - tiltLo);
    }

    // (4) THE GANG IDENTITY — re-derived in §56, and it got STRONGER, not weaker.
    //
    // §54 asserted this as "the drive amp's DC gain IS A(g)", measured as the dirt
    // path's gain ratio between two knob positions at 5 Hz, on the argument that every
    // OTHER filter in that path is knob-independent and therefore cancels. §56 breaks
    // that argument on purpose: the drive path's input divider is the reference's real
    // network now, and the GAIN pot's OTHER gang half is its shunt leg, so the divider
    // is knob-dependent too and no longer cancels. It contributes a measured 1.296 dB
    // of the ratio, which is why the pre-§56 model reads 12.480 dB here where the two
    // laws together predict 13.764.
    //
    // So the bar is restated on what the model must actually keep: ONE wiper drives
    // BOTH halves, and each half follows its own published law. The prediction is the
    // product of the two, and each factor is written from component values here in the
    // test, transcribed from the reference netlist rather than read out of the model:
    //   * drive amp     A(g) = 1 + R12/((1-g)*R_pot + Rleg)                (§50's law)
    //   * input divider H_pre(s) = Zp/(Zc3 + Zp),
    //                   Zp = [(R6 || 1/sC5) + g*R_pot] || [R7 + (R19 || 1/sC16)]
    // This is a HARDER assertion than §54's: it fails if either half stops tracking the
    // wiper, AND it fails if they track it in opposite directions — the failure mode a
    // two-network gang actually has, and one that leaves each network individually
    // well-formed. Perturbations measured for docs §56, against a 0.5 dB tolerance:
    //   * revert the divider to §50's knob-independent 0.65*HP600 stand-in -> 12.480
    //     measured against a 13.764 prediction, fails by 1.284 dB
    //   * recover the divider's half as R10b instead of its complement (the two gang
    //     halves wired backwards, which leaves each network individually well-formed)
    //     -> 7.212 measured against the same 13.764 prediction, fails by 6.552 dB
    {
        const double lo = 5.0;
        const double r = dirtDb(lo, 1.0f) - dirtDb(lo, 0.35f);
        const double lawOnly = toDb(GoldModel::driveGainAt(1.0)) -
                               toDb(GoldModel::driveGainAt(0.35));
        const double analytic = lawOnly + 20.0 * std::log10(refPreAmpMag(lo, 1.0) /
                                                           refPreAmpMag(lo, 0.35));
        assert(std::fabs(r - analytic) < 0.5 &&
               "the GAIN gang has drifted: the drive amp's DC gain must be §50's law "
               "A = 1 + 422k/((1-g)*100k + 17k) AND the input divider's shunt must be "
               "the SAME wiper's other half, g*100k (docs §50, §56)");
        std::printf("  [ok] gang identity @ %.0f Hz: dirt gain at 5 Hz rises %.3f dB from "
                    "GAIN 0.35 to 1.00; the two gang halves' laws say %.3f dB "
                    "(§50's drive law alone says %.3f)\n", fs, r, analytic, lawOnly);
    }

    // (5) THE INPUT DIVIDER ITSELF — docs §56, the refit §54 named and deferred.
    //
    // Until 2026-07-31 the drive path's input network was a SCALAR times a one-pole
    // high-pass (kDrivePreScale 0.65 into kDriveHpHz 600), fitted in §50. Measured
    // against the reference implementation's own PreAmpStage tree it was up to 7.23 dB
    // out across 41 Hz - 10 kHz (§54's quoted "+/-1.3 dB" was the 82 Hz - 1 kHz window
    // at one knob position); the netlist network measures 0.004 dB worst-case over the
    // same grid. Two bars, one at each end of the band, because the stand-in was wrong
    // in OPPOSITE directions there and a single-frequency bar would miss one of them:
    //
    //   (a) THE BOTTOM. The real divider passes far more low end to the diodes than a
    //       600 Hz high-pass does — |H_pre| at 41 Hz is 0.098 against the stand-in's
    //       0.044, i.e. the stand-in threw away 7 dB of the drive signal's bass. The
    //       divider's own 41-vs-500 Hz contribution is -12.04 dB; the stand-in's is
    //       -19.45. Measured through the whole dirt path: -9.60 dB now, -17.01 dB with
    //       the stand-in (the rest of that path is a net +2.4 dB over the same pair,
    //       the 495 Hz summing pole being most of it). Bar at the midpoint of the two.
    //
    //   (b) THE TOP. Above its corner the real divider is essentially transparent
    //       (0.981 at 6 kHz, 0.993 at 10 kHz) where the stand-in shelved flat at 0.65
    //       from 600 Hz up, so the drive path now delivers the treble the germanium is
    //       supposed to be clipping. The divider's own 6 kHz-vs-1 kHz contribution is
    //       +3.69 dB against the stand-in's +1.29. Measured through the dirt path:
    //       -21.50 dB now, -23.90 with the stand-in. Bar at the midpoint of the two.
    //
    // Both bars are one-sided and stated in absolute dB, so neither can be satisfied by
    // a compensating gain change somewhere else in the path.
    {
        const double bass = dirtDb(41.0, 0.35f) - dirtDb(500.0, 0.35f);
        assert(bass > -13.3 &&
               "the drive path has lost its low end again — the input divider is a "
               "one-pole 600 Hz high-pass, not the reference's C3/C5/R6/gang/R7/R19/C16 "
               "network (docs §56; the stand-in measures -17.01 dB here)");
        std::printf("  [ok] H_pre bottom @ %.0f Hz: dirt path 41 Hz is %.2f dB below "
                    "500 Hz (bar -13.3; §50's 0.65*HP600 stand-in gives -17.01; the "
                    "divider alone contributes %.2f)\n", fs, bass,
                    20.0 * std::log10(refPreAmpMag(41.0, 0.35) / refPreAmpMag(500.0, 0.35)));

        const double shelf = dirtDb(6000.0, 0.35f) - dirtDb(1000.0, 0.35f);
        assert(shelf > -22.7 &&
               "the drive path's top has been shelved again — the reference divider has "
               "essentially NO shelf loss above its corner, where §50's stand-in held a "
               "flat 0.65 (docs §56; the stand-in measures -23.90 dB here)");
        std::printf("  [ok] H_pre top @ %.0f Hz: dirt path 6 kHz is %.2f dB below 1 kHz "
                    "(bar -22.7; §50's stand-in gives -23.90; the divider alone "
                    "contributes %+.2f)\n", fs, shelf,
                    20.0 * std::log10(refPreAmpMag(6000.0, 0.35) / refPreAmpMag(1000.0, 0.35)));
    }
}

// --- Test 10: THE DRIVE PATH'S ARITHMETIC FLOOR (docs §56). ------------------
// §56's input divider is a THIRD-order recursion whose slowest pole (the C3/node
// corner, ~60 Hz) runs at the OVERSAMPLED rate — 176-192 kHz on the shipped path, so
// the pole sits at radius 0.998 and a direct form amplifies its own state rounding by
// roughly (1 - r)^-order. That is not a last-bit detail: with `float` state the model
// emits an AUDIBLE hiss, and the only reason it does not show up in a THD or a level
// measurement is that the noise is broadband while every other bar in this file is a
// single Goertzel bin.
//
// So this bar measures the thing directly and needs no second build: drive a pure
// tone, project out every harmonic of it up to Nyquist, and look at what is left. The
// bound is the project's own -120 dBFS solver gate (docs §25, §34).
// Measured (0.15 V / 220 Hz, 2 s, settled half):
//     double state (shipped)   -135.4 to -142.4 dBFS
//     float state              -72.4 to -80.1 dBFS   <- the perturbation, fails by 40 dB
// GAIN 1.00 is deliberately NOT probed: at max drive the model's own 4x alias floor
// (-93 dBFS, docs §54) dominates this metric, which would make the bar measure
// aliasing rather than arithmetic.
void testDrivePathNumericalFloor(double fs) {
    for (float g : {0.15f, 0.35f}) {
        const double f0 = 220.0;
        const auto in = sine(f0, 0.15f, 2.0, fs);
        const auto o = render(in, {g, 0.5f, 0.5f}, fs);
        const size_t start = o.size() / 2;
        const size_t cycles = static_cast<size_t>((double(o.size() - start) * f0) / fs);
        const size_t win = static_cast<size_t>(cycles * fs / f0);
        assert(win > 0 && start + win <= o.size());
        std::vector<double> y(win);
        for (size_t i = 0; i < win; ++i) y[i] = o[start + i];
        double sig = 0.0;
        for (int h = 1; h * f0 < fs * 0.5; ++h) {
            double re = 0.0, im = 0.0;
            for (size_t i = 0; i < win; ++i) {
                const double a = kTwoPi * h * f0 * double(i) / fs;
                re += y[i] * std::cos(a);
                im += y[i] * std::sin(a);
            }
            re *= 2.0 / win;
            im *= 2.0 / win;
            sig += 0.5 * (re * re + im * im);
            for (size_t i = 0; i < win; ++i) {
                const double a = kTwoPi * h * f0 * double(i) / fs;
                y[i] -= re * std::cos(a) + im * std::sin(a);
            }
        }
        double res = 0.0;
        for (size_t i = 0; i < win; ++i) res += y[i] * y[i];
        res /= win;
        const double resDb = 10.0 * std::log10(res + 1e-300);
        assert(sig > 1e-6 && "no signal — the probe rendered silence");
        assert(resDb < -120.0 &&
               "the drive path is emitting broadband noise above the project's "
               "-120 dBFS floor — the input divider's or the drive amp's recursion has "
               "been narrowed to float state (docs §56)");
        std::printf("  [ok] drive-path arithmetic floor @ %.0f Hz, GAIN %.2f: residual "
                    "%.2f dBFS (bar -120; float state measures -72 to -80)\n",
                    fs, g, resDb);
    }
}

}  // namespace

int main(int, char**) {
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
    testClippingStageFidelity(48000.0);
    testDrivePathNumericalFloor(44100.0);
    testDrivePathNumericalFloor(48000.0);
    std::printf("All GoldModel tests passed (no known open defects — §52's two XFAILs "
                "were XPASSed and deleted by §54).\n");
    return 0;
}
