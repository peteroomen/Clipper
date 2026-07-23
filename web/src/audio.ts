// Main-thread audio engine (M3): a selectable source (built-in test tone or
// live guitar input) -> Clipper RAT worklet -> output.
import {
  PARAM_DISTORTION,
  PARAM_FILTER,
  PARAM_LEVEL,
  AMP_PARAM_VOLUME,
  AMP_PARAM_BASS,
  AMP_PARAM_MIDDLE,
  AMP_PARAM_TREBLE,
  AMP_PARAM_BRIGHT,
  AMP_PARAM_CAB,
  WORKLET_URL,
  trimKnobToGain,
} from './params';
import type { SourceKind, AmpParams } from './rig';

export type { SourceKind };

export type Unit = 'pedal' | 'amp';

export interface StartOptions {
  source: SourceKind;
  deviceId?: string; // preferred audio input (live only)
  inputTrim: number; // 0..1 knob position, rig-level pre-pedal input trim
  distortion: number; // 0..1 knob (pedal)
  filter: number; // 0..1 knob (pedal)
  level: number; // 0..1 knob (pedal)
  amp: AmpParams; // amp knob positions (0..1)
  ampEngaged: boolean; // false = amp+cab bypassed
  oversampling: number; // 1 | 2 | 4 | 8
  bypass: boolean; // pedal bypass
  // Called whenever the model reports a new latency (e.g. after an oversampling
  // or cab-toggle change). samples are base-rate samples.
  onLatencySamples?: (samples: number) => void;
  // Called ~43x/s with the worklet's post-trim input block peak (linear 0..1+),
  // for the input meter.
  onPeak?: (peak: number) => void;
}

export interface Engine {
  context: AudioContext;
  node: AudioWorkletNode;
  // Total model latency, in base-rate samples (pedal oversampling + cab
  // partition). Mutated in place when oversampling / cab / amp-power change.
  latencySamples: number;
  setParam(id: number, value: number): void; // pedal param
  setAmpParam(id: number, value: number): void; // amp param
  setInputTrim(knob: number): void; // 0..1 knob -> linear gain, applied pre-pedal
  setOversampling(factor: number): void;
  setBypass(on: boolean): void; // pedal bypass
  setAmpBypass(on: boolean): void; // amp power off = bypass
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

export async function startEngine(opts: StartOptions): Promise<Engine> {
  const context = new AudioContext();
  await context.resume();
  await context.audioWorklet.addModule(WORKLET_URL);

  const node = new AudioWorkletNode(context, 'clipper-processor', {
    numberOfInputs: 1,
    numberOfOutputs: 1,
    outputChannelCount: [1],
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
    setParam(id, value) {
      node.port.postMessage({ type: 'param', unit: 'pedal', id, value });
    },
    setAmpParam(id, value) {
      node.port.postMessage({ type: 'param', unit: 'amp', id, value });
    },
    setInputTrim(knob) {
      node.port.postMessage({ type: 'input', gain: trimKnobToGain(knob) });
    },
    setOversampling(factor) {
      node.port.postMessage({ type: 'oversampling', factor });
    },
    setBypass(on) {
      node.port.postMessage({ type: 'bypass', unit: 'pedal', on });
    },
    setAmpBypass(on) {
      node.port.postMessage({ type: 'bypass', unit: 'amp', on });
    },
    stop: async () => {}, // replaced below once the source is wired
  };

  // After ready, keep listening for post-oversampling latency updates and the
  // periodic input peak-meter reports.
  node.port.onmessage = (e) => {
    if (e.data?.type === 'latency') {
      engine.latencySamples = e.data.latencySamples ?? engine.latencySamples;
      opts.onLatencySamples?.(engine.latencySamples);
    } else if (e.data?.type === 'peak') {
      opts.onPeak?.(e.data.peak ?? 0);
    }
  };

  // Apply the initial configuration.
  engine.setInputTrim(opts.inputTrim);
  engine.setOversampling(opts.oversampling);
  engine.setParam(PARAM_DISTORTION, opts.distortion);
  engine.setParam(PARAM_FILTER, opts.filter);
  engine.setParam(PARAM_LEVEL, opts.level);
  engine.setBypass(opts.bypass);
  engine.setAmpParam(AMP_PARAM_VOLUME, opts.amp.volume);
  engine.setAmpParam(AMP_PARAM_BASS, opts.amp.bass);
  engine.setAmpParam(AMP_PARAM_MIDDLE, opts.amp.middle);
  engine.setAmpParam(AMP_PARAM_TREBLE, opts.amp.treble);
  engine.setAmpParam(AMP_PARAM_BRIGHT, opts.amp.bright);
  engine.setAmpParam(AMP_PARAM_CAB, opts.amp.cab);
  engine.setAmpBypass(!opts.ampEngaged);

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
