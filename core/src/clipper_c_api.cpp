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
#include "clipper/dsp/RatModel.h"
#include "clipper/dsp/SdModel.h"

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

struct AmpChain {
    clipper::dsp::AmpModel amp;
    // M6.3: the amp goes STEREO from the chorus stage on, so the cab IR runs PER
    // SIDE. Two independent CabConvolver instances (same IR) — the wet R side is
    // genuinely a different signal from the dry L side in chorus mode, so a single
    // mono cab AFTER a wet/dry sum would collapse the stereo bloom. cabL doubles
    // as the mono cab for the legacy amp_process() path.
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
    const std::vector<float> ir = clipper::dsp::generateDefaultCab2x12IR(sr);
    loadIrBothSides(c, ir);
    return c;
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
    if (param_id == kAmpParamCab) {
        c->cabOn = value >= 0.5f;
    } else {
        c->amp.setParameter(param_id, value);
    }
}

// Total latency of the amp instance in samples: the amp itself is linear (0),
// the cab adds one partition (128) when engaged. Both sides share the partition
// size, so this is the same for the mono and stereo paths.
EMSCRIPTEN_KEEPALIVE
int amp_latency_samples(void* handle) {
    if (!handle) return 0;
    auto* c = static_cast<AmpChain*>(handle);
    return c->cabOn ? c->cabL.latencySamples() : 0;
}

// Mono path (pre-M6.3, retained for ABI compatibility): tone+volume -> single
// cab. Chorus never runs here.
EMSCRIPTEN_KEEPALIVE
void amp_process(void* handle, const float* in_ptr, float* out_ptr,
                 int num_frames) {
    if (!handle) return;
    auto* c = static_cast<AmpChain*>(handle);
    c->amp.process(in_ptr, out_ptr, num_frames);
    if (c->cabOn) c->cabL.process(out_ptr, out_ptr, num_frames);  // in-place ok
}

// M6.3 STEREO path: tone+volume -> chorus split -> a per-side cab on each of
// outL/outR. With the chorus mode off, outL == outR. in_ptr may not alias the
// outputs; outL_ptr and outR_ptr must be distinct.
EMSCRIPTEN_KEEPALIVE
void amp_process_stereo(void* handle, const float* in_ptr, float* out_l_ptr,
                        float* out_r_ptr, int num_frames) {
    if (!handle) return;
    auto* c = static_cast<AmpChain*>(handle);
    c->amp.processStereo(in_ptr, out_l_ptr, out_r_ptr, num_frames);
    if (c->cabOn) {
        c->cabL.process(out_l_ptr, out_l_ptr, num_frames);  // in-place ok
        c->cabR.process(out_r_ptr, out_r_ptr, num_frames);
    }
}

}  // extern "C"
