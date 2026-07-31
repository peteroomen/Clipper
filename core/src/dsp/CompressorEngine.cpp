// Clipper — portable DSP core.
//
// CompressorEngine implementation. The circuit, the sources and every derived
// number live in docs §59; this file carries the discretization and the solver.
//
// Numerical notes worth reading before touching anything here:
//
//  * The DRIVE NETWORK ships as a cascade of two FIRST-ORDER sections, not a
//    biquad. Its poles are 9.29 Hz and 9105 Hz — a 980:1 spread — and at 192 kHz
//    the slow one sits at radius 0.99970. Docs §56 measured exactly that shape
//    emitting audible broadband hiss out of a `float` direct form, and §56.4b
//    showed the house `flushDenormal` one-liner cannot converge a direct form of
//    order >= 2 at all. Two first-order sections in `double` avoid both.
//
//  * The SIDECHAIN node solve is exact, not a lookup. Eliminating the coupling
//    cap's charge leaves ONE scalar unknown per leg:
//
//        g(vb) = vb − vDrive + qPrev + (Rsrc + T/C)·f(vb) = 0
//        f(vb) = vb/R + Ib(vb) + Ic(vb) − Idiode(vb)
//
//    f' > 0 everywhere, so g' = 1 + (Rsrc + T/C)·f' > 0 and g is STRICTLY
//    MONOTONE: Newton cannot rotate off a descent direction the way §53 found
//    `BjtStage`'s clamped step doing. A step limiter is still there as a guard
//    against the first exponential overshoot, and there is a residual early-out
//    in AMPS (docs §34's lesson: without one, a parked node burns the whole
//    iteration budget reproducing an answer it already has).
//
//  * The ENVELOPE node (C9) is the one state that does NOT get a denormal flush.
//    It rests at 8.5786 V — a real DC operating point — and per ADR 006 a guard
//    there is unreachable code in a hot loop. Measured, not assumed: see
//    `clipper_denormal_tests`, which asserts `maxAbsRestingState() == 0.0` for
//    everything that DOES rest at zero and would fail if this one were added.

#include "clipper/dsp/CompressorEngine.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/ParamGuard.h"

namespace clipper::dsp {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Newton budget for one sidechain node. The solve is monotone (see the banner),
// so this is a guard rail, not a working iteration count: the shipped 4x / 48 kHz
// path measures 2.0 evaluations per solve at idle and 4 at a hard transient.
constexpr int kMaxNewtonIter = 24;
// Residual tolerance as a CURRENT (amps). The node's own resistive leg carries
// ~2.5e-7 A at a typical excursion, so 1e-14 A is seven decades inside it — and
// docs §34's finding is that a solver with NO residual exit burns its whole
// budget at the parked operating point for nothing.
constexpr double kNewtonResidualTolA = 1e-14;
// Largest Newton step (volts). One exponential overshoot on the first iteration
// is otherwise enough to push exp() to infinity.
constexpr double kNewtonMaxStepV = 0.25;
// The gain cell's control current can never be negative (a rheostat and a
// junction cannot source current backwards into the bias pin).
constexpr double kMinControlAmps = 0.0;

// Bilinear one-pole section for H(s) = (s + wz) / (s + wp).
void bilinearSection(double wz, double wp, double fs, double& b0, double& b1,
                     double& a1) {
    const double k = 2.0 * fs;
    const double d = k + wp;
    b0 = (k + wz) / d;
    b1 = (wz - k) / d;
    a1 = (wp - k) / d;
}

}  // namespace

// --- Device helpers ----------------------------------------------------------
//
// Ebers-Moll forward collector current and the Gummel-Poon base current with the
// low-current (ISE/NE) term. The ISE term is why this file does not carry a
// hand-picked hFE: the published card reproduces the datasheet's own ~106 at
// 0.2 mA on its own.
namespace {

inline double npnIc(double vbe, const NpnDevice& d) {
    return d.Is * (std::exp(vbe / d.Vt) - 1.0);
}

inline double npnIb(double vbe, const NpnDevice& d) {
    return (d.Is / d.betaF) * (std::exp(vbe / d.Vt) - 1.0) +
           d.Ise * (std::exp(vbe / (d.Ne * d.Vt)) - 1.0);
}

// d(Ib)/d(vbe). ONLY the base current appears in the clamp node's KCL — the
// collector current flows from the envelope node to the emitter and never
// touches the base. Getting that wrong makes the node ~beta times too stiff,
// which starves the discharge and turns a 5 ms attack into half a second.
inline double npnBaseCond(double vbe, const NpnDevice& d) {
    const double e1 = std::exp(vbe / d.Vt);
    const double e2 = std::exp(vbe / (d.Ne * d.Vt));
    return (d.Is / d.betaF) * e1 / d.Vt + d.Ise * e2 / (d.Ne * d.Vt);
}

// The clamp diode: anode at GROUND, cathode at the node. Positive return value
// means current flowing INTO the node (which is what pulls a negative excursion
// back up toward one diode drop below ground).
inline double clampDiodeIn(double vNode, const DiodeDevice& d) {
    return d.Is * (std::exp(-vNode / d.nVt) - 1.0);
}

inline double clampDiodeCond(double vNode, const DiodeDevice& d) {
    return d.Is * std::exp(-vNode / d.nVt) / d.nVt;
}

}  // namespace

// --- Lifecycle ---------------------------------------------------------------

void CompressorEngine::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;

    os_.prepare(maxBlockSize_);
    os_.setFactor(osFactor_);

    // Knob smoothing: the house ~6 ms, applied per sample (never per block).
    sustainWiper_.prepare(0.006, sampleRate_);
    level_.prepare(0.006, sampleRate_);
    // Construction defaults mirror the shipped knob defaults (web
    // COMP_KNOB_DEFAULTS / the native APVTS), so a caller that never writes a
    // parameter still gets the pedal's opening voice rather than an arbitrary
    // one. Note the Muff trap docs §34 records — a model whose LEVEL smoother
    // defaults to 0 renders digital silence and makes a whole test sweep
    // vacuous. 0.4 is audible and is unity at a normal playing level.
    sustainWiper_.setImmediate(0.5f);
    level_.setImmediate(0.4f);

    compliance_.setKnees(cfg_.load.swingUp, cfg_.load.swingDown);

    rebuildRates();
    solveQuiescent();
    reset();
    snapPending_ = true;
}

void CompressorEngine::rebuildRates() {
    osRate_ = sampleRate_ * static_cast<double>(os_.factor());

    // Base-rate coupling high-passes: y = R*(y1 + x - x1).
    inHpR_ = std::exp(-2.0 * kPi * cfg_.in.couplingHz / sampleRate_);
    outHpR_ = std::exp(-2.0 * kPi * cfg_.out.couplingHz / sampleRate_);

    // Drive network: two first-order sections at the OVERSAMPLED rate. The whole
    // network's HF gain rides on section 0.
    for (int i = 0; i < 2; ++i) {
        bilinearSection(2.0 * kPi * cfg_.drive.zeroHz[i],
                        2.0 * kPi * cfg_.drive.poleHz[i], osRate_, drvB0_[i],
                        drvB1_[i], drvA1_[i]);
    }
    drvScale_ = cfg_.drive.hfGain;

    // Gain-cell load pole (one-pole low-pass at the oversampled rate).
    loadCoef_ = 1.0 - std::exp(-2.0 * kPi * cfg_.load.poleHz / osRate_);
}

// Bounded scalar fixed point for the QUIESCENT envelope node: with no signal the
// detector transistors are off, so C9 charges through R18 and the only load is
// Q5's base current. Converges in a handful of logs — this is nothing like the
// ~50 k silent samples a tube stage needs, so it is cheap enough to re-run when
// the SUSTAIN knob moves (which changes the answer).
void CompressorEngine::solveQuiescent() {
    const double rTot = cfg_.ctl.seriesOhms +
                        cfg_.ctl.potOhms * (1.0 - sustainWiper_.target());
    double v = cfg_.det.supplyV;
    double vbe = 0.62;
    double ie = 0.0;
    for (int i = 0; i < 40; ++i) {
        ie = (v - vbe - cfg_.ctl.cellPinV) / rTot;
        if (ie < kMinControlAmps) ie = kMinControlAmps;
        vbe = cfg_.npn.Vt * std::log(ie / cfg_.npn.Is + 1.0);
        const double ib = npnIb(vbe, cfg_.npn);
        const double next = cfg_.det.supplyV - cfg_.det.envResOhms * ib;
        if (std::fabs(next - v) < 1e-9) {
            v = next;
            break;
        }
        v = next;
    }
    vEnvPark_ = v;
    vbe5Park_ = vbe;
    iCtlPark_ = ie;
}

void CompressorEngine::reset() {
    inHpX1_ = inHpY1_ = 0.0;
    outHpX1_ = outHpY1_ = 0.0;
    for (int i = 0; i < 2; ++i) drvX1_[i] = drvY1_[i] = 0.0;
    loadY1_ = 0.0;
    clampP_ = clampN_ = 0.0;
    vbP_ = vbN_ = 0.0;
    // Re-park at the CACHED operating point — never re-solve here.
    vEnv_ = vEnvPark_;
    iCtl_ = iCtlPark_;
    vbe5_ = vbe5Park_;
    compliance_.reset();
    os_.reset();
    sustainWiper_.reset();
    level_.reset();
    osPhase_ = 0;
    rTotCtl_ = cfg_.ctl.seriesOhms +
               cfg_.ctl.potOhms *
                   (1.0 - static_cast<double>(sustainWiper_.value()));
}

void CompressorEngine::setOversampling(int factor) {
    os_.setFactor(factor);
    osFactor_ = os_.factor();
    rebuildRates();
    // The oversampled states are rate-dependent; clear them rather than leaving
    // history that belonged to another rate.
    for (int i = 0; i < 2; ++i) drvX1_[i] = drvY1_[i] = 0.0;
    loadY1_ = 0.0;
    clampP_ = clampN_ = 0.0;
    vbP_ = vbN_ = 0.0;
    compliance_.reset();
}

void CompressorEngine::setParameter(int paramId, float value) {
    // ParamGuard, never a hand-rolled clamp: std::clamp and the ternary both pass
    // NaN straight through, and one NaN latches forever in recursive state.
    const float v = clampParam01(value);
    switch (paramId) {
        case PARAM_SUSTAIN:
            sustainWiper_.setTarget(v);
            // The quiescent point depends on the wiper, so the park must follow
            // it. Bounded, allocation-free, audio-thread safe.
            solveQuiescent();
            break;
        case PARAM_LEVEL:
            level_.setTarget(v);
            break;
        default:
            // Slot 1 is carried and unused (a compressor has two knobs).
            break;
    }
}

// --- Gain cell ---------------------------------------------------------------

double CompressorEngine::applyGainCell(double vDiff) const {
    switch (cfg_.cell.kind) {
        case GainCellSpec::Kind::kOtaTanh:
        default:
            // The bipolar differential pair, verbatim: the pair splits its tail
            // current by tanh, so the OTA's output current is the tail current
            // (Iabc) times tanh of the differential input over 2·Vt. Small-signal
            // this is gm = Iabc/(2·Vt); large-signal it is the CA3080's own soft
            // limit, which is a large part of why this pedal colours the tone.
            return iCtl_ * std::tanh(vDiff / (2.0 * cfg_.cell.otaVt));
    }
}

double CompressorEngine::cellGainAtDc() const {
    // d(Vout)/d(Vin) at the origin: (dH/dV at DC) · gm · R_load. The drive
    // network's DC magnitude is hfGain·(z0·z1)/(p0·p1).
    const double hDc = std::fabs(cfg_.drive.hfGain) *
                       (cfg_.drive.zeroHz[0] * cfg_.drive.zeroHz[1]) /
                       (cfg_.drive.poleHz[0] * cfg_.drive.poleHz[1]);
    const double gm = iCtl_ / (2.0 * cfg_.cell.otaVt);
    return hDc * gm * cfg_.load.rLoadOhms * cfg_.split.gain;
}

// --- Sidechain ---------------------------------------------------------------

double CompressorEngine::detectorLeg(double drive, double& capState,
                                     double& vbWarm, double srcOhms) const {
    const double tOverC = 1.0 / (osRate_ * cfg_.det.clampCapF);
    const double k = srcOhms + tOverC;
    const double invR = 1.0 / cfg_.det.clampResOhms;

    // g(vb) = vb - drive + qPrev + k*f(vb);  f' > 0  =>  g strictly increasing.
    double vb = vbWarm;
    double f = 0.0;
    for (int it = 0; it < kMaxNewtonIter; ++it) {
        f = vb * invR + npnIb(vb, cfg_.npn) - clampDiodeIn(vb, cfg_.diode);
        const double g = vb - drive + capState + k * f;
        // Residual expressed as a CURRENT so the tolerance means something.
        if (std::fabs(g) / k < kNewtonResidualTolA) break;
        const double fp =
            invR + npnBaseCond(vb, cfg_.npn) + clampDiodeCond(vb, cfg_.diode);
        double step = g / (1.0 + k * fp);
        if (step > kNewtonMaxStepV) step = kNewtonMaxStepV;
        if (step < -kNewtonMaxStepV) step = -kNewtonMaxStepV;
        vb -= step;
    }
    // Recompute the branch current at the converged node so the cap advance
    // agrees with the same vb.
    f = vb * invR + npnIb(vb, cfg_.npn) - clampDiodeIn(vb, cfg_.diode);
    // NO flushDenormal on either of these, and that is measured rather than
    // assumed (ADR 006's "measure which, don't guess"). The node's physical
    // equilibrium against the clamp diode's reverse saturation current is
    // -4.90e-272 V, which IS below the flush floor — but the SOLVER cannot
    // resolve it: Newton exits at kNewtonResidualTolA, so vb floors at
    // kNewtonResidualTolA * (Rsrc + T/C) ~= 1.9e-11 V and the cap settles inside
    // that. 1e-11 is 297 decades above the subnormal range, so a guard here can
    // never fire — it would be unreachable code in the hottest loop in this
    // file, exactly what docs §33 found the wrong thing to ship. Measured after
    // a 40 s silent tail: |clamp cap| ~= 1e-11 V, not decaying, not subnormal.
    capState = capState + tOverC * f;
    vbWarm = vb;

    const double ic = npnIc(vb, cfg_.npn);
    return ic > 0.0 ? ic : 0.0;
}

void CompressorEngine::advanceEnvelope(double dischargeAmps) {
    // Simplified saturation of the discharge transistors: their collector current
    // collapses as Vce approaches zero. The exact statement is Ebers-Moll's
    // reverse term; this smooth, monotone, bounded stand-in only ever acts when
    // the envelope node is already bottomed, and it is the ONE approximation in
    // the sidechain (docs §59.6).
    const double vce = vEnv_ > 0.0 ? vEnv_ : 0.0;
    const double sat = vce / (vce + cfg_.det.vceSatV);

    const double h = 1.0 / (osRate_ * cfg_.det.envCapF);
    const double ib5 = npnIb(vbe5_, cfg_.npn);
    const double num =
        vEnv_ + h * (cfg_.det.supplyV / cfg_.det.envResOhms -
                     dischargeAmps * sat - ib5);
    const double den = 1.0 + h / cfg_.det.envResOhms;
    double v = num / den;
    if (v < 0.0) v = 0.0;
    if (v > cfg_.det.supplyV) v = cfg_.det.supplyV;
    // NO flushDenormal: this node rests at a nonzero DC operating point
    // (8.5786 V measured after a 20 s silent tail) and can never be subnormal.
    // ADR 006's scope rule — a guard here is unreachable code in a hot loop.
    vEnv_ = v;
}

void CompressorEngine::updateControl() {
    // Q5 emitter follower into the SUSTAIN rheostat and the fixed series
    // resistor, landing on the gain cell's control pin. `rTotCtl_` is refreshed
    // once per BASE sample by the caller — the wiper is the smoothed quantity,
    // never the derived current.
    double ie = 0.0;
    for (int i = 0; i < 2; ++i) {
        ie = (vEnv_ - vbe5_ - cfg_.ctl.cellPinV) / rTotCtl_;
        if (ie < kMinControlAmps) ie = kMinControlAmps;
        vbe5_ = cfg_.npn.Vt * std::log(ie / cfg_.npn.Is + 1.0);
    }
    iCtl_ = ie;
}

// --- Processing --------------------------------------------------------------

void CompressorEngine::process(const float* in, float* out, int numFrames) {
    int done = 0;
    while (done < numFrames) {
        const int n = std::min(numFrames - done, maxBlockSize_);
        processChunk(in + done, out + done, n);
        done += n;
    }
}

void CompressorEngine::processChunk(const float* in, float* out, int numFrames) {
    if (snapPending_) {
        // Deferred snap (docs §35): the C ABI's prepare-then-set order would
        // otherwise ramp every knob from its construction default on the first
        // block.
        sustainWiper_.setImmediate(sustainWiper_.target());
        level_.setImmediate(level_.target());
        solveQuiescent();
        vEnv_ = vEnvPark_;
        iCtl_ = iCtlPark_;
        vbe5_ = vbe5Park_;
        snapPending_ = false;
    }
    if (rTotCtl_ <= 0.0) {
        rTotCtl_ = cfg_.ctl.seriesOhms +
                   cfg_.ctl.potOhms *
                       (1.0 - static_cast<double>(sustainWiper_.value()));
    }

    // Stage 1 (base rate): input coupling + the follower's own gain.
    for (int i = 0; i < numFrames; ++i) {
        const double x = static_cast<double>(in[i]);
        const double y = inHpR_ * (inHpY1_ + x - inHpX1_);
        inHpX1_ = flushDenormal(x);
        inHpY1_ = flushDenormal(y);
        out[i] = static_cast<float>(y * cfg_.in.gain);
    }

    // Stages 2..6 run inside ONE oversampling domain: the gain cell's tanh is the
    // aliasing source, and the sidechain is inside the same loop because the
    // detector closes around the cell — splitting them would change the loop.
    os_.upsample(out, numFrames);
    float* w = os_.buffer();
    const int f = os_.factor();
    const int n = numFrames * f;
    osPhase_ = 0;
    for (int i = 0; i < n; ++i) {
        // One knob advance per BASE sample (see rTotCtl_'s declaration).
        if (osPhase_ == 0) {
            rTotCtl_ = cfg_.ctl.seriesOhms +
                       cfg_.ctl.potOhms *
                           (1.0 - static_cast<double>(sustainWiper_.next()));
        }
        if (++osPhase_ >= f) osPhase_ = 0;
        const double x = static_cast<double>(w[i]);

        // Stage 2 — the OTA's differential drive network (two 1st-order sections).
        double d = x * drvScale_;
        for (int s = 0; s < 2; ++s) {
            const double y = drvB0_[s] * d + drvB1_[s] * drvX1_[s] -
                             drvA1_[s] * drvY1_[s];
            drvX1_[s] = flushDenormal(d);
            drvY1_[s] = flushDenormal(y);
            d = y;
        }

        // Stage 3 — the gain cell, on the control current the sidechain set.
        const double iOut = applyGainCell(d);

        // Stage 4 — the load: the cell drives a current into R ∥ C, and the node
        // it develops runs into the supply compliance (ASYMMETRIC on a single
        // rail). The compliance clip is ADAA so a slammed transient does not
        // alias its way back through the detector.
        const double vRaw = iOut * cfg_.load.rLoadOhms;
        loadY1_ = flushDenormal(loadY1_ + loadCoef_ * (vRaw - loadY1_));
        const double vNode = static_cast<double>(
            compliance_.processSampleADAA(static_cast<float>(loadY1_)));

        // Stage 5 — the phase splitter. The EMITTER leg is the audio; both legs
        // drive the detector, which is what makes the rectifier full-wave.
        const double y = cfg_.split.gain * vNode;

        // Stage 6 — the detector. FEED-BACK: it tastes the cell's OUTPUT. The
        // false branch (feed-forward) is a measurement hook, never a product
        // mode — see the header and `clipper_comp_tests`.
        const double det = cfg_.detectorFromOutput ? y : x;
        const double icP =
            detectorLeg(det, clampP_, vbP_, cfg_.det.srcEmitterOhms);
        const double icN =
            detectorLeg(-det, clampN_, vbN_, cfg_.det.srcCollectorOhms);
        advanceEnvelope(icP + icN);
        updateControl();

        w[i] = static_cast<float>(y);
    }
    os_.downsample(out, numFrames);

    // Stage 8 (base rate): output coupling + LEVEL.
    const double denom =
        cfg_.out.srcOhms + cfg_.out.seriesOhms + cfg_.out.potOhms;
    for (int i = 0; i < numFrames; ++i) {
        const double x = static_cast<double>(out[i]);
        const double y = outHpR_ * (outHpY1_ + x - outHpX1_);
        outHpX1_ = flushDenormal(x);
        outHpY1_ = flushDenormal(y);
        const double frac =
            static_cast<double>(level_.next()) * cfg_.out.potOhms / denom;
        out[i] = static_cast<float>(y * frac);
    }
}

double CompressorEngine::maxAbsRestingState() const {
    // Every recursive accumulator that genuinely rests at ZERO, and nothing
    // else — the six linear filter states. Adding a state that rests elsewhere
    // would make this assertion untestable instead of stricter.
    //
    // Deliberately excluded, each MEASURED rather than assumed:
    //  * vEnv_  — a real DC operating point: 8.9684 V at SUSTAIN 0, 8.6353 V at
    //    SUSTAIN 1.0. Never subnormal (ADR 006's scope rule).
    //  * clampP_/clampN_ and vbP_/vbN_ — floored by the Newton residual
    //    tolerance at ~1e-11 V, not by physics; see detectorLeg().
    return maxAbsState(inHpX1_, inHpY1_, outHpX1_, outHpY1_, drvX1_[0],
                       drvY1_[0], drvX1_[1], drvY1_[1], loadY1_);
}

}  // namespace clipper::dsp
