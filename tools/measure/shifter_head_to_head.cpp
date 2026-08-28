// Head to head: the shipped SOLA PitchShifter vs the frequency-domain candidate,
// identical stimuli, identical estimator, same machine. This is the table the
// slice's decision is made on.
#include "bench_shifter.h"
#include "PhaseVocoderShifter.h"
#include "clipper/dsp/PitchShifter.h"

#include <chrono>

using clipper::dsp::PhaseVocoderShifter;
using clipper::dsp::PitchShifter;

static bench::RenderFn sola(double win) {
    return [win](const std::vector<float>& in, double r) {
        PitchShifter s;
        s.prepare(win, bench::kSr);
        s.setRatio(r);
        std::vector<float> out(in.size());
        for (size_t i = 0; i < in.size(); ++i) out[i] = s.process(in[i]);
        return out;
    };
}
static bench::RenderFn pv(int n) {
    return [n](const std::vector<float>& in, double r) {
        PhaseVocoderShifter s;
        s.prepare(n, bench::kSr);
        s.setRatio(r);
        std::vector<float> out(in.size());
        for (size_t i = 0; i < in.size(); ++i) out[i] = s.process(in[i]);
        return out;
    };
}

// TRUE latency, measured the only way that is valid for both: the lag that
// aligns the OUTPUT ENVELOPE of a plucked note with the input's. A vocoder is
// magnitude-transparent but NOT waveform-transparent, so a waveform correlation
// reports nothing for it (measured: best lag 85-168 samples at a -3.7 dB
// residual, i.e. no alignment exists).
static double envLatencyMs(const bench::RenderFn& render, double r, int maxLag) {
    const auto x = bench::pluck(82.41, 2.0);
    const auto y = render(x, r);
    auto env = [](const std::vector<float>& v) {
        std::vector<double> e(v.size());
        double s = 0;
        const double a = 1.0 - std::exp(-1.0 / (0.002 * bench::kSr));
        for (size_t i = 0; i < v.size(); ++i) { s += a * (std::fabs((double)v[i]) - s); e[i] = s; }
        return e;
    };
    const auto ex = env(x), ey = env(y);
    double best = -1e30;
    int bestLag = 0;
    for (int lag = 0; lag <= maxLag; ++lag) {
        double c = 0;
        for (size_t i = 0; i + (size_t)lag < ex.size(); ++i) c += ex[i] * ey[i + lag];
        if (c > best) { best = c; bestLag = lag; }
    }
    return bestLag / bench::kSr * 1000.0;
}

static void table(const char* name, const bench::RenderFn& render, int maxLag) {
    const int detents[] = {1, 2, 5, 7, 12};
    std::printf("\n--- %s ---\n", name);
    std::printf("  %-16s", "pitch worst/spread");
    for (int s : detents) std::printf("       -%-2d     ", s);
    std::printf("\n");
    for (const auto& shape : bench::kShapes) {
        std::printf("  %-16s", shape.name);
        for (int s : detents) {
            const auto p = bench::pitchOf(render, shape, s);
            std::printf("%7.3f/%-7.3f", p.worstAbs, p.spread);
        }
        std::printf("\n");
    }
    std::printf("  artifact floor   ");
    for (int s : {1, 5, 12}) std::printf(" -%d: %7.2f dB ", s, bench::artifactFloorDb(render, s));
    std::printf("\n  transient        ");
    for (int s : {1, 12}) {
        const auto t = bench::transientOf(render, s);
        std::printf(" -%d: rise %6.2f ms peak %.3fx ", s, t.riseMs, t.peakRatio);
    }
    std::printf("\n  true latency (envelope, -1 / -12) : %6.2f / %6.2f ms\n",
                envLatencyMs(render, std::pow(2.0, -1.0 / 12.0), maxLag),
                envLatencyMs(render, 0.5, maxLag));
    const auto x = bench::sustained({82.41}, 10.0);
    const auto t0 = std::chrono::steady_clock::now();
    auto y = render(x, std::pow(2.0, -1.0 / 12.0));
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  cpu : %.3f s / 10 s = %.2f%% of one 48 kHz stream\n", sec, sec / 10.0 * 100.0);
    (void)y;
}

int main() {
    table("SOLA (shipped, W=65 ms)", sola(0.065), 8000);
    table("PV N=4096 (85.3 ms window)", pv(4096), 16000);
    table("PV N=8192 (170.7 ms window)", pv(8192), 24000);
    return 0;
}
