// Clipper — portable DSP core (M10.7, docs §63).
//
// RockerverbPreamp: the DIRTY channel of an Orange Rockerverb — FOUR 12AX7 gain
// stages, a GANGED dual GAIN pot, a Marshall-lineage FMV tone stack and a
// post-stack VOLUME. This is the modern Orange, and it is the deliberate
// counterweight to the OR120 (docs §57) the way the OR120 was to the JCM800.
//
//   guitar in -> 68k grid stopper (1M leak)
//     -> S1  V9   Ra 100k to B2, Rk 1k5 || 10uF                (bypassed)
//     -> C1  1n   -> R30 220k -> [R1 220k || C2 470p] || GAIN-1 1M log
//                    with C16 100p bridging the pot's top lug to its WIPER
//     -> S2  V10  Ra 100k to B2 (100p across it), Rk 1k || 10uF
//     -> C13 2n2  -> R31 220k -> R32 470k || GAIN-2 1M log     (the SAME knob)
//     -> S3  V11  Ra 100k to B1 (100p across it), Rk 2k2 || 10uF
//     -> C7  4n7  -> R6 470k -> R5 220k                        (a 0.319 divider)
//     -> S4  V12  Ra 100k to B1, Rk 1k5 UNBYPASSED             (the cold stage)
//     -> FMV tone stack (560p / 39k / 22n / 22n; T 250k lin, B 500k log, M 25k lin)
//     -> VOLUME 1M LINEAR -> the phase inverter's grid
//
// ===========================================================================
// PROVENANCE — read docs §63.1 before changing a constant
// ===========================================================================
// EVERY component value above is TRANSCRIBED from a real netlist:
// `Tests/Examples/Orange Rockerverb 50 Preamp.schx`, an example circuit shipped
// inside LiveSPICE (github.com/dsharlet/LiveSPICE). It is a schematic FILE —
// components with values, positions, rotations and wires — so the netlist was
// recovered node by node (terminal offsets out of LiveSPICE's own LayoutSymbol
// code, the rotation/flip transform out of Schematic/Symbol.cs), not eyeballed
// off a picture. The designators (R1..R43, C1..C26, V9..V12) are that file's.
//
// The proxy in this build container permits github.com ONLY — el34world.com,
// orangeamps.com, dirtboxlayouts and Wikipedia all fail at CONNECT with 403, and
// WebFetch 403s on every one of them. So the Orange factory sheet was NOT read.
// What is a documented RECONSTRUCTION, named rather than hidden:
//   * the pots' taper LAWS (the file gives the letter — Gain/Bass Logarithmic,
//     Treble/Middle/Volume Linear — and LiveSPICE's own generic log curve is
//     k = 2; this project's house audio law is k = 4, docs §51, and that is what
//     ships, for consistency with every other log pot in the repo);
//   * the phase inverter, the power section, the supply and the reverb placement
//     (the file is a PREAMP only) — see RockerverbPowerAmp.h;
//   * the PI grid leak that loads the VOLUME pot (1 M, the house value).
//
// THE VOLUME POT IS LINEAR AND IT SHIPS LINEAR. The file marks Gain and Bass
// Logarithmic and Treble/Middle/Volume Linear, i.e. its author distinguished
// them deliberately. A linear master is unusual and puts the useful range low on
// the travel; that is MEASURED and REPORTED in docs §63.5 rather than "fixed"
// with an audio taper this circuit gives no support for.
//
// THE TWO GAIN POTS ARE ONE KNOB, AND THAT IS SOURCED. Both carry
// Group="Gain" in the schematic file, which is LiveSPICE's own gang marker, and
// both default to the same wipe. One knob drives both wipers here.
//
// ===========================================================================
// ONE MODELLING DEPARTURE, stated up front (docs §63.3)
// ===========================================================================
// TriodeStage conflates its INPUT coupling and its OUTPUT coupling: one (Cc,Rgl)
// pair serves both, so a cascade applies every interstage high-pass TWICE. In a
// uniform cascade (the 2204's 22n/1M = 7 Hz, the OR120's 68n/953k = 2.5 Hz) that
// is inaudible. Here the real interstage caps are 1n into 400k (398 Hz!), 2n2
// into 540k (134 Hz) and 4n7 into 690k (49 Hz), so a doubled corner would be a
// gross error. So every real coupling cap lives ONCE, inside the interstage MNA
// where its divider is, and each TriodeStage carries a deliberately transparent
// 1 uF coupling with Rgl set to the network's real resistive input (which is
// what loads the plate). The cost is that PREAMP grid BLOCKING (the slow bias
// shift as the coupling cap charges) is not modelled in this voice; grid
// CONDUCTION clamping is, through each grid's real source impedance in Rg, and
// the EL34 grid blocking that dominates a cranked amp (§18) is fully present.
// Named as a follow-up in §63.11: give TriodeStage independent input/output
// coupling configs. Do NOT "fix" this by setting Cc to the real cap here — that
// re-introduces the doubled corner.
//
// Convention: 1.0f == 1.0 V. Platform-free C++17 (no OS/browser/Emscripten deps).

#ifndef CLIPPER_DSP_ROCKERVERB_PREAMP_H
#define CLIPPER_DSP_ROCKERVERB_PREAMP_H

#include <array>
#include <vector>

#include "clipper/dsp/Denormal.h"
#include "clipper/dsp/OnePoleSmoother.h"
#include "clipper/dsp/TriodeStage.h"

namespace clipper::dsp {

// ---------------------------------------------------------------------------
// RockerverbInterstage — ONE class for the three interstage networks, because
// all three are the same shape and only their parts differ. Nodes:
//
//   PL -- (Cshunt to GND, the plate snubber)        PL is the driving PLATE node
//   PL -- Ccoup -- NJ -- Rser -- M                  M is the pot's / divider's top
//   M  -- Rgnd to GND ; M -- Cgnd to GND
//   M  -- (1-g)*Rpot -- W -- g*Rpot -- GND          W is the WIPER (the output)
//   M  -- Cbright -- W                              the GAIN pot's bright cap
//
// With Rpot <= 0 the pot is absent: M and W are joined by kShortOhms and W hangs
// on a 1 G leak, so the output node is always W and one matrix build serves all
// three networks. (1 ohm into 1 G is -8.7e-9 dB — the same stamped-short trick
// FacNetwork uses, docs §57.)
//
// The pot is INSIDE the matrix deliberately: it is the network's load, so its
// position changes the divider AND the bright cap's corner together. Knob
// smoothing follows MarshallToneStack exactly (audit finding 6, docs §35) —
// per-sample one-pole smoothing, matrix re-derived at a 32-sample control rate,
// cap states carried across.
// ---------------------------------------------------------------------------
class RockerverbInterstage {
public:
    struct Config {
        double Cshunt = 0.0;    // plate snubber to AC ground (C4 / C6 = 100p)
        double Ccoup = 1.0e-9;  // the real interstage coupling cap
        double Rser = 220.0e3;  // series resistor after it
        double Rgnd = 220.0e3;  // shunt to ground at the pot's top
        double Cgnd = 0.0;      // cap in parallel with Rgnd (C2 = 470p)
        double Rpot = 0.0;      // 0 == no pot (the S3->S4 divider)
        double Cbright = 0.0;   // top-lug-to-wiper bright cap (C16 = 100p)
    };

    static constexpr double kShortOhms = 1.0;
    static constexpr double kOpenOhms = 1.0e9;

    void configure(const Config& c);
    void prepare(double sampleRate);
    void reset();
    void setSourceImpedance(double rs);
    void setWiper(double g);  // post-taper pot fraction in [0,1]
    void snapKnobs();
    void process(const float* in, float* out, int numFrames);

    // Anti-denormal diagnostic (Denormal.h, docs §33, ADR 006). All four cap
    // companions rest at exactly zero, so a silent tail must leave this EXACTLY 0.
    double maxAbsRestingState() const {
        return maxAbsState(vS_, iS_, vC_, iC_, vG_, iG_, vB_, iB_);
    }

    // Steady-state |H(jw)| straight off the netlist (independent complex nodal
    // solve) — the discretization check.
    static double magnitudeAt(double freqHz, const Config& c, double g, double rs);

private:
    void rebuild();
    static constexpr int N = 4;  // PL=0, NJ=1, M=2, W=3
    static constexpr double kKnobSmoothSeconds = 0.008;
    static constexpr int kCtrlBlock = 32;

    Config cfg_{};
    double sampleRate_ = 44100.0;
    double T_ = 1.0 / 44100.0;
    double rs_ = 40.0e3;
    OnePoleSmootherD gainSm_;
    double gain_ = 0.5;
    bool knobsMoving_ = false;
    int ctrlCounter_ = 0;
    double geqS_ = 0.0, geqC_ = 0.0, geqG_ = 0.0, geqB_ = 0.0;
    std::array<std::array<double, N>, N> Ginv_{};
    double vS_ = 0.0, iS_ = 0.0;   // Cshunt
    double vC_ = 0.0, iC_ = 0.0;   // Ccoup
    double vG_ = 0.0, iG_ = 0.0;   // Cgnd
    double vB_ = 0.0, iB_ = 0.0;   // Cbright
    bool dirty_ = true;
};

// ---------------------------------------------------------------------------
// RockerverbToneStack — the FMV (Fender/Marshall/Vox) tone stack the schematic
// file carries, WITH the VOLUME pot and the phase inverter's grid leak inside
// the same MNA. Transcribed:
//
//   IN  = V12's plate (driven behind its own plate impedance — there is no
//         cathode follower anywhere in this preamp)
//   Ct  : IN - T             560p       (C24, the treble cap)
//   Rsl : IN - A             39k        (R43, the slope resistor)
//   Cb  : A  - B             22n        (C25 — in SERIES, not across the pot)
//   Cm  : A  - MW            22n        (C26 — to the MID pot's WIPER)
//   RT  : T -((1-t)RT)- OUT -((t)RT)- B    TREBLE 250k LINEAR
//   RB  : B -((b)RB)- C                    BASS 500k LOG, wiper tied to its top
//   RM  : C -((1-m)RM)- MW -((m)RM)- GND   MIDDLE 25k LINEAR
//   RV  : OUT -((1-v)RV)- VW -((v)RV)- GND VOLUME 1M LINEAR
//   Rpi : VW - GND          1M         (the PI grid leak — RECONSTRUCTION)
//
// Two things about this netlist are worth stating plainly, because they are the
// whole point of the voice:
//
//  1. IT IS AN FMV. The OR120's network is a JAMES / passive Baxandall whose two
//     parallel shelving branches leave the midrange standing (§57.4 measures a
//     mid BUMP). An FMV's slope resistor and its treble branch put a NOTCH in
//     the middle. That is the acceptance bar of this slice, and it is topology,
//     not a tweak: see docs §63.4.
//  2. THE MID POT IS NOT A RHEOSTAT. Both of its sections sit between node C and
//     ground, so the resistance C->GND is 25k at EVERY knob position; what the
//     knob moves is where the 22n Cm TAPS INTO that 25k. At MIDDLE 0 the cap
//     shunts the slope node almost to ground (everything but the treble branch
//     is pulled down); at MIDDLE 1 it lands on node C, the canonical FMV
//     position. Transcribed literally — it is asserted so nobody "tidies" it
//     into a rheostat.
// ---------------------------------------------------------------------------
class RockerverbToneStack {
public:
    // --- ALL TRANSCRIBED (the .schx netlist) --------------------------------
    static constexpr double kCt = 560.0e-12;  // C24
    static constexpr double kRslope = 39.0e3; // R43
    static constexpr double kCb = 22.0e-9;    // C25
    static constexpr double kCm = 22.0e-9;    // C26
    static constexpr double kRT = 250.0e3;    // Treble, LINEAR
    static constexpr double kRB = 500.0e3;    // Bass, LOG
    static constexpr double kRM = 25.0e3;     // Middle, LINEAR
    static constexpr double kRV = 1.0e6;      // Volume, LINEAR
    // RECONSTRUCTION: the phase inverter's grid leak. Not in the preamp file.
    static constexpr double kRpi = 1.0e6;

    void prepare(double sampleRate);
    void reset();
    void setSourceImpedance(double rs);
    // bass/mid/treble/volume are POST-taper pot fractions in [0,1].
    void setKnobs(double bass, double mid, double treble, double volume);
    void snapKnobs();
    void process(const float* in, float* out, int numFrames);

    double maxAbsRestingState() const {
        return maxAbsState(vT_, iT_, vB_, iB_, vM_, iM_);
    }

    // Analytic |H(jw)| from the same netlist. Probe::Out is the TREBLE WIPER —
    // the tone network's own output, the fair analogue of the OR120's James
    // probe in §57.4(a). Probe::Wiper is what process() emits (the VOLUME
    // wiper, i.e. the PI's grid).
    enum class Probe { Out, Wiper };
    static double magnitudeAt(double freqHz, double bass, double mid, double treble,
                              double volume, double rs, Probe probe = Probe::Wiper);

private:
    void rebuild();
    // Node order: IN=0, T=1, A=2, B=3, C=4, MW=5, OUT=6, VW=7.
    static constexpr int N = 8;
    static constexpr double kKnobSmoothSeconds = 0.008;
    static constexpr int kCtrlBlock = 32;

    double sampleRate_ = 44100.0;
    double T_ = 1.0 / 44100.0;
    double rs_ = 40.0e3;
    OnePoleSmootherD bassSm_, midSm_, trebleSm_, volSm_;
    double bass_ = 0.5, mid_ = 0.5, treble_ = 0.5, vol_ = 0.5;
    bool knobsMoving_ = false;
    int ctrlCounter_ = 0;
    double geqT_ = 0.0, geqB_ = 0.0, geqM_ = 0.0;
    std::array<std::array<double, N>, N> Ginv_{};
    double vT_ = 0.0, iT_ = 0.0;   // Ct
    double vB_ = 0.0, iB_ = 0.0;   // Cb
    double vM_ = 0.0, iM_ = 0.0;   // Cm
    bool dirty_ = true;
};

// ---------------------------------------------------------------------------
// The composed Rockerverb dirty-channel preamp.
// ---------------------------------------------------------------------------
class RockerverbPreamp {
public:
    enum ParamId : int {
        PARAM_GAIN = 0,    // the GANGED dual 1M log pot (both wipers)
        PARAM_VOLUME = 1,  // the post-stack 1M LINEAR master
        PARAM_BASS = 2,
        PARAM_MID = 3,
        PARAM_TREBLE = 4,
        PARAM_COUNT = 5,
    };

    enum Stage : int { S1 = 0, S2 = 1, S3 = 2, S4 = 3, STAGE_COUNT = 4 };

    RockerverbPreamp();

    void prepare(double sampleRate, int maxBlockSize);
    void setOversampling(int factor);
    int oversampling() const { return oversampling_; }

    int latencySamples() const {
        int n = 0;
        for (int s = S1; s <= S4; ++s)
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
    double plateSourceImpedance(int stage) const {
        return plateRout_[static_cast<size_t>(stage)];
    }
    const TriodeStage::Config& stageConfig(int stage) const { return cfg_[stage]; }
    RockerverbToneStack& toneStack() { return tone_; }
    RockerverbInterstage& interstage(int i) { return net_[static_cast<size_t>(i)]; }
    static const RockerverbInterstage::Config& interstageConfig(int i);

    // --- The transcribed supply chain ---------------------------------------
    // V8 400 V --R9 10k--> B1 (22uF) --R42 10k--> B2 (22uF)
    //   B1 feeds S3 + S4's plate loads; B2 feeds S1 + S2's.
    static constexpr double kVsupplyRaw = 400.0;  // the file's voltage source
    static constexpr double kRdrop1 = 10.0e3;     // R9
    static constexpr double kRdrop2 = 10.0e3;     // R42
    static constexpr double kRa = 100.0e3;        // every stage's plate load
    double supplyB1() const { return b1_; }
    double supplyB2() const { return b2_; }

    // Both GAIN gangs are 1M LOG. The taper LETTER is in the file, the taper LAW
    // is not, so both keep the house audio law with k = 4 (docs §51's
    // kMasterTaperK) — the same constant every other log pot in this project
    // uses. The BASS pot is log too and shares it. Nothing here is fitted to a
    // breakup target. VOLUME, TREBLE and MIDDLE are LINEAR in the file and ship
    // linear: the knob IS the wiper fraction.
    static constexpr double kLogTaperK = 4.0;
    static double logTaper(double x);
    double gainWiper() const;
    double bassWiper() const;

    double lastOutputPeak() const { return lastOutPeak_; }

private:
    void configureStages();
    void solveSupply();
    void primeSmoothers();
    void pushKnobs();

    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;

    std::array<TriodeStage, STAGE_COUNT> stage_;
    std::array<TriodeStage::Config, STAGE_COUNT> cfg_{};
    std::array<RockerverbInterstage, 3> net_;
    std::array<double, STAGE_COUNT> plateRout_{};
    RockerverbToneStack tone_;

    double b1_ = 360.0, b2_ = 340.0;
    double gain_ = 0.5, volume_ = 0.5;
    double bass_ = 0.5, mid_ = 0.5, treble_ = 0.5;
    bool primed_ = false;

    std::vector<float> buf_;
    double lastOutPeak_ = 0.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_ROCKERVERB_PREAMP_H
