// Baseline: the shipped SOLA PitchShifter, measured with the shared battery.
// Every number the frequency-domain shifter is later judged against is taken
// here, on this machine, with this harness — not quoted from docs §70's prose.
#include "bench_shifter.h"
#include "clipper/dsp/PitchShifter.h"

#include <chrono>

using clipper::dsp::PitchShifter;

static bench::RenderFn solaRender(double windowSec) {
    return [windowSec](const std::vector<float>& in, double r) {
        PitchShifter ps;
        ps.prepare(windowSec, bench::kSr);
        ps.setRatio(r);
        std::vector<float> out(in.size());
        for (size_t i = 0; i < in.size(); ++i) out[i] = ps.process(in[i]);
        return out;
    };
}

static void selfTest() {
    // The estimator must be shown to SEE an error before it is trusted to report
    // one (docs §70 records two measurement bugs of exactly this family).
    const auto pure = bench::sustained({220.0}, 4.0);
    std::printf("  estimator, pure 220 Hz sine        : %+.4f cents\n",
                est::cents(est::freqNear(pure, 220.0, bench::kSr, 1.5), 220.0));
    const auto det = bench::sustained({220.0 * std::pow(2.0, 5.0 / 1200.0)}, 4.0);
    std::printf("  estimator, deliberate +5.000 detune: %+.4f cents\n",
                est::cents(est::freqNear(det, 220.0, bench::kSr, 1.5), 220.0));
    const auto tri = bench::sustained({82.41, 103.83, 123.47}, 4.0);
    double w = 0;
    for (double f : {82.41, 103.83, 123.47})
        w = std::max(w, std::fabs(est::cents(est::freqNear(tri, f, bench::kSr, 1.5), f)));
    std::printf("  estimator, unshifted triad worst   : %+.4f cents\n", w);
}

int main() {
    std::printf("=== estimator validation ===\n");
    selfTest();

    const auto render = solaRender(0.065);

    std::printf("\n=== SOLA baseline: pitch accuracy (worst |cents| / spread) ===\n");
    std::printf("%-16s", "shape");
    const int detents[] = {1, 2, 3, 5, 7, 12};
    for (int s : detents) std::printf("      -%-2d    ", s);
    std::printf("\n");
    for (const auto& shape : bench::kShapes) {
        std::printf("%-16s", shape.name);
        for (int s : detents) {
            const auto p = bench::pitchOf(render, shape, s);
            std::printf("%6.3f/%-6.3f", p.worstAbs, p.spread);
        }
        std::printf("\n");
    }

    std::printf("\n=== SOLA baseline: artifact floor (non-harmonic re harmonic) ===\n");
    for (int s : {1, 5, 12})
        std::printf("  -%-2d semitones : %7.2f dB\n", s, bench::artifactFloorDb(render, s));

    std::printf("\n=== SOLA baseline: transient (plucked low E) ===\n");
    for (int s : {1, 12}) {
        const auto t = bench::transientOf(render, s);
        std::printf("  -%-2d semitones : rise %6.2f ms   peak %.4f x input\n", s, t.riseMs,
                    t.peakRatio);
    }

    {
        PitchShifter ps;
        ps.prepare(0.065, bench::kSr);
        std::printf("\n=== SOLA baseline: latency & cost ===\n");
        std::printf("  analytic mean read delay : %6.2f ms\n",
                    ps.meanDelaySamples() / bench::kSr * 1000.0);
        const auto x = bench::sustained({82.41}, 10.0);
        const auto t0 = std::chrono::steady_clock::now();
        auto y = render(x, std::pow(2.0, -1.0 / 12.0));
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  10 s render              : %6.3f s = %.2f%% of one 48 kHz stream\n",
                    sec, sec / 10.0 * 100.0);
        (void)y;
    }
    return 0;
}
