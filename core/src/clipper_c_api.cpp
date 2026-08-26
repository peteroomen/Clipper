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
#include "clipper/dsp/Ac30Amp.h"
#include "clipper/dsp/OrangeAmp.h"
#include "clipper/dsp/DropModel.h"
#include "clipper/dsp/EqModel.h"
#include "clipper/dsp/MesaAmp.h"
#include "clipper/dsp/ParamGuard.h"
#include "clipper/dsp/RockerverbAmp.h"
#include "clipper/dsp/PhaserModel.h"
#include "clipper/dsp/Ce1Model.h"
#include "clipper/dsp/CompModel.h"
#include "clipper/dsp/DelayModel.h"
#include "clipper/dsp/GateModel.h"
#include "clipper/dsp/WahModel.h"
#include "clipper/dsp/GoldModel.h"
#include "clipper/dsp/MuffModel.h"
#include "clipper/dsp/OptoModel.h"
#include "clipper/dsp/RatModel.h"
#include "clipper/dsp/SdModel.h"
#include "clipper/dsp/TsModel.h"
#include "clipper/dsp/VibeModel.h"

#include <cmath>
#include <vector>

// --- Non-finite parameter gate (2026-07-24 audit, finding 1) ------------------
//
// EVERY *_set_param export below rejects a non-finite value outright. This is the
// one chokepoint guaranteed to be on the web path, and it is a HARD gate, not a
// clamp: there is no sensible in-range meaning for NaN, so the write is dropped
// and the knob keeps its previous value.
//
// Why it matters, measured: before this gate, ONE NaN parameter left every unit
// emitting non-finite samples forever (12672/12672 in a 1 s window for Jcm800Amp,
// AmpModel and RatModel alike). NaN latches — it lands in a smoother value, a
// biquad delay, a WDF cap voltage or a Newton warm start, and every subsequent
// sample inherits it. Writing a good value afterwards never cleared it. Inf was
// always handled correctly; NaN was not, because every clamp in the tree used
// `v < 0 ? 0 : (v > 1 ? 1 : v)`, and both comparisons are false for NaN.
//
// The reachable path was the assistant: web/src/assistant/tools.ts declares
// `minimum: 0, maximum: 1` in the tool JSON schema but nothing enforced it, so a
// non-numeric model emission ("max", null, {}) became NaN via Number(...) and the
// worklet passed `+data.value` straight to _amp_set_param.
//
// This gate is DEFENCE IN DEPTH with the per-model clamps (ParamGuard.h), which
// also cover callers that bypass this ABI entirely — notably the JUCE plugin,
// which calls model setParameter() directly.
#define CLIPPER_REJECT_NON_FINITE(value) \
    do {                                 \
        if (!std::isfinite(value)) return; \
    } while (0)

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
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::Processor*>(handle)->setParameter(param_id, value);
}

EMSCRIPTEN_KEEPALIVE
void clipper_process(void* handle, const float* in_ptr, float* out_ptr,
                     int num_frames) {
    if (!handle) return;
    static_cast<clipper::Processor*>(handle)->process(in_ptr, out_ptr,
                                                      num_frames);
}

// --- Recovery: *_reset exports (2026-07-24 audit, finding 1) ------------------
//
// Before this, a poisoned engine could ONLY be recovered by destroy + recreate:
// there was no reset anywhere in the valve-amp tree and none in this ABI. The
// front-end therefore had no way to un-brick audio short of tearing the whole
// worklet graph down.
//
// Every reset() below clears recursive state and re-parks at the ALREADY-SOLVED DC
// operating point. It deliberately does NOT re-run prepare(): a tube stage's
// prepare() solves its DC point and then settles ~12 grid-leak RCs of silent
// samples (~50 k samples per stage at 4x/48 k; a whole Jcm800Amp::prepare()
// measures ~69 ms), which is not something a recovery path may cost. Knob
// positions and the oversampling factor survive a reset; only state is cleared.
// Allocation-free, so these are safe from the worklet's message handler.

EMSCRIPTEN_KEEPALIVE
void clipper_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::Processor*>(handle)->reset();
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
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::dsp::RatModel*>(handle)->setParameter(param_id, value);
}

// Recovery seam: clear recursive state, keep the knobs / rate / factor. See the
// banner above clipper_reset.
EMSCRIPTEN_KEEPALIVE
void rat_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::dsp::RatModel*>(handle)->reset();
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
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::dsp::SdModel*>(handle)->setParameter(param_id, value);
}

// Recovery seam: clear recursive state, keep the knobs / rate / factor. See the
// banner above clipper_reset.
EMSCRIPTEN_KEEPALIVE
void sd_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::dsp::SdModel*>(handle)->reset();
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
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::dsp::TsModel*>(handle)->setParameter(param_id, value);
}

// Recovery seam: clear recursive state, keep the knobs / rate / factor. See the
// banner above clipper_reset.
EMSCRIPTEN_KEEPALIVE
void ts_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::dsp::TsModel*>(handle)->reset();
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
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::dsp::PhaserModel*>(handle)->setParameter(param_id, value);
}

// Recovery seam: clears the LFO phase and all six allpass memories. See the banner
// above clipper_reset.
EMSCRIPTEN_KEEPALIVE
void phaser_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::dsp::PhaserModel*>(handle)->reset();
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
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::dsp::MuffModel*>(handle)->setParameter(param_id, value);
}

// Recovery seam: clear recursive state, keep the knobs / rate / factor. See the
// banner above clipper_reset.
EMSCRIPTEN_KEEPALIVE
void muff_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::dsp::MuffModel*>(handle)->reset();
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

// --- v1.1 item 6: GOLD overdrive exports -------------------------------------
//
// Additive alongside rat_*/sd_*/ts_*/muff_*/phaser_*, byte-for-byte the same
// opaque-handle ABI so the worklet drives it exactly like any other dirt pedal
// (param slots 0=GAIN, 1=TREBLE, 2=OUTPUT; oversampling + latency shared). Under
// the hood it is the parallel clean/dirt blend with the germanium WDF clipper
// (GoldModel), but the ABI is identical — a chain can mix any of them.

EMSCRIPTEN_KEEPALIVE
void* gold_create(float sample_rate) {
    auto* m = new clipper::dsp::GoldModel();
    m->prepare(static_cast<double>(sample_rate), 128);
    return m;
}

EMSCRIPTEN_KEEPALIVE
void gold_destroy(void* handle) {
    delete static_cast<clipper::dsp::GoldModel*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void gold_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::dsp::GoldModel*>(handle)->setParameter(param_id, value);
}

// Recovery seam: clear recursive state, keep the knobs / rate / factor. See the
// banner above clipper_reset.
EMSCRIPTEN_KEEPALIVE
void gold_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::dsp::GoldModel*>(handle)->reset();
}

EMSCRIPTEN_KEEPALIVE
void gold_set_oversampling(void* handle, int factor) {
    if (!handle) return;
    static_cast<clipper::dsp::GoldModel*>(handle)->setOversampling(factor);
}

EMSCRIPTEN_KEEPALIVE
int gold_latency_samples(void* handle) {
    if (!handle) return 0;
    return static_cast<clipper::dsp::GoldModel*>(handle)->latencySamples();
}

EMSCRIPTEN_KEEPALIVE
void gold_process(void* handle, const float* in_ptr, float* out_ptr,
                  int num_frames) {
    if (!handle) return;
    static_cast<clipper::dsp::GoldModel*>(handle)->process(in_ptr, out_ptr,
                                                           num_frames);
}

// --- The FILTER pedal: wah / envelope filter ("Weeper") exports --------------
//
// Additive alongside rat_*/sd_*/ts_*/muff_*/gold_*/phaser_*, byte-for-byte the
// same opaque-handle ABI so the worklet drives it exactly like every other pedal.
// Param slots: 0 = POSITION (heel→toe), 1 = SENSITIVITY (0 = manual pedal, > 0
// hands the same tank to the envelope follower), 2 = VOICE (the documented
// "vocal mod": R7 10.89 k…100 k, 0.5 = the stock 33 k). The resonator is linear
// and runs at base rate; the transistor OUTPUT stage is nonlinear and runs
// oversampled, so set_oversampling and latency_samples are REAL here (unlike the
// phaser's). Docs §58.

EMSCRIPTEN_KEEPALIVE
void* wah_create(float sample_rate) {
    auto* m = new clipper::dsp::WahModel();
    m->prepare(static_cast<double>(sample_rate), 128);
    return m;
}

EMSCRIPTEN_KEEPALIVE
void wah_destroy(void* handle) {
    delete static_cast<clipper::dsp::WahModel*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void wah_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::dsp::WahModel*>(handle)->setParameter(param_id, value);
}

// Recovery seam: clears the resonator's two integrator states and the envelope
// follower, and re-parks the transistor stage at its cached operating point
// WITHOUT re-solving. See the banner above clipper_reset.
EMSCRIPTEN_KEEPALIVE
void wah_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::dsp::WahModel*>(handle)->reset();
}

EMSCRIPTEN_KEEPALIVE
void wah_set_oversampling(void* handle, int factor) {
    if (!handle) return;
    static_cast<clipper::dsp::WahModel*>(handle)->setOversampling(factor);
}

EMSCRIPTEN_KEEPALIVE
int wah_latency_samples(void* handle) {
    if (!handle) return 0;
    return static_cast<clipper::dsp::WahModel*>(handle)->latencySamples();
}

EMSCRIPTEN_KEEPALIVE
void wah_process(void* handle, const float* in_ptr, float* out_ptr,
                 int num_frames) {
    if (!handle) return;
    static_cast<clipper::dsp::WahModel*>(handle)->process(in_ptr, out_ptr,
                                                          num_frames);
}

// --- M13.1: OTA compressor exports -------------------------------------------
//
// Additive alongside rat_*/sd_*/ts_*/muff_*/gold_*/phaser_*, byte-for-byte the
// same opaque-handle ABI so the worklet drives it exactly like any other pedal.
// Param slots: 0 = SUSTAIN, 1 = UNUSED (a compressor has two knobs — the phaser
// precedent), 2 = LEVEL. Under the hood it is the CA3080 gain cell inside a
// feed-back detector loop (CompModel / CompressorEngine), but the ABI is
// identical, so a chain can mix it with anything.

EMSCRIPTEN_KEEPALIVE
void* comp_create(float sample_rate) {
    auto* m = new clipper::dsp::CompModel();
    m->prepare(static_cast<double>(sample_rate), 128);
    return m;
}

EMSCRIPTEN_KEEPALIVE
void comp_destroy(void* handle) {
    delete static_cast<clipper::dsp::CompModel*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void comp_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::dsp::CompModel*>(handle)->setParameter(param_id, value);
}

// Recovery seam: clear recursive state, re-park at the cached quiescent point,
// keep the knobs / rate / factor. See the banner above clipper_reset.
EMSCRIPTEN_KEEPALIVE
void comp_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::dsp::CompModel*>(handle)->reset();
}

EMSCRIPTEN_KEEPALIVE
void comp_set_oversampling(void* handle, int factor) {
    if (!handle) return;
    static_cast<clipper::dsp::CompModel*>(handle)->setOversampling(factor);
}

EMSCRIPTEN_KEEPALIVE
int comp_latency_samples(void* handle) {
    if (!handle) return 0;
    return static_cast<clipper::dsp::CompModel*>(handle)->latencySamples();
}

EMSCRIPTEN_KEEPALIVE
void comp_process(void* handle, const float* in_ptr, float* out_ptr,
                  int num_frames) {
    if (!handle) return;
    static_cast<clipper::dsp::CompModel*>(handle)->process(in_ptr, out_ptr,
                                                           num_frames);
}

// --- M13.7: CE-1 Chorus Ensemble exports -------------------------------------
//
// Additive alongside every other pedal, byte-for-byte the same opaque-handle ABI.
// Param slots: 0 = RATE, 1 = DEPTH, 2 = MODE (< 0.5 chorus, >= 0.5 vibrato).
//
// Under the hood this is the JC-120 chorus (ChorusModel) re-voiced and summed to
// the CE-1's MONO output jack — see Ce1Model.h and docs §62. The ABI is the same
// as any other pedal's, so a chain can put it anywhere.

EMSCRIPTEN_KEEPALIVE
void* chorus_create(float sample_rate) {
    auto* m = new clipper::dsp::Ce1Model();
    m->prepare(static_cast<double>(sample_rate), 128);
    return m;
}

EMSCRIPTEN_KEEPALIVE
void chorus_destroy(void* handle) {
    delete static_cast<clipper::dsp::Ce1Model*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void chorus_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::dsp::Ce1Model*>(handle)->setParameter(param_id, value);
}

// Recovery seam: clear the delay line, LFO phase and smoothers; keep the knobs.
EMSCRIPTEN_KEEPALIVE
void chorus_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::dsp::Ce1Model*>(handle)->reset();
}

// Accepted and ignored — a linear time-varying effect has nothing to alias.
// Same answer as the phaser above.
EMSCRIPTEN_KEEPALIVE
void chorus_set_oversampling(void* /*handle*/, int /*factor*/) {}

// Always 0: the modulated delay IS the effect, not a compensable latency.
EMSCRIPTEN_KEEPALIVE
int chorus_latency_samples(void* /*handle*/) { return 0; }

EMSCRIPTEN_KEEPALIVE
void chorus_process(void* handle, const float* in_ptr, float* out_ptr,
                    int num_frames) {
    if (!handle) return;
    static_cast<clipper::dsp::Ce1Model*>(handle)->process(in_ptr, out_ptr,
                                                          num_frames);
}


// --- M13.6a: noise gate exports ----------------------------------------------
//
// Additive alongside rat_*/sd_*/ts_*/muff_*/gold_*/comp_*/phaser_*/wah_*,
// byte-for-byte the same opaque-handle ABI so the worklet drives it exactly like
// any other pedal. Param slots: 0 = THRESHOLD, 1 = UNUSED (the reference gate has
// two knobs — the compressor/phaser precedent), 2 = DECAY.
//
// `gate_set_oversampling` is a deliberate NO-OP and `gate_latency_samples`
// returns 0, exactly like the phaser's: the gate's signal path is a multiply and
// the measurement says an oversampler buys nothing (docs §61.7). The exports
// exist so the worklet's per-pedal loop stays uniform.

EMSCRIPTEN_KEEPALIVE
void* gate_create(float sample_rate) {
    auto* m = new clipper::dsp::GateModel();
    m->prepare(static_cast<double>(sample_rate), 128);
    return m;
}

EMSCRIPTEN_KEEPALIVE
void gate_destroy(void* handle) {
    delete static_cast<clipper::dsp::GateModel*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void gate_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::dsp::GateModel*>(handle)->setParameter(param_id, value);
}

// Recovery seam: clear recursive state, re-park at the cached quiescent point,
// keep the knobs / rate. See the banner above clipper_reset.
EMSCRIPTEN_KEEPALIVE
void gate_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::dsp::GateModel*>(handle)->reset();
}

EMSCRIPTEN_KEEPALIVE
void gate_set_oversampling(void* handle, int factor) {
    if (!handle) return;
    static_cast<clipper::dsp::GateModel*>(handle)->setOversampling(factor);
}

EMSCRIPTEN_KEEPALIVE
int gate_latency_samples(void* handle) {
    if (!handle) return 0;
    return static_cast<clipper::dsp::GateModel*>(handle)->latencySamples();
}

EMSCRIPTEN_KEEPALIVE
void gate_process(void* handle, const float* in_ptr, float* out_ptr,
                  int num_frames) {
    if (!handle) return;
    static_cast<clipper::dsp::GateModel*>(handle)->process(in_ptr, out_ptr,
                                                           num_frames);
}

// --- M13.3: optical compressor exports ----------------------------------------
//
// Additive alongside rat_*/sd_*/ts_*/muff_*/gold_*/comp_*/gate_*/phaser_*/wah_*,
// byte-for-byte the same opaque-handle ABI so the worklet drives it exactly like
// any other pedal. Param slots: 0 = PEAK REDUCTION, 1 = MODE (< 0.5 COMPRESS,
// >= 0.5 LIMIT — a DISCRETE two-state switch, the CE-1 precedent), 2 = GAIN.
// This is the lineup's SECOND dynamics pedal and the first one whose slot 1
// carries a real control rather than being unused.
//
// Under the hood it is an EL panel lighting a CdS photocell in a feed-back loop
// (OptoModel / OptoCell), with the cell's own two-stage, program-dependent
// release as the whole point — see docs §64. The ABI is identical to every other
// pedal's, so a chain can mix it with anything.

EMSCRIPTEN_KEEPALIVE
void* opto_create(float sample_rate) {
    auto* m = new clipper::dsp::OptoModel();
    m->prepare(static_cast<double>(sample_rate), 128);
    return m;
}

EMSCRIPTEN_KEEPALIVE
void opto_destroy(void* handle) {
    delete static_cast<clipper::dsp::OptoModel*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void opto_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::dsp::OptoModel*>(handle)->setParameter(param_id, value);
}

// Recovery seam: clear recursive state and re-park the cell DARK, keeping the
// knobs / rate / factor. See the banner above clipper_reset.
EMSCRIPTEN_KEEPALIVE
void opto_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::dsp::OptoModel*>(handle)->reset();
}

EMSCRIPTEN_KEEPALIVE
void opto_set_oversampling(void* handle, int factor) {
    if (!handle) return;
    static_cast<clipper::dsp::OptoModel*>(handle)->setOversampling(factor);
}

EMSCRIPTEN_KEEPALIVE
int opto_latency_samples(void* handle) {
    if (!handle) return 0;
    return static_cast<clipper::dsp::OptoModel*>(handle)->latencySamples();
}

EMSCRIPTEN_KEEPALIVE
void opto_process(void* handle, const float* in_ptr, float* out_ptr,
                  int num_frames) {
    if (!handle) return;
    static_cast<clipper::dsp::OptoModel*>(handle)->process(in_ptr, out_ptr,
                                                           num_frames);
}

// --- M13.5: Uni-Vibe exports --------------------------------------------------
//
// Additive alongside every other pedal prefix and byte-for-byte the same
// opaque-handle ABI. Param slots: 0 = SPEED, 1 = INTENSITY, 2 = MODE. All three
// are REAL controls. MODE is DISCRETE (< 0.5 CHORUS, >= 0.5 VIBRATO) but is
// implemented as a smoothed dry/wet weight rather than a branch — docs §62
// measured a forwarded mode switch clicking at 17.02x the signal's own slew.
//
// Under the hood it is one incandescent lamp (LampDrive) lighting four photocells
// (OptoCell, unwidened) that sweep four STAGGERED allpass stages — docs §67. Two
// things a caller should know: `vibe_latency_samples` returns 72 at the shipped
// 4x, and that factor is NOT about aliasing (the floor is flat in the factor) but
// about the 470 pF stage's corner sitting against Nyquist — docs §67.7.
// INTENSITY 0 is NOT bypass: the lamp still sits at its bias point, so the pedal
// becomes a STATIC comb. That is the circuit, and it is asserted rather than
// assumed.

EMSCRIPTEN_KEEPALIVE
void* vibe_create(float sample_rate) {
    auto* m = new clipper::dsp::VibeModel();
    m->prepare(static_cast<double>(sample_rate), 128);
    return m;
}

EMSCRIPTEN_KEEPALIVE
void vibe_destroy(void* handle) {
    delete static_cast<clipper::dsp::VibeModel*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void vibe_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::dsp::VibeModel*>(handle)->setParameter(param_id, value);
}

// Recovery seam: clear the allpass memories and re-park the lamp and cells at the
// operating point INTENSITY holds, keeping the knobs / rate / factor. See the
// banner above clipper_reset.
EMSCRIPTEN_KEEPALIVE
void vibe_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::dsp::VibeModel*>(handle)->reset();
}

EMSCRIPTEN_KEEPALIVE
void vibe_set_oversampling(void* handle, int factor) {
    if (!handle) return;
    static_cast<clipper::dsp::VibeModel*>(handle)->setOversampling(factor);
}

EMSCRIPTEN_KEEPALIVE
int vibe_latency_samples(void* handle) {
    if (!handle) return 0;
    return static_cast<clipper::dsp::VibeModel*>(handle)->latencySamples();
}

EMSCRIPTEN_KEEPALIVE
void vibe_process(void* handle, const float* in_ptr, float* out_ptr,
                  int num_frames) {
    if (!handle) return;
    static_cast<clipper::dsp::VibeModel*>(handle)->process(in_ptr, out_ptr,
                                                           num_frames);
}

// --- M13.10: polyphonic drop-tune exports -------------------------------------
//
// Additive alongside every other pedal's exports. Two things a caller should
// know, and both are deliberate rather than oversights:
//
//   * `drop_latency_samples` returns 0 even though the shifter's mean read delay
//     is ~36 ms. The delay is a SAWTOOTH by construction (it sweeps a window and
//     re-seats), so there is no single group delay to hand a host; reporting a
//     fixed number would be a lie about a varying quantity, and compensating it
//     would misalign the dry path at OCT+DRY. Same call, same reason, as the BBD
//     delay's (docs §60).
//   * `drop_set_oversampling` is accepted and IGNORED. The signal path is a
//     delay-line read plus a crossfade multiply — linear, time-varying, no
//     nonlinearity — and the test suite proves a render at 1x and 8x is
//     BIT-IDENTICAL rather than taking that on trust. The phaser's arrangement.
//
// The AMOUNT knob is quantized to nine detents inside the model, so a host
// sweeping it lands only on real selector positions. Knob 0.0 is ONE SEMITONE
// DOWN — the setting the pedal exists for.

EMSCRIPTEN_KEEPALIVE
void* drop_create(float sample_rate) {
    auto* m = new clipper::dsp::DropModel();
    m->prepare(static_cast<double>(sample_rate), 128);
    return m;
}

EMSCRIPTEN_KEEPALIVE
void drop_destroy(void* handle) {
    delete static_cast<clipper::dsp::DropModel*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void drop_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::dsp::DropModel*>(handle)->setParameter(param_id, value);
}

// Recovery seam: clear the delay ring and re-seat the shifter's phase, keeping
// the selected position. See the banner above clipper_reset.
EMSCRIPTEN_KEEPALIVE
void drop_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::dsp::DropModel*>(handle)->reset();
}

EMSCRIPTEN_KEEPALIVE
void drop_set_oversampling(void* handle, int factor) {
    if (!handle) return;
    static_cast<clipper::dsp::DropModel*>(handle)->setOversampling(factor);
}

EMSCRIPTEN_KEEPALIVE
int drop_latency_samples(void* handle) {
    if (!handle) return 0;
    return static_cast<clipper::dsp::DropModel*>(handle)->latencySamples();
}

EMSCRIPTEN_KEEPALIVE
void drop_process(void* handle, const float* in_ptr, float* out_ptr,
                  int num_frames) {
    if (!handle) return;
    static_cast<clipper::dsp::DropModel*>(handle)->process(in_ptr, out_ptr,
                                                           num_frames);
}

// --- M13.6: ten-band graphic EQ exports ---------------------------------------
//
// Additive alongside every other pedal's exports, and THE FIRST ONE WHOSE
// PARAMETER SPACE IS LARGER THAN THREE SLOTS. The opaque-handle ABI itself does
// not change at all — `eq_set_param(handle, int id, float)` already took any id;
// it is the callers upstream that assumed three. Slots:
//
//   0  GAIN   (input level slider, 0.5 == unity)
//   1  unused (carried for the shared pedal shape, as the phaser/comp/gate do)
//   2  VOLUME (output level slider, 0.5 == unity)
//   3..12  the ten band sliders, 31.25 Hz .. 16 kHz in ascending order, 0.5 flat
//
// `eq_set_oversampling` is accepted and IGNORED and `eq_latency_samples` returns
// 0: the whole model is linear and time-invariant, and the test suite proves a
// render at 1x and 8x is BIT-IDENTICAL rather than taking it on trust. The
// phaser's arrangement (§12).

EMSCRIPTEN_KEEPALIVE
void* eq_create(float sample_rate) {
    auto* m = new clipper::dsp::EqModel();
    m->prepare(static_cast<double>(sample_rate));
    return m;
}

EMSCRIPTEN_KEEPALIVE
void eq_destroy(void* handle) {
    delete static_cast<clipper::dsp::EqModel*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void eq_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::dsp::EqModel*>(handle)->setParameter(param_id, value);
}

// Recovery seam: clear the ten legs' reactive state, keeping the slider
// positions. See the banner above clipper_reset.
EMSCRIPTEN_KEEPALIVE
void eq_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::dsp::EqModel*>(handle)->reset();
}

EMSCRIPTEN_KEEPALIVE
void eq_set_oversampling(void* handle, int factor) {
    if (!handle) return;
    static_cast<clipper::dsp::EqModel*>(handle)->setOversampling(factor);
}

EMSCRIPTEN_KEEPALIVE
int eq_latency_samples(void* handle) {
    if (!handle) return 0;
    return static_cast<clipper::dsp::EqModel*>(handle)->latencySamples();
}

EMSCRIPTEN_KEEPALIVE
void eq_process(void* handle, const float* in_ptr, float* out_ptr,
                int num_frames) {
    if (!handle) return;
    static_cast<clipper::dsp::EqModel*>(handle)->process(in_ptr, out_ptr, num_frames);
}

// --- M13.4: BBD analog delay exports -----------------------------------------
//
// Additive alongside rat_*/sd_*/ts_*/muff_*/gold_*/comp_*/phaser_*/wah_*, and
// byte-for-byte the same opaque-handle ABI, so the worklet drives the lineup's
// first DELAY exactly like any other pedal. Param slots: 0 = DELAY (the BBD
// clock, 30..550 ms), 1 = FEEDBACK, 2 = BLEND. Under the hood it is a sampled
// bucket-brigade device with an NE570 compander around it (DelayModel), but the
// ABI is identical, so a chain can mix it with anything.
//
// Two things a caller should know. `delay_latency_samples` returns 0 BY DESIGN:
// the dry path never enters the oversampled domain (that is what keeps BLEND 0
// bit-identical to bypass), so the pedal adds no latency to the dry signal.
// And `delay_set_oversampling` really does matter here — the shipped default is
// 8x, derived from the device's own 136.5 kHz maximum clock (docs §60.5), not
// the house 4x.

EMSCRIPTEN_KEEPALIVE
void* delay_create(float sample_rate) {
    auto* m = new clipper::dsp::DelayModel();
    m->prepare(static_cast<double>(sample_rate), 128);
    return m;
}

EMSCRIPTEN_KEEPALIVE
void delay_destroy(void* handle) {
    delete static_cast<clipper::dsp::DelayModel*>(handle);
}

EMSCRIPTEN_KEEPALIVE
void delay_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    CLIPPER_REJECT_NON_FINITE(value);
    static_cast<clipper::dsp::DelayModel*>(handle)->setParameter(param_id, value);
}

// Recovery seam: clear the line, the filters, the compander envelopes and the
// clock phase; keep the knobs / rate / factor. See the banner above clipper_reset.
EMSCRIPTEN_KEEPALIVE
void delay_reset(void* handle) {
    if (!handle) return;
    static_cast<clipper::dsp::DelayModel*>(handle)->reset();
}

EMSCRIPTEN_KEEPALIVE
void delay_set_oversampling(void* handle, int factor) {
    if (!handle) return;
    static_cast<clipper::dsp::DelayModel*>(handle)->setOversampling(factor);
}

EMSCRIPTEN_KEEPALIVE
int delay_latency_samples(void* handle) {
    if (!handle) return 0;
    return static_cast<clipper::dsp::DelayModel*>(handle)->latencySamples();
}

EMSCRIPTEN_KEEPALIVE
void delay_process(void* handle, const float* in_ptr, float* out_ptr,
                   int num_frames) {
    if (!handle) return;
    static_cast<clipper::dsp::DelayModel*>(handle)->process(in_ptr, out_ptr,
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

// M10.3 (docs §57): the Orange OR120's F.A.C. rotary. A NEW id, above every
// existing one, because no other voice has a six-position switch and reusing a
// knob slot for it would make a stale rig state silently mean something else.
// Everything else the OR120 needs is a REUSE, in the house pattern:
//   VOLUME (0)   -> the OR120's single volume (it has no master)
//   BASS (1) / TREBLE (3) -> the James stack; the 'middle' slot (2) never reaches
//                   it (like the AC30, this amp has no mid control)
//   PRESENCE (11) -> HF DRIVE (both are power-amp HF controls in the NFB loop)
//   REVERB (9)   -> the usability spring
// Must mirror web/src/params.ts AMP_PARAM_ORANGE_FAC.
constexpr int kAmpParamOrangeFac = 13;

// M10.4 (docs §69): the Mesa Dual Rectifier's THREE switched controls. All three
// are NEW ids, above every existing one, for the same reason §57 gave the F.A.C.
// its own: no other voice has any of them, and reusing a knob slot would make a
// stale rig state silently mean something else.
//
//   MODE      — the drawing's five states (OR CLN / OR NORM / OR MOD / RED VINT
//               / RED NORM). NOT a channel plus a mode: sheet `mbdr7` enumerates
//               the combinations that actually exist, and two conceivable ones
//               (RED CLEAN, ORANGE VINTAGE) do not.
//   RECTIFIER — silicon vs 5U4, the amp's signature switch.
//   POWERMODE — SPONGY vs BOLD. A SEPARATE mains-primary-side switch, commonly
//               confused with the rectifier selector; the sheet settles it.
//
// Everything else is the house reuse:
//   GAIN (10) / MASTER (12) — same function as the JCM's, so the same slots
//   BASS (1) / MID (2) / TREBLE (3) — the shared tone ids (this amp HAS a mid)
//   PRESENCE (11) — the transcribed 25k pot in the feedback leg
//   REVERB (9)    — carried for lineup parity; a Solo Head has no tank
// Must mirror web/src/params.ts AMP_PARAM_MESA_*.
constexpr int kAmpParamMesaMode = 14;
constexpr int kAmpParamMesaRectifier = 15;
constexpr int kAmpParamMesaPowerMode = 16;

// Which amp model the chain's single handle is currently voicing. M10.1 adds the
// Twin as the THIRD voice (index 2); M10.2 adds the AC30 as the FOURTH voice (index
// 3), purely additive — clean120/jcm/twin ids unchanged.
// M10.3 adds the Orange OR120 as the FIFTH voice (index 4), purely additive.
enum AmpModelId {
    kAmpClean120 = 0,
    kAmpJcm800 = 1,
    kAmpTwin = 2,
    kAmpAc30 = 3,
    kAmpOrange = 4,
    // M10.7 (docs §63) adds the Orange Rockerverb 100 dirty channel as the SIXTH
    // voice (index 5), purely additive. It needs NO new param id: its GAIN and its
    // post-stack VOLUME mean exactly what the JCM800's GAIN (10) and MASTER (12)
    // mean, and its BASS/MIDDLE/TREBLE are the shared tone ids. The panel WORD for
    // slot 12 on this amp is "Volume" (the Rockerverb's dirty channel calls its
    // master that); the SLOT is the master slot because the FUNCTION is a master.
    kAmpRockerverb = 5,
    // M10.4 (docs §69) adds the Mesa Dual Rectifier Solo Head as the SEVENTH
    // voice (index 6), purely additive. Unlike every amp before it this one is
    // TRANSCRIBED from the factory drawing set rather than reconstructed.
    kAmpMesa = 6,
};

// The tube amps' fixed internal oversampling. Docs §18/§20/§23 measured 4× as the
// requirement (8× buys nothing at the composed max-gain floor), so the tube amps run
// at 4× regardless of the rig's pedal-oversampling selector — a deliberate design
// constant, never silently reduced for perf.
constexpr int kJcmOversampling = 4;
constexpr int kTwinOversampling = 4;
constexpr int kAc30Oversampling = 4;
constexpr int kOrangeOversampling = 4;
constexpr int kRockerverbOversampling = 4;
constexpr int kMesaOversampling = 4;

// One per-side cab convolver pair (L/R). Two of these live in every AmpChain so a
// cab change can be BUILT into the spare while the render path keeps using the
// other — see amp_prepare_cab_builtin / amp_commit_cab and ADR 003.
struct CabPair {
    clipper::dsp::CabConvolver l, r;
};

struct AmpChain {
    // Three amp voices behind ONE handle (M9.4 → M10.1). All are created + prepared
    // up front so amp_set_model is a realtime-safe int flip (no allocation on the
    // audio thread). The cab pair below is SHARED: whichever model is active feeds
    // the same per-side CabConvolvers + custom-IR machinery.
    clipper::dsp::AmpModel amp;         // clean 120 (JC-120 style, linear, stereo)
    clipper::dsp::Jcm800Amp jcm;        // Marshall JCM800 2204 (mono head)
    clipper::dsp::TwinAmp twin;         // Fender blackface Twin (mono combo head)
    clipper::dsp::Ac30Amp ac30;         // Vox AC30 top boost (mono combo head)
    clipper::dsp::OrangeAmp orange;     // Orange OR120 Overdrive (mono head)
    clipper::dsp::RockerverbAmp rockerverb;  // Orange Rockerverb 100 dirty ch.
    clipper::dsp::MesaAmp mesa;              // Mesa Dual Rectifier Solo Head
    int model = kAmpClean120;
    // M6.3: the amp goes STEREO from the chorus stage on, so the cab IR runs PER
    // SIDE. Two independent CabConvolver instances (same IR) — the wet R side is
    // genuinely a different signal from the dry L side in chorus mode, so a single
    // mono cab AFTER a wet/dry sum would collapse the stereo bloom. `l` doubles
    // as the mono cab for the legacy amp_process() path. The JCM is a MONO head, so
    // its output is copied to both sides (dual-mono) before the identical cab pair.
    //
    // 2026-07-25 (audit finding 2): the pair is DOUBLE-BUFFERED. `cabs[activeCab]`
    // is what process() runs; `cabs[activeCab ^ 1]` is the spare that a cab change
    // is prepared into off the render path. See the banner above
    // amp_prepare_cab_builtin for why, and ADR 003.
    CabPair cabs[2];
    int activeCab = 0;
    bool cabOn = true;
    // Cab expansion: the engine rate is remembered so the cab can be regenerated
    // (built-in swap) or a user IR reloaded at the right rate on demand.
    double sr = 48000.0;

    CabPair& active() { return cabs[activeCab]; }
    CabPair& inactive() { return cabs[activeCab ^ 1]; }
};

// Built-in cab selector for amp_set_cab_builtin. Kept as small ints so the ABI
// stays language-neutral (the worklet passes 0/1).
enum CabBuiltin { kCabClean212 = 0, kCabBrit412 = 1, kCabOrange412 = 2 };

// Synthesise a built-in cab IR at the chain rate into `out`. `which` selects;
// anything other than kCabBrit412 falls back to the default 2x12 (matches the
// pre-2026-07-25 amp_set_cab_builtin behaviour exactly).
// Fills an out-param rather than returning the vector because everything in this
// file sits inside `extern "C"`, and a C-linkage function returning a
// user-defined type draws -Wreturn-type-c-linkage.
void builtinIr(const AmpChain* c, int which, std::vector<float>& out) {
    out = which == kCabBrit412    ? clipper::dsp::generateBrit4x12IR(c->sr)
        : which == kCabOrange412  ? clipper::dsp::generateOrange4x12IR(c->sr)
                                  : clipper::dsp::generateDefaultCab2x12IR(c->sr);
}

// Load an IR into BOTH sides of `pair` at the chain's engine rate (same IR, same
// 128-sample partition — latency/CPU unchanged from the single built-in).
// ALLOCATES (FFT plan + partitioned spectra + FDL): never call this from a render
// callback. Note that CabConvolver::prepare() also zeroes the FDL and overlap
// history, which is what makes an activated pair start the new IR from silence —
// the same side effect the pre-2026-07-25 in-place prepare() had.
void loadIrBothSides(AmpChain* c, CabPair& pair, const std::vector<float>& ir) {
    pair.l.prepare(c->sr, ir.data(), static_cast<int>(ir.size()), c->sr, 128);
    pair.r.prepare(c->sr, ir.data(), static_cast<int>(ir.size()), c->sr, 128);
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
    // Prepare the AC30 up front as well (M10.2) — same lock-free-swap discipline.
    c->ac30.setOversampling(kAc30Oversampling);
    c->ac30.prepare(sr, 128);
    // Prepare the Orange up front as well (M10.3) — same lock-free-swap discipline.
    c->orange.setOversampling(kOrangeOversampling);
    c->orange.prepare(sr, 128);
    // Prepare the Rockerverb up front as well (M10.7) — same discipline.
    c->rockerverb.setOversampling(kRockerverbOversampling);
    c->rockerverb.prepare(sr, 128);
    // Prepare the Mesa up front as well (M10.4) — same discipline.
    c->mesa.setOversampling(kMesaOversampling);
    c->mesa.prepare(sr, 128);
    // Load the default IR into BOTH double-buffered pairs. Only the active pair is
    // strictly needed at t=0, but preparing both means every pair is always a valid
    // convolver — amp_commit_cab can never activate an unprepared one, even if a
    // caller commits without a preceding prepare. Costs one extra IR synthesis at
    // engine start (nowhere near the audio thread; amp_create already solves four
    // tube DC operating points).
    const std::vector<float> ir = clipper::dsp::generateDefaultCab2x12IR(sr);
    loadIrBothSides(c, c->cabs[0], ir);
    loadIrBothSides(c, c->cabs[1], ir);
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
    // 0 = Clean 120, 1 = JCM800, 2 = Twin, 3 = AC30, 4 = Orange, 5 = Rockerverb,
    // 6 = Mesa Dual Rectifier. Unknown → Clean 120.
    c->model = (which == kAmpJcm800) ? kAmpJcm800
             : (which == kAmpTwin)   ? kAmpTwin
             : (which == kAmpAc30)   ? kAmpAc30
             : (which == kAmpOrange) ? kAmpOrange
             : (which == kAmpRockerverb) ? kAmpRockerverb
             : (which == kAmpMesa)   ? kAmpMesa
                                     : kAmpClean120;
}

// --- Cab change: PREPARE (heavy, off the render path) then COMMIT (O(1)) -------
//
// 2026-07-24 audit finding 2. A cab change used to be ONE call that synthesised an
// IR, heap-copied it, peak-normalized it and ran CabConvolver::prepare twice (FFT
// plan + one spectrum per partition per side) — and the worklet made that call from
// inside its per-sample loop, at the declick fade-out zero. Measured on the shipped
// WASM artifact against a 2.667 ms deadline (48 kHz / 128):
//
//     amp_process_stereo(128)      0.025 ms    0.9 % of the deadline
//     amp_set_cab_builtin         11.4  ms     427 %
//     amp_load_custom_ir(4096)    45.7  ms    1714 %   -> up to 17 dropped quanta
//
// The old design comment argued this was inaudible "because it runs at the
// output-zero of the declick". That reasoning is wrong, and CLAUDE.md now says so:
// output-zero prevents a STEP DISCONTINUITY; it does nothing about MISSING THE
// RENDER DEADLINE. A dropout is not a click, but it is not inaudible either.
//
// The fix is the same trick amp_set_model already uses for the four amp voices:
// keep TWO per-side convolver pairs and an active index. Building the new cab
// happens in the inactive pair (amp_prepare_cab_*, still heavy — just not on the
// render path), and the audio-thread step is a single integer write
// (amp_commit_cab). Deliberately done HERE rather than inside CabConvolver so the
// convolver itself is untouched (it is being rewritten for finding 3 in parallel).
//
// FIDELITY: strictly neutral, and the claim is BIT-IDENTITY. The activated pair was
// prepared by the same CabConvolver::prepare with the same IR samples and the same
// 128 partition, and prepare() zeroes the FDL/overlap — exactly what the old
// in-place prepare() on the live convolver did. core/tests/test_cab_swap.cpp pins
// max|new - old| == 0 over 128 000 samples per side (~2.7 s), across a scripted
// session that includes a custom IR and a swap-and-swap-back.
//
// INVARIANT: every activation is preceded by a prepare() of the pair being
// activated. Do NOT "optimise" the prepare away when the requested IR equals the
// one already sitting in the inactive pair — that pair's FDL still holds history
// from when it was last live, and activating it without a prepare would splice
// stale convolution tail into the output.

// Prepare the BUILT-IN cab IR (0 = Clean 2x12, 1 = Brit 4x12) into the INACTIVE
// pair, regenerated at the engine rate. ALLOCATES and runs FFT setup: call this
// from the message handler / UI thread, NEVER from a render callback. Nothing
// audible changes until amp_commit_cab. Returns 1 on success, 0 on a bad handle.
EMSCRIPTEN_KEEPALIVE
int amp_prepare_cab_builtin(void* handle, int which) {
    if (!handle) return 0;
    auto* c = static_cast<AmpChain*>(handle);
    std::vector<float> ir;
    builtinIr(c, which, ir);
    loadIrBothSides(c, c->inactive(), ir);
    return 1;
}

// Prepare a USER IR (mono float samples already at/near the engine rate; the
// convolver resamples if irSampleRate differs, but the worklet hands us engine-rate
// samples) into the INACTIVE pair. The core PEAK-NORMALIZES it (M6.6 — never trust
// the file's level: a cab must not boost). ALLOCATES: same threading rule as
// amp_prepare_cab_builtin. Returns 1 on success, 0 if the arguments are unusable —
// the caller MUST NOT commit on 0, or it would activate a pair holding the previous
// IR and stale FDL history.
EMSCRIPTEN_KEEPALIVE
int amp_prepare_cab_custom(void* handle, const float* ir_ptr, int ir_len) {
    if (!handle || !ir_ptr || ir_len <= 0) return 0;
    auto* c = static_cast<AmpChain*>(handle);
    std::vector<float> ir(ir_ptr, ir_ptr + ir_len);
    clipper::dsp::peakNormalizeIR(ir, c->sr);  // NEVER trust the file's level
    loadIrBothSides(c, c->inactive(), ir);
    return 1;
}

// Make the most recently prepared cab the live one. This is the ONLY part of a cab
// change that touches the render path: one integer write, no allocation, no FFT, no
// loop. Safe to call from inside process() — the worklet calls it at the declick
// fade-out zero so the IR change also lands without a step discontinuity.
EMSCRIPTEN_KEEPALIVE
void amp_commit_cab(void* handle) {
    if (!handle) return;
    auto* c = static_cast<AmpChain*>(handle);
    c->activeCab ^= 1;
}

// Compatibility wrappers: the pre-2026-07-25 one-shot form, kept so the ABI stays
// purely additive (core tests, the render/bench tools and any FFI keep working).
// These are prepare+commit back to back, so they carry the full 11-46 ms cost and
// are NOT render-callback safe. New callers should use the split pair.
EMSCRIPTEN_KEEPALIVE
void amp_set_cab_builtin(void* handle, int which) {
    if (amp_prepare_cab_builtin(handle, which)) amp_commit_cab(handle);
}

EMSCRIPTEN_KEEPALIVE
void amp_load_custom_ir(void* handle, const float* ir_ptr, int ir_len) {
    if (amp_prepare_cab_custom(handle, ir_ptr, ir_len)) amp_commit_cab(handle);
}

EMSCRIPTEN_KEEPALIVE
void amp_destroy(void* handle) {
    delete static_cast<AmpChain*>(handle);
}

// Recovery seam for the WHOLE amp chain (see the banner above clipper_reset).
// Resets ALL FOUR voices, not just the active one: the inactive voices keep
// accumulating knob writes (amp_set_param deliberately keeps every voice current
// so a model swap lands on the right tone), and a poisoned inactive voice would
// otherwise stay poisoned until it was switched to. Both per-side cab convolvers
// are cleared too — the FDL holds a full IR's worth of history, so a NaN sample
// keeps re-emerging for the length of the impulse response. Both DOUBLE-BUFFERED
// pairs are cleared, for the same reason the inactive amp voices are: an inactive
// pair that has ever been live still holds its FDL history, and a poisoned one
// would spray NaN the moment a cab change activated it.
//
// What is NOT touched: the loaded IR, the active model index, the cab on/off flag,
// the engine rate, and every knob position. This un-bricks audio; it does not
// reset the rig.
EMSCRIPTEN_KEEPALIVE
void amp_reset(void* handle) {
    if (!handle) return;
    auto* c = static_cast<AmpChain*>(handle);
    c->amp.reset();
    c->jcm.reset();
    c->twin.reset();
    c->ac30.reset();
    c->orange.reset();
    c->rockerverb.reset();
    c->mesa.reset();
    for (auto& pair : c->cabs) { pair.l.reset(); pair.r.reset(); }
}

EMSCRIPTEN_KEEPALIVE
void amp_set_param(void* handle, int param_id, float value) {
    if (!handle) return;
    CLIPPER_REJECT_NON_FINITE(value);
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
    //   - CHORUS_MODE (8)          -> Clean 120 chorus mode AND Twin TREMOLO ON/OFF
    //                                 (the Twin has no chorus — slot reused, docs §20).
    //   - REVERB (9)               -> ALL THREE (clean120 + jcm + twin all have springs;
    //                                 the JCM's is a usability add, docs §19 note).
    //   - GAIN/PRESENCE/MASTER (10/11/12) -> JCM only.
    // M10.2 AC30 (voice 3) routing (docs §23): volume → AC30 channel volume;
    // bass/treble → the top-boost stack; the 'middle' slot is UNUSED (the AC30 top-
    // boost has no mid control — never reaches ac30); reverb → AC30 (usability add);
    // and the 'presence' slot (kAmpParamJcmPresence, id 11) is REUSED as the AC30's
    // TOP CUT — both are power-amp HF controls (documented reuse; the UI labels it CUT
    // and inverts the sense). gain/master/bright/chorus never reach the AC30.
    using A = clipper::dsp::AmpModel;
    using J = clipper::dsp::Jcm800Amp;
    using T = clipper::dsp::TwinAmp;
    using X = clipper::dsp::Ac30Amp;
    using O = clipper::dsp::OrangeAmp;
    using R = clipper::dsp::RockerverbAmp;
    using M = clipper::dsp::MesaAmp;
    switch (param_id) {
        case A::PARAM_VOLUME:
            c->amp.setParameter(A::PARAM_VOLUME, value);
            c->twin.setParameter(T::PARAM_VOLUME, value);
            c->ac30.setParameter(X::PARAM_VOLUME, value);
            c->orange.setParameter(O::PARAM_VOLUME, value);
            break;
        case A::PARAM_BASS:
            c->amp.setParameter(A::PARAM_BASS, value);
            c->jcm.setParameter(J::PARAM_BASS, value);
            c->twin.setParameter(T::PARAM_BASS, value);
            c->ac30.setParameter(X::PARAM_BASS, value);
            c->orange.setParameter(O::PARAM_BASS, value);
            c->rockerverb.setParameter(R::PARAM_BASS, value);
            c->mesa.setParameter(M::PARAM_BASS, value);
            break;
        case A::PARAM_MIDDLE:
            c->amp.setParameter(A::PARAM_MIDDLE, value);
            c->jcm.setParameter(J::PARAM_MID, value);
            c->twin.setParameter(T::PARAM_MID, value);
            // M10.7: the Rockerverb DOES have a mid control (its FMV stack's 25k
            // pot) — the first Orange in this repo that does.
            c->rockerverb.setParameter(R::PARAM_MID, value);
            // M10.4: the Mesa's FMV stack has a 25k mid pot on BOTH channels.
            c->mesa.setParameter(M::PARAM_MID, value);
            // AC30 top-boost has NO mid control — the 'middle' slot never reaches it.
            break;
        case A::PARAM_TREBLE:
            c->amp.setParameter(A::PARAM_TREBLE, value);
            c->jcm.setParameter(J::PARAM_TREBLE, value);
            c->twin.setParameter(T::PARAM_TREBLE, value);
            c->ac30.setParameter(X::PARAM_TREBLE, value);
            c->orange.setParameter(O::PARAM_TREBLE, value);
            c->rockerverb.setParameter(R::PARAM_TREBLE, value);
            c->mesa.setParameter(M::PARAM_TREBLE, value);
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
            // M10.1 amendment (docs §20): the Twin has NO chorus, so the 'chorusMode'
            // slot is REUSED as the Twin's TREMOLO ON/OFF (0 = off, ≥1 = on) — the same
            // per-voice slot-reuse pattern as presence→CUT for the AC30. Default 0 (off),
            // so old rigs (chorusMode 0) round-trip with the trem bypassed bit-exact.
            c->twin.setParameter(T::PARAM_TREMOLO_ENABLE, value >= 0.5f ? 1.0f : 0.0f);
            break;
        case A::PARAM_REVERB:
            c->amp.setParameter(A::PARAM_REVERB, value);
            c->jcm.setParameter(J::PARAM_REVERB, value);
            c->twin.setParameter(T::PARAM_REVERB, value);
            c->ac30.setParameter(X::PARAM_REVERB, value);
            c->orange.setParameter(O::PARAM_REVERB, value);
            c->rockerverb.setParameter(R::PARAM_REVERB, value);
            c->mesa.setParameter(M::PARAM_REVERB, value);
            break;
        case kAmpParamJcmGain:
            c->jcm.setParameter(J::PARAM_GAIN, value);
            // M10.7: the Rockerverb's GANGED dual GAIN pot — the same meaning to
            // the player (preamp drive), so the same slot.
            c->rockerverb.setParameter(R::PARAM_GAIN, value);
            // M10.4: the Mesa's per-channel 1M GAIN pot — same meaning, same slot.
            c->mesa.setParameter(M::PARAM_GAIN, value);
            break;
        case kAmpParamJcmPresence:
            c->jcm.setParameter(J::PARAM_PRESENCE, value);
            c->ac30.setParameter(X::PARAM_TOPCUT, value);  // AC30 reuses the slot as TOP CUT
            c->orange.setParameter(O::PARAM_HF_DRIVE, value);  // Orange: HF DRIVE
            // M10.4: the Mesa's transcribed 25k presence pot, in the NFB leg like
            // the JCM's. In the two MODERN modes the loop is OPEN, so this knob
            // correctly does nothing there — that is the circuit, not a bug.
            c->mesa.setParameter(M::PARAM_PRESENCE, value);
            break;
        case kAmpParamJcmMaster:
            c->jcm.setParameter(J::PARAM_MASTER, value);
            // M10.7: the Rockerverb's post-tone-stack VOLUME. The PANEL calls it
            // Volume; the SLOT is the master slot because the FUNCTION is a master
            // (docs §63.5). The amp does not listen to slot 0 at all.
            c->rockerverb.setParameter(R::PARAM_VOLUME, value);
            c->mesa.setParameter(M::PARAM_MASTER, value);
            break;
        case kAmpParamOrangeFac:   c->orange.setParameter(O::PARAM_FAC, value); break;
        // M10.4: the Mesa's three DISCRETE switches. Each arrives as a 0..1 float
        // (the ABI has no integer parameter path) and is quantized here, at the
        // boundary, so the model never sees an in-between state. clampParam01
        // rejects non-finite first (audit finding 1, ADR 002).
        case kAmpParamMesaMode: {
            const double v = clipper::dsp::clampParam01(static_cast<double>(value));
            const int n = static_cast<int>(clipper::dsp::MesaMode::MESA_MODE_COUNT);
            int idx = static_cast<int>(v * static_cast<double>(n - 1) + 0.5);
            if (idx < 0) idx = 0;
            if (idx > n - 1) idx = n - 1;
            c->mesa.setMode(static_cast<clipper::dsp::MesaMode>(idx));
            break;
        }
        case kAmpParamMesaRectifier:
            c->mesa.setRectifier(clipper::dsp::clampParam01(static_cast<double>(value)) < 0.5
                                     ? clipper::dsp::MesaRectifier::Silicon
                                     : clipper::dsp::MesaRectifier::Valve5U4);
            break;
        case kAmpParamMesaPowerMode:
            c->mesa.setPowerMode(clipper::dsp::clampParam01(static_cast<double>(value)) < 0.5
                                     ? clipper::dsp::MesaPowerMode::Bold
                                     : clipper::dsp::MesaPowerMode::Spongy);
            break;
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
    else if (c->model == kAmpAc30) n = c->ac30.latencySamples();
    else if (c->model == kAmpOrange) n = c->orange.latencySamples();
    else if (c->model == kAmpRockerverb) n = c->rockerverb.latencySamples();
    else if (c->model == kAmpMesa) n = c->mesa.latencySamples();
    // Both double-buffered pairs share the 128 partition, so a pending cab change
    // never moves reported latency — but read the ACTIVE pair anyway so this stays
    // correct if a future cab ever uses a different partition size.
    if (c->cabOn) n += c->active().l.latencySamples();
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
    else if (c->model == kAmpAc30) c->ac30.process(in_ptr, out_ptr, num_frames);
    else if (c->model == kAmpOrange) c->orange.process(in_ptr, out_ptr, num_frames);
    else if (c->model == kAmpRockerverb)
        c->rockerverb.process(in_ptr, out_ptr, num_frames);
    else if (c->model == kAmpMesa)
        c->mesa.process(in_ptr, out_ptr, num_frames);
    else c->amp.process(in_ptr, out_ptr, num_frames);
    if (c->cabOn) c->active().l.process(out_ptr, out_ptr, num_frames);  // in-place ok
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
    } else if (c->model == kAmpAc30) {
        // The AC30 is a 2×12 COMBO but modelled as a mono head → dual-mono into the
        // identical cab pair (M10.2). Natural pairing is the clean212 (closest 2×12
        // platform; a future alnico 2×12 IR is ledgered) — the app hints at it.
        c->ac30.process(in_ptr, out_l_ptr, num_frames);
        for (int i = 0; i < num_frames; ++i) out_r_ptr[i] = out_l_ptr[i];
    } else if (c->model == kAmpOrange) {
        // The OR120 is a HEAD into a separate 4x12 — mono, dual-mono into the
        // identical cab pair (M10.3). Natural pairing is the orange412.
        c->orange.process(in_ptr, out_l_ptr, num_frames);
        for (int i = 0; i < num_frames; ++i) out_r_ptr[i] = out_l_ptr[i];
    } else if (c->model == kAmpMesa) {
        // The Dual Rectifier is a HEAD into a separate 4x12 — mono, dual-mono into
        // the identical cab pair (M10.4). Natural pairing is the brit412: a Recto
        // is normally run into a Mesa oversized 4x12, and no such IR exists here
        // yet, so the closest shipped 4x12 is reused rather than inventing one.
        // Named as a follow-up in docs §69.11.
        c->mesa.process(in_ptr, out_l_ptr, num_frames);
        for (int i = 0; i < num_frames; ++i) out_r_ptr[i] = out_l_ptr[i];
    } else if (c->model == kAmpRockerverb) {
        // The Rockerverb 100 is a HEAD into a separate 4x12 — mono, dual-mono into
        // the identical cab pair (M10.7). Natural pairing is the orange412: it is
        // the same PPC412 Orange sells against the OR120, so this voice REUSES that
        // cab rather than inventing a second one (docs §63.9).
        c->rockerverb.process(in_ptr, out_l_ptr, num_frames);
        for (int i = 0; i < num_frames; ++i) out_r_ptr[i] = out_l_ptr[i];
    } else {
        c->amp.processStereo(in_ptr, out_l_ptr, out_r_ptr, num_frames);
    }
    if (c->cabOn) {
        CabPair& cab = c->active();
        cab.l.process(out_l_ptr, out_l_ptr, num_frames);  // in-place ok
        cab.r.process(out_r_ptr, out_r_ptr, num_frames);
    }
}

}  // extern "C"
