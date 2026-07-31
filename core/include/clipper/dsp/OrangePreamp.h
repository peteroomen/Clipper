// Clipper — portable DSP core (M10.3, docs §57).
//
// OrangePreamp: the front half of an early-70s Orange OR120 "Overdrive" head —
//
//   guitar in
//     -> V1A   12AX7 common cathode, Ra 100k, Rk 820 || 25uF (FULLY bypassed)
//     -> 0.022uF coupling + 1M VOLUME pot            (the ONLY gain control)
//     -> V1B   12AX7 common cathode, Ra 100k, Rk 820 || 25uF
//     -> F.A.C. series coupling cap (6-position rotary)
//     -> JAMES / passive-Baxandall tone stack (BASS + TREBLE, no MID)
//   B+ ~ 320 V throughout.
//
// The power section (OrangePowerAmp) carries the THIRD triode — the driver that is
// DC-coupled to the cathodyne phase inverter — because the global feedback returns
// to that driver's cathode, so the driver and the loop belong to the same module.
//
// ===========================================================================
// WHY THIS IS NOT A RE-SKINNED JCM800 (docs §57)
// ===========================================================================
// The OR120 is EL34 push-pull like the 2204 and the power machinery is shared, so
// every difference that matters is HERE and in the inverter:
//
//  1. The stack is a JAMES network (passive Baxandall), not an FMV. An FMV puts a
//     structural NOTCH in the midrange (the JCM's own stack measures -6.0 dB of
//     mid scoop at noon by the §57 metric); a James stack is two PARALLEL shelving
//     branches that leave the midrange alone and measures +2.3 dB — a mid BUMP.
//     The mid-forward Orange character is therefore TOPOLOGICAL, not an EQ curve
//     bolted on. Both numbers are asserted in clipper_orange_tests.
//  2. There is NO BRIGHT CAP across the volume pot. The 2204's 470 pF top-lug cap
//     (docs §47) tilts drive into the second stage by +6-8 dB at mid travel; the
//     OR120 has none, and the amp's brightness comes from HF DRIVE inside the
//     feedback loop instead. That is one circuit reason for the thicker low end.
//  3. THREE gain triodes before the inverter, not four, and the James stack's
//     insertion loss at noon is ~-5 dB where the FMV's is ~-11 dB, so the OR120
//     needs far less make-up gain to reach the same drive.
//  4. NO MASTER VOLUME. The single VOLUME pot drives everything: the power section
//     is the overdrive, and breakup onset must TRACK THE KNOB (the docs §46 AC30
//     convention, asserted the same way).
//
// ===========================================================================
// PROVENANCE — read docs §57 before changing a constant
// ===========================================================================
// No OR120 schematic could be reached from this container (every host outside the
// search tool returned 403). What IS sourced: the topology (two ECC83 in the
// preamp with the volume between the first tube's two stages; James/Baxandall
// BASS+TREBLE with no mid; a six-position F.A.C. rotary of SERIES coupling caps
// spanning roughly 330 pF to 0.047 uF that takes away bass and gain as it climbs)
// and ONE component value: the early ('72/'74) amps' 1500 pF treble cap, against
// 330 pF in post-'74 units — which is exactly why owners describe the early
// treble knob as bringing the upper mids up with it. Everything else is a
// documented reconstruction on the standard James network (R1 = R3 = 100 k is the
// textbook choice) constrained by the network's own defining property: flat at
// noon. Do NOT "fix" a value here against a tone target; find the schematic.
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
// network, plus the F.A.C. series cap, as ONE nodal MNA with trapezoidal
// capacitor companions. Same numerical shape as MarshallToneStack (docs §14) so
// the two are directly comparable; the TOPOLOGY is what differs.
//
// Netlist (nodes IN, F, A, B, T, U, OUT; GND):
//   Rs*  : source - IN                 (* driven input: V1B's plate impedance)
//   Cfac : IN  - F                     (F.A.C. rotary series cap — 6 positions)
//   R1   : F   - A                     (bass branch series resistor)
//   RB   : A -((1-b)RB)- OUT -((b)RB)- B     (BASS pot; the wiper IS the output)
//   C1   : A   - B                     (across the whole bass track)
//   R3   : B   - GND
//   C2   : F   - T                     (treble branch series cap — the 1500 pF)
//   RT   : T -((1-t)RT)- OUT -((t)RT)- U     (TREBLE pot; wiper also the output)
//   C3   : U   - GND
//   RL   : OUT - GND                   (the next grid leak, 1 M)
//
// Two PARALLEL branches summing at the shared wiper node is the whole point: the
// bass branch is a resistive divider that C1 shorts out above its corner (so BASS
// acts only below it) and the treble branch is blocked by C2 below its corner (so
// TREBLE acts only above it). Neither branch does anything to the middle, which
// is why the mids survive — the "James mostly leaves the midrange alone".
//
// Knob smoothing follows MarshallToneStack exactly (audit finding 6, docs §35):
// pot fractions are one-pole smoothed per sample and the matrix is re-derived at a
// 32-sample control rate, with the cap states carried across (they are physical
// node voltages; a real wiper perturbs the network under them too).
// ---------------------------------------------------------------------------
class JamesToneStack {
public:
    // --- Component values. See the PROVENANCE banner above. -----------------
    static constexpr double kR1 = 100.0e3;      // bass branch series R (Ohm)
    static constexpr double kR3 = 100.0e3;      // bass branch shunt R  (Ohm)
    static constexpr double kRB = 1.0e6;        // BASS pot (Ohm)
    static constexpr double kC1 = 470.0e-12;    // across the bass track (F)
    // SOURCED: the early ('72/'74) OR120's treble cap. Post-'74 units use 330 pF,
    // and owners describe that later knob as "just the high frequencies" where the
    // early one "brings high mids up with it" — which is exactly this value's
    // corner, 1/(2*pi*kRT*kC2) = 424 Hz against the later 1.93 kHz.
    static constexpr double kC2 = 1500.0e-12;   // treble branch series cap (F)
    static constexpr double kRT = 250.0e3;      // TREBLE pot (Ohm)
    static constexpr double kC3 = 470.0e-12;    // treble branch shunt cap (F)
    static constexpr double kRL = 1.0e6;        // next-stage grid leak (Ohm)

    // F.A.C. — six positions of SERIES coupling cap, biggest (fattest, loudest)
    // at position 0. Sourced range: "coupling caps from like 330p to .047"; the
    // ladder between the two ends is a reconstruction. Position 5 is a ~1.5 kHz
    // corner into the stack's input impedance: the thin, quiet end of the switch.
    static constexpr int kFacPositions = 6;
    static constexpr double kFacCaps[kFacPositions] = {
        47.0e-9, 22.0e-9, 10.0e-9, 4.7e-9, 1.5e-9, 330.0e-12,
    };

    void prepare(double sampleRate);
    void reset();
    void setSourceImpedance(double rs);              // V1B plate impedance (Ohm)
    void setKnobs(double bass, double treble);       // each in [0,1]
    void setFacPosition(int pos);                    // 0..kFacPositions-1
    int facPosition() const { return fac_; }
    void process(const float* in, float* out, int numFrames);

    // Snap the knob smoothers onto their targets and re-derive the matrix with no
    // ramp (the owning preamp's deferred prime, and reset()).
    void snapKnobs();

    // Anti-denormal diagnostic (Denormal.h, docs §33). All eight cap companions
    // rest at exactly zero, so after a silent tail this must be EXACTLY 0.0.
    double maxAbsRestingState() const {
        return maxAbsState(vF_, iF_, v1_, i1_, v2_, i2_, v3_, i3_);
    }

    // Analytic steady-state response |H(jw)| of THIS netlist, for the
    // discretization check and the §57 mid-forward metric. Static so a test can
    // evaluate it without owning an instance (and so the JCM comparison in the
    // test is apples-to-apples with MarshallToneStack's own solve).
    static double magnitudeAt(double freqHz, double bass, double treble,
                              double rs, int facPos);

private:
    void rebuild();

    // Node order: IN=0, F=1, A=2, B=3, T=4, U=5, OUT=6.
    static constexpr int N = 7;
    static constexpr double kKnobSmoothSeconds = 0.008;
    static constexpr int kCtrlBlock = 32;

    double sampleRate_ = 44100.0;
    double T_ = 1.0 / 44100.0;
    double rs_ = 38.0e3;
    OnePoleSmootherD bassSm_, trebleSm_;
    double bass_ = 0.5, treble_ = 0.5;
    int fac_ = 1;  // default position 2 of 6 (the fat, usable one)
    bool knobsMoving_ = false;
    int ctrlCounter_ = 0;
    // Cap companion conductances: F.A.C., C1, C2, C3.
    double geqF_ = 0.0, geq1_ = 0.0, geq2_ = 0.0, geq3_ = 0.0;
    std::array<std::array<double, N>, N> Ginv_{};
    // Trapezoidal cap states (v across, i through) for the four caps.
    double vF_ = 0.0, iF_ = 0.0;
    double v1_ = 0.0, i1_ = 0.0;
    double v2_ = 0.0, i2_ = 0.0;
    double v3_ = 0.0, i3_ = 0.0;
    bool dirty_ = true;
};

// ---------------------------------------------------------------------------
// The composed OR120 preamp: V1A -> VOLUME -> V1B -> F.A.C. -> James stack.
// ---------------------------------------------------------------------------
class OrangePreamp {
public:
    enum ParamId : int {
        PARAM_VOLUME = 0,  // the ONE gain control (1M pot, audio taper)
        PARAM_BASS = 1,
        PARAM_TREBLE = 2,
        PARAM_FAC = 3,     // 0..1 -> one of six F.A.C. positions
        PARAM_COUNT = 4,
    };

    enum Stage : int { V1A = 0, V1B = 1, STAGE_COUNT = 2 };

    OrangePreamp();

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
    double plateSourceImpedance() const { return plateRout_; }
    const TriodeStage::Config& stageConfig(int stage) const { return cfg_[stage]; }
    JamesToneStack& toneStack() { return tone_; }

    // The VOLUME pot's steady-state scale (post-taper), for analytic checks.
    double volumeScale() const;
    // The 2204's audio-taper law shape, k = 4 (docs §51's kMasterTaperK). The
    // OR120's single volume gets the SAME k the JCM's MASTER kept: no field report
    // asks for anything else, and inventing a per-amp taper without one would be
    // fitting. Breakup onset is set by the circuit, not by re-tapering the knob.
    static constexpr double kVolumeTaperK = 4.0;
    static double volumeTaper(double x);
    // The volume network's DC series loss: V1A couples through 470 k into the 1 M
    // pot, exactly as the 2204 does — minus the bright cap, which the OR120 has
    // not got (see the header banner).
    static constexpr double kVolumeSeriesOhms = 470.0e3;
    static constexpr double kVolumePotOhms = 1.0e6;
    static constexpr double kVolumeDivider = 0.68;  // 1M / (1M + 470k)

    double lastOutputPeak() const { return lastOutPeak_; }

private:
    void configureStages();
    void primeSmoothers();

    static constexpr double kSmoothSeconds = 0.008;

    double sampleRate_ = 44100.0;
    int maxBlockSize_ = 128;
    int oversampling_ = 4;

    std::array<TriodeStage, STAGE_COUNT> stage_;
    std::array<TriodeStage::Config, STAGE_COUNT> cfg_{};
    JamesToneStack tone_;

    double volume_ = 0.5;
    OnePoleSmootherD volSm_;
    bool primed_ = false;
    double bass_ = 0.5, treble_ = 0.5;
    double fac_ = 0.2;
    double plateRout_ = 38.0e3;

    std::vector<float> buf_;
    double lastOutPeak_ = 0.0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_ORANGE_PREAMP_H
