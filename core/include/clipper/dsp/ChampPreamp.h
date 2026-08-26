// Clipper — portable DSP core (M10.10).
//
// ChampPreamp: the Fender tweed Champ 5F1 PREAMP — two 12AX7 common-cathode stages
// with the 1 MΩ log VOLUME pot between them, and nothing else. There is no tone
// stack in this amp at all. Composed from the M9.1 TriodeStage (no new device
// model). Docs §73. Platform-free C++17.
//
// ===========================================================================
// THE 5F1 HAS NO TONE CONTROL — THAT IS THE WHOLE PREAMP
// ===========================================================================
// One volume knob and no tone stack of any kind. The Champ did not gain a tone
// control until the 1964 blackface AA764, which is a different circuit. (The
// ROADMAP's M10.10 entry said "ONE tone control"; that was wrong and is corrected in
// the same slice.) So this amp's voicing is entirely (a) the coupling caps, (b) the
// cathode bypasses, (c) the tiny single-ended output transformer — and there is
// nothing downstream to correct any of it with. That is why the tone controls a
// player reaches for on a Champ are the guitar's.
//
// Transcribed values (docs §73.1): both stages Ra 100 kΩ, Rk 1.5 kΩ bypassed 25 µF,
// grid leak 1 MΩ; input coupling 25 nF; interstage coupling 20 nF; volume 1 MΩ LOG.
//
// TriodeStage's ONE (Cc, Rgl) pair is applied to BOTH that stage's input and output
// coupling (§63.3's documented limitation), so a cascade applies each interstage
// high-pass twice. Here that is HARMLESS and the numbers say so: every corner in
// this preamp is 25 nF or 20 nF into 1 MΩ, i.e. 6.4 Hz and 8.0 Hz, so the doubling
// moves a pole that is already two decades below the guitar's lowest note. Compare
// the Rockerverb, where the real caps give 398 / 134 / 49 Hz and the same doubling
// is a gross error. Do NOT "fix" this by folding the caps into an MNA here — that
// would cost the preamp grid BLOCKING that TriodeStage models and this amp needs
// (a Champ is played into its own blocking distortion constantly).
//
// THE VOLUME POT IS ITS OWN GRID LEAK, which is why the taper is the BARE audio
// taper and not a loaded one (§68's distinction). The wiper drives V1B's grid
// directly and the pot's lower section IS that grid's DC return, so the source sees
// a CONSTANT 1 MΩ at every position and the wiper voltage is exactly w·V(top).
// There is no wiper load to correct for — unlike the TS's LEVEL pot (§68), which is
// loaded by 226 kΩ and therefore is NOT the bare taper.
//
#pragma once

#include "clipper/dsp/TriodeStage.h"

#include <vector>

namespace clipper::dsp {

class ChampPreamp {
public:
    enum ParamId : int {
        PARAM_VOLUME = 0,   // the amp's ONLY knob
        PARAM_COUNT = 1,
    };

    ChampPreamp();

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);
    int oversampling() const { return v1a_.oversampling(); }
    int latencySamples() const { return v1a_.latencySamples() + v1b_.latencySamples(); }

    void setParameter(int paramId, float value);
    void reset();

    void process(const float* in, float* out, int numFrames);

    // --- Introspection (measurement / tests) --------------------------------
    double rail1a() const { return rail1a_; }   // B3 — the most-filtered node
    double rail1b() const { return rail1b_; }   // B2
    double volumeWiper() const { return wiper_; }
    const TriodeStage& v1a() const { return v1a_; }
    const TriodeStage& v1b() const { return v1b_; }

    // Documented constants (cited in the tests / docs §73).
    static constexpr double kRa = 100.0e3;      // both plate loads (Ω)
    static constexpr double kRk = 1500.0;       // both cathode resistors (Ω)
    static constexpr double kCk = 25.0e-6;      // both cathode bypass caps (F)
    static constexpr double kRgl = 1.0e6;       // grid leaks (Ω)
    static constexpr double kCcIn = 25.0e-9;    // input coupling (F) — 6.4 Hz
    static constexpr double kCcInter = 20.0e-9; // interstage coupling (F) — 8.0 Hz
    static constexpr double kVolPot = 1.0e6;    // VOLUME pot (Ω), LOG
    // The standard Fender input-jack grid stopper. NOT present in the one
    // component-level source for this amp (which also omits the 6V6's cathode
    // bypass cap), but it is on every published 5F1 schematic. Named as such.
    static constexpr double kRgStopper = 68.0e3;
    // Supply decoupling, transcribed: B1 --1k--> B2 --10k--> B3.
    static constexpr double kRdecB2 = 1.0e3;
    static constexpr double kRdecB3 = 10.0e3;

    // Set the B1 rail the power section actually solved, so the preamp's own two
    // nodes are derived from it rather than from a second, independent guess.
    void setMainRail(double b1Volts);

private:
    void solveRails();

    TriodeStage v1a_, v1b_;
    double sampleRate_ = 48000.0;
    int maxBlockSize_ = 128;
    double b1_ = 305.0;
    double rail1a_ = 0.0, rail1b_ = 0.0;
    double volumeKnob_ = 0.5, wiper_ = 0.0;
    std::vector<float> scratch_;
};

}  // namespace clipper::dsp
