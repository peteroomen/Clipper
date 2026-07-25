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
// STAGE 3 — SD-1 tone control + level.
//   The real tone control is a 10k pot blending a 0.018 uF treble-lean path
//   against a 10k/0.027 uF darker path — a first-order treble TILT about a mid
//   pivot. We approximate it as: split the signal at kTonePivotHz into low/high
//   halves and scale the HIGH half by a tilt gain g_t in [-12, +12] dB as TONE
//   sweeps 0..1 (g_t = 0 dB, i.e. TRANSPARENT, at noon). This matches the
//   published SD-1 tone response SHAPE (progressive treble cut/boost, bass ~fixed)
//   without modelling the exact pot law — a documented approximation. LEVEL is a
//   clean linear output gain (identity map, as in the RAT).
//   A ~12 Hz output DC-blocker (the real pedal's output coupling cap) removes the
//   DC the asymmetric clip produces.
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
//   - 4558 op-amp 3 MHz GBW / 1.7 V/us slew; tone tilt +/-12 dB about 1 kHz;
//     12 Hz output DC blocker.
constexpr OverdriveConfig kSdConfig = {
    /* midHumpHz            */ 720.5,
    /* driveMinDb           */ 12.0f,
    /* driveMaxDb           */ 46.6f,
    /* diodeVp              */ AsymSoftClipper::kDefaultVp,  // 0.95 (2 diodes)
    /* diodeVn              */ AsymSoftClipper::kDefaultVn,  // 0.50 (1 diode)
    /* opAmpGbwHz           */ 3.0e6,
    /* opAmpSlewVoltsPerSec */ 1.7e6,
    /* tonePivotHz          */ 1000.0,
    /* toneMaxTiltDb        */ 12.0f,
    /* dcBlockHz            */ 12.0,
};
}  // namespace

struct SdModel::Impl {
    OverdriveEngine engine{kSdConfig};
};

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
