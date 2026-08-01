// Non-finite parameter guard + engine recovery — the 2026-07-24 audit's finding 1.
//
// THE BUG THIS PINS. Every parameter clamp in the tree was transparent to NaN,
// because both comparisons of `v < 0 ? 0 : (v > 1 ? 1 : v)` are false for NaN
// (`std::clamp` and `Math.min/Math.max` have the same hole). One NaN write
// therefore reached a recursive state — a smoother value, a biquad delay, a WDF
// cap voltage, a tube Newton warm start — and LATCHED: measured 12672/12672
// non-finite samples in a 1 s window, forever, for Jcm800Amp / AmpModel /
// RatModel alike. Writing a good value afterwards never cleared it, and there was
// no reset anywhere in the valve-amp tree or the C ABI, so the only recovery was
// destroy + recreate. Inf was handled correctly all along; NaN was not. The
// reachable path was the assistant: the tool schema declares `minimum: 0,
// maximum: 1` but nothing enforced it, so a non-numeric model emission ("max",
// null, {}) became NaN via Number(...) and the worklet passed it straight on.
//
// WHAT THIS SUITE ASSERTS — player-observable properties, not identities:
//
//   A. THE ABI GATE. Through the exact C ABI the worklet calls, a non-finite
//      write to ANY parameter of ANY unit is REJECTED: the render afterwards is
//      not merely finite, it is BIT-IDENTICAL to a run that never received the
//      bad write. "A malformed assistant message must not change my tone" is the
//      property; bit-identity is how you check it without a tolerance to argue
//      about.
//
//   B. THE IN-MODEL CLAMPS. Driving the C++ models directly — the path the JUCE
//      plugin takes, which never touches the C ABI — a non-finite write must
//      leave the unit finite, AND a subsequent good write must bring the unit
//      back to the reference render. This is the half of the fix an ABI-only gate
//      would miss, and it is what fails hardest against unmodified code.
//
//   C. THE RECOVERY PATH. Poison a unit through a channel no parameter guard can
//      cover — a NaN AUDIO SAMPLE — confirm the poisoning actually latches (so
//      the assertion below is not vacuous), then reset() and require the unit to
//      render the reference again. Recovery must not go through prepare(): a tube
//      stage's prepare() re-solves its DC point and settles ~50 k silent samples
//      per stage (~69 ms for a whole Jcm800Amp).
//
// Coverage: all seven pedals (rat / sd / ts / muff / gold / comp / phaser) and all four amp
// voices (clean120 / jcm800 / twin / ac30), every parameter id of each, times
// {NaN, +Inf, -Inf} — over both the C ABI and the direct C++ API.
//
// Everything runs at 48 kHz in 128-frame IN-PLACE blocks: the AudioWorklet's rate
// and calling convention, i.e. exactly how the shipped engine is driven.
//
// Build note: -DCLIPPER_NAN_TEST_NO_RESET compiles blocks A and B only, so the
// suite can be run against a checkout that predates the reset() API — that is how
// "this test fails before the fix" was verified.
//
// No framework: int main + <cassert>, as every other suite here.

#include "clipper/dsp/Ac30Amp.h"
#include "clipper/dsp/AmpModel.h"
#include "clipper/dsp/CompModel.h"
#include "clipper/dsp/GateModel.h"
#include "clipper/dsp/GoldModel.h"
#include "clipper/dsp/Jcm800Amp.h"
#include "clipper/dsp/MuffModel.h"
#include "clipper/dsp/PhaserModel.h"
#include "clipper/dsp/RatModel.h"
#include "clipper/dsp/SdModel.h"
#include "clipper/dsp/TsModel.h"
#include "clipper/dsp/TwinAmp.h"

#include "support/AssertsLive.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// --- The worklet's C ABI (clipper_c_api.cpp), declared verbatim --------------
extern "C" {
void* rat_create(float);    void rat_destroy(void*);
void  rat_set_param(void*, int, float);
void  rat_process(void*, const float*, float*, int);
void* sd_create(float);     void sd_destroy(void*);
void  sd_set_param(void*, int, float);
void  sd_process(void*, const float*, float*, int);
void* ts_create(float);     void ts_destroy(void*);
void  ts_set_param(void*, int, float);
void  ts_process(void*, const float*, float*, int);
void* muff_create(float);   void muff_destroy(void*);
void  muff_set_param(void*, int, float);
void  muff_process(void*, const float*, float*, int);
void* comp_create(float);   void comp_destroy(void*);
void  comp_set_param(void*, int, float);
void  comp_process(void*, const float*, float*, int);
void  comp_reset(void*);
void* gate_create(float);   void gate_destroy(void*);
void  gate_set_param(void*, int, float);
void  gate_process(void*, const float*, float*, int);
void  gate_reset(void*);
void* gold_create(float);   void gold_destroy(void*);
void  gold_set_param(void*, int, float);
void  gold_process(void*, const float*, float*, int);
void* phaser_create(float); void phaser_destroy(void*);
void  phaser_set_param(void*, int, float);
void  phaser_process(void*, const float*, float*, int);
void* amp_create(float);    void amp_destroy(void*);
void  amp_set_model(void*, int);
void  amp_set_param(void*, int, float);
void  amp_process(void*, const float*, float*, int);
#ifndef CLIPPER_NAN_TEST_NO_RESET
void  rat_reset(void*);
void  sd_reset(void*);
void  ts_reset(void*);
void  muff_reset(void*);
void  gold_reset(void*);
void  phaser_reset(void*);
void  amp_reset(void*);
#endif
}

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr double kFs = 48000.0;   // the AudioWorklet rate
constexpr int kQuantum = 128;     // the AudioWorklet render quantum

const float kNaN = std::numeric_limits<float>::quiet_NaN();
const float kInf = std::numeric_limits<float>::infinity();

// --- signal helpers ---------------------------------------------------------

bool allFinite(const std::vector<float>& x) {
    for (float v : x)
        if (!std::isfinite(v)) return false;
    return true;
}

size_t countNonFinite(const std::vector<float>& x) {
    size_t n = 0;
    for (float v : x)
        if (!std::isfinite(v)) ++n;
    return n;
}

double peakOf(const std::vector<float>& x) {
    double p = 0.0;
    for (float v : x) p = std::max(p, std::fabs(static_cast<double>(v)));
    return p;
}

// Max absolute sample difference. Returns infinity if either side is non-finite,
// so a poisoned render can never be mistaken for a matching one.
double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    assert(a.size() == b.size());
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        if (!std::isfinite(d)) return std::numeric_limits<double>::infinity();
        m = std::max(m, d);
    }
    return m;
}

std::vector<float> silence(double secs) {
    return std::vector<float>(static_cast<size_t>(secs * kFs), 0.0f);
}

// The house plucked-string burst (same shape as test_player_expectations.cpp's
// standardPluck / the render CLI): 220 Hz, six harmonics, brighter partials decay
// faster, peak normalized to −20 dBFS — a normal single-note guitar DI level.
std::vector<float> pluck(double secs, float peakTarget = 0.1f) {
    const int n = static_cast<int>(secs * kFs);
    std::vector<float> sig(static_cast<size_t>(n));
    const double attackS = 0.004;
    for (int i = 0; i < n; ++i) {
        const double t = i / kFs;
        const double attack = 1.0 - std::exp(-t / attackS);
        double s = 0.0;
        for (int h = 1; h <= 6; ++h)
            s += (1.0 / h) * std::exp(-t * (2.0 + 1.2 * h)) *
                 std::sin(kTwoPi * 220.0 * h * t);
        sig[static_cast<size_t>(i)] = static_cast<float>(attack * s);
    }
    const double pk = peakOf(sig);
    const float norm = pk > 1e-12 ? static_cast<float>(peakTarget / pk) : 1.0f;
    for (float& v : sig) v *= norm;
    return sig;
}

// ---------------------------------------------------------------------------
// One unit under test, driven the way the worklet drives it.
// ---------------------------------------------------------------------------
struct Unit {
    std::string name;
    std::vector<int> paramIds;
    std::vector<float> defaults;   // parallel to paramIds
    // Ids that carry a small INTEGER (a mode selector), not a 0..1 knob. Their
    // documented non-finite fallback is 0 for every non-finite input (including
    // +Inf), because std::lround(Inf) is unspecified — see ParamGuard::paramToInt.
    std::vector<int> modeParamIds;
    std::function<void(int, float)> setParam;
    std::function<void(float*, int)> processInPlace;  // in-place, <= kQuantum frames
    std::function<void()> reset;                      // empty when unavailable
    std::shared_ptr<void> owner;                      // keeps the instance alive
};

bool isModeParam(const Unit& u, int id) {
    for (int m : u.modeParamIds)
        if (m == id) return true;
    return false;
}

using UnitMaker = std::function<Unit()>;

void applyDefaults(Unit& u) {
    for (size_t i = 0; i < u.paramIds.size(); ++i)
        u.setParam(u.paramIds[i], u.defaults[i]);
}

// Render `in` through the unit in 128-frame IN-PLACE blocks (the worklet's exact
// convention) and return the result.
std::vector<float> render(Unit& u, const std::vector<float>& in) {
    std::vector<float> buf = in;
    size_t off = 0;
    while (off < buf.size()) {
        const int n = static_cast<int>(std::min<size_t>(kQuantum, buf.size() - off));
        u.processInPlace(buf.data() + off, n);
        off += static_cast<size_t>(n);
    }
    return buf;
}

// Settle: push silence through so every smoother has reached its target and every
// blocking / sag / grid-leak state has recovered to idle. 0.4 s is >10 time
// constants of the slowest modelled recovery (the AC30's 40 ms sag release; the
// triode grid-leak τ = Rgl·Cc = 22 ms).
constexpr double kSettleSecs = 0.4;
constexpr double kProbeSecs = 0.3;

void settle(Unit& u) { render(u, silence(kSettleSecs)); }

// ---------------------------------------------------------------------------
// Makers — the C ABI set (block A/C) ...
// ---------------------------------------------------------------------------
using AbiCreate = void* (*)(float);
using AbiDestroy = void (*)(void*);
using AbiSetParam = void (*)(void*, int, float);
using AbiProcess = void (*)(void*, const float*, float*, int);
using AbiReset = void (*)(void*);

Unit makeAbiUnit(const char* name, AbiCreate create, AbiDestroy destroy,
                 AbiSetParam setp, AbiProcess proc, AbiReset rst,
                 std::vector<int> ids, std::vector<float> defs,
                 int ampModel = -1) {
    void* h = create(static_cast<float>(kFs));
    if (ampModel >= 0) amp_set_model(h, ampModel);
    Unit u;
    u.name = name;
    u.paramIds = std::move(ids);
    u.defaults = std::move(defs);
    u.owner = std::shared_ptr<void>(h, [destroy](void* p) { destroy(p); });
    u.setParam = [h, setp](int id, float v) { setp(h, id, v); };
    u.processInPlace = [h, proc](float* b, int n) { proc(h, b, b, n); };
    if (rst) u.reset = [h, rst]() { rst(h); };
    return u;
}

// ... and the direct C++ set (block B/C), which is the path the JUCE plugin takes.
template <typename Model>
Unit makeCppUnit(const char* name, std::vector<int> ids, std::vector<float> defs,
                 bool hasReset, int oversampling = 4) {
    auto m = std::make_shared<Model>();
    m->setOversampling(oversampling);
    m->prepare(kFs, kQuantum);
    Unit u;
    u.name = name;
    u.paramIds = std::move(ids);
    u.defaults = std::move(defs);
    u.owner = m;
    Model* raw = m.get();
    u.setParam = [raw](int id, float v) { raw->setParameter(id, v); };
    u.processInPlace = [raw](float* b, int n) { raw->process(b, b, n); };
#ifndef CLIPPER_NAN_TEST_NO_RESET
    if (hasReset) u.reset = [raw]() { raw->reset(); };
#else
    (void)hasReset;
#endif
    return u;
}

// AmpModel has no setOversampling (it is strictly linear), so it needs its own.
Unit makeCleanAmpCppUnit() {
    auto m = std::make_shared<clipper::dsp::AmpModel>();
    m->prepare(kFs, kQuantum);
    Unit u;
    u.name = "AmpModel (clean 120, direct C++)";
    // Every id AmpModel handles. 5 is the chain-level cab toggle and is not one of
    // its own, so it is excluded here (the ABI unit covers it).
    u.paramIds = {0, 1, 2, 3, 4, 6, 7, 8, 9};
    u.defaults = {0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.3f, 0.5f, 0.0f, 0.25f};
    u.modeParamIds = {8};  // PARAM_CHORUS_MODE carries 0/1/2, not a knob
    u.owner = m;
    clipper::dsp::AmpModel* raw = m.get();
    u.setParam = [raw](int id, float v) { raw->setParameter(id, v); };
    u.processInPlace = [raw](float* b, int n) { raw->process(b, b, n); };
#ifndef CLIPPER_NAN_TEST_NO_RESET
    u.reset = [raw]() { raw->reset(); };
#endif
    return u;
}

// The amp chain's C ABI param ids (mirror web/src/params.ts / clipper_c_api.cpp):
// 0 volume, 1 bass, 2 mid, 3 treble, 4 bright, 5 CAB toggle, 6 speed, 7 depth,
// 8 chorus mode / trem enable, 9 reverb, 10 jcm gain, 11 presence / AC30 cut,
// 12 jcm master. Every one of them is exercised on every voice.
const std::vector<int> kAmpIds = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
const std::vector<float> kAmpDefaults = {0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 0.3f,
                                         0.5f, 0.0f, 0.25f, 0.6f, 0.4f, 0.6f};

std::vector<UnitMaker> abiMakers() {
    std::vector<UnitMaker> v;
    const std::vector<int> three = {0, 1, 2};
    const std::vector<float> dirt = {0.6f, 0.5f, 0.6f};
    AbiReset ratR = nullptr, sdR = nullptr, tsR = nullptr, muffR = nullptr,
             goldR = nullptr, phaserR = nullptr, ampR = nullptr, compR = nullptr,
             gateR = nullptr;
#ifndef CLIPPER_NAN_TEST_NO_RESET
    ratR = rat_reset; sdR = sd_reset; tsR = ts_reset; muffR = muff_reset;
    goldR = gold_reset; phaserR = phaser_reset; ampR = amp_reset;
    compR = comp_reset;
    gateR = gate_reset;
#endif
    v.push_back([=] { return makeAbiUnit("rat_* (ABI)", rat_create, rat_destroy,
                                         rat_set_param, rat_process, ratR, three, dirt); });
    v.push_back([=] { return makeAbiUnit("sd_* (ABI)", sd_create, sd_destroy,
                                         sd_set_param, sd_process, sdR, three, dirt); });
    v.push_back([=] { return makeAbiUnit("ts_* (ABI)", ts_create, ts_destroy,
                                         ts_set_param, ts_process, tsR, three, dirt); });
    v.push_back([=] { return makeAbiUnit("muff_* (ABI)", muff_create, muff_destroy,
                                         muff_set_param, muff_process, muffR, three, dirt); });
    v.push_back([=] { return makeAbiUnit("gold_* (ABI)", gold_create, gold_destroy,
                                         gold_set_param, gold_process, goldR, three, dirt); });
    // M13.1: the compressor writes slots 0 and 2 only (slot 1 is unused), but the
    // guard must still reject a NaN arriving on ANY slot, so all three are driven.
    v.push_back([=] { return makeAbiUnit("comp_* (ABI)", comp_create, comp_destroy,
                                         comp_set_param, comp_process, compR,
                                         three, {0.5f, 0.5f, 0.4f}); });
    // M13.6a: the gate writes slots 0 and 2 only (slot 1 is unused), but the
    // guard must still reject a NaN arriving on ANY slot, so all three are driven.
    v.push_back([=] { return makeAbiUnit("gate_* (ABI)", gate_create, gate_destroy,
                                         gate_set_param, gate_process, gateR,
                                         three, {0.35f, 0.5f, 0.5f}); });
    v.push_back([=] { return makeAbiUnit("phaser_* (ABI)", phaser_create, phaser_destroy,
                                         phaser_set_param, phaser_process, phaserR,
                                         three, {0.35f, 0.5f, 0.5f}); });
    const char* voice[4] = {"amp_* clean120 (ABI)", "amp_* jcm800 (ABI)",
                            "amp_* twin (ABI)", "amp_* ac30 (ABI)"};
    for (int model = 0; model < 4; ++model)
        v.push_back([=] { return makeAbiUnit(voice[model], amp_create, amp_destroy,
                                             amp_set_param, amp_process, ampR,
                                             kAmpIds, kAmpDefaults, model); });
    return v;
}

std::vector<UnitMaker> cppMakers() {
    using namespace clipper::dsp;
    std::vector<UnitMaker> v;
    const std::vector<int> three = {0, 1, 2};
    const std::vector<float> dirt = {0.6f, 0.5f, 0.6f};
    v.push_back([=] { return makeCppUnit<RatModel>("RatModel (direct C++)", three, dirt, true); });
    v.push_back([=] { return makeCppUnit<SdModel>("SdModel (direct C++)", three, dirt, true); });
    v.push_back([=] { return makeCppUnit<TsModel>("TsModel (direct C++)", three, dirt, true); });
    v.push_back([=] { return makeCppUnit<MuffModel>("MuffModel (direct C++)", three, dirt, true); });
    v.push_back([=] { return makeCppUnit<GoldModel>("GoldModel (direct C++)", three, dirt, true); });
    v.push_back([=] { return makeCppUnit<CompModel>("CompModel (direct C++)", three,
                                                    {0.5f, 0.5f, 0.4f}, true); });
    v.push_back([=] { return makeCppUnit<GateModel>("GateModel (direct C++)", three,
                                                    {0.35f, 0.5f, 0.5f}, true); });
    v.push_back([] { return makeCleanAmpCppUnit(); });
    // Jcm800Amp: 0 gain, 1 master, 2 bass, 3 mid, 4 treble, 5 presence, 6 reverb.
    v.push_back([] {
        return makeCppUnit<clipper::dsp::Jcm800Amp>(
            "Jcm800Amp (direct C++)", {0, 1, 2, 3, 4, 5, 6},
            {0.6f, 0.5f, 0.5f, 0.5f, 0.5f, 0.4f, 0.2f}, true);
    });
    // TwinAmp: 0 volume, 1 bass, 2 mid, 3 treble, 4 bright, 5 reverb, 6 speed,
    // 7 intensity, 8 tremolo enable.
    v.push_back([] {
        return makeCppUnit<clipper::dsp::TwinAmp>(
            "TwinAmp (direct C++)", {0, 1, 2, 3, 4, 5, 6, 7, 8},
            {0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.2f, 0.4f, 0.5f, 1.0f}, true);
    });
    // Ac30Amp: 0 volume, 1 bass, 2 treble, 3 top cut, 4 reverb.
    v.push_back([] {
        return makeCppUnit<clipper::dsp::Ac30Amp>(
            "Ac30Amp (direct C++)", {0, 1, 2, 3, 4},
            {0.6f, 0.5f, 0.5f, 0.3f, 0.2f}, true);
    });
    // PhaserModel takes prepare(sampleRate) only and has no oversampling, so it is
    // built by hand rather than through makeCppUnit.
    v.push_back([] {
        auto m = std::make_shared<PhaserModel>();
        m->prepare(kFs);
        Unit u;
        u.name = "PhaserModel (direct C++)";
        u.paramIds = {0, 1, 2};
        u.defaults = {0.35f, 0.5f, 0.5f};
        u.owner = m;
        PhaserModel* raw = m.get();
        u.setParam = [raw](int id, float v) { raw->setParameter(id, v); };
        u.processInPlace = [raw](float* b, int n) { raw->process(b, b, n); };
        u.reset = [raw]() { raw->reset(); };
        return u;
    });
    return v;
}

const char* poisonName(float v) {
    if (std::isnan(v)) return "NaN";
    return v > 0.0f ? "+Inf" : "-Inf";
}

double toDb(double a) { return 20.0 * std::log10(a + 1e-30); }

double rmsOf(const std::vector<float>& x) {
    double s = 0.0;
    for (float v : x) s += static_cast<double>(v) * v;
    return x.empty() ? 0.0 : std::sqrt(s / x.size());
}

// "The latch is gone" bar for block B: after the good write, the unit's broadband
// level must be within 40 dB of the reference — i.e. it is neither dead silent nor
// blown up. The pre-fix failure mode was total (100 % non-finite samples forever),
// and the failure mode a partial fix could leave is a unit stuck at the clamp or
// muted, both of which this catches.
//
// Why the bar is loose and not a tight level match: two effects legitimately move
// the level after ANY knob excursion, poisoned or not.
//   * Knob hysteresis. Driving the JCM's MASTER to 1.0 and back charges the triode
//     blocking caps (τ = Rgl·Cc = 22 ms) and pulls the B+ rail (τ = 7.5 ms); the next
//     render sits 0.10 dB off a never-touched reference at a 0.4 s settle, and 0.000 dB
//     off at a 3 s settle. An ordinary in-range 0.5 → 1.0 → 0.5 sweep leaves EXACTLY
//     the same residue (verified out-of-band: the two renders are bit-identical).
//   * LFO phase. Moving the Twin's tremolo SPEED moves where its LFO is, so the level
//     over a 0.3 s window shifts by ~3 dB — again identically for an in-range write.
// Neither is a guard property, so sample-exactness is pinned by the EQUIVALENCE
// claim (B3) instead, which is immune to both because the control run performs the
// same excursion with the legal value.
constexpr double kLatchGoneToleranceDb = 40.0;

// Block C's C3 bar: by how much resetting a HEALTHY unit is allowed to move its
// level. It is not exactly zero, and the reason is the only thing that differs
// between a reset unit and a naturally-settled one:
//
//   A OnePoleSmoother settling toward its target stalls ONE FLOAT ULP short of it
//   forever — `value_ += coeff*(target_ - value_)` underflows to `value_ += 0` long
//   before the residual reaches the 1e-30 denormal snap. reset() snaps value_ onto
//   the target exactly, so a reset unit's knob is one ULP different from (in fact
//   marginally more accurate than) a settled one. On the RAT that one ULP of
//   pre-gain (~6e-8 of 0.6) emerges as 3.2e-6 at the output, −96.7 dB below peak.
//   Confirmed by bisection out-of-band: reset() with the smoother snaps removed is
//   BIT-IDENTICAL to a settled fresh instance, i.e. nothing else survives a reset.
//
// That is why C2 compares reset-against-reset (where it can demand bit-exactness)
// and C3 measures the reset-vs-never-reset difference as a LEVEL instead.
//
// MEASURED C3: every unit without a running modulator lands at 0.0000–0.0002 dB.
// The TwinAmp with its tremolo engaged lands at 0.039 dB, because reset() restarts
// the opto tremolo's LFO at phase 0 — an intended consequence (the LFO phase IS
// state, and a recovery path has no better phase to resume from), and 0.039 dB is
// a small fraction of the tremolo's own modulation depth. 0.15 dB is ~4x headroom
// over that while still catching a reset that mutes a stage, drops a coefficient,
// or forgets to re-park a DC operating point — those move the level by dB, not
// hundredths.
constexpr double kResetLevelTolDb = 0.15;

// The in-range value each non-finite input is DOCUMENTED to behave as. This is the
// contract from ParamGuard.h: clampParam01 maps NaN to the low end and +Inf to the
// high end, and paramToInt maps every non-finite to its fallback (0).
float equivalentInRangeValue(const Unit& u, int paramId, float poison) {
    if (isModeParam(u, paramId)) return 0.0f;
    return (std::isfinite(poison) || poison < 0.0f || std::isnan(poison)) ? 0.0f : 1.0f;
}

// ===========================================================================
// A. The C ABI gate: a non-finite write is REJECTED, so the render afterwards is
//    BIT-IDENTICAL to one that never saw it. Rejection (not clamping) is the
//    contract: NaN has no in-range meaning, so the knob keeps its value.
// ===========================================================================
void testAbiRejectsNonFinite() {
    std::printf("--- A. C ABI: non-finite parameter writes are rejected ---\n");
    const std::vector<float> probe = pluck(kProbeSecs);
    const float poisons[3] = {kNaN, kInf, -kInf};
    size_t combos = 0;

    for (const UnitMaker& make : abiMakers()) {
        Unit refUnit = make();
        applyDefaults(refUnit);
        settle(refUnit);
        const std::vector<float> ref = render(refUnit, probe);
        assert(allFinite(ref) && "reference render is not finite — bad test setup");
        // Non-trivial reference: without this the comparisons below could pass on
        // "silence equals silence".
        assert(peakOf(ref) > 1.0e-5 && "reference render is silent — bad test setup");

        double worst = 0.0;
        for (int pi = 0; pi < 3; ++pi) {
            for (size_t k = 0; k < refUnit.paramIds.size(); ++k) {
                Unit u = make();
                applyDefaults(u);
                settle(u);
                u.setParam(u.paramIds[k], poisons[pi]);
                const std::vector<float> got = render(u, probe);
                if (!allFinite(got)) {
                    std::printf("  FAIL %-24s param %d <- %s : %zu/%zu non-finite samples\n",
                                u.name.c_str(), u.paramIds[k], poisonName(poisons[pi]),
                                countNonFinite(got), got.size());
                }
                assert(allFinite(got) && "non-finite parameter reached the engine "
                                         "through the C ABI");
                const double d = maxAbsDiff(got, ref);
                if (d != 0.0) {
                    std::printf("  FAIL %-24s param %d <- %s : write was not rejected "
                                "(max diff %.3e vs ref peak %.3e)\n",
                                u.name.c_str(), u.paramIds[k], poisonName(poisons[pi]),
                                d, peakOf(ref));
                }
                assert(d == 0.0 && "a non-finite parameter write changed the tone — "
                                   "the ABI must reject it, not clamp it");
                worst = std::max(worst, d);
                ++combos;
            }
        }
        std::printf("  %-24s %2zu params x 3 poisons: all finite, max deviation "
                    "from reference %.1e (peak %.3f)\n",
                    refUnit.name.c_str(), refUnit.paramIds.size(), worst, peakOf(ref));
    }
    std::printf("  %zu (unit, param, poison) combinations rejected cleanly\n\n", combos);
}

// ===========================================================================
// B. The in-model clamps. Same poisoning, but straight into the C++ models — the
//    path the JUCE plugin uses, which never crosses the C ABI, so the ABI gate
//    cannot help it. Three assertions per (param, poison):
//
//      B1  the poisoned render is FINITE (this is the assertion the pre-fix code
//          fails catastrophically: 100 % non-finite samples, forever);
//      B2  after writing the good value back, the LEVEL comes back (the pre-fix
//          signature was that it never did — no write ever cleared the latch);
//      B3  the poisoned write is BIT-IDENTICAL to writing the in-range value it
//          documents itself as clamping to (NaN → 0, +Inf → 1, mode ids → 0).
//          This is the sample-exact half of the contract, and unlike a comparison
//          against a never-touched reference it is not confounded by the knob
//          hysteresis every high-gain amp legitimately has.
// ===========================================================================
void testModelClampsRejectNonFinite() {
    std::printf("--- B. Direct C++ setParameter: models clamp non-finite safely ---\n");
    const std::vector<float> probe = pluck(kProbeSecs);
    const float poisons[3] = {kNaN, kInf, -kInf};

    for (const UnitMaker& make : cppMakers()) {
        Unit refUnit = make();
        applyDefaults(refUnit);
        settle(refUnit);
        const std::vector<float> ref = render(refUnit, probe);
        const double refPeak = peakOf(ref);
        const double refRmsDb = toDb(rmsOf(ref));
        assert(allFinite(ref) && "reference render is not finite — bad test setup");
        assert(refPeak > 1.0e-5 && "reference render is silent — bad test setup");

        double worstLevelDb = 0.0;
        for (int pi = 0; pi < 3; ++pi) {
            for (size_t k = 0; k < refUnit.paramIds.size(); ++k) {
                const int id = refUnit.paramIds[k];
                const float poison = poisons[pi];

                // --- the poisoned run ---
                Unit u = make();
                applyDefaults(u);
                settle(u);
                u.setParam(id, poison);
                const std::vector<float> poisoned = render(u, probe);
                if (!allFinite(poisoned)) {
                    std::printf("  FAIL %-28s param %d <- %s : %zu/%zu non-finite samples\n",
                                u.name.c_str(), id, poisonName(poison),
                                countNonFinite(poisoned), poisoned.size());
                }
                assert(allFinite(poisoned) &&
                       "B1: a non-finite parameter reached recursive state — the "
                       "model's clamp is transparent to it");
                u.setParam(id, refUnit.defaults[k]);
                settle(u);
                const std::vector<float> recovered = render(u, probe);
                assert(allFinite(recovered) && "B1: still non-finite after a good write");

                // --- the equivalent in-range run: same excursion, legal value ---
                Unit c = make();
                applyDefaults(c);
                settle(c);
                c.setParam(id, equivalentInRangeValue(refUnit, id, poison));
                render(c, probe);
                c.setParam(id, refUnit.defaults[k]);
                settle(c);
                const std::vector<float> control = render(c, probe);

                // B2: the latch is gone — the unit is making audio again, at a sane
                // level. (Sample-exactness is B3's job; see kLatchGoneToleranceDb.)
                const double levelErrDb = std::fabs(toDb(rmsOf(recovered)) - refRmsDb);
                if (!(levelErrDb <= kLatchGoneToleranceDb)) {
                    std::printf("  FAIL %-28s param %d <- %s : unit did not come back "
                                "(level %.2f dB off the reference, bar %.0f dB)\n",
                                u.name.c_str(), id, poisonName(poison), levelErrDb,
                                kLatchGoneToleranceDb);
                }
                assert(levelErrDb <= kLatchGoneToleranceDb &&
                       "B2: the unit is silent or blown up after a good parameter write");
                worstLevelDb = std::max(worstLevelDb, levelErrDb);

                // B3: indistinguishable from the in-range value it clamps to.
                const double d = maxAbsDiff(recovered, control);
                if (d != 0.0) {
                    std::printf("  FAIL %-28s param %d <- %s : not equivalent to the "
                                "in-range value %.1f it clamps to (max diff %.3e)\n",
                                u.name.c_str(), id, poisonName(poison),
                                equivalentInRangeValue(refUnit, id, poison), d);
                }
                assert(d == 0.0 &&
                       "B3: a non-finite write did not behave as the in-range value it "
                       "documents itself as clamping to");
            }
        }
        std::printf("  %-28s %2zu params x 3 poisons: finite throughout, exactly "
                    "equivalent to the clamped in-range write, worst level error "
                    "%.3f dB (peak %.3f)\n",
                    refUnit.name.c_str(), refUnit.paramIds.size(), worstLevelDb, refPeak);
    }
    std::printf("\n");
}

#ifndef CLIPPER_NAN_TEST_NO_RESET
// ===========================================================================
// C. The recovery path. Parameter guards cannot cover every channel — a NaN can
//    also arrive as an AUDIO SAMPLE, straight off a broken input device or an
//    upstream node. That is precisely why a reset export has to exist. Three
//    assertions per unit:
//
//      C1  the poisoning LATCHES (100 % non-finite output, still non-finite after
//          0.4 s of silence). Without this the rest would be vacuous.
//      C2  after reset(), the render is BIT-IDENTICAL to a clean instance that was
//          also reset — so every poisoned state was cleared, exactly, with nothing
//          left in a filter memory, a delay line, a cap voltage or a warm start.
//          Comparing reset-against-reset is what makes bit-exactness the right bar
//          (see kResetLevelTolDb for the one thing that differs otherwise).
//      C3  reset() is INAUDIBLE on a healthy unit: a clean instance's level is
//          unchanged by resetting it.
// ===========================================================================
void testResetRecoversPoisonedUnit() {
    std::printf("--- C. reset() recovers a unit poisoned by a NaN audio sample ---\n");
    const std::vector<float> probe = pluck(kProbeSecs);

    std::vector<UnitMaker> makers = abiMakers();
    for (UnitMaker& m : cppMakers()) makers.push_back(m);

    size_t checked = 0;
    for (const UnitMaker& make : makers) {
        Unit probeUnit = make();
        if (!probeUnit.reset) continue;

        // Baseline 1: a clean instance, never reset.
        Unit fresh = make();
        applyDefaults(fresh);
        settle(fresh);
        const std::vector<float> refNoReset = render(fresh, probe);
        const double refPeak = peakOf(refNoReset);
        assert(allFinite(refNoReset) && refPeak > 1.0e-5 && "bad test setup");

        // Baseline 2: a clean instance that WAS reset — the like-for-like target.
        Unit clean = make();
        applyDefaults(clean);
        settle(clean);
        clean.reset();
        settle(clean);
        const std::vector<float> ref = render(clean, probe);
        assert(allFinite(ref) && "resetting a healthy unit made it non-finite");

        // C3: resetting a healthy unit does not change what it sounds like.
        const double resetLevelDb =
            std::fabs(toDb(rmsOf(ref)) - toDb(rmsOf(refNoReset)));
        if (!(resetLevelDb <= kResetLevelTolDb)) {
            std::printf("  FAIL %-28s reset() changed a healthy unit's level by %.4f dB "
                        "(bar %.3f dB)\n",
                        probeUnit.name.c_str(), resetLevelDb, kResetLevelTolDb);
        }
        assert(resetLevelDb <= kResetLevelTolDb &&
               "C3: reset() is audible on a healthy unit");

        // C1: poison with a single NaN sample in one render quantum.
        Unit u = make();
        applyDefaults(u);
        settle(u);
        std::vector<float> bad(kQuantum, 0.0f);
        bad[0] = kNaN;
        u.processInPlace(bad.data(), kQuantum);
        settle(u);
        const std::vector<float> stillBad = render(u, probe);
        const size_t poisonedCount = countNonFinite(stillBad);
        if (poisonedCount == 0) {
            std::printf("  FAIL %-28s a NaN input sample did not latch — the poisoning "
                        "premise no longer holds, so this test proves nothing\n",
                        u.name.c_str());
        }
        assert(poisonedCount > 0 &&
               "C1: NaN input did not latch — nothing left for reset() to prove");

        // C2: recover, and require exactness against the reset baseline.
        u.reset();
        settle(u);
        const std::vector<float> recovered = render(u, probe);
        const double d = maxAbsDiff(recovered, ref);
        if (!(allFinite(recovered) && d == 0.0)) {
            std::printf("  FAIL %-28s reset() did not fully recover: %zu/%zu non-finite, "
                        "max diff from a reset clean instance %.3e\n",
                        u.name.c_str(), countNonFinite(recovered), recovered.size(), d);
        }
        assert(allFinite(recovered) && "C2: reset() left the unit non-finite");
        assert(d == 0.0 && "C2: reset() did not restore a clean instance's exact state");
        ++checked;
        std::printf("  %-28s poisoned %zu/%zu non-finite -> after reset() 0/%zu, "
                    "bit-identical to a reset clean instance; reset() shifts a healthy "
                    "unit by %.4f dB (peak %.3f)\n",
                    u.name.c_str(), poisonedCount, stillBad.size(), recovered.size(),
                    resetLevelDb, refPeak);
    }
    assert(checked >= 10 && "reset coverage collapsed — expected every pedal + amp voice");
    std::printf("  %zu units recovered by reset()\n\n", checked);
}

// reset() must be a CHEAP re-park, not a re-prepare. prepare() re-solves every tube
// DC operating point and settles ~50 k silent samples per triode stage; that is not
// an acceptable recovery path for a live rig. Asserting a ratio (not an absolute
// millisecond figure) keeps this meaningful on any machine and in any build.
void testResetIsCheaperThanPrepare() {
    std::printf("--- C2. reset() is orders of magnitude cheaper than prepare() ---\n");
    clipper::dsp::Jcm800Amp amp;
    amp.setOversampling(4);

    const int kPrepareRuns = 3;
    const std::clock_t p0 = std::clock();
    for (int i = 0; i < kPrepareRuns; ++i) amp.prepare(kFs, kQuantum);
    const double prepareMs =
        1000.0 * static_cast<double>(std::clock() - p0) / CLOCKS_PER_SEC / kPrepareRuns;

    const int kResetRuns = 200;
    const std::clock_t r0 = std::clock();
    for (int i = 0; i < kResetRuns; ++i) amp.reset();
    const double resetMs =
        1000.0 * static_cast<double>(std::clock() - r0) / CLOCKS_PER_SEC / kResetRuns;

    std::printf("  Jcm800Amp: prepare() %.2f ms, reset() %.4f ms (%.0fx cheaper)\n",
                prepareMs, resetMs, resetMs > 0.0 ? prepareMs / resetMs : 0.0);
    // A generous bar: the measured ratio is ~4 orders of magnitude. 50x only fails
    // if reset() has quietly grown a DC solve or a settling loop.
    assert(resetMs * 50.0 < prepareMs &&
           "reset() is no longer a cheap re-park — did it start re-solving?");
    std::printf("\n");
}
#endif  // CLIPPER_NAN_TEST_NO_RESET

}  // namespace

int main() {
    clipper::test::requireAssertsLive();
    // Unbuffered: an assert() abort must not swallow the measurement lines that
    // explain WHY it fired.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== non-finite parameter guard + recovery (audit finding 1) ===\n\n");
    testAbiRejectsNonFinite();
    testModelClampsRejectNonFinite();
#ifndef CLIPPER_NAN_TEST_NO_RESET
    testResetRecoversPoisonedUnit();
    testResetIsCheaperThanPrepare();
#else
    std::printf("(reset blocks skipped: CLIPPER_NAN_TEST_NO_RESET)\n\n");
#endif
    std::printf("all non-finite guard tests passed\n");
    return 0;
}
