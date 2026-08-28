// Clipper — portable DSP core (M10.10). See ChampPreamp.h for the circuit rationale.

#include "clipper/dsp/ChampPreamp.h"

#include "clipper/dsp/OutputPotTaper.h"
#include "clipper/dsp/ParamGuard.h"

#include <algorithm>

namespace clipper::dsp {

ChampPreamp::ChampPreamp() {
    wiper_ = pot::audioTaperWiper(volumeKnob_);
}

// The two preamp nodes hang off the power section's B1 through the transcribed
// 1 k / 10 k decoupling pair. Solved self-consistently (each stage's current sets
// the drop that sets its own rail) rather than guessed, once per prepare.
void ChampPreamp::solveRails() {
    double b2 = b1_, b3 = b1_;
    for (int it = 0; it < 200; ++it) {
        TriodeStage::Config ca = v1a_.config();
        TriodeStage::Config cb = v1b_.config();
        ca.bPlus = b3;
        cb.bPlus = b2;
        v1a_.configure(ca);
        v1b_.configure(cb);
        v1a_.prepare(sampleRate_, maxBlockSize_);
        v1b_.prepare(sampleRate_, maxBlockSize_);
        // Ohm's law on each plate load gives the stage current at its own rail.
        const double ia = (b3 - v1a_.quiescentPlateVoltage()) / kRa;
        const double ib = (b2 - v1b_.quiescentPlateVoltage()) / kRa;
        const double nb2 = b1_ - (ia + ib) * kRdecB2;   // both currents cross the 1 k
        const double nb3 = nb2 - ia * kRdecB3;          // only V1A crosses the 10 k
        if (std::abs(nb2 - b2) < 1e-9 && std::abs(nb3 - b3) < 1e-9) { b2 = nb2; b3 = nb3; break; }
        b2 += 0.5 * (nb2 - b2);
        b3 += 0.5 * (nb3 - b3);
    }
    rail1a_ = b3;
    rail1b_ = b2;
}

void ChampPreamp::setMainRail(double b1Volts) {
    if (b1Volts > 0.0) b1_ = b1Volts;
}

void ChampPreamp::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = maxBlockSize > 0 ? maxBlockSize : 128;
    scratch_.assign(static_cast<size_t>(maxBlockSize_), 0.0f);

    TriodeStage::Config a;
    a.Ra = kRa; a.Rk = kRk; a.Ck = kCk;
    a.Rg = kRgStopper;            // the input jack's 68 k
    a.Cc = kCcIn;                 // 25 nF in
    a.Rgl = kVolPot;              // V1A's load IS the volume pot
    a.topology = TriodeStage::Topology::CommonCathode;
    v1a_.configure(a);

    TriodeStage::Config b;
    // V1B's cathode is UNBYPASSED, and that is the stock 5F1 — not an omission.
    // Fender left it that way deliberately: the 1.5 kOhm carries the full cathode
    // current unbypassed, which is local negative feedback (cathode degeneration)
    // and halves the stage's gain. Adding a bypass cap here is one of the most
    // common 5F1 MODS precisely because it buys "a gain and treble boost" — i.e.
    // the modders are changing it, which is the evidence it ships without one.
    //
    // THE ONE COMPONENT-LEVEL SOURCE FOR THIS AMP GETS IT WRONG. valtyr/rust-5f1's
    // capacitor table carries `C_K1B: 25uF`, and docs §73.1 already records TWO
    // other errors in that same file (it omits the 6V6's own cathode bypass cap,
    // and its 100 Ohm supply lands the plate node 15 V above Fender's measured
    // 305 V). This is the third, and it is the one that mattered: shipping the cap
    // put 5.95 dB of extra drive on the 6V6 grid and made the amp distort about
    // twice as early as it should. Field report 2026-08-28, docs §74.
    //
    // Measured: bypassed 59.42x, unbypassed 30.00x, against the analytic
    // mu*RL/(RL+rp+(mu+1)*Rk) = 29.9x. clipper_champ_tests asserts the unbypassed
    // figure so nobody "restores" the cap thinking the gain loss is a bug.
    b.Ra = kRa; b.Rk = kRk; b.Ck = kCkV1b;
    b.Rg = 0.0;                   // no grid stopper on V1B in the 5F1
    b.Cc = kCcInter;              // 20 nF out, into the 6V6's 1 M grid leak
    b.Rgl = kRgl;
    b.topology = TriodeStage::Topology::CommonCathode;
    v1b_.configure(b);

    solveRails();   // configures + prepares both stages at their solved rails
}

void ChampPreamp::setOversampling(int factor) {
    v1a_.setOversampling(factor);
    v1b_.setOversampling(factor);
}

void ChampPreamp::setParameter(int paramId, float value) {
    if (paramId == PARAM_VOLUME) {
        volumeKnob_ = clampParam01(static_cast<double>(value));
        // BARE audio taper — the pot is its own grid leak, so there is no wiper
        // load to correct for (see the header, and §68's loaded-vs-bare rule).
        wiper_ = pot::audioTaperWiper(volumeKnob_);
    }
}

void ChampPreamp::reset() {
    v1a_.reset();
    v1b_.reset();
}

void ChampPreamp::process(const float* in, float* out, int numFrames) {
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(maxBlockSize_, numFrames - off);
        v1a_.process(in + off, scratch_.data(), n);
        // The VOLUME pot sits BETWEEN the two stages, so it sets how hard V1B is
        // driven — this amp's only knob is a gain control, not an output level.
        const float w = static_cast<float>(wiper_);
        for (int i = 0; i < n; ++i) scratch_[static_cast<size_t>(i)] *= w;
        v1b_.process(scratch_.data(), out + off, n);
        off += n;
    }
}

}  // namespace clipper::dsp
