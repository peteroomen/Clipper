import { useRef, useState } from 'react';
import { startEngine, type Engine } from './audio';

export default function App() {
  const engineRef = useRef<Engine | null>(null);
  const [running, setRunning] = useState(false);
  const [gain, setGain] = useState(1.0);
  const [status, setStatus] = useState('idle');
  const [sampleRate, setSampleRate] = useState<number | null>(null);

  async function handleStart() {
    if (running) return;
    setStatus('starting…');
    try {
      const engine = await startEngine(gain);
      engineRef.current = engine;
      setSampleRate(engine.context.sampleRate);
      setStatus(engine.context.state);
      setRunning(true);
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

  function handleGain(value: number) {
    setGain(value);
    engineRef.current?.setGain(value);
  }

  return (
    <main style={{ fontFamily: 'system-ui, sans-serif', maxWidth: 480, margin: '2rem auto', padding: '0 1rem' }}>
      <h1>Clipper — M0</h1>
      <p style={{ color: '#666' }}>
        Walking skeleton: a 220&nbsp;Hz sine passes through a C++ gain function
        compiled to WASM, running in an AudioWorklet.
      </p>

      <div style={{ display: 'flex', gap: '0.75rem', margin: '1rem 0' }}>
        <button onClick={handleStart} disabled={running}>
          Start audio
        </button>
        <button onClick={handleStop} disabled={!running}>
          Stop
        </button>
      </div>

      <label style={{ display: 'block', margin: '1rem 0' }}>
        Gain: {gain.toFixed(2)}
        <input
          type="range"
          min={0}
          max={2}
          step={0.01}
          value={gain}
          onChange={(e) => handleGain(parseFloat(e.target.value))}
          style={{ width: '100%' }}
        />
      </label>

      <dl style={{ color: '#444', fontSize: 14 }}>
        <div>
          <dt style={{ display: 'inline', fontWeight: 600 }}>Context state: </dt>
          <dd style={{ display: 'inline' }} data-testid="status">{status}</dd>
        </div>
        <div>
          <dt style={{ display: 'inline', fontWeight: 600 }}>Sample rate: </dt>
          <dd style={{ display: 'inline' }}>{sampleRate ? `${sampleRate} Hz` : '—'}</dd>
        </div>
      </dl>
    </main>
  );
}
