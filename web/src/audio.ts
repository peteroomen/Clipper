// Main-thread audio engine for M0: sine oscillator -> Clipper worklet -> output.
import { PARAM_GAIN, WORKLET_URL } from './params';

export interface Engine {
  context: AudioContext;
  node: AudioWorkletNode;
  setGain(value: number): void;
  stop(): Promise<void>;
}

// Build the graph and wait until the worklet's WASM instance reports ready.
export async function startEngine(initialGain: number): Promise<Engine> {
  const context = new AudioContext();
  await context.resume();
  await context.audioWorklet.addModule(WORKLET_URL);

  const node = new AudioWorkletNode(context, 'clipper-processor', {
    numberOfInputs: 1,
    numberOfOutputs: 1,
    outputChannelCount: [1],
  });

  // Wait for the processor to finish instantiating the WASM module.
  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error('worklet did not become ready in time')),
      5000
    );
    node.port.onmessage = (e) => {
      if (e.data?.type === 'ready') {
        clearTimeout(timer);
        resolve();
      } else if (e.data?.type === 'error') {
        clearTimeout(timer);
        reject(new Error(e.data.message));
      }
    };
  });

  const osc = new OscillatorNode(context, { type: 'sine', frequency: 220 });
  osc.connect(node).connect(context.destination);
  osc.start();

  const setGain = (value: number) => {
    node.port.postMessage({ type: 'param', id: PARAM_GAIN, value });
  };
  setGain(initialGain);

  const stop = async () => {
    osc.stop();
    osc.disconnect();
    node.disconnect();
    await context.close();
  };

  return { context, node, setGain, stop };
}
