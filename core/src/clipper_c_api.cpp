// C ABI wrapper around clipper::Processor for WASM (and any future FFI) export.
//
// This is the ONLY place a browser/Emscripten macro is allowed to appear, and
// even here it is kept minimal: EMSCRIPTEN_KEEPALIVE just prevents dead-code
// elimination of the exported symbols. When compiled natively (no Emscripten)
// the macro expands to nothing and this file still builds as plain C++.

#include "clipper/Processor.h"
#include "clipper/dsp/RatModel.h"

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

}  // extern "C"
