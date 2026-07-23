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

// --- M5: clean amp + cab exports ---------------------------------------------
//
// The amp instance is a small CHAIN: a linear AmpModel (volume + tone stack +
// bright) followed by a CabConvolver loaded with the procedural default 2x12 IR.
// The worklet drives the pedal (rat_*) and the amp (amp_*) as SEPARATE instances
// in sequence (pedal -> amp); the cab is part of THIS amp instance, so
// amp_process runs amp -> cab. AMP_PARAM_CAB (id == AmpModel::PARAM_COUNT)
// bypasses the cab for A/B without tearing anything down.

namespace {
// Chain-level param id for the cab on/off toggle (0 = bypass cab, 1 = cab on).
// One past the AmpModel param ids so it never collides with them.
constexpr int kAmpParamCab = clipper::dsp::AmpModel::PARAM_COUNT;  // == 5

struct AmpChain {
    clipper::dsp::AmpModel amp;
    clipper::dsp::CabConvolver cab;
    bool cabOn = true;
};
}  // namespace

// Create the amp+cab chain prepared for the given sample rate. 128 == the
// AudioWorklet render quantum, used as both the amp max block and the cab
// partition size. The default cab IR is generated at the engine rate (no
// resampling needed).
EMSCRIPTEN_KEEPALIVE
void* amp_create(float sample_rate) {
    const double sr = static_cast<double>(sample_rate);
    auto* c = new AmpChain();
    c->amp.prepare(sr, 128);
    const std::vector<float> ir = clipper::dsp::generateDefaultCab2x12IR(sr);
    c->cab.prepare(sr, ir.data(), static_cast<int>(ir.size()), sr, 128);
    return c;
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
// the cab adds one partition (128) when engaged.
EMSCRIPTEN_KEEPALIVE
int amp_latency_samples(void* handle) {
    if (!handle) return 0;
    auto* c = static_cast<AmpChain*>(handle);
    return c->cabOn ? c->cab.latencySamples() : 0;
}

EMSCRIPTEN_KEEPALIVE
void amp_process(void* handle, const float* in_ptr, float* out_ptr,
                 int num_frames) {
    if (!handle) return;
    auto* c = static_cast<AmpChain*>(handle);
    c->amp.process(in_ptr, out_ptr, num_frames);
    if (c->cabOn) c->cab.process(out_ptr, out_ptr, num_frames);  // in-place ok
}

}  // extern "C"
