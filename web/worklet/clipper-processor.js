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
// M11: STOMP toggles (pedal bypass / amp power) ride the SAME declick bracket —
// pre-M11 they flipped the signal path instantly and popped audibly (caught by
// web/tests/expectations.spec.ts; see docs §26).
//
// Authored as plain JS (no bundler transform): build-wasm.sh copies this next to
// the generated clipper.js so the static import below resolves as a sibling. The
// WASM is SINGLE_FILE (embedded base64) so there is no fetch/XHR here.
import createModule from './clipper.js';

// Amp param ids flow straight through to _amp_set_param by numeric id; the only
// one the worklet treats specially is the cab on/off toggle (it changes reported
// latency). (Chorus params 6/7/8 route into the ChorusModel, and the M6.7 reverb
// mix id 9 into the ReverbModel, in the C ABI — no worklet special-casing.)
const AMP_PARAM_CAB = 5;

const RENDER_QUANTUM = 128;

// Peak meter: report the post-trim input block peak back to the main thread every
// PEAK_REPORT_BLOCKS render quanta (~23 ms at 128/44.1k), carrying the window max.
const PEAK_REPORT_BLOCKS = 8;

// Tuner tap (M7). When an ENGAGED tuner is in the chain, tap the post-trim,
// pre-chain signal (the raw guitar) into a ring buffer and post the most recent
// TUNER_FRAME_SIZE samples to the main thread every TUNER_HOP_BLOCKS quanta, where
// `pitchy` (McLeod pitch method) runs the detection. Zero cost when no tuner is
// engaged (the ring is never written and nothing is posted). MIRRORS the shared
// constants in web/src/tuner.ts (TUNER_FRAME_SIZE / TUNER_HOP_BLOCKS) — keep in
// sync. 4096 (~85 ms @48k) is chosen so a 7-string low B (~31 Hz) locks reliably.
const TUNER_FRAME_SIZE = 4096;
const TUNER_HOP_BLOCKS = 4;

// Output limiter (M6.6) — a TRUE SAFETY catch, not a tone stage, and now a
// LOOKAHEAD GAIN-RIDER, not a waveshaper: the tanh knee it replaces bent every
// sample above threshold into harmonics, which is exactly the clean-path "fizz"
// the user kept hearing (the cab's presence bump pushed peaks into the knee).
// Gain-riding turns overs DOWN instead of bending them — briefly ducks level,
// adds zero harmonics, and is bit-transparent (gain exactly 1) whenever no over
// is in the lookahead window. One shared gain for L/R so the image never shifts.
// MIRRORS clipper::dsp::OutputLimiter (core/include/clipper/dsp/OutputLimiter.h)
// — keep the two in sync; the native tests exercise that C++ implementation.
const LIM_THRESH = 0.97;      // ceiling
const LIM_LOOKAHEAD = 64;     // samples of delay / sliding-max window
const LIM_RELEASE_S = 0.040;
const LIM_HOLD_S = 0.050;     // pin gain after attack: no inter-peak AM ripple

class LookaheadLimiter {
  constructor(sampleRate) {
    this.attackCoeff = Math.exp(-1 / (LIM_LOOKAHEAD / 3));
    this.releaseCoeff = Math.exp(-1 / (LIM_RELEASE_S * sampleRate));
    this.holdSamples = Math.floor(LIM_HOLD_S * sampleRate);
    this.holdCount = 0;
    this.delayL = new Float32Array(LIM_LOOKAHEAD);
    this.delayR = new Float32Array(LIM_LOOKAHEAD);
    // Monotonic deque (ring) for the sliding-window maximum; +2 so a full
    // deque is distinguishable from an empty one.
    const cap = LIM_LOOKAHEAD + 2;
    this.dqIdx = new Float64Array(cap);
    this.dqVal = new Float32Array(cap);
    this.dqCap = cap;
    this.dqHead = 0;
    this.dqTail = 0;
    this.writePos = 0;
    this.sampleCount = 0;
    this.gain = 1;
  }

  // Process one stereo sample; results land in this.outL / this.outR (no
  // per-sample allocation — the audio thread must not create garbage). JS
  // numbers are doubles, so the release cannot stall like a float32 accumulator.
  processSample(inL, inR) {
    const mag = Math.max(Math.abs(inL), Math.abs(inR));
    // push
    while (this.dqTail !== this.dqHead) {
      const prev = (this.dqTail + this.dqCap - 1) % this.dqCap;
      if (this.dqVal[prev] > mag) break;
      this.dqTail = prev;
    }
    this.dqIdx[this.dqTail] = this.sampleCount;
    this.dqVal[this.dqTail] = mag;
    this.dqTail = (this.dqTail + 1) % this.dqCap;
    // expire
    const oldest = this.sampleCount - LIM_LOOKAHEAD;
    while (this.dqHead !== this.dqTail && this.dqIdx[this.dqHead] < oldest)
      this.dqHead = (this.dqHead + 1) % this.dqCap;
    this.sampleCount++;

    const outL = this.delayL[this.writePos];
    const outR = this.delayR[this.writePos];
    this.delayL[this.writePos] = inL;
    this.delayR[this.writePos] = inR;
    this.writePos = (this.writePos + 1) % LIM_LOOKAHEAD;

    const peak = this.dqVal[this.dqHead];
    const target = peak > LIM_THRESH ? LIM_THRESH / peak : 1;
    if (target < this.gain) {
      this.gain = this.attackCoeff * this.gain + (1 - this.attackCoeff) * target;
      if (this.gain < target) this.gain = target;
      this.holdCount = this.holdSamples;
    } else if (this.holdCount > 0) {
      this.holdCount--;
    } else {
      this.gain = this.releaseCoeff * this.gain + (1 - this.releaseCoeff) * target;
    }
    if (target >= 1 && this.gain > 0.9999) this.gain = 1;

    let l = outL * this.gain;
    let r = outR * this.gain;
    if (l > 1) l = 1; else if (l < -1) l = -1;
    if (r > 1) r = 1; else if (r < -1) r = -1;
    this.outL = l;
    this.outR = r;
  }
}

// Declick fade for chain edits (M6.4): ~6 ms each way. Long enough to be
// inaudible as a transient, short enough that a reorder feels instant.
const DECLICK_SECONDS = 0.006;

// Extra samples to HOLD the output at zero after a cab swap, before the fade-in
// starts (2026-07-25).
//
// CabConvolver::prepare() zeroes the frequency-domain delay line and the overlap
// buffer, so a freshly swapped-in cab emits SILENCE until its FDL refills — and
// because the swap lands at an arbitrary offset within the convolver's 128-sample
// partition grid, that silence can begin up to one partition late and then last a
// full partition. Worst case: ~2 partitions of dead output after the commit.
//
// The 6 ms (288-sample) fade-in is longer than that, so without a hold the gap
// landed in the MIDDLE of the ramp — measured stepping from 59 % gain straight to
// exact zero and back. Holding at zero for two partitions first puts the whole dead
// region inside the silence the declick already provides, so the fade-in only ever
// ramps real audio. Costs ~5.3 ms on a deliberate cab change and nothing otherwise.
//
// This is a PRE-EXISTING artifact (it is in the shipped v1.1 worklet too) that the
// old before-rendering-starts cab test could never see; see docs §30.
const CAB_SWAP_DEAD_SAMPLES = 256;

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
    this._declickPhase = 'idle'; // 'idle' | 'out' | 'hold' | 'in'
    // Samples still to spend parked at zero before the fade-in (see
    // CAB_SWAP_DEAD_SAMPLES). Set by _commitPending when a cab swap lands.
    this._declickHold = 0;
    this._declickStep = 0; // per-sample gain delta (set in prepare)
    this._pending = null; // { nodes, removed } waiting for the fade-out zero
    // Cab expansion: a pending cab swap (built-in select or custom-IR load) that
    // is applied at the SAME declick fade-out zero as a chain edit (M6.4), so the
    // IR change is click-free. { builtin } or { irPtr, irLen }. Built-in indices
    // mirror CabBuiltin in clipper_c_api.cpp: 0 = Clean 2x12, 1 = Brit 4x12,
    // 2 = Orange 4x12 (M10.3).
    this._pendingCab = null;
    // M9.4/M10.1/v1.1/M10.3: a pending amp-model swap (0 = Clean 120, 1 = JCM800,
    // 2 = Twin, 3 = AC30, 4 = Orange OR120), applied at the SAME declick fade-out
    // zero, so switching
    // amps mid-signal is click-free. The index is passed through opaquely to
    // _amp_set_model.
    this._pendingAmpModel = null;
    // M11: pending STOMP toggles (pedal bypass + amp power), applied at the SAME
    // declick fade-out zero. Pre-M11 a stomp flipped the signal path instantly
    // between two waveforms mid-sample — an audible pop the player-expectations
    // suite caught (a clean −20 dBFS tone vs a driven pedal output can step by
    // hundreds of millivolts in one sample). [{ pedalId, on }] / true|false|null.
    this._pendingBypass = [];
    this._pendingAmpBypass = null;

    // Tuner mute (M7). An engaged tuner mutes the whole chain output (true tuner-
    // pedal behavior: stomp = mute). `_muteActive` is derived from the chain (any
    // engaged tuner); `_muteGain` ramps toward it click-free reusing the declick
    // step, so mute/unmute is a smooth raised-cosine fade with no pop.
    this._muteActive = false;
    this._muteGain = 1.0; // linear ramp position 0..1 (1 = pass, 0 = muted)

    // Tuner tap ring (M7). Written only while _muteActive; posted every hop.
    this._tapRing = new Float32Array(TUNER_FRAME_SIZE);
    this._tapWrite = 0;
    this._tapOut = new Float32Array(TUNER_FRAME_SIZE); // reused post scratch
    this._tapBlockCount = 0;

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

  // Create a pedal DSP instance for the chain. `type` selects the WASM model
  // ('sd1' = Boss SD-1 overdrive, M8; 'tuner' = M7's handle-less mute/tap node,
  // no WASM instance; anything else = RAT, M3). The dirt pedals share the same
  // handle ABI (create / set_param id 0/1/2 / set_oversampling / latency /
  // process), so the node carries its `type` and every call routes through the
  // matching sd_*/rat_* export. `params` (optional) is {distortion, filter,
  // level} in 0..1 (for an SD-1 these slots ARE drive/tone/level). Returns a
  // chain node.
  _createPedal(id, type, params, engaged) {
    if (type === 'tuner') {
      return { id, type: 'tuner', handle: 0, engaged: !!engaged };
    }
    const mod = this._module;
    // Each pedal type binds to a C-ABI export prefix; every pedal shares the
    // identical opaque-handle ABI (create / set_param 0/1/2 / set_oversampling /
    // latency / process), so routing is by prefix. 'sd1'=SD-1, 'ts'=TS Screamer
    // (both the SD-1 engine family), 'muff'=Pi fuzz, 'gold'=the GOLD clean-blend
    // overdrive (slots read GAIN/TREBLE/OUTPUT), 'comp'=the M13.1 "Squash" OTA
    // compressor (slot 0 = SUSTAIN, slot 2 = LEVEL, slot 1 unused-but-carried),
    // 'gate'=the M13.6a "Curfew" noise gate (slot 0 = THRESHOLD, slot 2 = DECAY,
    // slot 1 unused-but-carried; set_oversampling is a no-op and latency is 0,
    // because a gate's signal path is a multiply — docs §61.7),
    // 'phaser'=Ninety (linear allpass sweep: set_oversampling is a no-op,
    // latency 0), anything else = RAT.
    // overdrive (slots read GAIN/TREBLE/OUTPUT), 'phaser'=Ninety (linear allpass
    // sweep: set_oversampling is a no-op, latency 0), 'wah'=Weeper (the FIRST
    // FILTER pedal: a swept resonant tank into a real transistor stage, so unlike
    // the phaser its set_oversampling and latency ARE real — slots read
    // POSITION/SENSE/VOICE), anything else = RAT.
    const t = type === 'sd1' ? 'sd1' : type === 'ts' ? 'ts' : type === 'muff' ? 'muff' : type === 'gold' ? 'gold' : type === 'comp' ? 'comp' : type === 'gate' ? 'gate' : type === 'phaser' ? 'phaser' : type === 'wah' ? 'wah' : 'rat';
    const P = t === 'sd1' ? '_sd' : t === 'ts' ? '_ts' : t === 'muff' ? '_muff' : t === 'gold' ? '_gold' : t === 'comp' ? '_comp' : t === 'gate' ? '_gate' : t === 'phaser' ? '_phaser' : t === 'wah' ? '_wah' : '_rat';
    const handle = mod[P + '_create'](this._sr);
    mod[P + '_set_oversampling'](handle, this._oversampling | 0);
    if (params) {
      // Slot 0/1/2 are pedal-agnostic; for a phaser slot 0 = SPEED, 1/2 unused;
      // for a wah they are POSITION / SENSITIVITY / VOICE.
      mod[P + '_set_param'](handle, 0, +params.distortion);
      mod[P + '_set_param'](handle, 1, +params.filter);
      mod[P + '_set_param'](handle, 2, +params.level);
    }
    return { id, type: t, handle, engaged: !!engaged };
  }

  // C-ABI export prefix for a node's type
  // ('_sd' | '_ts' | '_muff' | '_gold' | '_comp' | '_phaser' | '_rat').
  // ('_sd' | '_ts' | '_muff' | '_gold' | '_phaser' | '_wah' | '_rat').
  _prefix(node) {
    return node.type === 'sd1' ? '_sd' : node.type === 'ts' ? '_ts' : node.type === 'muff' ? '_muff' : node.type === 'gold' ? '_gold' : node.type === 'comp' ? '_comp' : node.type === 'gate' ? '_gate' : node.type === 'phaser' ? '_phaser' : node.type === 'wah' ? '_wah' : '_rat';
  }

  _destroyPedal(node) {
    if (!node || !node.handle) return;
    this._module[this._prefix(node) + '_destroy'](node.handle);
  }

  // Per-node ABI routing by C-ABI prefix (sd_*/ts_*/phaser_*/wah_*/rat_*). Keeps the
  // chain dispatch type-agnostic everywhere below.
  _pedalSetParam(node, id, value) {
    this._module[this._prefix(node) + '_set_param'](node.handle, id, value);
  }
  _pedalSetOversampling(node, factor) {
    this._module[this._prefix(node) + '_set_oversampling'](node.handle, factor);
  }
  _pedalLatency(node) {
    return this._module[this._prefix(node) + '_latency_samples'](node.handle);
  }
  _pedalProcess(node, inPtr, outPtr, n) {
    this._module[this._prefix(node) + '_process'](node.handle, inPtr, outPtr, n);
  }

  // Recompute the tuner mute state from the current chain: muted iff any tuner
  // node is engaged. Called after every chain commit and bypass toggle; the
  // per-sample ramp (in process) does the click-free fade toward it.
  _refreshMute() {
    this._muteActive = this._chain.some((n) => n.type === 'tuner' && n.engaged);
  }

  // Total model latency in base-rate samples: every ENGAGED pedal's oversampling
  // group delay (they run in series) + (when the amp is powered) the cab
  // partition. Bypassed pedals and a powered-off amp contribute nothing.
  _latency() {
    if (!this._ready) return 0;
    const mod = this._module;
    let n = 0;
    for (const node of this._chain) {
      // Tuner nodes are handle-less (zero latency); dirt pedals report via
      // their model's latency export (routed by type in _pedalLatency).
      if (node.engaged && node.handle) n += this._pedalLatency(node);
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

    // Bypass (stomp) is worklet-local. Before WASM is ready it applies
    // immediately (no audio is running — nothing can click). Once live, a stomp
    // is a topology change like any other (M11): it is STAGED and applied at the
    // declick fade-out zero, so engaging/disengaging a pedal (or amp power)
    // mid-note no longer steps the waveform (the pre-M11 instant flip measured
    // as an audible pop — see web/tests/expectations.spec.ts).
    // unit:'amp' powers the amp; unit:'pedal' (default) toggles a pedal instance
    // (by pedalId, else the first pedal — back-compat).
    if (data.type === 'bypass') {
      if (!this._ready) {
        if (data.unit === 'amp') {
          this._bypassAmp = !!data.on;
        } else {
          const node = this._pedalById(data.pedalId);
          if (node) node.engaged = !data.on; // bypass on => not engaged
          this._refreshMute(); // a tuner toggle changes the mute state
        }
        return;
      }
      // An earlier edit still fading is NOT force-committed (see _stageEdit): this
      // stomp joins it and they land together at the one fade zero.
      if (data.unit === 'amp') {
        this._pendingAmpBypass = !!data.on;
      } else {
        this._pendingBypass.push({ pedalId: data.pedalId, on: !!data.on });
      }
      this._stageEdit();
      // The latency echo stays SYNCHRONOUS — it is the tests' delivery barrier
      // (and _commitPending republishes it once the flip lands).
      this._postLatency();
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
  //
  // The diff BASE is the chain that will be live once the current fade commits —
  // i.e. the already-staged pending nodes if there are any, else the committed
  // chain. Before 2026-07-25 a second edit inside one fade window force-committed
  // the first one so the base was always `this._chain`; that committed a topology
  // swap at a NON-ZERO declick gain (see _stageEdit). Diffing against the pending
  // nodes instead lets both edits ride the one fade to its zero.
  _prepareChain(spec) {
    const base = this._pending ? this._pending.nodes : this._chain;
    // Carry the earlier edit's removals forward. Without this union, staging
    // "remove pedal X" and then any second chain edit before the fade zero leaks
    // X's WASM handle forever: X is already absent from `base`, so it can never
    // appear in this edit's `removed` list.
    const carried = this._pending ? this._pending.removed : [];
    const oldById = new Map(base.map((n) => [n.id, n]));
    const nodes = [];
    const keep = new Set();
    for (const p of spec) {
      const existing = oldById.get(p.id);
      if (existing) {
        existing.engaged = !!p.engaged;
        // Refresh params on reused handles (cheap; core smooths them). A reused
        // id keeps its original type (a swap mints a NEW id), so route by it.
        if (p.params) {
          this._pedalSetParam(existing, 0, +p.params.distortion);
          this._pedalSetParam(existing, 1, +p.params.filter);
          this._pedalSetParam(existing, 2, +p.params.level);
        }
        nodes.push(existing);
        keep.add(p.id);
      } else {
        nodes.push(this._createPedal(p.id, p.type || 'rat', p.params, p.engaged));
      }
    }
    const removed = carried.concat(base.filter((n) => !keep.has(n.id)));
    return { nodes, removed };
  }

  // Arm the declick fade for a freshly staged topology edit.
  //
  // 2026-07-25 (audit finding 2, adjacent defect b): every staging path used to run
  // `if (this._hasPendingEdit()) this._commitPending();` first. If a SECOND edit
  // arrived before the fade reached zero — a drag-reorder, a rapid add/remove, or
  // the assistant issuing two tool calls in one turn — the FIRST edit's topology
  // swap was committed at whatever gain the ramp happened to be at, which is
  // precisely the step discontinuity the declick exists to prevent. Worst case is
  // not "mid-ramp" but FULL gain: when both messages land in the same audio-thread
  // message drain, no render has advanced the ramp yet, so _declickGain is still 1.0
  // and the first edit steps the waveform at full amplitude.
  //
  // Now edits MERGE and ride the fade that is already running: _pendingBypass
  // accumulates, _pendingCab / _pendingAmpModel take the newest value, and
  // _prepareChain diffs against the pending nodes. Re-asserting 'out' is a no-op
  // when the fade is already heading down, and correctly reverses an 'in' ramp.
  _stageEdit() {
    this._declickPhase = 'out'; // ramp to zero, swap at the bottom, ramp back
  }

  // Any topology edit staged for the next declick fade-out zero?
  _hasPendingEdit() {
    return !!this._pending || !!this._pendingCab || this._pendingAmpModel != null ||
           this._pendingBypass.length > 0 || this._pendingAmpBypass != null;
  }

  // Install every staged topology edit. Called from process() at the declick
  // fade-out zero — i.e. THIS RUNS ON THE AUDIO THREAD, INSIDE THE RENDER
  // CALLBACK — so everything here must be O(1)-ish and allocation-free. The heavy
  // half of a cab change (IR synthesis, peak normalize, FFT partitioning: 11-46 ms
  // measured, against a 2.667 ms deadline) was moved out to the `cab` message
  // handler on 2026-07-25; what is left is an integer flip in the C ABI. See the
  // banner above amp_prepare_cab_builtin in core/src/clipper_c_api.cpp and ADR 003.
  //
  // Still not strictly RT-clean: _destroyPedal() frees a WASM handle (bounded,
  // microseconds) and _postLatency() posts a message. Both are ledgered follow-ups.
  _commitPending() {
    if (!this._hasPendingEdit()) return;
    if (this._pending) {
      for (const node of this._pending.removed) this._destroyPedal(node);
      this._chain = this._pending.nodes;
      this._pending = null;
      this._refreshMute(); // the new topology may add/remove an engaged tuner
    }
    if (this._pendingCab) {
      // The new IR was already synthesised, normalized and FFT-partitioned into the
      // C ABI's INACTIVE convolver pair back in the message handler. All that is
      // left on the audio thread is making that pair the live one: one int write.
      this._module._amp_commit_cab(this._amp);
      this._pendingCab = null;
      // The swapped-in convolver's FDL is empty, so it emits silence for up to two
      // partitions. Stay parked at zero across that gap instead of ramping into it.
      this._declickHold = CAB_SWAP_DEAD_SAMPLES;
    }
    if (this._pendingAmpModel != null) {
      // M9.4: flip the amp voice at the fade zero. Both voices are pre-created in
      // the C ABI, so this is just an int flip (no allocation) — but the JCM adds
      // oversampling latency vs the linear Clean 120, so latency is republished.
      this._module._amp_set_model(this._amp, this._pendingAmpModel | 0);
      this._pendingAmpModel = null;
    }
    if (this._pendingBypass.length) {
      // M11: stomp toggles land at the fade zero, exactly like a chain edit.
      for (const b of this._pendingBypass) {
        const node = this._pedalById(b.pedalId);
        if (node) node.engaged = !b.on; // bypass on => not engaged
      }
      this._pendingBypass.length = 0;
      this._refreshMute(); // a tuner stomp changes the mute state
    }
    if (this._pendingAmpBypass != null) {
      this._bypassAmp = this._pendingAmpBypass;
      this._pendingAmpBypass = null;
    }
    this._postLatency();
  }

  _apply(data) {
    const mod = this._module;
    if (data.type === 'param') {
      // Reject a non-finite value BEFORE it reaches the engine (2026-07-24 audit,
      // finding 1). `+data.value` turns "max", null and {} into NaN, and one NaN
      // latches permanently in the smoother / biquad / Newton state — every sample
      // after it is non-finite and writing a good value never clears it. Dropping
      // the message is the right answer: NaN has no in-range meaning, so the knob
      // keeps its previous value. Same guard shape as the `input` handler above,
      // which had it all along. (The core rejects it too — see clipper_c_api.cpp —
      // but this keeps a bad message from ever crossing the WASM boundary.)
      const v = +data.value;
      if (!Number.isFinite(v)) return;
      if (data.unit === 'amp') {
        mod._amp_set_param(this._amp, data.id | 0, v);
        if ((data.id | 0) === AMP_PARAM_CAB) this._postLatency();
      } else {
        const node = this._pedalById(data.pedalId);
        if (node) this._pedalSetParam(node, data.id | 0, v);
      }
    } else if (data.type === 'oversampling') {
      this._oversampling = data.factor | 0;
      for (const node of this._chain) this._pedalSetOversampling(node, this._oversampling);
      this._postLatency();
    } else if (data.type === 'chain') {
      // A new topology. _prepareChain diffs against the pending nodes when an edit
      // is still fading (see there), so both edits ride the one fade to its zero.
      this._pending = this._prepareChain(data.pedals || []);
      this._stageEdit();
    } else if (data.type === 'cab') {
      // Cab change, click-free AND dropout-free (2026-07-24 audit finding 2).
      //
      // ALL the expensive work happens RIGHT HERE: synthesising or copying the IR,
      // peak-normalizing it, and running CabConvolver::prepare on both sides of the
      // C ABI's spare convolver pair (FFT plan + one spectrum per partition). That
      // was measured at 11.4 ms for a built-in and 45.7 ms for a 4096-tap upload —
      // 427 % and 1714 % of the 2.667 ms render deadline. It used to run from inside
      // process()'s per-sample loop, dropping up to 17 consecutive render quanta.
      //
      // BE PRECISE ABOUT WHAT THIS BUYS. AudioWorkletProcessor.port.onmessage runs
      // on the audio RENDERING thread, so this is NOT "off the audio thread". What
      // it does is move the work BETWEEN render quanta instead of mid-block: a
      // guaranteed multi-quantum stall becomes at worst one late quantum, at the
      // instant of a deliberate user action, with process() itself now allocation-
      // free. Getting the partitioned spectra computed on the MAIN thread and copied
      // in is the fully correct fix and is a bigger change — ledgered, not done.
      //
      // Only the O(1) index flip is deferred to the fade zero, in _commitPending.
      let prepared = 0;
      if (data.custom && data.custom.length) {
        const arr = data.custom; // transferred Float32Array (mono, engine-rate)
        const len = arr.length | 0;
        const irPtr = mod._malloc(len * 4);
        // Re-fetch HEAPF32: the _malloc above can have grown (and detached) it.
        mod.HEAPF32.set(arr, irPtr >> 2);
        prepared = mod._amp_prepare_cab_custom(this._amp, irPtr, len);
        mod._free(irPtr); // the core copied the samples in
      } else {
        prepared = mod._amp_prepare_cab_builtin(this._amp, (data.builtin | 0) || 0);
      }
      // Never stage a commit the prepare did not back: flipping to an unprepared
      // pair would splice in the previous IR plus its stale convolution tail.
      if (prepared) {
        this._pendingCab = { ready: true };
        this._stageEdit();
      } else {
        this._postLatency(); // keep the tests' delivery barrier honest
      }
    } else if (data.type === 'ampModel') {
      // M9.4/M10.1: swap the amp voice (0 = Clean 120, 1 = JCM800, 2 = Twin) click-
      // free — staged here and applied at the declick fade-out zero, like a cab swap.
      this._pendingAmpModel = (data.model | 0) || 0;
      this._stageEdit();
    }
  }

  // M6.6: final gain-riding limiter (see LookaheadLimiter above). Created
  // lazily so the sampleRate global is valid.
  _limiter() {
    if (!this._lim) this._lim = new LookaheadLimiter(sampleRate);
    return this._lim;
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
    const tapping = this._muteActive; // an engaged tuner wants the raw guitar
    let tw = this._tapWrite;
    const ring = this._tapRing;
    if (inCh) {
      for (let i = 0; i < n; i++) {
        const s = inCh[i] * g;
        heap[inBase + i] = s;
        const a = s < 0 ? -s : s;
        if (a > blockPeak) blockPeak = a;
        if (tapping) {
          ring[tw] = s;
          tw = tw + 1 === TUNER_FRAME_SIZE ? 0 : tw + 1;
        }
      }
    } else {
      for (let i = 0; i < n; i++) {
        heap[inBase + i] = 0;
        if (tapping) {
          ring[tw] = 0;
          tw = tw + 1 === TUNER_FRAME_SIZE ? 0 : tw + 1;
        }
      }
    }
    this._tapWrite = tw;
    if (blockPeak > this._peakAccum) this._peakAccum = blockPeak;
    if (++this._blockCount >= PEAK_REPORT_BLOCKS) {
      this.port.postMessage({ type: 'peak', peak: this._peakAccum });
      this._peakAccum = 0;
      this._blockCount = 0;
    }

    // Tuner frame post: every TUNER_HOP_BLOCKS quanta while tapping, linearize the
    // ring (oldest -> newest) into a reused scratch and post a COPY (no transfer,
    // so the scratch is reused next hop — no per-block allocation on the audio
    // thread). The main thread runs the McLeod pitch detection on it.
    if (tapping) {
      if (++this._tapBlockCount >= TUNER_HOP_BLOCKS) {
        this._tapBlockCount = 0;
        const out = this._tapOut;
        const head = tw; // oldest sample is at the write cursor
        out.set(ring.subarray(head), 0);
        out.set(ring.subarray(0, head), TUNER_FRAME_SIZE - head);
        this.port.postMessage({ type: 'tunerFrame', samples: out, sampleRate: this._sr });
      }
    } else {
      this._tapBlockCount = 0;
    }

    // 2. Pedal chain: ping-pong the mono signal through each ENGAGED pedal in
    // order (bypassed pedals are skipped — a true pass-through). Start from a
    // copy of the trimmed input; end in _midPtr (the amp input).
    let cur = this._pbufA;
    let other = this._pbufB;
    heap.copyWithin(cur >> 2, inBase, inBase + n); // trimmed input -> cur
    for (const node of this._chain) {
      // Skip bypassed pedals AND tuners (a tuner is a handle-less pass-through:
      // its only effect is the output mute, applied at the end of the block).
      if (!node.engaged || !node.handle) continue;
      this._pedalProcess(node, cur, other, n);
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
    let mg = this._muteGain;
    const mTarget = this._muteActive ? 0 : 1; // engaged tuner -> mute
    for (let i = 0; i < n; i++) {
      // Advance the declick envelope (raised-cosine shaped for C1 smoothness).
      if (this._declickPhase === 'out') {
        dg -= step;
        if (dg <= 0) {
          dg = 0;
          this._commitPending(); // topology swap happens exactly at zero
          // Re-fetch HEAPF32 (2026-07-24 audit finding 2, adjacent defect a). `heap`
          // was captured before this loop; _commitPending() can still free a removed
          // pedal handle, and ALLOW_MEMORY_GROWTH detaches every old view the moment
          // the heap grows — after which `heap[...]` reads NaN/undefined for the rest
          // of the block. Every other site in this file re-fetches for exactly this
          // reason; this was the one that didn't, and the one place a malloc actually
          // used to happen (the IR load, now hoisted into the message handler).
          heap = mod.HEAPF32;
          // A cab swap leaves the new convolver silent for up to two partitions;
          // stay at zero across that dead region before ramping back in.
          this._declickPhase = this._declickHold > 0 ? 'hold' : 'in';
        }
      } else if (this._declickPhase === 'hold') {
        // Parked at zero (dg stays 0, so env is 0) while the swapped-in cab's
        // frequency-domain delay line refills.
        if (--this._declickHold <= 0) this._declickPhase = 'in';
      } else if (this._declickPhase === 'in') {
        dg += step;
        if (dg >= 1) {
          dg = 1;
          this._declickPhase = 'idle';
        }
      }
      // Advance the tuner-mute ramp toward its target (same 6 ms step), so a
      // stomp mutes/unmutes click-free.
      if (mg < mTarget) mg = Math.min(mTarget, mg + step);
      else if (mg > mTarget) mg = Math.max(mTarget, mg - step);
      // Raised-cosine map of the linear ramps in [0,1] -> smooth gains.
      const env = this._declickPhase === 'idle' && dg >= 1 ? 1 : 0.5 - 0.5 * Math.cos(Math.PI * dg);
      const menv = mg >= 1 ? 1 : mg <= 0 ? 0 : 0.5 - 0.5 * Math.cos(Math.PI * mg);
      const gOut = env * menv;
      const lim = this._limiter();
      lim.processSample(heap[outLBase + i] * gOut, heap[outRBase + i] * gOut);
      outCh[i] = lim.outL;
      if (rCh) rCh[i] = lim.outR;
    }
    this._declickGain = dg;
    this._muteGain = mg;
    // Any channels beyond stereo mirror the left (keeps the output valid).
    for (let c = 2; c < output.length; c++) output[c].set(outCh);

    return true;
  }
}

registerProcessor('clipper-processor', ClipperProcessor);
