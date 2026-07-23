import { useEffect, useMemo, useRef, useState } from 'react';
import { startEngine, listInputDevices, type Engine } from './audio';
import { PARAM_ID, AMP_PARAM_ID, OVERSAMPLING_FACTORS } from './params';
import {
  loadRig,
  saveRig,
  serializeRig,
  deserializeRig,
  type RigState,
  type ParamName,
  type AmpParamName,
  type SourceKind,
} from './rig';
import { loadGuitar, saveGuitar, type GuitarProfile } from './guitar';
import type { RigController } from './assistant/tools';
import { Pedal } from './components/Pedal';
import { Amp } from './components/Amp';
import { Chat } from './components/Chat';

import './styles/tokens.css';
import './styles/base.css';
import './styles/pedal.css';
import './styles/amp.css';
import './styles/app.css';
import './styles/chat.css';

type ThemeChoice = 'system' | 'light' | 'dark';

function applyTheme(choice: ThemeChoice) {
  const root = document.documentElement;
  if (choice === 'system') root.removeAttribute('data-theme');
  else root.setAttribute('data-theme', choice);
}

export default function App() {
  const engineRef = useRef<Engine | null>(null);

  // The single source of truth for the whole rig. Knobs, footswitch, and the
  // selects all read/write this; changes propagate to the worklet and persist.
  const [rig, setRig] = useState<RigState>(() => loadRig());
  const rigRef = useRef(rig);
  rigRef.current = rig;

  const [running, setRunning] = useState(false);
  const [status, setStatus] = useState('idle');
  const [sampleRate, setSampleRate] = useState<number | null>(null);
  const [latencySamples, setLatencySamples] = useState(0);
  const [baseLatency, setBaseLatency] = useState<number | null>(null);
  const [outputLatency, setOutputLatency] = useState<number | null>(null);

  const [devices, setDevices] = useState<MediaDeviceInfo[]>([]);
  const [deviceId, setDeviceId] = useState<string>('');
  const [theme, setTheme] = useState<ThemeChoice>('system');

  // The guitar profile (M6): injected into the assistant's context. Separate
  // localStorage key from the rig; editable anytime from the chat panel.
  const [guitar, setGuitar] = useState<GuitarProfile>(() => loadGuitar());

  // Persist the rig on every change.
  useEffect(() => {
    saveRig(rig);
  }, [rig]);

  useEffect(() => {
    saveGuitar(guitar);
  }, [guitar]);

  useEffect(() => {
    applyTheme(theme);
  }, [theme]);

  // A small, stable test/AI hook: read live rig state + the last worklet param
  // message, and exercise the pure (de)serializers. This is also the seam the
  // M6 assistant reads rig state through.
  useEffect(() => {
    const w = window as unknown as { __CLIPPER_TEST__?: Record<string, unknown> };
    w.__CLIPPER_TEST__ = {
      ...(w.__CLIPPER_TEST__ ?? {}),
      getRig: () => rigRef.current,
      serializeRig,
      deserializeRig,
    };
  }, []);

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

  // ---- rig mutations (single source of truth -> worklet) ----

  function setParam(name: ParamName, value: number) {
    setRig((r) => ({ ...r, pedal: { ...r.pedal, params: { ...r.pedal.params, [name]: value } } }));
    const id = PARAM_ID[name];
    engineRef.current?.setParam(id, value);
    const w = window as unknown as { __CLIPPER_TEST__?: Record<string, unknown> };
    if (w.__CLIPPER_TEST__) w.__CLIPPER_TEST__.lastParam = { id, value };
  }

  // Set the pedal engaged state to an explicit value (used by the footswitch and
  // the assistant's set_engaged tool). engaged=false -> bypassed.
  function setPedalEngaged(engaged: boolean) {
    engineRef.current?.setBypass(!engaged); // bypass = not engaged
    setRig((r) => ({ ...r, pedal: { ...r.pedal, engaged } }));
    const w = window as unknown as { __CLIPPER_TEST__?: Record<string, unknown> };
    if (w.__CLIPPER_TEST__) w.__CLIPPER_TEST__.lastBypass = !engaged;
  }

  function toggleEngaged() {
    setPedalEngaged(!rigRef.current.pedal.engaged);
  }

  // ---- amp mutations ----

  function setAmpParam(name: AmpParamName, value: number) {
    setRig((r) => ({ ...r, amp: { ...r.amp, params: { ...r.amp.params, [name]: value } } }));
    engineRef.current?.setAmpParam(AMP_PARAM_ID[name], value);
    const w = window as unknown as { __CLIPPER_TEST__?: Record<string, unknown> };
    if (w.__CLIPPER_TEST__) w.__CLIPPER_TEST__.lastAmpParam = { id: AMP_PARAM_ID[name], value };
  }

  // Flip a 0/1 amp toggle (bright / cab).
  function toggleAmp(name: 'bright' | 'cab') {
    const next = rigRef.current.amp.params[name] >= 0.5 ? 0 : 1;
    setAmpParam(name, next);
  }

  // Set the amp power (engaged) state explicitly (used by the Power rocker and
  // the assistant's set_engaged tool). engaged=false -> amp+cab bypassed.
  function setAmpEngaged(engaged: boolean) {
    engineRef.current?.setAmpBypass(!engaged); // amp bypass = power off
    setRig((r) => ({ ...r, amp: { ...r.amp, engaged } }));
    const w = window as unknown as { __CLIPPER_TEST__?: Record<string, unknown> };
    if (w.__CLIPPER_TEST__) w.__CLIPPER_TEST__.lastAmpBypass = !engaged;
  }

  function toggleAmpPower() {
    setAmpEngaged(!rigRef.current.amp.engaged);
  }

  // The assistant's rig-control seam (M6): the tool executor mutates the rig ONLY
  // through here, reusing the same setters the knobs/switches use so the AI's
  // changes visibly move knobs, reach the worklet, and persist. Built once —
  // every method routes through stable setState/ref-based setters.
  const rigController = useMemo<RigController>(
    () => ({
      getRig: () => rigRef.current,
      setParam: (unit, param, value) => {
        const v = Math.min(1, Math.max(0, value));
        if (unit === 'pedal') setParam(param as ParamName, v);
        else setAmpParam(param as AmpParamName, v);
        return v;
      },
      setEngaged: (unit, engaged) => {
        if (unit === 'pedal') setPedalEngaged(engaged);
        else setAmpEngaged(engaged);
      },
      setSwitch: (name, on) => setAmpParam(name as AmpParamName, on ? 1 : 0),
    }),
    // The referenced setters are behaviorally stable (functional setState + refs).
    // eslint-disable-next-line react-hooks/exhaustive-deps
    []
  );

  function setOversampling(factor: number) {
    setRig((r) => ({ ...r, oversampling: factor }));
    engineRef.current?.setOversampling(factor);
  }

  function setSource(source: SourceKind) {
    setRig((r) => ({ ...r, source }));
  }

  // ---- transport ----

  async function handleStart() {
    if (running) return;
    setStatus('starting…');
    const r = rigRef.current;
    try {
      const engine = await startEngine({
        source: r.source,
        deviceId: r.source === 'live' && deviceId ? deviceId : undefined,
        distortion: r.pedal.params.distortion,
        filter: r.pedal.params.filter,
        level: r.pedal.params.level,
        amp: r.amp.params,
        ampEngaged: r.amp.engaged,
        oversampling: r.oversampling,
        bypass: !r.pedal.engaged,
        onLatencySamples: setLatencySamples,
      });
      engineRef.current = engine;
      setSampleRate(engine.context.sampleRate);
      setBaseLatency(engine.context.baseLatency ?? null);
      setOutputLatency(
        (engine.context as unknown as { outputLatency?: number }).outputLatency ?? null
      );
      setLatencySamples(engine.latencySamples);
      setStatus(engine.context.state);
      setRunning(true);
      if (r.source === 'live') void refreshDevices();
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

  const modelLatencyMs = sampleRate ? (latencySamples / sampleRate) * 1000 : 0;
  const ioLatencyMs = ((baseLatency ?? 0) + (outputLatency ?? 0)) * 1000;
  const totalLatencyMs = modelLatencyMs + ioLatencyMs;

  return (
    <div className="app">
      <div className="wrap">
        <header className="hero">
          <div className="topbar">
            <div className="eyebrow-row">
              <span className="eyebrow">Clipper · RAT pedal → Clean 120 amp + cab</span>
            </div>
            <div className="theme-seg" role="group" aria-label="Theme">
              {(['system', 'light', 'dark'] as ThemeChoice[]).map((t) => (
                <button
                  key={t}
                  type="button"
                  aria-pressed={theme === t}
                  onClick={() => setTheme(t)}
                >
                  {t}
                </button>
              ))}
            </div>
          </div>

          <div className="hero-head">
            <h1>Clipper</h1>
            <p>
              A RAT-style diode-clipper pedal into a JC-120-inspired clean amp and 2×12 cab —
              modeled and played live in the browser. Drag the knobs, stomp the switch, power the
              amp; the whole rig is one serializable state, restored on reload.
            </p>
            <div className="hint">
              <span className="dot" />
              pedal → amp → cab · stomp / power to bypass · flip the theme
            </div>
          </div>
        </header>

        <div className="workspace">
          <div className="stage">
          <div className="rig">
            <Pedal pedal={rig.pedal} onParam={setParam} onToggleEngaged={toggleEngaged} />
            <Amp
              amp={rig.amp}
              onParam={setAmpParam}
              onToggle={toggleAmp}
              onTogglePower={toggleAmpPower}
            />
          </div>

          <div className="rig desk-row">
            <section className="desk raised" aria-label="Control desk">
              <h2>Signal &amp; transport</h2>
              <p className="sub">Source, oversampling, and the audio engine.</p>

              <div className="transport">
                <button
                  type="button"
                  className="btn primary"
                  onClick={handleStart}
                  disabled={running}
                >
                  Start audio
                </button>
                <button type="button" className="btn" onClick={handleStop} disabled={!running}>
                  Stop
                </button>
              </div>

              <div className="field">
                <label htmlFor="source">Source</label>
                <select
                  id="source"
                  className="select"
                  aria-label="Source"
                  value={rig.source}
                  onChange={(e) => setSource(e.target.value as SourceKind)}
                  disabled={running}
                >
                  <option value="test">Test tone (220 Hz)</option>
                  <option value="live">Live input (guitar / interface)</option>
                </select>
              </div>

              {rig.source === 'live' && devices.length > 1 && (
                <div className="field">
                  <label htmlFor="device">Input device</label>
                  <select
                    id="device"
                    className="select"
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
                </div>
              )}

              {rig.source === 'live' && (
                <div className="notice" data-testid="feedback-hint">
                  <span className="dot" />
                  Live input through speakers can feed back. Use headphones.
                </div>
              )}

              <div className="field">
                <label htmlFor="oversampling">Oversampling</label>
                <select
                  id="oversampling"
                  className="select"
                  aria-label="Oversampling"
                  value={rig.oversampling}
                  onChange={(e) => setOversampling(parseInt(e.target.value, 10))}
                >
                  {OVERSAMPLING_FACTORS.map((f) => (
                    <option key={f} value={f}>
                      {f}×
                    </option>
                  ))}
                </select>
              </div>

              <div className="status">
                <dl>
                  <dt>State</dt>
                  <dd data-testid="status">{status}</dd>
                  <dt>Sample rate</dt>
                  <dd>{sampleRate ? `${sampleRate} Hz` : '—'}</dd>
                  <dt>Latency · model+cab</dt>
                  <dd>
                    {running ? `${modelLatencyMs.toFixed(1)} ms (${latencySamples} smp)` : '—'}
                  </dd>
                  <dt>Latency · I/O</dt>
                  <dd>{running ? `${ioLatencyMs.toFixed(1)} ms` : '—'}</dd>
                  <dt>Latency · total</dt>
                  <dd>{running ? `${totalLatencyMs.toFixed(1)} ms` : '—'}</dd>
                </dl>
              </div>
            </section>
          </div>
          </div>

          <Chat controller={rigController} guitar={guitar} onGuitarChange={setGuitar} />
        </div>
      </div>
    </div>
  );
}
