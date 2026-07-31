// Clipper — portable DSP core (M10.3, docs §57; SCHEMATIC CORRECTION 2026-07-31).
//
// OrangePreamp: the front half of an early-1970s Orange OR120 "Overdrive" head,
// transcribed from the owner-supplied schematics (see the PROVENANCE banner):
//
//   2 input jacks -> 68k grid stopper -> V1A grid (1M grid leak)
//     -> V1A   1/2 ECC83, Ra 220k to D+, Rk 2k2 bypassed by 50uF
//     -> 68n   -> JAMES / passive-Baxandall stack (BASS + TREBLE, no MID)
//     -> GAIN  1M log  (the panel's GAIN — this amp's only volume control)
//     -> 330p  -> V1B grid (1M grid leak)
//     -> V1B   1/2 ECC83, Ra 220k to D+, Rk 2k2 bypassed by 50uF
//     -> 68n   -> F.A.C. six-way series coupling cap -> the OUTPUT AMP
//
// ===========================================================================
// THE STRUCTURAL CORRECTION (this is defect #1 of the schematic pass)
// ===========================================================================
// The first release of this voice ran V1A -> VOLUME -> V1B -> F.A.C. -> James
// stack, i.e. it treated the James network as a TERMINAL EQ. The schematic says
// otherwise, and the difference is a response difference, not merely a
// frequency-response one:
//
//   * The stack sits BETWEEN V1A and the GAIN pot, driven by V1A's 220k plate
//     (a genuinely high-Z source, not a follower), and its insertion loss is
//     made up by V1B. So turning GAIN up drives V1B harder WITH AN ALREADY-EQ'd
//     SIGNAL: BASS/TREBLE change what gets distorted, not what comes out of the
//     distortion.
//   * The F.A.C. is the LAST thing in the preamp and feeds the POWER AMP, so it
//     trims the drive into the phase inverter as well as the bandwidth.
//   * The GAIN pot is the stack's LOAD, so its setting changes the network's own
//     response; it is inside the same MNA here for exactly that reason.
//   * The 330p between the GAIN wiper and V1B's grid is a real, deliberate
//     high-pass (it and the 1M grid leak put a corner in the low mids). It is
//     why an OR120 is mid-forward and thick rather than flabby, and it did not
//     exist in the reconstruction at all.
//
// ===========================================================================
// PROVENANCE — read docs §57.1 before changing a constant
// ===========================================================================
// The first release of this voice was an explicit RECONSTRUCTION: no schematic
// was reachable from the build container, so the topology came from forum prose
// and nearly every component value was invented under physical constraints. That
// section said in terms: "Do not re-tune any of them toward a sound; find the
// schematic."  The owner supplied it on 2026-07-31.
//
// EVERY component value in this file is now transcribed from:
//   (1) "OR120 GRAPHIC MkII — Early 1970's", B. Hickmott's 2004 redraw of the 1972
//       factory schematics (Preamp / Output-amp / Power-supply sheets) — PRIMARY;
//   (2) the post-'74 "ORANGE GRAPHIC MkII" factory sheet + parts list — CROSS-CHECK;
//   (3) the Orange Amp Field Guide OR120 HEAD page — panel layout and tube count.
// ERA: the EARLY 1970s amp (source 1). It is the one with a complete drawn
// netlist and it carries the 1n5 treble cap; source 2 is the post-'74 amp (330 pF
// treble) and is the cross-check, not the target.
//
// Convention: 1.0f == 1.0 V. Platform-free C++17 (no OS/browser/Emscripten deps).

#ifndef CLIPPER_DSP_ORANGE_PREAMP_H
#define CLIPPER_DSP_ORANGE_PREAMP_H

#include <array>
#include <vector>

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/TriodeStage.h"

namespace clipper::dsp {

// ---------------------------------------------------------------------------
// JamesToneStack — the OR120's passive Baxandall (James, 1949) BASS/TREBLE
// network TOGETHER WITH the GAIN pot that loads it and the 330p/1M grid network
// that follows the wiper, as ONE nodal MNA with trapezoidal capacitor
// companions. Same numerical shape as MarshallToneStack (docs §14).
//
// The GAIN pot and the grid coupling are inside the same matrix deliberately:
// the pot is the network's load (so its position changes the stack's response),
// and the 330p sees the pot's wiper impedance (so the coupling corner MOVES with
// the knob). Splitting them into cascaded blocks would lose both effects.
//
// Netlist (nodes F, A, W, B, T, U, OUT, G, GRID; GND) — transcribed:
//   Rs*  : source - F                  (* V1A's PLATE impedance, Ra 220k || rp;
//                                         the 68n coupling cap is carried by the
//                                         TriodeStage's own output coupling)
//   R1   : F   - A          100k       (series into the BASS pot top)
//   RB   : A -((1-b)RB)- W -((b)RB)- B  BASS 1M LOG
//   Cbu  : A   - W          2n2        (across the pot's UPPER section)
//   Cbl  : W   - B          22n        (across the pot's LOWER section)
//   R3   : B   - GND        22k
//   R2   : W   - OUT        100k       (bass wiper into the treble pot / output)
//   C2   : F   - T          1n5        (TREBLE series cap — the EARLY value)
//   RT   : T -((1-t)RT)- OUT -((t)RT)- U  TREBLE 1M LOG
//   C3   : U   - GND        10n
//   RG   : OUT -((1-g)RG)- G -((g)RG)- GND  GAIN 1M LOG (the volume control)
//   Cg   : G   - GRID       330p       (GAIN wiper -> V1B grid)
//   Rgl  : GRID - GND       1M         (V1B grid leak)
//
// Two PARALLEL branches summing through R2 at the treble wiper is the whole
// mechanism. Both bass caps short the bass pot out above their corners (so BASS
// acts only below), and C2 blocks the treble branch below its corner (so TREBLE
// acts only above). Neither branch does anything to the middle, which is why the
// mids survive — that is TOPOLOGY, and it is what §57.4's bar measures.
//
// Knob smoothing follows MarshallToneStack exactly (audit finding 6, docs §35):
// pot fractions are one-pole smoothed per sample and the matrix is re-derived at
// a 32-sample control rate, with the cap states carried across.
// ---------------------------------------------------------------------------
class JamesToneStack {
public:
    // --- Component values: ALL TRANSCRIBED (source 1, preamp sheet). ---------
    static constexpr double kR1 = 100.0e3;      // 100k, input series into BASS top
    static constexpr double kRB = 1.0e6;        // BASS pot, 1M log
    static constexpr double kCbUpper = 2.2e-9;  // 2n2 across the upper section
    static constexpr double kCbLower = 22.0e-9; // 22n across the lower section
    static constexpr double kR3 = 22.0e3;       // 22k, bass pot bottom to ground
    static constexpr double kR2 = 100.0e3;      // 100k, BASS wiper to TREBLE pot
    // The EARLY ('72/'74) treble cap. The post-'74 sheet lists C13/C14 = 330 pF;
    // owners describe the later knob as "just the highs" where the early one
    // "brings high mids up with it" — which is precisely this value's corner.
    static constexpr double kC2 = 1.5e-9;       // 1n5 at the TREBLE pot top
    static constexpr double kRT = 1.0e6;        // TREBLE pot, 1M log
    static constexpr double kC3 = 10.0e-9;      // 10n, treble pot bottom to ground
    static constexpr double kRG = 1.0e6;        // GAIN pot, 1M log
    static constexpr double kCg = 330.0e-12;    // 330p, GAIN wiper -> V1B grid
    static constexpr double kRgl = 1.0e6;       // V1B grid leak

    void prepare(double sampleRate);
    void reset();
    void setSourceImpedance(double rs);                    // V1A plate impedance
    void setKnobs(double bass, double treble, double gain);  // each in [0,1]
    void process(const float* in, float* out, int numFrames);

    // Snap the knob smoothers onto their targets and re-derive the matrix with no
    // ramp (the owning preamp's deferred prime, and reset()).
    void snapKnobs();

    // Anti-denormal diagnostic (Denormal.h, docs §33). All ten cap companions
    // rest at exactly zero, so after a silent tail this must be EXACTLY 0.0.
    double maxAbsRestingState() const {
        return maxAbsState(vBu_, iBu_, vBl_, iBl_, v2_, i2_, v3_, i3_, vg_, ig_);
    }

    // Analytic steady-state |H(jw)| of THIS netlist, for the discretization check
    // and the §57.4 mid-forward metric.
    //   node OUT  = the TONE NETWORK's own output (the treble wiper, loaded by the
    //               GAIN pot) — the fair analogue of the FMV's output node, and
    //               what §57.4(a) measures.
    //   node GRID = V1B's grid, i.e. everything including the pot divider and the
    //               330p — what process() emits.
    enum class Probe { Out, Grid };
    static double magnitudeAt(double freqHz, double bass, double treble, double gain,
                              double rs, Probe probe = Probe::Grid);

private:
    void rebuild();

    // Node order: F=0, A=1, W=2, B=3, T=4, U=5, OUT=6, G=7, GRID=8.
    static constexpr int N = 9;
    static constexpr double kKnobSmoothSeconds = 0.008;
    static constexpr int kCtrlBlock = 32;

    double sampleRate_ = 44100.0;
    double T_ = 1.0 / 44100.0;
    double rs_ = 60.0e3;
    OnePoleSmootherD bassSm_, trebleSm_, gainSm_;
    double bass_ = 0.5, treble_ = 0.5, gain_ = 0.5;
    bool knobsMoving_ = false;
    int ctrlCounter_ = 0;
    // Cap companion conductances: Cb-upper, Cb-lower, C2, C3, Cg.
    double geqBu_ = 0.0, geqBl_ = 0.0, geq2_ = 0.0, geq3_ = 0.0, geqg_ = 0.0;
    std::array<std::array<double, N>, N> Ginv_{};
    // Trapezoidal cap states (v across, i through).
    double vBu_ = 0.0, iBu_ = 0.0;
    double vBl_ = 0.0, iBl_ = 0.0;
    double v2_ = 0.0, i2_ = 0.0;
    double v3_ = 0.0, i3_ = 0.0;
    double vg_ = 0.0, ig_ = 0.0;
    bool dirty_ = true;
};

// ---------------------------------------------------------------------------
// FacNetwork — the F.A.C. ("Frequency Analysing Control") six-way rotary, which
// the schematic puts at the very END of the preamp: V1B's plate couples through
// 68n into the switch, and the switch feeds the OUTPUT AMP through the effects
// loop's 100k send + 100k return into the driver's grid leak.
//
//   source (V1B plate, behind rs) - P
//   P - Cfac - Q          (position 0 is STRAIGHT THROUGH — no cap at all)
//   Q - 200k - GG         (the loop's 100k preamp feed + 100k return)
//   GG - 1M - GND         (the driver's grid leak)
//
// TRANSCRIBED ladder, in switch order: [through], 4n7, 4n7, 2n2, 1n, 330p. The
// post-'74 parts list agrees on the five caps (C16 330p, C17 1000p, C18 2200p,
// C19 4700p, C20 4700p). Two things about it are worth stating plainly:
//   * The fat end tops out at 4n7 and a straight-through position, NOT at the
//     "47 n" the reconstruction used — a 10x error at that end.
//   * Positions 1 and 2 carry the SAME 4n7 on both sheets. The switch is a 2-pole
//     6-way (SW2), so the second pole plausibly does something the single-line
//     transcription does not carry. It is shipped literally; see docs §57.1.
// ---------------------------------------------------------------------------
class FacNetwork {
public:
    static constexpr int kPositions = 6;
    // 0.0 == the straight-through position (no series cap).
    static constexpr double kCaps[kPositions] = {
        0.0, 4.7e-9, 4.7e-9, 2.2e-9, 1.0e-9, 330.0e-12,
    };
    static constexpr double kRloop = 200.0e3;   // 100k send feed + 100k return
    static constexpr double kRgl = 1.0e6;       // driver grid leak
    // The "straight through" click is stamped as a 1 ohm series resistor rather
    // than a node merge, so one matrix build serves all six positions. 1 ohm into
    // a 1.2 M load is -1.5e-5 dB: below every measurement in the suite.
    static constexpr double kShortOhms = 1.0;

    void prepare(double sampleRate);
    void reset();
    void setSourceImpedance(double rs);
    void setPosition(int pos);
    int position() const { return pos_; }
    void process(const float* in, float* out, int numFrames);

    double maxAbsRestingState() const { return maxAbsState(vc_, ic_); }

    static double magnitudeAt(double freqHz, int pos, double rs);

private:
    void rebuild();
    static constexpr int N = 3;  // P=0, Q=1, GG=2

    double sampleRate_ = 44100.0;
    double T_ = 1.0 / 44100.0;
    double rs_ = 60.0e3;
    int pos_ = 1;
    double geqc_ = 0.0;
    std::array<std::array<double, N>, N> Ginv_{};
    double vc_ = 0.0, ic_ = 0.0;
    bool dirty_ = true;
};

// ---------------------------------------------------------------------------
// The composed OR120 preamp, in the schematic's order:
//   V1A -> James stack -> GAIN -> 330p -> V1B -> 68n -> F.A.C. -> output amp
// ---------------------------------------------------------------------------
class OrangePreamp {
public:
    enum ParamId : int {
        PARAM_VOLUME = 0,  // the panel's GAIN — the ONE gain control (1M log)
        PARAM_BASS = 1,
        PARAM_TREBLE = 2,
        PARAM_FAC = 3,     // 0..1 -> one of six F.A.C. positions
        PARAM_COUNT = 4,
    };

    enum Stage : int { V1A = 0, V1B = 1, STAGE_COUNT = 2 };

    OrangePreamp();

    // cPlus is the driver/inverter supply node the 33k dropper feeds D+ from; the
    // composed OrangeAmp passes the value OrangePowerAmp actually solved.
    void setSupplyCPlus(double cPlus);
    double supplyDPlus() const { return dPlus_; }

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);
    int oversampling() const { return oversampling_; }

    int latencySamples() const {
        int n = 0;
        for (int s = V1A; s <= V1B; ++s)
            n += stage_[static_cast<size_t>(s)].latencySamples();
        return n;
    }

    void setParameter(int paramId, float value);
    void reset();
    void process(const float* in, float* out, int numFrames);

    // --- Introspection (measurement / tests) --------------------------------
    double stageQuiescentPlate(int stage) const;
    double stageQuiescentCathode(int stage) const;
    double stageQuiescentCurrent(int stage) const;
    double plateSourceImpedance() const { return plateRoutA_; }
    double facSourceImpedance() const { return plateRoutB_; }
    const TriodeStage::Config& stageConfig(int stage) const { return cfg_[stage]; }
    JamesToneStack& toneStack() { return tone_; }
    FacNetwork& facNetwork() { return fac_; }

    // --- Transcribed supply chain (power-supply sheet) -----------------------
    // B+ (screens, after the choke) -> 33K 2W -> C+ (16uF) [driver + cathodyne]
    //                                 C+ -> 33K 2W -> D+ (16uF) [V1A + V1B]
    static constexpr double kRdropDplus = 33.0e3;
    // Default C+ for a STANDALONE preamp (no power section attached). It is the
    // value OrangePowerAmp::cPlusIdle() solves at the shipped constants, so a
    // bare OrangePreamp and a composed OrangeAmp park at the same DC.
    static constexpr double kCplusDefault = 434.0;

    // The GAIN pot is a 1M LOG part (both sheets). The taper LETTER is on the
    // sheet; the taper LAW is not, so the pot keeps the house audio law with
    // k = 4 (docs §51's kMasterTaperK) — the same constant every other log pot in
    // the project uses. Nothing here is fitted to a breakup target.
    static constexpr double kVolumeTaperK = 4.0;
    static double volumeTaper(double x);
    // The wiper fraction the MNA actually stamps (post-taper).
    double volumeWiper() const;

    double lastOutputPeak() const { return lastOutPeak_; }

private:
    void configureStages();
    void solveSupply();
    void primeSmoothers();

    static constexpr double kSmoothSeconds = 0.008;

    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;

    std::array<TriodeStage, STAGE_COUNT> stage_;
    std::array<TriodeStage::Config, STAGE_COUNT> cfg_{};
    JamesToneStack tone_;
    FacNetwork fac_;

    double cPlus_ = kCplusDefault;
    double dPlus_ = 380.0;

    double volume_ = 0.5;
    bool primed_ = false;
    double bass_ = 0.5, treble_ = 0.5;
    double facKnob_ = 0.2;
    double plateRoutA_ = 60.0e3;
    double plateRoutB_ = 60.0e3;

    std::vector<float> buf_;
    double lastOutPeak_ = 0.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_ORANGE_PREAMP_H
