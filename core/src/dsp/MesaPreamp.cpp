// Clipper — MesaPreamp (M10.4, docs §68). See MesaPreamp.h for the topology, the
// PROVENANCE banner (the owner-supplied `mbdr` sheet set) and the one modelling
// approximation the cathode-bypass idiom forces. This file is the numerics: the
// input-select MNA, the two interstage networks, the FMV tone-stack MNA, the
// transcribed TriodeStage configs, the supply ladder and the block signal flow.

#include "clipper/dsp/MesaPreamp.h"

#include "clipper/dsp/ParamGuard.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace clipper::dsp {

namespace {
// NaN-safe resistance floor — identical to MarshallToneStack's clampR and
// load-bearing for the same reason: a NaN pot fraction would stamp NaN into the
// matrix and its inverse, poisoning every later sample.
inline double clampR(double r) { return r > 1.0 ? r : 1.0; }

constexpr double kPi = 3.14159265358979323846;

// A node with no DC path to ground would make the matrix singular. Two of this
// preamp's nodes are between series capacitors (RED couples through 82p into a
// 1n), so they get the house 1 G leak — the same stamped-open trick
// RockerverbInterstage and FacNetwork use. 1 G against the 2M2 load is
// -0.0000191 dB, i.e. below every measurement in the suite.
constexpr double kOpenOhms = 1.0e9;

// Gauss-Jordan inverse of a small dense real matrix with partial pivoting.
template <int N>
void invertInPlace(std::array<std::array<double, N>, N>& M,
                   std::array<std::array<double, N>, N>& out) {
    std::array<std::array<double, N>, N> Amat = M;
    std::array<std::array<double, N>, N> I{};
    for (int i = 0; i < N; ++i) I[i][i] = 1.0;
    for (int c = 0; c < N; ++c) {
        int p = c;
        for (int r = c + 1; r < N; ++r)
            if (std::fabs(Amat[r][c]) > std::fabs(Amat[p][c])) p = r;
        std::swap(Amat[p], Amat[c]);
        std::swap(I[p], I[c]);
        const double d = Amat[c][c];
        const double inv = (std::fabs(d) > 1e-300) ? 1.0 / d : 0.0;
        for (int j = 0; j < N; ++j) {
            Amat[c][j] *= inv;
            I[c][j] *= inv;
        }
        for (int r = 0; r < N; ++r) {
            if (r == c) continue;
            const double f = Amat[r][c];
            for (int j = 0; j < N; ++j) {
                Amat[r][j] -= f * Amat[c][j];
                I[r][j] -= f * I[c][j];
            }
        }
    }
    out = I;
}

// Solve a complex nodal system by Gauss-Jordan; returns the requested node.
template <int N>
std::complex<double> solveComplexSys(std::complex<double> M[N][N + 1], int node) {
    using C = std::complex<double>;
    for (int c = 0; c < N; ++c) {
        int piv = c;
        for (int r = c + 1; r < N; ++r)
            if (std::abs(M[r][c]) > std::abs(M[piv][c])) piv = r;
        for (int j = 0; j <= N; ++j) std::swap(M[piv][j], M[c][j]);
        const C d = M[c][c];
        if (std::abs(d) < 1e-300) return C(0.0, 0.0);
        for (int j = c; j <= N; ++j) M[c][j] /= d;
        for (int r = 0; r < N; ++r) {
            if (r == c) continue;
            const C f = M[r][c];
            if (std::abs(f) == 0.0) continue;
            for (int j = c; j <= N; ++j) M[r][j] -= f * M[c][j];
        }
    }
    return M[node][N];
}

// The DC operating point of a common-cathode triode with plate load Ra and
// self-bias Rk, on the Koren law alone. BISECTION, for the reason docs §57.2
// gives: g(ip) is strictly decreasing in ip, so bisection is unconditionally
// stable where the obvious damped fixed point oscillates.
double korenSelfBiasCurrent(double bPlus, double Ra, double Rk,
                            const TriodeStage::KorenParams& tube) {
    auto g = [&](double ip) {
        const double vk = ip * Rk;
        const double va = bPlus - ip * Ra - vk;
        return TriodeStage::korenPlateCurrent(va > 0.0 ? va : 0.0, -vk, tube) - ip;
    };
    double lo = 0.0, hi = 20.0e-3;
    for (int it = 0; it < 200; ++it) {
        const double m = 0.5 * (lo + hi);
        if (g(m) > 0.0)
            lo = m;
        else
            hi = m;
    }
    return 0.5 * (lo + hi);
}

// The same, for a DIRECT-COUPLED cathode follower (V3B): plate tied to B+, Rk to
// ground, grid held at gridBias by the previous plate. Raising ip raises Vk,
// which lowers BOTH Va and Vgk, so the residual is monotone decreasing and
// bisection is again exact.
double korenFollowerCurrent(double bPlus, double Rk, double gridBias,
                            const TriodeStage::KorenParams& tube) {
    auto g = [&](double ip) {
        const double vk = ip * Rk;
        const double va = bPlus - vk;
        return TriodeStage::korenPlateCurrent(va > 0.0 ? va : 0.0, gridBias - vk, tube) -
               ip;
    };
    double lo = 0.0, hi = 20.0e-3;
    for (int it = 0; it < 200; ++it) {
        const double m = 0.5 * (lo + hi);
        if (g(m) > 0.0)
            lo = m;
        else
            hi = m;
    }
    return 0.5 * (lo + hi);
}
}  // namespace

// ===========================================================================
// MesaInputNetwork — the V1A output pad, the RED/ORANGE input select and the
// gain pot, as ONE matrix. The pot is inside it deliberately: it is the
// network's load, so its position changes the divider AND the select element's
// corner together.
// ===========================================================================
namespace {
enum { IPL = 0, IX1 = 1, ISS = 2, IPP = 3, IWW = 4 };
}

void MesaInputNetwork::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    T_ = 1.0 / sampleRate_;
    geqC_ = 2.0 * kCcoup / T_;
    geqP_ = 2.0 * kCpad / T_;
    geqS_ = 2.0 * kCsel / T_;
    geqG_ = 2.0 * kCpot / T_;
    gainSm_.prepare(kKnobSmoothSeconds, sampleRate_);
    reset();
}

void MesaInputNetwork::reset() {
    vC_ = iC_ = vP_ = iP_ = vS_ = iS_ = vG_ = iG_ = 0.0;
    snapKnobs();
}

void MesaInputNetwork::snapKnobs() {
    gainSm_.reset();
    gain_ = gainSm_.value();
    knobsMoving_ = false;
    ctrlCounter_ = 0;
    rebuild();
}

void MesaInputNetwork::setSourceImpedance(double rs) {
    rs_ = clampR(rs);
    dirty_ = true;
}

void MesaInputNetwork::setMode(MesaMode m) {
    if (m == mode_) return;
    mode_ = m;
    dirty_ = true;
}

void MesaInputNetwork::setWiper(double g) {
    gainSm_.setTarget(clampParam(g, 1.0e-3, 1.0 - 1.0e-3));
    ctrlCounter_ = 0;
    if (!gainSm_.settled()) knobsMoving_ = true;
}

void MesaInputNetwork::rebuild() {
    const MesaModeRow& row = mesaModeRow(mode_);
    const bool red = mesaModeIsRed(mode_);
    std::array<std::array<double, N>, N> Gm{};
    auto stamp = [&](int a, int b, double g) {
        Gm[a][a] += g;
        Gm[b][b] += g;
        Gm[a][b] -= g;
        Gm[b][a] -= g;
    };
    Gm[IPL][IPL] += 1.0 / rs_;             // driven from V1A's plate
    stamp(IPL, IX1, geqC_);                // 20n plate coupling
    Gm[IX1][IX1] += 1.0 / kRload;          // 2M2 to ground
    if (row.ldr4_v1aOutputPad) {           // LDR4: the 680k || 2n pad leg
        Gm[IX1][IX1] += 1.0 / kRpad;
        Gm[IX1][IX1] += geqP_;
    }
    // THE INPUT SELECT. RED couples through a CAPACITOR (82p), ORANGE through a
    // RESISTOR (2M2) — different kinds of component, not different values. See
    // the header; this is the structural half of "the red channel is tighter".
    if (red)
        stamp(IX1, ISS, geqS_);
    else
        stamp(IX1, ISS, 1.0 / kRsel);
    Gm[ISS][ISS] += 1.0 / kOpenOhms;       // RED's SS sits between two caps
    stamp(ISS, IPP, geqG_);                // 1n into the gain pot
    stamp(IPP, IWW, 1.0 / clampR((1.0 - gain_) * kRpot));
    Gm[IWW][IWW] += 1.0 / clampR(gain_ * kRpot);
    invertInPlace<N>(Gm, Ginv_);
    dirty_ = false;
}

void MesaInputNetwork::process(const float* in, float* out, int numFrames) {
    if (dirty_) rebuild();
    const MesaModeRow& row = mesaModeRow(mode_);
    const bool red = mesaModeIsRed(mode_);
    const bool hasPad = row.ldr4_v1aOutputPad;
    const double gs = 1.0 / rs_;
    for (int n = 0; n < numFrames; ++n) {
        if (knobsMoving_) {
            const double g = gainSm_.next();
            if (ctrlCounter_ == 0) {
                if (g != gain_) {
                    gain_ = g;
                    rebuild();
                }
                if (gainSm_.settled()) knobsMoving_ = false;
            }
            if (++ctrlCounter_ >= kCtrlBlock) ctrlCounter_ = 0;
        }
        const double IeqC = geqC_ * vC_ + iC_;
        const double IeqP = hasPad ? (geqP_ * vP_ + iP_) : 0.0;
        const double IeqS = red ? (geqS_ * vS_ + iS_) : 0.0;
        const double IeqG = geqG_ * vG_ + iG_;
        std::array<double, N> b{};
        b[IPL] = gs * static_cast<double>(in[n]) + IeqC;
        b[IX1] = -IeqC + IeqP + IeqS;
        b[ISS] = -IeqS + IeqG;
        b[IPP] = -IeqG;
        b[IWW] = 0.0;
        std::array<double, N> v{};
        for (int r = 0; r < N; ++r) {
            double acc = 0.0;
            for (int cc = 0; cc < N; ++cc) acc += Ginv_[r][cc] * b[cc];
            v[r] = acc;
        }
        // ADR 006 / docs §33: every companion here rests at exactly zero.
        {
            const double vn = v[IPL] - v[IX1];
            iC_ = flushDenormal(geqC_ * vn - IeqC);
            vC_ = flushDenormal(vn);
        }
        if (hasPad) {
            const double vn = v[IX1];
            iP_ = flushDenormal(geqP_ * vn - IeqP);
            vP_ = flushDenormal(vn);
        }
        if (red) {
            const double vn = v[IX1] - v[ISS];
            iS_ = flushDenormal(geqS_ * vn - IeqS);
            vS_ = flushDenormal(vn);
        }
        {
            const double vn = v[ISS] - v[IPP];
            iG_ = flushDenormal(geqG_ * vn - IeqG);
            vG_ = flushDenormal(vn);
        }
        out[n] = static_cast<float>(v[IWW]);
    }
}

double MesaInputNetwork::magnitudeAt(double freqHz, MesaMode m, double g, double rs) {
    using Cx = std::complex<double>;
    const MesaModeRow& row = mesaModeRow(m);
    const bool red = mesaModeIsRed(m);
    const Cx jw(0.0, 2.0 * kPi * freqHz);
    Cx M[N][N + 1];
    for (auto& r0 : M)
        for (auto& e : r0) e = Cx(0.0, 0.0);
    auto stamp = [&](int a, int b, Cx gg) {
        M[a][a] += gg;
        M[b][b] += gg;
        M[a][b] -= gg;
        M[b][a] -= gg;
    };
    const double rsc = rs > 1.0 ? rs : 1.0;
    M[IPL][IPL] += Cx(1.0 / rsc, 0.0);
    M[IPL][N] += Cx(1.0 / rsc, 0.0);  // unit drive behind rs
    stamp(IPL, IX1, jw * kCcoup);
    M[IX1][IX1] += Cx(1.0 / kRload, 0.0);
    if (row.ldr4_v1aOutputPad) {
        M[IX1][IX1] += Cx(1.0 / kRpad, 0.0);
        M[IX1][IX1] += jw * kCpad;
    }
    if (red)
        stamp(IX1, ISS, jw * kCsel);
    else
        stamp(IX1, ISS, Cx(1.0 / kRsel, 0.0));
    M[ISS][ISS] += Cx(1.0 / kOpenOhms, 0.0);
    stamp(ISS, IPP, jw * kCpot);
    const double gg = g < 1.0e-3 ? 1.0e-3 : (g > 1.0 - 1.0e-3 ? 1.0 - 1.0e-3 : g);
    stamp(IPP, IWW, Cx(1.0 / clampR((1.0 - gg) * kRpot), 0.0));
    M[IWW][IWW] += Cx(1.0 / clampR(gg * kRpot), 0.0);
    return std::abs(solveComplexSys<N>(M, IWW));
}

// ===========================================================================
// MesaInterstage — PL --Ccoup-- NJ --Rser-- G --Rleak-- GND
// ===========================================================================
namespace {
enum { SPL = 0, SNJ = 1, SGG = 2 };
}

void MesaInterstage::configure(const Config& c) {
    cfg_ = c;
    dirty_ = true;
}

void MesaInterstage::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    T_ = 1.0 / sampleRate_;
    geqC_ = 2.0 * cfg_.Ccoup / T_;
    reset();
}

void MesaInterstage::reset() {
    vC_ = iC_ = 0.0;
    rebuild();
}

void MesaInterstage::setSourceImpedance(double rs) {
    rs_ = clampR(rs);
    dirty_ = true;
}

void MesaInterstage::rebuild() {
    std::array<std::array<double, N>, N> Gm{};
    auto stamp = [&](int a, int b, double g) {
        Gm[a][a] += g;
        Gm[b][b] += g;
        Gm[a][b] -= g;
        Gm[b][a] -= g;
    };
    Gm[SPL][SPL] += 1.0 / rs_;
    stamp(SPL, SNJ, geqC_);
    stamp(SNJ, SGG, 1.0 / clampR(cfg_.Rser));
    Gm[SGG][SGG] += 1.0 / clampR(cfg_.Rleak);
    invertInPlace<N>(Gm, Ginv_);
    dirty_ = false;
}

void MesaInterstage::process(const float* in, float* out, int numFrames) {
    if (dirty_) rebuild();
    const double gs = 1.0 / rs_;
    for (int n = 0; n < numFrames; ++n) {
        const double IeqC = geqC_ * vC_ + iC_;
        std::array<double, N> b{};
        b[SPL] = gs * static_cast<double>(in[n]) + IeqC;
        b[SNJ] = -IeqC;
        b[SGG] = 0.0;
        std::array<double, N> v{};
        for (int r = 0; r < N; ++r) {
            double acc = 0.0;
            for (int cc = 0; cc < N; ++cc) acc += Ginv_[r][cc] * b[cc];
            v[r] = acc;
        }
        const double vn = v[SPL] - v[SNJ];
        iC_ = flushDenormal(geqC_ * vn - IeqC);
        vC_ = flushDenormal(vn);
        out[n] = static_cast<float>(v[SGG]);
    }
}

double MesaInterstage::magnitudeAt(double freqHz, const Config& c, double rs) {
    using Cx = std::complex<double>;
    const Cx jw(0.0, 2.0 * kPi * freqHz);
    Cx M[N][N + 1];
    for (auto& r0 : M)
        for (auto& e : r0) e = Cx(0.0, 0.0);
    auto stamp = [&](int a, int b, Cx gg) {
        M[a][a] += gg;
        M[b][b] += gg;
        M[a][b] -= gg;
        M[b][a] -= gg;
    };
    const double rsc = rs > 1.0 ? rs : 1.0;
    M[SPL][SPL] += Cx(1.0 / rsc, 0.0);
    M[SPL][N] += Cx(1.0 / rsc, 0.0);
    stamp(SPL, SNJ, jw * c.Ccoup);
    stamp(SNJ, SGG, Cx(1.0 / clampR(c.Rser), 0.0));
    M[SGG][SGG] += Cx(1.0 / clampR(c.Rleak), 0.0);
    return std::abs(solveComplexSys<N>(M, SGG));
}

// ===========================================================================
// MesaToneStack — the transcribed FMV, with the MASTER pot and the next grid's
// leak inside the same matrix.
// ===========================================================================
namespace {
enum { TIN = 0, TTT = 1, TAA = 2, TBB = 3, TCC = 4, TMW = 5, TOUT = 6, TVW = 7 };
}

void MesaToneStack::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    T_ = 1.0 / sampleRate_;
    geqT_ = 2.0 * trebleCapFor(mode_) / T_;
    geqB_ = 2.0 * kCb / T_;
    geqM_ = 2.0 * kCm / T_;
    bassSm_.prepare(kKnobSmoothSeconds, sampleRate_);
    midSm_.prepare(kKnobSmoothSeconds, sampleRate_);
    trebleSm_.prepare(kKnobSmoothSeconds, sampleRate_);
    masterSm_.prepare(kKnobSmoothSeconds, sampleRate_);
    reset();
}

void MesaToneStack::reset() {
    vT_ = iT_ = vB_ = iB_ = vM_ = iM_ = 0.0;
    snapKnobs();
}

void MesaToneStack::snapKnobs() {
    bassSm_.reset();
    midSm_.reset();
    trebleSm_.reset();
    masterSm_.reset();
    bass_ = bassSm_.value();
    mid_ = midSm_.value();
    treble_ = trebleSm_.value();
    master_ = masterSm_.value();
    knobsMoving_ = false;
    ctrlCounter_ = 0;
    rebuild();
}

void MesaToneStack::setSourceImpedance(double rs) {
    rs_ = clampR(rs);
    dirty_ = true;
}

void MesaToneStack::setMode(MesaMode m) {
    if (m == mode_) return;
    // The ONLY component that changes between the two channels' stacks.
    mode_ = m;
    geqT_ = 2.0 * trebleCapFor(mode_) / T_;
    dirty_ = true;
}

void MesaToneStack::setKnobs(double bass, double mid, double treble, double master) {
    bassSm_.setTarget(clampParam01(bass));
    midSm_.setTarget(clampParam01(mid));
    trebleSm_.setTarget(clampParam01(treble));
    masterSm_.setTarget(clampParam01(master));
    ctrlCounter_ = 0;
    if (!bassSm_.settled() || !midSm_.settled() || !trebleSm_.settled() ||
        !masterSm_.settled())
        knobsMoving_ = true;
}

void MesaToneStack::rebuild() {
    std::array<std::array<double, N>, N> Gm{};
    auto stamp = [&](int a, int b, double g) {
        Gm[a][a] += g;
        Gm[b][b] += g;
        Gm[a][b] -= g;
        Gm[b][a] -= g;
    };
    Gm[TIN][TIN] += 1.0 / rs_;                            // V3B's CATHODE (low Z)
    stamp(TIN, TTT, geqT_);                               // 680p / 500p treble cap
    stamp(TIN, TAA, 1.0 / kRslope);                       // 47k slope
    stamp(TAA, TBB, geqB_);                               // 20n, IN SERIES
    stamp(TAA, TMW, geqM_);                               // 20n to the MID WIPER
    stamp(TTT, TOUT, 1.0 / clampR((1.0 - treble_) * kRT));
    stamp(TOUT, TBB, 1.0 / clampR(treble_ * kRT));
    stamp(TBB, TCC, 1.0 / clampR(bass_ * kRB));           // bass rheostat
    stamp(TCC, TMW, 1.0 / clampR((1.0 - mid_) * kRM));
    Gm[TMW][TMW] += 1.0 / clampR(mid_ * kRM);
    stamp(TOUT, TVW, 1.0 / clampR((1.0 - master_) * kRV));
    Gm[TVW][TVW] += 1.0 / clampR(master_ * kRV);
    Gm[TVW][TVW] += 1.0 / kRnext;                         // the next grid's leak
    invertInPlace<N>(Gm, Ginv_);
    dirty_ = false;
}

void MesaToneStack::process(const float* in, float* out, int numFrames) {
    if (dirty_) rebuild();
    const double gs = 1.0 / rs_;
    for (int n = 0; n < numFrames; ++n) {
        if (knobsMoving_) {
            const double b = bassSm_.next();
            const double m = midSm_.next();
            const double t = trebleSm_.next();
            const double v = masterSm_.next();
            if (ctrlCounter_ == 0) {
                if (b != bass_ || m != mid_ || t != treble_ || v != master_) {
                    bass_ = b;
                    mid_ = m;
                    treble_ = t;
                    master_ = v;
                    rebuild();
                }
                if (bassSm_.settled() && midSm_.settled() && trebleSm_.settled() &&
                    masterSm_.settled())
                    knobsMoving_ = false;
            }
            if (++ctrlCounter_ >= kCtrlBlock) ctrlCounter_ = 0;
        }
        const double IeqT = geqT_ * vT_ + iT_;
        const double IeqB = geqB_ * vB_ + iB_;
        const double IeqM = geqM_ * vM_ + iM_;
        std::array<double, N> b{};
        b[TIN] = gs * static_cast<double>(in[n]) + IeqT;
        b[TTT] = -IeqT;
        b[TAA] = IeqB + IeqM;
        b[TBB] = -IeqB;
        b[TMW] = -IeqM;
        std::array<double, N> v{};
        for (int r = 0; r < N; ++r) {
            double acc = 0.0;
            for (int cc = 0; cc < N; ++cc) acc += Ginv_[r][cc] * b[cc];
            v[r] = acc;
        }
        {
            const double vn = v[TIN] - v[TTT];
            iT_ = flushDenormal(geqT_ * vn - IeqT);
            vT_ = flushDenormal(vn);
        }
        {
            const double vn = v[TAA] - v[TBB];
            iB_ = flushDenormal(geqB_ * vn - IeqB);
            vB_ = flushDenormal(vn);
        }
        {
            const double vn = v[TAA] - v[TMW];
            iM_ = flushDenormal(geqM_ * vn - IeqM);
            vM_ = flushDenormal(vn);
        }
        out[n] = static_cast<float>(v[TVW]);
    }
}

double MesaToneStack::magnitudeAt(double freqHz, MesaMode m, double bass, double mid,
                                  double treble, double master, double rs,
                                  Probe probe) {
    using Cx = std::complex<double>;
    const Cx jw(0.0, 2.0 * kPi * freqHz);
    Cx M[N][N + 1];
    for (auto& r0 : M)
        for (auto& e : r0) e = Cx(0.0, 0.0);
    auto stamp = [&](int a, int b, Cx gg) {
        M[a][a] += gg;
        M[b][b] += gg;
        M[a][b] -= gg;
        M[b][a] -= gg;
    };
    const double rsc = rs > 1.0 ? rs : 1.0;
    M[TIN][TIN] += Cx(1.0 / rsc, 0.0);
    M[TIN][N] += Cx(1.0 / rsc, 0.0);
    stamp(TIN, TTT, jw * trebleCapFor(m));
    stamp(TIN, TAA, Cx(1.0 / kRslope, 0.0));
    stamp(TAA, TBB, jw * kCb);
    stamp(TAA, TMW, jw * kCm);
    stamp(TTT, TOUT, Cx(1.0 / clampR((1.0 - treble) * kRT), 0.0));
    stamp(TOUT, TBB, Cx(1.0 / clampR(treble * kRT), 0.0));
    stamp(TBB, TCC, Cx(1.0 / clampR(bass * kRB), 0.0));
    stamp(TCC, TMW, Cx(1.0 / clampR((1.0 - mid) * kRM), 0.0));
    M[TMW][TMW] += Cx(1.0 / clampR(mid * kRM), 0.0);
    stamp(TOUT, TVW, Cx(1.0 / clampR((1.0 - master) * kRV), 0.0));
    M[TVW][TVW] += Cx(1.0 / clampR(master * kRV), 0.0);
    M[TVW][TVW] += Cx(1.0 / kRnext, 0.0);
    return std::abs(solveComplexSys<N>(M, probe == Probe::Out ? TOUT : TVW));
}

// ===========================================================================
// MesaPreamp
// ===========================================================================

double MesaPreamp::logTaper(double x) {
    const double t = clampParam01(x);
    const double k = kLogTaperK;
    return (std::exp(k * t) - 1.0) / (std::exp(k) - 1.0);
}

double MesaPreamp::gainWiper() const { return logTaper(gain_); }

MesaPreamp::MesaPreamp() { configureStages(); }

int MesaPreamp::latencySamples() const {
    int n = 0;
    for (int s = 0; s < STAGE_COUNT; ++s)
        n += stage_[static_cast<size_t>(s)].latencySamples();
    return n;
}

// The transcribed dropper ladder (sheet `mbdr4`). The choke is ideal at DC, so
// A and B are one node (the §57 precedent for the OR120's HT choke). Each rail
// carries the stages that the PREAMP sheet's node letters say it does:
//   E -> V1A ; D -> V2A, V2B ; C -> V3A, V3B
void MesaPreamp::solveSupply() {
    const TriodeStage::KorenParams tube{};
    // Two passes: currents depend on rails, rails on currents. The ladder is
    // stiff (2k7 / 15k / 22k against ~100k plate loads) so this converges hard;
    // ten passes is far past the residual floor and costs nothing in prepare().
    railC_ = 422.0;
    railD_ = 406.0;
    railE_ = 402.0;
    for (int it = 0; it < 10; ++it) {
        const double iV1A = korenSelfBiasCurrent(railE_, 220.0e3, 1.8e3, tube);
        const double iV2A = korenSelfBiasCurrent(railD_, 100.0e3, 1.8e3, tube);
        const double iV2B = korenSelfBiasCurrent(railD_, 100.0e3, 39.0e3, tube);
        const double iV3A = korenSelfBiasCurrent(railC_, 220.0e3, 1.8e3, tube);
        // V3B is a DIRECT-COUPLED cathode follower off V3A's plate, and the
        // sheet's own marked voltages corroborate that: V3A's plate is 213 V and
        // V3B's cathode is 216 V, i.e. Vgk = -3 V. It draws from the same C rail.
        const double vg3b = railC_ - iV3A * 220.0e3;
        const double iV3B = korenFollowerCurrent(railC_, 100.0e3, vg3b, tube);
        const double iE = iV1A;
        const double iD = iV2A + iV2B + iE;
        const double iC = iV3A + iV3B + iD + kIdownstreamC;
        railC_ = kVrailA - iC * kRbc;
        railD_ = railC_ - iD * kRcd;
        railE_ = railD_ - iE * kRde;
    }
}

// TRANSCRIBED stage configs (sheet `mbdr1`). Every real interstage coupling cap
// lives ONCE, in a MesaInterstage / MesaInputNetwork, for the reason §63.3 gives
// — so each TriodeStage carries a deliberately transparent 1uF coupling whose
// Rgl is the network's real resistive input (which is what loads the plate).
void MesaPreamp::configureStages() {
    const MesaModeRow& row = mesaModeRow(mode_);
    // The switched cathode bypass: LDR ON shorts the 47k, giving the full 1u.
    // LDR OFF leaves 1u + 47k, which is under 0.4 dB of bypass — shipped as
    // unbypassed. See the header; do NOT fold the 47k into Rk (it moves the bias
    // the sheet's marked 1.6 V pins).
    const double ck = row.ldr3_cathodeCaps ? 1.0e-6 : 0.0;
    const double ck3 = row.ldr10_v3CathodeCap ? 1.0e-6 : 0.0;

    // V1A — Ra 220k from E, Rk 1k8, marked Vp 200 V / Vk 1.6 V
    cfg_[V1A] = TriodeStage::Config{};
    cfg_[V1A].bPlus = railE_;
    cfg_[V1A].Ra = 220.0e3;
    cfg_[V1A].Rk = 1.8e3;
    cfg_[V1A].Ck = ck;
    cfg_[V1A].Rg = 68.0e3;
    cfg_[V1A].Cc = 1.0e-6;
    cfg_[V1A].Rgl = MesaInputNetwork::kRload;

    // V2A — Ra 100k from D, Rk 1k8, marked Vp 280 V / Vk 2 V
    cfg_[V2A] = TriodeStage::Config{};
    cfg_[V2A].bPlus = railD_;
    cfg_[V2A].Ra = 100.0e3;
    cfg_[V2A].Rk = 1.8e3;
    cfg_[V2A].Ck = ck;
    cfg_[V2A].Rg = 470.0e3;  // the transcribed series grid resistor
    cfg_[V2A].Cc = 1.0e-6;
    cfg_[V2A].Rgl = 470.0e3 + 1.0e6;

    // V2B — Ra 100k from D, Rk 39k UNBYPASSED, marked Vp 384 V / Vk 5 V.
    // THE COLD STAGE, and it is not switched: 39k of undegenerated cathode
    // resistance is ~22x V1A's, so this stage has the headroom the cascade needs
    // to stay articulate at the gain the two stages before it deliver.
    cfg_[V2B] = TriodeStage::Config{};
    cfg_[V2B].bPlus = railD_;
    cfg_[V2B].Ra = 100.0e3;
    cfg_[V2B].Rk = 39.0e3;
    cfg_[V2B].Ck = 0.0;
    cfg_[V2B].Rg = 220.0e3;
    cfg_[V2B].Cc = 1.0e-6;
    cfg_[V2B].Rgl = 220.0e3 + 330.0e3;

    // V3A — Ra 220k from C, Rk 1k8, marked Vp 213 V / Vk 1.6 V
    cfg_[V3A] = TriodeStage::Config{};
    cfg_[V3A].bPlus = railC_;
    cfg_[V3A].Ra = 220.0e3;
    cfg_[V3A].Rk = 1.8e3;
    cfg_[V3A].Ck = ck3;
    cfg_[V3A].Rg = 68.0e3;
    cfg_[V3A].Cc = 1.0e-6;
    cfg_[V3A].Rgl = 1.0e6;

    // V3B — the CATHODE FOLLOWER driving both tone stacks. Plate to C, 100k
    // cathode load, marked Vp 415 V / Vk 216 V. This is why this amp's tone
    // controls interact less than either Orange's: the stack is driven from a
    // few hundred ohms rather than off a plate.
    cfg_[V3B] = TriodeStage::Config{};
    cfg_[V3B].topology = TriodeStage::Topology::CathodeFollower;
    cfg_[V3B].bPlus = railC_;
    cfg_[V3B].Ra = 0.0;
    cfg_[V3B].Rk = 100.0e3;
    cfg_[V3B].Ck = 0.0;
    cfg_[V3B].Rg = 68.0e3;
    cfg_[V3B].Cc = 1.0e-6;
    cfg_[V3B].Rgl = 1.0e6;
    cfg_[V3B].gridBias = followerGridBias_;
}

void MesaPreamp::applyMode() {
    input_.setMode(mode_);
    tone_.setMode(mode_);
    configureStages();
    for (int s = 0; s < STAGE_COUNT; ++s)
        stage_[static_cast<size_t>(s)].configure(cfg_[s]);
}

void MesaPreamp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    solveSupply();
    configureStages();

    static const MesaInterstage::Config kNet0{20.0e-9, 470.0e3, 1.0e6};
    static const MesaInterstage::Config kNet1{20.0e-9, 220.0e3, 330.0e3};
    net_[0].configure(kNet0);
    net_[1].configure(kNet1);

    // V1A..V3A first — their operating points do not depend on the follower.
    for (int s = V1A; s <= V3A; ++s) {
        stage_[static_cast<size_t>(s)].configure(cfg_[s]);
        stage_[static_cast<size_t>(s)].prepare(sampleRate_, maxBlockSize_);
        stage_[static_cast<size_t>(s)].setOversampling(oversampling_);
    }
    // V3B is DIRECT-COUPLED: its grid DC bias IS V3A's quiescent plate voltage
    // (the JCM800 V2B precedent, docs §14). The sheet's 213 V / 216 V pair is the
    // absolute check on this and the DC test asserts it.
    followerGridBias_ = stage_[V3A].quiescentPlateVoltage();
    cfg_[V3B].gridBias = followerGridBias_;
    stage_[V3B].configure(cfg_[V3B]);
    stage_[V3B].prepare(sampleRate_, maxBlockSize_);
    stage_[V3B].setOversampling(oversampling_);

    // Source impedances. V1A..V3A drive from a PLATE (Ra || rp); V3B drives the
    // tone stack from a CATHODE (1/gm || Rk), which is the structural reason this
    // amp's tone controls interact less than either Orange's.
    for (int s = V1A; s <= V3A; ++s) {
        const TriodeStage::Config& c = cfg_[s];
        const double Va = stage_[static_cast<size_t>(s)].quiescentPlateVoltage();
        const double Vk = stage_[static_cast<size_t>(s)].quiescentCathodeVoltage();
        const double h = 1e-2;
        const double dIp = (TriodeStage::korenPlateCurrent(Va + h, -Vk, c.tube) -
                            TriodeStage::korenPlateCurrent(Va - h, -Vk, c.tube)) /
                           (2.0 * h);
        const double rp = dIp > 1e-12 ? 1.0 / dIp : 1.0e6;
        plateRout_[static_cast<size_t>(s)] = 1.0 / (1.0 / c.Ra + 1.0 / rp);
    }
    {
        const double Vk = stage_[V3B].quiescentCathodeVoltage();
        const double Vgk = cfg_[V3B].gridBias - Vk;
        const double Va = cfg_[V3B].bPlus - Vk;
        const double h = 1e-4;
        const double gm = (TriodeStage::korenPlateCurrent(Va, Vgk + h, cfg_[V3B].tube) -
                           TriodeStage::korenPlateCurrent(Va, Vgk - h, cfg_[V3B].tube)) /
                          (2.0 * h);
        plateRout_[V3B] = 1.0 / (gm + 1.0 / cfg_[V3B].Rk);
    }

    input_.prepare(sampleRate_);
    input_.setSourceImpedance(plateRout_[V1A]);
    for (int i = 0; i < 2; ++i) {
        net_[static_cast<size_t>(i)].prepare(sampleRate_);
        net_[static_cast<size_t>(i)].setSourceImpedance(plateRout_[V2A + i]);
    }
    tone_.prepare(sampleRate_);
    tone_.setSourceImpedance(plateRout_[V3B]);
    applyMode();

    buf_.assign(static_cast<size_t>(maxBlockSize_), 0.0f);
    buf2_.assign(static_cast<size_t>(maxBlockSize_), 0.0f);
    // Snap onto the knobs as they stand NOW, then clear `primed_` so the first
    // process() snaps AGAIN. The second snap is the load-bearing one: the C ABI's
    // order is prepare-THEN-set, so without it a knob pushed after prepare would
    // ramp from the default over the first block while a reset() model would not,
    // and the two would render differently forever after (measured 1.016e+01 —
    // that is how this was found). Jcm800Preamp documents the same two-step.
    primeSmoothers();
    primed_ = false;
}

void MesaPreamp::setOversampling(int factor) {
    oversampling_ = factor;
    for (auto& s : stage_) s.setOversampling(factor);
}

void MesaPreamp::primeSmoothers() {
    pushKnobs();
    input_.snapKnobs();
    tone_.snapKnobs();
    primed_ = true;
}

void MesaPreamp::pushKnobs() {
    input_.setWiper(gainWiper());
    tone_.setKnobs(bass_, mid_, treble_, master_);
}

void MesaPreamp::setParameter(int paramId, float value) {
    const double v = clampParam01(value);
    switch (paramId) {
        case PARAM_GAIN: gain_ = v; break;
        case PARAM_MASTER: master_ = v; break;
        case PARAM_BASS: bass_ = v; break;
        case PARAM_MID: mid_ = v; break;
        case PARAM_TREBLE: treble_ = v; break;
        default: return;
    }
    pushKnobs();
}

void MesaPreamp::setMode(MesaMode m) {
    const int i = static_cast<int>(m);
    if (i < 0 || i >= static_cast<int>(MesaMode::MESA_MODE_COUNT)) return;
    if (m == mode_) return;
    mode_ = m;
    applyMode();
}

void MesaPreamp::reset() {
    for (auto& s : stage_) s.reset();
    input_.reset();
    for (auto& n : net_) n.reset();
    tone_.reset();
    primeSmoothers();
}

void MesaPreamp::process(const float* in, float* out, int numFrames) {
    if (numFrames <= 0) return;
    // DEFERRED SNAP (audit finding 6, docs §35). prepare() snaps the smoothers onto
    // whatever the knobs held at the time, but the C ABI's order is prepare-THEN-set,
    // so knobs pushed after prepare would otherwise RAMP from the defaults on the
    // first block. Snapping on the first process() is what makes a freshly prepared
    // model and a reset() model produce identical audio — without it, reset() vs
    // fresh measures 4.7e-01 (that is how this was found).
    if (!primed_) primeSmoothers();
    if (static_cast<int>(buf_.size()) < numFrames) {
        buf_.assign(static_cast<size_t>(numFrames), 0.0f);
        buf2_.assign(static_cast<size_t>(numFrames), 0.0f);
    }
    float* a = buf_.data();
    float* b = buf2_.data();

    stage_[V1A].process(in, a, numFrames);      // V1A
    input_.process(a, b, numFrames);            // pad + select + GAIN pot
    stage_[V2A].process(b, a, numFrames);       // V2A
    net_[0].process(a, b, numFrames);           // 20n / 470k / 1M
    stage_[V2B].process(b, a, numFrames);       // V2B (the cold stage)
    net_[1].process(a, b, numFrames);           // 20n / 220k / 330k
    stage_[V3A].process(b, a, numFrames);       // V3A
    stage_[V3B].process(a, b, numFrames);       // V3B cathode follower
    tone_.process(b, out, numFrames);           // FMV + MASTER

    double peak = 0.0;
    for (int n = 0; n < numFrames; ++n) {
        const double m = std::fabs(static_cast<double>(out[n]));
        if (m > peak) peak = m;
    }
    lastOutPeak_ = peak;
}

double MesaPreamp::stageQuiescentPlate(int stage) const {
    if (stage < 0 || stage >= STAGE_COUNT) return 0.0;
    return stage_[static_cast<size_t>(stage)].quiescentPlateVoltage();
}

double MesaPreamp::stageQuiescentCathode(int stage) const {
    if (stage < 0 || stage >= STAGE_COUNT) return 0.0;
    return stage_[static_cast<size_t>(stage)].quiescentCathodeVoltage();
}

double MesaPreamp::stageQuiescentCurrent(int stage) const {
    if (stage < 0 || stage >= STAGE_COUNT) return 0.0;
    return stage_[static_cast<size_t>(stage)].quiescentCurrent();
}

}  // namespace clipper::dsp
