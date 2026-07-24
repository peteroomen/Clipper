// Clipper native shell — the shared DSP engine.
//
// This is the SINGLE place the native plugin composes the portable C++ core into
// the rig signal chain. It uses the core classes DIRECTLY (RatModel, SdModel,
// TsModel, MuffModel, PhaserModel, AmpModel + owned ChorusModel, CabConvolver x2,
// OutputLimiter) — NOT the C ABI. The chain mirrors
// web/worklet/clipper-processor.js exactly:
//
//   mono in
//     -> * inputGain (input trim, -12..+24 dB)
//     -> chain[0] (if engaged)       [mono]   \
//     -> chain[1] (if engaged)       [mono]    |  the USER-ORDERED pedal board
//     -> ...                                  /
//     -> AmpModel.processStereo      [mono -> stereo: tone stack + volume + bright
//                                     + JC-120 chorus/vibrato split]
//     -> per-side CabConvolver       [cabL / cabR, if cab on]
//     -> * declick envelope          [6 ms raised cosine, chain edits only]
//     -> OutputLimiter.processStereo [lookahead gain-rider safety limiter]
//   -> stereo out
//
// Amp powered off ("power" toggle) bypasses amp + cab: the mono chain signal is
// copied to BOTH sides (stereo passthrough), then still limited. This matches the
// worklet's _bypassAmp branch.
//
// NATIVE PARITY (was: a FIXED two-pedal chain, RAT then SD-1). The board is now
// DYNAMIC, exactly like the web app: any of the five audio pedal types (RAT, SD-1,
// TS, Muff, Phaser) may sit on it, in ANY user-chosen order, each engaged or true-
// bypassed independently. Each type is instantiable ONCE (the board is a subset +
// permutation of the five), which keeps every DSP instance a plain member — no
// allocation, no handle table, and a reorder is a memcpy of five ints.
//
// Chain edits (add / remove / reorder / swap / engage-toggle) are DECLICKED with
// the worklet's 6 ms raised-cosine output fade: the output ramps to zero, the
// topology swap happens exactly at that zero (mid-block, in process()), then it
// ramps back up. Because the discontinuity always lands at output-zero there is no
// step and no zipper. Plain knob moves are NOT bracketed — the core's ~5 ms one-
// pole smoothing already declicks those, and bracketing them would break the
// identical-core bit-exactness contract. When no edit is in flight the envelope is
// bypassed ENTIRELY (not multiplied by 1.0), so a steady chain stays bit-exact.
//
// The TUNER is deliberately absent: it is a display-only pedal (no audio DSP — it
// mutes the chain and drives a needle), and the native shell has no pitch-detection
// tap or needle widget yet. See docs/DEVELOPMENT.md → "Native pedal-board parity".
//
// Platform-free except it includes the core headers; it has no JUCE dependency so
// the identical-core console test can drive it, and the plugin wraps it.

#ifndef CLIPPER_NATIVE_ENGINE_H
#define CLIPPER_NATIVE_ENGINE_H

#include <vector>

#include "clipper/dsp/Ac30Amp.h"
#include "clipper/dsp/AmpModel.h"
#include "clipper/dsp/CabConvolver.h"
#include "clipper/dsp/CabIR.h"
#include "clipper/dsp/Jcm800Amp.h"
#include "clipper/dsp/MuffModel.h"
#include "clipper/dsp/OutputLimiter.h"
#include "clipper/dsp/PhaserModel.h"
#include "clipper/dsp/RatModel.h"
#include "clipper/dsp/SdModel.h"
#include "clipper/dsp/TsModel.h"
#include "clipper/dsp/TwinAmp.h"

namespace clipper::native {

// The pedal types the native board can hold, mirroring web/src/rig.ts PedalType
// minus 'tuner' (display-only; see the header note). The integer values are STABLE:
// they are what the APVTS chain-order state and the packed atomic snapshot store.
enum PedalType : int {
    PEDAL_RAT = 0,
    PEDAL_SD = 1,
    PEDAL_TS = 2,
    PEDAL_MUFF = 3,
    PEDAL_PHASER = 4,
    PEDAL_TYPE_COUNT = 5,
};

// Each type is instantiable once, so the board can never be longer than this.
constexpr int kMaxChain = PEDAL_TYPE_COUNT;

// The short, stable key each type serializes as in the APVTS chain-order state
// (matches the web PedalType strings). Returns nullptr for an out-of-range id.
const char* pedalTypeKey(int type);
// Parse a key back to a PedalType, or -1 if unknown.
int pedalTypeFromKey(const char* key);

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

    // TS "Screamer" (v1.1). Same three-knob shape as the SD-1 (drive/tone/level);
    // defaults mirror web TS_KNOB_DEFAULTS.
    bool  tsOn = true;
    float tsDrive = 0.5f;
    float tsTone = 0.5f;
    float tsLevel = 0.75f;

    // Muff "Pi" fuzz (v1.1 item 4). Slots read as sustain/tone/volume; defaults
    // mirror web MUFF_KNOB_DEFAULTS.
    bool  muffOn = true;
    float muffSustain = 0.6f;
    float muffTone = 0.5f;
    float muffVolume = 0.6f;

    // Phaser "Ninety" (v1.1). ONE real knob (SPEED — the shared slot 0); the web
    // carries two unused slots for the common pedal shape, which the native
    // parameter model simply does not expose. Default mirrors PHASER_KNOB_DEFAULTS.
    bool  phaserOn = true;
    float phaserSpeed = 0.35f;

    // THE BOARD: which pedal types are on it, in signal order (guitar -> chain[0]
    // -> ... -> amp). Each type appears at most once. This is the parity feature —
    // it replaces the old fixed RAT-then-SD-1 pair.
    //
    // The default here is that LEGACY PAIR, so any code that builds a Params by
    // hand and only sets ratOn/sdOn (the pre-parity tests, the reference renders)
    // keeps its exact old routing. The PLUGIN's shipped default board is the web
    // app's DEFAULT_RIG instead — a single RAT (see PluginProcessor).
    int   chain[kMaxChain] = {PEDAL_RAT, PEDAL_SD, 0, 0, 0};
    int   chainLength = 2;

    // Amp voice (M9.4/M10.1/M10.2): 0 = Clean 120, 1 = JCM800, 2 = Twin, 3 = AC30.
    // Selects which head process() drives; all are always kept current so a live
    // switch is instant.
    int   ampModel = 0;

    // Clean 120 / Twin shared amp knobs. volume/bright feed clean120 + twin;
    // bass/middle/treble feed all three; reverb feeds all three; chorusSpeed/Depth
    // feed clean120's chorus AND the twin's tremolo SPEED/INTENSITY.
    bool  ampOn = true;   // power
    float volume = 0.4f;  // clean120 volume + twin channel volume
    float bass = 0.5f;    // SHARED across all three tone stacks
    float middle = 0.5f;  // SHARED
    float treble = 0.6f;  // SHARED
    bool  bright = false; // clean120 + twin
    bool  cab = true;
    int   chorusMode = 0;  // 0 off | 1 chorus | 2 vibrato (clean120 only)
    float chorusSpeed = 0.3f;  // clean120 chorus speed + twin tremolo SPEED
    float chorusDepth = 0.5f;  // clean120 chorus depth + twin tremolo INTENSITY
    float reverb = 0.0f;   // spring reverb wet/dry mix — clean120 + jcm + twin

    // JCM800 (M9.4) knobs — gain = preamp drive, master = power-amp drive,
    // presence = power-amp HF lift. bass/middle/treble above are shared. The AC30
    // (M10.2) reuses volume/bass/treble/reverb from the shared fields and REUSES the
    // presence field as its TOP CUT (both are power-amp HF controls) — no new fields.
    float jcmGain = 0.5f;
    float jcmMaster = 0.4f;
    float jcmPresence = 0.5f;

    // Nonlinear-stage oversampling for the dirt pedals (1/2/4/8, default 4).
    int oversampling = 4;

    // Is `type` on the board (in the current chain)?
    bool onBoard(int type) const {
        for (int i = 0; i < chainLength; ++i)
            if (chain[i] == type) return true;
        return false;
    }
    // The engaged ("stomped on") flag for one pedal type — the per-type bool above.
    bool pedalOn(int type) const;
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
    // compensation: every ENGAGED, ON-BOARD pedal's oversampling group delay (they
    // run in series) + the cab partition (128, only when the amp is powered AND cab
    // on) + the limiter lookahead (64). Reported from the TARGET chain, so the host
    // is told about an edit as soon as it is requested.
    int latencySamples() const;

    // --- introspection (tests / editor) --------------------------------------
    // True while a chain edit is fading out/in (the declick envelope is active).
    bool declicking() const { return declickPhase_ != Declick::Idle; }
    // The COMMITTED chain length (what is actually processing right now).
    int activeChainLength() const { return activeLength_; }

private:
    void applyParamsToModels();

    // Run one pedal type over a mono block (in -> out, distinct buffers).
    void processPedal(int type, const float* in, float* out, int numFrames);
    // Per-type oversampling push (the phaser is linear and has none).
    void setPedalOversampling(int factor);
    // Does the COMMITTED topology differ from the target in `params_`? Any
    // difference (board membership, order, or an engaged flag) is a chain edit and
    // must be declicked.
    bool chainEditPending() const;
    // Copy the target topology into the committed one (at the fade zero, or up
    // front in setParams/prepare where a fade would be wrong).
    void commitChain();

    Params params_;
    double sampleRate_ = 48000.0;
    int    maxBlock_ = 128;

    // The COMMITTED topology — what process() actually runs. It only ever changes
    // at a declick fade zero, so the audio never sees a mid-block reorder.
    int  activeChain_[kMaxChain] = {PEDAL_RAT, PEDAL_SD, 0, 0, 0};
    int  activeLength_ = 2;
    bool activeOn_[PEDAL_TYPE_COUNT] = {true, false, true, true, true};

    // Declick state machine (mirrors the worklet's): a linear ramp position in
    // [0,1] mapped through a raised cosine, ~6 ms each way.
    enum class Declick { Idle, Out, In };
    Declick declickPhase_ = Declick::Idle;
    float   declickGain_ = 1.0f;  // linear ramp position (1 = fully open)
    float   declickStep_ = 1.0f;  // per-sample delta (set in prepare)

    clipper::dsp::RatModel rat_;
    clipper::dsp::SdModel  sd_;
    clipper::dsp::TsModel  ts_;
    clipper::dsp::MuffModel muff_;
    clipper::dsp::PhaserModel phaser_;
    clipper::dsp::AmpModel amp_;      // Clean 120
    clipper::dsp::Jcm800Amp jcm_;     // JCM800 2204 (mono head, M9.4)
    clipper::dsp::TwinAmp twin_;      // Fender blackface Twin (mono combo, M10.1)
    clipper::dsp::Ac30Amp ac30_;      // Vox AC30 top boost (mono combo, M10.2)
    clipper::dsp::CabConvolver cabL_, cabR_;
    clipper::dsp::OutputLimiter limiter_;

    // Mono scratch buffers (sized in prepare to maxBlock). Two ping-pong buffers
    // carry the signal through the serial pedal chain.
    std::vector<float> bufA_, bufB_;
};

}  // namespace clipper::native

#endif  // CLIPPER_NATIVE_ENGINE_H
