// Clipper — OrangePreamp (M10.3, docs §57). See OrangePreamp.h for the topology,
// the "why this is not a re-skinned JCM800" argument and the PROVENANCE banner.
// This file is the numerics: the James/F.A.C. MNA, the per-stage TriodeStage
// configs, V1B's plate source impedance, and the block signal flow.

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

enum { IN = 0, F = 1, A = 2, B = 3, T = 4, U = 5, OUT = 6 };
}  // namespace

// ===========================================================================
// JamesToneStack
// ===========================================================================
void JamesToneStack::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    T_ = 1.0 / sampleRate_;
    geq1_ = 2.0 * kC1 / T_;
    geq2_ = 2.0 * kC2 / T_;
    geq3_ = 2.0 * kC3 / T_;
    geqF_ = 2.0 * kFacCaps[fac_] / T_;
    bassSm_.prepare(kKnobSmoothSeconds, sampleRate_);
    trebleSm_.prepare(kKnobSmoothSeconds, sampleRate_);
    reset();
}

void JamesToneStack::reset() {
    vF_ = iF_ = v1_ = i1_ = v2_ = i2_ = v3_ = i3_ = 0.0;
    snapKnobs();
}

void JamesToneStack::snapKnobs() {
    bassSm_.reset();
    trebleSm_.reset();
    bass_ = bassSm_.value();
    treble_ = trebleSm_.value();
    knobsMoving_ = false;
    ctrlCounter_ = 0;
    rebuild();
}

void JamesToneStack::setSourceImpedance(double rs) {
    rs_ = clampR(rs);
    dirty_ = true;
}

void JamesToneStack::setKnobs(double bass, double treble) {
    auto cl = [](double v) { return clampParam(v, 1.0e-3, 1.0 - 1.0e-3); };
    bassSm_.setTarget(cl(bass));
    trebleSm_.setTarget(cl(treble));
    ctrlCounter_ = 0;
    if (!(bassSm_.settled() && trebleSm_.settled())) knobsMoving_ = true;
}

// The F.A.C. is a SWITCH, not a knob: there is nothing to smooth between two cap
// values, and the real rotary steps too. The worklet/plugin bracket a switch
// position change with the chain declick, exactly like a topology change; the cap
// STATE is deliberately carried across so the network's stored charge is not
// discontinuous (which is also what the real switch does).
void JamesToneStack::setFacPosition(int pos) {
    const int p = pos < 0 ? 0 : (pos >= kFacPositions ? kFacPositions - 1 : pos);
    if (p == fac_) return;
    fac_ = p;
    geqF_ = 2.0 * kFacCaps[fac_] / T_;
    dirty_ = true;
}

void JamesToneStack::rebuild() {
    std::array<std::array<double, N>, N> G{};
    auto stamp = [&](int a, int b, double g) {
        G[a][a] += g;
        G[b][b] += g;
        G[a][b] -= g;
        G[b][a] -= g;
    };
    G[IN][IN] += 1.0 / rs_;                                  // source conductance
    stamp(IN, F, geqF_);                                     // F.A.C. series cap
    stamp(F, A, 1.0 / kR1);                                  // bass series R
    stamp(A, OUT, 1.0 / clampR((1.0 - bass_) * kRB));        // bass pot upper
    stamp(OUT, B, 1.0 / clampR(bass_ * kRB));                // bass pot lower
    stamp(A, B, geq1_);                                      // C1 across the track
    G[B][B] += 1.0 / kR3;                                    // bass shunt R to gnd
    stamp(F, T, geq2_);                                      // treble series cap
    stamp(T, OUT, 1.0 / clampR((1.0 - treble_) * kRT));      // treble pot upper
    stamp(OUT, U, 1.0 / clampR(treble_ * kRT));              // treble pot lower
    G[U][U] += geq3_;                                        // C3 to gnd
    G[OUT][OUT] += 1.0 / kRL;                                // next grid leak

    std::array<std::array<double, N>, N> Amat = G;
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
    Ginv_ = I;
    dirty_ = false;
}

void JamesToneStack::process(const float* in, float* out, int numFrames) {
    if (dirty_) rebuild();
    const double gs = 1.0 / rs_;
    for (int n = 0; n < numFrames; ++n) {
        if (knobsMoving_) {
            const double b = bassSm_.next();
            const double t = trebleSm_.next();
            if (ctrlCounter_ == 0) {
                if (b != bass_ || t != treble_) {
                    bass_ = b;
                    treble_ = t;
                    rebuild();
                }
                if (bassSm_.settled() && trebleSm_.settled()) knobsMoving_ = false;
            }
            if (++ctrlCounter_ >= kCtrlBlock) ctrlCounter_ = 0;
        }
        const double Vs = static_cast<double>(in[n]);
        const double IeqF = geqF_ * vF_ + iF_;
        const double Ieq1 = geq1_ * v1_ + i1_;
        const double Ieq2 = geq2_ * v2_ + i2_;
        const double Ieq3 = geq3_ * v3_ + i3_;
        std::array<double, N> b{};
        b[IN] = gs * Vs + IeqF;
        b[F] = -IeqF + Ieq2;
        b[A] = Ieq1;
        b[B] = -Ieq1;
        b[T] = -Ieq2;
        b[U] = Ieq3;
        b[OUT] = 0.0;
        std::array<double, N> v{};
        for (int r = 0; r < N; ++r) {
            double acc = 0.0;
            for (int cc = 0; cc < N; ++cc) acc += Ginv_[r][cc] * b[cc];
            v[r] = acc;
        }
        const double vFn = v[IN] - v[F];
        const double v1n = v[A] - v[B];
        const double v2n = v[F] - v[T];
        const double v3n = v[U];
        // Anti-denormal (Denormal.h, docs §33): all eight companions REST AT ZERO,
        // so a silent tail rings them into double subnormals and they stick — the
        // same 68x cliff MarshallToneStack measured. Invisible in the float output,
        // which is exactly why it has to be guarded rather than observed.
        iF_ = flushDenormal(geqF_ * vFn - IeqF);
        i1_ = flushDenormal(geq1_ * v1n - Ieq1);
        i2_ = flushDenormal(geq2_ * v2n - Ieq2);
        i3_ = flushDenormal(geq3_ * v3n - Ieq3);
        vF_ = flushDenormal(vFn);
        v1_ = flushDenormal(v1n);
        v2_ = flushDenormal(v2n);
        v3_ = flushDenormal(v3n);
        out[n] = static_cast<float>(v[OUT]);
    }
}

// Steady-state |H(jw)| straight off the NETLIST (complex nodal solve). Used by the
// discretization check and by the §57 mid-forward metric. Deliberately written
// from the component constants rather than from the discretized matrix, so it is
// an independent evaluation of the same netlist — the honest limit of that (it
// cannot catch a wrong topology) is recorded in docs §57.
double JamesToneStack::magnitudeAt(double freqHz, double bass, double treble,
                                   double rs, int facPos) {
    using C = std::complex<double>;
    const int p = facPos < 0 ? 0 : (facPos >= kFacPositions ? kFacPositions - 1 : facPos);
    const C jw(0.0, 2.0 * 3.14159265358979323846 * freqHz);
    C G[N][N];
    for (auto& row : G)
        for (auto& e : row) e = C(0.0, 0.0);
    auto stamp = [&](int a, int b, C g) {
        G[a][a] += g;
        G[b][b] += g;
        G[a][b] -= g;
        G[b][a] -= g;
    };
    const double rsc = rs > 1.0 ? rs : 1.0;
    G[IN][IN] += C(1.0 / rsc, 0.0);
    stamp(IN, F, jw * kFacCaps[p]);
    stamp(F, A, C(1.0 / kR1, 0.0));
    stamp(A, OUT, C(1.0 / clampR((1.0 - bass) * kRB), 0.0));
    stamp(OUT, B, C(1.0 / clampR(bass * kRB), 0.0));
    stamp(A, B, jw * kC1);
    G[B][B] += C(1.0 / kR3, 0.0);
    stamp(F, T, jw * kC2);
    stamp(T, OUT, C(1.0 / clampR((1.0 - treble) * kRT), 0.0));
    stamp(OUT, U, C(1.0 / clampR(treble * kRT), 0.0));
    G[U][U] += jw * kC3;
    G[OUT][OUT] += C(1.0 / kRL, 0.0);

    C M[N][N + 1];
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) M[r][c] = G[r][c];
        M[r][N] = C(0.0, 0.0);
    }
    M[IN][N] = C(1.0 / rsc, 0.0);  // unit source behind rs
    for (int c = 0; c < N; ++c) {
        int piv = c;
        for (int r = c + 1; r < N; ++r)
            if (std::abs(M[r][c]) > std::abs(M[piv][c])) piv = r;
        for (int j = 0; j <= N; ++j) std::swap(M[piv][j], M[c][j]);
        const C d = M[c][c];
        if (std::abs(d) < 1e-300) return 0.0;
        for (int j = c; j <= N; ++j) M[c][j] /= d;
        for (int r = 0; r < N; ++r) {
            if (r == c) continue;
            const C f = M[r][c];
            if (std::abs(f) == 0.0) continue;
            for (int j = c; j <= N; ++j) M[r][j] -= f * M[c][j];
        }
    }
    return std::abs(M[OUT][N]);
}

// ===========================================================================
// OrangePreamp
// ===========================================================================
double OrangePreamp::volumeTaper(double x) {
    x = clampParam01(x);
    return (std::exp(kVolumeTaperK * x) - 1.0) / (std::exp(kVolumeTaperK) - 1.0);
}

OrangePreamp::OrangePreamp() { configureStages(); }

// Per-stage circuit values. Documented reconstruction on standard British ECC83
// practice — see the PROVENANCE banner in the header.
void OrangePreamp::configureStages() {
    // V1A: the input stage. Ra 100 k, Rk 820, and — unlike the 2204's 0.68 uF
    // partial bypass — a FULL 25 uF bypass, so the bottom is not thinned at the
    // very first stage. Rgl = the volume network (470 k series + 1 M pot).
    TriodeStage::Config& a = cfg_[V1A];
    a = TriodeStage::Config{};
    a.Ra = 100.0e3;
    a.Rk = 820.0;
    a.Ck = 25.0e-6;
    a.Rgl = 1.47e6;

    // V1B: the second half of the same ECC83, after the volume pot. Also fully
    // bypassed (the OR120 has no cold clipper stage — the 2204's 10 k unbypassed
    // V1B is a Marshall trait and is a large part of why a JCM is tighter).
    // Rgl = the tone stack's input resistance seen through the F.A.C. cap.
    TriodeStage::Config& b = cfg_[V1B];
    b = TriodeStage::Config{};
    b.Ra = 100.0e3;
    b.Rk = 820.0;
    b.Ck = 25.0e-6;
    b.Rgl = 470.0e3;
}

void OrangePreamp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    buf_.assign(static_cast<size_t>(maxBlockSize_), 0.0f);

    configureStages();
    for (int s = V1A; s <= V1B; ++s) {
        stage_[s].configure(cfg_[s]);
        stage_[s].prepare(sampleRate_, maxBlockSize_);
        stage_[s].setOversampling(oversampling_);
    }

    // The James stack is driven from V1B's PLATE, not from a cathode follower —
    // the OR120 has no follower, and that high source impedance is part of the
    // network's real response (it loads the two branches and softens the treble
    // corner). rout = Ra || rp, with rp = dVa/dIp from the Koren law at V1B's
    // operating point (central difference, the same method the JCM uses for its
    // follower's gm).
    {
        const TriodeStage::Config& c = cfg_[V1B];
        const double Va = stage_[V1B].quiescentPlateVoltage();
        const double Vk = stage_[V1B].quiescentCathodeVoltage();
        const double Vgk = -Vk;
        const double h = 1e-2;
        const double dIp = (TriodeStage::korenPlateCurrent(Va + h, Vgk, c.tube) -
                            TriodeStage::korenPlateCurrent(Va - h, Vgk, c.tube)) /
                           (2.0 * h);
        const double rp = dIp > 1e-12 ? 1.0 / dIp : 1.0e6;
        plateRout_ = 1.0 / (1.0 / c.Ra + 1.0 / rp);
    }

    tone_.prepare(sampleRate_);
    tone_.setSourceImpedance(plateRout_);
    tone_.setKnobs(bass_, treble_);
    tone_.setFacPosition(static_cast<int>(
        std::floor(clampParam01(fac_) * (JamesToneStack::kFacPositions - 1) + 0.5)));

    volSm_.prepare(kSmoothSeconds, sampleRate_);
    volSm_.setTarget(volumeScale());
    primed_ = false;
}

void OrangePreamp::primeSmoothers() {
    volSm_.reset();
    tone_.snapKnobs();
}

void OrangePreamp::reset() {
    for (auto& s : stage_) s.reset();
    tone_.reset();
    volSm_.reset();
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
            volSm_.setTarget(volumeScale());
            break;
        case PARAM_BASS:
            bass_ = v;
            tone_.setKnobs(bass_, treble_);
            break;
        case PARAM_TREBLE:
            treble_ = v;
            tone_.setKnobs(bass_, treble_);
            break;
        case PARAM_FAC: {
            fac_ = v;
            // Six detents across the knob: nearest-position rounding, so 0 and 1
            // land exactly on the two ends of the switch.
            const int pos = static_cast<int>(
                std::floor(v * (JamesToneStack::kFacPositions - 1) + 0.5));
            tone_.setFacPosition(pos);
            break;
        }
        default:
            break;
    }
}

double OrangePreamp::volumeScale() const {
    return kVolumeDivider * volumeTaper(volume_);
}

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
        stage_[V1A].process(in + off, w, n);
        // VOLUME: a plain smoothed scalar applied PER SAMPLE (audit finding 6).
        // There is no bright cap to make this frequency-dependent — see the
        // header; that absence is a modelled feature, not an omission.
        for (int i = 0; i < n; ++i) w[i] = static_cast<float>(w[i] * volSm_.next());
        stage_[V1B].process(w, w, n);
        // F.A.C. + James stack, one network, at base rate (both linear).
        tone_.process(w, w, n);
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
