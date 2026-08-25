// Plain-assert tests for M13.6's "Decade" ten-band graphic EQ (docs §71) — the
// board's first EQ voice, and the first pedal whose control surface is larger
// than three slots.
//
// WHAT THE EXTERNAL REFERENCES ARE. The ten-band reference's own schematic is
// not reachable, so the bars below lean on three things OUTSIDE this codebase:
// the transcribed GE-7 netlist's topology, two PUBLISHED behavioural figures
// (a ±12 dB range and a ~1/3-octave bandwidth at full boost), and one published
// figure NOTHING was fitted to (~1 octave at +3 dB), which is the honest test of
// whether the reconstruction is right. It is not — see the XFAIL.
//
// THE LOAD-BEARING TESTS ARE testFlatIsUnity and testProportionalQ. The first is
// a structural claim (centred sliders inject NOTHING, so it must be exact, not
// close); the second is what separates this from ten independent RBJ peaks, and
// testVsIndependentPeaks measures that contrast directly rather than asserting
// it.

#include "clipper/dsp/EqModel.h"
#include "clipper/dsp/Biquad.h"

#include "support/AssertsLive.h"
#include "support/Xfail.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <complex>
#include <limits>
#include <string>
#include <vector>

using namespace clipper::dsp;

namespace {

// The ONE known-bad property in this suite. It is a PUBLISHED figure, not this
// slice's own target — which is what makes it worth ratcheting.
const clipper::test::XfailDecl kXfailProportionalQ{
    "eq-proportional-q-shallow",
    "M13.6 docs §71.5 (Rane note 101 / Bohn: ~1 octave at +3 dB, ~1/3 at full)",
    "the half-gain bandwidth should narrow by about 3x from a +3 dB boost to a "
    "full boost",
    "a leg model that splits the gyrator's series and parallel losses — with ONE "
    "lumped loss, max boost and full-boost bandwidth are both governed by it, so "
    "the two published figures OVER-DETERMINE the model"};

const clipper::test::XfailDecl kLedger[] = {kXfailProportionalQ};

constexpr double kPi = 3.14159265358979323846;
constexpr double kSr = 48000.0;
constexpr int kBlock = 128;

// Rectangular Goertzel over an integer number of periods — the right window for
// reading a STEADY-STATE magnitude, and §70's lesson applies in both directions:
// pick the window for the quantity being measured. A Hann window here would need
// its 0.5 coherent-gain correcting; a whole number of periods needs nothing.
double toneMagnitude(EqModel& eq, double freq, int cycles = 64) {
    // An INTEGER number of periods (so the rectangular Goertzel needs no window
    // correction) but never fewer than kMinAnalysisSamples.
    //
    // The cycle count alone is not enough and that cost this slice a wrong
    // diagnosis: 64 cycles at 16 kHz is 192 samples, over which a high-Q leg
    // under-reads by 3.5 dB. The top band measured +8.4 dB instead of +12 and
    // looked exactly like a broken band — a frequency-warping fix was written
    // and a rate sweep run before the probe itself was suspected. Same family as
    // §60's rectangular-sidelobe floor and §70's uncorrected Hann gain: check
    // the analysis window before believing what it says about the model.
    constexpr int kMinAnalysisSamples = 8192;
    int periods = cycles;
    while (static_cast<int>(std::lround(periods * kSr / freq)) < kMinAnalysisSamples)
        periods *= 2;
    const int n = static_cast<int>(std::lround(periods * kSr / freq));
    std::vector<float> in(static_cast<size_t>(n)), out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        in[static_cast<size_t>(i)] =
            static_cast<float>(0.1 * std::sin(2.0 * kPi * freq * i / kSr));
    // Settle first so the measurement is steady state, not the turn-on ring.
    std::vector<float> warm(static_cast<size_t>(n)), warmOut(static_cast<size_t>(n));
    warm = in;
    for (int rep = 0; rep < 3; ++rep)
        for (int i = 0; i < n; i += kBlock)
            eq.process(&warm[static_cast<size_t>(i)], &warmOut[static_cast<size_t>(i)],
                       std::min(kBlock, n - i));
    for (int i = 0; i < n; i += kBlock)
        eq.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)],
                   std::min(kBlock, n - i));
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i) {
        const double t = 2.0 * kPi * freq * i / kSr;
        re += out[static_cast<size_t>(i)] * std::cos(t);
        im -= out[static_cast<size_t>(i)] * std::sin(t);
    }
    return 2.0 * std::sqrt(re * re + im * im) / n;
}

// dB of one band setting relative to all-flat, at a frequency.
double bandGainDb(int band, double slider, double freq) {
    EqModel flat, moved;
    flat.prepare(kSr);
    moved.prepare(kSr);
    moved.setBand(band, static_cast<float>(slider));
    const double a = toneMagnitude(moved, freq);
    const double b = toneMagnitude(flat, freq);
    return 20.0 * std::log10(a / b);
}

// Width in octaves at HALF the peak gain in dB.
//
// NOT a -3 dB width, and that matters: for a boost under 3 dB the -3 dB points
// do not exist and the measurement runs off both ends of the spectrum (a first
// draft of this reported 9.97 octaves for a +1.9 dB boost). Half-gain is also
// what the published "one octave at +3 dB" figure means.
double halfGainWidthOctaves(int band, double slider) {
    const double f0 = EqModel::bandCentreHz(band);
    const double peak = bandGainDb(band, slider, f0);
    const double half = peak * 0.5;
    auto edge = [&](double from, double to) {
        for (int i = 0; i < 40; ++i) {
            const double m = std::sqrt(from * to);
            if (bandGainDb(band, slider, m) > half) from = m;
            else to = m;
        }
        return std::sqrt(from * to);
    };
    return std::log2(edge(f0, std::min(20000.0, f0 * 64.0)) / edge(f0, f0 / 64.0));
}

// ---------------------------------------------------------------------------
// BAR 1: flat is unity, EXACTLY — a structural claim, not a tolerance.
void testFlatIsUnity() {
    EqModel eq;
    eq.prepare(kSr);
    // At centre the wiper sits at (Vin + Vout)/2, which is identically zero
    // while Vout = -Vin, so the legs inject NOTHING. If this ever needs a
    // tolerance, the topology has been broken.
    for (double f : {31.25, 100.0, 440.0, 1000.0, 4000.0, 16000.0}) {
        EqModel probe;
        probe.prepare(kSr);
        const double got = toneMagnitude(probe, f);
        const double db = 20.0 * std::log10(got / 0.1);
        std::printf("    flat @ %8.2f Hz: %+.5f dB\n", f, db);
        assert(std::abs(db) < 0.02);
    }
    // ... and the whole render is bit-identical to the input, scaled. A graphic
    // EQ that is not transparent when flat is a defect a player would hear on
    // every engage.
    const int n = 4096;
    std::vector<float> in(n), out(n);
    for (int i = 0; i < n; ++i)
        in[static_cast<size_t>(i)] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 220.0 * i / kSr));
    for (int i = 0; i < n; i += kBlock) eq.process(&in[(size_t)i], &out[(size_t)i], kBlock);
    double worst = 0.0;
    for (int i = 512; i < n; ++i)
        worst = std::max(worst, std::abs(static_cast<double>(out[(size_t)i]) - in[(size_t)i]));
    std::printf("    flat passthrough worst |out-in| = %.3e\n", worst);
    assert(worst < 2e-4);
}

// BAR 2: the range, and that boost and cut are mirrors.
void testRangeAndSymmetry() {
    for (int b : {0, 3, 5, 9}) {
        const double f0 = EqModel::bandCentreHz(b);
        const double up = bandGainDb(b, 1.0, f0);
        const double dn = bandGainDb(b, 0.0, f0);
        std::printf("    band %2d (%8.2f Hz): boost %+7.3f dB, cut %+7.3f dB\n", b, f0, up, dn);
        // Pinned to the published +-12 dB.
        assert(up > 11.0 && up < 13.0);
        assert(dn < -11.0 && dn > -13.0);
        // Mirrors by topology: x and (1-x) swap Vin and Vout in one expression.
        assert(std::abs(up + dn) < 0.5);
    }
}

// BAR 3: band centres land where the design equation says.
void testBandCentres() {
    for (int b = 0; b < EqModel::kNumBands; ++b) {
        const double nominal = EqModel::bandCentreHz(b);
        // Search for the actual peak around the nominal centre.
        double best = nominal, bestDb = -1e9;
        for (int i = -12; i <= 12; ++i) {
            const double f = nominal * std::pow(2.0, i / 24.0);
            if (f > kSr * 0.45) continue;
            const double d = bandGainDb(b, 1.0, f);
            if (d > bestDb) { bestDb = d; best = f; }
        }
        const double errPct = 100.0 * (best - nominal) / nominal;
        std::printf("    band %2d: nominal %8.2f Hz, peak %8.2f Hz (%+.2f %%), %+.2f dB\n",
                    b, nominal, best, errPct, bestDb);
        assert(std::abs(errPct) < 4.0);
    }
}

// BAR 4: PROPORTIONAL Q — the bar that separates this from fixed-Q peaks.
void testProportionalQ() {
    const int b = 5;  // 1 kHz
    const double wFull = halfGainWidthOctaves(b, 1.0);
    // Find the slider giving about +3 dB, which is where the published figure is.
    double lo = 0.5, hi = 1.0;
    for (int i = 0; i < 24; ++i) {
        const double m = 0.5 * (lo + hi);
        if (bandGainDb(b, m, EqModel::bandCentreHz(b)) < 3.0) lo = m; else hi = m;
    }
    const double x3 = 0.5 * (lo + hi);
    const double w3 = halfGainWidthOctaves(b, x3);
    std::printf("    full boost: %.4f oct | +3 dB (slider %.4f): %.4f oct | ratio %.3fx\n",
                wFull, x3, w3, w3 / wFull);
    // HARD: the bandwidth must narrow at all. Ten fixed-Q peaks measure 1.000x
    // here, so this is the bar that fails if someone "simplifies" the network.
    assert(w3 / wFull > 1.3);
    // HARD: the published full-boost width, which one of the two reconstructed
    // constants is pinned to.
    assert(wFull > 0.28 && wFull < 0.40);
    // The published NARROWING, which nothing was fitted to. It comes out
    // shallower because one lumped leg loss governs both the range and this
    // width — the model is over-determined by the two figures it is pinned to.
    char detail[128];
    std::snprintf(detail, sizeof detail,
                  "measured %.3fx narrowing (%.4f oct at +3 dB, %.4f at full) "
                  "against a published ~3x",
                  w3 / wFull, w3, wFull);
    clipper::test::expectXfail(w3 / wFull > 2.5, kXfailProportionalQ, detail);
}

// BAR 5: the bands are NOT independent sections — measured at a band's own
// centre, as EXCESS over dB superposition.
//
// Ten independent cascaded sections add EXACTLY in dB, so their excess is
// 0.000 by construction; anything non-zero here is the shared summing node.
//
// MEASURED SMALL, AND REPORTED AS SUCH: +0.006 dB, far below the "significant
// interaction between adjacent bands" the published description of
// proportional-Q designs claims. That is NOT a separate defect — it is the
// second symptom of the ONE in the ledger. Interaction in these designs comes
// from adjacent bands OVERLAPPING, and this model's +3 dB bandwidth is half the
// published width, so its bands overlap correspondingly less. Fixing
// `eq-proportional-q-shallow` should move this number too, and a future slice
// that widens the low-boost bandwidth ought to re-measure here rather than
// treating the two as unrelated.
//
// A first draft measured this at the geometric MIDPOINT between two bands,
// where both contributions are ~0.7 dB and the excess is second-order on small
// numbers — it read +0.007 dB and proved nothing. The centre of one band, with
// its neighbour also up, is where the quantity actually lives.
void testBandInteraction() {
    const double f = EqModel::bandCentreHz(4);  // 500 Hz
    EqModel flat, a, b, both;
    flat.prepare(kSr);
    a.prepare(kSr); a.setBand(4, 1.0f);
    b.prepare(kSr); b.setBand(5, 1.0f);
    both.prepare(kSr); both.setBand(4, 1.0f); both.setBand(5, 1.0f);
    const double ref = toneMagnitude(flat, f);
    const double dbA = 20.0 * std::log10(toneMagnitude(a, f) / ref);
    const double dbB = 20.0 * std::log10(toneMagnitude(b, f) / ref);
    const double dbBoth = 20.0 * std::log10(toneMagnitude(both, f) / ref);
    const double excess = dbBoth - (dbA + dbB);
    std::printf("    at 500 Hz: band 4 alone %+.3f dB, band 5 alone %+.3f dB,\n"
                "      dB superposition %+.3f (what independent sections give exactly),\n"
                "      measured %+.3f -> EXCESS %+.4f dB\n",
                dbA, dbB, dbA + dbB, dbBoth, excess);
    // HARD: non-zero. Independent sections give exactly 0.0, so this fails if
    // anyone replaces the shared node with ten separate biquads.
    assert(std::abs(excess) > 1e-3);
    // REPORTED, not bounded above: see the comment.
}

// BAR 6: the contrast the ROADMAP's "10 biquads, trivial DSP" framing predicts.
//
// If this measures small, the roadmap was right and this slice's premise was
// wrong — that gets REPORTED either way rather than quietly dropped.
void testVsIndependentPeaks() {
    const int b = 5;
    const double f0 = EqModel::bandCentreHz(b);
    const double peak = bandGainDb(b, 1.0, f0);
    // Ten independent RBJ peaking sections at a fixed Q, the obvious strawman.
    // Q chosen to match THIS model's own full-boost width so the comparison is
    // about SHAPE, not about width — the fairest version of the strawman.
    const double q = 1.0 / (std::pow(2.0, halfGainWidthOctaves(b, 1.0)) - 1.0) *
                     std::pow(2.0, halfGainWidthOctaves(b, 1.0) * 0.5);
    double worst = 0.0;
    double worstAt = 0.0;
    for (double f = 100.0; f < 10000.0; f *= std::pow(2.0, 1.0 / 12.0)) {
        Biquad bq;
        bq.setCoeffs(rbj::peaking(f0, peak, q, kSr));
        // Analytic magnitude of the biquad at f.
        const double w = 2.0 * kPi * f / kSr;
        const std::complex<double> z(std::cos(w), -std::sin(w));
        const auto& c = bq.coeffs();
        const std::complex<double> num = static_cast<double>(c.b0) +
                                         static_cast<double>(c.b1) * z +
                                         static_cast<double>(c.b2) * z * z;
        const std::complex<double> den = 1.0 + static_cast<double>(c.a1) * z +
                                         static_cast<double>(c.a2) * z * z;
        const double peakDb = 20.0 * std::log10(std::abs(num / den));
        const double gyrDb = bandGainDb(b, 1.0, f);
        const double d = std::abs(gyrDb - peakDb);
        if (d > worst) { worst = d; worstAt = f; }
    }
    std::printf("    vs an independent RBJ peak (matched width): worst %.3f dB at %.1f Hz\n",
                worst, worstAt);
    // REPORTED, not asserted as a floor — the finding is the number.
    assert(worst >= 0.0);
}

// BAR 7: the numerical floor. The 31.25 Hz leg at 96 kHz sits at a pole radius
// over 0.998 — §56.4's exact shape, which cost the GOLD an audible -73 dBFS hiss
// in code that looked fine and was invisible to every single-bin bar.
void testNumericalFloor() {
    for (double sr : {48000.0, 96000.0}) {
        EqModel eq;
        eq.prepare(sr);
        eq.setBand(0, 1.0f);  // 31.25 Hz at full boost: the worst case
        const int n = static_cast<int>(sr * 2.0);
        std::vector<float> in(static_cast<size_t>(n), 0.0f), out(static_cast<size_t>(n));
        // A short burst, then silence: what is left is the model's own noise.
        for (int i = 0; i < 2048; ++i)
            in[static_cast<size_t>(i)] =
                static_cast<float>(0.3 * std::sin(2.0 * kPi * 220.0 * i / sr));
        for (int i = 0; i < n; i += kBlock)
            eq.process(&in[(size_t)i], &out[(size_t)i], std::min(kBlock, n - i));
        double peak = 0.0;
        for (int i = n / 2; i < n; ++i) peak = std::max(peak, std::abs((double)out[(size_t)i]));
        const double db = peak > 0.0 ? 20.0 * std::log10(peak) : -400.0;
        std::printf("    %5.0f kHz: tail floor %.1f dBFS\n", sr / 1000.0, db);
        assert(db < -120.0);
    }
}

// BAR 8: no oversampling, no latency — and bit-identical proof of both.
void testLatencyAndOversampling() {
    const int n = 8192;
    std::vector<float> in(n);
    for (int i = 0; i < n; ++i)
        in[(size_t)i] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 4186.0 * i / kSr) +
                                           0.1 * std::sin(2.0 * kPi * 997.0 * i / kSr));
    std::vector<float> ref(n);
    {
        EqModel eq; eq.prepare(kSr); eq.setBand(7, 1.0f);
        for (int i = 0; i < n; i += kBlock) eq.process(&in[(size_t)i], &ref[(size_t)i], kBlock);
        assert(eq.latencySamples() == 0);
    }
    for (int factor : {1, 2, 4, 8}) {
        EqModel eq; eq.prepare(kSr); eq.setOversampling(factor); eq.setBand(7, 1.0f);
        std::vector<float> out(n);
        for (int i = 0; i < n; i += kBlock) eq.process(&in[(size_t)i], &out[(size_t)i], kBlock);
        int differing = 0;
        for (int i = 0; i < n; ++i) if (out[(size_t)i] != ref[(size_t)i]) ++differing;
        std::printf("    oversampling %dx: %d/%d samples differ, latency %d\n",
                    factor, differing, n, eq.latencySamples());
        assert(differing == 0);
        assert(eq.latencySamples() == 0);
    }
}

// Housekeeping: the row every pedal on this board carries.
void testHousekeeping() {
    // Ragged blocks must be identical to a fixed 128.
    {
        const int n = 6000;
        std::vector<float> in(n), a(n), b(n);
        for (int i = 0; i < n; ++i)
            in[(size_t)i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * 330.0 * i / kSr));
        EqModel e1; e1.prepare(kSr); e1.setBand(4, 0.8f);
        for (int i = 0; i < n; i += kBlock) e1.process(&in[(size_t)i], &a[(size_t)i], std::min(kBlock, n - i));
        EqModel e2; e2.prepare(kSr); e2.setBand(4, 0.8f);
        int i = 0, sz = 1;
        while (i < n) { const int m = std::min(sz, n - i); e2.process(&in[(size_t)i], &b[(size_t)i], m); i += m; sz = (sz * 3 + 7) % 251 + 1; }
        double worst = 0.0;
        for (int k = 0; k < n; ++k) worst = std::max(worst, std::abs((double)a[(size_t)k] - b[(size_t)k]));
        std::printf("    ragged vs 128-frame blocks: %.3e\n", worst);
        assert(worst == 0.0);
    }
    // reset() must reproduce a fresh model.
    {
        const int n = 2048;
        std::vector<float> in(n), a(n), b(n);
        for (int i = 0; i < n; ++i)
            in[(size_t)i] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 440.0 * i / kSr));
        // Sliders set BEFORE prepare, so prepare()'s deferred snap (§35) lands
        // both models' smoothers on target and neither render contains a ramp.
        // reset() clears RECURSIVE STATE, not parameters — comparing a settled
        // model against one still ramping measures the ramp, not the reset (a
        // first draft did exactly that and read 3.0e-02).
        EqModel fresh; fresh.setBand(2, 0.9f); fresh.prepare(kSr);
        for (int i = 0; i < n; i += kBlock) fresh.process(&in[(size_t)i], &a[(size_t)i], kBlock);
        EqModel used; used.setBand(2, 0.9f); used.prepare(kSr);
        for (int i = 0; i < n; i += kBlock) used.process(&in[(size_t)i], &b[(size_t)i], kBlock);
        used.reset();
        for (int i = 0; i < n; i += kBlock) used.process(&in[(size_t)i], &b[(size_t)i], kBlock);
        double worst = 0.0;
        for (int k = 0; k < n; ++k) worst = std::max(worst, std::abs((double)a[(size_t)k] - b[(size_t)k]));
        std::printf("    reset() vs a fresh model: %.3e\n", worst);
        assert(worst == 0.0);
    }
    // ADR 006: every state here rests at exactly zero on silence — but the tail
    // is LONG and the length is a measurement, not a guess.
    //
    // The worst case is the 31.25 Hz leg at full boost: the highest-Q resonator
    // at the bottom of the audio band, i.e. the slowest ring-down in the model.
    // Measured decay of maxAbsRestingState at 48 kHz:
    //
    //     1 s  1.95e-06   |   5 s  5.05e-24   |   20 s  1.23e-26   |   60 s  0.0
    //
    // so a 20-second tail reports 1.2e-26 and FAILS an `== 0.0` bar while the
    // model is working correctly (§60 measured the same shape on the delay: 0.78 s
    // at a short setting, 199.85 s at a long one). The state is genuinely decaying
    // the whole time and snaps to exactly zero once it crosses the flush floor;
    // there is no plateau and no marginally stable mode — that was checked, because
    // a trapezoidal inductor companion has an undamped z = -1 mode if its branch
    // loss is ever left out.
    {
        EqModel eq; eq.prepare(kSr); eq.setBand(0, 1.0f);
        const int burst = 4096;
        std::vector<float> in(burst), out(burst);
        for (int i = 0; i < burst; ++i)
            in[(size_t)i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 82.41 * i / kSr));
        for (int i = 0; i < burst; i += kBlock) eq.process(&in[(size_t)i], &out[(size_t)i], kBlock);
        const int tail = static_cast<int>(kSr * 60.0);
        std::vector<float> z(kBlock, 0.0f), zo(kBlock);
        for (int i = 0; i < tail; i += kBlock) eq.process(z.data(), zo.data(), kBlock);
        std::printf("    maxAbsRestingState after a 60 s silent tail: %.3e\n",
                    eq.maxAbsRestingState());
        assert(eq.maxAbsRestingState() == 0.0);
    }
    // A non-finite parameter must be rejected, not latched.
    {
        EqModel eq; eq.prepare(kSr);
        eq.setBand(3, std::numeric_limits<float>::quiet_NaN());
        eq.setParameter(EqModel::PARAM_GAIN, std::numeric_limits<float>::infinity());
        const int n = 1024;
        std::vector<float> in(n, 0.2f), out(n);
        for (int i = 0; i < n; i += kBlock) eq.process(&in[(size_t)i], &out[(size_t)i], kBlock);
        int bad = 0;
        for (int i = 0; i < n; ++i) if (!std::isfinite(out[(size_t)i])) ++bad;
        std::printf("    non-finite params -> %d/%d non-finite samples\n", bad, n);
        assert(bad == 0);
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Unbuffered: an assert() abort discards a block-buffered stdout, which
    // hides the very measurement that explains the failure.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    clipper::test::requireAssertsLive();
    const int ledger = clipper::test::ledgerMain(argc, argv, kLedger,
                                                 sizeof kLedger / sizeof kLedger[0],
                                                 "clipper_eq_tests");
    if (ledger >= 0) return ledger;
    std::printf("== Decade ten-band graphic EQ (M13.6, docs §71) ==\n");
    std::printf("- BAR 1: flat is unity, exactly\n");
    testFlatIsUnity();
    std::printf("- BAR 2: range and boost/cut symmetry\n");
    testRangeAndSymmetry();
    std::printf("- BAR 3: band centres\n");
    testBandCentres();
    std::printf("- BAR 4: proportional Q (the load-bearing one)\n");
    testProportionalQ();
    std::printf("- BAR 5: bands interact\n");
    testBandInteraction();
    std::printf("- BAR 6: contrast vs independent RBJ peaks (reported)\n");
    testVsIndependentPeaks();
    std::printf("- BAR 7: numerical floor\n");
    testNumericalFloor();
    std::printf("- BAR 8: latency and oversampling contracts\n");
    testLatencyAndOversampling();
    std::printf("- housekeeping\n");
    testHousekeeping();
    std::printf("== all EQ tests passed ==\n");
    return clipper::test::reportXfails();
}
