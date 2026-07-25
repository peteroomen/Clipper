// Clipper native shell — the CHAIN-EDIT (declick) test.
//
// The parity board lets the user reorder, add, remove and stomp pedals WHILE the
// rig is making noise. Every one of those is a topology change: the signal path
// literally changes between one sample and the next, and done naively that is a
// step discontinuity — a click, and at high gain a genuinely nasty one.
//
// ClipperEngine brackets each edit in the web worklet's 6 ms raised-cosine fade:
// the output ramps to zero, the topology swaps AT that zero, then it ramps back.
// This test proves the bracket works, and that it is doing real work:
//
//   1. CONTINUITY — through a reorder, a bypass toggle, an add and a remove, the
//      largest sample-to-sample step never meaningfully exceeds the steady-state
//      slew of the signal itself. A click would blow straight past that bound.
//   2. SENSITIVITY — the same edit applied as a HARD switch (the two topologies
//      rendered separately and cut together) produces a step far larger than the
//      bound. Without this, case 1 could pass on a test that cannot fail.
//   3. IT REOPENS — after the fade the output is back at the level the NEW chain
//      should be making (compared against a from-scratch render of it), i.e. the
//      declick is a fade, not a mute. Comparing against the OLD level would be
//      wrong: half these edits legitimately change the rig's output level.
//   4. KNOBS ARE NOT EDITS — moving a knob must NOT arm the fade. That is the
//      identical-core contract: a steady chain with a moving knob has to stay
//      bit-exact against a single-shot core render, which it cannot be if an
//      envelope is multiplying the output.
//
// The rig runs with the amp POWERED OFF (a stereo passthrough of the pedal chain)
// so nothing downstream — no tone stack, no cab convolution — smooths a step away
// before it is measured.
//
// The board is deliberately a GENTLE one (a low-drive Screamer and the linear
// phaser, fed below unity). That is not a soft-pedalled test, it is what makes the
// test able to fail: run this through a cranked fuzz and the output is a square
// wave whose own edges step by 0.8 between samples, so a click of 0.1 hides
// completely inside the signal's natural slew and ANY implementation passes. Keep
// the waveform smooth and a discontinuity has nowhere to hide — the sensitivity
// case below pins that property explicitly.

#include <cmath>
#include <cstdio>
#include <vector>

#include "ClipperEngine.h"

using clipper::native::ClipperEngine;
using clipper::native::Params;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 64;
constexpr int kPre = 24000;    // 0.5 s of steady state before the edit
constexpr int kPost = 12000;   // 0.25 s after it
constexpr int kTotal = kPre + kPost;

// A steady 220 Hz sine — no envelope, so the signal's own slew is constant and a
// discontinuity has nowhere to hide.
std::vector<float> makeSignal() {
    std::vector<float> x((size_t)kTotal);
    for (int i = 0; i < kTotal; ++i)
        x[(size_t)i] = (float)(0.15 * std::sin(2.0 * M_PI * 220.0 * (i / kFs)));
    return x;
}

// The board this test edits: Screamer -> Ninety, both engaged, into a powered-DOWN
// amp. Low drive and a below-unity trim keep the waveform smooth (see the note
// above); the RAT is configured but starts OFF the board, ready to be added.
Params baseParams() {
    Params p;
    p.chain[0] = clipper::native::PEDAL_TS;
    p.chain[1] = clipper::native::PEDAL_PHASER;
    p.chainLength = 2;
    p.inputTrim = 0.28f;
    p.tsOn = true;     p.tsDrive = 0.12f; p.tsTone = 0.5f; p.tsLevel = 0.5f;
    p.phaserOn = true; p.phaserSpeed = 0.3f;
    p.ratOn = true;    p.ratDist = 0.15f; p.ratFilter = 0.5f; p.ratLevel = 0.45f;
    p.ampOn = false;   // stereo passthrough — no amp/cab smoothing over the seam
    p.cab = false;
    p.oversampling = 4;
    return p;
}

// Render `total` samples, applying `edited` from sample kPre onward.
std::vector<float> render(const Params& start, const Params& edited, bool* declickedOut) {
    ClipperEngine eng;
    eng.setParams(start);
    eng.prepare(kFs, kBlock);

    const std::vector<float> in = makeSignal();
    std::vector<float> out((size_t)kTotal, 0.0f);
    std::vector<float> l(kBlock), r(kBlock);
    bool sawDeclick = false;

    int off = 0;
    while (off < kTotal) {
        const int n = std::min(kBlock, kTotal - off);
        // The edit arrives between blocks, exactly as a UI edit would.
        eng.updateParams(off >= kPre ? edited : start);
        if (off >= kPre && eng.declicking()) sawDeclick = true;
        eng.process(in.data() + off, l.data(), r.data(), n);
        for (int i = 0; i < n; ++i) out[(size_t)(off + i)] = l[(size_t)i];
        off += n;
    }
    if (declickedOut) *declickedOut = sawDeclick;
    return out;
}

double maxStep(const std::vector<float>& x, int from, int to) {
    double m = 0.0;
    for (int i = std::max(1, from); i < to && i < (int)x.size(); ++i)
        m = std::max(m, std::fabs((double)x[(size_t)i] - (double)x[(size_t)(i - 1)]));
    return m;
}

double rms(const std::vector<float>& x, int from, int to) {
    double s = 0.0;
    int n = 0;
    for (int i = std::max(0, from); i < to && i < (int)x.size(); ++i, ++n)
        s += (double)x[(size_t)i] * (double)x[(size_t)i];
    return n > 0 ? std::sqrt(s / n) : 0.0;
}

bool failed = false;

void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failed = true;
}

// One edit case: render it, and hold the seam to the continuity bound.
void runCase(const char* label, const Params& edited) {
    std::printf("\n--- edit: %s ---\n", label);
    const Params base = baseParams();

    bool declicked = false;
    const std::vector<float> out = render(base, edited, &declicked);

    // The signal's own steady-state slew, measured well before the edit.
    const double slew = maxStep(out, kPre - 8000, kPre - 100);
    // The seam: the fade is 6 ms each way, so ±25 ms covers it generously.
    const int seamFrom = kPre - 1200;
    const int seamTo = kPre + 2400;
    const double seam = maxStep(out, seamFrom, seamTo);
    const double bound = std::max(1.25 * slew, 1e-4);

    // What the SAME edit would have cost with no fade at all: the two topologies
    // rendered separately and spliced at the edit point.
    const std::vector<float> a = render(base, base, nullptr);
    const std::vector<float> b = render(edited, edited, nullptr);
    const double hardStep =
        std::fabs((double)b[(size_t)kPre] - (double)a[(size_t)(kPre - 1)]);

    std::printf("  steady slew      : %.6f\n", slew);
    std::printf("  max step at seam : %.6f  (bound %.6f)\n", seam, bound);
    std::printf("  hard-switch step : %.6f\n", hardStep);

    check(declicked, "the edit armed the declick fade");
    check(seam <= bound, "no discontinuity beyond the declick envelope");
    // The fade must REOPEN, and reopen to the RIGHT level: measure the settled
    // output against a from-scratch render of the edited board over the same window.
    // (A plain "did it get louder than before" check would be wrong — adding a pedal
    // legitimately changes the level, sometimes a lot.)
    const double settled = rms(out, kPre + 1600, kTotal);
    const double target = rms(b, kPre + 1600, kTotal);
    std::printf("  settled RMS      : %.6f  (new chain alone: %.6f)\n", settled, target);
    check(target > 1e-6 && settled > 0.5 * target && settled < 2.0 * target,
          "output settles at the new chain's own level (a fade, not a mute)");
    for (int i = 0; i < kTotal; ++i)
        if (!std::isfinite(out[(size_t)i])) {
            check(false, "output stayed finite");
            break;
        }
}

}  // namespace

int main() {
    std::printf("=== Clipper chain-edit (declick) test ===\n");
    std::printf("220 Hz steady sine, %d samples @ %.0f Hz, block %d, edit at %d\n", kTotal,
                kFs, kBlock, kPre);

    // 1. REORDER: Screamer -> Ninety becomes Ninety -> Screamer.
    {
        Params p = baseParams();
        p.chain[0] = clipper::native::PEDAL_PHASER;
        p.chain[1] = clipper::native::PEDAL_TS;
        runCase("reorder (TS->Phaser becomes Phaser->TS)", p);
    }
    // 2. BYPASS: stomp the Screamer off mid-note.
    {
        Params p = baseParams();
        p.tsOn = false;
        runCase("bypass (TS stomped off)", p);
    }
    // 3. ADD: a RAT lands at the end of the board.
    {
        Params p = baseParams();
        p.chain[2] = clipper::native::PEDAL_RAT;
        p.chainLength = 3;
        runCase("add (RAT appended to the board)", p);
    }
    // 4. REMOVE: the Screamer is pulled off the board entirely.
    {
        Params p = baseParams();
        p.chain[0] = clipper::native::PEDAL_PHASER;
        p.chainLength = 1;
        runCase("remove (TS taken off the board)", p);
    }

    // 5. SENSITIVITY: the continuity bound has to be one a real click would fail.
    // Splice the loudest of the edits (adding the RAT) HARD — no fade — and confirm
    // the resulting step blows well past the bound. Without this the continuity
    // check could be passing simply because nothing could ever exceed it.
    {
        std::printf("\n--- sensitivity ---\n");
        Params p = baseParams();
        p.chain[2] = clipper::native::PEDAL_RAT;
        p.chainLength = 3;
        const std::vector<float> a = render(baseParams(), baseParams(), nullptr);
        const std::vector<float> b = render(p, p, nullptr);
        const double slew = maxStep(a, kPre - 8000, kPre - 100);
        const double bound = std::max(1.25 * slew, 1e-4);
        const double hard =
            std::fabs((double)b[(size_t)kPre] - (double)a[(size_t)(kPre - 1)]);
        std::printf("  steady slew %.6f, bound %.6f, hard-switch step %.6f (x%.1f slew)\n",
                    slew, bound, hard, slew > 0.0 ? hard / slew : 0.0);
        check(hard > 4.0 * bound,
              "an unfaded switch really would click (the bound can fail)");
    }

    // 6. A KNOB MOVE IS NOT A CHAIN EDIT: it must never arm the fade, or the
    // identical-core bit-exactness contract is broken.
    {
        std::printf("\n--- knob move (must NOT declick) ---\n");
        Params p = baseParams();
        p.ratDist = 0.8f;  // same board, same engaged flags, different knob
        bool declicked = false;
        render(baseParams(), p, &declicked);
        check(!declicked, "a knob change leaves the declick idle");
    }

    if (failed) {
        std::printf("\nFAIL: chain edits are not click-free.\n");
        return 1;
    }
    std::printf("\nPASS: every chain edit is bracketed by the declick fade.\n");
    return 0;
}
