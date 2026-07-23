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
// plus the M6.3 chorus params 6/7/8 (AmpModel::ParamId). All arrive by numeric id
// in the message and flow straight through to _amp_set_param, so the worklet only
// needs the ONE chain-level id it treats specially: the cab on/off toggle
// (AMP_PARAM_CAB == AmpModel::PARAM_COUNT == 5, handled by the C ABI wrapper),
// because flipping it changes the amp's reported latency. (Chorus params 6/7/8
// need no special handling here — the C ABI routes them into the ChorusModel.)
const AMP_PARAM_CAB = 5;

const RENDER_QUANTUM = 128;

// Peak meter: report the post-trim input block peak back to the main thread
// every PEAK_REPORT_BLOCKS render quanta (~2.9 ms/block => ~23 ms at 128/44.1k,
// ~43 Hz), carrying the max seen across the window. Cheap and smooth for a UI
// meter without flooding the message port.
const PEAK_REPORT_BLOCKS = 8;

// Output soft limiter (M6.1). Transparent below ±LIM_THRESH, then a tanh knee
// asymptoting to ±1.0 so the louder M6.1 staging can never hand the browser raw
// overs. C1-continuous at the threshold (slope 1). Threshold kept high (0.9) so
// in-range signals (incl. a full-scale bypass passthrough) are ~untouched — a
// ±1.0 sine picks up only ~0.6% 3rd harmonic. See _softLimit below.
const LIM_THRESH = 0.9;

class ClipperProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();

    this._ready = false;
    this._module = null;

    // Two model handles + heap scratch buffers. The amp stage is STEREO from the
    // chorus split on (M6.3), so the output is a PAIR: in -> mid (mono) -> outL/outR.
    this._rat = 0;
    this._amp = 0;
    this._inPtr = 0;
    this._midPtr = 0;
    this._outLPtr = 0;
    this._outRPtr = 0;

    // Per-unit bypass is worklet-local (pass audio through untouched), always
    // applicable even before WASM is ready. pedal bypass = skip the RAT; amp
    // bypass ("power off") = skip amp+cab, passing the pedal output straight out.
    this._bypass = { pedal: false, amp: false };

    // Rig-level input trim (M6.1): a LINEAR gain applied to the input BEFORE the
    // pedal chain (calibration for real-world interface levels). Default unity.
    this._inputGain = 1.0;

    // Peak meter accumulator (post-trim block peak) + block counter.
    this._peakAccum = 0.0;
    this._blockCount = 0;

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
        this._outLPtr = mod._malloc(RENDER_QUANTUM * 4);
        this._outRPtr = mod._malloc(RENDER_QUANTUM * 4);
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
    // Input trim is a worklet-local sample multiply — no core state, apply
    // immediately regardless of WASM readiness. Carries a LINEAR gain.
    if (data.type === 'input') {
      const g = +data.gain;
      this._inputGain = Number.isFinite(g) && g >= 0 ? g : 1.0;
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

  // Soft limiter: identity below ±LIM_THRESH, tanh knee to ±1.0 above it.
  _softLimit(x) {
    const t = LIM_THRESH;
    if (x > t) return t + (1 - t) * Math.tanh((x - t) / (1 - t));
    if (x < -t) return -t + (1 - t) * Math.tanh((x + t) / (1 - t));
    return x;
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

    // 1. Copy input into the WASM heap (or zeros if nothing upstream), applying
    // the rig-level input trim FIRST (before the pedal chain), and track the
    // post-trim block peak for the meter.
    // Re-fetch HEAPF32 each block: ALLOW_MEMORY_GROWTH can detach old views.
    let heap = mod.HEAPF32;
    const inBase = this._inPtr >> 2;
    const g = this._inputGain;
    let blockPeak = 0;
    if (inCh) {
      for (let i = 0; i < n; i++) {
        const s = inCh[i] * g;
        heap[inBase + i] = s;
        const a = s < 0 ? -s : s;
        if (a > blockPeak) blockPeak = a;
      }
    } else {
      for (let i = 0; i < n; i++) heap[inBase + i] = 0;
    }
    // Accumulate + periodically report the post-trim peak (dBFS computed on the
    // main thread) so the UI meter reflects the level actually hitting the pedal.
    if (blockPeak > this._peakAccum) this._peakAccum = blockPeak;
    if (++this._blockCount >= PEAK_REPORT_BLOCKS) {
      this.port.postMessage({ type: 'peak', peak: this._peakAccum });
      this._peakAccum = 0;
      this._blockCount = 0;
    }

    // 2. Pedal stage: RAT into _midPtr, or bypass (copy input -> mid).
    if (this._bypass.pedal) {
      heap.copyWithin(this._midPtr >> 2, inBase, inBase + n);
    } else {
      mod._rat_process(this._rat, this._inPtr, this._midPtr, n);
    }

    // 3. Amp stage: amp -> chorus split -> per-side cab, into _outLPtr/_outRPtr.
    // With chorus OFF the two sides are identical. Amp bypass ("power off") copies
    // the mono pedal signal to BOTH sides (a true stereo passthrough).
    if (this._bypass.amp) {
      heap = mod.HEAPF32;
      const midWords = this._midPtr >> 2;
      heap.copyWithin(this._outLPtr >> 2, midWords, midWords + n);
      heap.copyWithin(this._outRPtr >> 2, midWords, midWords + n);
    } else {
      mod._amp_process_stereo(this._amp, this._midPtr, this._outLPtr, this._outRPtr, n);
    }

    // 4. Read the stereo pair to the output (re-fetch heap in case of growth),
    // each channel through the soft limiter so louder staging can never emit raw
    // overs past ±1.0. A 1-channel output (e.g. an OfflineAudioContext configured
    // mono) takes the LEFT side only.
    heap = mod.HEAPF32;
    const outLBase = this._outLPtr >> 2;
    const outRBase = this._outRPtr >> 2;
    const rCh = output[1];
    for (let i = 0; i < n; i++) {
      outCh[i] = this._softLimit(heap[outLBase + i]);
      if (rCh) rCh[i] = this._softLimit(heap[outRBase + i]);
    }
    // Any channels beyond stereo mirror the left (unusual; keeps output valid).
    for (let c = 2; c < output.length; c++) output[c].set(outCh);

    return true;
  }
}

registerProcessor('clipper-processor', ClipperProcessor);
