// Clipper — TwinPreamp (M10.1). See TwinPreamp.h for the AB763 vibrato-channel
// topology, the pre-gain Fender tone stack, and the Marshall-contrast rationale.
// This file is the wiring: per-stage TriodeStage::Config, the Fender TMB MNA, the
// volume + bright network, and the block signal flow.

#include "clipper/dsp/TwinPreamp.h"

#include <algorithm>
#include <cmath>

namespace clipper::dsp {

// ===========================================================================
// FenderToneStack — trapezoidal-companion nodal MNA of the FMV network (Fender
// values). Identical numerics to MarshallToneStack; only the component constants
// and the high-Z plate source differ.
// ===========================================================================
namespace {
inline double clampR(double r) { return r < 1.0 ? 1.0 : r; }
}  // namespace

void FenderToneStack::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    T_ = 1.0 / sampleRate_;
    geqT_ = 2.0 * kCt / T_;
    geqB_ = 2.0 * kCb / T_;
    geqM_ = 2.0 * kCm / T_;
    reset();
    dirty_ = true;
    rebuild();
}

void FenderToneStack::reset() { vT_ = iT_ = vB_ = iB_ = vM_ = iM_ = 0.0; }

void FenderToneStack::setSourceImpedance(double rs) {
    rs_ = clampR(rs);
    dirty_ = true;
}

void FenderToneStack::setKnobs(double bass, double mid, double treble) {
    auto cl = [](double v) { return std::clamp(v, 1.0e-3, 1.0 - 1.0e-3); };
    bass_ = cl(bass);
    mid_ = cl(mid);
    treble_ = cl(treble);
    dirty_ = true;
}

void FenderToneStack::rebuild() {
    enum { IN = 0, N2 = 1, N3 = 2, N4 = 3, OUT = 4 };
    std::array<std::array<double, N>, N> G{};
    auto stamp = [&](int a, int b, double g) {
        G[a][a] += g;
        G[b][b] += g;
        G[a][b] -= g;
        G[b][a] -= g;
    };
    const double gs = 1.0 / rs_;
    G[IN][IN] += gs;                                      // source conductance
    stamp(IN, N2, geqT_);                                // treble cap Ct
    stamp(IN, N3, 1.0 / kRslope);                        // slope resistor
    stamp(N2, OUT, 1.0 / clampR((1.0 - treble_) * kRT)); // treble pot upper
    stamp(OUT, N3, 1.0 / clampR(treble_ * kRT));         // treble pot lower
    stamp(N3, N4, 1.0 / clampR(bass_ * kRB));            // bass pot rheostat
    stamp(N3, N4, geqB_);                                // bass cap Cb
    G[N4][N4] += 1.0 / clampR(mid_ * kRM);               // mid pot rheostat to gnd
    G[N4][N4] += geqM_;                                  // mid cap Cm to gnd

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

void FenderToneStack::process(const float* in, float* out, int numFrames) {
    if (dirty_) rebuild();
    enum { IN = 0, N2 = 1, N3 = 2, N4 = 3, OUT = 4 };
    const double gs = 1.0 / rs_;
    for (int n = 0; n < numFrames; ++n) {
        const double Vs = static_cast<double>(in[n]);
        const double IeqT = geqT_ * vT_ + iT_;
        const double IeqB = geqB_ * vB_ + iB_;
        const double IeqM = geqM_ * vM_ + iM_;
        std::array<double, N> b{};
        b[IN] = gs * Vs + IeqT;
        b[N2] = -IeqT;
        b[N3] = IeqB;
        b[N4] = -IeqB + IeqM;
        b[OUT] = 0.0;
        std::array<double, N> v{};
        for (int r = 0; r < N; ++r) {
            double acc = 0.0;
            for (int cc = 0; cc < N; ++cc) acc += Ginv_[r][cc] * b[cc];
            v[r] = acc;
        }
        const double vTn = v[IN] - v[N2];
        const double vBn = v[N3] - v[N4];
        const double vMn = v[N4];
        iT_ = geqT_ * vTn - IeqT;
        iB_ = geqB_ * vBn - IeqB;
        iM_ = geqM_ * vMn - IeqM;
        vT_ = vTn;
        vB_ = vBn;
        vM_ = vMn;
        out[n] = static_cast<float>(v[OUT]);
    }
}

// ===========================================================================
// TwinPreamp
// ===========================================================================

double TwinPreamp::audioTaper(double x) {
    x = std::clamp(x, 0.0, 1.0);
    constexpr double k = 4.0;  // ~12% at noon (a musical audio/log taper)
    return (std::exp(k * x) - 1.0) / (std::exp(k) - 1.0);
}

TwinPreamp::TwinPreamp() { configureStages(); }

void TwinPreamp::configureStages() {
    // V1: warm blackface input stage. Ra 100k, Rk 1.5k bypassed by 25 uF, B+ 410 V.
    // Rgl = the plate load seen through the coupling toward the tone stack (~1 M
    // grid-leak of the tone-stack input network).
    TriodeStage::Config& a = cfg_[V1];
    a = TriodeStage::Config{};
    a.bPlus = 410.0; a.Ra = 100.0e3; a.Rk = 1.5e3; a.Ck = 25.0e-6; a.Rgl = 1.0e6;

    // V2: recovery stage, identical warm biasing. Rgl = 1 M (its own grid leak).
    TriodeStage::Config& b = cfg_[V2];
    b = TriodeStage::Config{};
    b.bPlus = 410.0; b.Ra = 100.0e3; b.Rk = 1.5e3; b.Ck = 25.0e-6; b.Rgl = 1.0e6;
}

void TwinPreamp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    buf_.assign(static_cast<size_t>(maxBlockSize_), 0.0f);

    configureStages();
    for (int s = V1; s <= V2; ++s) {
        stage_[s].configure(cfg_[s]);
        stage_[s].prepare(sampleRate_, maxBlockSize_);
        stage_[s].setOversampling(oversampling_);
    }

    // Fender stack source impedance = V1 plate output impedance Ra||rp (a HIGH-Z
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
    tone_.setKnobs(bass_, mid_, treble_);

    // Bright treble-bleed one-pole (TPT high-pass coefficient at kBrightHz).
    const double g = std::tan(M_PI * kBrightHz / sampleRate_);
    brightA_ = g / (1.0 + g);
    brightS_ = 0.0;
}

void TwinPreamp::setOversampling(int factor) {
    oversampling_ = factor;
    for (auto& s : stage_) s.setOversampling(factor);
}

void TwinPreamp::setParameter(int paramId, float value) {
    const double v = std::clamp(static_cast<double>(value), 0.0, 1.0);
    switch (paramId) {
        case PARAM_VOLUME: volume_ = v; break;
        case PARAM_BASS: bass_ = v; tone_.setKnobs(bass_, mid_, treble_); break;
        case PARAM_MID: mid_ = v; tone_.setKnobs(bass_, mid_, treble_); break;
        case PARAM_TREBLE: treble_ = v; tone_.setKnobs(bass_, mid_, treble_); break;
        case PARAM_BRIGHT: bright_ = value >= 0.5f; break;
        default: break;
    }
}

double TwinPreamp::volumeScale() const { return audioTaper(volume_); }

void TwinPreamp::process(const float* in, float* out, int numFrames) {
    const double vScale = volumeScale();
    // Bright treble-bleed gain rises as volume falls (cap around the pot). Off = 0.
    const double brightExtra = bright_ ? kBrightMax * (1.0 - vScale) : 0.0;
    lastOutPeak_ = 0.0;
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        float* w = buf_.data();
        // V1 gain stage.
        stage_[V1].process(in + off, w, n);
        for (int i = 0; i < n; ++i) w[i] = static_cast<float>(w[i] * kV1ToStack);
        // Fender TMB tone stack (pre-gain, base rate).
        tone_.process(w, w, n);
        // V2 recovery stage.
        stage_[V2].process(w, w, n);
        // VOLUME + BRIGHT treble-bleed.
        for (int i = 0; i < n; ++i) {
            double x = static_cast<double>(w[i]) * vScale;
            if (brightExtra > 0.0) {
                // One-pole high-pass -> add scaled highs (treble bleed).
                const double lp = brightS_ + brightA_ * (x - brightS_);
                brightS_ = 2.0 * lp - brightS_;
                const double hp = x - lp;
                x += brightExtra * hp;
            }
            const float o = static_cast<float>(x);
            out[off + i] = o;
            lastOutPeak_ = std::max(lastOutPeak_, std::fabs(static_cast<double>(o)));
        }
        off += n;
    }
}

double TwinPreamp::stageQuiescentPlate(int stage) const {
    return stage_[static_cast<size_t>(stage)].quiescentPlateVoltage();
}
double TwinPreamp::stageQuiescentCathode(int stage) const {
    return stage_[static_cast<size_t>(stage)].quiescentCathodeVoltage();
}
double TwinPreamp::stageQuiescentCurrent(int stage) const {
    return stage_[static_cast<size_t>(stage)].quiescentCurrent();
}

}  // namespace clipper::dsp
