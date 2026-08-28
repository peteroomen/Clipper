// Clipper native shell — the shared DSP engine.
//
// This is the SINGLE place the native plugin composes the portable C++ core into
// the rig signal chain. It uses the core classes DIRECTLY (RatModel, SdModel,
// TsModel, MuffModel, PhaserModel, GoldModel, WahModel, CompModel, GateModel, Ce1Model,
// DelayModel, AmpModel + owned ChorusModel,
// CabConvolver x2, OutputLimiter) — NOT the C ABI. The chain mirrors
// web/worklet/clipper-processor.js exactly:
//
//   mono in
//     -> * inputGain (input trim, -12..+24 dB)
//     -> chain[0] (if engaged)       [mono]   \
//     -> chain[1] (if engaged)       [mono]    |  the USER-ORDERED pedal board
//     -> ...                                  /
//     -> AmpModel.processStereo      [mono -> stereo: tone stack + volume + bright
//                                     + JC-120 chorus/vibrato split]
//     -> per-side CabConvolver       [the LIVE cab pair, if cab on — see the cab
//                                     picker note below]
//     -> * declick envelope          [6 ms raised cosine, chain edits only]
//     -> OutputLimiter.processStereo [lookahead gain-rider safety limiter]
//   -> stereo out
//
// Amp powered off ("power" toggle) bypasses amp + cab: the mono chain signal is
// copied to BOTH sides (stereo passthrough), then still limited. This matches the
// worklet's _bypassAmp branch.
//
// NATIVE PARITY (was: a FIXED two-pedal chain, RAT then SD-1). The board is now
// DYNAMIC, exactly like the web app: any of the six audio pedal types (RAT, SD-1,
// TS, Muff, Phaser, Gold, Wah, Comp) may sit on it, in ANY user-chosen order, each engaged or
// true-bypassed independently. Each type is instantiable ONCE (the board is a subset
// + permutation of the six), which keeps every DSP instance a plain member — no
// allocation, no handle table, and a reorder is a memcpy of six ints.
//
// Chain edits (add / remove / reorder / swap / engage-toggle) are DECLICKED with
// the worklet's 6 ms raised-cosine output fade: the output ramps to zero, the
// topology swap happens exactly at that zero (mid-block, in process()), then it
// ramps back up. Because the discontinuity always lands at output-zero there is no
// step and no zipper.
//
// One addition to the worklet's recipe: a short ZERO HOLD between the swap and the
// fade back in. Pedals KEEP their internal state across a reorder (as they do in
// the web, which reuses each handle), so after the swap a pedal is suddenly fed a
// differently-phased signal and rings while its filters and oversampler settle.
// That settling peaks a few ms AFTER the swap — i.e. underneath the worklet's
// immediate fade-in, where it is plainly measurable (a ~70x jump over the steady
// slew; see native/tests/chain_edit_test.cpp). Holding the output at zero for the
// settling window and only then fading in costs about 6 ms of extra gap, which no
// one can hear as latency, and removes the tick entirely.
//
// Plain knob moves are NOT bracketed — the core's ~5 ms one-pole smoothing already
// declicks those, and bracketing them would break the identical-core bit-exactness
// contract. When no edit is in flight the envelope is bypassed ENTIRELY (not
// multiplied by 1.0), so a steady chain stays bit-exact.
//
// The TUNER is deliberately absent: it is a display-only pedal (no audio DSP — it
// mutes the chain and drives a needle), and the native shell has no pitch-detection
// tap or needle widget yet. See docs/DEVELOPMENT.md → "Native pedal-board parity".
//
// Platform-free except it includes the core headers; it has no JUCE dependency so
// the identical-core console test can drive it, and the plugin wraps it.

// THE CAB / IR PICKER (2026-07-31, native parity with the web's cab select + upload).
// The convolver pair is DOUBLE BUFFERED, mirroring the C ABI's amp_prepare_cab_* /
// amp_commit_cab split (docs §30, ADR 003) — but with the one thing the worklet path
// still gets wrong fixed rather than copied:
//
//   MESSAGE thread : builds the requested IR into the INACTIVE pair (IR synthesis or
//                    a decoded file, peak normalization, FFT partitioning — 11-46 ms,
//                    and every allocation the whole feature makes), then ARMS a swap.
//   AUDIO thread   : notices the arm, runs the ordinary declick fade, and at the fade
//                    ZERO flips ONE integer. No allocation, no lock, no file I/O, no
//                    prepare(), no free().
//   MESSAGE thread : RETIRES the pair the audio thread stopped using — but only after
//                    it has observed the swap. The retired pair's buffers are released
//                    (and reused) by the NEXT message-thread prepare, so a free never
//                    lands on the render path. The worklet's _destroyPedal free inside
//                    _commitPending is a documented BUG (CLAUDE.md), not a precedent.
//
// The handshake is one atomic state word; see CabState below.

#ifndef CLIPPER_NATIVE_ENGINE_H
#define CLIPPER_NATIVE_ENGINE_H

#include <atomic>
#include <vector>

#include "clipper/dsp/Ac30Amp.h"
#include "clipper/dsp/OrangeAmp.h"
#include "clipper/dsp/ChampAmp.h"
#include "clipper/dsp/MesaAmp.h"
#include "clipper/dsp/RockerverbAmp.h"
#include "clipper/dsp/AmpModel.h"
#include "clipper/dsp/CabConvolver.h"
#include "clipper/dsp/CabIR.h"
#include "clipper/dsp/Ce1Model.h"
#include "clipper/dsp/CompModel.h"
#include "clipper/dsp/DelayModel.h"
#include "clipper/dsp/GateModel.h"
#include "clipper/dsp/OptoModel.h"
#include "clipper/dsp/VibeModel.h"
#include "clipper/dsp/DropModel.h"
#include "clipper/dsp/EqModel.h"
#include "clipper/dsp/GoldModel.h"
#include "clipper/dsp/WahModel.h"
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
    // v1.1 item 6: the GOLD "transparent" overdrive ("Myth"). APPENDED, never
    // inserted — these integers are the packed-snapshot encoding, and renumbering
    // an existing type would silently re-point a board mid-session.
    PEDAL_GOLD = 5,
    // Post-v1.1: the "Weeper" wah / envelope filter — the board's first FILTER
    // pedal (docs §58). APPENDED for the same reason as PEDAL_GOLD: these
    // integers ARE the packed-snapshot encoding.
    PEDAL_WAH = 6,

    // M13.1: the "Squash" OTA compressor — the first DYNAMICS pedal. APPENDED for
    // the same reason: these integers ARE the packed-snapshot encoding.
    PEDAL_COMP = 7,   // 7, not 6: PEDAL_WAH shipped first and these
                      // integers ARE the packed-snapshot encoding, so
                      // renumbering it would re-read saved boards wrong.

    // RESERVED 2026-07-31 by the slot-reservation commit, BEFORE the three
    // parallel slices that fill them. Tonight's three-way merge produced a real
    // collision — two slices independently claimed id 6, and these integers ARE
    // the packed-snapshot encoding, so getting that backwards silently re-points
    // every saved board. Pre-allocating removes the whole class: each slice fills
    // exactly its own slot and never has to guess "next free".
    PEDAL_DELAY = 8,    // M13.4  analog delay
    PEDAL_GATE  = 9,    // M13.6a noise gate
    PEDAL_CHORUS = 10,  // M13.7  CE-1 chorus ensemble

    // M13.3: the "Lumen" OPTICAL compressor — the second DYNAMICS voice.
    // APPENDED at 11 for the same reason as every type above it: these integers
    // ARE the packed-snapshot encoding and the APVTS chain-order state, so
    // renumbering one silently re-points every saved board.
    PEDAL_OPTO = 11,

    // M13.5: the "Swirl" UNI-VIBE — the board's second phaser, a lamp lighting
    // four photocells across four STAGGERED allpass stages. APPENDED at 12 for
    // the same reason as every type above it: these integers ARE the packed-
    // snapshot encoding and the APVTS chain-order state, so renumbering one
    // silently re-points every saved board.
    PEDAL_VIBE = 12,

    // M13.10: the "Cellar" drop-tune — the board's FIRST pitch shifter.
    // APPENDED at 13 for the same reason as every type above it: these integers
    // ARE the packed-snapshot encoding and the APVTS chain-order state, so
    // renumbering one silently re-points every saved board.
    PEDAL_DROP = 13,

    // M13.6: the "Decade" ten-band graphic EQ — the board's first EQ, and the
    // first pedal whose control surface is larger than three slots. APPENDED at
    // 14 for the same reason as every type above it: these integers ARE the
    // packed-snapshot encoding and the APVTS chain-order state.
    //
    // NOTE FOR THE NEXT PEDAL SLICE: this one exactly fills the packed chain
    // word. PluginProcessor.cpp asserts kChainField * (kMaxChain + 1) <= 64 with
    // kChainField = 4, which at PEDAL_TYPE_COUNT = 15 is 4 * 16 = 64. A
    // SIXTEENTH type fires that static_assert, exactly as the EIGHTH fired the
    // uint32 one it replaced. Widen the field or the word first.
    PEDAL_EQ = 14,
    PEDAL_TYPE_COUNT = 15,
};

// Each type is instantiable once, so the board can never be longer than this.
constexpr int kMaxChain = PEDAL_TYPE_COUNT;

// Which amp head the engine drives. These integers are the `ampModel` Params
// field, the APVTS `ampModel` Choice index and the C ABI's built-in voice id all
// at once, so they are STABLE and APPEND-ONLY: inserting one would silently
// re-voice every saved session and every host automation lane.
//
// AMP_MODEL_COUNT exists so the PLUGIN cannot drift from the ENGINE. The Mesa
// shipped in M10.4 (docs §69) with the engine wired and the plugin's
// `kAmpModelChoices` left at six entries, which made the voice unreachable from
// the UI for anyone using the plugin — the Choice parameter simply could not
// express the value 6. `native/tests/amp_voice_test.cpp` now asserts the choice
// count equals this constant, so the same omission goes RED for any future voice
// rather than shipping as an invisible one.
enum AmpModel : int {
    AMP_CLEAN120 = 0,    // M6.x  solid-state stereo clean + chorus
    AMP_JCM800 = 1,      // M9.4  Marshall 2204 head
    AMP_TWIN = 2,        // M10.1 blackface Twin combo
    AMP_AC30 = 3,        // M10.2 AC30 top boost combo
    AMP_ORANGE = 4,      // M10.3 Orange OR120 Overdrive head
    AMP_ROCKERVERB = 5,  // M10.7 Orange Rockerverb 100 head
    AMP_MESA = 6,        // M10.4 Mesa/Boogie Dual Rectifier Solo Head
    AMP_CHAMP = 7,       // M10.10 Fender tweed Champ 5F1 (single-ended 6V6)
    AMP_MODEL_COUNT = 8,
};

// Which cabinet IR the convolver pair is loaded with. 0/1 are ALSO the C ABI's
// built-in indices (amp_prepare_cab_builtin) and the web's CabChoice order
// ('clean212' | 'brit412' | 'custom' — web/src/rig.ts), so the integer round-trips
// between the two front-ends unchanged. Values are STABLE: the APVTS choice
// parameter stores them.
enum CabChoice : int {
    CAB_CLEAN212 = 0,  // the procedural closed-back 2x12 (generateDefaultCab2x12IR)
    CAB_BRIT412  = 1,  // the greenback-ish 4x12 (generateBrit4x12IR)
    CAB_CUSTOM   = 2,  // a user .wav IR, decoded + resampled by the shell
    // M10.3: the Orange 4x12 (generateOrange4x12IR) is APPENDED at 3 and therefore
    // does NOT match the C ABI's built-in index for it (which is 2 — the ABI has no
    // 'custom' slot in that enum). That divergence is DELIBERATE: this value is
    // stored in the APVTS `cabModel` choice parameter and in saved sessions, and
    // inserting a new cab at 2 would silently turn every session that says "Custom
    // IR" into "Orange 4x12". The engine maps the two spaces in code
    // (loadCurrentCabIntoPair), never by assuming they are the same integer.
    CAB_ORANGE412 = 3,
    // M10.10: the Tweed 1x8 (generateTweed1x8IR) is APPENDED at 4, for exactly the
    // reason CAB_ORANGE412 is appended at 3 — this integer is stored in the APVTS
    // choice parameter and in saved sessions. Its C ABI built-in index is 3, so
    // the two spaces diverge again; loadCurrentCabIntoPair maps them in code.
    CAB_TWEED8 = 4,
    CAB_CHOICE_COUNT = 5,
};

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

    // GOLD "Myth" transparent overdrive (v1.1 item 6). Same three-knob shape as the
    // dirt pedals, its slots reading as Gain/Treble/Output; defaults mirror web
    // GOLD_KNOB_DEFAULTS (a mostly-clean blend with a little grit, flat treble, and
    // an output above unity — the way the box is actually used).
    bool  goldOn = true;
    float goldGain = 0.35f;
    float goldTreble = 0.5f;
    float goldLevel = 0.7f;

    // Wah "Weeper" (post-v1.1) — the first FILTER pedal. Slots read as
    // POSITION / SENSE / VOICE; defaults mirror web WAH_KNOB_DEFAULTS. SENSE 0 is
    // deliberate: the pedal opens as a plain manual wah, and the envelope
    // follower contributes exactly nothing until asked. Docs §58.
    bool  wahOn = true;
    float wahPosition = 0.35f;
    float wahSense = 0.0f;
    float wahVoice = 0.5f;
    // "Squash" OTA compressor (M13.1) — the first non-dirt, non-modulation pedal
    // on the board. TWO knobs only: the shared slot-1 (filter) has no meaning for
    // a compressor and is not exposed, exactly as the phaser exposes only SPEED.
    // Defaults mirror web COMP_KNOB_DEFAULTS: SUSTAIN 0.5, LEVEL 0.4 (unity).
    bool  compOn = true;
    float compSustain = 0.5f;
    float compLevel = 0.4f;
    // M13.7 CE-1 "Ensemble" chorus — the board's second MODULATION pedal, and the
    // JC-120 amp's own chorus circuit in a floor box (docs §62). THREE knobs:
    // RATE / DEPTH / MODE. Defaults mirror web CHORUS_KNOB_DEFAULTS. MODE is
    // DISCRETE (< 0.5 chorus, >= 0.5 vibrato) and opens on CHORUS.
    // NAME NOTE: `ce1*`, not `chorus*`. The AMP's own chorus already owns
    // `chorusMode` and `chorusDepth` in this same struct (docs §11.2 / §20), and
    // reusing those names is a redefinition, not just a readability problem.
    bool  ce1On = true;
    float ce1Rate = 0.35f;
    float ce1Depth = 0.5f;
    float ce1Mode = 0.0f;

    // "Curfew" noise gate (M13.6a) — the board's first UTILITY. TWO knobs, for
    // the same reason as the compressor's: the reference gate has two, and its
    // third control (MODE) changes what the FOOTSWITCH does rather than the
    // audio. Defaults mirror web GATE_KNOB_DEFAULTS: THRESHOLD 0.35, DECAY 0.5.
    bool  gateOn = true;
    float gateThreshold = 0.35f;
    float gateDecay = 0.5f;

    // M13.3: the "Lumen" optical compressor. THREE real controls — this is the
    // first pedal on the board whose middle slot is not carried-and-unused.
    // MODE is DISCRETE (< 0.5 COMPRESS, >= 0.5 LIMIT), the CE-1 precedent.
    bool  optoOn = true;
    float optoPeakReduction = 0.5f;
    float optoMode = 0.0f;
    float optoGain = 0.62f;

    // M13.5 the Uni-Vibe. Defaults MUST match VIBE_KNOB_DEFAULTS in web/src/rig.ts
    // and VibeModel's constructor. MODE is DISCRETE (< 0.5 chorus, >= 0.5 vibrato)
    // but reaches the model as a smoothed weight, never a branch (docs §62).
    bool  vibeOn = true;
    float vibeSpeed = 0.35f;
    float vibeIntensity = 0.70f;
    float vibeMode = 0.0f;

    // M13.10 the drop-tune. Defaults MUST match DROP_KNOB_DEFAULTS in
    // web/src/rig.ts and DropModel's constructor. AMOUNT is a 9-position ROTARY
    // that the core quantizes (docs §70.2), so 0.0f is the DROP 1 detent — one
    // semitone down — not "off". Slots 1/2 are carried and unused: the
    // reference has one control and no MIX.
    bool  dropOn = true;
    float dropAmount = 0.0f;

    // M13.6 the ten-band graphic EQ. Defaults MUST match EQ_KNOB_DEFAULTS in
    // web/src/rig.ts and EqModel's constructor. EVERY slider opens at 0.5, which
    // for this pedal is not a convention but a structural property: at centre
    // each band leg injects nothing into the summing node, so a freshly added EQ
    // is exactly transparent (docs §73.3). GAIN rides slot 0 and VOLUME slot 2 —
    // ordinary slot reuse, the way the phaser takes slot 0 as SPEED; slot 1 is
    // carried and unused. The ten bands are their OWN param ids (3..12).
    bool  eqOn = true;
    float eqGain = 0.5f;
    float eqVolume = 0.5f;
    float eqBand[10] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};

    // "Echoman" BBD analog delay (M13.4) — the lineup's FIRST DELAY, and a new
    // DSP family. Three real knobs, the shared positional slots reading as
    // DELAY / FEEDBACK / BLEND; defaults mirror web DELAY_KNOB_DEFAULTS. DELAY is
    // the bucket-brigade CLOCK, so turning it up lengthens the echo AND darkens
    // the repeats; BLEND 0 is bit-identical to bypass. Docs §60.
    bool  delayOn = true;
    float delayTime = 0.35f;
    float delayFeedback = 0.3f;
    float delayBlend = 0.35f;

    // THE BOARD: which pedal types are on it, in signal order (guitar -> chain[0]
    // -> ... -> amp). Each type appears at most once. This is the parity feature —
    // it replaces the old fixed RAT-then-SD-1 pair.
    //
    // The default here is that LEGACY PAIR, so any code that builds a Params by
    // hand and only sets ratOn/sdOn (the pre-parity tests, the reference renders)
    // keeps its exact old routing. The PLUGIN's shipped default board is the web
    // app's DEFAULT_RIG instead — a single RAT (see PluginProcessor).
    int   chain[kMaxChain] = {PEDAL_RAT, PEDAL_SD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int   chainLength = 2;

    // Amp voice — see AmpModel below. Selects which head process() drives; all are
    // always kept current so a live switch is instant.
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

    // M10.3 Orange OR120: it reuses volume/bass/treble/reverb from the shared
    // fields and REUSES jcmPresence as its H.F. BOOST (both are power-amp HF
    // controls in a feedback loop — the same slot-reuse the AC30 makes for TOP
    // CUT). The ONE thing it needs of its own is the F.A.C. rotary: a 0..1 knob
    // the core snaps to the nearest of six detents.
    float orangeFac = 0.2f;

    // M10.7 Orange Rockerverb 100 (docs §63): NO new fields at all. Its GAIN rides
    // jcmGain and its post-tone-stack VOLUME rides jcmMaster — the same MEANING in
    // both amps, which is the house reuse rule; its BASS/MIDDLE/TREBLE are the
    // shared tone fields (it is the first Orange here with a mid) and its reverb is
    // the shared one. It has no presence control, so jcmPresence never reaches it.

    // M10.4 Mesa Dual Rectifier (docs §68): its GAIN rides jcmGain, its MASTER
    // rides jcmMaster and its PRESENCE rides jcmPresence — the same MEANING in
    // each case, which is the house reuse rule; bass/middle/treble and reverb are
    // the shared fields. What it needs of its own is its THREE switches, because
    // no other voice has any of them. Stored as 0..1 floats so they ride the same
    // APVTS float-parameter path as every other control and are quantized where
    // they reach the model.
    //   mesaMode  0 / .25 / .5 / .75 / 1 -> Clean / Vintage / Modern /
    //                                       Red Vintage / Red Modern
    //   mesaRectifier < .5 silicon, >= .5 5U4
    //   mesaPowerMode < .5 bold,    >= .5 spongy
    float mesaMode = 1.0f;
    float mesaRectifier = 0.0f;
    float mesaPowerMode = 0.0f;
    // M10.10 Champ (docs §73): its own field, NOT the shared `volume`. This knob is
    // the amp's only gain control (no master anywhere downstream) and it needs its
    // own default — the shared 0.40 would open this amp at ~50 % THD, which is
    // §63.14's "the amp opens at the wall" defect.
    //
    // RE-DERIVED 2026-08-28 (docs §75) from 0.20 to 0.10: at 0.20 this amp measured
    // 7.29 dB LOUDER than the JCM800 at its own default, sitting only 7.8 dB below
    // its own maximum where every sibling sits 16-29 dB below. 0.10 lands within
    // 0.44 dB of the JCM at 4.41 % THD, with 16.4 dB of level still above it.
    float champVolume = 0.1f;

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

    // --- THE CAB / IR PICKER — MESSAGE THREAD ONLY ---------------------------
    // See the banner at the top of this file for the threading contract. Every one
    // of these allocates; none of them may be called from process() or from a host
    // audio callback.
    //
    // prepareCab*() builds the IR into the INACTIVE convolver pair and ARMS the
    // swap; the audio thread performs it at the next declick fade zero. They return
    // false only if another prepare is already in flight on this engine (in which
    // case nothing was touched and the caller may retry) — a failed prepare NEVER
    // arms, because activating a pair that was not prepared would splice in the
    // previous IR plus its stale convolution tail (the C ABI's invariant).
    //
    // `which` is CAB_CLEAN212 / CAB_BRIT412 / CAB_ORANGE412. The custom form takes MONO samples;
    // the engine peak-normalizes them itself (M6.6 — the engine never trusts a
    // file's level), exactly like amp_prepare_cab_custom.
    bool prepareCabBuiltin(int which);
    bool prepareCabCustom(const float* ir, int irLength, double irSampleRate);

    // SETUP ONLY — perform an armed swap immediately, with no fade. Legal only when
    // the audio thread is NOT running (a constructor, prepareToPlay), because it
    // writes the active-pair index the render path reads.
    void commitCabNow();

    // Release the pair the audio thread has stopped using. A no-op unless a swap has
    // actually been observed. Called automatically by the next prepareCab*(); the
    // editor also polls it so the retirement is prompt rather than lazy.
    void retireCab();

    // True while a prepared swap is waiting for the audio thread's fade zero.
    bool cabSwapArmed() const;
    // Which IR the engine will build on the next prepare() (the message thread's
    // idea of the current cab).
    int  cabSource() const { return cabSource_; }
    bool hasCustomIr() const { return !customIr_.empty(); }
    // How many swaps the AUDIO thread has committed — the tests' proof that the
    // swap really landed inside process() and not on the message thread.
    int  cabCommitCount() const { return cabCommits_.load(std::memory_order_relaxed); }

    // --- introspection (tests / editor) --------------------------------------
    // True while a chain edit is fading out/in (the declick envelope is active).
    bool declicking() const { return declickPhase_ != Declick::Idle; }
    // The COMMITTED chain length (what is actually processing right now).
    int activeChainLength() const { return activeLength_; }

private:
    void applyParamsToModels();

    // --- cab double-buffer internals -----------------------------------------
    // Take ownership of the inactive pair for a message-thread prepare. Returns
    // false if another prepare holds it.
    bool beginCabPrepare();
    // Build `cabSource_`'s IR at the engine rate into `pair` (message thread).
    void loadCurrentCabIntoPair(int pair);
    // AUDIO THREAD, at the declick fade zero: flip the active pair if a prepared
    // swap is armed. One CAS plus one integer store — no allocation, no loop.
    bool commitCabIfArmed();

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
    int  activeChain_[kMaxChain] = {PEDAL_RAT, PEDAL_SD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int  activeLength_ = 2;
    bool activeOn_[PEDAL_TYPE_COUNT] = {true, false, true, true, true, true, true,
                                        true, true, true, true, true, true, true};

    // Declick state machine (mirrors the worklet's): a linear ramp position in
    // [0,1] mapped through a raised cosine, ~6 ms each way.
    enum class Declick { Idle, Out, Hold, In };
    Declick declickPhase_ = Declick::Idle;
    float   declickGain_ = 1.0f;  // linear ramp position (1 = fully open)
    float   declickStep_ = 1.0f;  // per-sample delta (set in prepare)
    int     declickHold_ = 0;     // samples left of the post-swap zero hold
    int     declickHoldLen_ = 0;  // its length at this sample rate

    clipper::dsp::RatModel rat_;
    clipper::dsp::SdModel  sd_;
    clipper::dsp::TsModel  ts_;
    clipper::dsp::MuffModel muff_;
    clipper::dsp::PhaserModel phaser_;
    clipper::dsp::GoldModel gold_;    // the "Myth" transparent overdrive (v1.1 item 6)
    clipper::dsp::WahModel wah_;      // the "Weeper" wah / envelope filter (docs §58)
    clipper::dsp::CompModel comp_;    // the "Squash" OTA compressor (M13.1)
    clipper::dsp::GateModel gate_;    // the "Curfew" noise gate (M13.6a)
    clipper::dsp::OptoModel opto_;    // the "Lumen" optical compressor (M13.3)
    clipper::dsp::VibeModel vibe_;    // the "Swirl" Uni-Vibe (M13.5)
    clipper::dsp::DropModel drop_;    // the "Cellar" drop-tune (M13.10)
    clipper::dsp::EqModel eq_;        // the "Decade" ten-band graphic EQ (M13.6)
    clipper::dsp::Ce1Model  ce1_;     // the CE-1 "Ensemble" chorus (M13.7)
    clipper::dsp::DelayModel delay_;  // the "Echoman" BBD analog delay (M13.4)
    clipper::dsp::AmpModel amp_;      // Clean 120
    clipper::dsp::Jcm800Amp jcm_;     // JCM800 2204 (mono head, M9.4)
    clipper::dsp::TwinAmp twin_;      // Fender blackface Twin (mono combo, M10.1)
    clipper::dsp::Ac30Amp ac30_;      // Vox AC30 top boost (mono combo, M10.2)
    clipper::dsp::OrangeAmp orange_;  // Orange OR120 Overdrive (mono head, M10.3)
    clipper::dsp::RockerverbAmp rockerverb_;  // Rockerverb 100 dirty ch. (M10.7)
    clipper::dsp::MesaAmp mesa_;              // Mesa Dual Rectifier (M10.4)
    clipper::dsp::ChampAmp champ_;            // Fender tweed Champ 5F1 (M10.10)

    // Amp parameter routing, shared by the setup path (applyParamsToModels,
    // old == nullptr => apply every row) and the per-block live path
    // (updateParams, old => apply only what changed). ONE body on purpose:
    // the two used to be separate and the Mesa was missed by the second.
    void applyAmpRouting(const Params& p, const Params* old);
    void applyMesaSwitches(const Params& p);
    // THE DOUBLE-BUFFERED CAB. cab_[pair][0] is the left side, cab_[pair][1] the
    // right. Exactly one pair is live at a time; the message thread only ever
    // touches the other one. Both are plain members, so the "swap" is an index and
    // the retired pair's storage is reused rather than freed on the render path.
    clipper::dsp::CabConvolver cab_[2][2];
    // The live pair. Written by the AUDIO thread at the declick zero (and by
    // commitCabNow() during setup, where no audio thread exists); read by the audio
    // thread every block and by the message thread's retirement, which is why it is
    // atomic rather than a plain int.
    std::atomic<int> activeCabPair_{0};

    // The cab-swap handshake, one word:
    //   Idle       nobody owns the inactive pair.
    //   Preparing  the MESSAGE thread owns it (building an IR into it).
    //   Armed      it holds a finished IR; the AUDIO thread may swap to it.
    //   Committing the AUDIO thread is mid-flip (two instructions).
    //   Swapped    the flip is done; the RETIRED pair is the message thread's again.
    enum CabState : int {
        CabIdle = 0, CabPreparing = 1, CabArmed = 2, CabCommitting = 3, CabSwapped = 4
    };
    std::atomic<int> cabState_{CabIdle};
    std::atomic<int> cabCommits_{0};

    // MESSAGE-THREAD-OWNED cab source. prepare() rebuilds THIS at the new engine
    // rate, so a sample-rate change keeps the user's cab instead of silently
    // reverting to the 2x12.
    int cabSource_ = CAB_CLEAN212;
    std::vector<float> customIr_;   // mono user IR, as handed in (NOT normalized)
    double customIrRate_ = 0.0;     // the rate customIr_ is sampled at

    clipper::dsp::OutputLimiter limiter_;

    // Mono scratch buffers (sized in prepare to maxBlock). Two ping-pong buffers
    // carry the signal through the serial pedal chain.
    std::vector<float> bufA_, bufB_;
};

}  // namespace clipper::native

#endif  // CLIPPER_NATIVE_ENGINE_H
