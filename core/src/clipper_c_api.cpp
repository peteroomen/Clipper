// C ABI wrapper around clipper::Processor for WASM (and any future FFI) export.
//
// This is the ONLY place a browser/Emscripten macro is allowed to appear, and
// even here it is kept minimal: EMSCRIPTEN_KEEPALIVE just prevents dead-code
// elimination of the exported symbols. When compiled natively (no Emscripten)
// the macro expands to nothing and this file still builds as plain C++.

#include "clipper/Processor.h"
#include "clipper/dsp/AmpModel.h"
#include "clipper/dsp/CabConvolver.h"
#include "clipper/dsp/CabIR.h"
#include "clipper/dsp/Jcm800Amp.h"
#include "clipper/dsp/TwinAmp.h"
#include "clipper/dsp/PhaserModel.h"
#include "clipper/dsp/MuffModel.h"
#include "clipper/dsp/RatModel.h"
#include "clipper/dsp/SdModel.h"
#include "clipper/dsp/TsModel.h"

#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

extern "C" {

// Create a processor prepared for the given sample rate. Returns an opaque
// handle (pointer) the caller passes back to the other functions.
EMSCRIPTEN_KEEPALIVE
void* clipper_create(float sample_rate) {
    auto* p = new clipper::Processor();
    // 128 == the AudioWorklet render quantum; a safe fixed max block for M0.
    p->prepare(static_cast<double>(sample_rate), 128);
    return p;
}

EMSCRIPTEN_KEEPALIVE
void clipper_destroy(void* handle) {
    delete static_cast<clipper::Processor*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void clipper_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    static_cast<clipper::Processor*>(handle)->setParameter(param_id, value);
}

EMSCRIPTEN_KEEPALIVE
void clipper_process(void* handle, const float* in_ptr, float* out_ptr,
                     int num_frames) {
    if (!handle) return;
    static_cast<clipper::Processor*>(handle)->process(in_ptr, out_ptr,
                                                      num_frames);
}

// --- M3: RAT diode-clipper model exports -------------------------------------
//
// Same opaque-handle style as the gain core above. These wrap
// clipper::dsp::RatModel (the M1/M2 pedal model) for the AudioWorklet. The gain
// exports stay so the M0 skeleton test path keeps working; the worklet uses the
// rat_* set below.

// Create a RAT model prepared for the given sample rate. 128 == the AudioWorklet
// render quantum, the fixed max block that sizes the oversampling scratch. The
// model defaults to 4x oversampling (see RatModel::setOversampling).
EMSCRIPTEN_KEEPALIVE
void* rat_create(float sample_rate) {
    auto* m = new clipper::dsp::RatModel();
    m->prepare(static_cast<double>(sample_rate), 128);
    return m;
}

EMSCRIPTEN_KEEPALIVE
void rat_destroy(void* handle) {
    delete static_cast<clipper::dsp::RatModel*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void rat_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    static_cast<clipper::dsp::RatModel*>(handle)->setParameter(param_id, value);
}

EMSCRIPTEN_KEEPALIVE
void rat_set_oversampling(void* handle, int factor) {
    if (!handle) return;
    static_cast<clipper::dsp::RatModel*>(handle)->setOversampling(factor);
}

// Round-trip oversampling-filter latency in base-rate samples (0 at 1x).
EMSCRIPTEN_KEEPALIVE
int rat_latency_samples(void* handle) {
    if (!handle) return 0;
    return static_cast<clipper::dsp::RatModel*>(handle)->latencySamples();
}

EMSCRIPTEN_KEEPALIVE
void rat_process(void* handle, const float* in_ptr, float* out_ptr,
                 int num_frames) {
    if (!handle) return;
    static_cast<clipper::dsp::RatModel*>(handle)->process(in_ptr, out_ptr,
                                                          num_frames);
}

// --- M8: SD-1 Super Overdrive model exports ----------------------------------
//
// Additive alongside rat_* (M3), byte-for-byte the same opaque-handle ABI so the
// worklet drives an SD-1 exactly like a RAT (param ids 0=DRIVE, 1=TONE, 2=LEVEL;
// oversampling + latency shared). A chain can mix rat_* and sd_* instances.

EMSCRIPTEN_KEEPALIVE
void* sd_create(float sample_rate) {
    auto* m = new clipper::dsp::SdModel();
    m->prepare(static_cast<double>(sample_rate), 128);
    return m;
}

EMSCRIPTEN_KEEPALIVE
void sd_destroy(void* handle) {
    delete static_cast<clipper::dsp::SdModel*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void sd_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    static_cast<clipper::dsp::SdModel*>(handle)->setParameter(param_id, value);
}

EMSCRIPTEN_KEEPALIVE
void sd_set_oversampling(void* handle, int factor) {
    if (!handle) return;
    static_cast<clipper::dsp::SdModel*>(handle)->setOversampling(factor);
}

EMSCRIPTEN_KEEPALIVE
int sd_latency_samples(void* handle) {
    if (!handle) return 0;
    return static_cast<clipper::dsp::SdModel*>(handle)->latencySamples();
}

EMSCRIPTEN_KEEPALIVE
void sd_process(void* handle, const float* in_ptr, float* out_ptr,
                int num_frames) {
    if (!handle) return;
    static_cast<clipper::dsp::SdModel*>(handle)->process(in_ptr, out_ptr,
                                                         num_frames);
}

// --- v1.1: TS808-style "Screamer" overdrive exports --------------------------
//
// Additive alongside rat_* / sd_*, byte-for-byte the same opaque-handle ABI (the
// TS shares the SD-1's engine + param ids 0=DRIVE, 1=TONE, 2=LEVEL). The worklet
// drives a Screamer exactly like an SD-1; a chain can mix rat_*, sd_*, ts_*.

EMSCRIPTEN_KEEPALIVE
void* ts_create(float sample_rate) {
    auto* m = new clipper::dsp::TsModel();
    m->prepare(static_cast<double>(sample_rate), 128);
    return m;
}

// --- v1.1: Phaser ("Ninety") model exports -----------------------------------
//
// Additive alongside rat_*/sd_*, and byte-for-byte the same opaque-handle ABI so
// the worklet can drive a phaser exactly like a dirt pedal (param slot 0 = SPEED;
// slots 1/2 are carried but ignored). The phaser is a LINEAR time-varying block
// (allpass sweep): no clipping, so no oversampling and zero added latency — the
// set_oversampling export is a documented NO-OP and latency is always 0, letting
// the generic per-node chain routing call the same five entry points on it.

EMSCRIPTEN_KEEPALIVE
void* phaser_create(float sample_rate) {
    auto* m = new clipper::dsp::PhaserModel();
    m->prepare(static_cast<double>(sample_rate));
    return m;
}

EMSCRIPTEN_KEEPALIVE
void ts_destroy(void* handle) {
    delete static_cast<clipper::dsp::TsModel*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void ts_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    static_cast<clipper::dsp::TsModel*>(handle)->setParameter(param_id, value);
}

EMSCRIPTEN_KEEPALIVE
void ts_set_oversampling(void* handle, int factor) {
    if (!handle) return;
    static_cast<clipper::dsp::TsModel*>(handle)->setOversampling(factor);
}

EMSCRIPTEN_KEEPALIVE
int ts_latency_samples(void* handle) {
    if (!handle) return 0;
    return static_cast<clipper::dsp::TsModel*>(handle)->latencySamples();
}

EMSCRIPTEN_KEEPALIVE
void ts_process(void* handle, const float* in_ptr, float* out_ptr,
                int num_frames) {
    if (!handle) return;
    static_cast<clipper::dsp::TsModel*>(handle)->process(in_ptr, out_ptr,
                                                         num_frames);
}

EMSCRIPTEN_KEEPALIVE
void phaser_destroy(void* handle) {
    delete static_cast<clipper::dsp::PhaserModel*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void phaser_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    static_cast<clipper::dsp::PhaserModel*>(handle)->setParameter(param_id, value);
}

// No-op: the phaser is linear time-varying, so it never oversamples. Present only
// so the worklet's generic pedal routing can call it uniformly.
EMSCRIPTEN_KEEPALIVE
void phaser_set_oversampling(void* /*handle*/, int /*factor*/) {}

// Always 0: no oversampling filter, no lookahead — the phaser adds no latency.
EMSCRIPTEN_KEEPALIVE
int phaser_latency_samples(void* /*handle*/) { return 0; }

EMSCRIPTEN_KEEPALIVE
void phaser_process(void* handle, const float* in_ptr, float* out_ptr,
                    int num_frames) {
    if (!handle) return;
    static_cast<clipper::dsp::PhaserModel*>(handle)->process(in_ptr, out_ptr,
                                                             num_frames);
}

// --- v1.1 item 4: Muff fuzz ("Pi") exports -----------------------------------
//
// Additive alongside rat_*/sd_*/ts_*/phaser_*, byte-for-byte the same opaque-handle
// ABI so the worklet drives the fuzz exactly like any other dirt pedal (param slots
// 0=SUSTAIN, 1=TONE, 2=VOLUME; oversampling + latency shared). A chain can mix any
// of rat_*, sd_*, ts_*, muff_*, phaser_*. Under the hood it is the FIRST BJT voice
// (MuffModel -> 4× BjtStage, Ebers-Moll), but the ABI is identical.

EMSCRIPTEN_KEEPALIVE
void* muff_create(float sample_rate) {
    auto* m = new clipper::dsp::MuffModel();
    m->prepare(static_cast<double>(sample_rate), 128);
    return m;
}

EMSCRIPTEN_KEEPALIVE
void muff_destroy(void* handle) {
    delete static_cast<clipper::dsp::MuffModel*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void muff_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    static_cast<clipper::dsp::MuffModel*>(handle)->setParameter(param_id, value);
}

EMSCRIPTEN_KEEPALIVE
void muff_set_oversampling(void* handle, int factor) {
    if (!handle) return;
    static_cast<clipper::dsp::MuffModel*>(handle)->setOversampling(factor);
}

EMSCRIPTEN_KEEPALIVE
int muff_latency_samples(void* handle) {
    if (!handle) return 0;
    return static_cast<clipper::dsp::MuffModel*>(handle)->latencySamples();
}

EMSCRIPTEN_KEEPALIVE
void muff_process(void* handle, const float* in_ptr, float* out_ptr,
                  int num_frames) {
    if (!handle) return;
    static_cast<clipper::dsp::MuffModel*>(handle)->process(in_ptr, out_ptr,
                                                           num_frames);
}

// --- M5: clean amp + cab exports ---------------------------------------------
//
// The amp instance is a small CHAIN: a linear AmpModel (volume + tone stack +
// bright) followed by a CabConvolver loaded with the procedural default 2x12 IR.
// The worklet drives the pedal (rat_*) and the amp (amp_*) as SEPARATE instances
// in sequence (pedal -> amp); the cab is part of THIS amp instance, so
// amp_process runs amp -> cab. AMP_PARAM_CAB (id == AmpModel::PARAM_COUNT)
// bypasses the cab for A/B without tearing anything down. All other amp param ids
// (tone/volume/bright, chorus 6/7/8, M6.7 reverb 9) route straight into the owned
// AmpModel via amp_set_param — no special-casing here beyond the cab toggle.

namespace {
// Chain-level param id for the cab on/off toggle (0 = bypass cab, 1 = cab on).
// One past the AmpModel param ids so it never collides with them.
constexpr int kAmpParamCab = clipper::dsp::AmpModel::PARAM_COUNT;  // == 5

// M9.4: the JCM800's three amp-specific knob ids, placed ABOVE every clean-120 id
// (reverb == 9 is the current top) so the ABI stays purely additive and the
// clean-120 ids never shift. GAIN/PRESENCE/MASTER are JCM-only; the JCM's
// BASS/MID/TREBLE REUSE the clean-120 tone ids (1/2/3) since they mean the same
// thing to the player. Must mirror web/src/params.ts AMP_PARAM_JCM_*.
constexpr int kAmpParamJcmGain = 10;
constexpr int kAmpParamJcmPresence = 11;
constexpr int kAmpParamJcmMaster = 12;

// Which amp model the chain's single handle is currently voicing. M10.1 adds the
// Twin as the THIRD voice (index 2), purely additive — clean120/jcm ids unchanged.
enum AmpModelId { kAmpClean120 = 0, kAmpJcm800 = 1, kAmpTwin = 2 };

// The JCM's (and Twin's) fixed internal oversampling. Docs §18/§20 measured 4× as
// the requirement (8× buys nothing at the composed max-gain floor), so the tube
// amps run at 4× regardless of the rig's pedal-oversampling selector — a deliberate
// design constant, never silently reduced for perf.
constexpr int kJcmOversampling = 4;
constexpr int kTwinOversampling = 4;

struct AmpChain {
    // Three amp voices behind ONE handle (M9.4 → M10.1). All are created + prepared
    // up front so amp_set_model is a realtime-safe int flip (no allocation on the
    // audio thread). The cab pair below is SHARED: whichever model is active feeds
    // the same per-side CabConvolvers + custom-IR machinery.
    clipper::dsp::AmpModel amp;         // clean 120 (JC-120 style, linear, stereo)
    clipper::dsp::Jcm800Amp jcm;        // Marshall JCM800 2204 (mono head)
    clipper::dsp::TwinAmp twin;         // Fender blackface Twin (mono combo head)
    int model = kAmpClean120;
    // M6.3: the amp goes STEREO from the chorus stage on, so the cab IR runs PER
    // SIDE. Two independent CabConvolver instances (same IR) — the wet R side is
    // genuinely a different signal from the dry L side in chorus mode, so a single
    // mono cab AFTER a wet/dry sum would collapse the stereo bloom. cabL doubles
    // as the mono cab for the legacy amp_process() path. The JCM is a MONO head, so
    // its output is copied to both sides (dual-mono) before the identical cab pair.
    clipper::dsp::CabConvolver cabL, cabR;
    bool cabOn = true;
    // Cab expansion: the engine rate is remembered so the cab can be regenerated
    // (built-in swap) or a user IR reloaded at the right rate on demand.
    double sr = 48000.0;
};

// Built-in cab selector for amp_set_cab_builtin. Kept as small ints so the ABI
// stays language-neutral (the worklet passes 0/1).
enum CabBuiltin { kCabClean212 = 0, kCabBrit412 = 1 };

// Load an IR into BOTH per-side convolvers at the chain's engine rate (same IR,
// same 128-sample partition — latency/CPU unchanged from the single built-in).
void loadIrBothSides(AmpChain* c, const std::vector<float>& ir) {
    c->cabL.prepare(c->sr, ir.data(), static_cast<int>(ir.size()), c->sr, 128);
    c->cabR.prepare(c->sr, ir.data(), static_cast<int>(ir.size()), c->sr, 128);
}
}  // namespace

// Create the amp+cab chain prepared for the given sample rate. 128 == the
// AudioWorklet render quantum, used as both the amp max block and the cab
// partition size. The default cab IR is generated at the engine rate (no
// resampling needed). Both per-side cabs load the SAME IR.
EMSCRIPTEN_KEEPALIVE
void* amp_create(float sample_rate) {
    const double sr = static_cast<double>(sample_rate);
    auto* c = new AmpChain();
    c->sr = sr;
    c->amp.prepare(sr, 128);
    // Prepare the JCM up front too (heavy: solves all tube DC op points), so the
    // model swap later is a lock-free int flip. It runs at its fixed 4× internally.
    c->jcm.setOversampling(kJcmOversampling);
    c->jcm.prepare(sr, 128);
    // Prepare the Twin up front as well (M10.1) — same lock-free-swap discipline.
    c->twin.setOversampling(kTwinOversampling);
    c->twin.prepare(sr, 128);
    const std::vector<float> ir = clipper::dsp::generateDefaultCab2x12IR(sr);
    loadIrBothSides(c, ir);
    return c;
}

// M9.4: select which amp voice the handle drives (0 = Clean 120, 1 = JCM800). The
// worklet calls this at the declick fade-out zero (never inside process()), so the
// topology swap is click-free — exactly like a cab swap. Both voices are already
// prepared, so this only flips an int. Chorus/reverb are Clean-120 features and are
// simply not reached on the JCM path (the 2204 has neither).
EMSCRIPTEN_KEEPALIVE
void amp_set_model(void* handle, int which) {
    if (!handle) return;
    auto* c = static_cast<AmpChain*>(handle);
    // 0 = Clean 120, 1 = JCM800, 2 = Twin. Unknown values fall back to Clean 120.
    c->model = (which == kAmpJcm800) ? kAmpJcm800
             : (which == kAmpTwin)   ? kAmpTwin
                                     : kAmpClean120;
}

// Cab expansion: swap the BUILT-IN cab IR (0 = Clean 2x12, 1 = Brit 4x12) on both
// per-side convolvers, regenerated at the engine rate. The worklet calls this at
// the declick fade-out zero (never inside process()), so the topology swap is
// click-free. Latency/CPU are unchanged (same partition, same convolver).
EMSCRIPTEN_KEEPALIVE
void amp_set_cab_builtin(void* handle, int which) {
    if (!handle) return;
    auto* c = static_cast<AmpChain*>(handle);
    const std::vector<float> ir = which == kCabBrit412
        ? clipper::dsp::generateBrit4x12IR(c->sr)
        : clipper::dsp::generateDefaultCab2x12IR(c->sr);
    loadIrBothSides(c, ir);
}

// Cab expansion: load a USER IR (mono float samples already at/near the engine
// rate; the convolver resamples if irSampleRate differs, but the worklet hands us
// engine-rate samples). The core PEAK-NORMALIZES it (M6.6 — never trust the
// file's level: a cab must not boost) before preparing both per-side convolvers.
// Declick-bracketed by the worklet exactly like the built-in swap.
EMSCRIPTEN_KEEPALIVE
void amp_load_custom_ir(void* handle, const float* ir_ptr, int ir_len) {
    if (!handle || !ir_ptr || ir_len <= 0) return;
    auto* c = static_cast<AmpChain*>(handle);
    std::vector<float> ir(ir_ptr, ir_ptr + ir_len);
    clipper::dsp::peakNormalizeIR(ir, c->sr);  // NEVER trust the file's level
    loadIrBothSides(c, ir);
}

EMSCRIPTEN_KEEPALIVE
void amp_destroy(void* handle) {
    delete static_cast<AmpChain*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void amp_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    auto* c = static_cast<AmpChain*>(handle);
    // The cab on/off toggle is chain-level and shared by BOTH amp models.
    if (param_id == kAmpParamCab) {
        c->cabOn = value >= 0.5f;
        return;
    }
    // Keep ALL THREE amp voices current at all times, independent of which is active:
    // the model swap is deferred to the declick zero, so a knob moved just before a
    // switch must already be reflected in the incoming voice. The models use DIFFERENT
    // id spaces (AmpModel::PARAM_VOLUME==0 vs Jcm800Amp::PARAM_GAIN==0 vs
    // TwinAmp::PARAM_VOLUME==0), so ids are translated EXPLICITLY, never forwarded
    // blindly. Routing (M10.1, docs §20):
    //   - BASS/MID/TREBLE (1/2/3)  -> SHARED tone -> all three voices.
    //   - VOLUME (0)               -> Clean 120 volume AND Twin channel volume.
    //   - BRIGHT (4)               -> Clean 120 AND Twin (both have a bright switch);
    //                                 the JCM 2204 has none.
    //   - CHORUS_SPEED/DEPTH (6/7) -> Clean 120 chorus AND Twin tremolo SPEED/INTENSITY
    //                                 (a per-model reuse of the two mod knobs).
    //   - CHORUS_MODE (8)          -> Clean 120 only.
    //   - REVERB (9)               -> ALL THREE (clean120 + jcm + twin all have springs;
    //                                 the JCM's is a usability add, docs §19 note).
    //   - GAIN/PRESENCE/MASTER (10/11/12) -> JCM only.
    using A = clipper::dsp::AmpModel;
    using J = clipper::dsp::Jcm800Amp;
    using T = clipper::dsp::TwinAmp;
    switch (param_id) {
        case A::PARAM_VOLUME:
            c->amp.setParameter(A::PARAM_VOLUME, value);
            c->twin.setParameter(T::PARAM_VOLUME, value);
            break;
        case A::PARAM_BASS:
            c->amp.setParameter(A::PARAM_BASS, value);
            c->jcm.setParameter(J::PARAM_BASS, value);
            c->twin.setParameter(T::PARAM_BASS, value);
            break;
        case A::PARAM_MIDDLE:
            c->amp.setParameter(A::PARAM_MIDDLE, value);
            c->jcm.setParameter(J::PARAM_MID, value);
            c->twin.setParameter(T::PARAM_MID, value);
            break;
        case A::PARAM_TREBLE:
            c->amp.setParameter(A::PARAM_TREBLE, value);
            c->jcm.setParameter(J::PARAM_TREBLE, value);
            c->twin.setParameter(T::PARAM_TREBLE, value);
            break;
        case A::PARAM_BRIGHT:
            c->amp.setParameter(A::PARAM_BRIGHT, value);
            c->twin.setParameter(T::PARAM_BRIGHT, value);
            break;
        case A::PARAM_CHORUS_SPEED:
            c->amp.setParameter(A::PARAM_CHORUS_SPEED, value);
            c->twin.setParameter(T::PARAM_SPEED, value);
            break;
        case A::PARAM_CHORUS_DEPTH:
            c->amp.setParameter(A::PARAM_CHORUS_DEPTH, value);
            c->twin.setParameter(T::PARAM_INTENSITY, value);
            break;
        case A::PARAM_CHORUS_MODE:
            c->amp.setParameter(A::PARAM_CHORUS_MODE, value);
            break;
        case A::PARAM_REVERB:
            c->amp.setParameter(A::PARAM_REVERB, value);
            c->jcm.setParameter(J::PARAM_REVERB, value);
            c->twin.setParameter(T::PARAM_REVERB, value);
            break;
        case kAmpParamJcmGain:     c->jcm.setParameter(J::PARAM_GAIN, value); break;
        case kAmpParamJcmPresence: c->jcm.setParameter(J::PARAM_PRESENCE, value); break;
        case kAmpParamJcmMaster:   c->jcm.setParameter(J::PARAM_MASTER, value); break;
        default:
            c->amp.setParameter(param_id, value);
            break;
    }
}

// Total latency of the amp instance in base-rate samples: the Clean 120 is linear
// (0); the JCM adds its oversampling group delay; the cab adds one partition (128)
// when engaged. Both cab sides share the partition size, so this is the same for
// the mono and stereo paths.
EMSCRIPTEN_KEEPALIVE
int amp_latency_samples(void* handle) {
    if (!handle) return 0;
    auto* c = static_cast<AmpChain*>(handle);
    int n = 0;
    if (c->model == kAmpJcm800) n = c->jcm.latencySamples();
    else if (c->model == kAmpTwin) n = c->twin.latencySamples();
    if (c->cabOn) n += c->cabL.latencySamples();
    return n;
}

// Mono path (pre-M6.3, retained for ABI compatibility): amp voice -> single cab.
// Chorus never runs here. Routes to the JCM (mono head) when it is the active model.
EMSCRIPTEN_KEEPALIVE
void amp_process(void* handle, const float* in_ptr, float* out_ptr,
                 int num_frames) {
    if (!handle) return;
    auto* c = static_cast<AmpChain*>(handle);
    if (c->model == kAmpJcm800) c->jcm.process(in_ptr, out_ptr, num_frames);
    else if (c->model == kAmpTwin) c->twin.process(in_ptr, out_ptr, num_frames);
    else c->amp.process(in_ptr, out_ptr, num_frames);
    if (c->cabOn) c->cabL.process(out_ptr, out_ptr, num_frames);  // in-place ok
}

// M6.3 STEREO path. Clean 120: tone+volume -> chorus split -> a per-side cab on
// each of outL/outR (with the chorus mode off, outL == outR). JCM800: it is a MONO
// head, so its single mono output is copied to BOTH sides (dual-mono) and then run
// through the SAME identical cab pair — any stereo width there would be fake, so
// the JCM is honestly mono-into-a-cab-pair. in_ptr may not alias the outputs;
// out_l_ptr and out_r_ptr must be distinct.
EMSCRIPTEN_KEEPALIVE
void amp_process_stereo(void* handle, const float* in_ptr, float* out_l_ptr,
                        float* out_r_ptr, int num_frames) {
    if (!handle) return;
    auto* c = static_cast<AmpChain*>(handle);
    if (c->model == kAmpJcm800) {
        // Mono head into out_l, then mirror to out_r (dual-mono before the cabs).
        c->jcm.process(in_ptr, out_l_ptr, num_frames);
        for (int i = 0; i < num_frames; ++i) out_r_ptr[i] = out_l_ptr[i];
    } else if (c->model == kAmpTwin) {
        // The Twin is a 2×12 COMBO but modelled as a mono head → dual-mono into the
        // identical cab pair (any stereo width here would be fake). The natural cab
        // pairing is the clean212 (a real Twin is a 2×12); the app hints at it.
        c->twin.process(in_ptr, out_l_ptr, num_frames);
        for (int i = 0; i < num_frames; ++i) out_r_ptr[i] = out_l_ptr[i];
    } else {
        c->amp.processStereo(in_ptr, out_l_ptr, out_r_ptr, num_frames);
    }
    if (c->cabOn) {
        c->cabL.process(out_l_ptr, out_l_ptr, num_frames);  // in-place ok
        c->cabR.process(out_r_ptr, out_r_ptr, num_frames);
    }
}

}  // extern "C"
