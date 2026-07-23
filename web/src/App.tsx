import { useEffect, useMemo, useRef, useState } from 'react';
import { startEngine, listInputDevices, detectPitch, type Engine } from './audio';
import { TUNER_FRAME_SIZE, type TunerReading } from './tuner';
import { PARAM_ID, AMP_PARAM_ID, OVERSAMPLING_FACTORS } from './params';
import {
  loadRig,
  saveRig,
  serializeRig,
  deserializeRig,
  makePedal,
  type RigState,
  type ParamName,
  type AmpParamName,
  type SourceKind,
  type PedalType,
  type CabChoice,
} from './rig';
import {
  processIrFile,
  saveCustomIr,
  loadCustomIr,
  customIrAtRate,
  type StoredCustomIr,
} from './cab';
import { loadGuitar, saveGuitar, type GuitarProfile } from './guitar';
import type { RigController } from './assistant/tools';
import { Board } from './components/Board';
import { InputStage } from './components/InputStage';
import { Chat } from './components/Chat';

import './styles/tokens.css';
import './styles/base.css';
import './styles/pedal.css';
import './styles/amp.css';
import './styles/board.css';
import './styles/tuner.css';
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

  // Post-trim input peak (linear, from the worklet meter). null until running.
  const [inputPeak, setInputPeak] = useState<number | null>(null);
  const inputPeakRef = useRef<number | null>(null);
  inputPeakRef.current = inputPeak;

  // Latest tuner reading (M7). The ref holds the full-rate value (for the needle
  // + the assistant snapshot); the state is throttled so the app doesn't re-render
  // ~90x/s — the needle smooths from the ref via its own rAF loop.
  const [tunerReading, setTunerReading] = useState<TunerReading | null>(null);
  const tunerReadingRef = useRef<TunerReading | null>(null);
  const tunerThrottleRef = useRef(0);
  function handleTuner(r: TunerReading | null) {
    tunerReadingRef.current = r;
    const now = performance.now();
    if (now - tunerThrottleRef.current >= 33) {
      tunerThrottleRef.current = now;
      setTunerReading(r);
    }
  }

  // The guitar profile (M6): injected into the assistant's context. Separate
  // localStorage key from the rig; editable anytime from the chat panel.
  const [guitar, setGuitar] = useState<GuitarProfile>(() => loadGuitar());

  // Cab expansion: the user's custom IR (samples live in their own localStorage
  // key, never in the rig JSON). `cabNote` is a short transient UI message for
  // upload results and the missing-custom-IR fallback.
  const [customIr, setCustomIr] = useState<StoredCustomIr | null>(() => loadCustomIr());
  const customIrRef = useRef(customIr);
  customIrRef.current = customIr;
  const [cabNote, setCabNote] = useState<string | null>(null);

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
      // M7 tuner detection seam: run the McLeod pitch path over a known frame.
      tuner: { frameSize: TUNER_FRAME_SIZE, detect: detectPitch },
      getTunerReading: () => tunerReadingRef.current,
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

  // Resolve a pedal index (0-based, used by the assistant + Board) to its stable
  // id, clamped to the current chain.
  function pedalIdAt(index: number): string | null {
    const list = rigRef.current.pedals;
    if (list.length === 0) return null;
    const i = Math.min(list.length - 1, Math.max(0, index | 0));
    return list[i].id;
  }

  // Set a knob on a specific pedal instance (by id). Lightweight per-pedal
  // message — does NOT re-push the chain (so no declick fade on a knob move).
  function setPedalParamById(pedalId: string, name: ParamName, value: number) {
    setRig((r) => ({
      ...r,
      pedals: r.pedals.map((p) =>
        p.id === pedalId ? { ...p, params: { ...p.params, [name]: value } } : p
      ),
    }));
    const id = PARAM_ID[name];
    engineRef.current?.setPedalParam(pedalId, id, value);
    const w = window as unknown as { __CLIPPER_TEST__?: Record<string, unknown> };
    if (w.__CLIPPER_TEST__) w.__CLIPPER_TEST__.lastParam = { id, value, pedalId };
  }

  // Rig-level input trim (0..1 knob). Applied in the worklet before the pedals.
  function setInputTrim(value: number) {
    setRig((r) => ({ ...r, input: { ...r.input, trim: value } }));
    engineRef.current?.setInputTrim(value);
    const w = window as unknown as { __CLIPPER_TEST__?: Record<string, unknown> };
    if (w.__CLIPPER_TEST__) w.__CLIPPER_TEST__.lastInputTrim = value;
  }

  // Set a pedal instance's engaged state (footswitch / assistant). Per-instance
  // bypass message; engaged=false -> bypassed.
  function setPedalEngagedById(pedalId: string, engaged: boolean) {
    engineRef.current?.setPedalBypass(pedalId, !engaged); // bypass = not engaged
    setRig((r) => ({
      ...r,
      pedals: r.pedals.map((p) => (p.id === pedalId ? { ...p, engaged } : p)),
    }));
    const w = window as unknown as { __CLIPPER_TEST__?: Record<string, unknown> };
    if (w.__CLIPPER_TEST__) w.__CLIPPER_TEST__.lastBypass = !engaged;
  }

  function toggleEngagedById(pedalId: string) {
    const p = rigRef.current.pedals.find((x) => x.id === pedalId);
    if (p) setPedalEngagedById(pedalId, !p.engaged);
  }

  // ---- chain edits (add / remove / reorder / swap) -> whole-chain push ----
  // Every chain edit re-pushes the full ordered chain to the worklet, which
  // applies it click-free (short output fade). Persisted like any rig change.

  function pushChain(next: RigState) {
    setRig(next);
    engineRef.current?.setChain(next.pedals);
    const w = window as unknown as { __CLIPPER_TEST__?: Record<string, unknown> };
    if (w.__CLIPPER_TEST__) w.__CLIPPER_TEST__.lastChain = next.pedals.map((p) => p.id);
  }

  // Add a pedal of `type` at `position` (default: end of chain). Returns the new
  // instance's index.
  function addPedal(type: PedalType = 'rat', position?: number): number {
    const r = rigRef.current;
    const pedal = makePedal(type);
    const pedals = [...r.pedals];
    const at = position == null ? pedals.length : Math.min(pedals.length, Math.max(0, position));
    pedals.splice(at, 0, pedal);
    pushChain({ ...r, pedals });
    return at;
  }

  function removePedal(pedalId: string) {
    const r = rigRef.current;
    pushChain({ ...r, pedals: r.pedals.filter((p) => p.id !== pedalId) });
  }

  // Swap a pedal in place (remove + add the new type at the same slot). Keeps the
  // chain position; today only 'rat' exists so this is the plumbing for M7/M8.
  function swapPedal(pedalId: string, type: PedalType) {
    const r = rigRef.current;
    const idx = r.pedals.findIndex((p) => p.id === pedalId);
    if (idx < 0) return;
    const pedals = [...r.pedals];
    pedals[idx] = makePedal(type);
    pushChain({ ...r, pedals });
  }

  // Move the pedal at `from` to index `to` (reorder). Both are chain indices.
  function movePedal(from: number, to: number) {
    const r = rigRef.current;
    const n = r.pedals.length;
    if (from < 0 || from >= n) return;
    const clampedTo = Math.min(n - 1, Math.max(0, to));
    if (clampedTo === from) return;
    const pedals = [...r.pedals];
    const [moved] = pedals.splice(from, 1);
    pedals.splice(clampedTo, 0, moved);
    pushChain({ ...r, pedals });
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

  // Set the 3-way chorus mode (0 off / 1 chorus / 2 vibrato). M6.3.
  function setChorusMode(mode: number) {
    setAmpParam('chorusMode', mode);
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

  // ---- cab selection + custom IR upload (cab expansion) ----

  // Missing-custom-IR fallback: a restored rig may say cabModel:'custom' while the
  // IR data (separate key) is absent (e.g. loaded in a different browser, or
  // cleared). Fall back to the Clean 2x12 with a note, once, on mount.
  useEffect(() => {
    if (rigRef.current.amp.cabModel === 'custom' && !customIrRef.current) {
      setRig((r) => ({ ...r, amp: { ...r.amp, cabModel: 'clean212', customCabLabel: undefined } }));
      setCabNote('Custom IR not found — using the Clean 2×12.');
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Push a custom IR into the running engine, resampled to the engine rate.
  async function loadCustomIntoEngine(ir: StoredCustomIr) {
    const engine = engineRef.current;
    if (!engine) return;
    const atRate = await customIrAtRate(ir, engine.context.sampleRate);
    engine.loadCustomIr(atRate.slice()); // slice: the transfer detaches the buffer
  }

  // Select a cab (built-in or the already-loaded custom). Updates the rig and, if
  // running, the engine (click-free in the worklet).
  function setCabModel(cab: CabChoice) {
    if (cab === 'custom' && !customIrRef.current) {
      setCabNote('No custom IR loaded yet — use Upload IR…');
      return;
    }
    setCabNote(null);
    setRig((r) => ({ ...r, amp: { ...r.amp, cabModel: cab } }));
    if (cab === 'brit412' || cab === 'clean212') {
      engineRef.current?.setCabBuiltin(cab);
    } else if (cab === 'custom' && customIrRef.current) {
      void loadCustomIntoEngine(customIrRef.current);
    }
  }

  // Upload + process a user IR (.wav): decode/mono/resample/cap on the main
  // thread, persist it (separate key), select it, and load it into the engine.
  async function uploadIr(file: File) {
    setCabNote('Loading IR…');
    try {
      const engineRate = engineRef.current?.context.sampleRate ?? 48000;
      const processed = await processIrFile(file, engineRate);
      const stored: StoredCustomIr = {
        label: processed.label,
        sampleRate: processed.sampleRate,
        originalLength: processed.originalLength,
        samples: processed.samples,
      };
      saveCustomIr(stored);
      setCustomIr(stored);
      customIrRef.current = stored;
      setRig((r) => ({
        ...r,
        amp: { ...r.amp, cabModel: 'custom', customCabLabel: processed.label },
      }));
      await loadCustomIntoEngine(stored);
      const lenNote = processed.truncated
        ? ` (${processed.originalLength} samples → capped to ${processed.samples.length})`
        : ` (${processed.samples.length} samples)`;
      setCabNote(`Loaded “${processed.label}”${lenNote}.`);
    } catch (err) {
      setCabNote('Could not load that IR: ' + (err as Error).message);
    }
  }

  // The assistant's rig-control seam (M6): the tool executor mutates the rig ONLY
  // through here, reusing the same setters the knobs/switches use so the AI's
  // changes visibly move knobs, reach the worklet, and persist. Built once —
  // every method routes through stable setState/ref-based setters.
  const rigController = useMemo<RigController>(
    () => ({
      getRig: () => rigRef.current,
      // Current post-trim input peak in dBFS (null when not running), so the
      // assistant can help calibrate the trim.
      getPeakDbFs: () => {
        const p = inputPeakRef.current;
        return p != null && p > 0 ? 20 * Math.log10(p) : null;
      },
      // Latest tuner reading snapshot (M7), so the coach can see pitch/cents.
      getTunerReading: () => tunerReadingRef.current,
      setParam: (unit, param, value, pedalIndex) => {
        const v = Math.min(1, Math.max(0, value));
        if (unit === 'input') setInputTrim(v);
        else if (unit === 'pedal') {
          const id = pedalIdAt(pedalIndex ?? 0);
          if (id) setPedalParamById(id, param as ParamName, v);
        } else setAmpParam(param as AmpParamName, v);
        return v;
      },
      setEngaged: (unit, engaged, pedalIndex) => {
        if (unit === 'pedal') {
          const id = pedalIdAt(pedalIndex ?? 0);
          if (id) setPedalEngagedById(id, engaged);
        } else setAmpEngaged(engaged);
      },
      setSwitch: (name, on) => {
        // bright/cab are 0/1 toggles; chorus/vibrato select the 3-way chorus mode
        // (turning one on selects it; turning it off returns to mode 0 = off).
        if (name === 'chorus') setChorusMode(on ? 1 : 0);
        else if (name === 'vibrato') setChorusMode(on ? 2 : 0);
        else setAmpParam(name as AmpParamName, on ? 1 : 0);
      },
      // Cab expansion: the coach may switch between the BUILT-IN cabs only
      // (never 'custom' — that requires a user file upload).
      setCab: (cab) => {
        if (cab === 'clean212' || cab === 'brit412') setCabModel(cab);
      },
      addPedal: (type, position) => addPedal((type as PedalType) ?? 'rat', position),
      removePedal: (index) => {
        const id = pedalIdAt(index);
        if (id) removePedal(id);
      },
      movePedal: (from, to) => movePedal(from, to),
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
        inputTrim: r.input.trim,
        pedals: r.pedals,
        amp: r.amp.params,
        ampEngaged: r.amp.engaged,
        cabModel: r.amp.cabModel,
        customIr:
          r.amp.cabModel === 'custom' && customIrRef.current
            ? { samples: customIrRef.current.samples, sampleRate: customIrRef.current.sampleRate }
            : null,
        oversampling: r.oversampling,
        onLatencySamples: setLatencySamples,
        onPeak: setInputPeak,
        onTuner: handleTuner,
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
    setInputPeak(null);
    tunerReadingRef.current = null;
    setTunerReading(null);
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
              <span className="build-stamp" title="Build (git revision) — if this doesn't match the latest commit, the app is stale">
                build {__BUILD_STAMP__}
              </span>
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
              A RAT-style diode-clipper pedal into a JC-120-inspired clean amp and a selectable
              speaker cab (Clean 2×12, Brit 4×12, or your own IR) — modeled and played live in the
              browser. Drag the knobs, stomp the switch, power the amp; the whole rig is one
              serializable state, restored on reload.
            </p>
            <div className="hint">
              <span className="dot" />
              pedal → amp → cab · stomp / power to bypass · flip the theme
            </div>
          </div>
        </header>

        <div className="workspace">
          <div className="stage">
          <InputStage
            trim={rig.input.trim}
            onTrim={setInputTrim}
            peak={inputPeak}
            running={running}
          />
          <Board
            pedals={rig.pedals}
            amp={rig.amp}
            tunerReading={tunerReading}
            onPedalParam={setPedalParamById}
            onPedalToggle={toggleEngagedById}
            onAddPedal={addPedal}
            onRemovePedal={removePedal}
            onSwapPedal={swapPedal}
            onMovePedal={movePedal}
            onAmpParam={setAmpParam}
            onAmpToggle={toggleAmp}
            onAmpPower={toggleAmpPower}
            onChorusMode={setChorusMode}
            onCabSelect={setCabModel}
            onUploadIr={uploadIr}
          />
          {cabNote && (
            <div className="notice" data-testid="cab-note">
              <span className="dot" />
              {cabNote}
            </div>
          )}

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
