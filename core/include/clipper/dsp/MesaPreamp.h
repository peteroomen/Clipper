// Clipper — portable DSP core (M10.4, docs §68).
//
// MesaPreamp: the preamp of a Mesa/Boogie Dual Rectifier Solo Head — FIVE 12AX7
// gain stages, a CATHODE FOLLOWER driving the tone stack, TWO complete FMV tone
// stacks (one per channel) and two masters. This is the grunge / 90s-metal amp,
// and it is the first voice in this repo with FIVE switched MODES rather than a
// channel count.
//
//   guitar in -> 1M grid leak
//     -> V1A  Ra 220k to E, Rk 1k8, bypass 1u + (47k shorted by LDR3b)
//     -> C 20n -> node X1
//          X1 -- 2M2 -- GND                        (the load)
//          X1 -- (680k || 2n) -- LDR4 -- GND       ("V1A OUTPUT PAD")
//          X1 -- 82p  -- LDR1 -> RED path          ("RD input sel")
//          X1 -- 2M2  -- LDR2 -> ORANGE path       ("OR input select")
//     -> 1n -> GAIN POT 1M (RED and ORANGE have their OWN pot)
//     -> wiper -> LDR5/LDR6 -> 470k -> V2A grid (20p to GND)
//     -> V2A  Ra 100k to D, Rk 1k8, bypass 1u + (47k shorted by LDR3a)
//     -> C 20n -> 470k series -> 1M leak
//     -> V2B  Ra 100k to D, Rk 39k UNBYPASSED                  (the cold stage)
//     -> C 20n -> 330k leak -> 220k stopper
//     -> V3A  Ra 220k to C, Rk 1k8, bypass 1u + (47k shorted by LDR10)
//     -> V3B  CATHODE FOLLOWER (plate to C, 100k cathode load)
//     -> FMV tone stack, RED (680p) or ORANGE (500p)
//     -> MASTER 1M -> the effects loop -> the phase inverter
//
// ===========================================================================
// PROVENANCE — read docs §68.1 before changing a constant
// ===========================================================================
// EVERY component value above is TRANSCRIBED from the factory drawing set for
// the MESA/BOOGIE DUAL RECTIFIER SOLO HEAD (sheets `mbdr1`..`mbdr8`), supplied
// by the owner. Sheet `mbdr1` is the preamp; `mbdr7` is the LDR TRUTH TABLE,
// reproduced verbatim in kModeTable below. The sheets also carry MARKED DC NODE
// VOLTAGES, which is the strongest kind of check available to this project: an
// ABSOLUTE reference produced by the manufacturer, not by this model.
//
//     V1A plate 200 V   V2A plate 280 V   V2B plate 384 V   V3A plate 213 V
//     V1A Vk 1.6 V      V2A Vk 2 V        V2B Vk 5 V        V3A Vk 1.6 V
//     V3B plate 415 V, cathode 216 V
//     rail ladder: A 460 -> B 454 -> C 422 -> D 406 -> E 402 V
//
// The stage supplies are the sheet's own node letters: V1A<-E, V2A<-D, V2B<-D,
// V3A<-C, V3B<-C. Those are asserted in the test, not merely used.
//
// This is a TRANSCRIPTION, not the documented reconstruction §57 had to settle
// for on the OR120. WebFetch is EGRESS_BLOCKED in this container, GitHub code
// search is repository-scoped this session, and LiveSPICE carries no Mesa
// example — so without the owner's sheets this voice would have been invented.
//
// WHAT IS NOT TRANSCRIBED, named rather than hidden:
//   * the pots' taper LAWS. The sheet gives values (1M gain, 220k treble, 1M
//     bass, 25k mid, 1M master) but not letters, so every log pot keeps the
//     house audio law with k = 4 (docs §51), as every other voice does.
//   * the 12AX7 device card itself — the shared Koren fit (TriodeStage.h).
//   * the effects loop is modelled ONLY as its bypass path (see MesaAmp.h).
//
// ===========================================================================
// THE CATHODE-BYPASS IDIOM, and the ONE approximation it forces
// ===========================================================================
// Every switched cathode bypass on this amp is the SAME network: a 1u cap in
// series with a 47k resistor that an LDR SHORTS OUT. So:
//
//     LDR ON  -> 47k shorted   -> full 1u across Rk        (bypassed, high gain)
//     LDR OFF -> 1u + 47k      -> essentially unbypassed
//
// i.e. a "cathode cap" LDR is really a GAIN switch, and only OR CLN turns them
// off. TriodeStage::Config carries a bare `Ck` with no series resistance, so the
// OFF state ships as Ck = 0 (unbypassed). THE ERROR IS BOUNDED AND MEASURED: at
// 100 Hz the real OFF branch is |1/(jwC)| + 47k = 48.6k against Rk = 1k8, i.e.
// it shunts 3.6% of the cathode impedance, worth under 0.4 dB of stage gain.
// Do NOT "fix" this by putting 47k into Rk — that changes the BIAS, which the
// sheet's marked 1.6 V pins. The proper fix is a series-R field on TriodeStage's
// cathode network, which is the same shared-class change §63.3 already names for
// the coupling caps; it is a follow-up, not this slice.
//
// ===========================================================================
// THE FIVE MODES ARE THE VOICE (sheet `mbdr7`)
// ===========================================================================
// This amp does not have "channels" in the sense the Rockerverb does. It has two
// channels (RED / ORANGE) each with modes, and the drawing enumerates FIVE
// states. The single most important row is the FEEDBACK pair:
//
//     OR NORM : FB on            OR CLN : FB on + MORE FB on
//     OR MOD  : FB OFF           RED NORM: FB OFF          RED VINT: FB on
//
// THE TWO "MODERN" MODES RUN THE POWER AMP WITH NO GLOBAL NEGATIVE FEEDBACK AT
// ALL. Every other amp in this lineup has a permanently wired loop (JCM 6.37 dB,
// OR120 7.81 dB, Rockerverb 6.65 dB). That is this slice's acceptance bar and it
// is TOPOLOGY, not a voicing curve — see docs §68.4. The loop is owned by
// MesaPowerAmp; MesaPreamp exposes the mode so the composed amp can route it.
//
// Convention: 1.0f == 1.0 V. Platform-free C++17 (no OS/browser/Emscripten deps).

#ifndef CLIPPER_DSP_MESA_PREAMP_H
#define CLIPPER_DSP_MESA_PREAMP_H

#include <array>
#include <vector>

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/TriodeStage.h"

namespace clipper::dsp {

// ---------------------------------------------------------------------------
// The five switched states, in the drawing's own order and naming (sheet
// `mbdr7`). These are NOT a "channel" plus a "mode" — the sheet enumerates the
// combinations that actually exist, and two of the twelve conceivable ones do
// not (there is no RED CLEAN, and no ORANGE VINTAGE).
// ---------------------------------------------------------------------------
enum class MesaMode : int {
    OrangeClean = 0,   // OR CLN  — cathode caps OFF, master bypassed, MOST feedback
    OrangeNormal = 1,  // OR NORM — the "vintage" orange voice
    OrangeModern = 2,  // OR MOD  — feedback OFF, treble roll-off OFF
    RedVintage = 3,    // RED VINT— feedback ON, treble roll-off ON
    RedModern = 4,     // RED NORM— feedback OFF, treble roll-off OFF: the Recto voice
    MESA_MODE_COUNT = 5,
};

// ---------------------------------------------------------------------------
// The truth table, transcribed VERBATIM from sheet `mbdr7`. It is data, not a
// pile of if-statements, so the test can assert it row by row against the
// drawing and a future revision can be diffed rather than re-read.
//
// Fields are named for the drawing's LDR numbers so the mapping is checkable.
// LDR 11/12/13 (FX send/return/bypass) follow the selected mode and are not a
// voicing choice, so they are absent.
// ---------------------------------------------------------------------------
struct MesaModeRow {
    bool ldr1_redInputSel;    // LDR1  INPUT SELECT (RED)
    bool ldr2_orInputSel;     // LDR2  INPUT SELECT (ORANGE)
    bool ldr3_cathodeCaps;    // LDR3a/3b V2A + V1A CATHODE CAP (ganged on the sheet)
    bool ldr4_v1aOutputPad;   // LDR4  V1A OUTPUT PAD
    bool ldr5_redGainPot;     // LDR5  RD GAIN POT
    bool ldr6_orGainPot;      // LDR6  OR GAIN POT
    bool ldr7_trebleRollOff;  // LDR7  TREBLE ROLL OFF
    bool ldr8_redToneCaps;    // LDR8  RD TONE CAPS
    bool ldr9_orToneCaps;     // LDR9  OR TONE CAPS
    bool ldr10_v3CathodeCap;  // LDR10 V3 CATHODE CAP
    bool ldr14_redMaster;     // LDR14 RD MASTER POT
    bool ldr15_orMaster;      // LDR15 OR MASTER POT
    bool ldr16_orMasterBypass;// LDR16 OR MASTER BYPASS
    bool ldr19_feedback;      // LDR19 FEEDBACK
    bool ldr20_moreFeedback;  // LDR20 MORE FEEDBACK
};

// Indexed by MesaMode. Column order in the drawing is
// OR NORM | OR CLN | OR MOD | RED NORM | RED VINT; this table is re-ordered to
// the enum above, and test_mesa_amp.cpp asserts every cell against the drawing.
inline constexpr std::array<MesaModeRow, 5> kMesaModeTable{{
    // OR CLN
    {false, true, false, false, false, true, true, false, true, false,
     false, true, true, true, true},
    // OR NORM
    {false, true, true, true, false, true, true, false, true, true,
     false, true, false, true, false},
    // OR MOD
    {false, true, true, true, false, true, false, false, true, true,
     false, true, false, false, false},
    // RED VINT
    {true, false, true, true, true, false, true, true, false, true,
     true, false, false, true, false},
    // RED MODERN (the sheet's "RED NORM")
    {true, false, true, true, true, false, false, true, false, true,
     true, false, false, false, false},
}};

inline constexpr const MesaModeRow& mesaModeRow(MesaMode m) {
    return kMesaModeTable[static_cast<size_t>(m)];
}

inline constexpr bool mesaModeIsRed(MesaMode m) {
    return m == MesaMode::RedVintage || m == MesaMode::RedModern;
}

// ---------------------------------------------------------------------------
// MesaInputNetwork — the node the whole amp's character starts at, and the one
// place where RED and ORANGE genuinely differ BEFORE the tone stack.
//
//   PL --Ccoup 20n-- X1
//   X1 --Rload 2M2-- GND
//   X1 --(Rpad 680k || Cpad 2n)-- GND        present only when LDR4 is ON
//   X1 --Csel 82p--  W   (RED)      or       X1 --Rsel 2M2-- W  (ORANGE)
//   W  --Cpot 1n--   pot top ; pot = 1M, wiper = output
//
// THE TWO INPUT SELECTS ARE NOT THE SAME KIND OF COMPONENT, and that is the
// finding this network exists to carry. RED couples through 82p — a capacitor,
// so its impedance FALLS with frequency and the path is a high-pass into the 1M
// pot; ORANGE couples through a 2M2 RESISTOR, a flat 3.2:1 pad against the same
// pot. So the red channel is not merely "more gain": it is level-matched only at
// high frequency and rolls its own bottom off before the first gain pot, which
// is exactly the "tight, no flub in the preamp" behaviour a Recto is bought for.
// Measured in docs §68.5; asserted, so a later slice cannot tidy the 82p into a
// coupling cap of convenience.
// ---------------------------------------------------------------------------
class MesaInputNetwork {
public:
    // --- ALL TRANSCRIBED (sheet `mbdr1`) ------------------------------------
    static constexpr double kCcoup = 20.0e-9;   // V1A plate coupling
    static constexpr double kRload = 2.2e6;     // X1 -> GND
    static constexpr double kRpad = 680.0e3;    // LDR4 pad leg
    static constexpr double kCpad = 2.0e-9;     // in parallel with it
    static constexpr double kCsel = 82.0e-12;   // RED input select
    static constexpr double kRsel = 2.2e6;      // ORANGE input select
    static constexpr double kCpot = 1.0e-9;     // into the gain pot
    static constexpr double kRpot = 1.0e6;      // both gain pots
    static constexpr double kOpenOhms = 1.0e9;

    void prepare(double sampleRate);
    void reset();
    void setSourceImpedance(double rs);
    void setMode(MesaMode m);
    void setWiper(double g);  // post-taper gain-pot fraction in [0,1]
    void snapKnobs();
    void process(const float* in, float* out, int numFrames);

    // ADR 006: every cap companion here rests at exactly zero.
    double maxAbsRestingState() const {
        return maxAbsState(vC_, iC_, vP_, iP_, vS_, iS_, vG_, iG_);
    }

    // Analytic |H(jw)| from the same netlist — the discretization check.
    static double magnitudeAt(double freqHz, MesaMode m, double g, double rs);

private:
    void rebuild();
    // PL=0 (V1A's plate), X1=1 (the shared select node), SS=2 (after the select
    // element), PP=3 (the gain pot's top), WW=4 (its wiper — the output).
    static constexpr int N = 5;
    static constexpr double kKnobSmoothSeconds = 0.008;
    static constexpr int kCtrlBlock = 32;

    double sampleRate_ = 44100.0;
    double T_ = 1.0 / 44100.0;
    double rs_ = 40.0e3;
    MesaMode mode_ = MesaMode::RedModern;
    OnePoleSmootherD gainSm_;
    double gain_ = 0.5;
    bool knobsMoving_ = false;
    int ctrlCounter_ = 0;
    double geqC_ = 0.0, geqP_ = 0.0, geqS_ = 0.0, geqG_ = 0.0;
    std::array<std::array<double, N>, N> Ginv_{};
    double vC_ = 0.0, iC_ = 0.0;
    double vP_ = 0.0, iP_ = 0.0;
    double vS_ = 0.0, iS_ = 0.0;
    double vG_ = 0.0, iG_ = 0.0;
    bool dirty_ = true;
};

// ---------------------------------------------------------------------------
// MesaInterstage — the two plain interstage networks (V2A->V2B and V2B->V3A).
// Both are the same shape and differ only in parts:
//
//   PL --Ccoup 20n-- NJ --Rser-- G ; G --Rleak-- GND
//
//     V2A -> V2B :  20n, Rser 470k, Rleak 1M
//     V2B -> V3A :  20n, Rser 220k, Rleak 330k
//
// (The 470k/220k are the drawing's series grid resistors; the leak is the next
// grid's DC return. Kept OUT of TriodeStage's own coupling for exactly the
// reason §63.3 gives — one (Cc,Rgl) pair cannot serve two different couplings,
// so each real cap lives ONCE, here, and each TriodeStage carries a transparent
// 1uF coupling with Rgl set to this network's real resistive input.)
// ---------------------------------------------------------------------------
class MesaInterstage {
public:
    struct Config {
        double Ccoup = 20.0e-9;
        double Rser = 470.0e3;
        double Rleak = 1.0e6;
    };

    void configure(const Config& c);
    void prepare(double sampleRate);
    void reset();
    void setSourceImpedance(double rs);
    void process(const float* in, float* out, int numFrames);

    double maxAbsRestingState() const { return maxAbsState(vC_, iC_); }
    static double magnitudeAt(double freqHz, const Config& c, double rs);

private:
    void rebuild();
    static constexpr int N = 3;  // PL=0, NJ=1, G=2
    Config cfg_{};
    double sampleRate_ = 44100.0;
    double T_ = 1.0 / 44100.0;
    double rs_ = 40.0e3;
    double geqC_ = 0.0;
    std::array<std::array<double, N>, N> Ginv_{};
    double vC_ = 0.0, iC_ = 0.0;
    bool dirty_ = true;
};

// ---------------------------------------------------------------------------
// MesaToneStack — an FMV (Fender/Marshall/Vox) stack, transcribed, WITH the
// master pot and the next grid's leak inside the same MNA.
//
//   IN  = V3B's CATHODE (a follower — so this stack is driven from a LOW source
//         impedance, unlike the OR120's and the Rockerverb's, both of which hang
//         off a plate. That is a real structural difference and it is why this
//         amp's tone controls interact less than either Orange's.)
//   Ct  : IN - T      680p (RED) / 500p (ORANGE)      <-- the ONLY value that
//   Rsl : IN - A      47k                                 differs between the
//   Cb  : A  - B      20n                                 two channels' stacks
//   Cm  : A  - MW     20n
//   RT  : T -((1-t)RT)- OUT -((t)RT)- B     TREBLE 220k
//   RB  : B -((b)RB)- C                     BASS 1M, wiper tied to its top
//   RM  : C -((1-m)RM)- MW -((m)RM)- GND    MIDDLE 25k
//   RV  : OUT -((1-v)RV)- VW -((v)RV)- GND  MASTER 1M
//   Rnx : VW - GND    1M                    the next grid's leak
//
// BOTH CHANNELS' STACKS ARE THE SAME TOPOLOGY AND DIFFER IN ONE CAPACITOR. That
// is worth saying plainly because it is the opposite of what players describe:
// the red channel's extra aggression is NOT its tone stack (680p vs 500p is
// 2.6 dB of treble-branch corner), it is the 82p input select, the cathode caps
// and the absent feedback loop. Measured in docs §68.5.
//
// NOTE FOR A FUTURE SLICE: this network is the SAME SHAPE as
// RockerverbToneStack (down to Cm landing on the MID pot's WIPER rather than its
// top). Three FMV stacks now exist in this repo (JCM, Rockerverb, Mesa) and a
// shared component is defensible — but ADR 021's rule applies: extract it when a
// slice can MEASURE that the union is not a union, not because the pictures
// rhyme. Deliberately NOT extracted here.
// ---------------------------------------------------------------------------
class MesaToneStack {
public:
    // --- ALL TRANSCRIBED (sheet `mbdr1`) ------------------------------------
    static constexpr double kCtRed = 680.0e-12;
    static constexpr double kCtOrange = 500.0e-12;
    static constexpr double kRslope = 47.0e3;
    static constexpr double kCb = 20.0e-9;
    static constexpr double kCm = 20.0e-9;
    static constexpr double kRT = 220.0e3;
    static constexpr double kRB = 1.0e6;
    static constexpr double kRM = 25.0e3;
    static constexpr double kRV = 1.0e6;
    static constexpr double kRnext = 1.0e6;

    void prepare(double sampleRate);
    void reset();
    void setSourceImpedance(double rs);
    void setMode(MesaMode m);
    void setKnobs(double bass, double mid, double treble, double master);
    void snapKnobs();
    void process(const float* in, float* out, int numFrames);

    double maxAbsRestingState() const {
        return maxAbsState(vT_, iT_, vB_, iB_, vM_, iM_);
    }

    // Probe::Out is the TREBLE WIPER (the network's own output, the fair
    // analogue of §57.4's James probe); Probe::Wiper is what process() emits.
    enum class Probe { Out, Wiper };
    static double magnitudeAt(double freqHz, MesaMode m, double bass, double mid,
                              double treble, double master, double rs,
                              Probe probe = Probe::Wiper);

    static constexpr double trebleCapFor(MesaMode m) {
        return mesaModeIsRed(m) ? kCtRed : kCtOrange;
    }

private:
    void rebuild();
    // IN=0, T=1, A=2, B=3, C=4, MW=5, OUT=6, VW=7.
    static constexpr int N = 8;
    static constexpr double kKnobSmoothSeconds = 0.008;
    static constexpr int kCtrlBlock = 32;

    double sampleRate_ = 44100.0;
    double T_ = 1.0 / 44100.0;
    double rs_ = 1.0e3;  // a cathode follower — LOW, unlike the two Oranges
    MesaMode mode_ = MesaMode::RedModern;
    OnePoleSmootherD bassSm_, midSm_, trebleSm_, masterSm_;
    double bass_ = 0.5, mid_ = 0.5, treble_ = 0.5, master_ = 0.35;
    bool knobsMoving_ = false;
    int ctrlCounter_ = 0;
    double geqT_ = 0.0, geqB_ = 0.0, geqM_ = 0.0;
    std::array<std::array<double, N>, N> Ginv_{};
    double vT_ = 0.0, iT_ = 0.0;
    double vB_ = 0.0, iB_ = 0.0;
    double vM_ = 0.0, iM_ = 0.0;
    bool dirty_ = true;
};

// ---------------------------------------------------------------------------
// The composed Dual Rectifier preamp.
// ---------------------------------------------------------------------------
class MesaPreamp {
public:
    enum ParamId : int {
        PARAM_GAIN = 0,
        PARAM_MASTER = 1,
        PARAM_BASS = 2,
        PARAM_MID = 3,
        PARAM_TREBLE = 4,
        PARAM_COUNT = 5,
    };

    enum Stage : int { V1A = 0, V2A = 1, V2B = 2, V3A = 3, V3B = 4, STAGE_COUNT = 5 };

    MesaPreamp();

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);
    int oversampling() const { return oversampling_; }
    int latencySamples() const;

    void setParameter(int paramId, float value);
    void setMode(MesaMode m);
    MesaMode mode() const { return mode_; }
    void reset();
    void process(const float* in, float* out, int numFrames);

    // --- Introspection (measurement / tests) --------------------------------
    double stageQuiescentPlate(int stage) const;
    double stageQuiescentCathode(int stage) const;
    double stageQuiescentCurrent(int stage) const;
    double plateSourceImpedance(int stage) const {
        return plateRout_[static_cast<size_t>(stage)];
    }
    const TriodeStage::Config& stageConfig(int stage) const { return cfg_[stage]; }
    MesaToneStack& toneStack() { return tone_; }
    MesaInputNetwork& inputNetwork() { return input_; }

    // --- The transcribed supply ladder (sheet `mbdr4`) -----------------------
    // A 460 --choke-- B 454 --2k7-- C 422 --15k-- D 406 --22k-- E 402
    // The choke is ideal at DC, so A and B are one node here (the §57 precedent
    // for the OR120's HT choke). The marked voltages are the ABSOLUTE reference
    // the DC test asserts against.
    static constexpr double kVrailA = 460.0;
    static constexpr double kRbc = 2.7e3;
    static constexpr double kRcd = 15.0e3;
    static constexpr double kRde = 22.0e3;

    // NODE C DOES NOT ONLY FEED THIS CLASS. Sheets `mbdr2` and `mbdr3` show the
    // phase inverter (V5A/V5B) and BOTH effects-loop triodes (V4A/V4B) tapping
    // the same C rail, and their standing draw is most of what pulls C down. The
    // PI lives in MesaPowerAmp and the loop is bypassed, so neither is in this
    // class's ladder — but leaving their current out puts C ~23 V high and every
    // stage above it with it.
    //
    // So it is DERIVED FROM THE SHEETS' OWN MARKED VOLTAGES, not fitted:
    //     V5B  (422 - 280) / 82k  = 1.7317 mA
    //     V5A  (422 - 280) / 90k  = 1.5778 mA
    //     V4A  (422 - 283) / 120k = 1.1583 mA
    //     V4B  210 / (22k + 82k)  = 2.0192 mA   (a follower; marked Vk 210 V)
    //                               ---------
    //                               6.4870 mA
    // Every term is a marked node voltage over a transcribed resistor, i.e. Ohm's
    // law on the drawing. If a later slice moves the PI's supply into a shared
    // ladder, delete this and let the real current flow — do NOT re-tune it.
    static constexpr double kIdownstreamC = 6.4870e-3;
    double supplyC() const { return railC_; }
    double supplyD() const { return railD_; }
    double supplyE() const { return railE_; }

    // The house audio law, k = 4 (docs §51). The sheet gives no taper letters.
    static constexpr double kLogTaperK = 4.0;
    static double logTaper(double x);
    double gainWiper() const;

    double lastOutputPeak() const { return lastOutPeak_; }

private:
    void configureStages();
    void solveSupply();
    void primeSmoothers();
    void pushKnobs();
    void applyMode();

    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;

    std::array<TriodeStage, STAGE_COUNT> stage_;
    std::array<TriodeStage::Config, STAGE_COUNT> cfg_{};
    std::array<double, STAGE_COUNT> plateRout_{};
    MesaInputNetwork input_;
    std::array<MesaInterstage, 2> net_;
    MesaToneStack tone_;

    double railC_ = 422.0, railD_ = 406.0, railE_ = 402.0;
    double followerGridBias_ = 213.0;  // V3A's plate; the sheet's marked value
    MesaMode mode_ = MesaMode::RedModern;
    double gain_ = 0.5, master_ = 0.35;
    double bass_ = 0.5, mid_ = 0.5, treble_ = 0.5;
    bool primed_ = false;

    std::vector<float> buf_;
    std::vector<float> buf2_;
    double lastOutPeak_ = 0.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_MESA_PREAMP_H
