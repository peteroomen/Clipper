// Clipper AudioWorklet processor (M0).
//
// Owns a WASM clipper::Processor instance and runs each 128-frame render quantum
// through clipper_process on the audio thread. Authored as plain JS (no bundler
// transform): build-wasm.sh copies this next to the generated clipper.js so the
// static import below resolves as a sibling in the same served directory.
//
// The WASM is SINGLE_FILE (embedded base64) so there is no fetch/XHR here — none
// exists inside AudioWorkletGlobalScope.
import createModule from './clipper.js';

// Must mirror clipper::ParamId in core/include/clipper/Processor.h.
const PARAM_GAIN = 0;

const RENDER_QUANTUM = 128;

class ClipperProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();

    this._ready = false;
    this._handle = 0;
    this._inPtr = 0;
    this._outPtr = 0;
    this._module = null;

    // Queue param messages that arrive before WASM is ready.
    this._pending = [];
    this.port.onmessage = (e) => this._onMessage(e.data);

    // sampleRate is a global available in AudioWorkletGlobalScope.
    const sr = (options && options.processorOptions && options.processorOptions.sampleRate) || sampleRate;

    createModule().then((mod) => {
      this._module = mod;
      this._handle = mod._clipper_create(sr);
      // Fixed scratch buffers sized to one render quantum (mono).
      this._inPtr = mod._malloc(RENDER_QUANTUM * 4);
      this._outPtr = mod._malloc(RENDER_QUANTUM * 4);
      this._ready = true;

      // Apply any params that arrived during init.
      for (const msg of this._pending) this._applyParam(msg);
      this._pending.length = 0;

      this.port.postMessage({ type: 'ready', sampleRate: sr });
    }).catch((err) => {
      this.port.postMessage({ type: 'error', message: String(err) });
    });
  }

  _onMessage(data) {
    if (!data) return;
    if (data.type === 'param') {
      if (this._ready) this._applyParam(data);
      else this._pending.push(data);
    }
  }

  _applyParam(data) {
    this._module._clipper_set_param(this._handle, data.id | 0, +data.value);
  }

  process(inputs, outputs) {
    const output = outputs[0];
    const input = inputs[0];
    const outCh = output[0];
    if (!outCh) return true;

    if (!this._ready) {
      // Not initialised yet: emit silence (keeps the graph alive).
      outCh.fill(0);
      return true;
    }

    const mod = this._module;
    const n = outCh.length; // normally 128

    // Copy input into the WASM heap (or zeros if no upstream input connected).
    // Re-fetch HEAPF32 each block: ALLOW_MEMORY_GROWTH can detach old views.
    let heap = mod.HEAPF32;
    const inBase = this._inPtr >> 2;
    const inCh = input && input[0];
    if (inCh) {
      for (let i = 0; i < n; i++) heap[inBase + i] = inCh[i];
    } else {
      for (let i = 0; i < n; i++) heap[inBase + i] = 0;
    }

    mod._clipper_process(this._handle, this._inPtr, this._outPtr, n);

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
