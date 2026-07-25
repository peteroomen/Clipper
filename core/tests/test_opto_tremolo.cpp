// Plain-assert tests for clipper::dsp::OptoTremolo — the AB763 optical tremolo — and for
// the tremolo AS REACHED THROUGH the composed clipper::dsp::TwinAmp.
//
// WHY THIS FILE EXISTS (2026-07-24 audit, Medium/DSP + Test integrity):
//
//   "OptoTremolo has no test at all"
//   "The opto tremolo's depth collapses and its level sags ~5.6 dB as SPEED rises. A fixed
//    55 ms release against a 100 ms LFO period means the LDR never discharges: measured max
//    gain 0.973 -> 0.830 -> 0.522 across the SPEED range, with effective depth shrinking
//    from -71.9 dB to -27.2 dB."
//
// Half true, and the half that was true is the half that mattered. `test_twin_amp.cpp`
// DOES drive the class: depth vs INTENSITY, the enable bypass, the rate map, the
// attack/release asymmetry. But every one of those probes sits at a FIXED `setSpeed(0.4f)`,
// so the one axis with a real defect on it — SPEED — was never swept. And no render in the
// Twin suite reaches the tremolo through `TwinAmp` at all: they all pass INTENSITY 0.
//
// So this file pins the two things nothing pinned:
//
//   A. SPEED INVARIANCE. Turning the speed knob must change the RATE of the throb and
//      (near enough) nothing else. Two separate player expectations:
//        A1  the throb stays DEEP at every speed          -- holds today, asserted for real
//        A2  the mean level stays put at every speed      -- XFAIL, sags 5.1 dB
//        A3  the loud peak of the throb still reaches ~unity -- XFAIL, collapses to 0.52
//      A2 and A3 are the same defect seen two ways: a 55 ms release cannot discharge the
//      cell inside a 100 ms period, so the LDR sits permanently part-lit and the whole
//      waveform slides down instead of the dips getting closer together. A player hears
//      the amp get QUIETER as they speed the tremolo up, which no AB763 does.
//   B. THE COMPOSED PATH. Drive TwinAmp with SPEED/INTENSITY/TREMOLO_ENABLE and prove the
//      modulation actually reaches the output, in both directions of the enable.
//
// Absolute references, not self-comparisons: the rate map is the documented log sweep
// kMinHz..kMaxHz; unity gain is 1.0; "the mean level should not move with speed" is a
// property of every real optical tremolo, not of this implementation.
//
// No framework: int main + <cassert>. Portable C++17.

#include "clipper/dsp/OptoTremolo.h"

#include "clipper/dsp/TwinAmp.h"
#include "support/AssertsLive.h"
#include "support/Xfail.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::OptoTremolo;
using clipper::dsp::TwinAmp;

// --- known-bad properties (2026-07-24 audit) --------------------------------------
constexpr clipper::test::XfailDecl kXfTremLevel{
    "trem-mean-level-sags-with-speed",
    "2026-07-24 audit, Medium/DSP (the opto tremolo)",
    "the tremolo's MEAN LEVEL stays within 1.5 dB across the whole SPEED range -- a player "
    "expects the throb to get faster, not the amp to get quieter",
    "the OptoTremolo release-time fix: scale kReleaseMs with the LFO period (or shorten it) "
    "so the cell actually discharges between dips"};
constexpr clipper::test::XfailDecl kXfTremPeak{
    "trem-peak-gain-collapses-with-speed",
    "2026-07-24 audit, Medium/DSP (the opto tremolo)",
    "the LOUD half of the throb still reaches >=0.90 of unity gain at every SPEED (the LDR "
    "must fully discharge between dips)",
    "the OptoTremolo release-time fix (same root cause as trem-mean-level-sags-with-speed)"};

// --- helpers ----------------------------------------------------------------------
std::vector<float> sine(double f, float amp, double secs, double fs) {
    const int n = static_cast<int>(secs * fs);
    std::vector<float> s(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        s[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(kTwoPi * f * i / fs));
    return s;
}
double toDb(double a) { return 20.0 * std::log10(a + 1e-15); }

// The tremolo's gain envelope, measured the honest way: feed unit DC so the output IS the
// gain. Processed in 128-frame blocks — the worklet's calling convention.
struct Envelope {
    double min = 0.0, max = 0.0, mean = 0.0;
    double depthDb = 0.0;  // 20*log10(min/max) — how deep the dips go
};
Envelope envelopeAt(float speed, float intensity, double fs, double secs = 6.0) {
    OptoTremolo tr;
    tr.prepare(fs);
    tr.setEnabled(true);
    tr.setSpeed(speed);
    tr.setIntensity(intensity);
    const int n = static_cast<int>(secs * fs);
    std::vector<float> in(static_cast<size_t>(n), 1.0f), out(static_cast<size_t>(n), 0.0f);
    for (int off = 0; off < n; off += 128)
        tr.process(in.data() + off, out.data() + off, std::min(128, n - off));
    // Analyse the SECOND HALF only: the enable ramp and the cell's initial charge have
    // settled, and at 1 Hz the first few seconds are not yet periodic.
    Envelope e;
    e.min = 1e30;
    e.max = -1e30;
    double sum = 0.0;
    const int st = n / 2;
    for (int i = st; i < n; ++i) {
        const double v = out[static_cast<size_t>(i)];
        e.min = std::min(e.min, v);
        e.max = std::max(e.max, v);
        sum += v;
    }
    e.mean = sum / (n - st);
    e.depthDb = toDb(e.min / e.max);
    return e;
}

// ---------------------------------------------------------------------------
// Test A — SPEED INVARIANCE. The speed knob sets the RATE; it must not also set the
// level. Swept across the full knob range at full INTENSITY (the worst case, and the
// setting a player who wants an obvious throb actually uses).
// ---------------------------------------------------------------------------
void testSpeedInvariance(double fs) {
    const float speeds[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    Envelope env[5];
    double minMeanDb = 1e30, maxMeanDb = -1e30, worstPeak = 1e30, shallowestDepth = -1e30;
    for (int i = 0; i < 5; ++i) {
        env[i] = envelopeAt(speeds[i], 1.0f, fs);
        OptoTremolo probe;
        probe.setSpeed(speeds[i]);
        probe.prepare(fs);
        std::printf("  [A ] speed %.2f (%.2f Hz): gain %.4f..%.4f mean %.4f -> depth %.1f dB, "
                    "mean level %+.2f dB\n",
                    speeds[i], probe.rateHz(), env[i].min, env[i].max, env[i].mean,
                    env[i].depthDb, toDb(env[i].mean));
        std::fflush(stdout);
        minMeanDb = std::min(minMeanDb, toDb(env[i].mean));
        maxMeanDb = std::max(maxMeanDb, toDb(env[i].mean));
        worstPeak = std::min(worstPeak, env[i].max);
        shallowestDepth = std::max(shallowestDepth, env[i].depthDb);
        // Sanity, at every speed: a gain envelope is a gain — bounded, positive, and it
        // never inverts the signal or amplifies it.
        assert(env[i].min >= 0.0 && "tremolo gain went NEGATIVE (phase inversion, not AM)");
        assert(env[i].max <= 1.0 + 1e-6 && "tremolo gain exceeded unity (it can only attenuate)");
        assert(std::isfinite(env[i].mean) && "non-finite tremolo envelope");
    }

    // A1 — the throb is DEEP at every speed. HOLDS today (worst is -27.2 dB); asserted for
    // real. A tremolo whose dips stop being audible at speed is broken in the other
    // direction, and the release-time fix must not trade one defect for the other.
    assert(shallowestDepth < -12.0 &&
           "tremolo depth at INTENSITY 1 shallower than 12 dB at some SPEED (no audible throb)");

    // A2 — the MEAN LEVEL must not move with SPEED. XFAIL: it sags ~5 dB.
    {
        char detail[256];
        std::snprintf(detail, sizeof detail,
                      "@ %.0f Hz, INTENSITY 1: mean level spans %.2f dB across the SPEED range "
                      "(%+.2f dB at speed 0 -> %+.2f dB at speed 1); bar is 1.5 dB",
                      fs, maxMeanDb - minMeanDb, toDb(env[0].mean), toDb(env[4].mean));
        clipper::test::expectXfail(maxMeanDb - minMeanDb <= 1.5, kXfTremLevel, detail);
    }

    // A3 — the loud half of the throb still reaches ~unity. XFAIL: collapses to 0.52.
    {
        char detail[256];
        std::snprintf(detail, sizeof detail,
                      "@ %.0f Hz, INTENSITY 1: worst peak gain %.4f across the SPEED range "
                      "(%.4f at speed 0 -> %.4f at speed 1); bar is 0.90",
                      fs, worstPeak, env[0].max, env[4].max);
        clipper::test::expectXfail(worstPeak >= 0.90, kXfTremPeak, detail);
    }

    // Whatever else is wrong, the RATE must still be monotonic in the knob — the one thing
    // the speed knob is for. (Measured from the envelope, not from rateHz(): count the dip
    // minima in a fixed window, so this reads the AUDIBLE rate rather than the LFO state.)
    auto dipsPerSecond = [&](float speed) {
        OptoTremolo tr;
        tr.prepare(fs);
        tr.setEnabled(true);
        tr.setSpeed(speed);
        tr.setIntensity(1.0f);
        const int n = static_cast<int>(6.0 * fs);
        std::vector<float> in(static_cast<size_t>(n), 1.0f), out(static_cast<size_t>(n), 0.0f);
        for (int off = 0; off < n; off += 128)
            tr.process(in.data() + off, out.data() + off, std::min(128, n - off));
        const int st = n / 2;
        // Count rising zero-crossings of (gain - mean): one per LFO cycle.
        double sum = 0.0;
        for (int i = st; i < n; ++i) sum += out[static_cast<size_t>(i)];
        const double mean = sum / (n - st);
        int crossings = 0;
        for (int i = st + 1; i < n; ++i)
            if (out[static_cast<size_t>(i - 1)] <= mean && out[static_cast<size_t>(i)] > mean)
                ++crossings;
        return crossings / ((n - st) / fs);
    };
    const double r0 = dipsPerSecond(0.0f), r5 = dipsPerSecond(0.5f), r10 = dipsPerSecond(1.0f);
    // Absolute reference: the documented log map, kMinHz..kMaxHz.
    assert(std::fabs(r0 - OptoTremolo::kMinHz) < 0.35 &&
           "measured throb rate at speed 0 is not ~kMinHz");
    assert(std::fabs(r10 - OptoTremolo::kMaxHz) / OptoTremolo::kMaxHz < 0.12 &&
           "measured throb rate at speed 1 is not ~kMaxHz");
    assert(r5 > r0 && r10 > r5 && "measured throb rate not monotonic with SPEED");
    std::printf("  [ok] SPEED @ %.0f Hz: audible throb rate %.2f -> %.2f -> %.2f Hz (map "
                "%.0f..%.0f Hz), depth stays <= %.1f dB\n",
                fs, r0, r5, r10, OptoTremolo::kMinHz, OptoTremolo::kMaxHz, shallowestDepth);
}

// ---------------------------------------------------------------------------
// Test B — the tremolo AS REACHED THROUGH TwinAmp. Nothing in the Twin suite drives
// this path: every render there passes INTENSITY 0. So the wiring from
// TwinAmp::PARAM_SPEED / PARAM_INTENSITY / PARAM_TREMOLO_ENABLE down into the
// OptoTremolo was untested end to end, and a mis-routed param id would have been
// invisible in both directions.
//
// Measured on the ENVELOPE of a real note, per-cycle-peak, not on the raw samples.
// ---------------------------------------------------------------------------
void testThroughTwinAmp(double fs) {
    const double f0 = 220.0;
    auto renderTwin = [&](bool tremOn, float intensity) {
        TwinAmp amp;
        amp.prepare(fs, 128);
        amp.setOversampling(4);
        amp.setParameter(TwinAmp::PARAM_VOLUME, 0.4f);
        amp.setParameter(TwinAmp::PARAM_BASS, 0.5f);
        amp.setParameter(TwinAmp::PARAM_MID, 0.5f);
        amp.setParameter(TwinAmp::PARAM_TREBLE, 0.6f);
        amp.setParameter(TwinAmp::PARAM_REVERB, 0.0f);
        amp.setParameter(TwinAmp::PARAM_SPEED, 0.3f);
        amp.setParameter(TwinAmp::PARAM_INTENSITY, intensity);
        amp.setParameter(TwinAmp::PARAM_TREMOLO_ENABLE, tremOn ? 1.0f : 0.0f);
        auto in = sine(f0, 0.2f, 3.0, fs);
        std::vector<float> out(in.size(), 0.0f);
        for (size_t off = 0; off < in.size(); off += 128) {
            const int c = static_cast<int>(std::min<size_t>(128, in.size() - off));
            amp.process(in.data() + off, out.data() + off, c);
        }
        return out;
    };
    // Per-cycle peak envelope over the settled second half, then its excursion.
    auto envSwing = [&](const std::vector<float>& o) {
        const int per = static_cast<int>(fs / f0);
        double mn = 1e30, mx = -1e30;
        for (size_t i = o.size() / 2; i + per <= o.size(); i += per) {
            double pk = 0.0;
            for (int j = 0; j < per; ++j)
                pk = std::max(pk, std::fabs(static_cast<double>(o[i + j])));
            mn = std::min(mn, pk);
            mx = std::max(mx, pk);
        }
        return mx > 0.0 ? (mx - mn) / mx : 0.0;  // 0 = steady, ->1 = fully modulated
    };

    const auto off = renderTwin(false, 1.0f);
    const auto on = renderTwin(true, 1.0f);
    const auto zeroIntensity = renderTwin(true, 0.0f);
    for (float v : on) assert(std::isfinite(v) && "non-finite TwinAmp output with trem on");

    const double sOff = envSwing(off), sOn = envSwing(on), sZero = envSwing(zeroIntensity);
    // TREMOLO_ENABLE = 0 must leave the envelope essentially steady; = 1 at full intensity
    // must modulate it hard. Both directions, so a stuck-on or stuck-off enable fails.
    assert(sOff < 0.05 && "TwinAmp envelope is modulated with TREMOLO_ENABLE = 0");
    assert(sZero < 0.05 && "TwinAmp envelope is modulated at INTENSITY = 0");
    assert(sOn > 0.5 && "TREMOLO_ENABLE = 1 at INTENSITY 1 did not modulate the TwinAmp "
                        "envelope (the tremolo is not reached through the composed amp)");
    assert(sOn > sOff * 10.0 && "tremolo on/off is not a large envelope difference");
    std::printf("  [ok] through TwinAmp @ %.0f Hz: envelope swing %.4f (trem off) / %.4f "
                "(intensity 0) / %.4f (trem on, intensity 1)\n",
                fs, sOff, sZero, sOn);
}

// Known defects this binary exercises under XFAIL. Printed by --xfail-ledger, which ctest
// surfaces as ***Skipped in its default summary (see core/CMakeLists.txt).
const clipper::test::XfailDecl kLedger[] = {kXfTremLevel, kXfTremPeak};

}  // namespace

int main(int argc, char** argv) {
    const int ledger = clipper::test::ledgerMain(argc, argv, kLedger,
                                                 sizeof kLedger / sizeof kLedger[0],
                                                 "clipper_opto_tremolo_tests");
    if (ledger >= 0) return ledger;
    clipper::test::requireAssertsLive();

    std::printf("Running clipper::dsp::OptoTremolo tests (SPEED invariance + the composed "
                "TwinAmp path)...\n");
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        testSpeedInvariance(fs);
        testThroughTwinAmp(fs);
    }
    std::printf("All OptoTremolo tests passed (XFAILs listed below are known open defects, "
                "not regressions).\n");
    return clipper::test::reportXfails();
}
