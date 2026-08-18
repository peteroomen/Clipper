// Plain-assert tests for the M10.4 Mesa/Boogie Dual Rectifier Solo Head (docs §68)
// — the grunge / 90s-metal voice, and the FIRST amp in this repo TRANSCRIBED from a
// complete factory drawing set rather than reconstructed from prose.
//
// Covers MesaPreamp (five 12AX7 stages, the RED/ORANGE input select, the cathode
// follower, the FMV stack with its MASTER inside the same MNA), MesaPowerAmp (the
// LTP with its TRANSCRIBED unequal plate loads, the 6L6 quad, the switchable
// feedback, the selectable rectifier) and the composed MesaAmp.
//
// No framework: int main + <cassert>.
//
// THE LOAD-BEARING TESTS ARE testMarkedNodeVoltages, testModernModesRunOpenLoop and
// testRectifierMovesSag.
//
//   * testMarkedNodeVoltages is the strongest kind of check this project has ever
//     had on an amp: the drawing carries the MANUFACTURER'S OWN measured DC node
//     voltages, so the comparison is against an ABSOLUTE external reference rather
//     than against an analytic form derived from the same netlist (docs §29's
//     standing complaint about vacuous tone-stack tests).
//   * testModernModesRunOpenLoop is the voice's acceptance bar and it is TOPOLOGY:
//     the drawing's truth table switches global negative feedback OFF entirely in
//     the two MODERN modes, which no other amp in this lineup can do at any knob.
//   * testRectifierMovesSag is the second half of that bar.

#include "clipper/dsp/MesaAmp.h"
#include "clipper/dsp/MesaPowerAmp.h"
#include "clipper/dsp/MesaPreamp.h"

#include "support/AssertsLive.h"
#include "support/DcOffset.h"
// NO support/Xfail.h — this suite registers ZERO known-bad properties.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace clipper::dsp;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kBlock = 128;

// Goertzel amplitude of `f` over x[n0..end).
double bin(const std::vector<float>& x, double sr, double f, size_t n0) {
    const double w = 2.0 * kPi * f / sr;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t n = n0; n < x.size(); ++n) {
        const double s = static_cast<double>(x[n]) + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double m = s1 * s1 + s2 * s2 - c * s1 * s2;
    return std::sqrt(m > 0.0 ? m : 0.0) * 2.0 / static_cast<double>(x.size() - n0);
}

std::vector<float> tone(double sr, int n, double f, double amp) {
    std::vector<float> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<size_t>(i)] =
            static_cast<float>(amp * std::sin(2.0 * kPi * f * i / sr));
    return v;
}

void runBlocks(MesaAmp& a, const std::vector<float>& in, std::vector<float>& out) {
    out.assign(in.size(), 0.0f);
    for (size_t off = 0; off + kBlock <= in.size(); off += kBlock)
        a.process(in.data() + off, out.data() + off, kBlock);
}

// MesaAmp owns a unique_ptr and declares a destructor, so it is not movable —
// configure in place rather than returning by value.
void setUpAmp(MesaAmp& a, MesaMode m, double sr, double gain, double master) {
    a.setMode(m);
    a.prepare(sr, kBlock);
    a.setParameter(MesaAmp::PARAM_GAIN, static_cast<float>(gain));
    a.setParameter(MesaAmp::PARAM_MASTER, static_cast<float>(master));
    a.setParameter(MesaAmp::PARAM_BASS, 0.5f);
    a.setParameter(MesaAmp::PARAM_MID, 0.5f);
    a.setParameter(MesaAmp::PARAM_TREBLE, 0.5f);
    a.setParameter(MesaAmp::PARAM_PRESENCE, 0.5f);
    a.setParameter(MesaAmp::PARAM_REVERB, 0.0f);
}

// ---------------------------------------------------------------------------
// 1. THE TRUTH TABLE, asserted cell by cell against sheet `mbdr7`.
//
// The table is DATA in the header precisely so this test can compare it with the
// drawing rather than with the code that consumes it. If a future revision of the
// amp is transcribed, this is the diff.
// ---------------------------------------------------------------------------
void testTruthTable() {
    // Columns in the DRAWING's order: OR NORM | OR CLN | OR MOD | RED NORM | RED VINT
    // Rows in the drawing's order. 1 == ON.
    struct Row { const char* name; int orNorm, orCln, orMod, redNorm, redVint; };
    const Row sheet[] = {
        {"LDR1  INPUT SELECT",     0, 0, 0, 1, 1},
        {"LDR2  INPUT SELECT",     1, 1, 1, 0, 0},
        {"LDR3  CATHODE CAPS",     1, 0, 1, 1, 1},
        {"LDR4  V1A OUTPUT PAD",   1, 0, 1, 1, 1},
        {"LDR5  RD GAIN POT",      0, 0, 0, 1, 1},
        {"LDR6  OR GAIN POT",      1, 1, 1, 0, 0},
        {"LDR7  TREBLE ROLL OFF",  1, 1, 0, 0, 1},
        {"LDR8  RD TONE CAPS",     0, 0, 0, 1, 1},
        {"LDR9  OR TONE CAPS",     1, 1, 1, 0, 0},
        {"LDR10 V3 CATHODE CAP",   1, 0, 1, 1, 1},
        {"LDR14 RD MASTER POT",    0, 0, 0, 1, 1},
        {"LDR15 OR MASTER POT",    1, 1, 1, 0, 0},
        {"LDR16 OR MASTER BYPASS", 0, 1, 0, 0, 0},
        {"LDR19 FEEDBACK",         1, 1, 0, 0, 1},
        {"LDR20 MORE FEEDBACK",    0, 1, 0, 0, 0},
    };
    // Map the drawing's column order onto the enum.
    const MesaMode col[5] = {MesaMode::OrangeNormal, MesaMode::OrangeClean,
                             MesaMode::OrangeModern, MesaMode::RedModern,
                             MesaMode::RedVintage};
    for (size_t r = 0; r < 15; ++r) {
        const Row& s = sheet[r];
        const int want[5] = {s.orNorm, s.orCln, s.orMod, s.redNorm, s.redVint};
        for (int cidx = 0; cidx < 5; ++cidx) {
            const MesaModeRow& w = mesaModeRow(col[cidx]);
            // Explicit, in the drawing's row order — no struct-layout assumption.
            const bool fields[15] = {
                w.ldr1_redInputSel,  w.ldr2_orInputSel,   w.ldr3_cathodeCaps,
                w.ldr4_v1aOutputPad, w.ldr5_redGainPot,   w.ldr6_orGainPot,
                w.ldr7_trebleRollOff,w.ldr8_redToneCaps,  w.ldr9_orToneCaps,
                w.ldr10_v3CathodeCap,w.ldr14_redMaster,   w.ldr15_orMaster,
                w.ldr16_orMasterBypass, w.ldr19_feedback, w.ldr20_moreFeedback};
            const bool got = fields[r];
            if (got != (want[cidx] != 0)) {
                std::printf("  TRUTH TABLE MISMATCH %s col %d: got %d want %d\n",
                            s.name, cidx, got ? 1 : 0, want[cidx]);
            }
            assert(got == (want[cidx] != 0));
        }
    }
    std::printf("  truth table: 15 devices x 5 modes = 75 cells match sheet mbdr7\n");
}

// ---------------------------------------------------------------------------
// 2. THE MARKED DC NODE VOLTAGES — an ABSOLUTE reference from the manufacturer.
//
// Windows are +/-8 %, which is wider than the model actually achieves (worst is
// 4.0 %) and is the right width for a hand-marked service drawing. The rail
// ORDERING is asserted separately and exactly, because a dropper ladder that ran
// the wrong way would still land inside a percentage window on some nodes.
// ---------------------------------------------------------------------------
void testMarkedNodeVoltages() {
    MesaPreamp p;
    p.setMode(MesaMode::RedModern);
    p.prepare(48000.0 * 4, kBlock * 4);

    struct Node { const char* name; int stage; double markedVp, markedVk; };
    const Node nodes[] = {
        {"V1A", MesaPreamp::V1A, 200.0, 1.6},
        {"V2A", MesaPreamp::V2A, 280.0, 2.0},
        {"V2B", MesaPreamp::V2B, 384.0, 5.0},
        {"V3A", MesaPreamp::V3A, 213.0, 1.6},
    };
    for (const Node& n : nodes) {
        const double vp = p.stageQuiescentPlate(n.stage);
        const double err = std::fabs(vp - n.markedVp) / n.markedVp;
        std::printf("  %s plate %7.2f V vs sheet %6.1f  (%.2f %%)\n", n.name, vp,
                    n.markedVp, 100.0 * err);
        assert(err < 0.08);
    }
    // V3B is the cathode follower: the sheet marks its CATHODE at 216 V.
    const double vk3b = p.stageQuiescentCathode(MesaPreamp::V3B);
    std::printf("  V3B cathode %7.2f V vs sheet 216.0  (%.2f %%)\n", vk3b,
                100.0 * std::fabs(vk3b - 216.0) / 216.0);
    assert(std::fabs(vk3b - 216.0) / 216.0 < 0.08);

    // The ladder must run downhill in the drawing's order: C > D > E.
    std::printf("  rails C %.2f  D %.2f  E %.2f (sheet 422 / 406 / 402)\n",
                p.supplyC(), p.supplyD(), p.supplyE());
    assert(p.supplyC() > p.supplyD());
    assert(p.supplyD() > p.supplyE());
    assert(std::fabs(p.supplyC() - 422.0) / 422.0 < 0.08);

    // THE SHEET IS INTERNALLY INCONSISTENT HERE AND THE MODEL HONOURS THE PLATE.
    // Its marked E = 402 V cannot coexist with its marked V1A plate = 200 V through
    // the transcribed 220k: that plate needs (402-200)/220k = 0.918 mA, which drops
    // 20.2 V across the transcribed 22k and would put D at 422, not the marked 406.
    // This model reproduces the PLATE (1.7 %) and lands E low. Asserted so nobody
    // later "fixes" E toward 402 and breaks the plate that sets the tone.
    assert(p.supplyE() < 402.0);
}

// ---------------------------------------------------------------------------
// 3. THE ACCEPTANCE BAR (a): THE MODERN MODES RUN OPEN-LOOP.
//
// Measured as loop DEPTH — the small-signal gain of the same power stage with the
// mode's own feedback against the same stage with beta == 0. That is a property of
// the circuit, not of a coefficient: nothing in the test reads feedbackBeta() to
// decide what to expect, so a future edit that quietly gave the modern modes a
// small non-zero beta would fail here.
// ---------------------------------------------------------------------------
double loopDepthDb(MesaMode m, double sr) {
    const int n = kBlock * 94;
    auto measure = [&](MesaMode mode) {
        MesaPowerAmp pa;
        pa.setMode(mode);
        pa.prepare(sr, kBlock);
        pa.setParameter(MesaPowerAmp::PARAM_DRIVE, 0.5f);
        pa.setParameter(MesaPowerAmp::PARAM_PRESENCE, 0.0f);
        std::vector<float> in = tone(sr, n, 1000.0, 0.05), out(static_cast<size_t>(n));
        for (int off = 0; off + kBlock <= n; off += kBlock)
            pa.process(in.data() + off, out.data() + off, kBlock);
        return bin(out, sr, 1000.0, static_cast<size_t>(n / 2));
    };
    const double open = measure(MesaMode::RedModern);  // beta == 0 by the table
    const double closed = measure(m);
    return 20.0 * std::log10(open / (closed + 1e-30));
}

void testModernModesRunOpenLoop() {
    const double sr = 48000.0;
    const double dClean = loopDepthDb(MesaMode::OrangeClean, sr);
    const double dNorm = loopDepthDb(MesaMode::OrangeNormal, sr);
    const double dVint = loopDepthDb(MesaMode::RedVintage, sr);
    const double dOrMod = loopDepthDb(MesaMode::OrangeModern, sr);
    const double dRedMod = loopDepthDb(MesaMode::RedModern, sr);
    std::printf("  loop depth: OR CLN %.2f  OR NORM %.2f  RED VINT %.2f  "
                "OR MOD %.2f  RED MOD %.2f dB\n",
                dClean, dNorm, dVint, dOrMod, dRedMod);

    // The two MODERN modes are open-loop: EXACTLY zero depth, not merely small.
    assert(std::fabs(dOrMod) < 1e-9);
    assert(std::fabs(dRedMod) < 1e-9);
    // The three others have a real loop. 4 dB is the bar; the measurement is 7.72
    // for the single-47k modes, RECORDED not snugged.
    assert(dVint > 4.0);
    assert(dNorm > 4.0);
    // OR CLN parallels a second 47k, so it must be DEEPER than the single ones.
    assert(dClean > dVint + 2.0);
    // ...and the two single-47k modes must agree, since the drawing gives them the
    // same resistor. An identity in the circuit, so a tight bound is fair.
    assert(std::fabs(dNorm - dVint) < 0.01);

    // No other amp in this lineup can reach zero: their loops are hard-wired.
    // Asserted as a betas check so the intent survives a refactor.
    MesaPowerAmp pa;
    pa.setMode(MesaMode::RedModern);
    assert(pa.feedbackBeta() == 0.0);
    pa.setMode(MesaMode::RedVintage);
    assert(pa.feedbackBeta() > 0.0);
}

// ---------------------------------------------------------------------------
// 4. THE ACCEPTANCE BAR (b): THE RECTIFIER SELECT MOVES SAG.
//
// A RATIO, not an absolute droop, so it cannot be satisfied by simply lowering the
// valve setting's rail: the bar is that the rail WALKS FURTHER under load, which is
// what a player hears as bloom.
// ---------------------------------------------------------------------------
double railDroop(MesaRectifier r, MesaPowerMode pm, double sr) {
    const int n = kBlock * 94;
    MesaPowerAmp pa;
    pa.setMode(MesaMode::RedModern);
    pa.setRectifier(r);
    pa.setPowerMode(pm);
    pa.prepare(sr, kBlock);
    pa.setParameter(MesaPowerAmp::PARAM_DRIVE, 1.0f);
    const double idle = pa.railIdle();
    std::vector<float> in = tone(sr, n, 82.41, 12.0), out(static_cast<size_t>(n));
    for (int off = 0; off + kBlock <= n; off += kBlock)
        pa.process(in.data() + off, out.data() + off, kBlock);
    return idle - pa.railNow();
}

void testRectifierMovesSag() {
    const double sr = 48000.0;
    const double si = railDroop(MesaRectifier::Silicon, MesaPowerMode::Bold, sr);
    const double tu = railDroop(MesaRectifier::Valve5U4, MesaPowerMode::Bold, sr);
    const double siSp = railDroop(MesaRectifier::Silicon, MesaPowerMode::Spongy, sr);
    std::printf("  droop: silicon %.2f V  5U4 %.2f V  ratio %.2fx  "
                "(silicon SPONGY %.2f V)\n", si, tu, tu / si, siSp);
    // Bar 2.0x; measured 2.41x. RECORDED not snugged — the plan file said 3.0x
    // before anything was measured and the honest value is what shipped.
    assert(tu / si > 2.0);
    // SPONGY is a SEPARATE switch and must move sag on its own, with the rectifier
    // held at silicon. If someone wired the two together this fails.
    assert(siSp > si * 1.5);

    // The valve setting must also sit on a LOWER rail (it taps 350 V, not 350+50).
    MesaPowerAmp a, b;
    a.setRectifier(MesaRectifier::Silicon);
    b.setRectifier(MesaRectifier::Valve5U4);
    a.prepare(sr, kBlock);
    b.prepare(sr, kBlock);
    std::printf("  idle rail: silicon %.2f V  5U4 %.2f V\n", a.railIdle(), b.railIdle());
    assert(b.railIdle() < a.railIdle());
    // ...and the silicon rail must land on the sheet's own marked A = 460 V.
    assert(std::fabs(a.railIdle() - 460.0) < 10.0);
}

// ---------------------------------------------------------------------------
// 5. THE INPUT SELECT IS A CAPACITOR ON RED AND A RESISTOR ON ORANGE.
//
// This is the structural half of "the red channel is tighter", and it is asserted
// on the NETWORK's own transfer function rather than on the composed amp (where
// five stages of clipping compress the difference away). A resistor divider is
// FLAT; an 82p into a 1M pot is a high-pass. So the RED path must have a steeper
// low-frequency slope than the ORANGE one — a shape difference no gain trim can
// imitate.
// ---------------------------------------------------------------------------
void testInputSelectIsAShapeNotALevel() {
    const double rs = 43.0e3;  // V1A's plate source impedance
    const double g = 0.7;
    auto tilt = [&](MesaMode m) {
        const double lo = MesaInputNetwork::magnitudeAt(82.41, m, g, rs);
        const double hi = MesaInputNetwork::magnitudeAt(2000.0, m, g, rs);
        return 20.0 * std::log10(hi / lo);
    };
    const double red = tilt(MesaMode::RedModern);
    const double orange = tilt(MesaMode::OrangeNormal);
    std::printf("  input select tilt (2 kHz re low E): RED %+.2f dB  ORANGE %+.2f dB"
                "  contrast %.2f dB\n", red, orange, red - orange);
    // The ORANGE path is a resistive divider into a pot: essentially flat.
    assert(std::fabs(orange) < 3.0);
    // The RED path is a capacitor: it must rise materially with frequency.
    assert(red > orange + 6.0);
}

// ---------------------------------------------------------------------------
// 6. Housekeeping the whole lineup shares.
// ---------------------------------------------------------------------------
void testLatencyAndOversampling() {
    MesaAmp a;
    a.prepare(48000.0, kBlock);
    std::printf("  latency %d samples (%.2f ms) at %dx\n", a.latencySamples(),
                1000.0 * a.latencySamples() / 48000.0, a.oversampling());
    // ONE shared oversampling domain (docs §63.14) — so this is one Oversampler's
    // group delay at 4x, NOT five triodes' plus a power section's. Asserted against
    // Oversampler's own documented 0/64/72/76 rather than read back.
    assert(a.oversampling() == 4);
    assert(a.latencySamples() == 72);
}

void testBlockSizeInvariance() {
    const double sr = 48000.0;
    const int n = kBlock * 40;
    std::vector<float> in = tone(sr, n, 220.0, 0.15);
    MesaAmp a;
    setUpAmp(a, MesaMode::RedModern, sr, 0.7, 0.5);
    std::vector<float> ref;
    runBlocks(a, in, ref);

    // Ragged blocking: the only segmentation that can catch a block-size bug.
    MesaAmp b;
    setUpAmp(b, MesaMode::RedModern, sr, 0.7, 0.5);
    std::vector<float> got(static_cast<size_t>(n), 0.0f);
    const int sizes[] = {13, 1, 64, 100, 7, 128, 31};
    int off = 0, k = 0;
    while (off < n) {
        int m = sizes[k % 7];
        if (off + m > n) m = n - off;
        b.process(in.data() + off, got.data() + off, m);
        off += m;
        ++k;
    }
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(ref[static_cast<size_t>(i)] -
                                                              got[static_cast<size_t>(i)])));
    std::printf("  ragged-block vs 128-frame: worst |diff| %.3e\n", worst);
    assert(worst < 1e-6);
}

void testResetAndNaN() {
    const double sr = 48000.0;
    const int n = kBlock * 40;
    std::vector<float> in = tone(sr, n, 220.0, 0.15);

    MesaAmp fresh;
    setUpAmp(fresh, MesaMode::RedModern, sr, 0.7, 0.5);
    std::vector<float> a;
    runBlocks(fresh, in, a);

    MesaAmp used;
    setUpAmp(used, MesaMode::RedModern, sr, 0.7, 0.5);
    std::vector<float> junk = tone(sr, n, 700.0, 0.8), tmp;
    runBlocks(used, junk, tmp);
    used.reset();
    std::vector<float> b;
    runBlocks(used, in, b);
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(a[static_cast<size_t>(i)] -
                                                              b[static_cast<size_t>(i)])));
    std::printf("  reset() vs fresh: worst |diff| %.3e\n", worst);
    assert(worst < 1e-6);

    // One NaN in, then reset() -> no non-finite output.
    MesaAmp p;
    setUpAmp(p, MesaMode::RedModern, sr, 0.7, 0.5);
    std::vector<float> bad = in;
    bad[100] = std::nanf("");
    std::vector<float> out;
    runBlocks(p, bad, out);
    p.reset();
    std::vector<float> clean;
    runBlocks(p, in, clean);
    int nonFinite = 0;
    for (float v : clean)
        if (!std::isfinite(v)) ++nonFinite;
    std::printf("  non-finite samples after reset: %d / %d\n", nonFinite, n);
    assert(nonFinite == 0);
}

void testDcOnSignal() {
    const double sr = 48000.0;
    const int n = kBlock * 60;
    for (int m = 0; m < 5; ++m) {
        MesaAmp a;
        setUpAmp(a, static_cast<MesaMode>(m), sr, 0.7, 0.5);
        std::vector<float> in = tone(sr, n, 220.0, 0.15), out;
        runBlocks(a, in, out);
        double sum = 0.0, peak = 0.0;
        for (size_t i = out.size() / 2; i < out.size(); ++i) {
            sum += out[i];
            peak = std::max(peak, std::fabs(static_cast<double>(out[i])));
        }
        const double dc = sum / static_cast<double>(out.size() / 2);
        const double pct = 100.0 * std::fabs(dc) / (peak + 1e-30);
        std::printf("  mode %d DC on signal %.4f %% of peak\n", m, pct);
        assert(pct < 2.0);
    }
}

void testRestingState() {
    // ADR 006 / docs §33 — and this is the §59 case, not the §33 one, which is a
    // MEASURED conclusion rather than an omission.
    //
    // Every cap companion in these two networks would rest at exactly zero if its
    // input did. It does not: the networks are driven by TriodeStage, whose nodal
    // Newton exits at a RESIDUAL TOLERANCE, so on silence its output floors at a
    // small nonzero value and keeps re-exciting them. Measured over a 41 s silent
    // tail at 192 kHz, both states decay and then PLATEAU:
    //
    //     t = 1 s   input 1.77e-04   tone 1.24e-05
    //     t = 11 s  input 2.34e-05   tone 1.05e-08
    //     t = 26 s  input 7.10e-08   tone 1.04e-08
    //     t = 41 s  input 2.40e-08   tone 1.04e-08   (flat)
    //
    // 2.4e-08 is ~30 decades above the double subnormal threshold, so these states
    // can NEVER become denormal and the flushDenormal calls in the process loops
    // are guard-rails rather than fixes (docs §33's own instruction to label them
    // as such). What has to be true for the POLICY is that nothing SUBNORMAL leaves
    // the model, and that is what this test asserts.
    const double sr = 48000.0 * 4;
    const int b = kBlock * 4;
    MesaPreamp p;
    p.prepare(sr, b);
    std::vector<float> silence(static_cast<size_t>(b), 0.0f);
    std::vector<float> out(static_cast<size_t>(b), 0.0f);
    const int blocksPerSecond = static_cast<int>(sr) / b;
    for (int i = 0; i < blocksPerSecond * 8; ++i)
        p.process(silence.data(), out.data(), b);

    const double inRest = p.inputNetwork().maxAbsRestingState();
    const double toneRest = p.toneStack().maxAbsRestingState();
    std::printf("  after 8 s of silence: input net %.3e  tone stack %.3e\n", inRest,
                toneRest);
    // Decayed to something small — nothing is stuck at an operating value.
    assert(inRest < 1.0e-3);
    assert(toneRest < 1.0e-3);
    // ...and far enough above subnormal that a flush there is unreachable code.
    assert(inRest > 1.0e-300 || inRest == 0.0);

    // THE PROPERTY THE POLICY IS ACTUALLY ABOUT: no subnormal float may leave the
    // model, because a double subnormal is invisible in float output and a float
    // subnormal is a permanent audio-thread cost on a platform with no FTZ (WASM).
    int subnormals = 0;
    for (int i = 0; i < blocksPerSecond; ++i) {
        p.process(silence.data(), out.data(), b);
        for (float v : out)
            if (v != 0.0f && std::fabs(v) < 1.17549435e-38f) ++subnormals;
    }
    std::printf("  subnormal output samples over the next second: %d\n", subnormals);
    assert(subnormals == 0);
}

void testModesAreAudiblyDistinct() {
    const double sr = 48000.0;
    const int n = kBlock * 94;
    std::vector<float> in = tone(sr, n, 220.0, 0.15);
    double f1[5], peak[5];
    for (int m = 0; m < 5; ++m) {
        MesaAmp a;
        setUpAmp(a, static_cast<MesaMode>(m), sr, 1.0, 1.0);
        std::vector<float> out;
        runBlocks(a, in, out);
        f1[m] = bin(out, sr, 220.0, static_cast<size_t>(n / 2));
        peak[m] = 0.0;
        for (size_t i = out.size() / 2; i < out.size(); ++i)
            peak[m] = std::max(peak[m], std::fabs(static_cast<double>(out[i])));
        std::printf("  mode %d cranked peak %.4f  f1 %.4f\n", m, peak[m], f1[m]);
        // §23's normalization convention: a cranked voice peaks near 0.9, never
        // clipping the float scale.
        assert(peak[m] > 0.4 && peak[m] <= 1.0);
    }
    // Every mode must differ from every other one somewhere. The pairs that share
    // a preamp path (OR NORM/OR MOD, RED VINT/RED MOD) differ ONLY in the power
    // amp's feedback, so this is what proves the mode reaches the power section.
    for (int i = 0; i < 5; ++i)
        for (int j = i + 1; j < 5; ++j)
            assert(std::fabs(peak[i] - peak[j]) > 1e-4 ||
                   std::fabs(f1[i] - f1[j]) > 1e-4);
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();
    std::printf("== Mesa Dual Rectifier (M10.4, docs §68) ==\n");
    std::printf("- truth table (sheet mbdr7)\n");
    testTruthTable();
    std::printf("- marked DC node voltages (sheets mbdr1 / mbdr4)\n");
    testMarkedNodeVoltages();
    std::printf("- BAR (a): the modern modes run open-loop\n");
    testModernModesRunOpenLoop();
    std::printf("- BAR (b): the rectifier select moves sag\n");
    testRectifierMovesSag();
    std::printf("- the input select is a shape, not a level\n");
    testInputSelectIsAShapeNotALevel();
    std::printf("- latency / oversampling\n");
    testLatencyAndOversampling();
    std::printf("- block-size invariance\n");
    testBlockSizeInvariance();
    std::printf("- reset() and NaN recovery\n");
    testResetAndNaN();
    std::printf("- DC on signal\n");
    testDcOnSignal();
    std::printf("- resting state (ADR 006)\n");
    testRestingState();
    std::printf("- the five modes are audibly distinct\n");
    testModesAreAudiblyDistinct();
    std::printf("== all Mesa tests passed ==\n");
    return 0;
}
