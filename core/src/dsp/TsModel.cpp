// Clipper — TS808-style "Screamer" overdrive (v1.1). See TsModel.h for overview.
//
// ---------------------------------------------------------------------------
// Circuit values, targets DERIVED analytically, and knob mappings. Circuit-
// INFORMED, not SPICE-accurate (per roadmap scope). Reference: 1.0f == 1.0 V.
// The TS IS the SD-1 topology (SdModel.cpp documents it in full); this file only
// states the TWO family differences and the ONE shared trait's derivation.
// ---------------------------------------------------------------------------
//
// SHARED with the SD-1 (the family trait): the same non-inverting feedback-clip
// stage V_out = V_in + f(K * HP720(V_in)) with the clean "+V_in pedestal"; the
// same to-ground leg Zg = R_g 4.7 kΩ + C_g 0.047 µF, so the mid-hump corner is
//     f_mid = 1/(2*pi*R_g*C_g) = 1/(2*pi*4700*0.047e-6) = 720.5 Hz,
// identical to the SD-1 — bass below ~720 Hz stays comparatively clean while the
// mids/highs are slammed (the forward-midrange Tube-Screamer voice). Same 4558-
// class op-amp (3 MHz GBW, 1.7 V/µs slew), same tone-network TOPOLOGY + level +
// DC-blocker — but see DIFFERENCE 3 for the tone stage's own values.
//
// DIFFERENCE 1 — SYMMETRIC clipping. The TS uses ONE diode each way in the
// feedback loop (1-vs-1), both silicon Vf ~= 0.60 V, so Vp == Vn == 0.60 V. A
// symmetric (odd) transfer curve => the 2nd (even) harmonic is ~ABSENT — the
// mirror of the SD-1's 2-vs-1 asymmetry (Vp 0.95 / Vn 0.50), whose even harmonic
// is its warmth. The symmetry test asserts this side-by-side with the SD-1.
//
// DIFFERENCE 2 — DRIVE pot 500 kΩ (vs the SD-1's 1 MΩ). The non-inverting gain
// plateau is 1 + Zf/R_g; at max DRIVE Zf = 500 kΩ, R_g = 4.7 kΩ, so
//     K_max = 500e3 / 4.7e3 = 106.38  =>  plateau 1 + K_max = 107.38x
//     = 20*log10(107.38) = +40.6 dB   (vs the SD-1's +46.6 dB — 6 dB less top gain).
// DRIVE maps linear-in-dB over [12, 40.6] dB (K = 3.0 .. 106.4). As with the SD-1
// the minimum is NOT unity: a hot input still clears the ~0.6 V knee, so the TS
// has no fully-clean setting (it grinds a little even at DRIVE 0), by design.
//
// DIFFERENCE 3 (docs §65) — the TONE NETWORK's values. Same topology as the SD-1
// (OverdriveToneStack.h), transcribed from LiveSPICE's own
// `Ibanez Tube Screamer TS-9.schx` and cross-checked component-for-component
// against `Cushychicken/ltspice-guitar-pedals`' TS808 LTspice netlist and against
// Yeh & Abel's DAFx-07 tone-stage transfer function (via
// `JamesStubbsEng/TS-808-Ultra`). With the schematic's designators:
//     R5  = 1 kΩ      clipper output -> the second op-amp's NON-inverting input
//     C4  = 0.22 µF   that node to ground  =>  a LOW-PASS at 723.4 Hz — the
//                     figure the Tube Screamer is famous for, and the half of
//                     the mid HUMP this model used not to have at all
//     R11 = 10 kΩ     that node to the 4.5 V rail (an AC ground)
//     TONE pot 20 kΩ, LINEAR, wiper -> C5 -> R7 -> ground
//     C5  = 0.22 µF , R7 = 220 Ω
//     R8  = 1 kΩ      feedback around that op-amp
//     C3  : ABSENT (the SD-1 puts a 0.01 µF across its R8; the TS does not)
// R5/R11 make the stage's DC insertion loss 0.83 dB — against the SD-1's 6.02 dB,
// which is the single largest level difference between the two pedals and is why
// they are the same engine with different CONFIGS, not the same numbers.
// **Sourcing note:** the LiveSPICE file marks the tone pot 10 kΩ; the LTspice
// netlist and Yeh's published derivation both use 20 kΩ, so the 20 kΩ ships and
// the outlier is recorded (docs §65.1).
//
// The 4558 closed-loop corner GBW/A at max DRIVE = 3e6 / 107.4 ~= 27.9 kHz — ABOVE
// the audio band even at 44.1 k, so (unlike the SD-1's 14 kHz) the op-amp adds no
// audible top-octave softening; it is present only as an antialiasing / stability
// nicety. Same M2 oversampled ADAA clip as the SD-1.
//
// Implementation: a thin wrapper over the shared OverdriveEngine with the TS
// config below. Zero DSP duplication — the SD-1 and TS ARE one engine.

#include "clipper/dsp/TsModel.h"

#include "clipper/dsp/OverdriveEngine.h"

namespace clipper::dsp {

namespace {
// TS808 config for the shared OverdriveEngine: SYMMETRIC 0.60 V knees + a +40.6 dB
// DRIVE plateau; everything else identical to the SD-1 (the shared family trait).
constexpr double kTsDiodeVf = 0.60;  // one silicon diode each way (1-vs-1)
constexpr OverdriveConfig kTsConfig = {
    /* midHumpHz            */ 720.5,   // 1/(2*pi*4.7k*0.047uF) — SHARED with SD-1
    /* driveMinDb           */ 12.0f,   // grinds lightly at a hot input (no clean)
    /* driveMaxDb           */ 40.6f,   // plateau 1 + 500k/4.7k ~= 107.4x (+40.6 dB)
    /* diodeVp              */ kTsDiodeVf,  // 0.60 (SYMMETRIC)
    /* diodeVn              */ kTsDiodeVf,  // 0.60 (SYMMETRIC)
    /* opAmpGbwHz           */ 3.0e6,   // 4558-class, SHARED with SD-1
    /* opAmpSlewVoltsPerSec */ 1.7e6,
    /* tone                 */
    {
        /* rIn   R5  */ 1.0e3,
        /* cIn   C4  */ 220.0e-9,  // with R5: the famous 723.4 Hz low-pass
        /* rBias R11 */ 10.0e3,    // => DC insertion loss 0.83 dB (SD-1: 6.02 dB)
        /* rPot      */ 20.0e3,    // LINEAR; see the sourcing note above
        /* rW    R7  */ 220.0,
        /* cW    C5  */ 220.0e-9,
        /* rFb   R8  */ 1.0e3,
        /* cFb   C3  */ 0.0,       // ABSENT on the TS (the SD-1 has 0.01 uF here)
    },
    /* dcBlockHz            */ 12.0,
};

// The SD-1's ASYMMETRIC 2-vs-1 knees — the even-harmonic reference the TS symmetry
// test forces to prove its own symmetric curve makes ~no 2nd harmonic.
constexpr double kSdAsymVp = 0.95;
constexpr double kSdAsymVn = 0.50;
}  // namespace

struct TsModel::Impl {
    OverdriveEngine engine{kTsConfig};
    bool symmetric = true;  // TS ships symmetric (production)
};

OverdriveToneConfig TsModel::toneConfig() { return kTsConfig.tone; }

TsModel::TsModel() : impl_(std::make_unique<Impl>()) {}
TsModel::~TsModel() = default;

void TsModel::prepare(double sampleRate, int maxBlockSize) {
    impl_->engine.prepare(sampleRate, maxBlockSize);
}

void TsModel::reset() { impl_->engine.reset(); }
void TsModel::setOversampling(int factor) { impl_->engine.setOversampling(factor); }
int TsModel::oversampling() const { return impl_->engine.oversampling(); }
int TsModel::latencySamples() const { return impl_->engine.latencySamples(); }

void TsModel::setClipMode(int mode) { impl_->engine.setClipMode(mode); }
int TsModel::clipMode() const { return impl_->engine.clipMode(); }

void TsModel::setIdealOpAmp(bool ideal) { impl_->engine.setIdealOpAmp(ideal); }
bool TsModel::idealOpAmp() const { return impl_->engine.idealOpAmp(); }

void TsModel::setSymmetric(bool symmetric) {
    impl_->symmetric = symmetric;
    if (symmetric) {
        // Production: the config's symmetric 0.60/0.60 knees.
        impl_->engine.setSymmetric(false);
    } else {
        // Reference: force the SD-1's ASYMMETRIC 2-vs-1 knees so the test can
        // show the even harmonic APPEAR (the mirror of the SD-1 symmetry test).
        impl_->engine.setDiodeKnees(kSdAsymVp, kSdAsymVn);
    }
}
bool TsModel::symmetric() const { return impl_->symmetric; }

void TsModel::setParameter(int paramId, float value) {
    impl_->engine.setParameter(paramId, value);
}

void TsModel::process(const float* in, float* out, int numFrames) {
    impl_->engine.process(in, out, numFrames);
}

}  // namespace clipper::dsp
