// The frequency-domain candidate, measured with the SAME battery as the SOLA
// baseline so the two are comparable on this machine.
#include "bench_shifter.h"
#include "PhaseVocoderShifter.h"

#include <chrono>

using clipper::dsp::PhaseVocoderShifter;

static bench::RenderFn pvRender(int n) {
    return [n](const std::vector<float>& in, double r) {
        PhaseVocoderShifter pv;
        pv.prepare(n, bench::kSr);
        pv.setRatio(r);
        std::vector<float> out(in.size());
        for (size_t i = 0; i < in.size(); ++i) out[i] = pv.process(in[i]);
        return out;
    };
}

// Unity transparency + measured group delay: at r == 1 a correct STFT is the
// input delayed by n - hop. If it is not, the OLA placement is wrong and every
// other number below is measuring a bug.
static void unityCheck(int n) {
    PhaseVocoderShifter pv;
    pv.prepare(n, bench::kSr);
    pv.setRatio(1.0);
    const auto x = bench::pluck(220.0, 1.5);
    std::vector<float> y(x.size());
    for (size_t i = 0; i < x.size(); ++i) y[i] = pv.process(x[i]);
    int bestLag = 0;
    double best = -1e30;
    for (int lag = 0; lag <= n; ++lag) {
        double c = 0;
        for (size_t i = 0; i + (size_t)lag < x.size(); ++i) c += (double)x[i] * y[i + lag];
        if (c > best) { best = c; bestLag = lag; }
    }
    double num = 0, den = 0;
    for (size_t i = 0; i + (size_t)bestLag < x.size(); ++i) {
        const double d = (double)y[i + bestLag] - (double)x[i];
        num += d * d;
        den += (double)x[i] * x[i];
    }
    std::printf("  N=%-5d unity: lag %4d (%5.2f ms, reported %5.2f)  residual %6.2f dB\n", n,
                bestLag, bestLag / bench::kSr * 1000.0,
                pv.latencySamples() / bench::kSr * 1000.0, 10.0 * std::log10(num / den + 1e-30));
}

int main() {
    std::printf("=== PV sanity: unity transparency & group delay ===\n");
    for (int n : {2048, 4096, 8192}) unityCheck(n);

    const int detents[] = {1, 2, 5, 12};
    for (int n : {2048, 4096, 8192}) {
        const auto render = pvRender(n);
        std::printf("\n=== PV N=%d (%.1f ms window, %.1f Hz bins) ===\n", n,
                    n / bench::kSr * 1000.0, bench::kSr / n);
        std::printf("  latency %.2f ms\n", (double)(n - n / 4) / bench::kSr * 1000.0);
        std::printf("  %-16s", "pitch (worst/spread)");
        for (int s : detents) std::printf("      -%-2d    ", s);
        std::printf("\n");
        for (const auto& shape : bench::kShapes) {
            std::printf("  %-16s", shape.name);
            for (int s : detents) {
                const auto p = bench::pitchOf(render, shape, s);
                std::printf("%6.3f/%-6.3f", p.worstAbs, p.spread);
            }
            std::printf("\n");
        }
        for (int s : {1, 5, 12})
            std::printf("  artifact floor -%-2d : %7.2f dB\n", s, bench::artifactFloorDb(render, s));
        for (int s : {1, 12}) {
            const auto t = bench::transientOf(render, s);
            std::printf("  transient      -%-2d : rise %6.2f ms   peak %.4f x input\n", s, t.riseMs,
                        t.peakRatio);
        }
        const auto x = bench::sustained({82.41}, 10.0);
        const auto t0 = std::chrono::steady_clock::now();
        auto y = render(x, std::pow(2.0, -1.0 / 12.0));
        const double sec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  cpu                : %.3f s / 10 s = %.2f%% of one stream\n", sec,
                    sec / 10.0 * 100.0);
        (void)y;
    }
    return 0;
}
