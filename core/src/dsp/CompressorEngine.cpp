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
    sc_.configure(cfg_.det, cfg_.npn, cfg_.diode);
    sc_.setRate(osRate_);

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
    // Re-park at the CACHED operating point — never re-solve here.
    sc_.reset(vEnvPark_);
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
    sc_.clearRateState();
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

void CompressorEngine::updateControl() {
    // Q5 emitter follower into the SUSTAIN rheostat and the fixed series
    // resistor, landing on the gain cell's control pin. `rTotCtl_` is refreshed
    // once per BASE sample by the caller — the wiper is the smoothed quantity,
    // never the derived current.
    double ie = 0.0;
    for (int i = 0; i < 2; ++i) {
        ie = (sc_.envelopeVolts() - vbe5_ - cfg_.ctl.cellPinV) / rTotCtl_;
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
        sc_.setEnvelopeVolts(vEnvPark_);
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
        // The SHARED detector (docs §61.2). Q5's base current is the CONSUMER's
        // own load on the envelope node, so it is passed in rather than living
        // inside the detector — a gate's comparator draws none.
        sc_.advanceEnvelope(sc_.detect(det), npnIb(vbe5_, cfg_.npn));
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
    //  * the detector's envelope node — a real DC operating point: 8.9684 V at
    //    SUSTAIN 0, 8.6353 V at SUSTAIN 1.0. Never subnormal (ADR 006's rule).
    //  * the detector's clamp caps and Newton warm starts — floored by the Newton
    //    residual tolerance at ~1e-11 V, not by physics; see
    //    SidechainDetector::leg().
    return maxAbsState(inHpX1_, inHpY1_, outHpX1_, outHpY1_, drvX1_[0],
                       drvY1_[0], drvX1_[1], drvY1_[1], loadY1_);
}

}  // namespace clipper::dsp
