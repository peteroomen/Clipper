import { useEffect, useRef, useState } from 'react';
import {
  startEngine,
  listInputDevices,
  type Engine,
  type SourceKind,
} from './audio';
import {
  PARAM_DISTORTION,
  PARAM_FILTER,
  PARAM_LEVEL,
  OVERSAMPLING_FACTORS,
} from './params';

export default function App() {
  const engineRef = useRef<Engine | null>(null);
  const [running, setRunning] = useState(false);
  const [status, setStatus] = useState('idle');
  const [sampleRate, setSampleRate] = useState<number | null>(null);
  const [latencySamples, setLatencySamples] = useState(0);
  const [baseLatency, setBaseLatency] = useState<number | null>(null);
  const [outputLatency, setOutputLatency] = useState<number | null>(null);

  // Source + device.
  const [source, setSource] = useState<SourceKind>('test');
  const [devices, setDevices] = useState<MediaDeviceInfo[]>([]);
  const [deviceId, setDeviceId] = useState<string>('');

  // Knobs (0..1 internally; displayed 0..100).
  const [distortion, setDistortion] = useState(0.7);
  const [filter, setFilter] = useState(0.4);
  const [level, setLevel] = useState(0.8);
  const [bypass, setBypass] = useState(false);
  const [oversampling, setOversampling] = useState(4);

  // Populate the input-device list. Labels only appear after permission has
  // been granted once, so we refresh again right after a live start.
  async function refreshDevices() {
    try {
      setDevices(await listInputDevices());
    } catch {
      /* enumeration can fail before permission; ignore */
    }
  }
  useEffect(() => {
    void refreshDevices();
  }, []);

  async function handleStart() {
    if (running) return;
    setStatus('starting…');
    try {
      const engine = await startEngine({
        source,
        deviceId: source === 'live' && deviceId ? deviceId : undefined,
        distortion,
        filter,
        level,
        oversampling,
        bypass,
        onLatencySamples: setLatencySamples,
      });
      engineRef.current = engine;
      setSampleRate(engine.context.sampleRate);
      setBaseLatency(engine.context.baseLatency ?? null);
      // outputLatency is not in the TS lib DOM types everywhere yet.
      setOutputLatency(
        (engine.context as unknown as { outputLatency?: number }).outputLatency ?? null
      );
      setLatencySamples(engine.latencySamples);
      setStatus(engine.context.state);
      setRunning(true);
      if (source === 'live') void refreshDevices();
    } catch (err) {
      setStatus('error: ' + (err as Error).message);
    }
  }

  async function handleStop() {
    const engine = engineRef.current;
    if (!engine) return;
    await engine.stop();
    engineRef.current = null;
    setRunning(false);
    setStatus('stopped');
  }

  function handleKnob(
    setter: (v: number) => void,
    id: number,
    value: number
  ) {
    setter(value);
    engineRef.current?.setParam(id, value);
  }

  function handleBypass(on: boolean) {
    setBypass(on);
    engineRef.current?.setBypass(on);
  }

  function handleOversampling(factor: number) {
    setOversampling(factor);
    engineRef.current?.setOversampling(factor);
  }

  const modelLatencyMs =
    sampleRate ? (latencySamples / sampleRate) * 1000 : 0;
  const ioLatencyMs =
    ((baseLatency ?? 0) + (outputLatency ?? 0)) * 1000;
  const totalLatencyMs = modelLatencyMs + ioLatencyMs;

  const pct = (v: number) => Math.round(v * 100);

  return (
    <main style={{ fontFamily: 'system-ui, sans-serif', maxWidth: 520, margin: '2rem auto', padding: '0 1rem' }}>
      <h1>Clipper — M3</h1>
      <p style={{ color: '#666' }}>
        The RAT-style diode-clipper pedal, live in the browser. Pick a source,
        press Start, and dial in the three knobs. Runs the M2 oversampled model
        in an AudioWorklet.
      </p>

      <div style={{ display: 'flex', gap: '0.75rem', margin: '1rem 0' }}>
        <button onClick={handleStart} disabled={running}>Start audio</button>
        <button onClick={handleStop} disabled={!running}>Stop</button>
      </div>

      <label style={{ display: 'block', margin: '1rem 0' }}>
        Source:{' '}
        <select
          aria-label="Source"
          value={source}
          onChange={(e) => setSource(e.target.value as SourceKind)}
          disabled={running}
        >
          <option value="test">Test tone (220 Hz)</option>
          <option value="live">Live input (guitar / interface)</option>
        </select>
      </label>

      {source === 'live' && devices.length > 1 && (
        <label style={{ display: 'block', margin: '1rem 0' }}>
          Input device:{' '}
          <select
            aria-label="Input device"
            value={deviceId}
            onChange={(e) => setDeviceId(e.target.value)}
            disabled={running}
          >
            <option value="">Default</option>
            {devices.map((d) => (
              <option key={d.deviceId} value={d.deviceId}>
                {d.label || `Input ${d.deviceId.slice(0, 6)}`}
              </option>
            ))}
          </select>
        </label>
      )}

      {source === 'live' && (
        <p
          data-testid="feedback-hint"
          style={{ background: '#fff6e5', border: '1px solid #f0c36d', borderRadius: 6, padding: '0.5rem 0.75rem', fontSize: 13, color: '#7a5b00' }}
        >
          ⚠ Live input through speakers can feed back. Use headphones.
        </p>
      )}

      <Slider label="Distortion" value={distortion} display={pct(distortion)}
        onChange={(v) => handleKnob(setDistortion, PARAM_DISTORTION, v)} />
      <Slider label="Filter" value={filter} display={pct(filter)}
        onChange={(v) => handleKnob(setFilter, PARAM_FILTER, v)} />
      <Slider label="Level" value={level} display={pct(level)}
        onChange={(v) => handleKnob(setLevel, PARAM_LEVEL, v)} />

      <label style={{ display: 'block', margin: '1rem 0' }}>
        <input
          type="checkbox"
          checked={bypass}
          onChange={(e) => handleBypass(e.target.checked)}
        />{' '}
        Bypass (pass input through untouched)
      </label>

      <label style={{ display: 'block', margin: '1rem 0' }}>
        Oversampling:{' '}
        <select
          aria-label="Oversampling"
          value={oversampling}
          onChange={(e) => handleOversampling(parseInt(e.target.value, 10))}
        >
          {OVERSAMPLING_FACTORS.map((f) => (
            <option key={f} value={f}>{f}×</option>
          ))}
        </select>
      </label>

      <dl style={{ color: '#444', fontSize: 14 }}>
        <Row label="Context state" value={status} testid="status" />
        <Row label="Sample rate" value={sampleRate ? `${sampleRate} Hz` : '—'} />
        <Row
          label="Latency (model)"
          value={running ? `${modelLatencyMs.toFixed(1)} ms (${latencySamples} smp)` : '—'}
        />
        <Row
          label="Latency (I/O)"
          value={running ? `${ioLatencyMs.toFixed(1)} ms` : '—'}
        />
        <Row
          label="Latency (total est.)"
          value={running ? `${totalLatencyMs.toFixed(1)} ms` : '—'}
        />
      </dl>
    </main>
  );
}

function Slider(props: {
  label: string;
  value: number;
  display: number;
  onChange: (v: number) => void;
}) {
  return (
    <label style={{ display: 'block', margin: '1rem 0' }}>
      {props.label}: {props.display}
      <input
        type="range"
        aria-label={props.label}
        min={0}
        max={100}
        step={1}
        value={Math.round(props.value * 100)}
        onChange={(e) => props.onChange(parseInt(e.target.value, 10) / 100)}
        style={{ width: '100%' }}
      />
    </label>
  );
}

function Row(props: { label: string; value: string; testid?: string }) {
  return (
    <div>
      <dt style={{ display: 'inline', fontWeight: 600 }}>{props.label}: </dt>
      <dd style={{ display: 'inline' }} data-testid={props.testid}>{props.value}</dd>
    </div>
  );
}
