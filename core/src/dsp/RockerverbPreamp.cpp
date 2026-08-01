// Clipper — RockerverbPreamp (M10.7, docs §63). See RockerverbPreamp.h for the
// topology, the PROVENANCE banner and the one modelling departure. This file is
// the numerics: the interstage MNA, the FMV tone-stack MNA, the transcribed
// TriodeStage configs, the 10k dropper chain and the block signal flow.

#include "clipper/dsp/RockerverbPreamp.h"

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
std::complex<double> solveComplex(std::complex<double> M[N][N + 1], int node) {
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
// self-bias Rk, on the Koren law alone. Used only to converge the dropper chain
// before the stages are configured once and prepared once (TriodeStage::prepare
// settles ~50k silent samples per call and cannot be run inside an iteration).
// g(ip) is strictly decreasing in ip, so BISECTION is exact and unconditionally
// stable — the same reason docs §57.2 gives for the OR120's D+ chain, whose
// obvious damped fixed point has a loop gain near -7 and oscillates.
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
}  // namespace

// ===========================================================================
// RockerverbInterstage
// ===========================================================================
namespace {
enum { PL = 0, NJ = 1, MM = 2, WW = 3 };
}

void RockerverbInterstage::configure(const Config& c) {
    cfg_ = c;
    dirty_ = true;
}

void RockerverbInterstage::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    T_ = 1.0 / sampleRate_;
    geqS_ = 2.0 * cfg_.Cshunt / T_;
    geqC_ = 2.0 * cfg_.Ccoup / T_;
    geqG_ = 2.0 * cfg_.Cgnd / T_;
    geqB_ = 2.0 * cfg_.Cbright / T_;
    gainSm_.prepare(kKnobSmoothSeconds, sampleRate_);
    reset();
}

void RockerverbInterstage::reset() {
    vS_ = iS_ = vC_ = iC_ = vG_ = iG_ = vB_ = iB_ = 0.0;
    snapKnobs();
}

void RockerverbInterstage::snapKnobs() {
    gainSm_.reset();
    gain_ = gainSm_.value();
    knobsMoving_ = false;
    ctrlCounter_ = 0;
    rebuild();
}

void RockerverbInterstage::setSourceImpedance(double rs) {
    rs_ = clampR(rs);
    dirty_ = true;
}

void RockerverbInterstage::setWiper(double g) {
    gainSm_.setTarget(clampParam(g, 1.0e-3, 1.0 - 1.0e-3));
    ctrlCounter_ = 0;
    if (!gainSm_.settled()) knobsMoving_ = true;
}

void RockerverbInterstage::rebuild() {
    std::array<std::array<double, N>, N> Gm{};
    auto stamp = [&](int a, int b, double g) {
        Gm[a][a] += g;
        Gm[b][b] += g;
        Gm[a][b] -= g;
        Gm[b][a] -= g;
    };
    Gm[PL][PL] += 1.0 / rs_;                       // driven from the plate
    if (cfg_.Cshunt > 0.0) Gm[PL][PL] += geqS_;    // 100p plate snubber
    stamp(PL, NJ, geqC_);                          // the interstage coupling cap
    stamp(NJ, MM, 1.0 / clampR(cfg_.Rser));
    Gm[MM][MM] += 1.0 / clampR(cfg_.Rgnd);
    if (cfg_.Cgnd > 0.0) Gm[MM][MM] += geqG_;
    if (cfg_.Rpot > 0.0) {
        stamp(MM, WW, 1.0 / clampR((1.0 - gain_) * cfg_.Rpot));
        Gm[WW][WW] += 1.0 / clampR(gain_ * cfg_.Rpot);
        if (cfg_.Cbright > 0.0) stamp(MM, WW, geqB_);
    } else {
        stamp(MM, WW, 1.0 / kShortOhms);
        Gm[WW][WW] += 1.0 / kOpenOhms;
    }
    invertInPlace<N>(Gm, Ginv_);
    dirty_ = false;
}

void RockerverbInterstage::process(const float* in, float* out, int numFrames) {
    if (dirty_) rebuild();
    const double gs = 1.0 / rs_;
    const bool hasS = cfg_.Cshunt > 0.0;
    const bool hasG = cfg_.Cgnd > 0.0;
    const bool hasB = cfg_.Rpot > 0.0 && cfg_.Cbright > 0.0;
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
        const double IeqS = hasS ? (geqS_ * vS_ + iS_) : 0.0;
        const double IeqC = geqC_ * vC_ + iC_;
        const double IeqG = hasG ? (geqG_ * vG_ + iG_) : 0.0;
        const double IeqB = hasB ? (geqB_ * vB_ + iB_) : 0.0;
        std::array<double, N> b{};
        b[PL] = gs * static_cast<double>(in[n]) + IeqS + IeqC;
        b[NJ] = -IeqC;
        b[MM] = IeqG + IeqB;
        b[WW] = -IeqB;
        std::array<double, N> v{};
        for (int r = 0; r < N; ++r) {
            double acc = 0.0;
            for (int cc = 0; cc < N; ++cc) acc += Ginv_[r][cc] * b[cc];
            v[r] = acc;
        }
        // Anti-denormal (Denormal.h, docs §33, ADR 006): every companion here
        // rests at exactly zero, so a silent tail rings them into double
        // subnormals and they stick — invisible in the float output.
        if (hasS) {
            const double vn = v[PL];
            iS_ = flushDenormal(geqS_ * vn - IeqS);
            vS_ = flushDenormal(vn);
        }
        {
            const double vn = v[PL] - v[NJ];
            iC_ = flushDenormal(geqC_ * vn - IeqC);
            vC_ = flushDenormal(vn);
        }
        if (hasG) {
            const double vn = v[MM];
            iG_ = flushDenormal(geqG_ * vn - IeqG);
            vG_ = flushDenormal(vn);
        }
        if (hasB) {
            const double vn = v[MM] - v[WW];
            iB_ = flushDenormal(geqB_ * vn - IeqB);
            vB_ = flushDenormal(vn);
        }
        out[n] = static_cast<float>(v[WW]);
    }
}

double RockerverbInterstage::magnitudeAt(double freqHz, const Config& c, double g,
                                         double rs) {
    using Cx = std::complex<double>;
    const Cx jw(0.0, 2.0 * kPi * freqHz);
    Cx M[N][N + 1];
    for (auto& row : M)
        for (auto& e : row) e = Cx(0.0, 0.0);
    auto stamp = [&](int a, int b, Cx gg) {
        M[a][a] += gg;
        M[b][b] += gg;
        M[a][b] -= gg;
        M[b][a] -= gg;
    };
    const double rsc = rs > 1.0 ? rs : 1.0;
    M[PL][PL] += Cx(1.0 / rsc, 0.0);
    if (c.Cshunt > 0.0) M[PL][PL] += jw * c.Cshunt;
    stamp(PL, NJ, jw * c.Ccoup);
    stamp(NJ, MM, Cx(1.0 / clampR(c.Rser), 0.0));
    M[MM][MM] += Cx(1.0 / clampR(c.Rgnd), 0.0);
    if (c.Cgnd > 0.0) M[MM][MM] += jw * c.Cgnd;
    if (c.Rpot > 0.0) {
        const double gg = clampParam(g, 1.0e-3, 1.0 - 1.0e-3);
        stamp(MM, WW, Cx(1.0 / clampR((1.0 - gg) * c.Rpot), 0.0));
        M[WW][WW] += Cx(1.0 / clampR(gg * c.Rpot), 0.0);
        if (c.Cbright > 0.0) stamp(MM, WW, jw * c.Cbright);
    } else {
        stamp(MM, WW, Cx(1.0 / kShortOhms, 0.0));
        M[WW][WW] += Cx(1.0 / kOpenOhms, 0.0);
    }
    M[PL][N] = Cx(1.0 / rsc, 0.0);  // unit source behind rs
    return std::abs(solveComplex<N>(M, WW));
}

// ===========================================================================
// RockerverbToneStack
// ===========================================================================
namespace {
enum { IN = 0, TT = 1, AA = 2, BB = 3, CC = 4, MW = 5, OUT = 6, VW = 7 };
}

void RockerverbToneStack::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    T_ = 1.0 / sampleRate_;
    geqT_ = 2.0 * kCt / T_;
    geqB_ = 2.0 * kCb / T_;
    geqM_ = 2.0 * kCm / T_;
    bassSm_.prepare(kKnobSmoothSeconds, sampleRate_);
    midSm_.prepare(kKnobSmoothSeconds, sampleRate_);
    trebleSm_.prepare(kKnobSmoothSeconds, sampleRate_);
    volSm_.prepare(kKnobSmoothSeconds, sampleRate_);
    reset();
}

void RockerverbToneStack::reset() {
    vT_ = iT_ = vB_ = iB_ = vM_ = iM_ = 0.0;
    snapKnobs();
}

void RockerverbToneStack::snapKnobs() {
    bassSm_.reset();
    midSm_.reset();
    trebleSm_.reset();
    volSm_.reset();
    bass_ = bassSm_.value();
    mid_ = midSm_.value();
    treble_ = trebleSm_.value();
    vol_ = volSm_.value();
    knobsMoving_ = false;
    ctrlCounter_ = 0;
    rebuild();
}

void RockerverbToneStack::setSourceImpedance(double rs) {
    rs_ = clampR(rs);
    dirty_ = true;
}

void RockerverbToneStack::setKnobs(double bass, double mid, double treble,
                                   double volume) {
    auto cl = [](double v) { return clampParam(v, 1.0e-3, 1.0 - 1.0e-3); };
    bassSm_.setTarget(cl(bass));
    midSm_.setTarget(cl(mid));
    trebleSm_.setTarget(cl(treble));
    volSm_.setTarget(cl(volume));
    ctrlCounter_ = 0;
    if (!(bassSm_.settled() && midSm_.settled() && trebleSm_.settled() &&
          volSm_.settled()))
        knobsMoving_ = true;
}

void RockerverbToneStack::rebuild() {
    std::array<std::array<double, N>, N> Gm{};
    auto stamp = [&](int a, int b, double g) {
        Gm[a][a] += g;
        Gm[b][b] += g;
        Gm[a][b] -= g;
        Gm[b][a] -= g;
    };
    Gm[IN][IN] += 1.0 / rs_;                                  // V12's plate
    stamp(IN, TT, geqT_);                                     // 560p treble cap
    stamp(IN, AA, 1.0 / kRslope);                             // 39k slope
    stamp(AA, BB, geqB_);                                     // 22n, IN SERIES
    stamp(AA, MW, geqM_);                                     // 22n to the MID WIPER
    stamp(TT, OUT, 1.0 / clampR((1.0 - treble_) * kRT));      // treble upper
    stamp(OUT, BB, 1.0 / clampR(treble_ * kRT));              // treble lower
    stamp(BB, CC, 1.0 / clampR(bass_ * kRB));                 // bass rheostat
    stamp(CC, MW, 1.0 / clampR((1.0 - mid_) * kRM));          // mid upper
    Gm[MW][MW] += 1.0 / clampR(mid_ * kRM);                   // mid lower to gnd
    stamp(OUT, VW, 1.0 / clampR((1.0 - vol_) * kRV));         // volume upper
    Gm[VW][VW] += 1.0 / clampR(vol_ * kRV);                   // volume lower
    Gm[VW][VW] += 1.0 / kRpi;                                 // PI grid leak
    invertInPlace<N>(Gm, Ginv_);
    dirty_ = false;
}

void RockerverbToneStack::process(const float* in, float* out, int numFrames) {
    if (dirty_) rebuild();
    const double gs = 1.0 / rs_;
    for (int n = 0; n < numFrames; ++n) {
        if (knobsMoving_) {
            const double b = bassSm_.next();
            const double m = midSm_.next();
            const double t = trebleSm_.next();
            const double v = volSm_.next();
            if (ctrlCounter_ == 0) {
                if (b != bass_ || m != mid_ || t != treble_ || v != vol_) {
                    bass_ = b;
                    mid_ = m;
                    treble_ = t;
                    vol_ = v;
                    rebuild();
                }
                if (bassSm_.settled() && midSm_.settled() && trebleSm_.settled() &&
                    volSm_.settled())
                    knobsMoving_ = false;
            }
            if (++ctrlCounter_ >= kCtrlBlock) ctrlCounter_ = 0;
        }
        const double IeqT = geqT_ * vT_ + iT_;
        const double IeqB = geqB_ * vB_ + iB_;
        const double IeqM = geqM_ * vM_ + iM_;
        std::array<double, N> b{};
        b[IN] = gs * static_cast<double>(in[n]) + IeqT;
        b[TT] = -IeqT;
        b[AA] = IeqB + IeqM;
        b[BB] = -IeqB;
        b[MW] = -IeqM;
        std::array<double, N> v{};
        for (int r = 0; r < N; ++r) {
            double acc = 0.0;
            for (int cc = 0; cc < N; ++cc) acc += Ginv_[r][cc] * b[cc];
            v[r] = acc;
        }
        const double vTn = v[IN] - v[TT];
        const double vBn = v[AA] - v[BB];
        const double vMn = v[AA] - v[MW];
        // Anti-denormal (docs §33): all six companions rest at exactly zero —
        // the same 68x cliff MarshallToneStack measured in isolation.
        iT_ = flushDenormal(geqT_ * vTn - IeqT);
        iB_ = flushDenormal(geqB_ * vBn - IeqB);
        iM_ = flushDenormal(geqM_ * vMn - IeqM);
        vT_ = flushDenormal(vTn);
        vB_ = flushDenormal(vBn);
        vM_ = flushDenormal(vMn);
        out[n] = static_cast<float>(v[VW]);
    }
}

double RockerverbToneStack::magnitudeAt(double freqHz, double bass, double mid,
                                        double treble, double volume, double rs,
                                        Probe probe) {
    using Cx = std::complex<double>;
    const Cx jw(0.0, 2.0 * kPi * freqHz);
    Cx M[N][N + 1];
    for (auto& row : M)
        for (auto& e : row) e = Cx(0.0, 0.0);
    auto stamp = [&](int a, int b, Cx g) {
        M[a][a] += g;
        M[b][b] += g;
        M[a][b] -= g;
        M[b][a] -= g;
    };
    auto cl = [](double v) { return clampParam(v, 1.0e-3, 1.0 - 1.0e-3); };
    const double b = cl(bass), m = cl(mid), t = cl(treble), v = cl(volume);
    const double rsc = rs > 1.0 ? rs : 1.0;
    M[IN][IN] += Cx(1.0 / rsc, 0.0);
    stamp(IN, TT, jw * kCt);
    stamp(IN, AA, Cx(1.0 / kRslope, 0.0));
    stamp(AA, BB, jw * kCb);
    stamp(AA, MW, jw * kCm);
    stamp(TT, OUT, Cx(1.0 / clampR((1.0 - t) * kRT), 0.0));
    stamp(OUT, BB, Cx(1.0 / clampR(t * kRT), 0.0));
    stamp(BB, CC, Cx(1.0 / clampR(b * kRB), 0.0));
    stamp(CC, MW, Cx(1.0 / clampR((1.0 - m) * kRM), 0.0));
    M[MW][MW] += Cx(1.0 / clampR(m * kRM), 0.0);
    stamp(OUT, VW, Cx(1.0 / clampR((1.0 - v) * kRV), 0.0));
    M[VW][VW] += Cx(1.0 / clampR(v * kRV), 0.0);
    M[VW][VW] += Cx(1.0 / kRpi, 0.0);
    M[IN][N] = Cx(1.0 / rsc, 0.0);
    return std::abs(solveComplex<N>(M, probe == Probe::Out ? OUT : VW));
}

// ===========================================================================
// RockerverbPreamp
// ===========================================================================

// The three interstage networks, ALL TRANSCRIBED from the .schx netlist.
// Designators are the file's.
static const RockerverbInterstage::Config kNet[3] = {
    // S1 -> S2: C1 1n, R30 220k, R1 220k || C2 470p, GAIN-1 1M with C16 100p.
    {0.0, 1.0e-9, 220.0e3, 220.0e3, 470.0e-12, 1.0e6, 100.0e-12},
    // S2 -> S3: C4 100p across S2's plate load, C13 2n2, R31 220k, R32 470k,
    // GAIN-2 1M (no bright cap on this gang).
    {100.0e-12, 2.2e-9, 220.0e3, 470.0e3, 0.0, 1.0e6, 0.0},
    // S3 -> S4: C6 100p across S3's plate load, C7 4n7, R6 470k, R5 220k. NO POT
    // — a fixed 470k/220k divider (0.319) straight into S4's grid.
    {100.0e-12, 4.7e-9, 470.0e3, 220.0e3, 0.0, 0.0, 0.0},
};

const RockerverbInterstage::Config& RockerverbPreamp::interstageConfig(int i) {
    return kNet[i < 0 ? 0 : (i > 2 ? 2 : i)];
}

double RockerverbPreamp::logTaper(double x) {
    x = clampParam01(x);
    return (std::exp(kLogTaperK * x) - 1.0) / (std::exp(kLogTaperK) - 1.0);
}

double RockerverbPreamp::gainWiper() const { return logTaper(gain_); }
double RockerverbPreamp::bassWiper() const { return logTaper(bass_); }

RockerverbPreamp::RockerverbPreamp() {
    for (int i = 0; i < 3; ++i) net_[static_cast<size_t>(i)].configure(kNet[i]);
    configureStages();
}

// Per-stage circuit values — ALL TRANSCRIBED (the .schx netlist):
//   S1 V9  : Ra R19 100k -> B2 ; Rk R2  1k5 || C14 10uF ; grid R23 68k, R20 1M
//   S2 V10 : Ra R24 100k -> B2 ; Rk R25 1k  || C3  10uF
//   S3 V11 : Ra R26 100k -> B1 ; Rk R27 2k2 || C5  10uF
//   S4 V12 : Ra R4  100k -> B1 ; Rk R3  1k5 UNBYPASSED  <- the cold/tight stage
//
// Cc/Rgl: see the header's "ONE MODELLING DEPARTURE". Cc is a deliberately
// transparent 1 uF (the REAL coupling caps live in the interstage MNAs, once);
// Rgl is the following network's real RESISTIVE input, which is what loads the
// plate. Rg is each grid's real series source impedance — the transcribed 68k
// stopper on S1, the 1M pots' worst-case Thevenin (R/4 = 250k) on S2/S3, and the
// transcribed 470k||220k = 149.8k divider impedance on S4.
void RockerverbPreamp::configureStages() {
    auto mk = [](double bPlus, double Rk, double Ck, double Rg, double Rgl) {
        TriodeStage::Config c{};
        c.bPlus = bPlus;
        c.Ra = kRa;
        c.Rk = Rk;
        c.Ck = Ck;
        c.Rg = Rg;
        c.Cc = 1.0e-6;
        c.Rgl = Rgl;
        return c;
    };
    // Net 0's resistive input: R30 + (R1 || GAIN-1) = 220k + (220k || 1M).
    const double rin0 = 220.0e3 + 1.0 / (1.0 / 220.0e3 + 1.0 / 1.0e6);
    // Net 1's: R31 + (R32 || GAIN-2) = 220k + (470k || 1M).
    const double rin1 = 220.0e3 + 1.0 / (1.0 / 470.0e3 + 1.0 / 1.0e6);
    // Net 2's: R6 + R5 = 470k + 220k.
    const double rin2 = 470.0e3 + 220.0e3;
    // The tone stack is DC-BLOCKED from S4's plate (every path out of node A goes
    // through a cap), so it has no DC input resistance at all. S4's plate load is
    // therefore R4 alone at DC; Rgl stands for the stack's midband input, taken
    // as the transcribed slope resistor plus the bass+mid legs at noon.
    const double rin3 = 220.0e3;
    cfg_[S1] = mk(b2_, 1.5e3, 10.0e-6, 68.0e3, rin0);
    cfg_[S2] = mk(b2_, 1.0e3, 10.0e-6, 250.0e3, rin1);
    cfg_[S3] = mk(b1_, 2.2e3, 10.0e-6, 250.0e3, rin2);
    cfg_[S4] = mk(b1_, 1.5e3, 0.0, 149.8e3, rin3);
}

// The transcribed dropper chain: 400 V --R9 10k--> B1 --R42 10k--> B2.
// B1 carries ALL FOUR stages' current, B2 only S1 + S2's, and each stage's
// current depends on its own rail, so the pair is a nested fixed point. Both are
// bisected on a monotone residual, never iterated as a map (docs §57.2).
void RockerverbPreamp::solveSupply() {
    const TriodeStage::KorenParams tube{};
    auto ip = [&](double rail, double Rk) {
        return korenSelfBiasCurrent(rail, kRa, Rk, tube);
    };
    // For a candidate B1, B2 = B1 - R42*(I_S1(B2) + I_S2(B2)) is itself monotone
    // decreasing in B2, so bisect the inner one first.
    auto solveB2 = [&](double b1) {
        auto h = [&](double d) {
            return b1 - kRdrop2 * (ip(d, 1.5e3) + ip(d, 1.0e3)) - d;
        };
        double lo = 0.0, hi = b1;
        for (int it = 0; it < 120; ++it) {
            const double m = 0.5 * (lo + hi);
            if (h(m) > 0.0)
                lo = m;
            else
                hi = m;
        }
        return 0.5 * (lo + hi);
    };
    auto h1 = [&](double b1) {
        const double b2 = solveB2(b1);
        const double itot = ip(b2, 1.5e3) + ip(b2, 1.0e3) + ip(b1, 2.2e3) + ip(b1, 1.5e3);
        return kVsupplyRaw - kRdrop1 * itot - b1;
    };
    double lo = 0.0, hi = kVsupplyRaw;
    for (int it = 0; it < 120; ++it) {
        const double m = 0.5 * (lo + hi);
        if (h1(m) > 0.0)
            lo = m;
        else
            hi = m;
    }
    b1_ = 0.5 * (lo + hi);
    b2_ = solveB2(b1_);
}

void RockerverbPreamp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    buf_.assign(static_cast<size_t>(maxBlockSize_), 0.0f);

    solveSupply();
    configureStages();
    for (int s = S1; s <= S4; ++s) {
        stage_[s].configure(cfg_[s]);
        stage_[s].prepare(sampleRate_, maxBlockSize_);
        stage_[s].setOversampling(oversampling_);
    }

    // Every network in this preamp is driven from a PLATE — there is no cathode
    // follower anywhere in the Rockerverb's dirty channel, which is a real part
    // of why its tone stack is softer than a 2204's. rout = Ra || rp, with rp =
    // dVa/dIp from the Koren law at the stage's operating point.
    for (int s = S1; s <= S4; ++s) {
        const TriodeStage::Config& c = cfg_[s];
        const double Va = stage_[s].quiescentPlateVoltage();
        const double Vk = stage_[s].quiescentCathodeVoltage();
        const double h = 1e-2;
        const double dIp = (TriodeStage::korenPlateCurrent(Va + h, -Vk, c.tube) -
                            TriodeStage::korenPlateCurrent(Va - h, -Vk, c.tube)) /
                           (2.0 * h);
        const double rp = dIp > 1e-12 ? 1.0 / dIp : 1.0e6;
        plateRout_[static_cast<size_t>(s)] = 1.0 / (1.0 / c.Ra + 1.0 / rp);
    }

    for (int i = 0; i < 3; ++i) {
        net_[static_cast<size_t>(i)].configure(kNet[i]);
        net_[static_cast<size_t>(i)].prepare(sampleRate_);
        net_[static_cast<size_t>(i)].setSourceImpedance(plateRout_[static_cast<size_t>(i)]);
    }
    tone_.prepare(sampleRate_);
    tone_.setSourceImpedance(plateRout_[S4]);
    pushKnobs();

    primed_ = false;
}

void RockerverbPreamp::pushKnobs() {
    const double gw = gainWiper();
    net_[0].setWiper(gw);
    net_[1].setWiper(gw);  // the GANG: one knob, two wipers (Group="Gain")
    tone_.setKnobs(bassWiper(), mid_, treble_, volume_);
}

void RockerverbPreamp::primeSmoothers() {
    for (auto& n : net_) n.snapKnobs();
    tone_.snapKnobs();
}

void RockerverbPreamp::reset() {
    for (auto& s : stage_) s.reset();
    for (auto& n : net_) n.reset();
    tone_.reset();
    lastOutPeak_ = 0.0;
}

void RockerverbPreamp::setOversampling(int factor) {
    oversampling_ = factor;
    for (auto& s : stage_) s.setOversampling(factor);
}

void RockerverbPreamp::setParameter(int paramId, float value) {
    const double v = clampParam01(static_cast<double>(value));
    switch (paramId) {
        case PARAM_GAIN: gain_ = v; break;
        case PARAM_VOLUME: volume_ = v; break;
        case PARAM_BASS: bass_ = v; break;
        case PARAM_MID: mid_ = v; break;
        case PARAM_TREBLE: treble_ = v; break;
        default: return;
    }
    pushKnobs();
}

void RockerverbPreamp::process(const float* in, float* out, int numFrames) {
    if (!primed_) {
        primeSmoothers();
        primed_ = true;
    }
    lastOutPeak_ = 0.0;
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        float* w = buf_.data();
        // THE SCHEMATIC ORDER: S1 -> GAIN-1 -> S2 -> GAIN-2 -> S3 -> divider ->
        // S4 -> FMV stack -> VOLUME. Both GAIN pots and the VOLUME pot are
        // INSIDE their MNAs, so there is no scalar knob multiply anywhere in this
        // signal path: every pot is a real load on the network it sits in, and
        // per-sample smoothing happens inside those matrices (audit finding 6).
        stage_[S1].process(in + off, w, n);
        net_[0].process(w, w, n);
        stage_[S2].process(w, w, n);
        net_[1].process(w, w, n);
        stage_[S3].process(w, w, n);
        net_[2].process(w, w, n);
        stage_[S4].process(w, w, n);
        tone_.process(w, w, n);
        for (int i = 0; i < n; ++i) {
            out[off + i] = w[i];
            lastOutPeak_ =
                std::max(lastOutPeak_, std::fabs(static_cast<double>(w[i])));
        }
        off += n;
    }
}

double RockerverbPreamp::stageQuiescentPlate(int stage) const {
    return stage_[static_cast<size_t>(stage)].quiescentPlateVoltage();
}
double RockerverbPreamp::stageQuiescentCathode(int stage) const {
    return stage_[static_cast<size_t>(stage)].quiescentCathodeVoltage();
}
double RockerverbPreamp::stageQuiescentCurrent(int stage) const {
    return stage_[static_cast<size_t>(stage)].quiescentCurrent();
}

}  // namespace clipper::dsp
