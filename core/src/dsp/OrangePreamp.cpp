// Clipper — OrangePreamp (M10.3, docs §57; SCHEMATIC CORRECTION 2026-07-31).
// See OrangePreamp.h for the topology, the structural correction and the
// PROVENANCE banner. This file is the numerics: the James+GAIN+grid MNA, the
// F.A.C. network, the transcribed TriodeStage configs, the 33k dropper chain and
// the block signal flow.

#include "clipper/dsp/OrangePreamp.h"

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

enum { F = 0, A = 1, W = 2, B = 3, T = 4, U = 5, OUT = 6, G = 7, GRID = 8 };

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
// self-bias Rk, solved on the Koren law alone (no TriodeStage::prepare, which
// settles ~50k silent samples per call). Used only to converge the 33k dropper
// chain before the stages are configured once and prepared once.
// g(ip) = koren(B+ - ip*Ra - ip*Rk, -ip*Rk) - ip is strictly decreasing in ip (more
// current lowers the plate AND raises the cathode), so BISECTION is exact and
// unconditionally stable. A damped fixed point is not: the loop gain here measures
// about -7, so the obvious `ip += k*(f-ip)` oscillates for any k above ~0.25.
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
// JamesToneStack — James network + GAIN pot + 330p grid coupling, one MNA
// ===========================================================================
void JamesToneStack::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    T_ = 1.0 / sampleRate_;
    geqBu_ = 2.0 * kCbUpper / T_;
    geqBl_ = 2.0 * kCbLower / T_;
    geq2_ = 2.0 * kC2 / T_;
    geq3_ = 2.0 * kC3 / T_;
    geqg_ = 2.0 * kCg / T_;
    bassSm_.prepare(kKnobSmoothSeconds, sampleRate_);
    trebleSm_.prepare(kKnobSmoothSeconds, sampleRate_);
    gainSm_.prepare(kKnobSmoothSeconds, sampleRate_);
    reset();
}

void JamesToneStack::reset() {
    vBu_ = iBu_ = vBl_ = iBl_ = v2_ = i2_ = v3_ = i3_ = vg_ = ig_ = 0.0;
    snapKnobs();
}

void JamesToneStack::snapKnobs() {
    bassSm_.reset();
    trebleSm_.reset();
    gainSm_.reset();
    bass_ = bassSm_.value();
    treble_ = trebleSm_.value();
    gain_ = gainSm_.value();
    knobsMoving_ = false;
    ctrlCounter_ = 0;
    rebuild();
}

void JamesToneStack::setSourceImpedance(double rs) {
    rs_ = clampR(rs);
    dirty_ = true;
}

void JamesToneStack::setKnobs(double bass, double treble, double gain) {
    auto cl = [](double v) { return clampParam(v, 1.0e-3, 1.0 - 1.0e-3); };
    bassSm_.setTarget(cl(bass));
    trebleSm_.setTarget(cl(treble));
    gainSm_.setTarget(cl(gain));
    ctrlCounter_ = 0;
    if (!(bassSm_.settled() && trebleSm_.settled() && gainSm_.settled()))
        knobsMoving_ = true;
}

void JamesToneStack::rebuild() {
    std::array<std::array<double, N>, N> Gm{};
    auto stamp = [&](int a, int b, double g) {
        Gm[a][a] += g;
        Gm[b][b] += g;
        Gm[a][b] -= g;
        Gm[b][a] -= g;
    };
    Gm[F][F] += 1.0 / rs_;                                    // V1A plate source
    stamp(F, A, 1.0 / kR1);                                   // 100k into BASS top
    stamp(A, W, 1.0 / clampR((1.0 - bass_) * kRB));           // BASS upper section
    stamp(A, W, geqBu_);                                      // 2n2 across it
    stamp(W, B, 1.0 / clampR(bass_ * kRB));                   // BASS lower section
    stamp(W, B, geqBl_);                                      // 22n across it
    Gm[B][B] += 1.0 / kR3;                                    // 22k to ground
    stamp(W, OUT, 1.0 / kR2);                                 // 100k wiper -> out
    stamp(F, T, geq2_);                                       // 1n5 to TREBLE top
    stamp(T, OUT, 1.0 / clampR((1.0 - treble_) * kRT));       // TREBLE upper
    stamp(OUT, U, 1.0 / clampR(treble_ * kRT));               // TREBLE lower
    Gm[U][U] += geq3_;                                        // 10n to ground
    stamp(OUT, G, 1.0 / clampR((1.0 - gain_) * kRG));         // GAIN upper
    Gm[G][G] += 1.0 / clampR(gain_ * kRG);                    // GAIN lower to gnd
    stamp(G, GRID, geqg_);                                    // 330p -> V1B grid
    Gm[GRID][GRID] += 1.0 / kRgl;                             // V1B grid leak

    invertInPlace<N>(Gm, Ginv_);
    dirty_ = false;
}

void JamesToneStack::process(const float* in, float* out, int numFrames) {
    if (dirty_) rebuild();
    const double gs = 1.0 / rs_;
    for (int n = 0; n < numFrames; ++n) {
        if (knobsMoving_) {
            const double b = bassSm_.next();
            const double t = trebleSm_.next();
            const double g = gainSm_.next();
            if (ctrlCounter_ == 0) {
                if (b != bass_ || t != treble_ || g != gain_) {
                    bass_ = b;
                    treble_ = t;
                    gain_ = g;
                    rebuild();
                }
                if (bassSm_.settled() && trebleSm_.settled() && gainSm_.settled())
                    knobsMoving_ = false;
            }
            if (++ctrlCounter_ >= kCtrlBlock) ctrlCounter_ = 0;
        }
        const double Vs = static_cast<double>(in[n]);
        const double IeqBu = geqBu_ * vBu_ + iBu_;
        const double IeqBl = geqBl_ * vBl_ + iBl_;
        const double Ieq2 = geq2_ * v2_ + i2_;
        const double Ieq3 = geq3_ * v3_ + i3_;
        const double Ieqg = geqg_ * vg_ + ig_;
        std::array<double, N> b{};
        b[F] = gs * Vs + Ieq2;
        b[A] = IeqBu;
        b[W] = -IeqBu + IeqBl;
        b[B] = -IeqBl;
        b[T] = -Ieq2;
        b[U] = Ieq3;
        b[OUT] = 0.0;
        b[G] = Ieqg;
        b[GRID] = -Ieqg;
        std::array<double, N> v{};
        for (int r = 0; r < N; ++r) {
            double acc = 0.0;
            for (int cc = 0; cc < N; ++cc) acc += Ginv_[r][cc] * b[cc];
            v[r] = acc;
        }
        const double vBun = v[A] - v[W];
        const double vBln = v[W] - v[B];
        const double v2n = v[F] - v[T];
        const double v3n = v[U];
        const double vgn = v[G] - v[GRID];
        // Anti-denormal (Denormal.h, docs §33): all ten companions REST AT ZERO,
        // so a silent tail rings them into double subnormals and they stick — the
        // same 68x cliff MarshallToneStack measured. Invisible in the float output.
        iBu_ = flushDenormal(geqBu_ * vBun - IeqBu);
        iBl_ = flushDenormal(geqBl_ * vBln - IeqBl);
        i2_ = flushDenormal(geq2_ * v2n - Ieq2);
        i3_ = flushDenormal(geq3_ * v3n - Ieq3);
        ig_ = flushDenormal(geqg_ * vgn - Ieqg);
        vBu_ = flushDenormal(vBun);
        vBl_ = flushDenormal(vBln);
        v2_ = flushDenormal(v2n);
        v3_ = flushDenormal(v3n);
        vg_ = flushDenormal(vgn);
        out[n] = static_cast<float>(v[GRID]);
    }
}

// Steady-state |H(jw)| straight off the NETLIST (complex nodal solve). Written
// from the component constants rather than from the discretized matrix, so it is
// an independent evaluation of the same netlist — the honest limit of that (it
// cannot catch a wrong topology) is recorded in docs §57.
double JamesToneStack::magnitudeAt(double freqHz, double bass, double treble,
                                   double gain, double rs, Probe probe) {
    using C = std::complex<double>;
    const C jw(0.0, 2.0 * 3.14159265358979323846 * freqHz);
    C M[N][N + 1];
    for (auto& row : M)
        for (auto& e : row) e = C(0.0, 0.0);
    auto stamp = [&](int a, int b, C g) {
        M[a][a] += g;
        M[b][b] += g;
        M[a][b] -= g;
        M[b][a] -= g;
    };
    const double rsc = rs > 1.0 ? rs : 1.0;
    M[F][F] += C(1.0 / rsc, 0.0);
    stamp(F, A, C(1.0 / kR1, 0.0));
    stamp(A, W, C(1.0 / clampR((1.0 - bass) * kRB), 0.0));
    stamp(A, W, jw * kCbUpper);
    stamp(W, B, C(1.0 / clampR(bass * kRB), 0.0));
    stamp(W, B, jw * kCbLower);
    M[B][B] += C(1.0 / kR3, 0.0);
    stamp(W, OUT, C(1.0 / kR2, 0.0));
    stamp(F, T, jw * kC2);
    stamp(T, OUT, C(1.0 / clampR((1.0 - treble) * kRT), 0.0));
    stamp(OUT, U, C(1.0 / clampR(treble * kRT), 0.0));
    M[U][U] += jw * kC3;
    stamp(OUT, G, C(1.0 / clampR((1.0 - gain) * kRG), 0.0));
    M[G][G] += C(1.0 / clampR(gain * kRG), 0.0);
    stamp(G, GRID, jw * kCg);
    M[GRID][GRID] += C(1.0 / kRgl, 0.0);
    M[F][N] = C(1.0 / rsc, 0.0);  // unit source behind rs
    return std::abs(solveComplex<N>(M, probe == Probe::Out ? OUT : GRID));
}

// ===========================================================================
// FacNetwork
// ===========================================================================
namespace {
enum { P = 0, Q = 1, GG = 2 };
}

void FacNetwork::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    T_ = 1.0 / sampleRate_;
    geqc_ = 2.0 * kCaps[pos_] / T_;
    reset();
}

void FacNetwork::reset() {
    vc_ = ic_ = 0.0;
    rebuild();
}

void FacNetwork::setSourceImpedance(double rs) {
    rs_ = clampR(rs);
    dirty_ = true;
}

// The F.A.C. is a SWITCH, not a knob: there is nothing to smooth between two cap
// values and the real rotary steps too. The worklet/plugin bracket a position
// change with the chain declick; the cap STATE is deliberately carried across so
// the stored charge is not discontinuous (which is what the real switch does).
void FacNetwork::setPosition(int pos) {
    const int p = pos < 0 ? 0 : (pos >= kPositions ? kPositions - 1 : pos);
    if (p == pos_) return;
    pos_ = p;
    geqc_ = 2.0 * kCaps[pos_] / T_;
    dirty_ = true;
}

void FacNetwork::rebuild() {
    std::array<std::array<double, N>, N> Gm{};
    auto stamp = [&](int a, int b, double g) {
        Gm[a][a] += g;
        Gm[b][b] += g;
        Gm[a][b] -= g;
        Gm[b][a] -= g;
    };
    Gm[P][P] += 1.0 / rs_;
    if (kCaps[pos_] <= 0.0)
        stamp(P, Q, 1.0 / kShortOhms);  // the straight-through click
    else
        stamp(P, Q, geqc_);
    stamp(Q, GG, 1.0 / kRloop);
    Gm[GG][GG] += 1.0 / kRgl;
    invertInPlace<N>(Gm, Ginv_);
    dirty_ = false;
}

void FacNetwork::process(const float* in, float* out, int numFrames) {
    if (dirty_) rebuild();
    const double gs = 1.0 / rs_;
    const bool shorted = kCaps[pos_] <= 0.0;
    for (int n = 0; n < numFrames; ++n) {
        const double Ieq = shorted ? 0.0 : (geqc_ * vc_ + ic_);
        std::array<double, N> b{};
        b[P] = gs * static_cast<double>(in[n]) + Ieq;
        b[Q] = -Ieq;
        b[GG] = 0.0;
        std::array<double, N> v{};
        for (int r = 0; r < N; ++r) {
            double acc = 0.0;
            for (int cc = 0; cc < N; ++cc) acc += Ginv_[r][cc] * b[cc];
            v[r] = acc;
        }
        if (!shorted) {
            const double vn = v[P] - v[Q];
            // Rests at exactly zero (docs §33, ADR 006).
            ic_ = flushDenormal(geqc_ * vn - Ieq);
            vc_ = flushDenormal(vn);
        }
        out[n] = static_cast<float>(v[GG]);
    }
}

double FacNetwork::magnitudeAt(double freqHz, int pos, double rs) {
    using C = std::complex<double>;
    const int p = pos < 0 ? 0 : (pos >= kPositions ? kPositions - 1 : pos);
    const C jw(0.0, 2.0 * 3.14159265358979323846 * freqHz);
    C M[N][N + 1];
    for (auto& row : M)
        for (auto& e : row) e = C(0.0, 0.0);
    auto stamp = [&](int a, int b, C g) {
        M[a][a] += g;
        M[b][b] += g;
        M[a][b] -= g;
        M[b][a] -= g;
    };
    const double rsc = rs > 1.0 ? rs : 1.0;
    M[P][P] += C(1.0 / rsc, 0.0);
    if (kCaps[p] <= 0.0)
        stamp(P, Q, C(1.0 / kShortOhms, 0.0));
    else
        stamp(P, Q, jw * kCaps[p]);
    stamp(Q, GG, C(1.0 / kRloop, 0.0));
    M[GG][GG] += C(1.0 / kRgl, 0.0);
    M[P][N] = C(1.0 / rsc, 0.0);
    return std::abs(solveComplex<N>(M, GG));
}

// ===========================================================================
// OrangePreamp
// ===========================================================================
double OrangePreamp::volumeTaper(double x) {
    x = clampParam01(x);
    return (std::exp(kVolumeTaperK * x) - 1.0) / (std::exp(kVolumeTaperK) - 1.0);
}

OrangePreamp::OrangePreamp() { configureStages(); }

void OrangePreamp::setSupplyCPlus(double cPlus) {
    cPlus_ = cPlus > 1.0 ? cPlus : kCplusDefault;
}

// Per-stage circuit values — ALL TRANSCRIBED from the preamp sheet:
//   "V1A (1/2 ECC83)  Rk 2k2 bypassed by 50uF ;  Ra 220k to D+"
//   "V1B (1/2 ECC83)  Rk 2k2 bypassed by 50uF ;  Ra 220k to D+"
// The 220k plate load (against the reconstruction's 100k) is a large part of why
// an OR120's preamp is both quieter per stage and softer-edged than a 2204's, and
// it is what makes V1A a genuinely high-Z source for the James stack.
void OrangePreamp::configureStages() {
    TriodeStage::Config& a = cfg_[V1A];
    a = TriodeStage::Config{};
    a.bPlus = dPlus_;
    a.Ra = 220.0e3;
    a.Rk = 2.2e3;
    a.Ck = 50.0e-6;
    a.Rg = 68.0e3;   // the input jacks' transcribed 68k grid stopper
    a.Cc = 68.0e-9;  // "V1A plate -> 68n -> TONE STACK input"
    // The load V1A's plate actually sees: the James network's own input
    // resistance at DC, computed from the transcribed netlist at noon —
    //   R1 + [(1-b)RB + ((b)RB + R3) || (R2 + RG)] = 100k + 853k = 953k.
    a.Rgl = 953.0e3;

    TriodeStage::Config& b = cfg_[V1B];
    b = TriodeStage::Config{};
    b.bPlus = dPlus_;
    b.Ra = 220.0e3;
    b.Rk = 2.2e3;
    b.Ck = 50.0e-6;
    b.Rg = 68.0e3;
    b.Cc = 68.0e-9;  // "V1B plate -> 68n -> F.A.C. rotary -> OUTPUT AMP"
    // The F.A.C. network's input resistance with the switch straight through:
    // 200k loop + 1M driver grid leak.
    b.Rgl = FacNetwork::kRloop + FacNetwork::kRgl;
}

// The transcribed dropper chain: C+ -> 33K 2W -> D+ (16uF) feeding V1A and V1B.
// D+ therefore depends on the two stages' standing current and vice versa, so it
// is a fixed point. Solved on the Koren law alone (cheap) BEFORE the stages are
// configured and prepared once — TriodeStage::prepare settles ~50k silent
// samples per call and cannot be run inside an iteration.
void OrangePreamp::solveSupply() {
    const TriodeStage::KorenParams tube{};
    // Bisect on D+ instead of iterating the map: h(D+) = C+ - 33k*2*Ip(D+) - D+ is
    // strictly decreasing (a higher rail draws more current, which drops more).
    auto h = [&](double d) {
        return cPlus_ - kRdropDplus * (2.0 * korenSelfBiasCurrent(d, 220.0e3, 2.2e3, tube)) - d;
    };
    double lo = 0.0, hi = cPlus_;
    for (int it = 0; it < 200; ++it) {
        const double m = 0.5 * (lo + hi);
        if (h(m) > 0.0)
            lo = m;
        else
            hi = m;
    }
    dPlus_ = 0.5 * (lo + hi);
}

void OrangePreamp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    buf_.assign(static_cast<size_t>(maxBlockSize_), 0.0f);

    solveSupply();
    configureStages();
    for (int s = V1A; s <= V1B; ++s) {
        stage_[s].configure(cfg_[s]);
        stage_[s].prepare(sampleRate_, maxBlockSize_);
        stage_[s].setOversampling(oversampling_);
    }

    // Both linear networks are driven from a PLATE, not a cathode follower — the
    // OR120 has no follower anywhere, and that high source impedance is part of
    // both networks' real response. rout = Ra || rp, with rp = dVa/dIp from the
    // Koren law at the stage's operating point (central difference).
    auto plateRout = [&](int s) {
        const TriodeStage::Config& c = cfg_[s];
        const double Va = stage_[s].quiescentPlateVoltage();
        const double Vk = stage_[s].quiescentCathodeVoltage();
        const double h = 1e-2;
        const double dIp = (TriodeStage::korenPlateCurrent(Va + h, -Vk, c.tube) -
                            TriodeStage::korenPlateCurrent(Va - h, -Vk, c.tube)) /
                           (2.0 * h);
        const double rp = dIp > 1e-12 ? 1.0 / dIp : 1.0e6;
        return 1.0 / (1.0 / c.Ra + 1.0 / rp);
    };
    plateRoutA_ = plateRout(V1A);
    plateRoutB_ = plateRout(V1B);

    tone_.prepare(sampleRate_);
    tone_.setSourceImpedance(plateRoutA_);
    tone_.setKnobs(bass_, treble_, volumeWiper());

    fac_.prepare(sampleRate_);
    fac_.setSourceImpedance(plateRoutB_);
    fac_.setPosition(static_cast<int>(
        std::floor(clampParam01(facKnob_) * (FacNetwork::kPositions - 1) + 0.5)));

    primed_ = false;
}

void OrangePreamp::primeSmoothers() { tone_.snapKnobs(); }

void OrangePreamp::reset() {
    for (auto& s : stage_) s.reset();
    tone_.reset();
    fac_.reset();
    lastOutPeak_ = 0.0;
}

void OrangePreamp::setOversampling(int factor) {
    oversampling_ = factor;
    for (auto& s : stage_) s.setOversampling(factor);
}

void OrangePreamp::setParameter(int paramId, float value) {
    const double v = clampParam01(static_cast<double>(value));
    switch (paramId) {
        case PARAM_VOLUME:
            volume_ = v;
            tone_.setKnobs(bass_, treble_, volumeWiper());
            break;
        case PARAM_BASS:
            bass_ = v;
            tone_.setKnobs(bass_, treble_, volumeWiper());
            break;
        case PARAM_TREBLE:
            treble_ = v;
            tone_.setKnobs(bass_, treble_, volumeWiper());
            break;
        case PARAM_FAC: {
            facKnob_ = v;
            // Six detents across the knob: nearest-position rounding, so 0 and 1
            // land exactly on the two ends of the switch.
            const int pos = static_cast<int>(
                std::floor(v * (FacNetwork::kPositions - 1) + 0.5));
            fac_.setPosition(pos);
            break;
        }
        default:
            break;
    }
}

double OrangePreamp::volumeWiper() const { return volumeTaper(volume_); }

void OrangePreamp::process(const float* in, float* out, int numFrames) {
    if (!primed_) {
        primeSmoothers();
        primed_ = true;
    }
    lastOutPeak_ = 0.0;
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        float* w = buf_.data();
        // THE SCHEMATIC ORDER (docs §57.2): V1A -> stack+GAIN+330p -> V1B -> F.A.C.
        // The GAIN pot lives inside the tone MNA, so there is no separate scalar
        // multiply here: the knob is a wiper on a 1M pot that also loads the
        // network, and per-sample smoothing happens inside the stack (finding 6).
        stage_[V1A].process(in + off, w, n);
        tone_.process(w, w, n);
        stage_[V1B].process(w, w, n);
        fac_.process(w, w, n);
        for (int i = 0; i < n; ++i) {
            out[off + i] = w[i];
            lastOutPeak_ =
                std::max(lastOutPeak_, std::fabs(static_cast<double>(w[i])));
        }
        off += n;
    }
}

double OrangePreamp::stageQuiescentPlate(int stage) const {
    return stage_[static_cast<size_t>(stage)].quiescentPlateVoltage();
}
double OrangePreamp::stageQuiescentCathode(int stage) const {
    return stage_[static_cast<size_t>(stage)].quiescentCathodeVoltage();
}
double OrangePreamp::stageQuiescentCurrent(int stage) const {
    return stage_[static_cast<size_t>(stage)].quiescentCurrent();
}

}  // namespace clipper::dsp
