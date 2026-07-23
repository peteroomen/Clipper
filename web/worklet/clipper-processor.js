// Clipper AudioWorklet processor (M5).
//
// Owns TWO WASM DSP instances driven in sequence on the audio thread:
//   input -> RatModel (pedal) -> AmpModel+Cab (amp) -> output
// The pedal is rat_* (M3) and the amp is amp_* (M5, a linear tone stack + volume
// + bright, followed by the partitioned cab convolver). They are separate WASM
// instances; the worklet owns the buffers and runs them back to back.
//
// Authored as plain JS (no bundler transform): build-wasm.sh copies this next to
// the generated clipper.js so the static import below resolves as a sibling. The
// WASM is SINGLE_FILE (embedded base64) so there is no fetch/XHR here.
import createModule from './clipper.js';

// Pedal (RAT) param ids are 0/1/2 (RatModel::ParamId); amp param ids are 0..4
// (AmpModel::ParamId). Both arrive by numeric id in the message, so the worklet
// only needs the ONE chain-level id it treats specially: the cab on/off toggle
// (AMP_PARAM_CAB == AmpModel::PARAM_COUNT == 5, handled by the C ABI wrapper),
// because flipping it changes the amp's reported latency.
const AMP_PARAM_CAB = 5;

const RENDER_QUANTUM = 128;

class ClipperProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();

    this._ready = false;
    this._module = null;

    // Two model handles + three heap scratch buffers (in -> mid -> out).
    this._rat = 0;
    this._amp = 0;
    this._inPtr = 0;
    this._midPtr = 0;
    this._outPtr = 0;

    // Per-unit bypass is worklet-local (pass audio through untouched), always
    // applicable even before WASM is ready. pedal bypass = skip the RAT; amp
    // bypass ("power off") = skip amp+cab, passing the pedal output straight out.
    this._bypass = { pedal: false, amp: false };

    // Queue param/oversampling messages that arrive before WASM is ready.
    this._pending = [];
    this.port.onmessage = (e) => this._onMessage(e.data);

    const sr =
      (options && options.processorOptions && options.processorOptions.sampleRate) || sampleRate;
    this._sr = sr;

    createModule()
      .then((mod) => {
        this._module = mod;
        this._rat = mod._rat_create(sr);
        this._amp = mod._amp_create(sr);
        this._inPtr = mod._malloc(RENDER_QUANTUM * 4);
        this._midPtr = mod._malloc(RENDER_QUANTUM * 4);
        this._outPtr = mod._malloc(RENDER_QUANTUM * 4);
        this._ready = true;

        for (const msg of this._pending) this._apply(msg);
        this._pending.length = 0;

        this.port.postMessage({
          type: 'ready',
          sampleRate: sr,
          latencySamples: this._latency(),
        });
      })
      .catch((err) => {
        this.port.postMessage({ type: 'error', message: String(err) });
      });
  }

  // Total model latency, in base-rate samples: pedal oversampling filters +
  // (when the amp is engaged) the cab partition. Amp bypass removes the cab path.
  _latency() {
    if (!this._ready) return 0;
    const mod = this._module;
    let n = mod._rat_latency_samples(this._rat);
    if (!this._bypass.amp) n += mod._amp_latency_samples(this._amp);
    return n;
  }

  _postLatency() {
    this.port.postMessage({ type: 'latency', latencySamples: this._latency() });
  }

  _onMessage(data) {
    if (!data) return;
    // Bypass is worklet-local; apply immediately regardless of WASM readiness.
    // Back-compat: a bypass message with no `unit` targets the pedal (M3/M4).
    if (data.type === 'bypass') {
      const unit = data.unit === 'amp' ? 'amp' : 'pedal';
      this._bypass[unit] = !!data.on;
      if (unit === 'amp' && this._ready) this._postLatency();
      return;
    }
    if (this._ready) this._apply(data);
    else this._pending.push(data);
  }

  _apply(data) {
    const mod = this._module;
    if (data.type === 'param') {
      // Back-compat: a param message with no `unit` targets the pedal.
      const unit = data.unit === 'amp' ? 'amp' : 'pedal';
      if (unit === 'amp') {
        mod._amp_set_param(this._amp, data.id | 0, +data.value);
        // The cab on/off toggle changes the amp's latency; re-report it.
        if ((data.id | 0) === AMP_PARAM_CAB) this._postLatency();
      } else {
        mod._rat_set_param(this._rat, data.id | 0, +data.value);
      }
    } else if (data.type === 'oversampling') {
      mod._rat_set_oversampling(this._rat, data.factor | 0);
      this._postLatency();
    }
  }

  process(inputs, outputs) {
    const output = outputs[0];
    const input = inputs[0];
    const outCh = output[0];
    if (!outCh) return true;

    const n = outCh.length; // normally 128
    const inCh = input && input[0];

    if (!this._ready) {
      outCh.fill(0);
      return true;
    }

    const mod = this._module;

    // 1. Copy input into the WASM heap (or zeros if nothing upstream).
    // Re-fetch HEAPF32 each block: ALLOW_MEMORY_GROWTH can detach old views.
    let heap = mod.HEAPF32;
    const inBase = this._inPtr >> 2;
    if (inCh) {
      for (let i = 0; i < n; i++) heap[inBase + i] = inCh[i];
    } else {
      for (let i = 0; i < n; i++) heap[inBase + i] = 0;
    }

    // 2. Pedal stage: RAT into _midPtr, or bypass (copy input -> mid).
    if (this._bypass.pedal) {
      heap.copyWithin(this._midPtr >> 2, inBase, inBase + n);
    } else {
      mod._rat_process(this._rat, this._inPtr, this._midPtr, n);
    }

    // 3. Amp stage: amp+cab into _outPtr, or bypass (copy mid -> out).
    if (this._bypass.amp) {
      heap = mod.HEAPF32;
      heap.copyWithin(this._outPtr >> 2, this._midPtr >> 2, (this._midPtr >> 2) + n);
    } else {
      mod._amp_process(this._amp, this._midPtr, this._outPtr, n);
    }

    // 4. Read _outPtr to the output (re-fetch heap in case of growth).
    heap = mod.HEAPF32;
    const outBase = this._outPtr >> 2;
    for (let i = 0; i < n; i++) outCh[i] = heap[outBase + i];

    // Mirror to any additional output channels (mono source).
    for (let c = 1; c < output.length; c++) output[c].set(outCh);

    return true;
  }
}

registerProcessor('clipper-processor', ClipperProcessor);
