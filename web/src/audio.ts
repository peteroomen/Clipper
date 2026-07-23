// Main-thread audio engine (M3): a selectable source (built-in test tone or
// live guitar input) -> Clipper RAT worklet -> output.
import {
  PARAM_DISTORTION,
  PARAM_FILTER,
  PARAM_LEVEL,
  WORKLET_URL,
} from './params';

export type SourceKind = 'test' | 'live';

export interface StartOptions {
  source: SourceKind;
  deviceId?: string; // preferred audio input (live only)
  distortion: number; // 0..1 knob
  filter: number; // 0..1 knob
  level: number; // 0..1 knob
  oversampling: number; // 1 | 2 | 4 | 8
  bypass: boolean;
  // Called whenever the model reports a new latency (e.g. after an oversampling
  // change). samples are base-rate samples.
  onLatencySamples?: (samples: number) => void;
}

export interface Engine {
  context: AudioContext;
  node: AudioWorkletNode;
  // Model's own oversampling-filter latency, in base-rate samples (mutated in
  // place when oversampling changes).
  latencySamples: number;
  setParam(id: number, value: number): void;
  setOversampling(factor: number): void;
  setBypass(on: boolean): void;
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
      node.port.postMessage({ type: 'param', id, value });
    },
    setOversampling(factor) {
      node.port.postMessage({ type: 'oversampling', factor });
    },
    setBypass(on) {
      node.port.postMessage({ type: 'bypass', on });
    },
    stop: async () => {}, // replaced below once the source is wired
  };

  // After ready, keep listening for post-oversampling latency updates.
  node.port.onmessage = (e) => {
    if (e.data?.type === 'latency') {
      engine.latencySamples = e.data.latencySamples ?? engine.latencySamples;
      opts.onLatencySamples?.(engine.latencySamples);
    }
  };

  // Apply the initial configuration.
  engine.setOversampling(opts.oversampling);
  engine.setParam(PARAM_DISTORTION, opts.distortion);
  engine.setParam(PARAM_FILTER, opts.filter);
  engine.setParam(PARAM_LEVEL, opts.level);
  engine.setBypass(opts.bypass);

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
