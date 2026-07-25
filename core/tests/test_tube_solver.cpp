// Clipper — solver early-exit regression test (solver-perf pass, docs §25, §34).
//
// The valve solvers (TriodeStage / cathode follower, LtpInverter, the pentode
// plate & grid Newtons) exit at PRODUCTION tolerances chosen for speed, and since
// audit finding 12 so does the BJT solver (BjtStage, the Muff's four stages). This
// test pins the accuracy contract behind those choices: rendering the same program
// material with every tolerance tightened 1000x ("reference mode", the converged
// ground truth — same equations, same solver, tighter exit) must agree with the
// production render to below -120 dBFS. Any future solver shortcut — looser exit,
// cheaper transcendental, different warm start — has to keep this gate green.
//
// For the MUFF this gate is not a formality — it is the whole justification for the
// residual early-out, and it is load-bearing in a way the amp rows are not.
//
// BjtStage's early-out is at 1e-17 A, and reference mode scales that to 1e-20 A,
// BELOW the measured idle residual ceiling of 2.06e-18 A. So in reference mode the
// early-out never fires, which means the reference render here is exactly the
// pre-fix solver: this test measures the early-out's true audio cost on every run,
// not a proxy for it. Measured -127.4 dBFS absolute / -134.1 dB relative, i.e. 7.4 dB
// inside the gate.
//
// Bit-identity was attempted first and is NOT achievable: the pre-fix solver drove
// the residual to the floating-point floor, so any early-out that fires at all
// declines refinement it performed. A sweep of the tolerance (recorded in
// BjtStage.cpp) shows bit-identity only at 1e-19, where the early-out never fires
// and the 3x idle-CPU pathology returns. The dB gate is therefore the honest bar
// here, and the sweep table is what keeps that from being a convenient excuse.
//
// Program material: a riff-like burst (plucked fundamentals + harmonics, sharp
// attacks, full decays) that drives each unit through clean, clipping and recovery,
// through each of the three valve amp voices (jcm800, twin, ac30) at working gain
// settings, and the Muff at MIN / working / MAX sustain.
//
// Plain-assert (no framework); -UNDEBUG keeps asserts live in Release.

#include "clipper/dsp/Ac30Amp.h"
#include "clipper/dsp/Jcm800Amp.h"
#include "clipper/dsp/MuffModel.h"
#include "clipper/dsp/TubeSolverMode.h"
#include "clipper/dsp/TwinAmp.h"

#include "support/AssertsLive.h"

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

// The Muff is a PEDAL, not an amp: process() rather than the amp block API, and its
// own construction path. Same -120 dBFS gate; see the file header for why reference
// mode here is literally the pre-fix solver, which makes this the real measurement
// of the residual early-out's audio cost.
// `inGain` scales the riff. It is a real knob on the result, not decoration: the
// early-out's cost was swept across five input levels and the WORST case is not the
// hot-DI level the amp rows use — it is a LOUD input (riff x5, ~1.5 peak) at sustain
// 0.70, which measures 19 dB worse than the same setting at hot-DI level. A version
// of this test that only ever drove hot DI would have reported -228 dBFS and missed
// the number that actually matters. Silence and a ±20 V slam were also swept: both
// are exactly 0 (parked, and never converged-to-tolerance, respectively).
void checkMuff(const char* name, const std::vector<float>& riff, float sustain,
               float inGain) {
    using clipper::dsp::setTubeSolverReferenceMode;

    std::vector<float> in(riff.size(), 0.0f);
    for (size_t i = 0; i < riff.size(); ++i) in[i] = riff[i] * inGain;

    auto renderMuff = [&]() {
        clipper::dsp::MuffModel m;
        m.prepare(kFs, kBlock);
        m.setParameter(clipper::dsp::MuffModel::PARAM_SUSTAIN, sustain);
        m.setParameter(clipper::dsp::MuffModel::PARAM_TONE, 0.5f);
        m.setParameter(clipper::dsp::MuffModel::PARAM_VOLUME, 0.8f);  // NOT optional
        std::vector<float> out(in.size(), 0.0f);
        const int n = static_cast<int>(in.size());
        for (int off = 0; off < n; off += kBlock) {
            const int c = std::min(kBlock, n - off);
            m.process(in.data() + off, out.data() + off, c);
        }
        return out;
    };

    setTubeSolverReferenceMode(false);
    const std::vector<float> prod = renderMuff();
    setTubeSolverReferenceMode(true);
    const std::vector<float> ref = renderMuff();
    setTubeSolverReferenceMode(false);

    double peak = 0.0, diff = 0.0;
    size_t differing = 0;
    for (size_t i = 0; i < prod.size(); ++i) {
        peak = std::max(peak, static_cast<double>(std::fabs(ref[i])));
        diff = std::max(diff, static_cast<double>(std::fabs(prod[i] - ref[i])));
        if (prod[i] != ref[i]) ++differing;
    }
    // Not a formality: PARAM_VOLUME defaults to 0, so a Muff driven only through
    // PARAM_SUSTAIN renders digital silence and every difference metric below reads
    // a perfect 0. That exact mistake made a whole bit-identity sweep vacuous during
    // the §34 slice before this line caught it. Keep it.
    assert(peak > 0.01 && "muff rendered near-silence; test signal broken");
    const double diffDb = diff > 0.0 ? 20.0 * std::log10(diff) : -400.0;
    const double relDb = diff > 0.0 ? 20.0 * std::log10(diff / peak) : -400.0;
    std::printf("  [%s] out peak %.3f, prod-vs-reference residual %.1f dBFS "
                "(%.1f dB rel, %zu/%zu samples differ, gate %.0f)\n",
                name, peak, diffDb, relDb, differing, prod.size(), kGateDb);
    assert(diffDb <= kGateDb &&
           "the Muff's residual early-out (kNewtonResidualTolA) now costs more than "
           "-120 dBFS of accuracy — it is truncating real Newton work (docs §34)");
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();
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

    // The BJT solver (audit finding 12). MIN and MAX sustain are here because they
    // are the ends of the drive range the early-out has to hold at: MIN is the
    // quietest case, and therefore the one whose genuine-work residuals sit closest
    // to the tolerance, while MAX is the most nonlinear.
    checkMuff("muff sus MIN  hot ", riff, 0.0f, 1.0f);
    checkMuff("muff sus 0.60 hot ", riff, 0.6f, 1.0f);
    checkMuff("muff sus MAX  hot ", riff, 1.0f, 1.0f);
    // The measured worst case of the whole sweep. Keep all three loud rows: the
    // early-out's cost is not monotonic in sustain, so MIN/mid/MAX each has to be
    // driven loud rather than trusting 0.70 to be the permanent worst setting.
    checkMuff("muff sus MIN  LOUD", riff, 0.0f, 5.0f);
    checkMuff("muff sus 0.70 LOUD", riff, 0.7f, 5.0f);
    checkMuff("muff sus MAX  LOUD", riff, 1.0f, 5.0f);

    std::printf("All solver early-exit regression checks passed.\n");
    return 0;
}
