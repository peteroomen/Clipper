// Clipper — tube-solver early-exit regression test (solver-perf pass, docs §25).
//
// The valve solvers (TriodeStage / cathode follower, LtpInverter, the pentode
// plate & grid Newtons) exit at PRODUCTION tolerances chosen for speed. This test
// pins the accuracy contract behind that choice: rendering the same program
// material with every tolerance tightened 1000x ("reference mode", the converged
// ground truth — same equations, same solver, tighter exit) must agree with the
// production render to below -120 dBFS. Any future solver shortcut — looser exit,
// cheaper transcendental, different warm start — has to keep this gate green.
//
// Program material: a riff-like burst (plucked fundamentals + harmonics, sharp
// attacks, full decays) that drives each amp through clean, clipping and recovery,
// through each of the three valve amp voices (jcm800, twin, ac30) at working
// gain settings.
//
// Plain-assert (no framework); -UNDEBUG keeps asserts live in Release.

#include "clipper/dsp/Ac30Amp.h"
#include "clipper/dsp/Jcm800Amp.h"
#include "clipper/dsp/TubeSolverMode.h"
#include "clipper/dsp/TwinAmp.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 128;
constexpr double kSeconds = 1.25;
constexpr double kGateDb = -120.0;  // residual gate, dBFS (1.0 == full scale)

// Deterministic riff: four plucked notes (fundamental + harmonics, sharp attack,
// exponential decay) peaking ~0.3 — a hot guitar DI. Exercises attack transients
// (solver stress), sustained clipping, and decaying tails.
std::vector<float> makeRiff() {
    const int n = static_cast<int>(kSeconds * kFs);
    std::vector<float> s(static_cast<size_t>(n));
    const double notes[4] = {110.0, 146.83, 196.0, 246.94};
    for (int i = 0; i < n; ++i) {
        const double t = i / kFs;
        const int pluck = static_cast<int>(t / 0.3);
        const double tp = t - pluck * 0.3;
        const double f0 = notes[pluck % 4];
        const double env = std::exp(-tp / 0.25) * (1.0 - std::exp(-tp / 0.002));
        double v = 0.0;
        for (int h = 1; h <= 5; ++h) v += std::sin(2.0 * M_PI * f0 * h * t) / h;
        s[static_cast<size_t>(i)] = static_cast<float>(0.20 * env * v);
    }
    return s;
}

// Render the riff through a freshly-built amp; renderer builds/configures/renders
// so production and reference renders use IDENTICAL construction paths.
template <typename MakeAmp>
std::vector<float> render(const std::vector<float>& in, MakeAmp&& makeAmp) {
    auto amp = makeAmp();  // std::unique_ptr-like or value with process()
    std::vector<float> out(in.size(), 0.0f);
    const int n = static_cast<int>(in.size());
    for (int off = 0; off < n; off += kBlock) {
        const int m = std::min(kBlock, n - off);
        amp->process(in.data() + off, out.data() + off, m);
    }
    return out;
}

template <typename MakeAmp>
void checkAmp(const char* name, const std::vector<float>& riff, MakeAmp&& makeAmp) {
    using clipper::dsp::setTubeSolverReferenceMode;

    setTubeSolverReferenceMode(false);
    const std::vector<float> prod = render(riff, makeAmp);

    setTubeSolverReferenceMode(true);
    const std::vector<float> ref = render(riff, makeAmp);
    setTubeSolverReferenceMode(false);

    double peak = 0.0, diff = 0.0;
    for (size_t i = 0; i < prod.size(); ++i) {
        peak = std::max(peak, static_cast<double>(std::fabs(ref[i])));
        diff = std::max(diff, static_cast<double>(std::fabs(prod[i] - ref[i])));
    }
    assert(peak > 0.01 && "amp rendered near-silence; test signal broken");
    const double diffDb = diff > 0.0 ? 20.0 * std::log10(diff) : -400.0;
    std::printf("  [%s] out peak %.3f, prod-vs-reference residual %.1f dBFS (gate %.0f)\n",
                name, peak, diffDb, kGateDb);
    assert(diffDb <= kGateDb &&
           "production solver tolerances drifted above the -120 dBFS gate");
}

}  // namespace

int main() {
    std::printf("Running tube-solver early-exit regression (production vs 1000x-tight)...\n");
    const std::vector<float> riff = makeRiff();

    checkAmp("jcm800", riff, [] {
        auto a = std::make_unique<clipper::dsp::Jcm800Amp>();
        a->prepare(kFs, kBlock);
        a->setParameter(clipper::dsp::Jcm800Amp::PARAM_GAIN, 0.7f);
        a->setParameter(clipper::dsp::Jcm800Amp::PARAM_MASTER, 0.6f);
        return a;
    });
    checkAmp("twin", riff, [] {
        auto a = std::make_unique<clipper::dsp::TwinAmp>();
        a->prepare(kFs, kBlock);
        a->setParameter(clipper::dsp::TwinAmp::PARAM_VOLUME, 0.6f);
        return a;
    });
    checkAmp("ac30", riff, [] {
        auto a = std::make_unique<clipper::dsp::Ac30Amp>();
        a->prepare(kFs, kBlock);
        a->setParameter(clipper::dsp::Ac30Amp::PARAM_VOLUME, 0.6f);
        return a;
    });

    std::printf("All tube-solver regression checks passed.\n");
    return 0;
}
