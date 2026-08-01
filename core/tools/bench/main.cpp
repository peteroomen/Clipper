// clipper-bench — native per-unit CPU cost benchmark (docs §25). NOT part of the
// WASM build. Times every pedal / amp / cab / reverb unit over a deterministic
// riff-like program signal at 48 kHz in 128-frame blocks (the AudioWorklet render
// quantum) and reports the realtime multiple (audio seconds rendered per wall
// second) plus the CPU share of one realtime stream. Native numbers are a PROXY
// for WASM (same source, no SIMD divergence in the core); the relative ranking is
// what the table in docs §25 uses. Deterministic input, so runs are comparable
// across commits; wall-clock noise is ~a few percent — bench on an idle machine.
//
// Usage: clipper-bench [--seconds N] [--unit NAME]   (default 10 s, all units)

#include "clipper/dsp/Ac30Amp.h"
#include "clipper/dsp/AmpModel.h"
#include "clipper/dsp/CabConvolver.h"
#include "clipper/dsp/CabIR.h"
#include "clipper/dsp/Jcm800Amp.h"
#include "clipper/dsp/CompModel.h"
#include "clipper/dsp/DelayModel.h"
#include "clipper/dsp/GoldModel.h"
#include "clipper/dsp/MuffModel.h"
#include "clipper/dsp/OutputLimiter.h"
#include "clipper/dsp/Oversampler.h"
#include "clipper/dsp/PhaserModel.h"
#include "clipper/dsp/RatModel.h"
#include "clipper/dsp/ReverbModel.h"
#include "clipper/dsp/SdModel.h"
#include "clipper/dsp/TsModel.h"
#include "clipper/dsp/TwinAmp.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 128;

// Deterministic "riff": a cycle of four plucked notes (fundamental + 4 harmonics,
// sharp attack, exponential decay) peaking around 0.3 — a hot guitar DI level.
// Includes full decays to silence so denormal-prone tails are exercised too.
std::vector<float> makeRiff(double seconds) {
    const int n = static_cast<int>(seconds * kSr);
    std::vector<float> s(static_cast<size_t>(n), 0.0f);
    const double notes[4] = {110.0, 146.83, 196.0, 246.94};
    const double noteLen = 0.5;  // s between plucks
    for (int i = 0; i < n; ++i) {
        const double t = i / kSr;
        const int pluck = static_cast<int>(t / noteLen);
        const double tp = t - pluck * noteLen;  // time since pluck onset
        const double f0 = notes[pluck % 4];
        const double env = std::exp(-tp / 0.35) * (1.0 - std::exp(-tp / 0.002));
        double v = 0.0;
        for (int h = 1; h <= 5; ++h)
            v += std::sin(2.0 * M_PI * f0 * h * t) / h;
        s[static_cast<size_t>(i)] = static_cast<float>(0.20 * env * v);
    }
    return s;
}

struct Result { std::string name; double xRealtime, cpuPct; };

Result benchUnit(const std::string& name, const std::vector<float>& riff,
                 const std::function<void(const float*, float*, int)>& proc) {
    std::vector<float> out(static_cast<size_t>(kBlock), 0.0f);
    const int n = static_cast<int>(riff.size());
    // Warm-up: one full pass (converges solver warm starts, faults in memory).
    for (int off = 0; off + kBlock <= n; off += kBlock)
        proc(riff.data() + off, out.data(), kBlock);
    const auto t0 = std::chrono::steady_clock::now();
    for (int off = 0; off + kBlock <= n; off += kBlock)
        proc(riff.data() + off, out.data(), kBlock);
    const auto t1 = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(t1 - t0).count();
    const double audio = n / kSr;
    Result r{name, audio / wall, 100.0 * wall / audio};
    std::printf("  %-28s %7.2fx realtime   %6.2f%% of one stream\n",
                r.name.c_str(), r.xRealtime, r.cpuPct);
    std::fflush(stdout);
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    double seconds = 10.0;
    std::string only;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--unit") && i + 1 < argc) only = argv[++i];
    }
    const std::vector<float> riff = makeRiff(seconds);
    auto want = [&](const char* n) { return only.empty() || only == n; };

    std::printf("clipper-bench: %.1f s riff @ %.0f Hz, %d-frame blocks\n",
                seconds, kSr, kBlock);

    using clipper::dsp::AmpModel;

    // --- Pedals (worklet defaults: 4x oversampling where applicable) ----------
    if (want("rat")) {
        clipper::dsp::RatModel m;
        m.prepare(kSr, kBlock);
        m.setParameter(clipper::dsp::RatModel::PARAM_DISTORTION, 0.7f);
        m.setParameter(clipper::dsp::RatModel::PARAM_FILTER, 0.4f);
        m.setParameter(clipper::dsp::RatModel::PARAM_LEVEL, 0.8f);
        benchUnit("rat (dist pedal)", riff,
                  [&](const float* i, float* o, int n) { m.process(i, o, n); });
    }
    if (want("sd1")) {
        clipper::dsp::SdModel m;
        m.prepare(kSr, kBlock);
        benchUnit("sd1 (overdrive)", riff,
                  [&](const float* i, float* o, int n) { m.process(i, o, n); });
    }
    if (want("ts")) {
        clipper::dsp::TsModel m;
        m.prepare(kSr, kBlock);
        benchUnit("screamer (overdrive)", riff,
                  [&](const float* i, float* o, int n) { m.process(i, o, n); });
    }
    if (want("muff")) {
        clipper::dsp::MuffModel m;
        m.prepare(kSr, kBlock);
        m.setParameter(clipper::dsp::MuffModel::PARAM_SUSTAIN, 0.7f);
        benchUnit("muff (BJT fuzz)", riff,
                  [&](const float* i, float* o, int n) { m.process(i, o, n); });
    }
    if (want("gold")) {
        clipper::dsp::GoldModel m;
        m.prepare(kSr, kBlock);
        m.setParameter(clipper::dsp::GoldModel::PARAM_GAIN, 0.35f);
        m.setParameter(clipper::dsp::GoldModel::PARAM_TREBLE, 0.5f);
        m.setParameter(clipper::dsp::GoldModel::PARAM_OUTPUT, 0.7f);
        benchUnit("gold (clean-blend drive)", riff,
                  [&](const float* i, float* o, int n) { m.process(i, o, n); });
    }
    if (want("comp")) {
        clipper::dsp::CompModel m;
        m.prepare(kSr, kBlock);
        m.setParameter(clipper::dsp::CompModel::PARAM_SUSTAIN, 0.5f);
        m.setParameter(clipper::dsp::CompModel::PARAM_LEVEL, 0.4f);
        benchUnit("squash (OTA compressor)", riff,
                  [&](const float* i, float* o, int n) { m.process(i, o, n); });
    }
    if (want("delay")) {
        clipper::dsp::DelayModel m;
        m.prepare(kSr, kBlock);
        m.setParameter(clipper::dsp::DelayModel::PARAM_DELAY, 0.35f);
        m.setParameter(clipper::dsp::DelayModel::PARAM_FEEDBACK, 0.3f);
        m.setParameter(clipper::dsp::DelayModel::PARAM_BLEND, 0.35f);
        benchUnit("echoman (BBD analog delay)", riff,
                  [&](const float* i, float* o, int n) { m.process(i, o, n); });
    }
    if (want("phaser")) {
        clipper::dsp::PhaserModel m;
        m.prepare(kSr);
        benchUnit("ninety (phaser)", riff,
                  [&](const float* i, float* o, int n) { m.process(i, o, n); });
    }

    // --- Resampling in isolation (docs §32) ----------------------------------
    // Every oversampled unit above pays this cost, and in the valve amps it is
    // paid once PER TRIODE STAGE, so the halfband inner loop is the hottest loop
    // in the project. Benching it alone keeps a resampler change from being
    // diluted (or hidden) by whatever nonlinearity sits inside the domain: the
    // in-domain work here is one multiply per oversampled sample, so ~all of the
    // time is up/down filtering. 4x is the shipped default for every nonlinear
    // stage; 8x is the worst case the scratch is sized for.
    for (const int factor : {2, 4, 8}) {
        char name[32];
        std::snprintf(name, sizeof(name), "os%dx (up+down only)", factor);
        char sel[8];
        std::snprintf(sel, sizeof(sel), "os%d", factor);
        if (!want(sel)) continue;
        clipper::dsp::Oversampler os;
        os.prepare(kBlock);
        os.setFactor(factor);
        benchUnit(name, riff, [&](const float* i, float* o, int n) {
            os.upsample(i, n);
            float* w = os.buffer();
            const int m = os.bufferLength();
            for (int k = 0; k < m; ++k) w[k] *= 0.9f;
            os.downsample(o, n);
        });
    }

    // --- Amps ----------------------------------------------------------------
    if (want("clean")) {
        AmpModel m;
        m.prepare(kSr, kBlock);
        benchUnit("clean amp (JC-120, mono)", riff,
                  [&](const float* i, float* o, int n) { m.process(i, o, n); });
    }
    if (want("clean-fx")) {
        AmpModel m;
        m.prepare(kSr, kBlock);
        m.setParameter(AmpModel::PARAM_CHORUS_MODE, 1.0f);
        m.setParameter(AmpModel::PARAM_CHORUS_DEPTH, 0.6f);
        m.setParameter(AmpModel::PARAM_REVERB, 0.4f);
        std::vector<float> outR(static_cast<size_t>(kBlock));
        benchUnit("clean amp + chorus + reverb", riff,
                  [&](const float* i, float* o, int n) { m.processStereo(i, o, outR.data(), n); });
    }
    if (want("jcm800")) {
        clipper::dsp::Jcm800Amp m;
        m.prepare(kSr, kBlock);
        m.setParameter(clipper::dsp::Jcm800Amp::PARAM_GAIN, 0.6f);
        m.setParameter(clipper::dsp::Jcm800Amp::PARAM_MASTER, 0.5f);
        benchUnit("jcm800 (valve amp)", riff,
                  [&](const float* i, float* o, int n) { m.process(i, o, n); });
    }
    if (want("twin")) {
        clipper::dsp::TwinAmp m;
        m.prepare(kSr, kBlock);
        m.setParameter(clipper::dsp::TwinAmp::PARAM_VOLUME, 0.5f);
        benchUnit("twin (valve amp)", riff,
                  [&](const float* i, float* o, int n) { m.process(i, o, n); });
    }
    if (want("ac30")) {
        clipper::dsp::Ac30Amp m;
        m.prepare(kSr, kBlock);
        m.setParameter(clipper::dsp::Ac30Amp::PARAM_VOLUME, 0.5f);
        benchUnit("ac30 (valve amp)", riff,
                  [&](const float* i, float* o, int n) { m.process(i, o, n); });
    }

    // --- Cabs / reverb / limiter --------------------------------------------
    if (want("cab212")) {
        clipper::dsp::CabConvolver m;
        auto ir = clipper::dsp::generateDefaultCab2x12IR(kSr);
        m.prepare(kSr, ir.data(), static_cast<int>(ir.size()), kSr, kBlock);
        benchUnit("cab clean212 (convolver)", riff,
                  [&](const float* i, float* o, int n) { m.process(i, o, n); });
    }
    if (want("cab412")) {
        clipper::dsp::CabConvolver m;
        auto ir = clipper::dsp::generateBrit4x12IR(kSr);
        m.prepare(kSr, ir.data(), static_cast<int>(ir.size()), kSr, kBlock);
        benchUnit("cab brit412 (convolver)", riff,
                  [&](const float* i, float* o, int n) { m.process(i, o, n); });
    }
    if (want("reverb")) {
        clipper::dsp::ReverbModel m;
        m.prepare(kSr);
        m.setMix(0.5f);
        benchUnit("spring reverb (mix 0.5)", riff,
                  [&](const float* i, float* o, int n) { m.process(i, o, n); });
    }
    if (want("limiter")) {
        clipper::dsp::OutputLimiter m;
        m.prepare(kSr);
        benchUnit("output limiter", riff, [&](const float* i, float* o, int n) {
            std::memcpy(o, i, static_cast<size_t>(n) * sizeof(float));
            m.processMono(o, n);
        });
    }

    return 0;
}
