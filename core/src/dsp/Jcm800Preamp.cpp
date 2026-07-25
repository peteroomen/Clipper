// Clipper — Jcm800Preamp (M9.2). See Jcm800Preamp.h for the topology, the
// composition rationale, and the tone-stack netlist. This file is the wiring:
// per-stage TriodeStage::Config, the shared direct-coupled follower bias solve,
// the passive TMB tone-stack MNA, and the block signal flow.

#include "clipper/dsp/Jcm800Preamp.h"

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/ParamGuard.h"

#include <algorithm>
#include <cmath>

namespace clipper::dsp {

// ===========================================================================
// MarshallToneStack — trapezoidal-companion nodal MNA of the FMV/TMB network.
// ===========================================================================
namespace {
// Resistance floor, written NaN-safely (the ParamGuard.h inversion: `r > 1` is
// false for NaN, so NaN lands on the floor instead of passing through). Identical
// to `r < 1 ? 1 : r` for every finite r. Load-bearing: a NaN pot fraction would
// stamp a NaN conductance into the MNA matrix, and its inverse — hence every
// sample forever after — would be NaN.
inline double clampR(double r) { return r > 1.0 ? r : 1.0; }
}  // namespace

void MarshallToneStack::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    T_ = 1.0 / sampleRate_;
    geqT_ = 2.0 * kCt / T_;
    geqB_ = 2.0 * kCb / T_;
    geqM_ = 2.0 * kCm / T_;
    reset();
    dirty_ = true;
    rebuild();
}

void MarshallToneStack::reset() {
    vT_ = iT_ = vB_ = iB_ = vM_ = iM_ = 0.0;
}

void MarshallToneStack::setSourceImpedance(double rs) {
    rs_ = clampR(rs);
    dirty_ = true;
}

void MarshallToneStack::setKnobs(double bass, double mid, double treble) {
    // NaN-rejecting (std::clamp is transparent to NaN — audit finding 1).
    auto cl = [](double v) { return clampParam(v, 1.0e-3, 1.0 - 1.0e-3); };
    bass_ = cl(bass);
    mid_ = cl(mid);
    treble_ = cl(treble);
    dirty_ = true;
}

// Build the 5x5 node conductance matrix and invert it (Gauss-Jordan). The matrix
// is constant for fixed knobs; only the RHS (source + cap history) changes per
// sample, so per-sample cost is a 5x5 matrix-vector product.
void MarshallToneStack::rebuild() {
    enum { IN = 0, N2 = 1, N3 = 2, N4 = 3, OUT = 4 };
    std::array<std::array<double, N>, N> G{};
    auto stamp = [&](int a, int b, double g) {
        G[a][a] += g;
        G[b][b] += g;
        G[a][b] -= g;
        G[b][a] -= g;
    };
    const double gs = 1.0 / rs_;
    G[IN][IN] += gs;                                   // source conductance
    stamp(IN, N2, geqT_);                              // treble cap Ct
    stamp(IN, N3, 1.0 / kRslope);                      // slope resistor
    stamp(N2, OUT, 1.0 / clampR((1.0 - treble_) * kRT));  // treble pot upper
    stamp(OUT, N3, 1.0 / clampR(treble_ * kRT));          // treble pot lower
    stamp(N3, N4, 1.0 / clampR(bass_ * kRB));          // bass pot rheostat
    stamp(N3, N4, geqB_);                              // bass cap Cb
    G[N4][N4] += 1.0 / clampR(mid_ * kRM);             // mid pot rheostat to gnd
    G[N4][N4] += geqM_;                                // mid cap Cm to gnd

    // Gauss-Jordan inversion of G into Ginv_.
    std::array<std::array<double, N>, N> A = G;
    std::array<std::array<double, N>, N> I{};
    for (int i = 0; i < N; ++i) I[i][i] = 1.0;
    for (int c = 0; c < N; ++c) {
        int p = c;
        for (int r = c + 1; r < N; ++r)
            if (std::fabs(A[r][c]) > std::fabs(A[p][c])) p = r;
        std::swap(A[p], A[c]);
        std::swap(I[p], I[c]);
        const double d = A[c][c];
        const double inv = (std::fabs(d) > 1e-300) ? 1.0 / d : 0.0;
        for (int j = 0; j < N; ++j) {
            A[c][j] *= inv;
            I[c][j] *= inv;
        }
        for (int r = 0; r < N; ++r) {
            if (r == c) continue;
            const double f = A[r][c];
            for (int j = 0; j < N; ++j) {
                A[r][j] -= f * A[c][j];
                I[r][j] -= f * I[c][j];
            }
        }
    }
    Ginv_ = I;
    dirty_ = false;
}

void MarshallToneStack::process(const float* in, float* out, int numFrames) {
    if (dirty_) rebuild();
    enum { IN = 0, N2 = 1, N3 = 2, N4 = 3, OUT = 4 };
    const double gs = 1.0 / rs_;
    for (int n = 0; n < numFrames; ++n) {
        const double Vs = static_cast<double>(in[n]);
        // Trapezoidal cap history current sources: Ieq = Geq*v_prev + i_prev.
        const double IeqT = geqT_ * vT_ + iT_;
        const double IeqB = geqB_ * vB_ + iB_;
        const double IeqM = geqM_ * vM_ + iM_;
        std::array<double, N> b{};
        b[IN] = gs * Vs + IeqT;   // source current + Ct history (IN side)
        b[N2] = -IeqT;            // Ct other side
        b[N3] = IeqB;             // Cb history (N3 side)
        b[N4] = -IeqB + IeqM;     // Cb other side + Cm (to gnd) history
        b[OUT] = 0.0;
        std::array<double, N> v{};
        for (int r = 0; r < N; ++r) {
            double acc = 0.0;
            for (int cc = 0; cc < N; ++cc) acc += Ginv_[r][cc] * b[cc];
            v[r] = acc;
        }
        // Update cap states: v_new across cap, i_new = Geq*v_new - Ieq.
        const double vTn = v[IN] - v[N2];
        const double vBn = v[N3] - v[N4];
        const double vMn = v[N4];  // N4 - GND
        // Anti-denormal (Denormal.h). All six trapezoidal cap companions REST AT ZERO,
        // so a silent tail rings them down into DOUBLE subnormals (< 2.2e-308) and
        // they stick: measured 68.2x slower on silence than with hardware FTZ before
        // these flushes — the largest denormal cliff of any component in the core.
        // Invisible in the audio (out[] is a float cast of a double node voltage, and
        // float(1e-310) is 0.0f), which is why it survived. Audit finding 11, §33.
        iT_ = flushDenormal(geqT_ * vTn - IeqT);
        iB_ = flushDenormal(geqB_ * vBn - IeqB);
        iM_ = flushDenormal(geqM_ * vMn - IeqM);
        vT_ = flushDenormal(vTn);
        vB_ = flushDenormal(vBn);
        vM_ = flushDenormal(vMn);
        out[n] = static_cast<float>(v[OUT]);
    }
}

// ===========================================================================
// Jcm800Preamp
// ===========================================================================

double Jcm800Preamp::audioTaper(double x) {
    x = clampParam01(x);  // NaN-rejecting (ParamGuard.h)
    constexpr double k = 4.0;  // ~12% at noon (a musical audio/log taper)
    return (std::exp(k * x) - 1.0) / (std::exp(k) - 1.0);
}

Jcm800Preamp::Jcm800Preamp() { configureStages(); }

// Per-stage circuit values (documented canonical JCM800 2204 preamp voicing).
void Jcm800Preamp::configureStages() {
    // V1A: bright common-cathode input stage. Rgl = the GAIN network load seen by
    // the plate (470k series + 1M pot ~= 1.47M).
    TriodeStage::Config& a = cfg_[V1A];
    a = TriodeStage::Config{};
    a.Ra = 100.0e3; a.Rk = 820.0; a.Ck = 0.68e-6; a.Rgl = 1.47e6;

    // V1B: the COLD second stage — 10k UNBYPASSED cathode (Ck = 0), 82k plate.
    // Rgl = V2A's 470k grid leak.
    TriodeStage::Config& b = cfg_[V1B];
    b = TriodeStage::Config{};
    b.Ra = 82.0e3; b.Rk = 10.0e3; b.Ck = 0.0; b.Rgl = 470.0e3;

    // V2A: bright common-cathode stage driving the follower. Its grid leak Rgl =
    // 1 M models its own input grid-leak (blocking-recovery tau = Rgl*Cc = 22 ms);
    // the direct-coupled follower grid is high-Z, so 1 M barely loads the plate
    // (Ra||1M||rp ~ Ra||rp, <2% gain change) — a faithful compromise for the one
    // shared Rgl (input grid-leak vs output plate load).
    TriodeStage::Config& c = cfg_[V2A];
    c = TriodeStage::Config{};
    c.Ra = 100.0e3; c.Rk = 820.0; c.Ck = 0.68e-6; c.Rgl = 1.0e6;

    // V2B: direct-coupled cathode follower (gridBias set at prepare from V2A).
    TriodeStage::Config& d = cfg_[V2B];
    d = TriodeStage::Config{};
    d.topology = TriodeStage::Topology::CathodeFollower;
    d.Ra = 0.0; d.Rk = 100.0e3; d.Ck = 0.0; d.gridBias = 0.0;  // gridBias @prepare
}

void Jcm800Preamp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    buf_.assign(static_cast<size_t>(maxBlockSize_), 0.0f);

    configureStages();
    // Prepare V1A, V1B, V2A first (their op points don't depend on the follower).
    for (int s = V1A; s <= V2A; ++s) {
        stage_[s].configure(cfg_[s]);
        stage_[s].prepare(sampleRate_, maxBlockSize_);
        stage_[s].setOversampling(oversampling_);
    }
    // Direct-coupled follower: its grid DC bias is V2A's quiescent plate voltage.
    followerGridBias_ = stage_[V2A].quiescentPlateVoltage();
    cfg_[V2B].gridBias = followerGridBias_;
    stage_[V2B].configure(cfg_[V2B]);
    stage_[V2B].prepare(sampleRate_, maxBlockSize_);
    stage_[V2B].setOversampling(oversampling_);

    // Follower output impedance ~ 1/gm || Rk, from the small-signal gm at its op
    // point (central difference of the Koren law). Drives the tone stack.
    {
        const double Vk = stage_[V2B].quiescentCathodeVoltage();
        const double Vgk = followerGridBias_ - Vk;
        const double Va = cfg_[V2B].bPlus - Vk;
        const double h = 1e-4;
        const double gm = (TriodeStage::korenPlateCurrent(Va, Vgk + h, cfg_[V2B].tube) -
                           TriodeStage::korenPlateCurrent(Va, Vgk - h, cfg_[V2B].tube)) /
                          (2.0 * h);
        followerRout_ = 1.0 / (gm + 1.0 / cfg_[V2B].Rk);
    }

    tone_.prepare(sampleRate_);
    tone_.setSourceImpedance(followerRout_);
    tone_.setKnobs(bass_, mid_, treble_);
}

void Jcm800Preamp::reset() {
    for (auto& s : stage_) s.reset();
    tone_.reset();
    lastV1bGridPeak_ = 0.0;
    lastOutPeak_ = 0.0;
}

void Jcm800Preamp::setOversampling(int factor) {
    oversampling_ = factor;
    for (auto& s : stage_) s.setOversampling(factor);
}

void Jcm800Preamp::setParameter(int paramId, float value) {
    // NaN-rejecting: std::clamp(NaN, 0, 1) returns NaN, and one NaN knob poisoned
    // the tone-stack MNA inverse permanently (audit finding 1). See ParamGuard.h.
    const double v = clampParam01(static_cast<double>(value));
    switch (paramId) {
        case PARAM_GAIN: gain_ = v; break;
        case PARAM_MASTER: master_ = v; break;
        case PARAM_BASS: bass_ = v; tone_.setKnobs(bass_, mid_, treble_); break;
        case PARAM_MID: mid_ = v; tone_.setKnobs(bass_, mid_, treble_); break;
        case PARAM_TREBLE: treble_ = v; tone_.setKnobs(bass_, mid_, treble_); break;
        default: break;
    }
}

double Jcm800Preamp::gainInterstageScale() const {
    return kGainDivider * audioTaper(gain_);
}
double Jcm800Preamp::masterScale() const { return audioTaper(master_); }

void Jcm800Preamp::process(const float* in, float* out, int numFrames) {
    const double gA = gainInterstageScale();
    const double mScale = masterScale();
    lastV1bGridPeak_ = 0.0;
    lastOutPeak_ = 0.0;
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        float* w = buf_.data();
        // V1A
        stage_[V1A].process(in + off, w, n);
        // GAIN network: V1A plate AC * (series divider * audio-taper wiper).
        for (int i = 0; i < n; ++i) {
            w[i] = static_cast<float>(w[i] * gA);
            lastV1bGridPeak_ = std::max(lastV1bGridPeak_,
                                        std::fabs(static_cast<double>(w[i])));
        }
        // V1B (cold), interstage B is ~unity (grid-leak coupling).
        stage_[V1B].process(w, w, n);
        // V2A
        stage_[V2A].process(w, w, n);
        // V2B direct-coupled follower (AC rides on the DC gridBias internally).
        stage_[V2B].process(w, w, n);
        // Passive TMB tone stack (base rate).
        tone_.process(w, w, n);
        // MASTER volume.
        for (int i = 0; i < n; ++i) {
            const float o = static_cast<float>(w[i] * mScale);
            out[off + i] = o;
            lastOutPeak_ = std::max(lastOutPeak_, std::fabs(static_cast<double>(o)));
        }
        off += n;
    }
}

double Jcm800Preamp::stageQuiescentPlate(int stage) const {
    return stage_[static_cast<size_t>(stage)].quiescentPlateVoltage();
}
double Jcm800Preamp::stageQuiescentCathode(int stage) const {
    return stage_[static_cast<size_t>(stage)].quiescentCathodeVoltage();
}
double Jcm800Preamp::stageQuiescentCurrent(int stage) const {
    return stage_[static_cast<size_t>(stage)].quiescentCurrent();
}

}  // namespace clipper::dsp
