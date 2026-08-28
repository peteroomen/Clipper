// Attribution probes for the frequency-domain candidate: is the octave blow-up
// at N=8192 the shifter's or the estimator's, and does the obvious relocation
// improvement (2-bin magnitude split) change the verdict?
#include "bench_shifter.h"
#include "PhaseVocoderShifter.h"

using clipper::dsp::PhaseVocoderShifter;

static std::vector<float> pv(int n, const std::vector<float>& in, double r) {
    PhaseVocoderShifter s;
    s.prepare(n, bench::kSr);
    s.setRatio(r);
    std::vector<float> out(in.size());
    for (size_t i = 0; i < in.size(); ++i) out[i] = s.process(in[i]);
    return out;
}

int main() {
    std::printf("=== unity residual, search widened to 2N, startup skipped ===\n");
    for (int n : {2048, 4096, 8192}) {
        PhaseVocoderShifter s;
        s.prepare(n, bench::kSr);
        s.setRatio(1.0);
        const auto x = bench::pluck(220.0, 3.0);
        std::vector<float> y(x.size());
        for (size_t i = 0; i < x.size(); ++i) y[i] = s.process(x[i]);
        const size_t skip = (size_t)(2 * n);
        int bestLag = 0;
        double best = -1e30;
        for (int lag = 0; lag <= 2 * n; ++lag) {
            double c = 0;
            for (size_t i = skip; i + (size_t)lag < x.size(); ++i) c += (double)x[i] * y[i + lag];
            if (c > best) { best = c; bestLag = lag; }
        }
        double num = 0, den = 0;
        for (size_t i = skip; i + (size_t)bestLag < x.size(); ++i) {
            const double d = (double)y[i + bestLag] - (double)x[i];
            num += d * d;
            den += (double)x[i] * x[i];
        }
        std::printf("  N=%-5d lag %5d (%6.2f ms, reported %6.2f)  residual %7.2f dB\n", n, bestLag,
                    bestLag / bench::kSr * 1000.0, (double)n / bench::kSr * 1000.0,
                    10.0 * std::log10(num / den + 1e-30));
    }

    std::printf("\n=== the N=8192 octave blow-up: per-partial, not aggregated ===\n");
    const double parts[] = {82.41, 103.83, 123.47};
    for (int semis : {5, 7, 12}) {
        const double r = std::pow(2.0, -(double)semis / 12.0);
        const auto y = pv(8192, bench::sustained({parts[0], parts[1], parts[2]}, 6.0), r);
        std::printf("  -%-2d semitones (r=%.6f):\n", semis, r);
        for (double f : parts) {
            const double t = f * r;
            const double m = est::freqNear(y, t, bench::kSr, 2.5);
            std::printf("    partial %6.2f -> target %6.3f Hz, measured %8.3f Hz  (%+9.3f cents)\n",
                        f, t, m, est::cents(m, t));
        }
    }

    std::printf("\n=== is the octave error real? spectrum around each target ===\n");
    {
        const double r = 0.5;
        const auto y = pv(8192, bench::sustained({parts[0], parts[1], parts[2]}, 6.0), r);
        const size_t from = y.size() / 2, n = y.size() - from;
        for (double f = 34.0; f < 70.0; f += 1.0) {
            const double p = bench::goertzelPow(y, from, n, f);
            std::printf("    %5.1f Hz : %7.2f dB %s\n", f, 10.0 * std::log10(p + 1e-30),
                        (std::fabs(f - 41.205) < 0.6 || std::fabs(f - 51.915) < 0.6 ||
                         std::fabs(f - 61.735) < 0.6)
                            ? "   <-- target"
                            : "");
        }
    }
    return 0;
}
