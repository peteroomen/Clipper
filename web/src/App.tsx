import { useEffect, useMemo, useRef, useState } from 'react';
import {
  startEngine,
  listInputDevices,
  detectPitch,
  type Engine,
  type RenderLoad,
} from './audio';
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
  type AmpType,
} from './rig';
import {
  processIrFile,
  saveCustomIr,
  loadCustomIr,
  customIrAtRate,
  type StoredCustomIr,
} from './cab';
import { loadGuitar, saveGuitar, type GuitarProfile } from './guitar';
import { UndoRing, DEFAULT_CAPACITY } from './undo';
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

// ---- undo labels (docs §39) ----
// The Undo control says WHAT it will undo, so every mutator names its action.
// Short, player-facing words: the label lands on a button, not in a log.
const PEDAL_LABEL: Record<PedalType, string> = {
  rat: 'RAT',
  sd1: 'SD-1',
  ts: 'Screamer',
  muff: 'Pi Fuzz',
  gold: 'Myth',
  phaser: 'Phaser',
  tuner: 'Tuner',
};
const SLOT_LABEL: Record<ParamName, string> = {
  distortion: 'drive',
  filter: 'tone',
  level: 'level',
};
const AMP_LABEL: Record<AmpParamName, string> = {
  volume: 'Volume',
  bass: 'Bass',
  middle: 'Middle',
  treble: 'Treble',
  bright: 'Bright',
  cab: 'Cab switch',
  speed: 'Speed',
  depth: 'Depth',
  chorusMode: 'Mod mode',
  reverb: 'Reverb',
  gain: 'Gain',
  presence: 'Presence',
  master: 'Master',
};
const AMP_TYPE_LABEL: Record<AmpType, string> = {
  clean120: 'Clean 120',
  jcm800: 'JCM800',
  twin: 'Twin Sixty-Five',
  ac30: 'Thirty',
};
const CAB_LABEL: Record<CabChoice, string> = {
  clean212: 'Clean 2×12',
  brit412: 'Brit 4×12',
  custom: 'custom IR',
};

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

  // Engine (DSP) load from AudioContext.renderCapacity — ground-truth audio-thread
  // headroom on the user's own machine (perf visibility, field-lag report). null
  // until the first 'update'; {supported:false} when the browser lacks the API.
  // Updates at ~1 Hz on purpose, so this state does not add to render churn.
  const [engineLoad, setEngineLoad] = useState<RenderLoad | null>(null);

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

  // ---- the undo ring (audit UI/UX findings 2+3; docs §39, ADR 011) ----
  //
  // `rigRef` is the SYNCHRONOUS source of truth and every mutator now writes it
  // eagerly through `commitRig()`. Two reasons:
  //   1. An undo snapshot taken at the top of a mutator must be the state
  //      BEFORE that mutation — `rig` (state) only catches up on the next
  //      render, which is too late.
  //   2. React 18 batches, so several mutations inside ONE tick (an assistant
  //      turn firing three tool calls in a row) all read the same stale
  //      `rigRef.current`. The whole-object mutators (add/remove/swap/move)
  //      therefore CLOBBERED each other: "add a Screamer and a phaser" landed
  //      only the phaser. Writing the ref eagerly makes them compose.
  function commitRig(next: RigState) {
    rigRef.current = next;
    setRig(next);
  }

  const undoRef = useRef<UndoRing | null>(null);
  // The only undo state React renders. It changes at most once per undo entry
  // (never per pointer move), because a coalesced push does not fire onChange.
  const [undoUi, setUndoUi] = useState<{
    undo: string | null;
    redo: string | null;
    depth: number;
  }>({ undo: null, redo: null, depth: 0 });

  function undoRing(): UndoRing {
    if (!undoRef.current) {
      undoRef.current = new UndoRing({
        // Serializing the LIVE rig, called only when the ring decides to push.
        snapshot: () => serializeRig(rigRef.current),
        now: () => performance.now(),
        onChange: () => {
          const r = undoRef.current;
          if (!r) return;
          setUndoUi({ undo: r.topLabel, redo: r.redoLabel, depth: r.depth });
        },
      });
    }
    return undoRef.current;
  }

  // Record the pre-change state. `key` non-null coalesces a continuous gesture
  // (a knob drag) into ONE entry; discrete actions pass null.
  function recordUndo(label: string, key: string | null = null) {
    undoRing().record(label, key);
  }

  // Push a whole rig to the running engine. Used by the undo/redo restore path:
  // a snapshot can differ from the live rig in ANY field, so everything is
  // re-sent. `prev` lets the two expensive topology messages (cab IR, amp voice)
  // be skipped when they did not actually change — a cab prepare is 11-46 ms in
  // the worklet's onmessage (docs §30), so sending it needlessly would cost a
  // late render quantum for nothing.
  function pushWholeRigToEngine(next: RigState, prev: RigState) {
    const e = engineRef.current;
    if (!e) return;
    e.setInputTrim(next.input.trim);
    if (next.oversampling !== prev.oversampling) e.setOversampling(next.oversampling);
    // The chain message carries each instance's params + engaged flag, and is
    // applied click-free (declick fade) in the worklet.
    e.setChain(next.pedals);
    for (const name of Object.keys(AMP_PARAM_ID) as AmpParamName[]) {
      e.setAmpParam(AMP_PARAM_ID[name], next.amp.params[name]);
    }
    if (next.amp.type !== prev.amp.type) e.setAmpModel(next.amp.type);
    e.setAmpBypass(!next.amp.engaged);
    if (next.amp.cabModel !== prev.amp.cabModel) {
      if (next.amp.cabModel === 'custom') {
        if (customIrRef.current) void loadCustomIntoEngine(customIrRef.current);
      } else {
        e.setCabBuiltin(next.amp.cabModel);
      }
    }
    const w = window as unknown as { __CLIPPER_TEST__?: Record<string, unknown> };
    if (w.__CLIPPER_TEST__) w.__CLIPPER_TEST__.lastChain = next.pedals.map((p) => p.id);
  }

  // Restore one snapshot. The stored JSON is treated as UNTRUSTED INPUT — it
  // goes back through `deserializeRig()` -> `normalizeRig()`, the same clamp /
  // coerce / migrate path a rig restored from localStorage gets, BEFORE any of
  // it reaches the engine. The undo path must not become a way to put a
  // non-finite or out-of-range value into the DSP (audit finding 1 / ADR 002).
  function applySnapshot(json: string): boolean {
    let next: RigState;
    try {
      next = deserializeRig(json);
    } catch {
      return false; // unparseable snapshot — leave the rig exactly as it is
    }
    // The custom IR's SAMPLES live outside the rig JSON, so a snapshot can name
    // a cab this session has no data for.
    if (next.amp.cabModel === 'custom' && !customIrRef.current) {
      next = { ...next, amp: { ...next.amp, cabModel: 'clean212', customCabLabel: undefined } };
      setCabNote('Custom IR not found — using the Clean 2×12.');
    }
    const prev = rigRef.current;
    commitRig(next);
    pushWholeRigToEngine(next, prev);
    return true;
  }

  function doUndo(): boolean {
    const e = undoRing().undo();
    return e ? applySnapshot(e.json) : false;
  }

  function doRedo(): boolean {
    const e = undoRing().redo();
    return e ? applySnapshot(e.json) : false;
  }

  // Cmd/Ctrl-Z undo, Cmd/Ctrl-Shift-Z (or Ctrl-Y) redo, from anywhere in the
  // app — EXCEPT inside a text field, where the platform's own text undo has to
  // win: the chat input's Cmd-Z must undo what the player typed, not their rig.
  useEffect(() => {
    function isTextTarget(t: EventTarget | null): boolean {
      const el = t as HTMLElement | null;
      if (!el || typeof el.tagName !== 'string') return false;
      const tag = el.tagName.toLowerCase();
      if (tag === 'input' || tag === 'textarea' || tag === 'select') return true;
      return el.isContentEditable === true;
    }
    function onKeyDown(ev: KeyboardEvent) {
      if (!(ev.metaKey || ev.ctrlKey) || ev.altKey) return;
      const k = ev.key.toLowerCase();
      if (k !== 'z' && k !== 'y') return;
      if (isTextTarget(ev.target)) return;
      ev.preventDefault();
      if (k === 'y' || ev.shiftKey) doRedo();
      else doUndo();
    }
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
    // The handlers close over refs and stable setters only.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

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
      // M6.8: push a synthetic reading into the tuner UI (segmented meter +
      // note screen) without a live audio path — used by the tuner-face tests
      // and screenshot capture. Bypasses the live throttle so every push renders.
      // Additive test seam only; no app behavior change.
      pushTunerReading: (r: TunerReading | null) => {
        tunerReadingRef.current = r;
        setTunerReading(r);
      },
      // Undo-ring seam (docs §39). Read-only counters + the two commands, plus
      // `injectRaw` — the ONLY way the suite can hand the restore path a hostile
      // snapshot and prove it re-validates instead of trusting it. The app never
      // calls injectRaw.
      undo: {
        capacity: DEFAULT_CAPACITY,
        depth: () => undoRing().depth,
        redoDepth: () => undoRing().redoDepth,
        labels: () => undoRing().labels(),
        // attempts = what a naive one-entry-per-change ring would have pushed.
        stats: () => {
          const r = undoRing();
          return {
            attempts: r.attempts,
            pushed: r.pushed,
            coalesced: r.coalesced,
            serializations: r.serializations,
            dropped: r.dropped,
          };
        },
        bytes: () => undoRing().bytes(),
        undo: () => doUndo(),
        redo: () => doRedo(),
        injectRaw: (json: string, label?: string) => undoRing().injectRaw(json, label),
      },
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
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
    const r0 = rigRef.current;
    const inst = r0.pedals.find((p) => p.id === pedalId);
    // A no-op write (a clamped drag edge, a double-click reset of a knob already
    // at its default) must not leave an undo step that appears to do nothing.
    if (inst && inst.params[name] === value) return;
    // COALESCED by (instance, slot): a whole drag on one knob is one undo entry.
    recordUndo(
      `${inst ? PEDAL_LABEL[inst.type] : 'Pedal'} ${SLOT_LABEL[name]}`,
      `p:${pedalId}:${name}`
    );
    commitRig({
      ...r0,
      pedals: r0.pedals.map((p) =>
        p.id === pedalId ? { ...p, params: { ...p.params, [name]: value } } : p
      ),
    });
    const id = PARAM_ID[name];
    engineRef.current?.setPedalParam(pedalId, id, value);
    const w = window as unknown as { __CLIPPER_TEST__?: Record<string, unknown> };
    if (w.__CLIPPER_TEST__) w.__CLIPPER_TEST__.lastParam = { id, value, pedalId };
  }

  // Rig-level input trim (0..1 knob). Applied in the worklet before the pedals.
  function setInputTrim(value: number) {
    if (rigRef.current.input.trim === value) return; // no-op: no undo step
    recordUndo('Input trim', 'input:trim'); // coalesced (continuous knob)
    const r0 = rigRef.current;
    commitRig({ ...r0, input: { ...r0.input, trim: value } });
    engineRef.current?.setInputTrim(value);
    const w = window as unknown as { __CLIPPER_TEST__?: Record<string, unknown> };
    if (w.__CLIPPER_TEST__) w.__CLIPPER_TEST__.lastInputTrim = value;
  }

  // Set a pedal instance's engaged state (footswitch / assistant). Per-instance
  // bypass message; engaged=false -> bypassed.
  function setPedalEngagedById(pedalId: string, engaged: boolean) {
    const r0 = rigRef.current;
    const inst = r0.pedals.find((p) => p.id === pedalId);
    // DISCRETE: never coalesced. Two stomps inside the coalescing window have to
    // be two undos, or the first undo would look like it did nothing.
    recordUndo(`${inst ? PEDAL_LABEL[inst.type] : 'Pedal'} ${engaged ? 'on' : 'bypass'}`);
    engineRef.current?.setPedalBypass(pedalId, !engaged); // bypass = not engaged
    commitRig({
      ...r0,
      pedals: r0.pedals.map((p) => (p.id === pedalId ? { ...p, engaged } : p)),
    });
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
    commitRig(next);
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
    recordUndo(`Add ${PEDAL_LABEL[type]}`);
    pushChain({ ...r, pedals });
    return at;
  }

  // DESTRUCTIVE (audit UI/UX finding 2): the instance and its dialled-in knobs
  // are gone from the rig, and the autosave commits immediately. Undoable now.
  function removePedal(pedalId: string) {
    const r = rigRef.current;
    const inst = r.pedals.find((p) => p.id === pedalId);
    if (!inst) return;
    recordUndo(`Remove ${PEDAL_LABEL[inst.type]}`);
    pushChain({ ...r, pedals: r.pedals.filter((p) => p.id !== pedalId) });
  }

  // Swap a pedal in place (remove + add the new type at the same slot). Keeps the
  // chain position. DESTRUCTIVE (audit UI/UX finding 2): the replacement is a
  // FRESH instance at that type's defaults, so the dialled-in settings of the old
  // one existed nowhere until this ring — the undo entry recorded here IS them.
  function swapPedal(pedalId: string, type: PedalType) {
    const r = rigRef.current;
    const idx = r.pedals.findIndex((p) => p.id === pedalId);
    if (idx < 0) return;
    const pedals = [...r.pedals];
    recordUndo(`Swap ${PEDAL_LABEL[pedals[idx].type]} → ${PEDAL_LABEL[type]}`);
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
    recordUndo(`Move ${PEDAL_LABEL[moved.type]} #${from + 1} → #${clampedTo + 1}`);
    pushChain({ ...r, pedals });
  }

  // ---- amp mutations ----

  // `coalesceKey` defaults to per-param coalescing (continuous knobs). The
  // discrete callers below pass null so each flip is its own undo step.
  function setAmpParam(
    name: AmpParamName,
    value: number,
    coalesceKey: string | null = `amp:${name}`
  ) {
    if (rigRef.current.amp.params[name] === value) return; // no-op: no undo step
    recordUndo(`Amp ${AMP_LABEL[name]}`, coalesceKey);
    const r0 = rigRef.current;
    commitRig({ ...r0, amp: { ...r0.amp, params: { ...r0.amp.params, [name]: value } } });
    engineRef.current?.setAmpParam(AMP_PARAM_ID[name], value);
    const w = window as unknown as { __CLIPPER_TEST__?: Record<string, unknown> };
    if (w.__CLIPPER_TEST__) w.__CLIPPER_TEST__.lastAmpParam = { id: AMP_PARAM_ID[name], value };
  }

  // Flip a 0/1 amp toggle (bright / cab).
  function toggleAmp(name: 'bright' | 'cab') {
    const next = rigRef.current.amp.params[name] >= 0.5 ? 0 : 1;
    setAmpParam(name, next, null); // discrete
  }

  // Set the 3-way chorus mode (0 off / 1 chorus / 2 vibrato). M6.3.
  function setChorusMode(mode: number) {
    setAmpParam('chorusMode', mode, null); // discrete
  }

  // Set the amp power (engaged) state explicitly (used by the Power rocker and
  // the assistant's set_engaged tool). engaged=false -> amp+cab bypassed.
  function setAmpEngaged(engaged: boolean) {
    recordUndo(engaged ? 'Amp power on' : 'Amp power off');
    engineRef.current?.setAmpBypass(!engaged); // amp bypass = power off
    const r0 = rigRef.current;
    commitRig({ ...r0, amp: { ...r0.amp, engaged } });
    const w = window as unknown as { __CLIPPER_TEST__?: Record<string, unknown> };
    if (w.__CLIPPER_TEST__) w.__CLIPPER_TEST__.lastAmpBypass = !engaged;
  }

  function toggleAmpPower() {
    setAmpEngaged(!rigRef.current.amp.engaged);
  }

  // Swap the amp voice (clean120 | jcm800 | twin). Updates the rig and, if running,
  // the engine (click-free in the worklet). A one-line cab HINT nudges the natural
  // pairing — the Brit 4×12 for the JCM, the Clean 2×12 for the Twin (a real Twin is
  // a 2×12 combo) — but never auto-switches the cab (the player's choice stays theirs).
  function setAmpType(type: AmpType) {
    if (rigRef.current.amp.type === type) return;
    recordUndo(`Amp ${AMP_TYPE_LABEL[type]}`);
    const r0 = rigRef.current;
    commitRig({ ...r0, amp: { ...r0.amp, type } });
    engineRef.current?.setAmpModel(type);
    if (type === 'jcm800' && rigRef.current.amp.cabModel === 'clean212') {
      setCabNote('JCM800 loaded — try the Brit 4×12 cab for a thicker, darker rock voicing.');
    } else if (type === 'twin' && rigRef.current.amp.cabModel === 'brit412') {
      setCabNote('Twin Sixty-Five loaded — try the Clean 2×12 (a real Twin is a 2×12 combo).');
    } else if (type === 'ac30' && rigRef.current.amp.cabModel === 'brit412') {
      setCabNote('Thirty loaded — try the Clean 2×12 (the closest 2×12 platform for its chime).');
    } else {
      setCabNote(null);
    }
    const w = window as unknown as { __CLIPPER_TEST__?: Record<string, unknown> };
    if (w.__CLIPPER_TEST__) w.__CLIPPER_TEST__.lastAmpModel = type;
  }

  // ---- cab selection + custom IR upload (cab expansion) ----

  // Missing-custom-IR fallback: a restored rig may say cabModel:'custom' while the
  // IR data (separate key) is absent (e.g. loaded in a different browser, or
  // cleared). Fall back to the Clean 2x12 with a note, once, on mount.
  useEffect(() => {
    if (rigRef.current.amp.cabModel === 'custom' && !customIrRef.current) {
      const r0 = rigRef.current;
      commitRig({
        ...r0,
        amp: { ...r0.amp, cabModel: 'clean212', customCabLabel: undefined },
      });
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
    if (rigRef.current.amp.cabModel === cab) return;
    recordUndo(`Cab ${CAB_LABEL[cab]}`);
    const r0 = rigRef.current;
    commitRig({ ...r0, amp: { ...r0.amp, cabModel: cab } });
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
      // The undo entry restores the previous cab SELECTION. The uploaded samples
      // themselves are deliberately not in the rig JSON (they live under their
      // own storage key), so undo cannot "unload" an IR — see §39.
      recordUndo('Upload IR');
      const r0 = rigRef.current;
      commitRig({
        ...r0,
        amp: { ...r0.amp, cabModel: 'custom', customCabLabel: processed.label },
      });
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
        // NaN-rejecting (2026-07-24 audit, finding 1): Math.min/Math.max are
        // transparent to NaN, so the old clamp let a non-finite assistant value
        // reach the engine, where it latched permanently in recursive state.
        const v = Math.min(1, Math.max(0, Number.isFinite(value) ? value : 0));
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
        // 'tremolo' is the Twin's trem on/off — the same chorusMode slot (0/1).
        if (name === 'chorus' || name === 'tremolo') setChorusMode(on ? 1 : 0);
        else if (name === 'vibrato') setChorusMode(on ? 2 : 0);
        else setAmpParam(name as AmpParamName, on ? 1 : 0);
      },
      // Cab expansion: the coach may switch between the BUILT-IN cabs only
      // (never 'custom' — that requires a user file upload).
      setCab: (cab) => {
        if (cab === 'clean212' || cab === 'brit412') setCabModel(cab);
      },
      // M9.4/M10.1/v1.1: the coach may swap the amp voice (clean120 | jcm800 | twin | ac30).
      setAmp: (type) => {
        if (type === 'clean120' || type === 'jcm800' || type === 'twin' || type === 'ac30')
          setAmpType(type);
      },
      addPedal: (type, position) => addPedal((type as PedalType) ?? 'rat', position),
      removePedal: (index) => {
        const id = pedalIdAt(index);
        if (id) removePedal(id);
      },
      movePedal: (from, to) => movePedal(from, to),
      // ONE assistant turn = ONE undo entry (audit UI/UX finding 3). The Chat
      // brackets a turn with these; every record() inside is suppressed and the
      // single pre-turn snapshot is pushed on end — and only if the rig actually
      // changed, so a text-only or cancelled turn leaves no entry at all. Without
      // this, undoing a multi-tool turn would strand the rig in a state the
      // player never saw.
      beginUndo: (label) => undoRing().begin(label),
      endUndo: () => undoRing().end(),
    }),
    // The referenced setters are behaviorally stable (functional setState + refs).
    // eslint-disable-next-line react-hooks/exhaustive-deps
    []
  );

  function setOversampling(factor: number) {
    if (rigRef.current.oversampling === factor) return;
    recordUndo(`Oversampling ${factor}×`);
    commitRig({ ...rigRef.current, oversampling: factor });
    engineRef.current?.setOversampling(factor);
  }

  function setSource(source: SourceKind) {
    if (rigRef.current.source === source) return;
    recordUndo(`Source ${source === 'live' ? 'live input' : 'test tone'}`);
    commitRig({ ...rigRef.current, source });
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
        ampType: r.amp.type,
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
        onRenderLoad: setEngineLoad,
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
    setEngineLoad(null);
    tunerReadingRef.current = null;
    setTunerReading(null);
  }

  const modelLatencyMs = sampleRate ? (latencySamples / sampleRate) * 1000 : 0;
  const ioLatencyMs = ((baseLatency ?? 0) + (outputLatency ?? 0)) * 1000;
  const totalLatencyMs = modelLatencyMs + ioLatencyMs;

  // Engine-load readout (perf visibility). "DSP 23%" from renderCapacity's average
  // load, with the peak alongside; an underrun (a real dropout) flips it to an alarm
  // state. When the browser lacks the API we say so rather than showing a fake 0%.
  // 'warn' when the average creeps past 80% (headroom running out — pre-glitch).
  const engineLoadText = !running
    ? '—'
    : engineLoad == null
      ? 'measuring…'
      : !engineLoad.supported
        ? 'n/a (needs Chromium)'
        : `DSP ${Math.round(engineLoad.averageLoad * 100)}% · peak ${Math.round(engineLoad.peakLoad * 100)}%` +
          (engineLoad.underrunRatio > 0
            ? ` · UNDERRUN ${Math.round(engineLoad.underrunRatio * 100)}%`
            : '');
  const engineLoadState =
    !running || engineLoad == null || !engineLoad.supported
      ? 'idle'
      : engineLoad.underrunRatio > 0
        ? 'underrun'
        : engineLoad.averageLoad > 0.8
          ? 'warn'
          : 'ok';

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
            onAmpModelSelect={setAmpType}
            onChorusMode={setChorusMode}
            onCabSelect={setCabModel}
            onUploadIr={uploadIr}
          />

          {/* Rig history (docs §39). The undo affordance names the action it will
              revert, so it is never a mystery button, and both entries are plain
              <button>s — keyboard operable for free, plus Cmd/Ctrl-Z anywhere
              outside a text field. */}
          <div className="rig-history" data-testid="rig-history" aria-label="Rig history">
            <button
              type="button"
              className="btn rh-btn"
              data-testid="undo"
              onClick={() => doUndo()}
              disabled={undoUi.undo === null}
              aria-keyshortcuts="Control+Z Meta+Z"
              title={
                undoUi.undo === null
                  ? 'Nothing to undo'
                  : `Undo ${undoUi.undo} — Ctrl/Cmd-Z`
              }
            >
              <span aria-hidden="true">↶</span> Undo
              {undoUi.undo !== null && (
                <span className="rh-label" data-testid="undo-label">
                  {undoUi.undo}
                </span>
              )}
            </button>
            <button
              type="button"
              className="btn rh-btn"
              data-testid="redo"
              onClick={() => doRedo()}
              disabled={undoUi.redo === null}
              aria-keyshortcuts="Control+Shift+Z Meta+Shift+Z"
              title={
                undoUi.redo === null ? 'Nothing to redo' : `Redo ${undoUi.redo} — Ctrl/Cmd-Shift-Z`
              }
            >
              <span aria-hidden="true">↷</span> Redo
            </button>
            <span className="rh-note mono" data-testid="undo-depth">
              {undoUi.depth === 0
                ? 'no rig history yet'
                : `${undoUi.depth} step${undoUi.depth === 1 ? '' : 's'} back`}
            </span>
          </div>

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
                  <dt>Engine load</dt>
                  <dd data-testid="engine-load" data-load-state={engineLoadState}>
                    {engineLoadText}
                  </dd>
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
