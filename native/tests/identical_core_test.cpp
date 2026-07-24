// Clipper native shell — the LOAD-BEARING identical-core test.
//
// Proves the JUCE plugin is a re-wrap, not a re-implementation: it renders a known
// signal through the REAL ClipperAudioProcessor and, independently, through a
// from-scratch chain built from the portable core classes DIRECTLY (RatModel,
// SdModel, AmpModel / Jcm800Amp, CabConvolver x2, OutputLimiter) with identical
// settings, then asserts the plugin's LEFT-channel output matches the reference
// sample-for-sample (within a tight float tolerance).
//
// M9.4/M10.1/M10.2: the check now runs for ALL FOUR amp voices. The Clean 120 case
// exercises the linear stereo-chorus platform; the JCM800 case exercises the mono
// valve head (preamp cascade + power section) rendered dual-mono, now WITH its M10.1
// spring reverb engaged; the Twin case exercises the Fender-blackface mono combo
// with its full AB763 chain (spring reverb + optical tremolo) dual-mono into the same
// cab pair; the AC30 case exercises the Vox class-A "top boost" combo (preamp gain +
// top-boost stack + PI/TOP CUT + EL84 quad + spring reverb) dual-mono into the same
// cab pair, with the presence field reused as TOP CUT. All paths must be bit-exact,
// proving the amp-model switch + per-voice param routing are wrapped identically in
// the plugin and the raw engine.
//
// Both paths use the SAME core code and the SAME internal delays (JCM oversampling
// group delay + cab 128 + limiter 64), so their outputs are time-aligned — no
// latency offset is applied in the comparison. The reported plugin latency is
// separately checked against the sum of the models' latency accessors.
//
// Test signal: an M2-style 220 Hz sine under an exponential pluck envelope at
// 48 kHz. The Clean 120 set engages both dirt pedals, cab + bright, chorus and
// reverb, 4x oversampling; the JCM800 set runs an SD-1 boost into a cranked head.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "ClipperEngine.h"
#include "PluginProcessor.h"

#include "clipper/dsp/Ac30Amp.h"
#include "clipper/dsp/AmpModel.h"
#include "clipper/dsp/CabConvolver.h"
#include "clipper/dsp/CabIR.h"
#include "clipper/dsp/Jcm800Amp.h"
#include "clipper/dsp/OutputLimiter.h"
#include "clipper/dsp/RatModel.h"
#include "clipper/dsp/SdModel.h"
#include "clipper/dsp/TwinAmp.h"

using clipper::native::Params;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 128;
constexpr int kNumFrames = 48000;  // 1.0 s
constexpr int kJcmOversampling = 4;  // matches ClipperEngine/C ABI (docs §18)
constexpr int kTwinOversampling = 4; // matches ClipperEngine/C ABI (docs §20)
constexpr int kAc30Oversampling = 4; // matches ClipperEngine/C ABI (docs §23)
constexpr int kAmpJcm800 = 1;        // Params::ampModel value for the JCM head
constexpr int kAmpTwin = 2;          // Params::ampModel value for the Twin combo
constexpr int kAmpAc30 = 3;          // Params::ampModel value for the AC30 combo

// The CLEAN 120 parameter set (both pedals on, cab + bright, stereo chorus + spring
// reverb, 4x OS) — the linear clean-platform path.
Params cleanParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.ratOn = true;  p.ratDist = 0.7f; p.ratFilter = 0.4f; p.ratLevel = 0.8f;
    p.sdOn = true;   p.sdDrive = 0.5f; p.sdTone = 0.5f;    p.sdLevel = 0.7f;
    p.ampModel = 0;  // Clean 120
    p.ampOn = true;  p.volume = 0.5f;  p.bass = 0.5f; p.middle = 0.5f; p.treble = 0.6f;
    p.bright = true; p.cab = true;
    p.chorusMode = 1;  // chorus (stereo bloom)
    p.chorusSpeed = 0.3f; p.chorusDepth = 0.5f;
    p.reverb = 0.5f;   // M6.7 spring reverb engaged (exercises the wet stereo path)
    p.oversampling = 4;
    return p;
}

// The JCM800 parameter set — the canonical SD-1 boost into a cranked Marshall head.
// RAT off; SD-1 on as a clean-ish boost (low drive, higher level) shoving the JCM's
// front end. The JCM makes its own distortion (gain/master), so it is the amp doing
// the work. bass/middle/treble are the SHARED tone knobs; the JCM ignores volume/
// bright/chorus/reverb. Cab on (would pair with brit412 in the app, but the test's
// bit-exactness is IR-agnostic, so the default IR is fine here).
Params jcmParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.ratOn = false; p.ratDist = 0.7f; p.ratFilter = 0.4f; p.ratLevel = 0.8f;
    p.sdOn = true;   p.sdDrive = 0.25f; p.sdTone = 0.6f;   p.sdLevel = 0.85f;  // boost
    p.ampModel = kAmpJcm800;  // JCM800
    p.ampOn = true;
    p.bass = 0.55f; p.middle = 0.45f; p.treble = 0.65f;  // shared tone stack
    p.bright = false; p.cab = true;
    p.chorusMode = 0;
    p.reverb = 0.4f;       // M10.1: exercise the JCM's spring reverb (usability add)
    p.jcmGain = 0.7f;      // cranked preamp
    p.jcmMaster = 0.5f;    // power-amp pushed
    p.jcmPresence = 0.6f;  // HF lift
    p.oversampling = 4;
    return p;
}

// The TWIN parameter set (M10.1) — the clean benchmark, exercising the full AB763
// chain: spring reverb + optical tremolo. RAT on as a light pedal in front (the Twin
// itself stays clean); the Twin makes NO preamp gain, so this is a glassy clean voice
// with a moving throb and a spring tail. Reuses the shared knobs — volume/bright,
// bass/mid/treble, reverb, and speed/depth (routed to the tremolo SPEED/INTENSITY).
Params twinParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.ratOn = true;  p.ratDist = 0.3f; p.ratFilter = 0.5f; p.ratLevel = 0.8f;  // light edge
    p.sdOn = false;  p.sdDrive = 0.5f; p.sdTone = 0.5f;    p.sdLevel = 0.7f;
    p.ampModel = kAmpTwin;  // Twin
    p.ampOn = true;  p.volume = 0.55f; p.bass = 0.5f; p.middle = 0.55f; p.treble = 0.6f;
    p.bright = true; p.cab = true;
    p.chorusMode = 0;            // no chorus on the Twin
    p.chorusSpeed = 0.6f;        // → tremolo SPEED (~5 Hz)
    p.chorusDepth = 0.6f;        // → tremolo INTENSITY
    p.reverb = 0.35f;            // period-correct spring reverb
    p.oversampling = 4;
    return p;
}

// The AC30 parameter set (M10.2) — the Vox "top boost" chime, exercising the full
// class-A chain (preamp gain stage + top-boost tone stack + PI/TOP CUT + EL84 quad +
// spring reverb) dual-mono into the same cab pair. RAT on as a light edge in front;
// the AC30's own VOLUME is the overdrive. Reuses the shared knobs — volume + bass/
// treble + reverb — and REUSES the presence field (jcmPresence) as the AC30's TOP CUT.
// The 'middle' slot is UNUSED (top-boost has no mid).
Params ac30Params() {
    Params p;
    p.inputTrim = 0.5f;
    p.ratOn = true;  p.ratDist = 0.3f; p.ratFilter = 0.5f; p.ratLevel = 0.8f;  // light edge
    p.sdOn = false;  p.sdDrive = 0.5f; p.sdTone = 0.5f;    p.sdLevel = 0.7f;
    p.ampModel = kAmpAc30;  // AC30
    p.ampOn = true;  p.volume = 0.7f;  p.bass = 0.5f; p.middle = 0.5f; p.treble = 0.6f;
    p.bright = false; p.cab = true;
    p.chorusMode = 0;            // no chorus on the AC30
    p.reverb = 0.25f;           // usability spring reverb
    p.jcmPresence = 0.3f;       // → AC30 TOP CUT (presence field reused)
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
// engine does, then render the whole signal block by block. Handles BOTH amp voices:
// the Clean 120 splits to a stereo pair (chorus); the JCM800 is a mono head whose
// single output is mirrored to both sides (dual-mono) ahead of the identical cabs.
void renderReference(const Params& p, const std::vector<float>& in,
                     std::vector<float>& outL, std::vector<float>& outR,
                     int& latencyOut) {
    using namespace clipper::dsp;
    const bool jcm800 = p.ampModel == kAmpJcm800;
    const bool twin = p.ampModel == kAmpTwin;
    const bool ac30 = p.ampModel == kAmpAc30;

    RatModel rat;
    SdModel sd;
    AmpModel amp;        // Clean 120
    Jcm800Amp jcm;       // JCM800 head
    TwinAmp twinAmp;     // Twin combo
    Ac30Amp ac30Amp;     // AC30 combo
    CabConvolver cabL, cabR;
    OutputLimiter limiter;

    rat.setParameter(RatModel::PARAM_DISTORTION, p.ratDist);
    rat.setParameter(RatModel::PARAM_FILTER, p.ratFilter);
    rat.setParameter(RatModel::PARAM_LEVEL, p.ratLevel);
    sd.setParameter(SdModel::PARAM_DRIVE, p.sdDrive);
    sd.setParameter(SdModel::PARAM_TONE, p.sdTone);
    sd.setParameter(SdModel::PARAM_LEVEL, p.sdLevel);

    if (jcm800) {
        // Mirror ClipperEngine::applyParams' JCM order exactly.
        jcm.setParameter(Jcm800Amp::PARAM_GAIN, p.jcmGain);
        jcm.setParameter(Jcm800Amp::PARAM_MASTER, p.jcmMaster);
        jcm.setParameter(Jcm800Amp::PARAM_BASS, p.bass);
        jcm.setParameter(Jcm800Amp::PARAM_MID, p.middle);
        jcm.setParameter(Jcm800Amp::PARAM_TREBLE, p.treble);
        jcm.setParameter(Jcm800Amp::PARAM_PRESENCE, p.jcmPresence);
        jcm.setParameter(Jcm800Amp::PARAM_REVERB, p.reverb);  // M10.1 usability add
    } else if (twin) {
        // Mirror ClipperEngine::applyParams' Twin routing exactly.
        twinAmp.setParameter(TwinAmp::PARAM_VOLUME, p.volume);
        twinAmp.setParameter(TwinAmp::PARAM_BASS, p.bass);
        twinAmp.setParameter(TwinAmp::PARAM_MID, p.middle);
        twinAmp.setParameter(TwinAmp::PARAM_TREBLE, p.treble);
        twinAmp.setParameter(TwinAmp::PARAM_BRIGHT, p.bright ? 1.0f : 0.0f);
        twinAmp.setParameter(TwinAmp::PARAM_REVERB, p.reverb);
        twinAmp.setParameter(TwinAmp::PARAM_SPEED, p.chorusSpeed);
        twinAmp.setParameter(TwinAmp::PARAM_INTENSITY, p.chorusDepth);
    } else if (ac30) {
        // Mirror ClipperEngine::applyParams' AC30 routing exactly: volume/bass/treble
        // + reverb from the shared knobs, and the presence field reused as TOP CUT.
        // The AC30 top-boost has NO mid — 'middle' is not routed.
        ac30Amp.setParameter(Ac30Amp::PARAM_VOLUME, p.volume);
        ac30Amp.setParameter(Ac30Amp::PARAM_BASS, p.bass);
        ac30Amp.setParameter(Ac30Amp::PARAM_TREBLE, p.treble);
        ac30Amp.setParameter(Ac30Amp::PARAM_TOPCUT, p.jcmPresence);  // presence → TOP CUT
        ac30Amp.setParameter(Ac30Amp::PARAM_REVERB, p.reverb);
    } else {
        amp.setParameter(AmpModel::PARAM_VOLUME, p.volume);
        amp.setParameter(AmpModel::PARAM_BASS, p.bass);
        amp.setParameter(AmpModel::PARAM_MIDDLE, p.middle);
        amp.setParameter(AmpModel::PARAM_TREBLE, p.treble);
        amp.setParameter(AmpModel::PARAM_BRIGHT, p.bright ? 1.0f : 0.0f);
        amp.setParameter(AmpModel::PARAM_CHORUS_SPEED, p.chorusSpeed);
        amp.setParameter(AmpModel::PARAM_CHORUS_DEPTH, p.chorusDepth);
        amp.setParameter(AmpModel::PARAM_CHORUS_MODE, static_cast<float>(p.chorusMode));
        amp.setParameter(AmpModel::PARAM_REVERB, p.reverb);
    }

    rat.prepare(kFs, kBlock);
    sd.prepare(kFs, kBlock);
    amp.prepare(kFs, kBlock);
    // The JCM runs at its fixed 4x internally (set BEFORE prepare so its stages size
    // to it), independent of the pedal OS selector — matches ClipperEngine.
    jcm.setOversampling(kJcmOversampling);
    jcm.prepare(kFs, kBlock);
    twinAmp.setOversampling(kTwinOversampling);
    twinAmp.prepare(kFs, kBlock);
    ac30Amp.setOversampling(kAc30Oversampling);
    ac30Amp.prepare(kFs, kBlock);
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
            if (jcm800) {
                jcm.process(cur, l.data(), n);                   // mono head
                for (int i = 0; i < n; ++i) r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];  // dual-mono
            } else if (twin) {
                twinAmp.process(cur, l.data(), n);               // mono combo
                for (int i = 0; i < n; ++i) r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];  // dual-mono
            } else if (ac30) {
                ac30Amp.process(cur, l.data(), n);               // mono combo
                for (int i = 0; i < n; ++i) r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];  // dual-mono
            } else {
                amp.processStereo(cur, l.data(), r.data(), n);   // stereo split
            }
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
                 (p.ampOn && jcm800 ? jcm.latencySamples() : 0) +
                 (p.ampOn && twin ? twinAmp.latencySamples() : 0) +
                 (p.ampOn && ac30 ? ac30Amp.latencySamples() : 0) +
                 (p.ampOn && p.cab ? cabL.latencySamples() : 0) +
                 limiter.latencySamples();
}

// Drive the REAL plugin over the same signal, mono in -> stereo out, for the given
// parameter set (either amp voice).
void renderPlugin(const Params& p, const std::vector<float>& in,
                  std::vector<float>& outL, std::vector<float>& outR,
                  int& latencyOut) {
    clipper::native::ClipperAudioProcessor proc;

    // Push the known set into the APVTS BEFORE prepareToPlay, so the engine snaps
    // its smoothers to these targets (steady state from sample 0). convertTo0to1
    // maps each param's real value (knob 0..1 / bool 0-1 / choice index) correctly.
    auto set = [&](const char* id, float realValue) {
        auto* param = proc.apvts.getParameter(id);
        param->setValueNotifyingHost(param->convertTo0to1(realValue));
    };
    using namespace clipper::native::pid;
    set(inputTrim, p.inputTrim);
    set(ratOn, p.ratOn ? 1.0f : 0.0f); set(ratDist, p.ratDist); set(ratFilter, p.ratFilter); set(ratLevel, p.ratLevel);
    set(sdOn, p.sdOn ? 1.0f : 0.0f);   set(sdDrive, p.sdDrive); set(sdTone, p.sdTone); set(sdLevel, p.sdLevel);
    set(ampOn, p.ampOn ? 1.0f : 0.0f);
    set(ampModel, static_cast<float>(p.ampModel));  // choice index == model id
    set(volume, p.volume); set(bass, p.bass); set(middle, p.middle); set(treble, p.treble);
    set(bright, p.bright ? 1.0f : 0.0f); set(cab, p.cab ? 1.0f : 0.0f);
    set(chorusMode, static_cast<float>(p.chorusMode));
    set(chorusSpeed, p.chorusSpeed); set(chorusDepth, p.chorusDepth);
    set(reverb, p.reverb);
    set(jcmGain, p.jcmGain); set(jcmMaster, p.jcmMaster); set(jcmPresence, p.jcmPresence);
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

// Run the full identical-core comparison for one parameter set (one amp voice).
// Returns true on PASS.
bool runCase(const char* label, const Params& p, const std::vector<float>& in) {
    std::printf("\n--- case: %s ---\n", label);

    std::vector<float> refL, refR, plL, plR;
    int refLat = 0, plLat = 0;
    renderReference(p, in, refL, refR, refLat);
    renderPlugin(p, in, plL, plR, plLat);

    // Secondary cross-check: render straight through ClipperEngine (no JUCE glue),
    // set once + prepared (snapped), and confirm it too is bit-exact against the
    // hand-written core reference — isolating the engine from the plugin wrapper.
    double engMax = 0.0;
    {
        clipper::native::ClipperEngine eng;
        eng.setParams(p);
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

    // Also confirm the RIGHT channel matches (proves the stereo path — chorus bloom
    // for the Clean 120, dual-mono for the JCM — is wired identically).
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

    if (ok) std::printf("PASS: %s output is sample-identical to the direct core chain.\n", label);
    return ok;
}

}  // namespace

int main() {
    std::printf("=== Clipper identical-core test ===\n");
    std::printf("signal: 220 Hz sine + pluck, %d samples @ %.0f Hz, block %d\n",
                kNumFrames, kFs, kBlock);

    const std::vector<float> in = makeSignal();

    bool ok = true;
    ok &= runCase("Clean 120", cleanParams(), in);
    ok &= runCase("JCM800", jcmParams(), in);
    ok &= runCase("Twin", twinParams(), in);
    ok &= runCase("AC30", ac30Params(), in);

    if (ok) {
        std::printf("\nPASS: all four amp voices are sample-identical across plugin + engine + core.\n");
        return 0;
    }
    std::printf("\nFAIL: identical-core mismatch (see cases above).\n");
    return 1;
}
