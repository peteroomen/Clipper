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
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;
using clipper::dsp::AmpModel;
using clipper::dsp::CabConvolver;

// Mirror of ChorusModel::Mode (kept local so the test reads clearly); the values
// are the ABI contract 0=off / 1=chorus / 2=vibrato.
enum ChorusMode { Off = 0, Chorus = 1, Vibrato = 2 };

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
    float volume = 0.4f;  // ~unity gain (0 dB) in the M6.1 audio taper
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

// --- Test 3: volume is a loud-biased audio taper (M6.1). ----------------------
void testVolume(double fs) {
    // Audio taper: db(knob) = +6 - 46*(1-knob)^4.
    //   0.4 -> +0.04 dB (unity), 1.0 -> +6 dB, 0.2 -> -12.8 dB.
    // Anchor 1: the DEFAULT knob (0.4) is unity, so the rig is loud by default.
    {
        AmpKnobs unity;
        unity.volume = 0.4f;
        AmpKnobs top;
        top.volume = 1.0f;
        const double aU = ampResponse(1000.0, unity, fs);
        const double aT = ampResponse(1000.0, top, fs);
        // 0.4 -> 1.0 spans ~+6 dB (the taper's headroom above unity).
        assert(std::fabs(toDb(aT / aU) - 6.0) < 1.0 && "volume 0.4->1.0 not ~ +6 dB");
    }
    // Anchor 2: the bottom third is meaningfully quieter (usable quiet range).
    {
        AmpKnobs a04, a02;
        a04.volume = 0.4f;
        a02.volume = 0.2f;
        const double s = toDb(ampResponse(1000.0, a04, fs) / ampResponse(1000.0, a02, fs));
        assert(s > 10.0 && s < 15.0 && "volume 0.2->0.4 not ~ +12.8 dB (taper too flat)");
    }

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
    // Print the unity anchor for the record.
    AmpKnobs lo, hi;
    lo.volume = 0.5f;
    hi.volume = 1.0f;
    const double measured = toDb(ampResponse(1000.0, hi, fs) / ampResponse(1000.0, lo, fs));
    std::printf("  [ok] volume audio taper: 0.5->1.0 = %.2f dB, 0.4=unity, knob 0 silent (fs=%g)\n",
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

// --- Test 4b: amp+cab makeup gain (M6.1). -------------------------------------
// The rig was ~20 dB too quiet because the amp volume default (0.4) sat at
// -21.6 dB. With the M6.1 audio taper, knob 0.4 == unity, and the default cab IR
// is already normalized to ~unity passband, so the amp(0.4)+cab stage passes a
// broadband signal at roughly unity RMS (a clean platform, not an attenuator).
void testChainGain(double fs) {
    // A broadband-ish test signal (three partials across the guitar band).
    const int n = static_cast<int>(fs);
    std::vector<float> in(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = i / fs;
        in[static_cast<size_t>(i)] = 0.2f * static_cast<float>(
            std::sin(kTwoPi * 220.0 * t) + std::sin(kTwoPi * 660.0 * t) +
            std::sin(kTwoPi * 1500.0 * t)) / 3.0f;
    }
    auto rmsTail = [&](const std::vector<float>& x) {
        double a = 0.0;
        size_t s = x.size() / 3;
        for (size_t i = s; i < x.size(); ++i) a += double(x[i]) * x[i];
        return std::sqrt(a / (x.size() - s));
    };
    const double inRms = rmsTail(in);

    // amp at the DEFAULT volume knob (0.4), tone flat, then the default cab.
    AmpKnobs def;
    def.volume = 0.4f;
    auto amped = renderAmp(in, def, fs);
    auto ir = clipper::dsp::generateDefaultCab2x12IR(fs);
    CabConvolver cab;
    cab.prepare(fs, ir.data(), static_cast<int>(ir.size()), fs, 128);
    std::vector<float> chained = amped;
    cab.process(chained.data(), chained.data(), static_cast<int>(chained.size()));

    const double ampGainDb = toDb(rmsTail(amped) / inRms);
    const double chainGainDb = toDb(rmsTail(chained) / inRms);
    // amp(0.4) is unity within ~2 dB across this band (tone flat-ish).
    assert(std::fabs(ampGainDb) < 2.0 && "amp default volume (0.4) not ~ unity");
    // amp+cab net gain sits near unity (cab colors, does not attenuate ~20 dB).
    // (Upper edge allows the 96 kHz cab's stronger presence bump, ~+3 dB.)
    assert(chainGainDb > -5.0 && chainGainDb < 5.0 &&
           "amp+cab chain gain at default not near unity (rig too quiet/loud)");
    std::printf("  [ok] chain gain @ default vol 0.4: amp %.1f dB, amp+cab %.1f dB (fs=%g)\n",
                ampGainDb, chainGainDb, fs);
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
    // M6.1 audio taper: db(knob) = +6 - 46*(1-knob)^4; at 0.9 that is ~+6 dB.
    const double tk = 1.0 - 0.9;
    const double targetGainDb = 6.0 - 46.0 * (tk * tk) * (tk * tk);
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

// --- M6.3 chorus/vibrato helpers ---------------------------------------------

// Render the amp's STEREO path for a mono input at a given chorus config. Tone
// flat, volume unity (0.4) so the mono voice ~= the input (the chorus is what we
// are measuring). mode: 0 off / 1 chorus / 2 vibrato.
void renderAmpStereo(const std::vector<float>& in, int mode, float speed, float depth,
                     double fs, std::vector<float>& outL, std::vector<float>& outR) {
    AmpModel a;
    a.prepare(fs, 128);
    a.setParameter(AmpModel::PARAM_VOLUME, 0.4f);  // unity
    a.setParameter(AmpModel::PARAM_CHORUS_SPEED, speed);
    a.setParameter(AmpModel::PARAM_CHORUS_DEPTH, depth);
    a.setParameter(AmpModel::PARAM_CHORUS_MODE, static_cast<float>(mode));
    outL.assign(in.size(), 0.0f);
    outR.assign(in.size(), 0.0f);
    if (!in.empty())
        a.processStereo(in.data(), outL.data(), outR.data(), static_cast<int>(in.size()));
}

// --- Test 9: OFF mode is bit-exact L == R and equals the mono voice. ----------
void testChorusOff(double fs) {
    auto in = sine(440.0, 0.3f, 0.3, fs);
    std::vector<float> L, R;
    renderAmpStereo(in, ChorusMode::Off, 0.5f, 0.5f, fs, L, R);

    // L == R bit-exact.
    for (size_t i = 0; i < L.size(); ++i)
        assert(L[i] == R[i] && "chorus OFF: L != R (not bit-exact)");

    // And equals the mono process() voice exactly (chorus is truly bypassed).
    auto mono = renderAmp(in, AmpKnobs{}, fs);  // volume 0.4, tone flat
    assert(mono.size() == L.size());
    double maxErr = 0.0;
    for (size_t i = 0; i < L.size(); ++i)
        maxErr = std::max(maxErr, static_cast<double>(std::fabs(L[i] - mono[i])));
    assert(maxErr == 0.0 && "chorus OFF: stereo L != mono voice (bit-exact expected)");
    std::printf("  [ok] chorus OFF: L==R bit-exact and == mono voice (fs=%g)\n", fs);
}

// --- Test 10: CHORUS mode = dry L / decorrelated wet R. -----------------------
void testChorusChorus(double fs) {
    // Broadband (deterministic white-ish noise) probe: a pure sine's
    // autocorrelation is periodic and cannot pin down a delay, so use noise whose
    // cross-correlation has one sharp peak at the true delay. Low depth isolates
    // the ~5 ms BASE delay (the stereo-bloom offset) from the modulation smear.
    const size_t nlen = static_cast<size_t>(0.5 * fs);
    std::vector<float> in(nlen);
    uint32_t rng = 0x1234567u;
    for (size_t i = 0; i < nlen; ++i) {
        rng = rng * 1664525u + 1013904223u;
        in[i] = (static_cast<float>(rng >> 9) / 4194304.0f - 1.0f) * 0.3f;  // ~[-0.3,0.3), zero-mean
    }
    // Depth 0 => a pure, constant ~5 ms base delay on R (no sweep smear), so the
    // noise cross-correlation gives one sharp peak at the base lag. (Modulation is
    // exercised separately by the vibrato test.)
    std::vector<float> L, R;
    renderAmpStereo(in, ChorusMode::Chorus, 0.3f, 0.0f, fs, L, R);

    // L is the DRY voice (== mono process, bit-exact — the classic JC dry side).
    auto mono = renderAmp(in, AmpKnobs{}, fs);
    double maxErr = 0.0;
    for (size_t i = 0; i < L.size(); ++i)
        maxErr = std::max(maxErr, static_cast<double>(std::fabs(L[i] - mono[i])));
    assert(maxErr == 0.0 && "chorus CHORUS: L is not the bit-exact dry voice");

    // R is delayed/decorrelated: the normalized cross-correlation of L and R
    // PEAKS at a lag near the ~5 ms base delay (not at zero lag). Search lags.
    const int baseLag = static_cast<int>(std::round(0.005 * fs));
    const size_t s = L.size() / 3;  // skip the fill-in transient
    auto nccAt = [&](int lag) {
        double num = 0.0, dL = 0.0, dR = 0.0;
        for (size_t i = s; i + static_cast<size_t>(lag) < L.size(); ++i) {
            const double a = L[i];
            const double b = R[i + static_cast<size_t>(lag)];
            num += a * b;
            dL += a * a;
            dR += b * b;
        }
        return num / (std::sqrt(dL * dR) + 1e-12);
    };
    // Find the best lag in a window bracketing the base delay.
    int bestLag = 0;
    double bestNcc = -2.0;
    const int lo = std::max(1, baseLag - static_cast<int>(0.004 * fs));
    const int hi = baseLag + static_cast<int>(0.004 * fs);
    for (int lag = lo; lag <= hi; ++lag) {
        const double v = nccAt(lag);
        if (v > bestNcc) {
            bestNcc = v;
            bestLag = lag;
        }
    }
    const double zeroNcc = nccAt(0);
    // R lags L by ~the base delay: the peak correlation is at a nonzero lag near
    // 5 ms, and the wet is decorrelated at zero lag (well below the peak).
    assert(std::fabs(bestLag - baseLag) < 0.0025 * fs &&
           "chorus CHORUS: R not delayed ~5 ms behind L");
    assert(bestNcc > zeroNcc + 0.1 && "chorus CHORUS: R not decorrelated from L at zero lag");
    std::printf("  [ok] chorus CHORUS: L dry (bit-exact), R delayed %.2f ms (ncc %.2f vs %.2f @0) (fs=%g)\n",
                1000.0 * bestLag / fs, bestNcc, zeroNcc, fs);
}

// --- Test 11: VIBRATO mode = pitch wobble on BOTH sides; measured peak
//     deviation matches the depth+rate prediction. --------------------------
void testChorusVibrato(double fs) {
    // A steady 1 kHz probe; measure instantaneous frequency from zero-crossing
    // spacing. Slow LFO so many signal cycles sample each part of the sweep.
    const double f0 = 1000.0;
    const float depth = 0.7f;
    // Pick a rate directly so we can predict deviation. speed knob -> Hz is a log
    // map; invert it: we want ~3 Hz. speedToHz(k)=0.15*(8/0.15)^k => k for 3 Hz:
    const double targetHz = 3.0;
    const double kSpeed = std::log(targetHz / 0.15) / std::log(8.0 / 0.15);

    auto in = sine(f0, 0.5f, 1.5, fs);
    std::vector<float> L, R;
    renderAmpStereo(in, ChorusMode::Vibrato, static_cast<float>(kSpeed), depth, fs, L, R);

    // Both sides identical in vibrato (mono pitch wobble, no stereo width).
    for (size_t i = 0; i < L.size(); ++i)
        assert(L[i] == R[i] && "vibrato: L != R (should be identical wet on both sides)");

    // Instantaneous frequency via linearly-interpolated positive-going zero
    // crossings; peak |deviation| over the render tail.
    const size_t start = L.size() / 4;  // skip depth-smoother settle + fill
    double prevX = 0.0;
    std::vector<double> crossings;  // sample indices (fractional)
    for (size_t i = start + 1; i < L.size(); ++i) {
        const double a = L[i - 1], b = L[i];
        if (a <= 0.0 && b > 0.0) {
            const double frac = a == b ? 0.0 : (-a / (b - a));
            crossings.push_back(static_cast<double>(i - 1) + frac);
        }
        (void)prevX;
    }
    assert(crossings.size() > 20 && "vibrato: too few zero crossings to measure");
    double maxDevCents = 0.0;
    for (size_t i = 1; i < crossings.size(); ++i) {
        const double period = crossings[i] - crossings[i - 1];  // samples per cycle
        const double fInst = fs / period;
        const double cents = 1200.0 * std::log2(fInst / f0);
        maxDevCents = std::max(maxDevCents, std::fabs(cents));
    }

    // Predicted peak deviation: A = depth * 1.5 ms; peak = A * 2*pi*f (fractional),
    // in cents = 1200/ln2 * that. (Matches ChorusModel.cpp's documented formula.)
    const double A = depth * 0.0015;  // seconds
    const double predFrac = A * kTwoPi * targetHz;
    const double predCents = 1200.0 / std::log(2.0) * predFrac;
    // Zero-crossing spacing averages the deviation over a signal period, so the
    // MEASURED peak reads a little under the instantaneous peak; allow a broad
    // band (half..1.5x) — this is a sanity band on the depth+rate mapping, not a
    // precision meter.
    assert(maxDevCents > 0.5 * predCents && maxDevCents < 1.5 * predCents &&
           "vibrato: measured peak pitch deviation off the depth+rate prediction");
    std::printf("  [ok] chorus VIBRATO: peak dev %.1f cents (predicted %.1f, depth %.2f @ %.2f Hz) (fs=%g)\n",
                maxDevCents, predCents, static_cast<double>(depth), targetHz, fs);
}

// --- Test 12: two-cab stereo chain stays well under real time (CPU budget). ---
// The M6.3 stereo path runs the cab convolver TWICE (per side). Verify the whole
// amp+2xcab chain in chorus mode is still comfortably faster than real time.
void testChorusPerf(double fs) {
    auto ir = clipper::dsp::generateDefaultCab2x12IR(fs);
    AmpModel amp;
    amp.prepare(fs, 128);
    amp.setParameter(AmpModel::PARAM_VOLUME, 0.5f);
    amp.setParameter(AmpModel::PARAM_CHORUS_SPEED, 0.5f);
    amp.setParameter(AmpModel::PARAM_CHORUS_DEPTH, 0.6f);
    amp.setParameter(AmpModel::PARAM_CHORUS_MODE, static_cast<float>(ChorusMode::Chorus));
    CabConvolver cabL, cabR;
    cabL.prepare(fs, ir.data(), static_cast<int>(ir.size()), fs, 128);
    cabR.prepare(fs, ir.data(), static_cast<int>(ir.size()), fs, 128);

    const int n = static_cast<int>(fs);  // 1 second
    std::vector<float> in(static_cast<size_t>(n), 0.0f);
    std::vector<float> l(static_cast<size_t>(n), 0.0f), r(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i)
        in[static_cast<size_t>(i)] = 0.2f * static_cast<float>(std::sin(kTwoPi * 220.0 * i / fs));

    const auto t0 = std::chrono::high_resolution_clock::now();
    for (int off = 0; off < n; off += 128) {
        const int m = std::min(128, n - off);
        amp.processStereo(in.data() + off, l.data() + off, r.data() + off, m);
        cabL.process(l.data() + off, l.data() + off, m);
        cabR.process(r.data() + off, r.data() + off, m);
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    assert(!hasNaN(l) && !hasNaN(r) && "stereo chain produced NaN");
    assert(ms < 300.0 && "amp+2xcab stereo chain slower than 0.3x real time");
    std::printf("  [ok] perf: amp+2xcab (chorus) 1 s in %.1f ms (%.1f%% of one core, fs=%g)\n",
                ms, ms / 1000.0 * 100.0, fs);
}

}  // namespace

int main() {
    std::printf("Running AmpModel + CabConvolver (M5) tests...\n");
    for (double fs : {44100.0, 96000.0}) {
        testToneStack(fs);
        testBright(fs);
        testVolume(fs);
        testCabIR(fs);
        testChainGain(fs);
        testConvolverImpulse(fs);
        testConvolverChunking(fs);
        testSmoothing(fs);
        testPerf(fs);
        // M6.3 chorus/vibrato.
        testChorusOff(fs);
        testChorusChorus(fs);
        testChorusVibrato(fs);
        testChorusPerf(fs);
    }
    std::printf("All AmpModel + CabConvolver tests passed.\n");
    return 0;
}
