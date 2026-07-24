// Clipper native shell — the shared DSP engine.
//
// This is the SINGLE place the native plugin composes the portable C++ core into
// the rig signal chain. It uses the core classes DIRECTLY (RatModel, SdModel,
// AmpModel + owned ChorusModel, CabConvolver x2, OutputLimiter) — NOT the C ABI.
// The chain mirrors web/worklet/clipper-processor.js exactly:
//
//   mono in
//     -> * inputGain (input trim, -12..+24 dB)
//     -> RAT   (if engaged)          [mono]
//     -> SD-1  (if engaged)          [mono]
//     -> AmpModel.processStereo      [mono -> stereo: tone stack + volume + bright
//                                     + JC-120 chorus/vibrato split]
//     -> per-side CabConvolver       [cabL / cabR, if cab on]
//     -> OutputLimiter.processStereo [lookahead gain-rider safety limiter]
//   -> stereo out
//
// Amp powered off ("power" toggle) bypasses amp + cab: the mono chain signal is
// copied to BOTH sides (stereo passthrough), then still limited. This matches the
// worklet's _bypassAmp branch.
//
// The fixed two-pedal chain (RAT then SD-1) is v1; drag-reorder / arbitrary chains
// stay a web/UI feature for now (documented in docs/DEVELOPMENT.md). Both pedals
// default OFF except the RAT.
//
// Platform-free except it includes the core headers; it has no JUCE dependency so
// the identical-core console test can drive it, and the plugin wraps it.

#ifndef CLIPPER_NATIVE_ENGINE_H
#define CLIPPER_NATIVE_ENGINE_H

#include <vector>

#include "clipper/dsp/AmpModel.h"
#include "clipper/dsp/CabConvolver.h"
#include "clipper/dsp/CabIR.h"
#include "clipper/dsp/Jcm800Amp.h"
#include "clipper/dsp/OutputLimiter.h"
#include "clipper/dsp/RatModel.h"
#include "clipper/dsp/SdModel.h"

namespace clipper::native {

// A flat snapshot of every rig parameter, in the SAME units/semantics as
// web/src/rig.ts. Knob fields are 0..1 normalized positions; the core maps each
// to physical units internally (identical taper laws to the web build). Booleans
// are the engaged/on toggles. `chorusMode` is 0=off / 1=chorus / 2=vibrato.
struct Params {
    // Rig input stage.
    float inputTrim = 1.0f / 3.0f;  // 0..1 knob; 1/3 == 0 dB (INPUT_TRIM_UNITY_KNOB)

    // RAT (dirt pedal 1).
    bool  ratOn = true;
    float ratDist = 0.7f;
    float ratFilter = 0.4f;
    float ratLevel = 0.8f;

    // SD-1 (dirt pedal 2). Default OFF.
    bool  sdOn = false;
    float sdDrive = 0.5f;
    float sdTone = 0.5f;
    float sdLevel = 0.7f;

    // Amp voice (M9.4): 0 = Clean 120, 1 = JCM800. Selects which head process()
    // drives; both are always kept current so a live switch is instant.
    int   ampModel = 0;

    // Clean 120 amp.
    bool  ampOn = true;   // power
    float volume = 0.4f;
    float bass = 0.5f;    // SHARED with the JCM tone stack (both amps use it)
    float middle = 0.5f;  // SHARED
    float treble = 0.6f;  // SHARED
    bool  bright = false;
    bool  cab = true;
    int   chorusMode = 0;  // 0 off | 1 chorus | 2 vibrato
    float chorusSpeed = 0.3f;
    float chorusDepth = 0.5f;
    float reverb = 0.0f;   // M6.7 spring reverb wet/dry mix (0 = dry)

    // JCM800 (M9.4) knobs — gain = preamp drive, master = power-amp drive,
    // presence = power-amp HF lift. bass/middle/treble above are shared.
    float jcmGain = 0.5f;
    float jcmMaster = 0.4f;
    float jcmPresence = 0.5f;

    // Nonlinear-stage oversampling for the dirt pedals (1/2/4/8, default 4).
    int oversampling = 4;
};

// Input trim: 0..1 knob -> linear gain. Mirrors web/src/params.ts trimKnobToGain
// (dB range [-12, +24], unity at knob 1/3).
float trimKnobToGain(float knob01);

class ClipperEngine {
public:
    ClipperEngine();

    // Configure for a sample rate and maximum block size. Pushes the current
    // Params into every core model (as smoother targets) and prepares each model,
    // which SNAPS its smoothers to those targets — so the first processed block is
    // already steady-state (no start-up ramp). Re-generates the cab IR at `sr`.
    void prepare(double sampleRate, int maxBlockSize);

    // Replace the parameter snapshot and push EVERY field into the owned models.
    // Intended for setup (call before prepare() so the targets get snapped). Do NOT
    // call this per audio block: re-pushing an unchanged high-gain knob re-seeds its
    // smoother target and, through the RAT/SD-1 nonlinearity, perturbs the output.
    void setParams(const Params& p);

    // Realtime-safe per-block update: apply ONLY the fields that changed since the
    // last snapshot (mirrors the web worklet, which sets a core param only when a
    // knob actually moves). Unchanged params are left completely untouched, so a
    // steady chain stays bit-for-bit identical to a single-shot render. No
    // allocation, no locks — safe on the audio thread. An oversampling change routes
    // to each pedal's setOversampling (resets only the OS filter state), never a
    // full re-prepare.
    void updateParams(const Params& p);

    const Params& params() const { return params_; }

    // Process one block: mono `in` (numFrames samples) -> stereo `outL`/`outR`
    // (distinct buffers, each numFrames). `in` may not alias the outputs.
    void process(const float* in, float* outL, float* outR, int numFrames);

    // Total reported latency in base-rate samples, for host plugin delay
    // compensation: engaged pedals' oversampling group delay + the cab partition
    // (128, only when the amp is powered AND cab on) + the limiter lookahead (64).
    int latencySamples() const;

private:
    void applyParamsToModels();

    Params params_;
    double sampleRate_ = 48000.0;
    int    maxBlock_ = 128;

    clipper::dsp::RatModel rat_;
    clipper::dsp::SdModel  sd_;
    clipper::dsp::AmpModel amp_;      // Clean 120
    clipper::dsp::Jcm800Amp jcm_;     // JCM800 2204 (mono head, M9.4)
    clipper::dsp::CabConvolver cabL_, cabR_;
    clipper::dsp::OutputLimiter limiter_;

    // Mono scratch buffers (sized in prepare to maxBlock). Two ping-pong buffers
    // carry the signal through the serial pedal chain.
    std::vector<float> bufA_, bufB_;
};

}  // namespace clipper::native

#endif  // CLIPPER_NATIVE_ENGINE_H
