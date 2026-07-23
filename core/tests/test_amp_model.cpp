// Plain-assert tests for the M5 clean amp + cab: clipper::dsp::AmpModel,
// clipper::dsp::CabConvolver, and the default 2x12 IR generator. No framework:
// int main + <cassert>. Frequency content is measured with single-bin DFTs (a
// hand-rolled Goertzel-style sum) — no FFT dependency in the test itself.
//
// Each assert is written to FAIL if the corresponding stage is broken (verified
// during development by perturbing the model).

#include "clipper/dsp/AmpModel.h"
#include "clipper/dsp/CabConvolver.h"
#include "clipper/dsp/CabIR.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::AmpModel;
using clipper::dsp::CabConvolver;

// Single-bin magnitude of a real signal at frequency f (Hann-windowed).
double binMag(const std::vector<float>& x, size_t start, size_t n, double f, double fs) {
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

// Raw DTFT magnitude of a finite impulse response == |H(f)| exactly. Unlike the
// windowed, amplitude-normalized binMag() (which estimates a *sinusoid's*
// amplitude), this is the right tool for reading a filter/IR frequency response.
double irMag(const std::vector<float>& h, double f, double fs) {
    const double w = kTwoPi * f / fs;
    double re = 0.0, im = 0.0;
    for (size_t n = 0; n < h.size(); ++n) {
        re += h[n] * std::cos(w * n);
        im -= h[n] * std::sin(w * n);
    }
    return std::sqrt(re * re + im * im);
}

std::vector<float> sine(double f, float amp, double secs, double fs) {
    const int n = static_cast<int>(secs * fs);
    std::vector<float> s(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        s[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(kTwoPi * f * i / fs));
    return s;
}

struct AmpKnobs {
    float volume = 0.8696f;  // ~unity gain (db 0)
    float bass = 0.5f, middle = 0.5f, treble = 0.5f, bright = 0.0f;
};

std::vector<float> renderAmp(const std::vector<float>& in, AmpKnobs k, double fs) {
    AmpModel a;
    a.prepare(fs, 128);
    a.setParameter(AmpModel::PARAM_VOLUME, k.volume);
    a.setParameter(AmpModel::PARAM_BASS, k.bass);
    a.setParameter(AmpModel::PARAM_MIDDLE, k.middle);
    a.setParameter(AmpModel::PARAM_TREBLE, k.treble);
    a.setParameter(AmpModel::PARAM_BRIGHT, k.bright);
    std::vector<float> out(in.size(), 0.0f);
    if (!in.empty()) a.process(in.data(), out.data(), static_cast<int>(in.size()));
    return out;
}

// Steady-state amplitude of the amp output at frequency f for a given knob set.
double ampResponse(double f, AmpKnobs k, double fs) {
    auto in = sine(f, 0.25f, 1.0, fs);
    auto out = renderAmp(in, k, fs);
    const size_t n = out.size();
    const size_t win = std::min(n, static_cast<size_t>(fs * 0.7));
    const size_t start = n - win;  // skip the smoothing transient
    return binMag(out, start, win, f, fs);
}

bool hasNaN(const std::vector<float>& x) {
    for (float v : x)
        if (std::isnan(v) || std::isinf(v)) return true;
    return false;
}

// --- Test 1: tone stack moves each band by the documented dB; flat at 0.5. ----
void testToneStack(double fs) {
    // Reference: all tone knobs flat (0.5). Boost = response(knob=1)/flat.
    // Volume cancels since it is identical across the compared renders.

    // BASS: low-shelf @ 100 Hz, +/-12 dB. Measure in the shelf band (50 Hz).
    {
        AmpKnobs flat, up = {}, dn = {};
        up.bass = 1.0f;
        dn.bass = 0.0f;
        const double a0 = ampResponse(50.0, flat, fs);
        const double aUp = ampResponse(50.0, up, fs);
        const double aDn = ampResponse(50.0, dn, fs);
        const double boost = toDb(aUp / a0);
        const double cut = toDb(aDn / a0);
        assert(boost > 9.0 && boost < 14.0 && "bass boost not ~ +12 dB");
        assert(cut < -9.0 && cut > -14.0 && "bass cut not ~ -12 dB");
    }

    // MIDDLE: peaking @ 650 Hz, +/-9 dB. Measure at the center.
    {
        AmpKnobs flat, up = {};
        up.middle = 1.0f;
        const double a0 = ampResponse(650.0, flat, fs);
        const double aUp = ampResponse(650.0, up, fs);
        const double boost = toDb(aUp / a0);
        assert(boost > 6.5 && boost < 11.0 && "mid boost not ~ +9 dB");
    }

    // TREBLE: high-shelf @ 3.5 kHz, +/-12 dB. Measure well above the corner.
    {
        AmpKnobs flat, up = {}, dn = {};
        up.treble = 1.0f;
        dn.treble = 0.0f;
        const double a0 = ampResponse(9000.0, flat, fs);
        const double aUp = ampResponse(9000.0, up, fs);
        const double aDn = ampResponse(9000.0, dn, fs);
        assert(toDb(aUp / a0) > 9.0 && "treble boost not ~ +12 dB");
        assert(toDb(aDn / a0) < -9.0 && "treble cut not ~ -12 dB");
    }

    // FLAT at 0.5: each band center ~ 0 dB relative to the others (the stack is
    // transparent). Compare all three band-center responses; they should be
    // within ~1 dB of each other.
    {
        AmpKnobs flat;
        const double b = toDb(ampResponse(100.0, flat, fs));
        const double m = toDb(ampResponse(650.0, flat, fs));
        const double t = toDb(ampResponse(3500.0, flat, fs));
        assert(std::fabs(b - m) < 1.2 && "stack not flat at knobs=0.5 (bass vs mid)");
        assert(std::fabs(t - m) < 1.2 && "stack not flat at knobs=0.5 (treble vs mid)");
    }
    std::printf("  [ok] tone stack: bass/mid/treble bands move as documented; flat at 0.5 (fs=%g)\n", fs);
}

// --- Test 2: bright switch adds a high-shelf. ---------------------------------
void testBright(double fs) {
    AmpKnobs off, on = {};
    on.bright = 1.0f;
    // Above ~3 kHz the bright shelf adds ~+5 dB; below it, ~no change.
    const double hiOff = ampResponse(8000.0, off, fs);
    const double hiOn = ampResponse(8000.0, on, fs);
    const double loOff = ampResponse(200.0, off, fs);
    const double loOn = ampResponse(200.0, on, fs);
    const double hiBoost = toDb(hiOn / hiOff);
    const double loBoost = toDb(loOn / loOff);
    assert(hiBoost > 3.0 && hiBoost < 7.0 && "bright switch high boost not ~ +5 dB");
    assert(std::fabs(loBoost) < 1.0 && "bright switch should not move the low end");
    std::printf("  [ok] bright switch: +%.1f dB @ 8 kHz, %.2f dB @ 200 Hz (fs=%g)\n",
                hiBoost, loBoost, fs);
}

// --- Test 3: volume is linear-in-dB. ------------------------------------------
void testVolume(double fs) {
    // Two knob positions in the dB-linear region; the gain ratio must match the
    // dB spacing. knob 0.5 -> -17 dB, knob 1.0 -> +6 dB => 23 dB apart.
    AmpKnobs lo, hi;
    lo.volume = 0.5f;
    hi.volume = 1.0f;
    const double aLo = ampResponse(1000.0, lo, fs);
    const double aHi = ampResponse(1000.0, hi, fs);
    const double measured = toDb(aHi / aLo);
    // knob 1.0 db = -40 + 46 = +6; knob 0.5 db = -40 + 23 = -17. delta = 23 dB.
    assert(std::fabs(measured - 23.0) < 1.0 && "volume not linear-in-dB (23 dB span)");

    // Knob 0 = true silence.
    {
        AmpKnobs zero;
        zero.volume = 0.0f;
        auto out = renderAmp(sine(1000.0, 0.3f, 0.2, fs), zero, fs);
        double pk = 0.0;
        for (size_t i = out.size() / 2; i < out.size(); ++i)
            pk = std::max(pk, static_cast<double>(std::fabs(out[i])));
        assert(pk < 1e-4 && "volume knob 0 should be silent");
    }
    std::printf("  [ok] volume linear-in-dB: measured %.2f dB across 0.5->1.0 (expect 23); knob 0 silent (fs=%g)\n",
                measured, fs);
}

// --- Test 4: default cab IR frequency-response shape. -------------------------
void testCabIR(double fs) {
    auto ir = clipper::dsp::generateDefaultCab2x12IR(fs);
    assert(!ir.empty() && !hasNaN(ir) && "cab IR empty or NaN");

    const size_t n = ir.size();
    auto mag = [&](double f) { return irMag(ir, f, fs); };
    const double d100 = toDb(mag(100.0) / mag(1000.0));
    const double d1k = 0.0;  // reference
    const double d25k = toDb(mag(2500.0) / mag(1000.0));
    const double d5k = toDb(mag(5000.0) / mag(1000.0));
    const double d8k = toDb(mag(8000.0) / mag(1000.0));
    (void)d1k;

    // Expected shape: 1 kHz is the passband reference. 8 kHz sits far down the
    // steep speaker rolloff; 5 kHz is partway down; the presence region (2.5 kHz)
    // is not collapsed; the low end (100 Hz) is rolled off by the low cut.
    assert(d8k < -15.0 && "cab IR: 8 kHz not far below 1 kHz (speaker rolloff missing)");
    assert(d5k < -3.0 && "cab IR: 5 kHz should be below 1 kHz");
    assert(d5k > d8k && "cab IR: rolloff should be monotone 5k > 8k");
    assert(d25k > -6.0 && "cab IR: presence region collapsed");
    assert(d100 < -1.5 && "cab IR: low end not rolled off vs 1 kHz");
    const double d60 = toDb(mag(60.0) / mag(1000.0));
    assert(d60 < -6.0 && "cab IR: sub-bass (60 Hz) not cut");
    std::printf("  [ok] cab IR shape (dB re 1kHz): 60Hz %.1f  100Hz %.1f  2.5k %.1f  5k %.1f  8k %.1f (fs=%g, len=%zu)\n",
                d60, d100, d25k, d5k, d8k, fs, n);
}

// --- Test 5: impulse through the convolver reproduces the IR, delayed by the
//     reported latency (verifies the FFT partitioning + the latency contract). --
void testConvolverImpulse(double fs) {
    auto ir = clipper::dsp::generateDefaultCab2x12IR(fs);
    CabConvolver cab;
    cab.prepare(fs, ir.data(), static_cast<int>(ir.size()), fs, 128);
    const int lat = cab.latencySamples();
    assert(lat == 128 && "cab latency should equal the partition size (128)");

    const int total = static_cast<int>(ir.size()) + 2 * lat + 64;
    std::vector<float> in(static_cast<size_t>(total), 0.0f);
    std::vector<float> out(static_cast<size_t>(total), 0.0f);
    in[0] = 1.0f;
    cab.process(in.data(), out.data(), total);

    assert(!hasNaN(out) && "convolver produced NaN");

    // out[lat + n] must equal ir[n] within float tolerance.
    double maxErr = 0.0;
    for (size_t nn = 0; nn < ir.size(); ++nn) {
        const double e = std::fabs(out[static_cast<size_t>(lat) + nn] - ir[nn]);
        maxErr = std::max(maxErr, e);
    }
    assert(maxErr < 1e-4 && "impulse response != IR (partitioned FFT convolution wrong)");

    // Everything before the latency is silence (causality / delay correctness).
    for (int i = 0; i < lat; ++i)
        assert(std::fabs(out[static_cast<size_t>(i)]) < 1e-5 && "output before latency not silent");

    // Measured delay == reported latency: peak of the output aligns with the
    // IR peak shifted by lat.
    auto peakIdx = [](const std::vector<float>& v, int lo, int hi) {
        int bi = lo;
        double bv = 0.0;
        for (int i = lo; i < hi; ++i)
            if (std::fabs(v[static_cast<size_t>(i)]) > bv) {
                bv = std::fabs(v[static_cast<size_t>(i)]);
                bi = i;
            }
        return bi;
    };
    const int irPeak = peakIdx(ir, 0, static_cast<int>(ir.size()));
    const int outPeak = peakIdx(out, 0, total);
    assert(outPeak - irPeak == lat && "measured impulse delay != latencySamples()");
    std::printf("  [ok] convolver: impulse reproduces IR (maxErr %.2e), delay=%d == latency (fs=%g)\n",
                maxErr, outPeak - irPeak, fs);
}

// --- Test 6: convolver linearity chunking — whole-buffer vs block-by-block. ----
void testConvolverChunking(double fs) {
    auto ir = clipper::dsp::generateDefaultCab2x12IR(fs);
    auto in = sine(440.0, 0.3f, 0.05, fs);
    const int n = static_cast<int>(in.size());

    CabConvolver a;
    a.prepare(fs, ir.data(), static_cast<int>(ir.size()), fs, 128);
    std::vector<float> whole(static_cast<size_t>(n), 0.0f);
    a.process(in.data(), whole.data(), n);

    CabConvolver b;
    b.prepare(fs, ir.data(), static_cast<int>(ir.size()), fs, 128);
    std::vector<float> blocked(static_cast<size_t>(n), 0.0f);
    for (int off = 0; off < n; off += 128) {
        const int m = std::min(128, n - off);
        b.process(in.data() + off, blocked.data() + off, m);
    }
    double maxErr = 0.0;
    for (int i = 0; i < n; ++i)
        maxErr = std::max(maxErr, static_cast<double>(std::fabs(whole[static_cast<size_t>(i)] -
                                                                blocked[static_cast<size_t>(i)])));
    assert(maxErr < 1e-5 && "convolver output depends on block segmentation");
    std::printf("  [ok] convolver chunking invariant (maxErr %.2e, fs=%g)\n", maxErr, fs);
}

// --- Test 7: no zipper noise on a volume sweep during a sine. -----------------
void testSmoothing(double fs) {
    const double f = 100.0;
    const float A = 0.3f;
    auto in = sine(f, A, 0.3, fs);
    const int n = static_cast<int>(in.size());

    AmpModel amp;
    amp.prepare(fs, 128);
    amp.setParameter(AmpModel::PARAM_VOLUME, 0.3f);  // start low
    std::vector<float> out(static_cast<size_t>(n), 0.0f);

    // Process contiguously in blocks; jump the volume target abruptly at the
    // first block boundary past the midpoint.
    const int mid = n / 2;
    bool jumped = false;
    for (int i = 0; i < n; i += 64) {
        if (!jumped && i >= mid) {
            amp.setParameter(AmpModel::PARAM_VOLUME, 0.9f);  // abrupt jump
            jumped = true;
        }
        amp.process(in.data() + i, out.data() + i, std::min(64, n - i));
    }

    assert(!hasNaN(out) && "smoothing test produced NaN");

    // A click at the switch would spike the sample-to-sample difference far above
    // the sine's own maximum slope. Bound: 1.5x the sine slope at the TARGET gain.
    const double targetGainDb = -40.0 + 46.0 * 0.9;  // = +1.4 dB
    const double targetGain = std::pow(10.0, targetGainDb / 20.0);
    const double sineSlope = targetGain * A * kTwoPi * f / fs;
    const double bound = 1.5 * sineSlope;
    double maxDelta = 0.0;
    for (int k = 1; k < n; ++k)
        maxDelta = std::max(maxDelta, static_cast<double>(std::fabs(out[static_cast<size_t>(k)] -
                                                                    out[static_cast<size_t>(k - 1)])));
    assert(maxDelta < bound && "volume sweep produced a click (zipper noise)");
    std::printf("  [ok] smoothing: max per-sample delta %.5f < bound %.5f (no zipper, fs=%g)\n",
                maxDelta, bound, fs);
}

// --- Test 8: amp+cab chain is well under real time. ---------------------------
void testPerf(double fs) {
    auto ir = clipper::dsp::generateDefaultCab2x12IR(fs);
    AmpModel amp;
    amp.prepare(fs, 128);
    amp.setParameter(AmpModel::PARAM_VOLUME, 0.7f);
    amp.setParameter(AmpModel::PARAM_TREBLE, 0.6f);
    CabConvolver cab;
    cab.prepare(fs, ir.data(), static_cast<int>(ir.size()), fs, 128);

    const int n = static_cast<int>(fs);  // 1 second
    std::vector<float> in(static_cast<size_t>(n), 0.0f), buf(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i)
        in[static_cast<size_t>(i)] = 0.2f * static_cast<float>(std::sin(kTwoPi * 220.0 * i / fs));

    const auto t0 = std::chrono::high_resolution_clock::now();
    for (int off = 0; off < n; off += 128) {
        const int m = std::min(128, n - off);
        amp.process(in.data() + off, buf.data() + off, m);
        cab.process(buf.data() + off, buf.data() + off, m);
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    assert(!hasNaN(buf) && "chain produced NaN");
    assert(ms < 200.0 && "amp+cab chain slower than 0.2x real time");
    std::printf("  [ok] perf: amp+cab 1 s in %.1f ms (<< real time, fs=%g)\n", ms, fs);
}

}  // namespace

int main() {
    std::printf("Running AmpModel + CabConvolver (M5) tests...\n");
    for (double fs : {44100.0, 96000.0}) {
        testToneStack(fs);
        testBright(fs);
        testVolume(fs);
        testCabIR(fs);
        testConvolverImpulse(fs);
        testConvolverChunking(fs);
        testSmoothing(fs);
        testPerf(fs);
    }
    std::printf("All AmpModel + CabConvolver tests passed.\n");
    return 0;
}
