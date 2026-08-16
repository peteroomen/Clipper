// Clipper — M13.5: the Uni-Vibe.
//
// PLAYER-OBSERVABLE BARS ONLY, and the two that carry the slice are both
// computed AGAINST THE SIBLING rather than quoted:
//
//   * THE ACCEPTANCE BAR — NOTCH DISTRIBUTION. The ROADMAP frames this voice as
//     "four MISMATCHED stages (not the Ninety's matched four)" and that framing
//     is wrong: `PhaserModel` already carries a deterministic +/-1.5 % per-stage
//     detune. The distinction is the MAGNITUDE — 0.22 uF against 470 pF is
//     468:1 of capacitance, i.e. 8.87 octaves of corner spread against 0.043 —
//     and what a player hears is the SPREAD OF THE NOTCH PAIR.
//     `testNotchDistribution` renders BOTH models from the identical stimulus
//     and asserts the contrast. The metric is SCALE-FREE (it does not move with
//     the cell resistance), which is why none of this voice's reconstructed
//     constants can reach it — `testNotchSpreadIsScaleFree` measures that claim
//     rather than asserting it.
//
//   * THE SECOND BAR — SWEEP ASYMMETRY, which is the one a JFET phaser cannot
//     fake. The lamp has thermal mass, so the notch frequency rises faster than
//     it falls at the same SPEED. `PhaserModel`'s rounded triangle is symmetric
//     about its own peak by construction and measures 1.00 on the identical
//     metric. `testSweepAsymmetry` asserts both halves, and the attribution to
//     the LAMP (rather than to the cell) is measured with two independent hooks,
//     not assumed — docs §66 records a single hook that removed two behaviours
//     at once producing a bar that could not fail.
//
//   * THE ABSOLUTE REFERENCES, such as they are. This voice has ONE sourced set
//     of component values (the four staggered caps) and one published device
//     range (the LDRs' 10 kOhm...100 kOhm under illumination). Both are asserted
//     literally, so a later slice cannot tidy the caps into a matched set or let
//     the lamp derivation drift off the published cell range.
//
// No XFAIL ledger: this suite has no known-bad property. Do not add one without
// a measured number.

#include "clipper/dsp/LampDrive.h"
#include "clipper/dsp/OptoCell.h"
#include "clipper/dsp/PhaserModel.h"
#include "clipper/dsp/VibeModel.h"

#include "support/AssertsLive.h"
#include "support/StepSlew.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

using namespace clipper::dsp;

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
//  Shared measurement helpers
// ---------------------------------------------------------------------------

template <class M>
void render(M& m, const std::vector<float>& in, std::vector<float>& out, int block = 128) {
    out.assign(in.size(), 0.0f);
    int done = 0;
    const int n = static_cast<int>(in.size());
    while (done < n) {
        const int k = std::min(block, n - done);
        m.process(in.data() + done, out.data() + done, k);
        done += k;
    }
}

// A HANN-WINDOWED Goertzel. The window is not decoration: docs §60 records a
// whole slice nearly going the wrong way because a RECTANGULAR window's own
// sidelobe read as a -55.8 dB noise floor. Everything spectral here is windowed.
double magAt(const float* x, int n, double fs, double f) {
    double re = 0.0, im = 0.0, wsum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));
        const double ph = 2.0 * kPi * f * i / fs;
        re += w * x[i] * std::cos(ph);
        im -= w * x[i] * std::sin(ph);
        wsum += w;
    }
    return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

// Magnitude response in dB at `probe` frequencies, measured the honest way: a
// real tone in, the settled tail out, both read with the same windowed Goertzel.
template <class M>
std::vector<double> responseDb(M& m, double fs, const std::vector<double>& probe,
                               double amp = 0.2, double secs = 0.12) {
    std::vector<double> db;
    db.reserve(probe.size());
    const int n = static_cast<int>(fs * secs);
    std::vector<float> in(static_cast<size_t>(n)), out;
    for (double f : probe) {
        for (int i = 0; i < n; ++i)
            in[static_cast<size_t>(i)] =
                static_cast<float>(amp * std::sin(2.0 * kPi * f * i / fs));
        render(m, in, out);
        const int half = n / 2;
        const double o = magAt(out.data() + half, half, fs, f);
        const double v = magAt(in.data() + half, half, fs, f);
        db.push_back(20.0 * std::log10(std::max(1e-12, o / std::max(1e-12, v))));
    }
    return db;
}

// The notch frequencies of a dry+wet allpass sum, found from a RENDERED response
// by parabolic refinement of local minima on the (geometric) probe grid.
std::vector<double> findNotches(const std::vector<double>& fr, const std::vector<double>& db,
                                double floorDb = -6.0) {
    std::vector<double> ns;
    for (size_t i = 1; i + 1 < db.size(); ++i) {
        if (db[i] < db[i - 1] && db[i] < db[i + 1] && db[i] < floorDb) {
            const double y0 = db[i - 1], y1 = db[i], y2 = db[i + 1];
            const double denom = (y0 - 2.0 * y1 + y2);
            double d = 0.0;
            if (std::fabs(denom) > 1e-12) d = 0.5 * (y0 - y2) / denom;
            d = std::max(-1.0, std::min(1.0, d));
            const double step = std::log(fr[i + 1] / fr[i]);
            ns.push_back(fr[i] * std::exp(d * step));
        }
    }
    return ns;
}

std::vector<double> logGrid(double lo, double hi, int n) {
    std::vector<double> g;
    g.reserve(static_cast<size_t>(n) + 1);
    for (int i = 0; i <= n; ++i) g.push_back(lo * std::pow(hi / lo, static_cast<double>(i) / n));
    return g;
}

// The sweep-asymmetry metric, defined ONCE so the two models cannot drift on what
// it means: track the allpass corner over the settled tail, and compare the mean
// |d log2(fc)/dt| while it is RISING against the same while it is FALLING.
// Reported as max/min so it is >= 1 by construction and 1.000 means "symmetric".
//
// It is deliberately a SLOPE ratio and not a time ratio: a lag that merely delays
// a symmetric waveform leaves both half-periods equal, so a time ratio would
// measure phase and call it asymmetry.
struct SweepTrack {
    double asymmetry = 0.0;
    double riseOctPerSec = 0.0;
    double fallOctPerSec = 0.0;
    double depthOctaves = 0.0;
};

template <class Step>
SweepTrack trackSweep(Step step, double fs, int block, double secs) {
    const int nb = static_cast<int>(secs * fs / block);
    std::vector<double> lc;
    lc.reserve(static_cast<size_t>(nb));
    for (int b = 0; b < nb; ++b) lc.push_back(std::log2(step()));
    const double dt = static_cast<double>(block) / fs;
    const size_t first = lc.size() / 2;  // the settled tail only
    double up = 0.0, dn = 0.0;
    int nUp = 0, nDn = 0;
    double lo = std::numeric_limits<double>::max(), hi = -lo;
    for (size_t i = first; i < lc.size(); ++i) {
        lo = std::min(lo, lc[i]);
        hi = std::max(hi, lc[i]);
        if (i == first) continue;
        const double d = (lc[i] - lc[i - 1]) / dt;
        if (d > 0.0) {
            up += d;
            ++nUp;
        } else if (d < 0.0) {
            dn += -d;
            ++nDn;
        }
    }
    SweepTrack t;
    t.riseOctPerSec = nUp ? up / nUp : 0.0;
    t.fallOctPerSec = nDn ? dn / nDn : 0.0;
    t.depthOctaves = hi - lo;
    if (t.riseOctPerSec > 0.0 && t.fallOctPerSec > 0.0)
        t.asymmetry = t.riseOctPerSec > t.fallOctPerSec ? t.riseOctPerSec / t.fallOctPerSec
                                                        : t.fallOctPerSec / t.riseOctPerSec;
    return t;
}

// The Ninety's SPEED knob position that produces a given LFO rate, so the two
// models can be compared at the SAME Hz rather than at the same knob.
float phaserKnobForHz(double hz) {
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 200; ++i) {
        const double m = 0.5 * (lo + hi);
        if (PhaserModel::speedKnobToHz(m) < hz)
            lo = m;
        else
            hi = m;
    }
    return static_cast<float>(0.5 * (lo + hi));
}

double rms(const std::vector<float>& x) {
    double s = 0.0;
    for (float v : x) s += static_cast<double>(v) * v;
    return x.empty() ? 0.0 : std::sqrt(s / x.size());
}

// The analytic ANALOG four-allpass network's notch pair at cell resistance R.
// This is NOT derived from the model's discretization — it is the continuous-time
// phase sum of the four staggered RC corners — so comparing the rendered notch
// against it is a real reference and not an identity (CLAUDE.md's "measure, don't
// assert").
std::pair<double, double> analogNotches(double R) {
    double fc[4];
    for (int s = 0; s < 4; ++s) fc[s] = 1.0 / (2.0 * kPi * R * VibeModel::stageCapFarads(s));
    auto phase = [&](double f) {
        double a = 0.0;
        for (int s = 0; s < 4; ++s) a += std::atan(f / fc[s]);
        return a;
    };
    auto solve = [&](double target) {
        double lo = 0.05, hi = 1.0e7;
        for (int i = 0; i < 200; ++i) {
            const double mid = std::sqrt(lo * hi);
            if (phase(mid) < target)
                lo = mid;
            else
                hi = mid;
        }
        return std::sqrt(lo * hi);
    };
    return {solve(kPi / 2.0), solve(3.0 * kPi / 2.0)};
}

// ===========================================================================
//  1. THE ONE SOURCED SET OF COMPONENT VALUES
// ===========================================================================
// S1: 0.015 uF / 0.22 uF / 470 pF / 0.0047 uF, and the sources also say the
// reason for these particular values is NOT KNOWN. So they ship literally and are
// asserted literally: a later slice cannot tidy them into a matched set, a
// geometric progression, or anything else that would look like a rationale.
void testStaggeredCaps() {
    assert(VibeModel::numStages() == 4);
    const double want[4] = {0.015e-6, 0.22e-6, 470.0e-12, 0.0047e-6};
    for (int s = 0; s < 4; ++s) assert(std::fabs(VibeModel::stageCapFarads(s) / want[s] - 1.0) < 1e-12);

    for (int a = 0; a < 4; ++a)
        for (int b = a + 1; b < 4; ++b)
            assert(std::fabs(VibeModel::stageCapFarads(a) - VibeModel::stageCapFarads(b)) > 1e-15);

    double lo = 1e9, hi = 0.0;
    for (int s = 0; s < 4; ++s) {
        lo = std::min(lo, VibeModel::stageCapFarads(s));
        hi = std::max(hi, VibeModel::stageCapFarads(s));
    }
    const double ratio = hi / lo;
    const double oct = std::log2(ratio);

    double plo = 1e9, phi = 0.0;
    for (int s = 0; s < PhaserModel::numStages(); ++s) {
        plo = std::min(plo, PhaserModel::stageDetune(s));
        phi = std::max(phi, PhaserModel::stageDetune(s));
    }
    const double pOct = std::log2(phi / plo);
    std::printf("[caps] stagger %.1f:1 = %.3f oct | Ninety detune %.4f:1 = %.4f oct | %.1fx\n",
                ratio, oct, phi / plo, pOct, oct / pOct);
    assert(ratio > 400.0);
    assert(oct > 8.5);
    assert(pOct < 0.06);
    assert(oct / pOct > 100.0);
}

// ===========================================================================
//  2. THE LAMP DERIVATION IS ANCHORED TO A PUBLISHED DEVICE RANGE
// ===========================================================================
// S6 publishes the LDRs' illuminated range as 10 kOhm...100 kOhm. The lamp's bias
// and swing are NOT chosen: `LampDrive`'s inverse says what drive holds each end,
// and the model uses those two numbers. This bar is what stops that derivation
// silently drifting into a fit.
void testCellRangeIsThePublishedOne() {
    const double fs = 48000.0;
    VibeModel v;
    v.prepare(fs, 128);
    v.setParameter(VibeModel::PARAM_SPEED, 0.0f);      // 0.1 Hz — steady state reachable
    v.setParameter(VibeModel::PARAM_INTENSITY, 1.0f);  // full LFO into the lamp
    v.setParameter(VibeModel::PARAM_MODE, 0.0f);

    const int block = 256;
    std::vector<float> in(static_cast<size_t>(block), 0.0f), out(static_cast<size_t>(block));
    double rlo = 1e30, rhi = 0.0;
    const int nb = static_cast<int>(40.0 * fs / block);
    for (int b = 0; b < nb; ++b) {
        v.process(in.data(), out.data(), block);
        if (static_cast<double>(b) * block > 20.0 * fs) {
            const double r = v.cellResistanceOhms(0);
            rlo = std::min(rlo, r);
            rhi = std::max(rhi, r);
        }
    }
    std::printf("[cell] rendered excursion %.0f .. %.0f ohms (published 10k..100k)\n", rlo, rhi);
    std::printf("[cell] lamp drive endpoints: dim %.6f  bright %.6f V\n", v.lampDriveDimVolts(),
                v.lampDriveBrightVolts());
    // Within ~10 % of the published endpoints. It cannot be exact: the filament
    // does not quite reach steady state even at 0.1 Hz, which is the whole point
    // of the voice.
    assert(rlo > 9.0e3 && rlo < 11.5e3);
    assert(rhi > 88.0e3 && rhi < 112.0e3);
    assert(v.lampDriveBrightVolts() > v.lampDriveDimVolts());

    // S2: ONE lamp, ONE reflective box. The four cells therefore see the same
    // light and return the same resistance. Asserted so a future per-cell
    // tolerance has to be a deliberate, sourced act.
    for (int s = 1; s < 4; ++s) assert(v.cellResistanceOhms(s) == v.cellResistanceOhms(0));
}

// ===========================================================================
//  3. THE ACCEPTANCE BAR — NOTCH DISTRIBUTION vs `PhaserModel`
// ===========================================================================
struct NotchPair {
    double lo = 0.0, hi = 0.0, spreadOct = 0.0, deepestDb = 0.0;
};

template <class M>
NotchPair measureNotches(M& m, double fs, const char* label) {
    const std::vector<double> grid = logGrid(30.0, 16000.0, 240);
    const std::vector<double> db = responseDb(m, fs, grid);
    const std::vector<double> ns = findNotches(grid, db);
    NotchPair p;
    p.deepestDb = *std::min_element(db.begin(), db.end());
    assert(ns.size() >= 2 && "expected the two notches a 4-allpass dry+wet sum has");
    p.lo = ns.front();
    p.hi = ns.back();
    p.spreadOct = std::log2(p.hi / p.lo);
    std::printf("[notch] %-7s lo %8.2f Hz  hi %9.2f Hz  spread %.4f oct  deepest %.2f dB\n", label,
                p.lo, p.hi, p.spreadOct, p.deepestDb);
    return p;
}

void testNotchDistribution() {
    const double fs = 48000.0;

    // The Uni-Vibe at INTENSITY 0 — its own static operating point, a real knob
    // setting, not a measurement hook.
    VibeModel v;
    v.prepare(fs, 128);
    v.setParameter(VibeModel::PARAM_SPEED, 0.0f);
    v.setParameter(VibeModel::PARAM_INTENSITY, 0.0f);
    v.setParameter(VibeModel::PARAM_MODE, 0.0f);
    const NotchPair vibe = measureNotches(v, fs, "vibe");

    // The Ninety frozen mid-sweep — its own measurement hook, and the identical
    // stimulus, grid and notch finder.
    PhaserModel p;
    p.prepare(fs);
    p.setFrozen(true, 0.5);
    const NotchPair ninety = measureNotches(p, fs, "ninety");

    const double contrast = vibe.spreadOct - ninety.spreadOct;
    std::printf("[notch] CONTRAST %.4f octaves (vibe %.4f / ninety %.4f = %.3fx)\n", contrast,
                vibe.spreadOct, ninety.spreadOct, vibe.spreadOct / ninety.spreadOct);

    // Bars, with the margin RECORDED not snugged.
    assert(vibe.spreadOct > 5.0);
    assert(ninety.spreadOct < 2.8);
    assert(contrast > 2.5);
    assert(vibe.spreadOct / ninety.spreadOct > 2.0);
    // The notches are real notches, not shoulders.
    assert(vibe.deepestDb < -20.0);
    assert(ninety.deepestDb < -20.0);
}

// The scale-free claim, MEASURED. The notch spread is set by the CAP RATIOS
// alone, so it must not move as the lamp sweeps the cell resistance — which is
// what makes the acceptance bar immune to every reconstructed constant in the
// lamp card. Sampled at four points of a slow sweep, each measured in a window
// short enough that the LFO barely moves inside it.
void testNotchSpreadIsScaleFree() {
    const double fs = 48000.0;
    // Low probes need whole cycles, so the window is frequency-dependent: at
    // least 60 ms and at least 10 cycles. A fixed 30 ms window at 30 Hz measures
    // less than one cycle and the Goertzel reports noise.
    const std::vector<double> grid = logGrid(30.0, 16000.0, 130);

    double lo = 1e9, hi = -1e9, rLo = 1e30, rHi = 0.0;
    int measured = 0;
    // Frozen LFO phases: a sine, so 0.25 is the peak (brightest, lowest R) and
    // 0.75 the trough. These four span most of the sweep while keeping BOTH
    // notches inside the measurable band.
    for (double phase : {0.25, 0.35, 0.45, 0.55}) {
        VibeModel v;
        v.prepare(fs, 128);
        v.setParameter(VibeModel::PARAM_SPEED, 0.5f);
        v.setParameter(VibeModel::PARAM_INTENSITY, 1.0f);
        v.setParameter(VibeModel::PARAM_MODE, 0.0f);
        v.setFrozen(true, phase);

        const double r = v.cellResistanceOhms(0);
        rLo = std::min(rLo, r);
        rHi = std::max(rHi, r);

        std::vector<double> db;
        db.reserve(grid.size());
        for (double f : grid) {
            const int n = static_cast<int>(fs * std::max(0.06, 10.0 / f));
            std::vector<float> in(static_cast<size_t>(n)), out;
            for (int i = 0; i < n; ++i)
                in[static_cast<size_t>(i)] =
                    static_cast<float>(0.2 * std::sin(2.0 * kPi * f * i / fs));
            render(v, in, out);
            const int half = n / 2;
            db.push_back(20.0 * std::log10(std::max(
                1e-12, magAt(out.data() + half, half, fs, f) /
                           std::max(1e-12, magAt(in.data() + half, half, fs, f)))));
        }
        const std::vector<double> ns = findNotches(grid, db, -4.0);
        if (ns.size() < 2) {
            std::printf("[scale] phase %.2f: R %8.0f - fewer than two notches in band\n", phase, r);
            continue;
        }
        const double sp = std::log2(ns.back() / ns.front());
        // The ANALYTIC analog spread at this same R, which depends ONLY on the cap
        // ratios: printed alongside so the claim is visible, not just asserted.
        const std::pair<double, double> an = analogNotches(r);
        std::printf("[scale] phase %.2f: R %8.0f  notches %7.1f / %8.1f  spread %.4f oct "
                    "(analog %.4f)\n",
                    phase, r, ns.front(), ns.back(), sp, std::log2(an.second / an.first));
        lo = std::min(lo, sp);
        hi = std::max(hi, sp);
        ++measured;
    }
    std::printf("[scale] over a %.2fx resistance excursion: spread %.4f .. %.4f oct (range %.4f)\n",
                rHi / rLo, lo, hi, hi - lo);
    assert(measured >= 3);
    assert(rHi / rLo > 2.0);  // the resistance really did move
    assert(hi - lo < 0.30);   // ...and the spread did not
    assert(lo > 5.0);
}

// ===========================================================================
//  4. THE SECOND BAR — SWEEP ASYMMETRY, and the attribution
// ===========================================================================
void testSweepAsymmetry() {
    const double fs = 48000.0;
    const int block = 64;

    const float speeds[3] = {0.4f, 0.6f, 0.8f};
    for (float sk : speeds) {
        const double hz = VibeModel::speedKnobToHz(sk);
        const double secs = std::max(8.0, 8.0 / hz);

        VibeModel v;
        v.prepare(fs, 128);
        v.setParameter(VibeModel::PARAM_SPEED, sk);
        v.setParameter(VibeModel::PARAM_INTENSITY, 1.0f);
        v.setParameter(VibeModel::PARAM_MODE, 0.0f);
        std::vector<float> sil(static_cast<size_t>(block), 0.0f), o(static_cast<size_t>(block));
        const SweepTrack vt = trackSweep(
            [&] {
                v.process(sil.data(), o.data(), block);
                return v.stageCornerHz(0);
            },
            fs, block, secs);

        PhaserModel p;
        p.prepare(fs);
        p.setSpeed(phaserKnobForHz(hz));
        const SweepTrack pt = trackSweep(
            [&] {
                p.process(sil.data(), o.data(), block);
                return p.currentCornerHz();
            },
            fs, block, secs);

        std::printf(
            "[asym] %.3f Hz | vibe rise %7.3f fall %7.3f oct/s -> %.4f (depth %.3f oct) | "
            "ninety rise %7.3f fall %7.3f -> %.4f (depth %.3f oct)\n",
            hz, vt.riseOctPerSec, vt.fallOctPerSec, vt.asymmetry, vt.depthOctaves,
            pt.riseOctPerSec, pt.fallOctPerSec, pt.asymmetry, pt.depthOctaves);

        // THE BAR. The lamp heats faster than it cools, so the corner RISES
        // faster than it falls; the Ninety's rounded triangle is symmetric about
        // its own peak by construction.
        assert(vt.riseOctPerSec > vt.fallOctPerSec);
        assert(vt.asymmetry > 1.10);
        assert(pt.asymmetry < 1.02);
        assert(vt.asymmetry / pt.asymmetry > 1.10);
    }
}

// The asymmetry is attributed rather than assumed: the lamp's thermal mass and
// the cell's own dynamics are removed INDEPENDENTLY, and the numbers say which
// one it is.
void testAsymmetryIsTheLamp() {
    const double fs = 48000.0;
    const int block = 64;
    auto run = [&](bool lampMass, bool cellDyn, const char* label) {
        VibeModel v;
        v.prepare(fs, 128);
        v.setLampThermalMass(lampMass);
        v.setCellDynamics(cellDyn);
        v.setParameter(VibeModel::PARAM_SPEED, 0.6f);
        v.setParameter(VibeModel::PARAM_INTENSITY, 1.0f);
        v.setParameter(VibeModel::PARAM_MODE, 0.0f);
        std::vector<float> sil(static_cast<size_t>(block), 0.0f), o(static_cast<size_t>(block));
        const SweepTrack t = trackSweep(
            [&] {
                v.process(sil.data(), o.data(), block);
                return v.stageCornerHz(0);
            },
            fs, block, 12.0);
        std::printf("[attrib] %-22s asym %.4f   depth %.3f oct\n", label, t.asymmetry,
                    t.depthOctaves);
        return t.asymmetry;
    };
    const double shipped = run(true, true, "shipped");
    const double noLamp = run(false, true, "lamp instantaneous");
    const double noCell = run(true, false, "cell memoryless");
    const double neither = run(false, false, "both removed");

    // With BOTH removed the sweep is the LFO's own shape — a sine, symmetric.
    assert(neither < 1.03);
    // Removing the LAMP's thermal mass collapses the asymmetry; removing the
    // cell's dynamics barely moves it. That is the attribution, as a number.
    assert(noLamp < 1.08);
    assert(noCell > 1.12);
    assert(shipped - noLamp > (shipped - noCell) * 2.5);
}

// ===========================================================================
//  5. MODES — the §62 lesson, applied on the way in
// ===========================================================================
void testModeIsAWeightNotASwitch() {
    const double fs = 48000.0;

    // VIBRATO really has NO dry path — an `==`, not a tolerance, because
    // `OnePoleSmoother` converges EXACTLY to a zero target (§62's numeric note).
    {
        VibeModel v;
        v.prepare(fs, 128);
        v.setParameter(VibeModel::PARAM_MODE, 1.0f);
        std::vector<float> in(128, 0.1f), o(128);
        for (int b = 0; b < 400; ++b) v.process(in.data(), o.data(), 128);
        std::printf("[mode] vibrato dryW = %.20g   wetW = %.20g\n", v.dryWeight(), v.wetWeight());
        assert(v.dryWeight() == 0.0);
        assert(std::fabs(v.wetWeight() - 1.0) < 1e-6);
    }
    // CHORUS is S5's two 100 kOhm resistors: equal weights.
    {
        VibeModel v;
        v.prepare(fs, 128);
        v.setParameter(VibeModel::PARAM_MODE, 0.0f);
        std::vector<float> in(128, 0.1f), o(128);
        for (int b = 0; b < 400; ++b) v.process(in.data(), o.data(), 128);
        assert(std::fabs(v.dryWeight() - 0.5) < 1e-9);
        assert(std::fabs(v.wetWeight() - 0.5) < 1e-9);
    }

    // The click test: the mode change lands MID-RENDER (CLAUDE.md's rule — a
    // change applied before rendering starts passes even with the declick
    // deleted), swept over 16 step positions so the worst is what is reported.
    clipper::test::StepSlewConfig c;
    c.sampleRate = fs;
    c.amplitude = 0.15;
    VibeModel v;
    const clipper::test::StepSlewResult r = clipper::test::measureStepSlew(
        c,
        [&] {
            v.prepare(fs, 128);
            v.setParameter(VibeModel::PARAM_SPEED, 0.3f);
            v.setParameter(VibeModel::PARAM_INTENSITY, 0.7f);
            v.setParameter(VibeModel::PARAM_MODE, 0.0f);
        },
        [&] { v.setParameter(VibeModel::PARAM_MODE, 1.0f); },
        [&](const float* in, float* out, int n) { v.process(in, out, n); });
    std::printf("[mode] CHORUS -> VIBRATO mid-render seam/slew = %.2fx (§62 measured 17.02x for a "
                "forwarded hard switch)\n",
                r.ratio);
    assert(r.ratio < 2.5);
}

// ===========================================================================
//  6. INTENSITY 0 IS NOT BYPASS
// ===========================================================================
// S8 makes INTENSITY the fraction of the LFO reaching the LAMP DRIVER, so at zero
// the lamp still sits at its bias point and the four stages sit at a FIXED corner
// set. In CHORUS that is a static comb (audibly not bypass); in VIBRATO it is a
// static allpass, i.e. flat magnitude. Both are measured, because the plan said to
// assert what it SHOULD be rather than assume.
void testIntensityZeroIsAStaticNetwork() {
    const double fs = 48000.0;
    const std::vector<double> probe = {110.0, 220.0, 440.0, 880.0, 1760.0, 3520.0};

    {
        VibeModel v;
        v.prepare(fs, 128);
        v.setParameter(VibeModel::PARAM_SPEED, 0.5f);
        v.setParameter(VibeModel::PARAM_INTENSITY, 0.0f);
        v.setParameter(VibeModel::PARAM_MODE, 0.0f);
        const std::vector<double> db = responseDb(v, fs, probe);
        double lo = 1e9, hi = -1e9;
        for (double d : db) {
            lo = std::min(lo, d);
            hi = std::max(hi, d);
        }
        std::printf("[int0] chorus static comb: ");
        for (size_t i = 0; i < probe.size(); ++i) std::printf("%.0fHz %+.2f  ", probe[i], db[i]);
        std::printf("| span %.2f dB\n", hi - lo);
        assert(hi - lo > 6.0);  // NOT bypass, by a wide margin
    }

    {
        VibeModel v;
        v.prepare(fs, 128);
        v.setParameter(VibeModel::PARAM_SPEED, 1.0f);  // fastest — nothing to sweep
        v.setParameter(VibeModel::PARAM_INTENSITY, 0.0f);
        v.setParameter(VibeModel::PARAM_MODE, 0.0f);
        const int block = 64;
        std::vector<float> sil(static_cast<size_t>(block), 0.0f), o(static_cast<size_t>(block));
        const SweepTrack t = trackSweep(
            [&] {
                v.process(sil.data(), o.data(), block);
                return v.stageCornerHz(0);
            },
            fs, block, 4.0);
        std::printf("[int0] corner movement at INTENSITY 0, SPEED max: %.3e octaves\n",
                    t.depthOctaves);
        assert(t.depthOctaves < 1e-6);
    }

    {
        VibeModel v;
        v.prepare(fs, 128);
        v.setParameter(VibeModel::PARAM_SPEED, 0.5f);
        v.setParameter(VibeModel::PARAM_INTENSITY, 0.0f);
        v.setParameter(VibeModel::PARAM_MODE, 1.0f);
        const std::vector<double> db = responseDb(v, fs, probe);
        double worst = 0.0;
        for (double d : db) worst = std::max(worst, std::fabs(d));
        std::printf("[int0] vibrato static allpass: worst |gain| deviation %.4f dB\n", worst);
        // A pure allpass cascade has flat magnitude. The residual is the
        // oversampler's own halfband response, not the network's.
        assert(worst < 0.25);
    }
}

// ===========================================================================
//  7. OVERSAMPLING — the decision, measured both ways, and the factor PINNED
// ===========================================================================
void testOversamplingDecision() {
    const double fs = 48000.0;

    // (a) THE ALIAS FLOOR IS FLAT IN THE FACTOR. A single 9 kHz tone (a two-tone
    // stimulus would fill the band with legitimate IMD — §59's trap), read with a
    // HANN window (§60's trap), worst non-harmonic bin in band.
    std::printf("[os] alias floor vs factor (9 kHz, 0.30 V, sweeping):\n");
    double floorLo = 1e9, floorHi = -1e9;
    for (int f : {1, 2, 4, 8}) {
        VibeModel v;
        v.prepare(fs, 128);
        v.setOversampling(f);
        v.setParameter(VibeModel::PARAM_SPEED, 0.7f);
        v.setParameter(VibeModel::PARAM_INTENSITY, 1.0f);
        v.setParameter(VibeModel::PARAM_MODE, 0.0f);
        const int n = static_cast<int>(fs * 0.5);
        std::vector<float> in(static_cast<size_t>(n)), out;
        for (int i = 0; i < n; ++i)
            in[static_cast<size_t>(i)] =
                static_cast<float>(0.30 * std::sin(2.0 * kPi * 9000.0 * i / fs));
        render(v, in, out);
        const int half = n / 2;
        const double ref = magAt(out.data() + half, half, fs, 9000.0);
        double worst = 0.0;
        for (double probe = 200.0; probe < 20000.0; probe += 100.0) {
            if (std::fabs(probe - 9000.0) < 500.0) continue;   // the tone
            if (std::fabs(probe - 18000.0) < 500.0) continue;  // h2
            worst = std::max(worst, magAt(out.data() + half, half, fs, probe));
        }
        const double db = 20.0 * std::log10(std::max(1e-14, worst / std::max(1e-14, ref)));
        std::printf("      %dx -> %.2f dB (latency %d)\n", f, db, v.latencySamples());
        floorLo = std::min(floorLo, db);
        floorHi = std::max(floorHi, db);
    }
    std::printf("[os] floor spread across 1x..8x = %.2f dB — FLAT means it is not aliasing\n",
                floorHi - floorLo);
    assert(floorHi < -60.0);           // low at EVERY factor, 1x included
    assert(floorHi - floorLo < 15.0);  // and it does not move with the factor

    // (b) THE DISCRETIZATION ERROR IS WHY IT IS OVERSAMPLED. Measured against the
    // analytic ANALOG network, which is not derived from this model's
    // discretization — so it is a real reference and not an identity.
    const std::vector<double> grid = logGrid(30.0, 16000.0, 240);
    double err1x = 0.0, err4x = 0.0;
    for (int f : {1, 4}) {
        VibeModel v;
        v.prepare(44100.0, 128);  // the WORST shipped rate for this defect
        v.setOversampling(f);
        v.setParameter(VibeModel::PARAM_SPEED, 0.0f);
        v.setParameter(VibeModel::PARAM_INTENSITY, 0.0f);
        v.setParameter(VibeModel::PARAM_MODE, 0.0f);
        const std::vector<double> db = responseDb(v, 44100.0, grid);
        const std::vector<double> ns = findNotches(grid, db);
        assert(ns.size() >= 2);
        const std::pair<double, double> an = analogNotches(v.cellResistanceOhms(0));
        const double e = 100.0 * (ns.back() / an.second - 1.0);
        std::printf("[os] 44.1 kHz %dx: upper notch %.1f Hz vs analog %.1f Hz -> %+.2f %%\n", f,
                    ns.back(), an.second, e);
        (f == 1 ? err1x : err4x) = std::fabs(e);
    }
    // The Ninety's own base-rate discretization error, measured the same way from
    // its own matched-stage analog geometry. This is the yardstick the factor was
    // DERIVED against: 4x is the smallest house factor that beats it.
    double ninetyErr = 0.0;
    {
        PhaserModel p;
        p.prepare(44100.0);
        p.setFrozen(true, 0.5);
        const std::vector<double> db = responseDb(p, 44100.0, grid);
        const std::vector<double> ns = findNotches(grid, db);
        assert(ns.size() >= 2);
        const double analog = std::tan(3.0 * kPi / 8.0) * PhaserModel::cornerHzAtPhase(0.5);
        ninetyErr = std::fabs(100.0 * (ns.back() / analog - 1.0));
        std::printf("[os] ninety 44.1 kHz 1x: upper notch %.1f Hz vs analog %.1f Hz -> %.2f %%\n",
                    ns.back(), analog, ninetyErr);
    }
    std::printf("[os] discretization error: vibe 1x %.2f %% -> 4x %.2f %%  (ninety 1x %.2f %%)\n",
                err1x, err4x, ninetyErr);
    assert(err1x > 10.0);       // base rate is genuinely bad here
    assert(err4x < 2.0);        // 4x fixes it
    assert(err4x < ninetyErr);  // and clears the sibling's own error — the criterion

    // The shipped default is PINNED (§58's could-not-fail finding: nothing else
    // in this file reads it back).
    VibeModel v;
    v.prepare(fs, 128);
    std::printf("[os] shipped default factor %d, latency %d samples (%.2f ms at 48 kHz)\n",
                v.oversampling(), v.latencySamples(), 1000.0 * v.latencySamples() / fs);
    assert(v.oversampling() == 4);
    assert(v.latencySamples() == 72);
}

// ===========================================================================
//  8. `OptoCell` WAS NOT WIDENED — the structural half of the claim
// ===========================================================================
// The real proof is the diff (zero new fields on `OptoCellSpec`, zero new methods
// on `OptoCell`) and it is in the PR body. What a TEST can assert is that the
// reuse really is the documented one: the source law lives in `LampDrive` and the
// cell is handed LIGHT, which is exactly what `elExponent = 1` means. If a later
// slice "helpfully" restored an EL exponent here, this goes red.
void testOptoCellIsUsedUnwidened() {
    const double fs = 48000.0;
    // Read the exponent off the cell THIS MODEL HOLDS. An earlier version of this
    // bar constructed its own `OptoCell` with `elExponent = 1.0` and asserted on
    // that — which is a bar that cannot fail, and perturbation P7 (putting an EL
    // exponent back on the vibe's cell) walked straight through it. §66's lesson,
    // found again.
    VibeModel v;
    v.prepare(fs, 128);
    std::printf("[seam] the model's own cell: composite exponent p = %.6f "
                "(== the published CdS gamma, i.e. elExponent is the identity)\n",
                v.cellCompositeExponent());
    for (int st = 0; st < VibeModel::numStages(); ++st)
        assert(std::fabs(v.cellCompositeExponent(st) - 0.75) < 1e-12);

    // The lamp supplies the source law, and it is NOT a power of the drive: it is
    // a function of a STATE. Two identical drives after different histories give
    // different light — which is the whole reason the lamp could not have been a
    // `lightSourceKind` case on the cell.
    LampDrive a, b;
    a.configure(LampSpec{});
    b.configure(LampSpec{});
    a.setRate(fs);
    b.setRate(fs);
    a.parkAt(1.0);   // hot
    b.parkAt(0.32);  // cool
    const double la = a.process(0.66), lb = b.process(0.66);
    std::printf("[seam] same 0.66 V drive, different history: light %.6f vs %.6f (%.2fx)\n", la, lb,
                la / lb);
    assert(la > lb * 1.5);
}

// ===========================================================================
//  9. THE VOICE IS AUDIBLE AND SANE
// ===========================================================================
// A guard against the whole thing being technically correct and inaudible: the
// modulation must actually move the signal, and it must move MORE than the Ninety
// does on the same stimulus at the same rate. Also: the pedal loses sweep DEPTH
// at high speed and the Ninety does not — the lamp again.
void testItActuallyMovesAndLosesDepthWithSpeed() {
    const double fs = 48000.0;
    const int n = static_cast<int>(fs * 6.0);
    std::vector<float> in(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = i / fs;
        in[static_cast<size_t>(i)] = static_cast<float>(
            0.12 * (std::sin(2.0 * kPi * 110.0 * t) + std::sin(2.0 * kPi * 220.0 * t) +
                    std::sin(2.0 * kPi * 330.0 * t) + std::sin(2.0 * kPi * 660.0 * t)));
    }
    auto envSwingDb = [&](const std::vector<float>& x) {
        const int w = static_cast<int>(fs * 0.02);
        double lo = 1e9, hi = 0.0;
        for (size_t i = x.size() / 2; i + w < x.size(); i += w) {
            double s = 0.0;
            for (int k = 0; k < w; ++k) s += static_cast<double>(x[i + k]) * x[i + k];
            const double r = std::sqrt(s / w);
            lo = std::min(lo, r);
            hi = std::max(hi, r);
        }
        return 20.0 * std::log10(std::max(1e-12, hi) / std::max(1e-12, lo));
    };

    VibeModel v;
    v.prepare(fs, 128);
    v.setParameter(VibeModel::PARAM_SPEED, 0.4f);
    v.setParameter(VibeModel::PARAM_INTENSITY, 1.0f);
    std::vector<float> vo;
    render(v, in, vo);

    PhaserModel p;
    p.prepare(fs);
    p.setSpeed(phaserKnobForHz(VibeModel::speedKnobToHz(0.4)));
    std::vector<float> po;
    render(p, in, po);

    const double vs = envSwingDb(vo), ps = envSwingDb(po);
    std::printf("[voice] envelope swing on a 4-note chord at %.3f Hz: vibe %.2f dB, ninety %.2f dB\n",
                VibeModel::speedKnobToHz(0.4), vs, ps);
    assert(vs > 3.0);
    assert(vs > ps);
    std::printf("[voice] rms in %.4f  out %.4f (%.2f dB)\n", rms(in), rms(vo),
                20.0 * std::log10(rms(vo) / rms(in)));
    assert(rms(vo) > 0.3 * rms(in) && rms(vo) < 1.2 * rms(in));

    // Sweep DEPTH collapses with SPEED — the lamp cannot keep up. The Ninety's
    // LFO is a number generator and does not care.
    const int block = 64;
    std::vector<float> sil(static_cast<size_t>(block), 0.0f), o(static_cast<size_t>(block));
    double vSlow = 0.0, vFast = 0.0, pSlow = 0.0, pFast = 0.0;
    for (int fast = 0; fast < 2; ++fast) {
        const float sk = fast ? 1.0f : 0.2f;
        const double hz = VibeModel::speedKnobToHz(sk);
        VibeModel vv;
        vv.prepare(fs, 128);
        vv.setParameter(VibeModel::PARAM_SPEED, sk);
        vv.setParameter(VibeModel::PARAM_INTENSITY, 1.0f);
        const SweepTrack t = trackSweep(
            [&] {
                vv.process(sil.data(), o.data(), block);
                return vv.stageCornerHz(0);
            },
            fs, block, std::max(8.0, 8.0 / hz));
        PhaserModel pp;
        pp.prepare(fs);
        pp.setSpeed(phaserKnobForHz(hz));
        const SweepTrack u = trackSweep(
            [&] {
                pp.process(sil.data(), o.data(), block);
                return pp.currentCornerHz();
            },
            fs, block, std::max(8.0, 8.0 / hz));
        (fast ? vFast : vSlow) = t.depthOctaves;
        (fast ? pFast : pSlow) = u.depthOctaves;
    }
    std::printf("[voice] sweep depth slow->fast: vibe %.3f -> %.3f oct  ninety %.3f -> %.3f oct\n",
                vSlow, vFast, pSlow, pFast);
    assert(vSlow / vFast > 2.0);
    assert(pSlow / pFast < 1.05);
}

// ===========================================================================
//  10. THE HOUSE BATTERY
// ===========================================================================
void testBlockSizeInvariance() {
    const double fs = 48000.0;
    const int n = 24000;
    std::vector<float> in(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        in[static_cast<size_t>(i)] = static_cast<float>(
            0.25 * std::sin(2.0 * kPi * 220.0 * i / fs) * std::exp(-1.5 * i / fs));

    auto run = [&](const std::vector<int>& blocks) {
        VibeModel v;
        v.prepare(fs, 256);
        v.setParameter(VibeModel::PARAM_SPEED, 0.5f);
        v.setParameter(VibeModel::PARAM_INTENSITY, 0.8f);
        std::vector<float> out(static_cast<size_t>(n), 0.0f);
        int done = 0, k = 0;
        while (done < n) {
            const int b = std::min(blocks[static_cast<size_t>(k) % blocks.size()], n - done);
            v.process(in.data() + done, out.data() + done, b);
            done += b;
            ++k;
        }
        return out;
    };
    const std::vector<float> a = run({128});
    const std::vector<float> b = run({100, 37, 256, 1, 73});
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(a[static_cast<size_t>(i)]) -
                                          b[static_cast<size_t>(i)]));
    std::printf("[house] ragged-block vs 128: max |diff| = %.3e\n", worst);
    assert(worst == 0.0);
}

void testResetAndNaN() {
    const double fs = 48000.0;
    const int n = 12000;
    std::vector<float> in(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        in[static_cast<size_t>(i)] = static_cast<float>(0.2 * std::sin(2.0 * kPi * 330.0 * i / fs));

    VibeModel fresh, used;
    fresh.prepare(fs, 128);
    used.prepare(fs, 128);
    std::vector<float> hot(static_cast<size_t>(n)), o;
    for (int i = 0; i < n; ++i)
        hot[static_cast<size_t>(i)] = static_cast<float>(0.9 * std::sin(i * 0.01));
    render(used, hot, o);
    used.reset();

    std::vector<float> a, b;
    render(fresh, in, a);
    render(used, in, b);
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(a[static_cast<size_t>(i)]) -
                                          b[static_cast<size_t>(i)]));
    std::printf("[house] reset() vs a fresh model: max |diff| = %.3e\n", worst);
    assert(worst == 0.0);

    // One NaN in. The ABI rejects non-finite PARAMETERS; a non-finite SAMPLE is
    // the recovery seam's job.
    VibeModel v;
    v.prepare(fs, 128);
    std::vector<float> poisoned = in;
    poisoned[100] = std::numeric_limits<float>::quiet_NaN();
    render(v, poisoned, o);
    int bad = 0;
    for (float s : o)
        if (!std::isfinite(s)) ++bad;
    v.reset();
    render(v, in, o);
    int badAfter = 0;
    for (float s : o)
        if (!std::isfinite(s)) ++badAfter;
    std::printf("[house] one NaN poisons %d/%d samples; after reset() %d/%d\n", bad, n, badAfter, n);
    assert(bad > 0);
    assert(badAfter == 0);

    // A non-finite parameter must not reach the model at all.
    v.setParameter(VibeModel::PARAM_SPEED, std::numeric_limits<float>::quiet_NaN());
    v.setParameter(VibeModel::PARAM_INTENSITY, std::numeric_limits<float>::infinity());
    render(v, in, o);
    for (float s : o) assert(std::isfinite(s));
}

void testRestingState() {
    const double fs = 48000.0;
    VibeModel v;
    v.prepare(fs, 128);
    v.setParameter(VibeModel::PARAM_SPEED, 0.5f);
    v.setParameter(VibeModel::PARAM_INTENSITY, 0.8f);
    std::vector<float> in(4800), o;
    for (size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 110.0 * i / fs));
    render(v, in, o);
    std::vector<float> silence(static_cast<size_t>(fs * 6.0), 0.0f);
    render(v, silence, o);
    std::printf("[house] maxAbsRestingState() after 6 s of silence = %.20g\n",
                v.maxAbsRestingState());
    std::printf("[house]   lamp %.1f K, cell %.0f ohms, trap %.4f — all REAL operating points, "
                "deliberately NOT in the figure above\n",
                v.lampTemperatureK(), v.cellResistanceOhms(0), v.cellMemory(0));
    // ADR 006: the four allpass memories rest at EXACTLY zero and are guarded.
    assert(v.maxAbsRestingState() == 0.0);
    // The lamp and the cells do NOT rest at zero, and that is the finding — the
    // same `OptoCell` that `OptoModel` asserts to 0.0 cannot be asserted to 0.0
    // here, because in this pedal the light never goes out.
    assert(v.lampTemperatureK() > 1000.0);
    assert(v.cellResistanceOhms(0) < 1.0e6);
    // ...and the output really is exact digital silence.
    double peak = 0.0;
    for (size_t i = o.size() / 2; i < o.size(); ++i)
        peak = std::max(peak, std::fabs(static_cast<double>(o[i])));
    std::printf("[house] output peak over the last 3 s of silence = %.20g\n", peak);
    assert(peak == 0.0);
}

void testRateIndependence() {
    // The pedal's OWN properties, at four rates. What is asserted is what the
    // circuit fixes: the notch SPREAD and the lower notch. The upper notch's
    // absolute position still moves a little with the rate — the bilinear
    // allpass's own warping, 20x smaller at the shipped 4x than at 1x (docs §67.7).
    const std::vector<double> grid = logGrid(30.0, 16000.0, 240);
    double spLo = 1e9, spHi = -1e9, n1Lo = 1e9, n1Hi = -1e9, n2Lo = 1e9, n2Hi = -1e9;
    for (double fs : {44100.0, 48000.0, 88200.0, 96000.0}) {
        VibeModel v;
        v.prepare(fs, 128);
        v.setParameter(VibeModel::PARAM_SPEED, 0.0f);
        v.setParameter(VibeModel::PARAM_INTENSITY, 0.0f);
        v.setParameter(VibeModel::PARAM_MODE, 0.0f);
        const std::vector<double> db = responseDb(v, fs, grid);
        const std::vector<double> ns = findNotches(grid, db);
        assert(ns.size() >= 2);
        const double sp = std::log2(ns.back() / ns.front());
        std::printf("[rate] fs %6.0f: R %.0f  notches %7.2f / %8.2f  spread %.4f oct\n", fs,
                    v.cellResistanceOhms(0), ns.front(), ns.back(), sp);
        spLo = std::min(spLo, sp);
        spHi = std::max(spHi, sp);
        n1Lo = std::min(n1Lo, ns.front());
        n1Hi = std::max(n1Hi, ns.front());
        n2Lo = std::min(n2Lo, ns.back());
        n2Hi = std::max(n2Hi, ns.back());
    }
    std::printf("[rate] spread range %.4f oct | lower notch %+.3f %% | upper notch %+.3f %%\n",
                spHi - spLo, 100.0 * (n1Hi / n1Lo - 1.0), 100.0 * (n2Hi / n2Lo - 1.0));
    assert(spHi - spLo < 0.10);
    assert(n1Hi / n1Lo - 1.0 < 0.01);
    assert(n2Hi / n2Lo - 1.0 < 0.05);
    // The acceptance bar survives at EVERY rate, which is what actually matters.
    assert(spLo > 5.0);
}

void testDcOnSignal() {
    const double fs = 48000.0;
    for (double offset : {0.0, 0.1}) {
        VibeModel v;
        v.prepare(fs, 128);
        v.setParameter(VibeModel::PARAM_SPEED, 0.5f);
        v.setParameter(VibeModel::PARAM_INTENSITY, 1.0f);
        const int n = static_cast<int>(fs * 2.0);
        std::vector<float> in(static_cast<size_t>(n)), o;
        for (int i = 0; i < n; ++i)
            in[static_cast<size_t>(i)] =
                static_cast<float>(offset + 0.3 * std::sin(2.0 * kPi * 220.0 * i / fs));
        render(v, in, o);
        double sum = 0.0, peak = 0.0;
        const size_t from = o.size() / 2;
        for (size_t i = from; i < o.size(); ++i) {
            sum += o[i];
            peak = std::max(peak, std::fabs(static_cast<double>(o[i])));
        }
        const double mean = sum / static_cast<double>(o.size() - from);
        // The network is LINEAR, so an input offset passes straight through: this
        // model has no coupling caps (a named gap, docs §67.6), exactly as §62
        // records for the CE-1's delay line. Bounded and stated, not hidden.
        std::printf("[house] DC on signal, input offset %.2f V: mean %.6f V = %.4f %% of peak\n",
                    offset, mean, 100.0 * mean / peak);
        assert(std::fabs(mean - offset) < 0.01);
    }
}

void testZipperOnASlam() {
    const double fs = 48000.0;
    clipper::test::StepSlewConfig c;
    c.sampleRate = fs;
    c.amplitude = 0.15;

    VibeModel v;
    const clipper::test::StepSlewResult speed = clipper::test::measureStepSlew(
        c,
        [&] {
            v.prepare(fs, 128);
            v.setParameter(VibeModel::PARAM_SPEED, 0.0f);
            v.setParameter(VibeModel::PARAM_INTENSITY, 1.0f);
        },
        [&] { v.setParameter(VibeModel::PARAM_SPEED, 1.0f); },
        [&](const float* in, float* out, int n) { v.process(in, out, n); });
    std::printf("[house] SPEED min->max mid-render seam/slew = %.2fx\n", speed.ratio);
    assert(speed.ratio < 2.5);

    VibeModel w;
    const clipper::test::StepSlewResult intensity = clipper::test::measureStepSlew(
        c,
        [&] {
            w.prepare(fs, 128);
            w.setParameter(VibeModel::PARAM_SPEED, 0.4f);
            w.setParameter(VibeModel::PARAM_INTENSITY, 1.0f);
        },
        [&] { w.setParameter(VibeModel::PARAM_INTENSITY, 0.0f); },
        [&](const float* in, float* out, int n) { w.process(in, out, n); });
    std::printf("[house] INTENSITY max->min mid-render seam/slew = %.2fx\n", intensity.ratio);
    assert(intensity.ratio < 2.5);
}

}  // namespace

int main() {
    // Unbuffered: an assert() aborts, and a buffered stdout would swallow the
    // measurement that explains WHY it fired.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    clipper::test::requireAssertsLive();

    testStaggeredCaps();
    testCellRangeIsThePublishedOne();
    testNotchDistribution();
    testNotchSpreadIsScaleFree();
    testSweepAsymmetry();
    testAsymmetryIsTheLamp();
    testModeIsAWeightNotASwitch();
    testIntensityZeroIsAStaticNetwork();
    testOversamplingDecision();
    testOptoCellIsUsedUnwidened();
    testItActuallyMovesAndLosesDepthWithSpeed();
    testBlockSizeInvariance();
    testResetAndNaN();
    testRestingState();
    testRateIndependence();
    testDcOnSignal();
    testZipperOnASlam();

    std::printf("clipper_vibe_tests: all M13.5 Uni-Vibe checks passed\n");
    return 0;
}
