// Clipper — Boss SD-1 Super Overdrive model (M8). See SdModel.h for the overview.
//
// ---------------------------------------------------------------------------
// Circuit values, targets DERIVED analytically, and knob mappings. Circuit-
// INFORMED, not SPICE-accurate (per roadmap scope). Reference: 1.0f == 1.0 V.
// ---------------------------------------------------------------------------
//
// THE DEFINING SD-1-vs-RAT DIFFERENCE. The RAT clips with a diode pair SHUNTED
// to ground after a gain stage — a hard, symmetric clamp (odd harmonics). The
// SD-1 puts the diodes INSIDE the op-amp feedback loop, so the op-amp output is
//
//     V_out = V_in + V_fb ,   V_fb = the voltage the feedback diodes allow.
//
// The clean input V_in ALWAYS passes (the "+V_in" pedestal); only the amplified
// feedback component clips, and it clips SOFTLY (a resistor Rf is always in
// parallel with the diodes). That is the Tube-Screamer-family character: at low
// drive a mostly-clean signal with a light asymmetric edge, never a hard square.
//
// STAGE 1 — non-inverting gain + feedback soft clip.
//   Feedback  Zf = 1 MOhm DRIVE pot (|| 47 pF, || the 2-vs-1 diodes).
//   To ground Zg = R_g 4.7 kOhm + C_g 0.047 uF.
//   Non-inverting gain  A(s) = 1 + Zf/Zg. With Zg = R_g + 1/(sC_g):
//       A(s) = 1 + (Zf/R_g) * (s R_g C_g)/(1 + s R_g C_g)
//            = 1 + K * HP(s) ,   HP = first-order high-pass, unity at HF, 0 at DC,
//     corner  f_mid = 1/(2*pi*R_g*C_g) = 1/(2*pi*4700*0.047e-6) = 720.5 Hz.
//   THE MID-HUMP: gain is UNITY at DC and rises through f_mid to the HF plateau
//   1 + K. K = Zf/R_g is set by DRIVE; at max Zf = 1 MOhm => K = 1e6/4.7e3 = 212.8
//   => plateau 213.8 => +46.6 dB. So the bass below ~720 Hz stays comparatively
//   clean (unity) while the mids/highs are slammed — the SD-1's forward midrange.
//
//   We realise A(s) directly as  V_out = V_in + f( K * HP720(V_in) ), where f is
//   the asymmetric soft clipper (AsymSoftClipper.h). For SMALL signals f(u)~=u so
//   V_out ~= V_in*(1 + K*HP720) — exactly the analytic shelf (the mid-hump test
//   pins this within +/-1.5 dB). For LARGE signals f saturates => soft clip.
//
//   DRIVE knob -> plateau gain, linear-in-dB over [kDriveMinDb, kDriveMaxDb] =
//   [12, 46.6] dB (plateau 4x .. 214x). Min is NOT unity: even at DRIVE 0 a hot
//   input (~0.3 V) * (K_min~=3) clears the ~0.5 V diode knee, so the stage still
//   clips lightly — the SD-1 has no fully-clean setting, by design.
//
//   ASYMMETRY: 2 diodes clip the positive feedback swing (~2*Vf), 1 the negative
//   (~Vf). Knees Vp=0.95 V, Vn=0.50 V (AsymSoftClipper defaults) => an even-
//   harmonic component absent from the symmetric RAT (the asymmetry test measures
//   the 2nd harmonic vs a forced-symmetric reference).
//
//   4558 OP-AMP (LM308Stage reused with 4558 values). The SD-1 uses a FAST chip
//   (unlike the RAT's slow LM308): GBW ~= 3 MHz, slew ~= 1.7 V/us. The closed-loop
//   corner is GBW / A_noise; at the +46.6 dB max (A ~= 214) it sits at 3e6/214 ~=
//   14 kHz — high enough that the mid-hump voicing is unaffected in-band, and the
//   op-amp adds only a gentle top-octave softening at max drive (measured by the
//   closed-loop-bandwidth test). *Approximation* (as in M6.5): the op-amp band-
//   limit + slew are applied to the amplified FEEDBACK drive u only, before the
//   clip — the unity-gain clean pedestal is already full-bandwidth (its own
//   closed-loop corner is GBW/1 = 3 MHz), so limiting only the high-gain path is
//   the physically dominant effect.
//
// STAGE 3 — the SD-1's OWN TONE NETWORK (docs §65) + level.
//   Until 2026-08-01 this was a first-order treble TILT about a 1 kHz pivot,
//   +/-12 dB, TRANSPARENT at noon — explicitly documented here as an
//   approximation of "the published SD-1 tone response SHAPE". It was not one:
//   with no low-pass in it, the family's mid HUMP came out of this model as a
//   high SHELF (measured at DRIVE 0.5 / TONE noon: a monotone rise to a plateau
//   that ran FLAT from 4 kHz to 12 kHz).
//
//   It is now the netlist, transcribed from LiveSPICE's own
//   `Boss Super Overdrive SD-1.schx` (Tests/Examples; the file names gmarts.org
//   as its source) — see OverdriveToneStack.h for the topology and the exact
//   H(s). The values, with the schematic's designators:
//       R5  = 10 kOhm   clipper output -> IC1b's NON-inverting input
//       C4  = 0.018 uF  that node to ground  => a LOW-PASS at 884.2 Hz
//       R11 = 10 kOhm   that node to the 4.5 V rail (an AC ground)
//       TONE pot 22 kOhm, LINEAR, wiper -> C5 -> R7 -> ground
//       C5  = 0.027 uF , R7 = 470 Ohm
//       R8  = 10 kOhm   feedback around IC1b
//       C3  = 0.01 uF   ACROSS R8 (the TS has no such cap — see TsModel.cpp)
//   R5/R11 make the stage's DC gain EXACTLY 0.5 = -6.02 dB. That insertion loss
//   is real, it is 5.2 dB more than the TS's, and it is NOT compensated anywhere
//   (docs §65.5 — the §36 / ADR 008 precedent: nothing is re-gained to hide a
//   circuit correction).
//   A ~12 Hz output DC-blocker (the real pedal's C6 output coupling cap) removes
//   the DC the asymmetric clip produces. LEVEL is a clean linear output gain
//   (identity map, as in the RAT).
//
// M2 — antialiasing. Only the nonlinear feedback clip runs oversampled (default
// 4x); the 4558 op-amp model and the ADAA soft-clip both live at the oversampled
// rate. The pedestal V_in is upsampled alongside so it stays sample-aligned with
// the clipped feedback (no separate delay line); it is band-limited so it
// reconstructs exactly on downsample. ADAA is the production clip; a naive path
// is selectable for the aliasing A/B (see AsymSoftClipper.h).

#include "clipper/dsp/SdModel.h"

#include "clipper/dsp/AsymSoftClipper.h"
#include "clipper/dsp/OverdriveEngine.h"

namespace clipper::dsp {

namespace {
// SD-1 config for the shared OverdriveEngine. These are exactly the in-line
// constants the pre-refactor SdModel used, so the engine reproduces the M8
// behaviour byte-for-byte (the M8 suite passes unchanged):
//   - mid-hump 720.5 Hz  = 1/(2*pi*4.7k*0.047uF), the shared Zg leg;
//   - DRIVE plateau [12, 46.6] dB, max = 1 + 1M/4.7k ~= 213.8x (+46.6 dB);
//   - ASYMMETRIC diodes Vp=0.95 (2 diodes) / Vn=0.50 (1 diode) => even harmonics;
//   - 4558 op-amp 3 MHz GBW / 1.7 V/us slew; the transcribed TONE network;
//     12 Hz output DC blocker.
constexpr OverdriveConfig kSdConfig = {
    /* midHumpHz            */ 720.5,
    /* driveMinDb           */ 12.0f,
    /* driveMaxDb           */ 46.6f,
    /* diodeVp              */ AsymSoftClipper::kDefaultVp,  // 0.95 (2 diodes)
    /* diodeVn              */ AsymSoftClipper::kDefaultVn,  // 0.50 (1 diode)
    /* opAmpGbwHz           */ 3.0e6,
    /* opAmpSlewVoltsPerSec */ 1.7e6,
    /* tone                 */
    {
        /* rIn   R5  */ 10.0e3,
        /* cIn   C4  */ 18.0e-9,   // with R5: the 884.2 Hz low-pass
        /* rBias R11 */ 10.0e3,    // => DC gain exactly 0.5 (-6.02 dB)
        /* rPot      */ 22.0e3,    // LINEAR (the transcription's own marking)
        /* rW    R7  */ 470.0,
        /* cW    C5  */ 27.0e-9,
        /* rFb   R8  */ 10.0e3,
        /* cFb   C3  */ 10.0e-9,   // across R8 — the SD-1 has this, the TS does not
    },
    /* dcBlockHz            */ 12.0,
};
}  // namespace

struct SdModel::Impl {
    OverdriveEngine engine{kSdConfig};
};

OverdriveToneConfig SdModel::toneConfig() { return kSdConfig.tone; }

SdModel::SdModel() : impl_(std::make_unique<Impl>()) {}
SdModel::~SdModel() = default;

void SdModel::prepare(double sampleRate, int maxBlockSize) {
    impl_->engine.prepare(sampleRate, maxBlockSize);
}

void SdModel::reset() { impl_->engine.reset(); }
void SdModel::setOversampling(int factor) { impl_->engine.setOversampling(factor); }
int SdModel::oversampling() const { return impl_->engine.oversampling(); }
int SdModel::latencySamples() const { return impl_->engine.latencySamples(); }

void SdModel::setClipMode(int mode) { impl_->engine.setClipMode(mode); }
int SdModel::clipMode() const { return impl_->engine.clipMode(); }

void SdModel::setIdealOpAmp(bool ideal) { impl_->engine.setIdealOpAmp(ideal); }
bool SdModel::idealOpAmp() const { return impl_->engine.idealOpAmp(); }

void SdModel::setSymmetric(bool symmetric) { impl_->engine.setSymmetric(symmetric); }
bool SdModel::symmetric() const { return impl_->engine.symmetric(); }

void SdModel::setParameter(int paramId, float value) {
    impl_->engine.setParameter(paramId, value);
}

void SdModel::process(const float* in, float* out, int numFrames) {
    impl_->engine.process(in, out, numFrames);
}

void SdModel::processChunk(const float* in, float* out, int numFrames) {
    // Retained only to satisfy the header's private declaration; the engine owns
    // chunking now. Never called.
    impl_->engine.process(in, out, numFrames);
}

}  // namespace clipper::dsp
