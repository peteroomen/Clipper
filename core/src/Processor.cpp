#include "clipper/Processor.h"

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/ParamGuard.h"

#include <cmath>

namespace clipper {

namespace {
// Target smoothing time constant: how long the one-pole takes to cover most of
// the distance to a new target. ~5 ms is fast enough to feel instant but slow
// enough to eliminate zipper noise on slider moves.
constexpr double kSmoothingTimeSeconds = 0.005;
}  // namespace

void Processor::prepare(double sampleRate, int /*maxBlockSize*/) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;

    // One-pole coefficient for an exponential approach with the given time
    // constant: y += coeff * (target - y). coeff = 1 - exp(-1 / (tau * fs)).
    const double tau = kSmoothingTimeSeconds;
    smoothingCoeff_ =
        static_cast<float>(1.0 - std::exp(-1.0 / (tau * sampleRate_)));

    // Snap to target on (re)prepare so we don't ramp from a stale value.
    gain_ = gainTarget_;
}

void Processor::reset() {
    // Sanitize the target too: if a non-finite one ever got past the guards, the
    // one-pole in process() could never climb back out of it.
    if (!dsp::paramIsFinite(gainTarget_)) gainTarget_ = 1.0f;
    gain_ = gainTarget_;
}

void Processor::setParameter(int paramId, float value) {
    switch (paramId) {
        case PARAM_GAIN:
            // Clamp to the documented linear range [0, 2]. NaN-REJECTING: the old
            // `if (value < 0) value = 0; if (value > 2) value = 2;` pair left NaN
            // untouched (both comparisons are false), and a NaN gain latches in the
            // smoother forever — audit finding 1. clampParam is bit-identical here
            // for every finite value.
            gainTarget_ = dsp::clampParam(value, 0.0f, 2.0f);
            break;
        default:
            break;
    }
}

void Processor::process(const float* in, float* out, int numFrames) {
    const float coeff = smoothingCoeff_;
    const float target = gainTarget_;
    float g = gain_;

    for (int i = 0; i < numFrames; ++i) {
        // Anti-denormal (Denormal.h). This is the ONE place in the tree that
        // hand-rolls the OnePoleSmoother recurrence instead of using the guarded
        // primitive, so it never inherited the guard. With GAIN at 0 the value
        // asymptotes toward zero and STICKS in the subnormal range — its own ULP
        // shrinks with it, so the increment can never close the gap. Then EVERY
        // sample is multiplied by a subnormal, forever, even on full-scale audio:
        // measured 445514 subnormal output samples and a 12-21x slowdown against
        // hardware FTZ before this flush (audit finding 11, docs §33).
        //
        // The rule is OnePoleSmoother::next()'s, verbatim — snap on the RESIDUAL, not
        // on the value — so the duplicated recurrence and the shared primitive now
        // behave identically, and both settle EXACTLY on the target instead of
        // asymptoting. Bit-identical to the unguarded form for every trajectory whose
        // residual stays above 1e-30, i.e. every trajectory a player can hear.
        // (Collapsing this duplicate into OnePoleSmoother outright is a refactor, not
        // this slice: it would change Processor's prepare/reset/parameter seams.)
        g += coeff * (target - g);
        const float residual = target - g;
        if (residual < dsp::kDenormalFloor && residual > -dsp::kDenormalFloor) g = target;
        out[i] = in[i] * g;
    }

    gain_ = g;
}

}  // namespace clipper
