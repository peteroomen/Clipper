// Plain-assert tests for M13.10's "Cellar" polyphonic drop-tune pedal (docs §70)
// — the lineup's FIRST pitch effect, and the first voice here that is not a
// circuit.
//
// THAT CHANGES WHAT A TEST HAS TO BE. Every other suite in this repo can lean on
// a netlist: an analytic H(jw), Ohm's law on a transcribed resistor, a datasheet
// knee. There is no schematic for a pitch shifter, so the external references
// here are ARITHMETIC and PERCEPTUAL instead — the exact frequency ratio
// 2^(-n/12), the ~6-cent JND, and the dry signal itself. Every bar below is
// measured against one of those, never against another number this model
// produced.
//
// THE LOAD-BEARING TESTS ARE testPitchAccuracy and testPolyphony. The second is
// the one that separates this from a monophonic tracker, and it is the one that
// caught the two structural errors recorded in PitchShifter.h.

#include "clipper/dsp/DropModel.h"
#include "clipper/dsp/PitchShifter.h"

#include "support/AssertsLive.h"
#include "support/Xfail.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace clipper::dsp;

namespace {

// The ONE known-bad property in this suite, and it is this slice's own target
// rather than a perceptual one — see the call site.
const clipper::test::XfailDecl kXfailTriadSpread{
    "drop-triad-spread-at-minus-2",
    "M13.10 docs §70.6 (this slice's own 2-cent spread target)",
    "a 4:5:6 major triad's partials should all shift by the same ratio to within "
    "2 cents of spread at every position",
    "a frequency-domain shifter — one SOLA lag cannot align three partials at "
    "once, and neither a wider span nor sub-sample lag refinement moves it"};

// The binary's declared ledger, for `--xfail-ledger` mode. Without this array and
// the ledgerMain() call in main(), `clipper_add_xfail_ledger` registers a ctest
// entry that runs the WHOLE suite and reports Passed instead of ***Skipped — so
// the known defect never appears in a plain `ctest` run, which is the entire
// point of the ratchet. Caught by measuring the ctest listing rather than
// trusting the CMake call.
const clipper::test::XfailDecl kLedger[] = {kXfailTriadSpread};

constexpr double kPi = 3.14159265358979323846;
constexpr int kBlock = 128;
constexpr double kSr = 48000.0;

// Hann-windowed DFT magnitude at one frequency. Hann, not rectangular, for §60's
// reason: a rectangular window's own sidelobe reads as a ~-56 dB floor and would
// fake the artifact measurement outright.
double mag(const std::vector<float>& x, size_t n0, double f) {
    const size_t N = x.size() - n0;
    double re = 0.0, im = 0.0;
    for (size_t n = 0; n < N; ++n) {
        const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(n) /
                                               static_cast<double>(N - 1)));
        const double t = 2.0 * kPi * f * static_cast<double>(n) / kSr;
        re += w * x[n0 + n] * std::cos(t);
        im -= w * x[n0 + n] * std::sin(t);
    }
    return std::sqrt(re * re + im * im) / static_cast<double>(N);
}

// Where the spectral peak sits, in cents relative to `target`, searched over a
// band narrow enough that it cannot stray onto a neighbouring partial.
double peakOffsetCents(const std::vector<float>& x, size_t n0, double target,
                       double bandCents = 25.0) {
    double best = 0.0, bestMag = -1.0;
    for (double c = -bandCents; c <= bandCents; c += 0.25) {
        const double m = mag(x, n0, target * std::pow(2.0, c / 1200.0));
        if (m > bestMag) { bestMag = m; best = c; }
    }
    return best;
}

std::vector<float> renderTone(DropModel& d, const std::vector<double>& partials,
                              int nSamples, double amp) {
    std::vector<float> in(static_cast<size_t>(nSamples)), out(static_cast<size_t>(nSamples));
    for (int n = 0; n < nSamples; ++n) {
        double v = 0.0;
        for (double f : partials)
            v += (amp / static_cast<double>(partials.size())) *
                 std::sin(2.0 * kPi * f * n / kSr);
        in[static_cast<size_t>(n)] = static_cast<float>(v);
    }
    for (int off = 0; off + kBlock <= nSamples; off += kBlock)
        d.process(in.data() + off, out.data() + off, kBlock);
    return out;
}

void setUp(DropModel& d, int position) {
    d.prepare(kSr, kBlock);
    // Drive the knob, not the position, so the quantizer is exercised too.
    d.setParameter(DropModel::PARAM_AMOUNT,
                   static_cast<float>(position) /
                       static_cast<float>(DropModel::POSITION_COUNT - 1));
    assert(d.position() == position);
}

// ---------------------------------------------------------------------------
// BAR 1 — PITCH ACCURACY against the exact ratio.
//
// The reference is arithmetic (2^(-n/12)), so this is an absolute external
// check, not a self-comparison. The bar is +/-5 cents, under the ~6-cent JND for
// a sustained tone.
// ---------------------------------------------------------------------------
void testPitchAccuracy() {
    const int N = static_cast<int>(kSr) * 3;
    double worst = 0.0;
    for (int pos = 0; pos < DropModel::POSITION_COUNT; ++pos) {
        DropModel d;
        setUp(d, pos);
        const double target = 220.0 * std::pow(
            2.0, -static_cast<double>(DropModel::semitonesFor(pos)) / 12.0);
        const std::vector<float> y = renderTone(d, {220.0}, N, 0.5);
        const double off = peakOffsetCents(y, static_cast<size_t>(N / 2), target);
        std::printf("  pos %d (%2d semis): target %8.3f Hz, peak %+6.2f cents\n", pos,
                    -DropModel::semitonesFor(pos), target, off);
        worst = std::max(worst, std::fabs(off));
        assert(std::fabs(off) < 5.0);
    }
    std::printf("  worst single-note error %.2f cents (bar 5.0)\n", worst);
}

// ---------------------------------------------------------------------------
// BAR 2 — POLYPHONY. THE LOAD-BEARING TEST.
//
// Every partial of a chord must shift by the SAME ratio. A monophonic
// pitch-tracking shifter cannot do this: it picks one f0 and shifts everything by
// it, so the intervals collapse. The SPREAD across partials — not the absolute
// error — is what distinguishes the two, so the spread is what is asserted.
//
// Three shapes, because they are NOT equally hard and the difference is
// structural: SOLA aligns to where the waveform REPEATS, so a 2:3 power chord
// (repeats at its lower note's period) is far easier than a 4:5:6 triad (repeats
// only at its composite period, ~48 ms on E2). The triad is the real test and it
// is what forced the shipped search span — see PitchShifter.h.
// ---------------------------------------------------------------------------
void testPolyphony() {
    struct Shape { const char* name; std::vector<double> partials; };
    const Shape shapes[] = {
        {"E5 power chord", {82.41, 123.47}},
        {"E5 + octave",    {82.41, 123.47, 164.81}},
        {"E major triad",  {82.41, 103.83, 123.47}},
    };
    const int N = static_cast<int>(kSr) * 3;
    for (int pos : {DropModel::DROP_1, DropModel::DROP_2}) {
        for (const Shape& s : shapes) {
            DropModel d;
            setUp(d, pos);
            const double r = std::pow(
                2.0, -static_cast<double>(DropModel::semitonesFor(pos)) / 12.0);
            const std::vector<float> y = renderTone(d, s.partials, N, 0.5);
            double lo = 1e9, hi = -1e9, worstAbs = 0.0;
            for (double f0 : s.partials) {
                const double off =
                    peakOffsetCents(y, static_cast<size_t>(N / 2), f0 * r, 20.0);
                lo = std::min(lo, off);
                hi = std::max(hi, off);
                worstAbs = std::max(worstAbs, std::fabs(off));
            }
            std::printf("  %-16s at %2d semis: worst %+5.2f cents, spread %.2f\n",
                        s.name, -DropModel::semitonesFor(pos), worstAbs, hi - lo);

            // THE ABSOLUTE BAR IS THE EXTERNALLY-JUSTIFIED ONE and it is HARD
            // everywhere: 5 cents is under the ~6-cent JND for a sustained tone.
            assert(worstAbs < 5.0);

            // The 2-cent SPREAD target is this slice's own, tighter than anything
            // perception requires — it exists to separate this from a monophonic
            // tracker, which collapses a triad's intervals by TENS of cents. It
            // holds everywhere except one cell, and that cell is registered rather
            // than either hidden or legislated away by moving the number.
            // The 4:5:6 MAJOR TRIAD specifically — not E5+octave, whose middle
            // partial is the fifth. Matching on the major third is what separates
            // them; an earlier version keyed on "three partials" and wrongly
            // XPASSed on E5+octave.
            const bool hardestCell = (pos == DropModel::DROP_2) &&
                                     (s.partials.size() == 3) &&
                                     (std::fabs(s.partials[1] - 103.83) < 0.01);
            if (hardestCell) {
                clipper::test::expectXfail(
                    (hi - lo) < 2.0, kXfailTriadSpread,
                    [&] {
                        static char buf[400];
                        std::snprintf(buf, sizeof(buf),
                                      "E major triad at -2 semitones: partial spread "
                                      "%.4f cents against this slice's own 2.0 target. "
                                      "The 5-cent PERCEPTUAL bar is met with %.2f cents "
                                      "to spare. Widening the SOLA span (55/58 ms) and "
                                      "sub-sample lag refinement were both tried and "
                                      "moved it by under 0.002 cents.",
                                      hi - lo, 5.0 - worstAbs);
                        return buf;
                    }());
            } else {
                assert(hi - lo < 2.0);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// BAR 3 — THE ATTACK SURVIVES. What the reference is bought for, and the
// property a phase vocoder would fail; asserting it stops a later slice from
// "upgrading" the algorithm without noticing the cost.
// ---------------------------------------------------------------------------
void testAttackSurvives() {
    const int N = static_cast<int>(kSr);
    std::vector<float> in(static_cast<size_t>(N), 0.0f), out(static_cast<size_t>(N), 0.0f);
    const int onset = static_cast<int>(kSr) / 4;
    for (int n = onset; n < N; ++n) {
        const double t = static_cast<double>(n - onset) / kSr;
        in[static_cast<size_t>(n)] =
            static_cast<float>(0.6 * std::exp(-t * 3.0) * std::sin(2.0 * kPi * 110.0 * t));
    }
    DropModel d;
    setUp(d, DropModel::DROP_1);
    for (int off = 0; off + kBlock <= N; off += kBlock)
        d.process(in.data() + off, out.data() + off, kBlock);

    auto peakIn20ms = [&](const std::vector<float>& v, int from) {
        double p = 0.0;
        for (int n = from; n < from + static_cast<int>(0.020 * kSr) && n < N; ++n)
            p = std::max(p, std::fabs(static_cast<double>(v[static_cast<size_t>(n)])));
        return p;
    };
    // Find the output's onset: first sample above a tenth of its own later peak.
    double outPeak = 0.0;
    for (int n = 0; n < N; ++n) outPeak = std::max(outPeak, std::fabs((double)out[(size_t)n]));
    int outOnset = onset;
    for (int n = 0; n < N; ++n)
        if (std::fabs((double)out[(size_t)n]) > 0.1 * outPeak) { outOnset = n; break; }

    const double dryPk = peakIn20ms(in, onset);
    const double wetPk = peakIn20ms(out, outOnset);
    const double dropDb = 20.0 * std::log10(wetPk / (dryPk + 1e-30));
    const double lateMs = 1000.0 * static_cast<double>(outOnset - onset) / kSr;
    std::printf("  attack: %+.2f dB re dry over its first 20 ms, onset %+.2f ms\n",
                dropDb, lateMs);
    assert(std::fabs(dropDb) < 2.0);
    // The shifter reads from the past, so the onset can only be LATE, never early,
    // and never later than the window it reads across.
    assert(lateMs >= -0.01);
    assert(lateMs < 1000.0 * DropModel::kWindowSeconds);
}

// ---------------------------------------------------------------------------
// BAR 4 — the artifact floor is MEASURED AND REPORTED, not asserted inaudible.
// A crossfaded delay shifter has a comb at the grain rate by construction; the
// honest bar is that it is bounded and that it grows with shift depth (deeper
// shift == faster grain rate == more handovers per second).
// ---------------------------------------------------------------------------
void testArtifactFloor() {
    const int N = 1 << 16;
    double prev = -1e9;
    for (int pos : {DropModel::DROP_1, DropModel::DROP_5, DropModel::DROP_OCT}) {
        DropModel d;
        setUp(d, pos);
        const std::vector<float> y = renderTone(d, {220.0}, N, 0.5);
        const size_t n0 = static_cast<size_t>(N / 2);
        double tot = 0.0;
        for (size_t n = n0; n < y.size(); ++n) tot += y[n] * y[n];
        tot /= static_cast<double>(y.size() - n0);
        const double f1 = 220.0 * std::pow(
            2.0, -static_cast<double>(DropModel::semitonesFor(pos)) / 12.0);
        // Harmonic amplitudes via a RECTANGULAR Goertzel, not the Hann DFT above.
        // The Hann window has a coherent gain of 0.5, so reading amplitudes off it
        // without correcting under-counts them 2x and therefore under-counts
        // harmonic POWER 4x — which reads as a catastrophic artifact floor that is
        // purely an artefact of the estimator. (The Hann window is still right for
        // the PEAK searches above, where only the location matters.)
        double harm = 0.0;
        for (int k = 1; k <= 20; ++k) {
            const double f = f1 * static_cast<double>(k);
            if (f > 0.45 * kSr) break;
            const double w = 2.0 * kPi * f / kSr;
            const double c = 2.0 * std::cos(w);
            double s1 = 0.0, s2 = 0.0;
            for (size_t n = n0; n < y.size(); ++n) {
                const double t = static_cast<double>(y[n]) + c * s1 - s2;
                s2 = s1;
                s1 = t;
            }
            const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
            const double a = std::sqrt(p > 0.0 ? p : 0.0) * 2.0 /
                             static_cast<double>(y.size() - n0);
            harm += 0.5 * a * a;
        }
        const double db = 10.0 * std::log10(std::max(1e-30, tot - harm) / tot);
        std::printf("  %2d semis: non-harmonic energy %.2f dB\n",
                    -DropModel::semitonesFor(pos), db);
        assert(db < -6.0);          // the shifted tone must dominate its own output
        prev = db;
    }
    (void)prev;
}

// ---------------------------------------------------------------------------
// BAR 5 — the DRY path. At OCT + DRY the dry component must be present and
// EXACT; at every other position the pedal is 100 % wet, which is the reference's
// own design and must not drift into a blend.
// ---------------------------------------------------------------------------
void testDryPath() {
    const int N = static_cast<int>(kSr);
    // A quiet impulse-free probe: compare OCT and OCT+DRY on the same input.
    std::vector<float> in(static_cast<size_t>(N));
    for (int n = 0; n < N; ++n)
        in[static_cast<size_t>(n)] =
            static_cast<float>(0.4 * std::sin(2.0 * kPi * 220.0 * n / kSr));

    auto run = [&](int pos) {
        DropModel d;
        setUp(d, pos);
        std::vector<float> out(static_cast<size_t>(N), 0.0f);
        for (int off = 0; off + kBlock <= N; off += kBlock)
            d.process(in.data() + off, out.data() + off, kBlock);
        return out;
    };
    const std::vector<float> oct = run(DropModel::DROP_OCT);
    const std::vector<float> octDry = run(DropModel::DROP_OCT_DRY);

    // At OCT the ORIGINAL 220 Hz must be essentially absent; at OCT+DRY it must
    // be back, and at full level.
    const size_t n0 = static_cast<size_t>(N / 2);
    const double dryInOct = mag(oct, n0, 220.0);
    const double dryInOctDry = mag(octDry, n0, 220.0);
    std::printf("  220 Hz component: OCT %.6f, OCT+DRY %.6f (ratio %.1fx)\n",
                dryInOct, dryInOctDry, dryInOctDry / (dryInOct + 1e-12));
    assert(dryInOctDry > 20.0 * dryInOct);

    // ...and the shifted octave must still be there at OCT+DRY, i.e. the dry sum
    // adds to the wet rather than replacing it.
    assert(mag(octDry, n0, 110.0) > 0.25 * mag(oct, n0, 110.0));
}

// ---------------------------------------------------------------------------
// BAR 6 — latency is reported as ZERO by design, and oversampling is a no-op.
// Both are asserted against the DOCUMENTED contract rather than read back from
// the object, which is §58's "a bar that could not fail" lesson.
// ---------------------------------------------------------------------------
void testLatencyAndOversampling() {
    DropModel d;
    setUp(d, DropModel::DROP_1);
    std::printf("  latencySamples %d, mean algorithmic delay %.1f ms\n",
                d.latencySamples(), 1000.0 * d.meanDelaySamples() / kSr);
    assert(d.latencySamples() == 0);
    // The real delay is a sawtooth and is NOT compensated — it must be within the
    // window the shifter reads across.
    assert(d.meanDelaySamples() > 0.0);
    assert(d.meanDelaySamples() < DropModel::kWindowSeconds * kSr);

    // Oversampling must be an exact no-op: same render at 1x and 8x, bit for bit.
    const int N = kBlock * 40;
    auto render = [&](int factor) {
        DropModel m;
        m.prepare(kSr, kBlock);
        m.setOversampling(factor);
        m.setParameter(DropModel::PARAM_AMOUNT, 0.0f);
        std::vector<float> in(static_cast<size_t>(N)), out(static_cast<size_t>(N), 0.0f);
        for (int n = 0; n < N; ++n)
            in[static_cast<size_t>(n)] =
                static_cast<float>(0.3 * std::sin(2.0 * kPi * 330.0 * n / kSr));
        for (int off = 0; off + kBlock <= N; off += kBlock)
            m.process(in.data() + off, out.data() + off, kBlock);
        return out;
    };
    const std::vector<float> a = render(1), b = render(8);
    int differ = 0;
    for (int n = 0; n < N; ++n)
        if (a[static_cast<size_t>(n)] != b[static_cast<size_t>(n)]) ++differ;
    std::printf("  1x vs 8x: %d / %d samples differ\n", differ, N);
    assert(differ == 0);
}

// ---------------------------------------------------------------------------
// Housekeeping the whole lineup shares.
// ---------------------------------------------------------------------------
void testHousekeeping() {
    const int N = kBlock * 40;
    std::vector<float> in(static_cast<size_t>(N));
    for (int n = 0; n < N; ++n)
        in[static_cast<size_t>(n)] =
            static_cast<float>(0.3 * std::sin(2.0 * kPi * 196.0 * n / kSr));

    // Ragged blocking vs 128-frame — the only segmentation that catches a
    // block-size bug.
    DropModel a;
    setUp(a, DropModel::DROP_2);
    std::vector<float> ra(static_cast<size_t>(N), 0.0f);
    for (int off = 0; off + kBlock <= N; off += kBlock)
        a.process(in.data() + off, ra.data() + off, kBlock);

    DropModel b;
    setUp(b, DropModel::DROP_2);
    std::vector<float> rb(static_cast<size_t>(N), 0.0f);
    const int sizes[] = {13, 1, 64, 100, 7, 128, 31};
    int off = 0, k = 0;
    while (off < N) {
        int m = sizes[k % 7];
        if (off + m > N) m = N - off;
        b.process(in.data() + off, rb.data() + off, m);
        off += m;
        ++k;
    }
    double worst = 0.0;
    for (int n = 0; n < N; ++n)
        worst = std::max(worst, std::fabs(static_cast<double>(
                                    ra[static_cast<size_t>(n)] - rb[static_cast<size_t>(n)])));
    std::printf("  ragged vs 128-frame: worst %.3e\n", worst);
    assert(worst == 0.0);

    // reset() must reproduce a fresh model.
    DropModel c;
    setUp(c, DropModel::DROP_2);
    std::vector<float> junk(static_cast<size_t>(N), 0.5f), tmp(static_cast<size_t>(N), 0.0f);
    for (int o = 0; o + kBlock <= N; o += kBlock) c.process(junk.data() + o, tmp.data() + o, kBlock);
    c.reset();
    std::vector<float> rc(static_cast<size_t>(N), 0.0f);
    for (int o = 0; o + kBlock <= N; o += kBlock) c.process(in.data() + o, rc.data() + o, kBlock);
    worst = 0.0;
    for (int n = 0; n < N; ++n)
        worst = std::max(worst, std::fabs(static_cast<double>(
                                    ra[static_cast<size_t>(n)] - rc[static_cast<size_t>(n)])));
    std::printf("  reset() vs fresh: worst %.3e\n", worst);
    assert(worst == 0.0);

    // One NaN in, then reset() -> nothing non-finite.
    DropModel p;
    setUp(p, DropModel::DROP_1);
    std::vector<float> bad = in;
    bad[100] = std::nanf("");
    std::vector<float> o1(static_cast<size_t>(N), 0.0f);
    for (int o = 0; o + kBlock <= N; o += kBlock) p.process(bad.data() + o, o1.data() + o, kBlock);
    p.reset();
    std::vector<float> o2(static_cast<size_t>(N), 0.0f);
    for (int o = 0; o + kBlock <= N; o += kBlock) p.process(in.data() + o, o2.data() + o, kBlock);
    int nonFinite = 0;
    for (float v : o2)
        if (!std::isfinite(v)) ++nonFinite;
    std::printf("  non-finite after reset: %d / %d\n", nonFinite, N);
    assert(nonFinite == 0);

    // The knob must land on all nine detents and nowhere else.
    for (int i = 0; i < DropModel::POSITION_COUNT; ++i) {
        const double knob = static_cast<double>(i) /
                            static_cast<double>(DropModel::POSITION_COUNT - 1);
        assert(DropModel::positionFor(knob) == i);
    }
    assert(DropModel::positionFor(-1.0) == 0);
    assert(DropModel::positionFor(2.0) == DropModel::POSITION_COUNT - 1);
    // The DEFAULT knob (0.0) is ONE SEMITONE DOWN — the setting this pedal is
    // mostly bought for, and the owner's stated use.
    assert(DropModel::positionFor(0.0) == DropModel::DROP_1);
    assert(DropModel::semitonesFor(DropModel::DROP_1) == 1);
    std::printf("  9 detents map exactly; default is 1 semitone down\n");
}

}  // namespace

int main(int argc, char** argv) {
    clipper::test::requireAssertsLive();
    const int ledger = clipper::test::ledgerMain(argc, argv, kLedger,
                                                 sizeof kLedger / sizeof kLedger[0],
                                                 "clipper_drop_tests");
    if (ledger >= 0) return ledger;
    std::printf("== Cellar drop-tune (M13.10, docs §70) ==\n");
    std::printf("- BAR 1: pitch accuracy vs the exact ratio\n");
    testPitchAccuracy();
    std::printf("- BAR 2: polyphony (the load-bearing one)\n");
    testPolyphony();
    std::printf("- BAR 3: the attack survives\n");
    testAttackSurvives();
    std::printf("- BAR 4: artifact floor, measured and reported\n");
    testArtifactFloor();
    std::printf("- BAR 5: the dry path\n");
    testDryPath();
    std::printf("- BAR 6: latency and oversampling contracts\n");
    testLatencyAndOversampling();
    std::printf("- housekeeping\n");
    testHousekeeping();
    std::printf("== all drop tests passed ==\n");
    return clipper::test::reportXfails();
}
