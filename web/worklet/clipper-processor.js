// Clipper AudioWorklet processor (M3).
//
// Owns a WASM clipper::dsp::RatModel instance and runs each 128-frame render
// quantum through rat_process on the audio thread. Authored as plain JS (no
// bundler transform): build-wasm.sh copies this next to the generated
// clipper.js so the static import below resolves as a sibling in the same
// served directory.
//
// The WASM is SINGLE_FILE (embedded base64) so there is no fetch/XHR here — none
// exists inside AudioWorkletGlobalScope.
import createModule from './clipper.js';

// Must mirror clipper::dsp::RatModel::ParamId in
// core/include/clipper/dsp/RatModel.h (and web/src/params.ts).
const PARAM_DISTORTION = 0;
const PARAM_FILTER = 1;
const PARAM_LEVEL = 2;

const RENDER_QUANTUM = 128;

class ClipperProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();

    this._ready = false;
    this._handle = 0;
    this._inPtr = 0;
    this._outPtr = 0;
    this._module = null;

    // Bypass is handled here in the worklet (pass input through untouched),
    // NOT in the WASM model — it is worklet-local state, always applicable even
    // before the WASM instance is ready.
    this._bypass = false;

    // Queue param/oversampling messages that arrive before WASM is ready.
    this._pending = [];
    this.port.onmessage = (e) => this._onMessage(e.data);

    // sampleRate is a global available in AudioWorkletGlobalScope.
    const sr = (options && options.processorOptions && options.processorOptions.sampleRate) || sampleRate;

    createModule().then((mod) => {
      this._module = mod;
      this._handle = mod._rat_create(sr);
      // Fixed scratch buffers sized to one render quantum (mono).
      this._inPtr = mod._malloc(RENDER_QUANTUM * 4);
      this._outPtr = mod._malloc(RENDER_QUANTUM * 4);
      this._ready = true;

      // Apply any messages that arrived during init.
      for (const msg of this._pending) this._apply(msg);
      this._pending.length = 0;

      this.port.postMessage({
        type: 'ready',
        sampleRate: sr,
        latencySamples: mod._rat_latency_samples(this._handle),
      });
    }).catch((err) => {
      this.port.postMessage({ type: 'error', message: String(err) });
    });
  }

  _onMessage(data) {
    if (!data) return;
    // Bypass toggles worklet-local state; it never touches the WASM model, so
    // it is applied immediately regardless of readiness.
    if (data.type === 'bypass') {
      this._bypass = !!data.on;
      return;
    }
    if (this._ready) this._apply(data);
    else this._pending.push(data);
  }

  _apply(data) {
    const mod = this._module;
    if (data.type === 'param') {
      mod._rat_set_param(this._handle, data.id | 0, +data.value);
    } else if (data.type === 'oversampling') {
      mod._rat_set_oversampling(this._handle, data.factor | 0);
      // Latency depends on the oversampling factor; report the new value so the
      // UI can update its round-trip estimate.
      this.port.postMessage({
        type: 'latency',
        latencySamples: mod._rat_latency_samples(this._handle),
      });
    }
  }

  process(inputs, outputs) {
    const output = outputs[0];
    const input = inputs[0];
    const outCh = output[0];
    if (!outCh) return true;

    const n = outCh.length; // normally 128
    const inCh = input && input[0];

    // Bypass: pass the input straight through (or silence if nothing upstream).
    if (this._bypass) {
      if (inCh) outCh.set(inCh);
      else outCh.fill(0);
      for (let c = 1; c < output.length; c++) output[c].set(outCh);
      return true;
    }

    if (!this._ready) {
      // Not initialised yet: emit silence (keeps the graph alive).
      outCh.fill(0);
      return true;
    }

    const mod = this._module;

    // Copy input into the WASM heap (or zeros if no upstream input connected).
    // Re-fetch HEAPF32 each block: ALLOW_MEMORY_GROWTH can detach old views.
    let heap = mod.HEAPF32;
    const inBase = this._inPtr >> 2;
    if (inCh) {
      for (let i = 0; i < n; i++) heap[inBase + i] = inCh[i];
    } else {
      for (let i = 0; i < n; i++) heap[inBase + i] = 0;
    }

    mod._rat_process(this._handle, this._inPtr, this._outPtr, n);

    // Re-fetch in case processing triggered growth (it won't here, but safe).
    heap = mod.HEAPF32;
    const outBase = this._outPtr >> 2;
    for (let i = 0; i < n; i++) outCh[i] = heap[outBase + i];

    // Mirror to any additional output channels (mono source).
    for (let c = 1; c < output.length; c++) output[c].set(outCh);

    return true;
  }
}

registerProcessor('clipper-processor', ClipperProcessor);
