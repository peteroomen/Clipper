// Clipper native shell — the LOAD-BEARING identical-core test.
//
// Proves the JUCE plugin is a re-wrap, not a re-implementation: it renders a known
// signal through the REAL ClipperAudioProcessor and, independently, through a
// from-scratch chain built from the portable core classes DIRECTLY (RatModel,
// SdModel, AmpModel, CabConvolver x2, OutputLimiter) with identical settings, then
// asserts the plugin's LEFT-channel output matches the reference sample-for-sample
// (within a tight float tolerance).
//
// Both paths use the SAME core code and the SAME internal delays (cab 128 +
// limiter 64), so their outputs are time-aligned — no latency offset is applied in
// the comparison. The reported plugin latency is separately checked against the
// sum of the models' latency accessors.
//
// Test signal: an M2-style 220 Hz sine under an exponential pluck envelope at
// 48 kHz. Parameter set exercises the whole chain: both dirt pedals engaged, amp
// powered with cab + bright, chorus (stereo bloom) on, 4x oversampling.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "ClipperEngine.h"
#include "PluginProcessor.h"

#include "clipper/dsp/AmpModel.h"
#include "clipper/dsp/CabConvolver.h"
#include "clipper/dsp/CabIR.h"
#include "clipper/dsp/OutputLimiter.h"
#include "clipper/dsp/RatModel.h"
#include "clipper/dsp/SdModel.h"

using clipper::native::Params;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 128;
constexpr int kNumFrames = 48000;  // 1.0 s

// The known parameter set (both pedals on, cab + bright, stereo chorus, 4x OS).
Params testParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.ratOn = true;  p.ratDist = 0.7f; p.ratFilter = 0.4f; p.ratLevel = 0.8f;
    p.sdOn = true;   p.sdDrive = 0.5f; p.sdTone = 0.5f;    p.sdLevel = 0.7f;
    p.ampOn = true;  p.volume = 0.5f;  p.bass = 0.5f; p.middle = 0.5f; p.treble = 0.6f;
    p.bright = true; p.cab = true;
    p.chorusMode = 1;  // chorus (stereo bloom)
    p.chorusSpeed = 0.3f; p.chorusDepth = 0.5f;
    p.oversampling = 4;
    return p;
}

// M2-style 220 Hz sine * exponential pluck envelope.
std::vector<float> makeSignal() {
    std::vector<float> x(static_cast<size_t>(kNumFrames));
    const double f0 = 220.0;
    const double tau = 0.25;   // pluck decay time constant (s)
    const double amp = 0.3;    // hot-humbucker-ish reference level
    for (int i = 0; i < kNumFrames; ++i) {
        const double t = i / kFs;
        const double env = std::exp(-t / tau);
        x[static_cast<size_t>(i)] =
            static_cast<float>(amp * env * std::sin(2.0 * M_PI * f0 * t));
    }
    return x;
}

// The REFERENCE chain: mirrors ClipperEngine::process using core classes directly.
// Set params -> prepare (snaps smoothers) -> set oversampling, exactly as the
// engine does, then render the whole signal block by block.
void renderReference(const std::vector<float>& in, std::vector<float>& outL,
                     std::vector<float>& outR, int& latencyOut) {
    using namespace clipper::dsp;
    const Params p = testParams();

    RatModel rat;
    SdModel sd;
    AmpModel amp;
    CabConvolver cabL, cabR;
    OutputLimiter limiter;

    rat.setParameter(RatModel::PARAM_DISTORTION, p.ratDist);
    rat.setParameter(RatModel::PARAM_FILTER, p.ratFilter);
    rat.setParameter(RatModel::PARAM_LEVEL, p.ratLevel);
    sd.setParameter(SdModel::PARAM_DRIVE, p.sdDrive);
    sd.setParameter(SdModel::PARAM_TONE, p.sdTone);
    sd.setParameter(SdModel::PARAM_LEVEL, p.sdLevel);
    amp.setParameter(AmpModel::PARAM_VOLUME, p.volume);
    amp.setParameter(AmpModel::PARAM_BASS, p.bass);
    amp.setParameter(AmpModel::PARAM_MIDDLE, p.middle);
    amp.setParameter(AmpModel::PARAM_TREBLE, p.treble);
    amp.setParameter(AmpModel::PARAM_BRIGHT, p.bright ? 1.0f : 0.0f);
    amp.setParameter(AmpModel::PARAM_CHORUS_SPEED, p.chorusSpeed);
    amp.setParameter(AmpModel::PARAM_CHORUS_DEPTH, p.chorusDepth);
    amp.setParameter(AmpModel::PARAM_CHORUS_MODE, static_cast<float>(p.chorusMode));

    rat.prepare(kFs, kBlock);
    sd.prepare(kFs, kBlock);
    amp.prepare(kFs, kBlock);
    rat.setOversampling(p.oversampling);
    sd.setOversampling(p.oversampling);

    const std::vector<float> ir = generateDefaultCab2x12IR(kFs);
    cabL.prepare(kFs, ir.data(), static_cast<int>(ir.size()), kFs, 128);
    cabR.prepare(kFs, ir.data(), static_cast<int>(ir.size()), kFs, 128);
    limiter.prepare(kFs);

    outL.assign(in.size(), 0.0f);
    outR.assign(in.size(), 0.0f);

    std::vector<float> a(kBlock), b(kBlock), l(kBlock), r(kBlock);
    const float g = clipper::native::trimKnobToGain(p.inputTrim);

    int off = 0;
    while (off < kNumFrames) {
        const int n = std::min(kBlock, kNumFrames - off);
        for (int i = 0; i < n; ++i) a[static_cast<size_t>(i)] = in[static_cast<size_t>(off + i)] * g;
        float* cur = a.data();
        float* other = b.data();
        if (p.ratOn) { rat.process(cur, other, n); std::swap(cur, other); }
        if (p.sdOn)  { sd.process(cur, other, n);  std::swap(cur, other); }
        if (!p.ampOn) {
            for (int i = 0; i < n; ++i) { l[static_cast<size_t>(i)] = cur[i]; r[static_cast<size_t>(i)] = cur[i]; }
        } else {
            amp.processStereo(cur, l.data(), r.data(), n);       // stereo split
            if (p.cab) {
                cabL.process(l.data(), l.data(), n);             // per-side cab
                cabR.process(r.data(), r.data(), n);
            }
        }
        limiter.processStereo(l.data(), r.data(), n);            // safety limiter
        for (int i = 0; i < n; ++i) {
            outL[static_cast<size_t>(off + i)] = l[static_cast<size_t>(i)];
            outR[static_cast<size_t>(off + i)] = r[static_cast<size_t>(i)];
        }
        off += n;
    }

    latencyOut = (p.ratOn ? rat.latencySamples() : 0) +
                 (p.sdOn ? sd.latencySamples() : 0) +
                 (p.ampOn && p.cab ? cabL.latencySamples() : 0) +
                 limiter.latencySamples();
}

// Drive the REAL plugin over the same signal, mono in -> stereo out.
void renderPlugin(const std::vector<float>& in, std::vector<float>& outL,
                  std::vector<float>& outR, int& latencyOut) {
    clipper::native::ClipperAudioProcessor proc;
    const Params p = testParams();

    // Push the known set into the APVTS BEFORE prepareToPlay, so the engine snaps
    // its smoothers to these targets (steady state from sample 0). convertTo0to1
    // maps each param's real value (knob 0..1 / bool 0-1 / choice index) correctly.
    auto set = [&](const char* id, float realValue) {
        auto* param = proc.apvts.getParameter(id);
        param->setValueNotifyingHost(param->convertTo0to1(realValue));
    };
    using namespace clipper::native::pid;
    set(inputTrim, p.inputTrim);
    set(ratOn, 1.0f);  set(ratDist, p.ratDist); set(ratFilter, p.ratFilter); set(ratLevel, p.ratLevel);
    set(sdOn, 1.0f);   set(sdDrive, p.sdDrive); set(sdTone, p.sdTone);       set(sdLevel, p.sdLevel);
    set(ampOn, 1.0f);  set(volume, p.volume); set(bass, p.bass); set(middle, p.middle); set(treble, p.treble);
    set(bright, 1.0f); set(cab, 1.0f);
    set(chorusMode, static_cast<float>(p.chorusMode));  // choice index 1 == Chorus
    set(chorusSpeed, p.chorusSpeed); set(chorusDepth, p.chorusDepth);
    set(oversampling, 2.0f);  // choice index 2 == 4x

    proc.setPlayConfigDetails(1, 2, kFs, kBlock);
    proc.prepareToPlay(kFs, kBlock);

    outL.assign(in.size(), 0.0f);
    outR.assign(in.size(), 0.0f);

    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    int off = 0;
    while (off < kNumFrames) {
        const int n = std::min(kBlock, kNumFrames - off);
        buffer.clear();
        // Mono source into channel 0 (channel 1 left silent; the processor reads
        // channel 0 as the mono guitar).
        for (int i = 0; i < n; ++i) buffer.setSample(0, i, in[static_cast<size_t>(off + i)]);
        juce::AudioBuffer<float> block(buffer.getArrayOfWritePointers(), 2, n);
        proc.processBlock(block, midi);
        for (int i = 0; i < n; ++i) {
            outL[static_cast<size_t>(off + i)] = block.getSample(0, i);
            outR[static_cast<size_t>(off + i)] = block.getSample(1, i);
        }
        off += n;
    }
    latencyOut = proc.getLatencySamples();
}

}  // namespace

int main() {
    std::printf("=== Clipper identical-core test ===\n");
    std::printf("signal: 220 Hz sine + pluck, %d samples @ %.0f Hz, block %d\n",
                kNumFrames, kFs, kBlock);

    const std::vector<float> in = makeSignal();

    std::vector<float> refL, refR, plL, plR;
    int refLat = 0, plLat = 0;
    renderReference(in, refL, refR, refLat);
    renderPlugin(in, plL, plR, plLat);

    // Secondary cross-check: render straight through ClipperEngine (no JUCE glue),
    // set once + prepared (snapped), and confirm it too is bit-exact against the
    // hand-written core reference — isolating the engine from the plugin wrapper.
    double engMax = 0.0;
    {
        clipper::native::ClipperEngine eng;
        eng.setParams(testParams());
        eng.prepare(kFs, kBlock);
        int off = 0;
        std::vector<float> il(kBlock), ol(kBlock), orr(kBlock);
        while (off < kNumFrames) {
            const int n = std::min(kBlock, kNumFrames - off);
            for (int i = 0; i < n; ++i) il[static_cast<size_t>(i)] = in[static_cast<size_t>(off + i)];
            eng.process(il.data(), ol.data(), orr.data(), n);
            for (int i = 0; i < n; ++i)
                engMax = std::max(engMax, std::fabs(static_cast<double>(ol[static_cast<size_t>(i)]) -
                                                    static_cast<double>(refL[static_cast<size_t>(off + i)])));
            off += n;
        }
    }

    // Compare the LEFT channel (the task's spec) sample-for-sample.
    double maxDiff = 0.0, sumSq = 0.0, refPeak = 0.0;
    int maxIdx = -1;
    for (int i = 0; i < kNumFrames; ++i) {
        const double d = std::fabs(static_cast<double>(plL[static_cast<size_t>(i)]) -
                                   static_cast<double>(refL[static_cast<size_t>(i)]));
        if (d > maxDiff) { maxDiff = d; maxIdx = i; }
        sumSq += d * d;
        refPeak = std::max(refPeak, std::fabs(static_cast<double>(refL[static_cast<size_t>(i)])));
    }
    const double rms = std::sqrt(sumSq / kNumFrames);

    // Also confirm the RIGHT channel matches (proves the stereo chorus bloom path
    // is wired identically), and that the reference actually produced signal.
    double maxDiffR = 0.0;
    for (int i = 0; i < kNumFrames; ++i)
        maxDiffR = std::max(maxDiffR,
                            std::fabs(static_cast<double>(plR[static_cast<size_t>(i)]) -
                                      static_cast<double>(refR[static_cast<size_t>(i)])));

    std::printf("reference L peak      : %.6f\n", refPeak);
    std::printf("max |engine-ref| L    : %.3e\n", engMax);
    std::printf("max |plugin-ref| L    : %.3e (at sample %d)\n", maxDiff, maxIdx);
    std::printf("max |plugin-ref| R    : %.3e\n", maxDiffR);
    std::printf("RMS diff L            : %.3e\n", rms);
    std::printf("reported latency      : plugin=%d  reference=%d\n", plLat, refLat);

    // Tolerance: the plugin wraps the identical core with no re-seeding, so the
    // whole chain is expected BIT-EXACT. Allow only a hair of float slack.
    const double kTol = 1e-6;
    bool ok = true;
    if (refPeak < 1e-4) { std::printf("FAIL: reference produced no signal\n"); ok = false; }
    if (engMax > kTol)  { std::printf("FAIL: engine vs core reference exceeds %.1e\n", kTol); ok = false; }
    if (maxDiff > kTol) { std::printf("FAIL: L channel exceeds tolerance %.1e\n", kTol); ok = false; }
    if (maxDiffR > kTol) { std::printf("FAIL: R channel exceeds tolerance %.1e\n", kTol); ok = false; }
    if (plLat != refLat) { std::printf("FAIL: latency mismatch\n"); ok = false; }

    if (ok) {
        std::printf("PASS: plugin output is sample-identical to the direct core chain.\n");
        return 0;
    }
    return 1;
}
