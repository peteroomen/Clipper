// Clipper — Ac30Preamp (M10.2). See Ac30Preamp.h for the AC30 top-boost topology,
// the interactive treble/bass network, and the Marshall/Fender contrast. This file
// is the wiring: the single 12AX7 TriodeStage::Config, the Vox top-boost tone-stack
// MNA (with a SERIES input coupling cap), the volume network, and the block flow.

#include "clipper/dsp/Ac30Preamp.h"

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/ParamGuard.h"

#include <algorithm>
#include <cmath>

namespace clipper::dsp {

// ===========================================================================
// TopBoostToneStack — trapezoidal-companion nodal MNA of the Vox top-boost network.
// Nodes: SRC=0, IN=1, N2=2, OUT=3, N4=4. The input coupling cap Cc is IN SERIES
// (SRC-IN), the treble cap Ct bypasses the slope (IN-N2), the wiper OUT taps the
// treble pot between N2 and N4, and RB || Cb form the bass network at N4.
// ===========================================================================
namespace {
// NaN-safe resistance floor (the ParamGuard.h inversion; identical for finite r).
inline double clampR(double r) { return r > 1.0 ? r : 1.0; }
}  // namespace

void TopBoostToneStack::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    T_ = 1.0 / sampleRate_;
    geqC_ = 2.0 * kCc / T_;
    geqT_ = 2.0 * kCt / T_;
    geqB_ = 2.0 * kCb / T_;
    reset();
    dirty_ = true;
    rebuild();
}

void TopBoostToneStack::reset() { vC_ = iC_ = vT_ = iT_ = vB_ = iB_ = 0.0; }

void TopBoostToneStack::setSourceImpedance(double rs) {
    rs_ = clampR(rs);
    dirty_ = true;
}

void TopBoostToneStack::setKnobs(double bass, double treble) {
    // NaN-rejecting (std::clamp is transparent to NaN — audit finding 1).
    auto cl = [](double v) { return clampParam(v, 1.0e-3, 1.0 - 1.0e-3); };
    bass_ = cl(bass);
    treble_ = cl(treble);
    dirty_ = true;
}

void TopBoostToneStack::rebuild() {
    enum { SRC = 0, IN = 1, N2 = 2, OUT = 3, N4 = 4 };
    std::array<std::array<double, N>, N> G{};
    auto stamp = [&](int a, int b, double g) {
        G[a][a] += g;
        G[b][b] += g;
        G[a][b] -= g;
        G[b][a] -= g;
    };
    const double gs = 1.0 / rs_;
    G[SRC][SRC] += gs;                                    // source conductance
    stamp(SRC, IN, geqC_);                               // series coupling cap Cc
    stamp(IN, N2, geqT_);                                // treble cap Ct
    stamp(IN, N4, 1.0 / kRslope);                        // slope resistor R1
    stamp(N2, OUT, 1.0 / clampR((1.0 - treble_) * kRT)); // treble pot upper
    stamp(OUT, N4, 1.0 / clampR(treble_ * kRT));         // treble pot lower
    G[N4][N4] += 1.0 / clampR(bass_ * kRB);              // bass pot rheostat to gnd
    G[N4][N4] += geqB_;                                  // bass cap Cb to gnd

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

void TopBoostToneStack::process(const float* in, float* out, int numFrames) {
    if (dirty_) rebuild();
    enum { SRC = 0, IN = 1, N2 = 2, OUT = 3, N4 = 4 };
    const double gs = 1.0 / rs_;
    for (int n = 0; n < numFrames; ++n) {
        const double Vs = static_cast<double>(in[n]);
        const double IeqC = geqC_ * vC_ + iC_;
        const double IeqT = geqT_ * vT_ + iT_;
        const double IeqB = geqB_ * vB_ + iB_;
        std::array<double, N> b{};
        b[SRC] = gs * Vs + IeqC;
        b[IN] = -IeqC + IeqT;
        b[N2] = -IeqT;
        b[OUT] = 0.0;
        b[N4] = IeqB;
        std::array<double, N> v{};
        for (int r = 0; r < N; ++r) {
            double acc = 0.0;
            for (int cc = 0; cc < N; ++cc) acc += Ginv_[r][cc] * b[cc];
            v[r] = acc;
        }
        const double vCn = v[SRC] - v[IN];
        const double vTn = v[IN] - v[N2];
        const double vBn = v[N4];
        // Anti-denormal (Denormal.h). Six trapezoidal cap companions, all resting at
        // zero — including the vC_/iC_ pair for the 0.022 uF input coupling cap, which
        // the audit's list of unguarded states missed because the other two stacks have
        // no series input cap to have a companion for. This stack happens to measure a
        // clean 1.00x at knobs-at-noon (its poles decay fast enough to underflow
        // straight to zero rather than dwelling in the subnormal band, unlike the
        // Fender's 18.6x and the Marshall's 68.2x), but the pole positions MOVE WITH
        // THE KNOBS, so "it does not bite at noon" is not a reason to leave the class
        // of bug in. Audit finding 11, docs §33.
        iC_ = flushDenormal(geqC_ * vCn - IeqC);
        iT_ = flushDenormal(geqT_ * vTn - IeqT);
        iB_ = flushDenormal(geqB_ * vBn - IeqB);
        vC_ = flushDenormal(vCn);
        vT_ = flushDenormal(vTn);
        vB_ = flushDenormal(vBn);
        out[n] = static_cast<float>(v[OUT]);
    }
}

// ===========================================================================
// Ac30Preamp
// ===========================================================================

double Ac30Preamp::audioTaper(double x) {
    x = clampParam01(x);  // NaN-rejecting (ParamGuard.h)
    constexpr double k = 4.0;  // ~12% at noon (a musical audio/log taper)
    return (std::exp(k * x) - 1.0) / (std::exp(k) - 1.0);
}

Ac30Preamp::Ac30Preamp() { configureStages(); }

void Ac30Preamp::configureStages() {
    // V1: warm top-boost input stage. Ra 100k, Rk 1.5k bypassed by 25 uF, B+ 300 V.
    // Rgl = the grid-leak of the tone-stack input network seen through the coupling.
    TriodeStage::Config& a = cfg_[V1];
    a = TriodeStage::Config{};
    a.bPlus = 300.0; a.Ra = 100.0e3; a.Rk = 1.5e3; a.Ck = 25.0e-6; a.Rgl = 1.0e6;
}

void Ac30Preamp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    buf_.assign(static_cast<size_t>(maxBlockSize_), 0.0f);

    configureStages();
    stage_[V1].configure(cfg_[V1]);
    stage_[V1].prepare(sampleRate_, maxBlockSize_);
    stage_[V1].setOversampling(oversampling_);

    // Top-boost stack source impedance = V1 plate output impedance Ra||rp (a HIGH-Z
    // plate source, no cathode follower). rp from the Koren small-signal at V1's
    // operating point (central difference).
    {
        const double Va = stage_[V1].quiescentPlateVoltage();
        const double Vk = stage_[V1].quiescentCathodeVoltage();
        const double Vgk = -Vk;  // grid at 0 V DC, cathode at Vk
        const double h = 0.5;
        const double ipHi = TriodeStage::korenPlateCurrent(Va + h, Vgk, cfg_[V1].tube);
        const double ipLo = TriodeStage::korenPlateCurrent(Va - h, Vgk, cfg_[V1].tube);
        const double rp = (2.0 * h) / std::max(ipHi - ipLo, 1e-12);
        toneRs_ = 1.0 / (1.0 / cfg_[V1].Ra + 1.0 / rp);  // Ra || rp
    }

    tone_.prepare(sampleRate_);
    tone_.setSourceImpedance(toneRs_);
    tone_.setKnobs(bass_, treble_);
}

void Ac30Preamp::reset() {
    for (auto& s : stage_) s.reset();
    tone_.reset();
    lastOutPeak_ = 0.0;
}

void Ac30Preamp::setOversampling(int factor) {
    oversampling_ = factor;
    for (auto& s : stage_) s.setOversampling(factor);
}

void Ac30Preamp::setParameter(int paramId, float value) {
    // NaN-rejecting (ParamGuard.h) — audit finding 1.
    const double v = clampParam01(static_cast<double>(value));
    switch (paramId) {
        case PARAM_VOLUME: volume_ = v; break;
        case PARAM_BASS: bass_ = v; tone_.setKnobs(bass_, treble_); break;
        case PARAM_TREBLE: treble_ = v; tone_.setKnobs(bass_, treble_); break;
        default: break;
    }
}

double Ac30Preamp::volumeScale() const { return audioTaper(volume_); }

void Ac30Preamp::process(const float* in, float* out, int numFrames) {
    const double vScale = volumeScale();
    lastOutPeak_ = 0.0;
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        float* w = buf_.data();
        // V1 gain stage.
        stage_[V1].process(in + off, w, n);
        for (int i = 0; i < n; ++i) w[i] = static_cast<float>(w[i] * kV1ToStack);
        // Top-boost tone stack (interactive treble/bass, base rate).
        tone_.process(w, w, n);
        // VOLUME.
        for (int i = 0; i < n; ++i) {
            const float o = static_cast<float>(static_cast<double>(w[i]) * vScale);
            out[off + i] = o;
            lastOutPeak_ = std::max(lastOutPeak_, std::fabs(static_cast<double>(o)));
        }
        off += n;
    }
}

double Ac30Preamp::stageQuiescentPlate(int stage) const {
    return stage_[static_cast<size_t>(stage)].quiescentPlateVoltage();
}
double Ac30Preamp::stageQuiescentCathode(int stage) const {
    return stage_[static_cast<size_t>(stage)].quiescentCathodeVoltage();
}
double Ac30Preamp::stageQuiescentCurrent(int stage) const {
    return stage_[static_cast<size_t>(stage)].quiescentCurrent();
}

}  // namespace clipper::dsp
