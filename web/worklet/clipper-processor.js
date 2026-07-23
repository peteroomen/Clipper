// Clipper AudioWorklet processor (M6.4).
//
// Owns a DYNAMIC, ORDERED CHAIN of pedal DSP instances plus one amp instance,
// driven in sequence on the audio thread:
//
//   input(trim) -> pedals[0] -> pedals[1] -> ... -> AmpModel+Cab(stereo) -> out
//
// Each pedal is a rat_* WASM instance (M3); the amp is amp_* (M5, linear tone
// stack + volume + bright + M6.3 stereo chorus + per-side cab). They are separate
// WASM instances; the worklet owns the heap scratch buffers and runs them back to
// back. Multiple RAT instances are independent (the model is handle-based with no
// shared/global DSP state), so an arbitrary number of pedals stack safely.
//
// M6.4 chain edits (add / remove / reorder / swap) arrive as a single `chain`
// message and are applied CLICK-FREE via a short raised-cosine output fade
// (declick): the output ramps to zero, the topology swap happens at that zero
// point (between render quanta, in the message handler — no allocation inside
// process()), then ramps back up. Because the discontinuity always lands at
// output-zero there is no step/pop and no zipper. A plain PARAMETER change (knob)
// is NOT bracketed — the core's ~5 ms one-pole smoothing already declicks those.
//
// Authored as plain JS (no bundler transform): build-wasm.sh copies this next to
// the generated clipper.js so the static import below resolves as a sibling. The
// WASM is SINGLE_FILE (embedded base64) so there is no fetch/XHR here.
import createModule from './clipper.js';

// Amp param ids flow straight through to _amp_set_param by numeric id; the only
// one the worklet treats specially is the cab on/off toggle (it changes reported
// latency). (Chorus params 6/7/8 route into the ChorusModel in the C ABI.)
const AMP_PARAM_CAB = 5;

const RENDER_QUANTUM = 128;

// Peak meter: report the post-trim input block peak back to the main thread every
// PEAK_REPORT_BLOCKS render quanta (~23 ms at 128/44.1k), carrying the window max.
const PEAK_REPORT_BLOCKS = 8;

// Output soft limiter — a TRUE SAFETY catch, not a tone stage. Transparent below
// ±LIM_THRESH, then a narrow tanh knee asymptoting to ±1.0 so the output can
// never emit raw overs. M6.5: raised 0.9 -> 0.97 and the amp gain staging pulled
// down (volume tops out at unity, see AmpModel.cpp) so the CLEAN pedal-bypassed
// chain stays below the knee at realistic levels instead of soft-clipping every
// cycle ("fizz"). MIRRORS clipper::dsp::OutputLimiter::kThreshold and its tanh
// formula (core/include/clipper/dsp/OutputLimiter.h) — keep the two in sync; the
// native tests exercise that C++ implementation.
const LIM_THRESH = 0.97;

// Declick fade for chain edits (M6.4): ~6 ms each way. Long enough to be
// inaudible as a transient, short enough that a reorder feels instant.
const DECLICK_SECONDS = 0.006;

class ClipperProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();

    this._ready = false;
    this._module = null;

    // The ORDERED pedal chain: [{ id, handle, engaged }]. Built once WASM is
    // ready with a single default RAT (so the offline audio tests, which never
    // send a `chain` message and address the pedal by the legacy unit:'pedal',
    // keep working). The app replaces it with a real `chain` message on start.
    this._chain = [];
    this._amp = 0;

    // Heap scratch: trimmed input, two ping-pong pedal buffers, the amp input
    // (mono), and the stereo amp output pair.
    this._inPtr = 0;
    this._pbufA = 0;
    this._pbufB = 0;
    this._midPtr = 0;
    this._outLPtr = 0;
    this._outRPtr = 0;

    // Amp bypass ("power off") is worklet-local; pedal bypass is per-instance
    // (node.engaged). Both always apply even before WASM is ready.
    this._bypassAmp = false;

    // Global nonlinear-stage oversampling factor (rig-level), applied to every
    // pedal handle (existing + newly created).
    this._oversampling = 4;

    // Rig-level input trim (M6.1): a LINEAR gain applied BEFORE the chain.
    this._inputGain = 1.0;

    // Peak meter accumulator + block counter.
    this._peakAccum = 0.0;
    this._blockCount = 0;

    // Declick state machine (M6.4).
    this._declickGain = 1.0; // current applied output gain
    this._declickPhase = 'idle'; // 'idle' | 'out' | 'in'
    this._declickStep = 0; // per-sample gain delta (set in prepare)
    this._pending = null; // { nodes, removed } waiting for the fade-out zero

    // Queue messages that arrive before WASM is ready.
    this._pending_msgs = [];
    this.port.onmessage = (e) => this._onMessage(e.data);

    const sr =
      (options && options.processorOptions && options.processorOptions.sampleRate) || sampleRate;
    this._sr = sr;
    this._declickStep = 1 / Math.max(1, Math.round(DECLICK_SECONDS * sr));

    createModule()
      .then((mod) => {
        this._module = mod;
        this._amp = mod._amp_create(sr);
        this._inPtr = mod._malloc(RENDER_QUANTUM * 4);
        this._pbufA = mod._malloc(RENDER_QUANTUM * 4);
        this._pbufB = mod._malloc(RENDER_QUANTUM * 4);
        this._midPtr = mod._malloc(RENDER_QUANTUM * 4);
        this._outLPtr = mod._malloc(RENDER_QUANTUM * 4);
        this._outRPtr = mod._malloc(RENDER_QUANTUM * 4);

        // Default chain: a single engaged RAT (legacy addressing target).
        this._chain = [this._createPedal('default', 'rat', null, true)];

        this._ready = true;

        for (const msg of this._pending_msgs) this._apply(msg);
        this._pending_msgs.length = 0;

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

  // Create a pedal DSP instance for the chain. `params` (optional) is
  // {distortion, filter, level} in 0..1; the global oversampling factor is
  // applied. Returns a chain node.
  _createPedal(id, _type, params, engaged) {
    const mod = this._module;
    const handle = mod._rat_create(this._sr);
    mod._rat_set_oversampling(handle, this._oversampling | 0);
    if (params) {
      mod._rat_set_param(handle, 0, +params.distortion);
      mod._rat_set_param(handle, 1, +params.filter);
      mod._rat_set_param(handle, 2, +params.level);
    }
    return { id, handle, engaged: !!engaged };
  }

  _destroyPedal(node) {
    if (node && node.handle) this._module._rat_destroy(node.handle);
  }

  // Total model latency in base-rate samples: every ENGAGED pedal's oversampling
  // group delay (they run in series) + (when the amp is powered) the cab
  // partition. Bypassed pedals and a powered-off amp contribute nothing.
  _latency() {
    if (!this._ready) return 0;
    const mod = this._module;
    let n = 0;
    for (const node of this._chain) {
      if (node.engaged) n += mod._rat_latency_samples(node.handle);
    }
    if (!this._bypassAmp) n += mod._amp_latency_samples(this._amp);
    return n;
  }

  _postLatency() {
    this.port.postMessage({ type: 'latency', latencySamples: this._latency() });
  }

  // Find a chain node by id, or fall back to the first pedal (legacy
  // unit:'pedal' addressing from the M3..M6.3 offline tests / messages).
  _pedalById(id) {
    if (id != null) {
      for (const node of this._chain) if (node.id === id) return node;
      return null;
    }
    return this._chain[0] || null;
  }

  _onMessage(data) {
    if (!data) return;

    // Bypass is worklet-local; apply immediately regardless of WASM readiness.
    // unit:'amp' powers the amp; unit:'pedal' (default) toggles a pedal instance
    // (by pedalId, else the first pedal — back-compat).
    if (data.type === 'bypass') {
      if (data.unit === 'amp') {
        this._bypassAmp = !!data.on;
        if (this._ready) this._postLatency();
      } else {
        const node = this._pedalById(data.pedalId);
        if (node) node.engaged = !data.on; // bypass on => not engaged
        if (this._ready) this._postLatency();
      }
      return;
    }

    // Input trim: a worklet-local sample multiply — no core state.
    if (data.type === 'input') {
      const g = +data.gain;
      this._inputGain = Number.isFinite(g) && g >= 0 ? g : 1.0;
      return;
    }

    if (this._ready) this._apply(data);
    else this._pending_msgs.push(data);
  }

  // Build the new chain from a `chain` message (ordered [{id,type,engaged,
  // params}]), REUSING existing handles for ids that persist (so a reorder keeps
  // each pedal's smoothing/oversampler state) and creating handles for new ids.
  // Handles for removed ids are destroyed only AFTER the fade reaches zero.
  _prepareChain(spec) {
    const oldById = new Map(this._chain.map((n) => [n.id, n]));
    const nodes = [];
    const keep = new Set();
    for (const p of spec) {
      const existing = oldById.get(p.id);
      if (existing) {
        existing.engaged = !!p.engaged;
        // Refresh params on reused handles (cheap; core smooths them).
        if (p.params) {
          const mod = this._module;
          mod._rat_set_param(existing.handle, 0, +p.params.distortion);
          mod._rat_set_param(existing.handle, 1, +p.params.filter);
          mod._rat_set_param(existing.handle, 2, +p.params.level);
        }
        nodes.push(existing);
        keep.add(p.id);
      } else {
        nodes.push(this._createPedal(p.id, p.type || 'rat', p.params, p.engaged));
      }
    }
    const removed = this._chain.filter((n) => !keep.has(n.id));
    return { nodes, removed };
  }

  // Immediately install a prepared chain (destroy removed handles, swap in the
  // new node array). Called at the fade-out zero, or up front if a new chain
  // edit arrives while a previous one is still fading.
  _commitPending() {
    if (!this._pending) return;
    for (const node of this._pending.removed) this._destroyPedal(node);
    this._chain = this._pending.nodes;
    this._pending = null;
    this._postLatency();
  }

  _apply(data) {
    const mod = this._module;
    if (data.type === 'param') {
      if (data.unit === 'amp') {
        mod._amp_set_param(this._amp, data.id | 0, +data.value);
        if ((data.id | 0) === AMP_PARAM_CAB) this._postLatency();
      } else {
        const node = this._pedalById(data.pedalId);
        if (node) mod._rat_set_param(node.handle, data.id | 0, +data.value);
      }
    } else if (data.type === 'oversampling') {
      this._oversampling = data.factor | 0;
      for (const node of this._chain) mod._rat_set_oversampling(node.handle, this._oversampling);
      this._postLatency();
    } else if (data.type === 'chain') {
      // A new topology. If an edit is still fading, commit it first so the diff
      // is against the current committed chain, then prepare + start a new fade.
      if (this._pending) this._commitPending();
      this._pending = this._prepareChain(data.pedals || []);
      this._declickPhase = 'out'; // ramp to zero, swap at the bottom, ramp back
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

    // 1. Trimmed input into the WASM heap; track the post-trim block peak.
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
    if (blockPeak > this._peakAccum) this._peakAccum = blockPeak;
    if (++this._blockCount >= PEAK_REPORT_BLOCKS) {
      this.port.postMessage({ type: 'peak', peak: this._peakAccum });
      this._peakAccum = 0;
      this._blockCount = 0;
    }

    // 2. Pedal chain: ping-pong the mono signal through each ENGAGED pedal in
    // order (bypassed pedals are skipped — a true pass-through). Start from a
    // copy of the trimmed input; end in _midPtr (the amp input).
    let cur = this._pbufA;
    let other = this._pbufB;
    heap.copyWithin(cur >> 2, inBase, inBase + n); // trimmed input -> cur
    for (const node of this._chain) {
      if (!node.engaged) continue;
      mod._rat_process(node.handle, cur, other, n);
      const t = cur;
      cur = other;
      other = t;
    }
    heap = mod.HEAPF32;
    heap.copyWithin(this._midPtr >> 2, cur >> 2, (cur >> 2) + n); // chain out -> mid

    // 3. Amp stage: amp -> chorus split -> per-side cab into _outLPtr/_outRPtr.
    // Powered off copies the mono chain signal to BOTH sides (stereo passthrough).
    if (this._bypassAmp) {
      heap = mod.HEAPF32;
      const midWords = this._midPtr >> 2;
      heap.copyWithin(this._outLPtr >> 2, midWords, midWords + n);
      heap.copyWithin(this._outRPtr >> 2, midWords, midWords + n);
    } else {
      mod._amp_process_stereo(this._amp, this._midPtr, this._outLPtr, this._outRPtr, n);
    }

    // 4. Read the stereo pair out through the declick fade + soft limiter. A
    // 1-channel output (mono OfflineAudioContext) takes the LEFT side only.
    heap = mod.HEAPF32;
    const outLBase = this._outLPtr >> 2;
    const outRBase = this._outRPtr >> 2;
    const rCh = output[1];
    let dg = this._declickGain;
    const step = this._declickStep;
    for (let i = 0; i < n; i++) {
      // Advance the declick envelope (raised-cosine shaped for C1 smoothness).
      if (this._declickPhase === 'out') {
        dg -= step;
        if (dg <= 0) {
          dg = 0;
          this._commitPending(); // topology swap happens exactly at zero
          this._declickPhase = 'in';
        }
      } else if (this._declickPhase === 'in') {
        dg += step;
        if (dg >= 1) {
          dg = 1;
          this._declickPhase = 'idle';
        }
      }
      // Raised-cosine map of the linear ramp dg in [0,1] -> smooth gain.
      const env = this._declickPhase === 'idle' && dg >= 1 ? 1 : 0.5 - 0.5 * Math.cos(Math.PI * dg);
      outCh[i] = this._softLimit(heap[outLBase + i] * env);
      if (rCh) rCh[i] = this._softLimit(heap[outRBase + i] * env);
    }
    this._declickGain = dg;
    // Any channels beyond stereo mirror the left (keeps the output valid).
    for (let c = 2; c < output.length; c++) output[c].set(outCh);

    return true;
  }
}

registerProcessor('clipper-processor', ClipperProcessor);
