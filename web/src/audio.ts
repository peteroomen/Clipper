// Main-thread audio engine (M3): a selectable source (built-in test tone or
// live guitar input) -> Clipper RAT worklet -> output.
import {
  AMP_PARAM_VOLUME,
  AMP_PARAM_BASS,
  AMP_PARAM_MIDDLE,
  AMP_PARAM_TREBLE,
  AMP_PARAM_BRIGHT,
  AMP_PARAM_CAB,
  AMP_PARAM_CHORUS_SPEED,
  AMP_PARAM_CHORUS_DEPTH,
  AMP_PARAM_CHORUS_MODE,
  AMP_PARAM_REVERB,
  AMP_PARAM_JCM_GAIN,
  AMP_PARAM_JCM_PRESENCE,
  AMP_PARAM_JCM_MASTER,
  AMP_PARAM_ORANGE_FAC,
  AMP_MODEL_INDEX,
  WORKLET_URL,
  trimKnobToGain,
} from './params';
import { PitchDetector } from 'pitchy';
import type { SourceKind, AmpParams, AmpType, PedalInstance, CabChoice } from './rig';
import { CAB_BUILTIN_INDEX } from './rig';
import { resampleMono } from './cab';
import { TUNER_FRAME_SIZE, analyzePitch, type TunerReading } from './tuner';

export type { SourceKind };
export type { TunerReading };

export type Unit = 'pedal' | 'amp';

// Minimal shapes for the non-standard AudioRenderCapacity API (not yet in the DOM
// lib typings). Only the members we use; feature-detected at runtime.
interface RenderCapacityEventLike {
  averageLoad?: number;
  peakLoad?: number;
  underrunRatio?: number;
}
interface RenderCapacityLike {
  start(options?: { updateInterval?: number }): void;
  stop(): void;
  onupdate: ((ev: RenderCapacityEventLike) => void) | null;
}

// A compact pedal descriptor sent to the worklet's `chain` message: enough for it
// to create/reuse the DSP handle and apply the knobs.
export interface ChainPedal {
  id: string;
  type: string;
  engaged: boolean;
  params: { distortion: number; filter: number; level: number };
}

// Serialize the rig chain into the worklet's `chain` message payload.
export function toChainPayload(pedals: PedalInstance[]): ChainPedal[] {
  return pedals.map((p) => ({
    id: p.id,
    type: p.type,
    engaged: p.engaged,
    params: {
      distortion: p.params.distortion,
      filter: p.params.filter,
      level: p.params.level,
    },
  }));
}

export interface StartOptions {
  source: SourceKind;
  deviceId?: string; // preferred audio input (live only)
  inputTrim: number; // 0..1 knob position, rig-level pre-pedal input trim
  pedals: PedalInstance[]; // the ordered pedal chain (may be empty)
  amp: AmpParams; // amp knob positions (0..1)
  ampType: AmpType; // which amp voice (clean120 | jcm800 | twin | ac30 | orange)
  ampEngaged: boolean; // false = amp+cab bypassed
  cabModel: CabChoice; // which cab IR (built-in or 'custom')
  // Mono custom-cab samples + the rate they're stored at, when cabModel is
  // 'custom' and the IR is available (resampled to the engine rate on start).
  // Absent means fall back to the built-in Clean 2x12.
  customIr?: { samples: Float32Array; sampleRate: number } | null;
  oversampling: number; // 1 | 2 | 4 | 8
  // Called whenever the model reports a new latency (e.g. after a chain edit,
  // oversampling, or cab-toggle change). samples are base-rate samples.
  onLatencySamples?: (samples: number) => void;
  // Called ~43x/s with the worklet's post-trim input block peak (linear 0..1+),
  // for the input meter.
  onPeak?: (peak: number) => void;
  // Called with each new tuner reading (or null when the current frame is too
  // quiet / unclear to read) while an engaged tuner is tapping the input. Fires
  // only when a tuner is engaged; silent otherwise.
  onTuner?: (reading: TunerReading | null) => void;
  // ENGINE LOAD METER (perf visibility, field-lag report). Chromium exposes
  // AudioContext.renderCapacity (an AudioRenderCapacity) that fires 'update' events
  // carrying averageLoad / peakLoad / underrunRatio in [0,1] — the fraction of each
  // render quantum's deadline the audio thread actually spent. This is GROUND TRUTH
  // on the user's own machine: a rising load / any underrun is the objective signal
  // behind "lag". Fires at `renderLoadInterval` cadence (default 1 s) when the API
  // is present; never fires (and the UI shows a fallback) when it is not. See §25.
  onRenderLoad?: (load: RenderLoad) => void;
  // Update cadence in seconds for the render-capacity meter (default 1 s). Kept
  // slow on purpose so the meter's own state updates never add to UI churn.
  renderLoadInterval?: number;
}

// A render-capacity sample. Loads are fractions in [0,1] (0.23 == "DSP 23%").
// `supported` is false when the browser lacks AudioContext.renderCapacity, so the
// UI can show a fallback ("DSP n/a") rather than a misleading 0%.
export interface RenderLoad {
  supported: boolean;
  averageLoad: number;
  peakLoad: number;
  underrunRatio: number;
}

export interface Engine {
  context: AudioContext;
  node: AudioWorkletNode;
  // Total model latency, in base-rate samples (engaged pedals' oversampling +
  // cab partition). Mutated in place when the chain / oversampling / cab / power
  // change.
  latencySamples: number;
  // Push the whole ordered chain (add / remove / reorder / swap) — applied
  // click-free in the worklet via a short output fade.
  setChain(pedals: PedalInstance[]): void;
  setPedalParam(pedalId: string, id: number, value: number): void;
  setPedalBypass(pedalId: string, on: boolean): void; // per-instance bypass
  setAmpParam(id: number, value: number): void; // amp param
  // M9.4: swap the amp voice (clean120 | jcm800 | twin) — applied click-free in the
  // worklet via the declick fade, exactly like a cab swap.
  setAmpModel(type: AmpType): void;
  setInputTrim(knob: number): void; // 0..1 knob -> linear gain, applied pre-pedal
  setOversampling(factor: number): void;
  setAmpBypass(on: boolean): void; // amp power off = bypass
  // Cab expansion: swap the BUILT-IN cab IR (click-free in the worklet).
  setCabBuiltin(which: 'clean212' | 'brit412' | 'orange412'): void;
  // Cab expansion: load a user IR (engine-rate mono samples). The buffer is
  // TRANSFERRED to the worklet; the core peak-normalizes it.
  loadCustomIr(samples: Float32Array): void;
  stop(): Promise<void>;
}

// Enumerate audio input devices. Labels are only populated after the user has
// granted microphone permission at least once (browser privacy rule), so call
// this after a successful live start to get meaningful names.
export async function listInputDevices(): Promise<MediaDeviceInfo[]> {
  if (!navigator.mediaDevices?.enumerateDevices) return [];
  const devices = await navigator.mediaDevices.enumerateDevices();
  return devices.filter((d) => d.kind === 'audioinput');
}

// One-shot pitch detection over a time-domain frame (the same McLeod path the
// live tuner uses), returning a chromatic reading or null. Exposed for the test
// harness to feed known-frequency frames; the live engine uses a reused detector.
export function detectPitch(samples: Float32Array, sampleRate: number): TunerReading | null {
  const det = PitchDetector.forFloat32Array(samples.length);
  const [pitch, clarity] = det.findPitch(samples, sampleRate);
  return analyzePitch(pitch, clarity);
}

export async function startEngine(opts: StartOptions): Promise<Engine> {
  const context = new AudioContext();
  await context.resume();
  await context.audioWorklet.addModule(WORKLET_URL);

  // Output is STEREO from M6.3 on: the amp's chorus stage produces a dry/wet
  // (or vibrato) pair. Off/chorus-off collapses to identical channels.
  const node = new AudioWorkletNode(context, 'clipper-processor', {
    numberOfInputs: 1,
    numberOfOutputs: 1,
    outputChannelCount: [2],
  });

  // Wait for the processor to finish instantiating the WASM module, capturing
  // its initial latency.
  let latencySamples = 0;
  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error('worklet did not become ready in time')),
      5000
    );
    node.port.onmessage = (e) => {
      const d = e.data;
      if (d?.type === 'ready') {
        latencySamples = d.latencySamples ?? 0;
        clearTimeout(timer);
        resolve();
      } else if (d?.type === 'error') {
        clearTimeout(timer);
        reject(new Error(d.message));
      }
    };
  });

  const engine: Engine = {
    context,
    node,
    latencySamples,
    setChain(pedals) {
      node.port.postMessage({ type: 'chain', pedals: toChainPayload(pedals) });
    },
    setPedalParam(pedalId, id, value) {
      node.port.postMessage({ type: 'param', unit: 'pedal', pedalId, id, value });
    },
    setPedalBypass(pedalId, on) {
      node.port.postMessage({ type: 'bypass', unit: 'pedal', pedalId, on });
    },
    setAmpParam(id, value) {
      node.port.postMessage({ type: 'param', unit: 'amp', id, value });
    },
    setAmpModel(type) {
      node.port.postMessage({ type: 'ampModel', model: AMP_MODEL_INDEX[type] });
    },
    setInputTrim(knob) {
      node.port.postMessage({ type: 'input', gain: trimKnobToGain(knob) });
    },
    setOversampling(factor) {
      node.port.postMessage({ type: 'oversampling', factor });
    },
    setAmpBypass(on) {
      node.port.postMessage({ type: 'bypass', unit: 'amp', on });
    },
    setCabBuiltin(which) {
      node.port.postMessage({ type: 'cab', builtin: CAB_BUILTIN_INDEX[which] });
    },
    loadCustomIr(samples) {
      // Transfer the buffer (zero-copy) to the worklet; the core peak-normalizes.
      node.port.postMessage({ type: 'cab', custom: samples }, [samples.buffer]);
    },
    stop: async () => {}, // replaced below once the source is wired
  };

  // Tuner pitch detector (McLeod method), created lazily and reused. Sized to the
  // worklet's tap frame so no reallocation happens per frame.
  let pitchDetector: PitchDetector<Float32Array> | null = null;

  // After ready, keep listening for post-oversampling latency updates, the
  // periodic input peak-meter reports, and (M7) tuner frames to run detection on.
  node.port.onmessage = (e) => {
    const d = e.data;
    if (d?.type === 'latency') {
      engine.latencySamples = d.latencySamples ?? engine.latencySamples;
      opts.onLatencySamples?.(engine.latencySamples);
    } else if (d?.type === 'peak') {
      opts.onPeak?.(d.peak ?? 0);
    } else if (d?.type === 'tunerFrame' && opts.onTuner) {
      const samples = d.samples as Float32Array;
      const sr = (d.sampleRate as number) || context.sampleRate;
      if (samples && samples.length === TUNER_FRAME_SIZE) {
        if (!pitchDetector) pitchDetector = PitchDetector.forFloat32Array(TUNER_FRAME_SIZE);
        const [pitch, clarity] = pitchDetector.findPitch(samples, sr);
        opts.onTuner(analyzePitch(pitch, clarity));
      }
    }
  };

  // ENGINE LOAD METER (Deliverable 1). Wire AudioContext.renderCapacity if the
  // browser exposes it (Chromium 116+/Electron). Each 'update' carries the audio
  // thread's load over the last interval; forward it to the UI and log a compact
  // diagnostic. Feature-detected so non-Chromium browsers degrade to a fallback.
  let renderCapacity: RenderCapacityLike | null = null;
  const rcHost = context as unknown as { renderCapacity?: RenderCapacityLike };
  if (opts.onRenderLoad && rcHost.renderCapacity) {
    renderCapacity = rcHost.renderCapacity;
    const onUpdate = (ev: RenderCapacityEventLike) => {
      const load: RenderLoad = {
        supported: true,
        averageLoad: ev.averageLoad ?? 0,
        peakLoad: ev.peakLoad ?? 0,
        underrunRatio: ev.underrunRatio ?? 0,
      };
      opts.onRenderLoad?.(load);
      // Console diagnostics: quiet when healthy, LOUD on any underrun (a dropout —
      // the objective face of "crackle/lag"), so the field can copy-paste it back.
      const pct = (x: number) => `${Math.round(x * 100)}%`;
      if (load.underrunRatio > 0) {
        console.warn(
          `[clipper] DSP avg ${pct(load.averageLoad)} peak ${pct(load.peakLoad)} ` +
            `UNDERRUN ${pct(load.underrunRatio)} — audio thread missed its deadline (dropout)`
        );
      } else {
        console.debug(
          `[clipper] DSP avg ${pct(load.averageLoad)} peak ${pct(load.peakLoad)}`
        );
      }
    };
    renderCapacity.onupdate = onUpdate;
    try {
      renderCapacity.start({ updateInterval: opts.renderLoadInterval ?? 1 });
    } catch (err) {
      console.warn('[clipper] renderCapacity.start failed:', err);
      renderCapacity = null;
    }
  } else if (opts.onRenderLoad) {
    // No API here — tell the UI once so it can render the fallback state.
    opts.onRenderLoad({ supported: false, averageLoad: 0, peakLoad: 0, underrunRatio: 0 });
    console.info('[clipper] AudioContext.renderCapacity unavailable; DSP load meter shows n/a');
  }

  // Apply the initial configuration. Oversampling first (so chain handles are
  // created at the right factor), then the whole chain (which carries per-pedal
  // params + engaged), then input/amp.
  engine.setInputTrim(opts.inputTrim);
  engine.setOversampling(opts.oversampling);
  engine.setChain(opts.pedals);
  engine.setAmpParam(AMP_PARAM_VOLUME, opts.amp.volume);
  engine.setAmpParam(AMP_PARAM_BASS, opts.amp.bass);
  engine.setAmpParam(AMP_PARAM_MIDDLE, opts.amp.middle);
  engine.setAmpParam(AMP_PARAM_TREBLE, opts.amp.treble);
  engine.setAmpParam(AMP_PARAM_BRIGHT, opts.amp.bright);
  engine.setAmpParam(AMP_PARAM_CAB, opts.amp.cab);
  engine.setAmpParam(AMP_PARAM_CHORUS_SPEED, opts.amp.speed);
  engine.setAmpParam(AMP_PARAM_CHORUS_DEPTH, opts.amp.depth);
  engine.setAmpParam(AMP_PARAM_CHORUS_MODE, opts.amp.chorusMode);
  engine.setAmpParam(AMP_PARAM_REVERB, opts.amp.reverb);
  // M9.4 JCM800 knobs — sent every start; the C ABI keeps both amp voices current,
  // so these reach the JCM whether or not it is the active model yet.
  engine.setAmpParam(AMP_PARAM_JCM_GAIN, opts.amp.gain);
  engine.setAmpParam(AMP_PARAM_JCM_PRESENCE, opts.amp.presence);
  engine.setAmpParam(AMP_PARAM_JCM_MASTER, opts.amp.master);
  // M10.3 Orange F.A.C. — its own id, sent every start for the same reason.
  engine.setAmpParam(AMP_PARAM_ORANGE_FAC, opts.amp.fac);
  // Select the amp voice (default clean120 needs no swap, but sending is harmless
  // and keeps the worklet's model + reported latency in sync from sample 0). M10.1:
  // the twin is voice 2. The reverb/speed/depth already sent above reach it (the C
  // ABI routes reverb id 9 to all three voices and speed/depth to the twin tremolo).
  if (opts.ampType === 'jcm800') engine.setAmpModel('jcm800');
  else if (opts.ampType === 'twin') engine.setAmpModel('twin');
  // v1.1: the AC30 "top boost" is voice 3. It reuses the STABLE amp_* exports, so
  // volume/bass/treble/presence(=top cut)/reverb already sent above reach it.
  else if (opts.ampType === 'ac30') engine.setAmpModel('ac30');
  // M10.3: the Orange OR120 is voice 4. It reuses volume/bass/treble/reverb and the
  // presence slot as HF DRIVE, all sent above; F.A.C. is its own id and is sent
  // with the JCM block below.
  else if (opts.ampType === 'orange') engine.setAmpModel('orange');
  engine.setAmpBypass(!opts.ampEngaged);

  // Cab: the worklet's amp_create loads the Clean 2x12 by default. Apply the
  // rig's choice: brit412 -> built-in swap; custom -> load the IR if present,
  // else leave the default clean212 (App has already noted the fallback).
  if (opts.cabModel === 'brit412') {
    engine.setCabBuiltin('brit412');
  } else if (opts.cabModel === 'orange412') {
    engine.setCabBuiltin('orange412');
  } else if (opts.cabModel === 'custom' && opts.customIr && opts.customIr.samples.length) {
    // Resample the stored IR to THIS session's engine rate, then load a copy (the
    // transfer detaches the buffer, so never hand out the persisted samples).
    const atRate = await resampleMono(
      opts.customIr.samples,
      opts.customIr.sampleRate,
      context.sampleRate
    );
    engine.loadCustomIr(atRate.slice());
  }

  // Build the source and connect the graph.
  let stream: MediaStream | null = null;
  let osc: OscillatorNode | null = null;
  let sourceNode: AudioNode;

  if (opts.source === 'live') {
    // Disable ALL browser DSP — echo cancellation / noise suppression / AGC
    // wreck a guitar signal (gating, pumping, comb filtering). Mono.
    const audioConstraints: MediaTrackConstraints = {
      echoCancellation: false,
      noiseSuppression: false,
      autoGainControl: false,
      channelCount: 1,
    };
    if (opts.deviceId) {
      audioConstraints.deviceId = { exact: opts.deviceId };
    }
    stream = await navigator.mediaDevices.getUserMedia({
      audio: audioConstraints,
      video: false,
    });
    sourceNode = context.createMediaStreamSource(stream);
  } else {
    osc = new OscillatorNode(context, { type: 'sine', frequency: 220 });
    osc.start();
    sourceNode = osc;
  }

  sourceNode.connect(node).connect(context.destination);

  engine.stop = async () => {
    if (renderCapacity) {
      renderCapacity.onupdate = null;
      try {
        renderCapacity.stop();
      } catch {
        /* already stopped / context closing */
      }
      renderCapacity = null;
    }
    if (osc) {
      osc.stop();
      osc.disconnect();
    }
    sourceNode.disconnect();
    node.disconnect();
    if (stream) {
      for (const track of stream.getTracks()) track.stop();
    }
    await context.close();
  };

  return engine;
}
