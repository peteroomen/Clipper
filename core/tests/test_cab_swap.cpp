// Cab swap real-time safety — the 2026-07-24 audit's finding 2.
//
// THE BUG THIS PINS. A cab change was ONE C ABI call that synthesised an impulse
// response, heap-copied it, peak-normalized it and ran CabConvolver::prepare twice
// (FFT plan + one spectrum per partition per side) — and the worklet made that call
// from INSIDE its per-sample loop, at the declick fade-out zero. Measured on the
// shipped WASM artifact against the 2.667 ms deadline at 48 kHz / 128:
//
//     amp_process_stereo(128)      0.025 ms      0.9 % of the deadline
//     amp_set_cab_builtin         11.4  ms       427 %
//     amp_load_custom_ir(4096)    45.7  ms      1714 %   -> up to 17 dropped quanta
//
// i.e. an audible DROPOUT on every cab change and every IR upload. The design
// comment defended it as inaudible "because it runs at the output-zero of the
// declick", which conflates two different things: output-zero prevents a step
// discontinuity, and does nothing whatever about missing the render deadline.
//
// The fix double-buffers the per-side convolver pair inside the C ABI (the same
// trick amp_set_model already used for the four amp voices) and splits the call in
// two: amp_prepare_cab_* builds the new cab in the INACTIVE pair, amp_commit_cab
// makes it live. CabConvolver itself is deliberately untouched.
//
// WHAT THIS SUITE ASSERTS — player-observable properties, not identities:
//
//   A. THE AUDIO-THREAD STEP IS FREE. Counted with a REPLACED GLOBAL operator new,
//      so "allocation-free" is asserted DIRECTLY rather than inferred from a clock:
//      amp_commit_cab must perform exactly ZERO allocations, and so must
//      amp_process_stereo. amp_prepare_cab_* must perform MANY — that is the proof
//      the work still happens and merely moved, rather than having been deleted.
//      Then wall-clock, to pin the magnitude: the commit must cost less than one
//      128-frame render block and at least 100x less than the prepare.
//
//   B. THE TONE DID NOT MOVE. The whole point of finding 2 is a speed fix, and
//      CLAUDE.md's first rule is that a speed fix must be provably fidelity-
//      neutral. So the reference here is the PRE-FIX CONSTRUCTION, written out
//      longhand: an AmpModel feeding a locally-owned CabConvolver pair that is
//      re-prepared IN PLACE on a swap, exactly as the old loadIrBothSides did. A
//      scripted session (play, swap to the Brit 4x12, play, load a custom IR, play,
//      swap back to the 2x12, play) must come out of the new double-buffered ABI
//      BIT-IDENTICAL to that reference — max|a-b| exactly 0.0, no tolerance to
//      argue about. The swap-BACK leg is the one double-buffering could plausibly
//      have broken (a pair activated with stale FDL history from when it was last
//      live), which is why the script goes there and back.
//
// Build note: -DCLIPPER_CAB_TEST_LEGACY_ABI=1 routes the "commit" step through the
// old monolithic amp_set_cab_builtin, reproducing the pre-fix behaviour. Block A
// then FAILS (the commit allocates ~1000 times and costs milliseconds) while block
// B still passes — which is exactly the intended split: B is the guard that the
// speed-up cost no fidelity, so it must pass in both modes.
//
// No framework: int main + <cassert>, as every other suite here.

#include "clipper/dsp/AmpModel.h"
#include "clipper/dsp/CabConvolver.h"
#include "clipper/dsp/CabIR.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#ifndef CLIPPER_CAB_TEST_LEGACY_ABI
#define CLIPPER_CAB_TEST_LEGACY_ABI 0
#endif

// --- Allocation counter ------------------------------------------------------
//
// Replacing the global allocation functions is the only portable way to assert
// "this call does not allocate" without a platform hook. Everything routes to
// malloc/free so the standard library stays consistent with itself. The test is
// single-threaded, so a plain long is enough (an atomic would itself be fine, but
// this keeps the counted region free of any surprise).

namespace {
long g_allocs = 0;
bool g_counting = false;

// Snapshot/measure helper: counts allocations between construction and read.
struct AllocScope {
    long start;
    AllocScope() : start(g_allocs) { g_counting = true; }
    long count() const { return g_allocs - start; }
};
}  // namespace

void* operator new(std::size_t n) {
    if (g_counting) ++g_allocs;
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (g_counting) ++g_allocs;
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return operator new(n, t);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

// --- The worklet's C ABI (clipper_c_api.cpp), declared verbatim --------------
extern "C" {
void* amp_create(float);
void  amp_destroy(void*);
void  amp_set_param(void*, int, float);
void  amp_process_stereo(void*, const float*, float*, float*, int);
int   amp_latency_samples(void*);
// Pre-2026-07-25 monolithic form (kept as a prepare+commit wrapper).
void  amp_set_cab_builtin(void*, int);
void  amp_load_custom_ir(void*, const float*, int);
// The 2026-07-25 split: heavy prepare into the inactive pair, O(1) commit.
int   amp_prepare_cab_builtin(void*, int);
int   amp_prepare_cab_custom(void*, const float*, int);
void  amp_commit_cab(void*);
}

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 128;  // the AudioWorklet render quantum
constexpr int kCabClean212 = 0;
constexpr int kCabBrit412 = 1;

using A = clipper::dsp::AmpModel;

// Deterministic pseudo-noise (no <random>: identical bytes on every platform, and
// a real broadband signal rather than a single tone, so a spectral difference
// anywhere in the band shows up).
struct Noise {
    unsigned s = 0x13572468u;
    float next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<int>(s >> 8) - 8388608) / 8388608.0f * 0.25f;
    }
};

// The PRE-FIX construction, written out longhand: one AmpModel, ONE per-side
// convolver pair, and a swap that re-prepares those live convolvers in place. This
// is the reference block B measures against — it is deliberately NOT built on the
// new prepare/commit exports, or the comparison would be a tautology.
struct RefChain {
    clipper::dsp::AmpModel amp;
    clipper::dsp::CabConvolver cabL, cabR;

    void prepare() {
        amp.prepare(kSr, kBlock);
        loadIr(clipper::dsp::generateDefaultCab2x12IR(kSr));
    }
    // Exactly the old namespace-local loadIrBothSides().
    void loadIr(const std::vector<float>& ir) {
        cabL.prepare(kSr, ir.data(), static_cast<int>(ir.size()), kSr, kBlock);
        cabR.prepare(kSr, ir.data(), static_cast<int>(ir.size()), kSr, kBlock);
    }
    void swapBuiltin(int which) {
        loadIr(which == kCabBrit412 ? clipper::dsp::generateBrit4x12IR(kSr)
                                    : clipper::dsp::generateDefaultCab2x12IR(kSr));
    }
    // Exactly the old amp_load_custom_ir(): copy, peak-normalize, prepare.
    void loadCustom(const std::vector<float>& raw) {
        std::vector<float> ir = raw;
        clipper::dsp::peakNormalizeIR(ir, kSr);
        loadIr(ir);
    }
    // Exactly the old amp_process_stereo() clean-120 branch.
    void process(const float* in, float* l, float* r, int n) {
        amp.processStereo(in, l, r, n);
        cabL.process(l, l, n);
        cabR.process(r, r, n);
    }
};

// The knob positions both paths are driven with. Anything non-default is fine; the
// point is that the two chains are configured identically before the comparison.
void setKnobs(void* h, RefChain& ref) {
    struct { int id; float v; } knobs[] = {
        {A::PARAM_VOLUME, 0.42f}, {A::PARAM_BASS, 0.55f}, {A::PARAM_MIDDLE, 0.45f},
        {A::PARAM_TREBLE, 0.62f}, {A::PARAM_BRIGHT, 1.0f},
    };
    for (const auto& k : knobs) {
        amp_set_param(h, k.id, k.v);
        ref.amp.setParameter(k.id, k.v);
    }
}

// The "commit a cab change" step under test. In legacy mode there is no split, so
// the whole monolithic call IS the audio-thread step — which is the bug.
#if CLIPPER_CAB_TEST_LEGACY_ABI
void prepareBuiltin(void* h, int which) { (void)h; (void)which; }
void commitBuiltin(void* h, int which) { amp_set_cab_builtin(h, which); }
void prepareCustom(void* h, const float* p, int n) { (void)h; (void)p; (void)n; }
void commitCustom(void* h, const float* p, int n) { amp_load_custom_ir(h, p, n); }
const char* kMode = "LEGACY (pre-fix monolithic call)";
#else
void prepareBuiltin(void* h, int which) { assert(amp_prepare_cab_builtin(h, which) == 1); }
void commitBuiltin(void* h, int which) { (void)which; amp_commit_cab(h); }
void prepareCustom(void* h, const float* p, int n) { assert(amp_prepare_cab_custom(h, p, n) == 1); }
void commitCustom(void* h, const float* p, int n) { (void)p; (void)n; amp_commit_cab(h); }
const char* kMode = "SPLIT (prepare / commit)";
#endif

double msPerCall(const std::chrono::steady_clock::duration& d, long calls) {
    return std::chrono::duration<double, std::milli>(d).count() / static_cast<double>(calls);
}

// ---------------------------------------------------------------------------
// A. The audio-thread step allocates nothing and costs nothing.
// ---------------------------------------------------------------------------
void testCommitIsFreeOnTheAudioThread() {
    std::printf("--- A. audio-thread cost of a cab change [%s] ---\n", kMode);

    void* h = amp_create(static_cast<float>(kSr));
    assert(h);

    std::vector<float> in(kBlock), l(kBlock), r(kBlock);
    Noise noise;
    for (int i = 0; i < kBlock; ++i) in[static_cast<size_t>(i)] = noise.next();

    // A 4096-tap user upload is the worst case the app can produce (web/src/cab.ts
    // caps at MAX_IR_SAMPLES = 4096) — the audit's 45.7 ms line. Also the honest
    // subject for the prepare ALLOCATION count: a repeat prepare of the same-length
    // IR reuses the convolver's already-sized vectors and allocates almost nothing,
    // which would understate how much malloc traffic a real cab change carries.
    std::vector<float> custom(4096);
    Noise cn;
    for (size_t i = 0; i < custom.size(); ++i)
        custom[i] = cn.next() * static_cast<float>(std::exp(-3.0 * static_cast<double>(i) / 4096.0));

    // Warm up: the first blocks after amp_create are dominated by cold icache and
    // by the convolver's FDL filling, and libstdc++/stdio do one-off allocations.
    // Measuring those would make the ratios meaningless (and flaky).
    for (int i = 0; i < 2000; ++i) amp_process_stereo(h, in.data(), l.data(), r.data(), kBlock);
    prepareBuiltin(h, kCabBrit412);
    commitBuiltin(h, kCabBrit412);
    for (int i = 0; i < 2000; ++i) amp_process_stereo(h, in.data(), l.data(), r.data(), kBlock);

    // --- Allocation counts. This is the load-bearing assertion of block A: it is a
    // direct statement about malloc, not a proxy measurement.
    long processAllocs = 0, commitAllocs = 0, prepareAllocs = 0;
    {
        AllocScope s;
        amp_process_stereo(h, in.data(), l.data(), r.data(), kBlock);
        processAllocs = s.count();
    }
    {
        AllocScope s;
        commitBuiltin(h, kCabClean212);
        commitAllocs = s.count();
    }
    {
        AllocScope s;
#if CLIPPER_CAB_TEST_LEGACY_ABI
        commitCustom(h, custom.data(), static_cast<int>(custom.size()));
#else
        prepareCustom(h, custom.data(), static_cast<int>(custom.size()));
#endif
        prepareAllocs = s.count();
    }
    g_counting = false;
    prepareBuiltin(h, kCabBrit412);
    commitBuiltin(h, kCabBrit412);  // leave the handle in a defined state

    // --- Wall clock, to pin the magnitude. Best-of-5 means: a scheduler hiccup can
    // only ever make a measurement look SLOWER, so the minimum mean is the honest
    // estimate of the cost.
    const long kProcessCalls = 4000;
    // The split commit is ~1 ns, so it needs a lot of iterations to rise above clock
    // granularity. In legacy mode the same loop is the whole 11 ms cab rebuild, so
    // 200 000 of them would take half an hour — 20 is plenty to prove the point.
    const long kCommitCalls = CLIPPER_CAB_TEST_LEGACY_ABI ? 20 : 200000;
    const long kPrepareCalls = 40;
    double processMs = 1e9, commitMs = 1e9, prepareMs = 1e9;
    for (int rep = 0; rep < 5; ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        for (long i = 0; i < kProcessCalls; ++i)
            amp_process_stereo(h, in.data(), l.data(), r.data(), kBlock);
        auto t1 = std::chrono::steady_clock::now();
        const double m = msPerCall(t1 - t0, kProcessCalls);
        if (m < processMs) processMs = m;
    }
    for (int rep = 0; rep < 5; ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        // Alternating built-in so a legacy-mode run does the real work every call.
        for (long i = 0; i < kCommitCalls; ++i)
            commitBuiltin(h, (i & 1) ? kCabClean212 : kCabBrit412);
        auto t1 = std::chrono::steady_clock::now();
        const double m = msPerCall(t1 - t0, kCommitCalls);
        if (m < commitMs) commitMs = m;
    }
#if !CLIPPER_CAB_TEST_LEGACY_ABI
    for (int rep = 0; rep < 5; ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        for (long i = 0; i < kPrepareCalls; ++i)
            prepareBuiltin(h, (i & 1) ? kCabClean212 : kCabBrit412);
        auto t1 = std::chrono::steady_clock::now();
        const double m = msPerCall(t1 - t0, kPrepareCalls);
        if (m < prepareMs) prepareMs = m;
    }
#else
    // Legacy has no separate prepare; the monolithic call is the whole cost.
    prepareMs = commitMs;
    prepareAllocs = commitAllocs;
#endif

    double customMs = 1e9;
    for (int rep = 0; rep < 5; ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i) {
#if CLIPPER_CAB_TEST_LEGACY_ABI
            commitCustom(h, custom.data(), static_cast<int>(custom.size()));
#else
            prepareCustom(h, custom.data(), static_cast<int>(custom.size()));
#endif
        }
        auto t1 = std::chrono::steady_clock::now();
        const double m = msPerCall(t1 - t0, 20);
        if (m < customMs) customMs = m;
    }

    const double deadlineMs = 1000.0 * kBlock / kSr;  // 2.667 ms at 48 k / 128
    std::printf("[cab swap] render deadline (48k/128)   : %.4f ms\n", deadlineMs);
    std::printf("[cab swap] amp_process_stereo(128)     : %.6f ms/call, %ld allocations\n",
                processMs, processAllocs);
    std::printf("[cab swap] COMMIT (audio-thread step)  : %.6f ms/call, %ld allocations\n",
                commitMs, commitAllocs);
    std::printf("[cab swap] PREPARE builtin (off-path)  : %.6f ms/call\n", prepareMs);
    std::printf("[cab swap] PREPARE custom 4096 (off-p) : %.6f ms/call, %ld allocations\n",
                customMs, prepareAllocs);
    std::printf("[cab swap] prepare/commit time ratio   : %.0fx\n", prepareMs / commitMs);
    std::printf("[cab swap] commit vs one render block  : %.6fx\n", commitMs / processMs);
    std::printf("[cab swap] commit vs the deadline      : %.4f %%\n",
                100.0 * commitMs / deadlineMs);

    // The render path itself must be allocation-free (also proves the counter is
    // actually wired up and not silently returning 0 for everything).
    assert(processAllocs == 0);
    // THE assertion of this suite: the only part of a cab change that touches the
    // render callback does not allocate.
    assert(commitAllocs == 0);
    // ...and the prepare side DOES allocate, which is exactly why it may not live on
    // the render path. (Deliberately `> 0` and not an exact count: most of a
    // prepare's 11-43 ms is FFT compute rather than malloc traffic, and the sibling
    // fix/cab-block-size PR is rewriting CabConvolver's buffers — pinning a count
    // here would just be a trap for that branch. The timing ratio below is what
    // carries the "the work moved rather than vanished" claim.)
    assert(prepareAllocs > 0);
    // Magnitude: the commit is cheaper than a single render block, and at least two
    // orders of magnitude cheaper than building the cab.
    assert(commitMs < processMs);
    assert(prepareMs / commitMs > 100.0);
    // And it is a rounding error against the deadline it used to blow by 427 %.
    assert(commitMs < 0.01 * deadlineMs);

    amp_destroy(h);
    std::printf("  OK: the audio-thread step of a cab change is allocation-free.\n");
}

// ---------------------------------------------------------------------------
// B. Fidelity: bit-identical to the pre-fix construction, across a real session.
// ---------------------------------------------------------------------------
void testSwapIsBitIdenticalToPreFixBehaviour() {
    std::printf("--- B. fidelity vs the pre-fix construction [%s] ---\n", kMode);
    g_counting = false;

    void* h = amp_create(static_cast<float>(kSr));
    assert(h);
    RefChain ref;
    ref.prepare();
    setKnobs(h, ref);

    // Latency must be unchanged too: both double-buffered pairs use the same 128
    // partition, so a cab change must never renegotiate host latency.
    const int latencyBefore = amp_latency_samples(h);

    // A 4096-tap user IR (the app's cap), decaying so peakNormalizeIR has real work.
    std::vector<float> custom(4096);
    Noise cn;
    for (size_t i = 0; i < custom.size(); ++i)
        custom[i] = cn.next() * static_cast<float>(std::exp(-3.0 * static_cast<double>(i) / 4096.0));

    // The scripted session, in 128-frame blocks: play, swap, play, ... The swap-BACK
    // at the end is the leg double-buffering could have broken.
    struct Leg { int blocks; int action; };  // action: 0 none, 1 brit, 2 custom, 3 clean
    const Leg legs[] = {
        {200, 0}, {0, 1}, {200, 0}, {0, 2}, {200, 0}, {0, 3}, {200, 0}, {0, 1}, {200, 0},
    };

    Noise noise;
    std::vector<float> in(kBlock);
    std::vector<float> aL(kBlock), aR(kBlock), bL(kBlock), bR(kBlock);
    double maxDiffL = 0.0, maxDiffR = 0.0;
    long samples = 0;
    double refEnergy = 0.0;

    for (const Leg& leg : legs) {
        switch (leg.action) {
            case 1:
                prepareBuiltin(h, kCabBrit412);
                commitBuiltin(h, kCabBrit412);
                ref.swapBuiltin(kCabBrit412);
                break;
            case 2:
                prepareCustom(h, custom.data(), static_cast<int>(custom.size()));
                commitCustom(h, custom.data(), static_cast<int>(custom.size()));
                ref.loadCustom(custom);
                break;
            case 3:
                prepareBuiltin(h, kCabClean212);
                commitBuiltin(h, kCabClean212);
                ref.swapBuiltin(kCabClean212);
                break;
            default:
                break;
        }
        for (int b = 0; b < leg.blocks; ++b) {
            for (int i = 0; i < kBlock; ++i) in[static_cast<size_t>(i)] = noise.next();
            amp_process_stereo(h, in.data(), aL.data(), aR.data(), kBlock);
            ref.process(in.data(), bL.data(), bR.data(), kBlock);
            for (int i = 0; i < kBlock; ++i) {
                const double dl = std::fabs(static_cast<double>(aL[static_cast<size_t>(i)]) -
                                            static_cast<double>(bL[static_cast<size_t>(i)]));
                const double dr = std::fabs(static_cast<double>(aR[static_cast<size_t>(i)]) -
                                            static_cast<double>(bR[static_cast<size_t>(i)]));
                if (dl > maxDiffL) maxDiffL = dl;
                if (dr > maxDiffR) maxDiffR = dr;
                refEnergy += static_cast<double>(bL[static_cast<size_t>(i)]) *
                             static_cast<double>(bL[static_cast<size_t>(i)]);
                ++samples;
            }
        }
    }

    const double refRms = std::sqrt(refEnergy / static_cast<double>(samples));
    const int latencyAfter = amp_latency_samples(h);
    std::printf("[cab swap] compared samples per side   : %ld\n", samples);
    std::printf("[cab swap] reference RMS (signal check): %.6f\n", refRms);
    std::printf("[cab swap] max|new - pre-fix|  L / R   : %.10g / %.10g\n", maxDiffL, maxDiffR);
    std::printf("[cab swap] amp_latency_samples         : %d -> %d\n", latencyBefore, latencyAfter);

    // The comparison is not vacuous: real signal came out of the reference.
    assert(refRms > 0.005);
    assert(samples == 1000 * kBlock);
    // BIT-IDENTICAL. Not "within tolerance" — the new path runs the same
    // CabConvolver::prepare over the same IR samples with the same partition, so
    // any nonzero difference means the refactor changed the tone.
    assert(maxDiffL == 0.0);
    assert(maxDiffR == 0.0);
    // A cab change must not renegotiate reported latency.
    assert(latencyAfter == latencyBefore);
    assert(latencyBefore == kBlock);  // cab on, clean 120 is linear

    amp_destroy(h);
    std::printf("  OK: the double-buffered cab is bit-identical to the pre-fix path.\n");
}

// ---------------------------------------------------------------------------
// B2. A commit with no preceding prepare must not produce garbage, and a rejected
//     prepare must not disturb the live cab. Both are worklet contract edges.
// ---------------------------------------------------------------------------
void testDegenerateCallsAreSafe() {
    std::printf("--- B2. degenerate call sequences ---\n");
    g_counting = false;

    void* h = amp_create(static_cast<float>(kSr));
    RefChain ref;
    ref.prepare();
    setKnobs(h, ref);

    // A bad custom IR must be REJECTED (return 0) so the worklet knows not to
    // commit. Before the split there was no way to signal this.
#if !CLIPPER_CAB_TEST_LEGACY_ABI
    assert(amp_prepare_cab_custom(h, nullptr, 128) == 0);
    const float dummy = 0.0f;
    assert(amp_prepare_cab_custom(h, &dummy, 0) == 0);
    assert(amp_prepare_cab_custom(h, &dummy, -7) == 0);
    assert(amp_prepare_cab_builtin(nullptr, kCabBrit412) == 0);
    amp_commit_cab(nullptr);  // must not crash
#endif

    // amp_create prepares BOTH pairs with the default 2x12, so even a commit with
    // no prepare in front of it activates a valid convolver holding the default IR
    // — never an unprepared one. Feed a block and require finite, non-silent audio.
    std::vector<float> in(kBlock), l(kBlock), r(kBlock);
    Noise noise;
    double peak = 0.0;
    for (int b = 0; b < 60; ++b) {
        for (int i = 0; i < kBlock; ++i) in[static_cast<size_t>(i)] = noise.next();
#if !CLIPPER_CAB_TEST_LEGACY_ABI
        if (b == 20) amp_commit_cab(h);  // bare commit, nothing prepared
#endif
        amp_process_stereo(h, in.data(), l.data(), r.data(), kBlock);
        for (int i = 0; i < kBlock; ++i) {
            assert(std::isfinite(l[static_cast<size_t>(i)]));
            assert(std::isfinite(r[static_cast<size_t>(i)]));
            const double a = std::fabs(static_cast<double>(l[static_cast<size_t>(i)]));
            if (a > peak) peak = a;
        }
    }
    std::printf("[cab swap] bare-commit render peak     : %.6f (finite, non-silent)\n", peak);
    assert(peak > 0.005);

    amp_destroy(h);
    std::printf("  OK: degenerate sequences stay finite and audible.\n");
}

}  // namespace

int main() {
    // Warm stdio and the standard library's one-off allocations before any counted
    // region, so nothing unrelated lands in a measurement.
    // Unbuffered, so a failing assert still shows the measurements that led to it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== cab swap real-time safety (audit finding 2) ===\n");

    testCommitIsFreeOnTheAudioThread();
    testSwapIsBitIdenticalToPreFixBehaviour();
    testDegenerateCallsAreSafe();

    std::printf("=== all cab swap tests passed ===\n");
    return 0;
}
