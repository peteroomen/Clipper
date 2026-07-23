// Plain-assert tests for clipper::Processor. No test framework: just int main
// and <cassert>. Any failure aborts with a non-zero exit code.

#include "clipper/Processor.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

bool isFinite(float x) { return std::isfinite(x); }

// Run the processor long enough for one-pole smoothing to settle, then return
// the steady-state output for a constant input at the given gain.
float settledGainOutput(float gainTarget, float input, double sampleRate) {
    clipper::Processor p;
    p.prepare(sampleRate, 128);
    p.setParameter(clipper::PARAM_GAIN, gainTarget);

    // 0.5 s of audio: many time constants past the ~5 ms smoothing.
    const int n = static_cast<int>(sampleRate * 0.5);
    std::vector<float> in(static_cast<size_t>(n), input);
    std::vector<float> out(static_cast<size_t>(n), 0.0f);
    p.process(in.data(), out.data(), n);
    return out[static_cast<size_t>(n) - 1];
}

// Test 1: after smoothing settles, gain is applied correctly.
void testGainAppliedAfterSettle() {
    const double fs = 48000.0;
    const float input = 0.5f;

    float g1 = settledGainOutput(1.0f, input, fs);
    assert(std::fabs(g1 - input * 1.0f) < 1e-4f && "unity gain mismatch");

    float g05 = settledGainOutput(0.5f, input, fs);
    assert(std::fabs(g05 - input * 0.5f) < 1e-4f && "0.5 gain mismatch");

    float g2 = settledGainOutput(2.0f, input, fs);
    assert(std::fabs(g2 - input * 2.0f) < 1e-4f && "2.0 gain mismatch");

    // Range clamp: values above 2 clamp to 2.
    float gClamp = settledGainOutput(5.0f, input, fs);
    assert(std::fabs(gClamp - input * 2.0f) < 1e-4f && "clamp to 2 failed");

    std::printf("  [ok] gain applied correctly after smoothing settles\n");
}

// Test 2: no NaNs / infinities anywhere in the output.
void testNoNaNs() {
    const double fs = 44100.0;
    clipper::Processor p;
    p.prepare(fs, 128);
    p.setParameter(clipper::PARAM_GAIN, 1.5f);

    const int n = 4096;
    std::vector<float> in(static_cast<size_t>(n));
    std::vector<float> out(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        in[static_cast<size_t>(i)] =
            std::sin(2.0f * 3.14159265f * 220.0f * i / static_cast<float>(fs));
    }
    p.process(in.data(), out.data(), n);
    for (int i = 0; i < n; ++i) {
        assert(isFinite(out[static_cast<size_t>(i)]) && "non-finite output");
    }
    std::printf("  [ok] no NaNs/infinities in output\n");
}

// Test 3: smoothing actually ramps — after a parameter jump the gain changes
// gradually across a block rather than snapping instantly, and it is monotonic
// toward the new target.
void testSmoothingRamps() {
    const double fs = 48000.0;
    clipper::Processor p;
    p.prepare(fs, 128);

    // Settle at gain 0 first.
    p.setParameter(clipper::PARAM_GAIN, 0.0f);
    // 4096 samples ~= 17 time constants of the ~5 ms smoother: residual ~4e-8,
    // comfortably inside the 1e-4 tolerance below. (2048 left ~2e-4 — the test
    // only "passed" before because NDEBUG had disabled the assert.)
    {
        std::vector<float> in(4096, 1.0f), out(4096, 0.0f);
        p.process(in.data(), out.data(), 4096);
    }
    assert(std::fabs(p.currentGain() - 0.0f) < 1e-4f);

    // Jump target to 2.0 and render ONE 128-frame quantum on a DC input of 1.0
    // so the output equals the instantaneous gain each sample.
    p.setParameter(clipper::PARAM_GAIN, 2.0f);
    const int q = 128;
    std::vector<float> in(static_cast<size_t>(q), 1.0f);
    std::vector<float> out(static_cast<size_t>(q), 0.0f);
    p.process(in.data(), out.data(), q);

    // It must NOT have snapped to 2.0 in a single block...
    assert(out[static_cast<size_t>(q) - 1] < 2.0f && "gain snapped instantly");
    // ...it must have moved meaningfully off zero...
    assert(out[0] > 0.0f && "gain did not start ramping");
    // ...and the ramp is monotonically increasing toward the target.
    for (int i = 1; i < q; ++i) {
        assert(out[static_cast<size_t>(i)] >= out[static_cast<size_t>(i) - 1] &&
               "ramp not monotonic");
    }
    // The end of the block should be closer to the target than the start.
    assert(out[static_cast<size_t>(q) - 1] > out[0] && "ramp did not progress");

    std::printf("  [ok] smoothing ramps gradually across a block\n");
}

}  // namespace

int main() {
    std::printf("Running clipper::Processor tests...\n");
    testGainAppliedAfterSettle();
    testNoNaNs();
    testSmoothingRamps();
    std::printf("All tests passed.\n");
    return 0;
}
