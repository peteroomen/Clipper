// Does the rich-triad error the head-to-head found reach the PLAYER? Measured
// through DropModel — the shipped pedal, its own detents, its own block size —
// rather than through the bare primitive.
#include "bench_shifter.h"
#include "clipper/dsp/DropModel.h"

using clipper::dsp::DropModel;

static std::vector<float> pedal(int position, const std::vector<float>& in) {
    DropModel d;
    d.prepare(bench::kSr, 128);
    d.setParameter(DropModel::PARAM_AMOUNT,
                   (float)position / (float)(DropModel::POSITION_COUNT - 1));
    std::vector<float> out(in.size());
    for (size_t i = 0; i < in.size(); i += 128) {
        const int n = (int)std::min<size_t>(128, in.size() - i);
        d.process(in.data() + i, out.data() + i, n);
    }
    return out;
}

int main() {
    const char* names[] = {"DROP 1", "DROP 2", "DROP 3", "DROP 4", "DROP 5",
                           "DROP 6", "DROP 7", "OCT",    "OCT+DRY"};
    for (const auto& shape : bench::kShapes) {
        if (std::string(shape.name) == "single E2") continue;
        std::printf("\n=== %s, through DropModel ===\n", shape.name);
        for (int pos = 0; pos <= DropModel::DROP_OCT; ++pos) {
            const auto y = pedal(pos, bench::sustained(shape.partials, 5.0));
            const double r = std::pow(2.0, -(double)DropModel::semitonesFor(pos) / 12.0);
            double lo = 1e9, hi = -1e9, worst = 0;
            for (double f : shape.partials) {
                const double t = f * r;
                const double c = est::cents(est::freqNear(y, t, bench::kSr, 2.0), t);
                lo = std::min(lo, c); hi = std::max(hi, c); worst = std::max(worst, std::fabs(c));
            }
            std::printf("  %-8s worst |%7.3f| cents   spread %7.3f   %s\n", names[pos], worst,
                        hi - lo, worst >= 5.0 ? "<-- PAST the 5-cent perceptual bar" : "");
        }
    }
    return 0;
}
