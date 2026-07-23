// Rig state: a single serializable structure describing the whole rig.
//
// This is deliberate groundwork. It becomes:
//   - the AI assistant's tool-call surface in M6 (read state / mutate params), and
//   - the preset format later.
//
// M6.4: the single `pedal` grew into an ORDERED CHAIN `pedals: PedalInstance[]`
// (stackable, reorderable, add/remove/swap). Each instance carries a stable `id`
// so the worklet can diff the chain (reuse handles on reorder) and the UI can key
// / drag it. `deserializeRig()` normalizes unknown input, and `normalizeRig`
// migrates an old single-`pedal` rig (M4..M6.3) into a one-element chain, so old
// saved rigs and preset JSON keep loading.

import { OVERSAMPLING_FACTORS, INPUT_TRIM_UNITY_KNOB } from './params';

export type SourceKind = 'test' | 'live';
// M8 adds 'sd1' (Boss SD-1 Super Overdrive). Both dirt pedals share the SAME
// three-knob param shape (PedalParams below) and the same numeric worklet ABI
// (id 0/1/2); for an SD-1 those three knobs READ as Drive / Tone / Level (the
// Pedal component relabels them), so distortion==Drive and filter==Tone. Keeping
// one param shape keeps the chain/worklet/serializer pedal-agnostic.
export type PedalType = 'rat' | 'sd1';
export type AmpType = 'clean120';

// The pedal types that can be added from the gear tray (M6.4). RAT + SD-1 today;
// M7's tuner appends here.
export const AVAILABLE_PEDAL_TYPES: readonly PedalType[] = ['rat', 'sd1'];
// The amp types that can be selected in the amp slot (M6.4). Currently just the
// Clean 120; M9's JCM800 appends here.
export const AVAILABLE_AMP_TYPES: readonly AmpType[] = ['clean120'];

export type ParamName = 'distortion' | 'filter' | 'level';
export type AmpParamName =
  | 'volume'
  | 'bass'
  | 'middle'
  | 'treble'
  | 'bright'
  | 'cab'
  | 'speed'
  | 'depth'
  | 'chorusMode';

export interface PedalParams {
  distortion: number; // 0..1 knob position
  filter: number; // 0..1 knob position
  level: number; // 0..1 knob position
}

// A pedal INSTANCE in the chain (M6.4). `id` is a stable, unique handle used by
// the worklet (to reuse the DSP instance across reorders) and the UI (React key,
// drag id). Multiple instances of the same `type` are allowed (e.g. two RATs).
export interface PedalInstance {
  id: string;
  type: PedalType;
  engaged: boolean; // true = pedal active (LED lit); false = bypassed
  params: PedalParams;
}

// Kept as an alias for the per-pedal shape (some callers still speak "pedal").
export type PedalState = PedalInstance;

// Unique pedal-instance id generator. A monotonic counter keeps ids stable and
// human-legible within a session; the random suffix avoids collisions if two
// rigs are merged (e.g. presets). Not persisted globally — only the emitted ids
// are, and those round-trip through JSON untouched.
let pedalIdCounter = 0;
export function newPedalId(type: PedalType = 'rat'): string {
  pedalIdCounter += 1;
  return `${type}-${pedalIdCounter}-${Math.random().toString(36).slice(2, 7)}`;
}

// Amp params. volume/bass/middle/treble/speed/depth are continuous 0..1 knobs
// (tone controls flat at 0.5); bright and cab are 0/1 toggles; chorusMode is a
// 3-way integer switch (0=off, 1=chorus, 2=vibrato — the JC-120's stereo
// chorus/vibrato, M6.3).
export interface AmpParams {
  volume: number;
  bass: number;
  middle: number;
  treble: number;
  bright: number; // 0/1 bright-switch toggle
  cab: number; // 0/1 cab-convolver enable (1 = cab on)
  speed: number; // 0..1 chorus/vibrato LFO rate knob
  depth: number; // 0..1 chorus/vibrato sweep-depth knob
  chorusMode: number; // 0 off | 1 chorus | 2 vibrato
}

export interface AmpState {
  type: AmpType;
  engaged: boolean; // true = amp powered (jewel lit); false = amp+cab bypassed
  params: AmpParams;
}

// Rig-level input stage (M6.1): a trim applied BEFORE the pedal, for calibrating
// real-world interface levels into the model. `trim` is a 0..1 knob position
// (mapped to a dB gain in params.ts; INPUT_TRIM_UNITY_KNOB == 0 dB == default).
export interface InputState {
  trim: number; // 0..1 knob position
}

export interface RigState {
  input: InputState;
  pedals: PedalInstance[]; // ordered chain, guitar -> pedals[0] -> ... -> amp
  amp: AmpState;
  oversampling: number; // 1 | 2 | 4 | 8 (nonlinear-stage oversampling)
  source: SourceKind;
}

// Per-knob reset targets (double-click a knob to snap here). These mirror the
// RAT model's shipped defaults from M1/M3.
export const KNOB_DEFAULTS: PedalParams = {
  distortion: 0.7,
  filter: 0.4,
  level: 0.8,
};

// SD-1 (M8) opening state. Same param slots (distortion==Drive, filter==Tone,
// level==Level): a moderate Drive, Tone at noon (transparent), healthy Level.
export const SD1_KNOB_DEFAULTS: PedalParams = {
  distortion: 0.5,
  filter: 0.5,
  level: 0.7,
};

// Per-type opening knob positions (gear tray "add" / swap use this).
export const PEDAL_KNOB_DEFAULTS: Record<PedalType, PedalParams> = {
  rat: KNOB_DEFAULTS,
  sd1: SD1_KNOB_DEFAULTS,
};

// Amp defaults mirror the approved design's opening state: Vol 40, Bass/Mid 50
// (flat), Treble 60, bright off, cab on. Tone controls are flat at 0.5. Chorus
// ships OFF (mode 0) with a musical speed/depth ready for when it is engaged.
export const AMP_KNOB_DEFAULTS: AmpParams = {
  volume: 0.4,
  bass: 0.5,
  middle: 0.5,
  treble: 0.6,
  bright: 0,
  cab: 1,
  speed: 0.3,
  depth: 0.5,
  chorusMode: 0,
};

// Default input trim: unity (0 dB).
export const INPUT_DEFAULTS: InputState = {
  trim: INPUT_TRIM_UNITY_KNOB,
};

// A fresh pedal instance of the given type, with that type's default knobs and a
// new unique id. The gear tray's "add pedal" and swap flows use this.
export function makePedal(type: PedalType = 'rat'): PedalInstance {
  return {
    id: newPedalId(type),
    type,
    engaged: true,
    params: { ...(PEDAL_KNOB_DEFAULTS[type] ?? KNOB_DEFAULTS) },
  };
}

export const DEFAULT_RIG: RigState = {
  input: { ...INPUT_DEFAULTS },
  pedals: [
    {
      id: 'rat-1',
      type: 'rat',
      engaged: true,
      params: { ...KNOB_DEFAULTS },
    },
  ],
  amp: {
    type: 'clean120',
    engaged: true,
    params: { ...AMP_KNOB_DEFAULTS },
  },
  oversampling: 4,
  source: 'test',
};

const STORAGE_KEY = 'clipper.rig.v1';

function clamp01(n: unknown, fallback: number): number {
  return typeof n === 'number' && Number.isFinite(n) && n >= 0 && n <= 1 ? n : fallback;
}

// A 0/1 toggle field: coerce to exactly 0 or 1, falling back on invalid input.
function toggle01(n: unknown, fallback: number): number {
  if (typeof n === 'number' && Number.isFinite(n)) return n >= 0.5 ? 1 : 0;
  return fallback;
}

// The 3-way chorus mode: coerce to an integer in {0, 1, 2}, else the fallback.
function mode012(n: unknown, fallback: number): number {
  if (typeof n === 'number' && Number.isFinite(n)) {
    const m = Math.round(n);
    if (m === 0 || m === 1 || m === 2) return m;
  }
  return fallback;
}

// Coerce arbitrary parsed JSON into a valid RigState, filling any missing or
// invalid field from DEFAULT_RIG. A well-formed rig passes through unchanged, so
// serialize -> deserialize round-trips exactly.
// Normalize one raw pedal object into a valid PedalInstance. `fallbackId` is used
// only when the raw object carries no usable id (e.g. a migrated single pedal, or
// a preset authored without ids).
function normalizePedal(raw: unknown, fallbackId: string): PedalInstance {
  const p = (raw ?? {}) as Record<string, unknown>;
  const pr = (p.params ?? {}) as Record<string, unknown>;
  const dp = DEFAULT_RIG.pedals[0];
  const id = typeof p.id === 'string' && p.id.length > 0 ? p.id : fallbackId;
  const type: PedalType = p.type === 'sd1' ? 'sd1' : 'rat'; // unknown coerces to RAT
  return {
    id,
    type,
    engaged: typeof p.engaged === 'boolean' ? p.engaged : dp.engaged,
    params: {
      distortion: clamp01(pr.distortion, dp.params.distortion),
      filter: clamp01(pr.filter, dp.params.filter),
      level: clamp01(pr.level, dp.params.level),
    },
  };
}

export function normalizeRig(raw: unknown): RigState {
  const d = DEFAULT_RIG;
  const r = (raw ?? {}) as Record<string, unknown>;
  // Migration seam: an M4-shaped rig has no `amp` — the whole amp block then
  // falls back to DEFAULT_RIG.amp, field by field.
  const a = (r.amp ?? {}) as Record<string, unknown>;
  const ar = (a.params ?? {}) as Record<string, unknown>;

  const oversampling = (OVERSAMPLING_FACTORS as readonly number[]).includes(
    r.oversampling as number
  )
    ? (r.oversampling as number)
    : d.oversampling;

  const source: SourceKind = r.source === 'live' || r.source === 'test' ? r.source : d.source;

  // Migration seam: a pre-M6.1 rig has no `input` — fall back to the default.
  const inp = (r.input ?? {}) as Record<string, unknown>;

  // Chain migration (M6.4). Priority:
  //   1. `pedals` is an array   -> normalize each (an EMPTY array is a valid
  //      chain: guitar straight into the amp).
  //   2. legacy single `pedal`  -> wrap it as a one-element chain (M4..M6.3).
  //   3. neither                -> the default one-RAT chain.
  let pedals: PedalInstance[];
  if (Array.isArray(r.pedals)) {
    pedals = r.pedals.map((raw, i) => normalizePedal(raw, `rat-${i + 1}`));
  } else if (r.pedal != null) {
    pedals = [normalizePedal(r.pedal, 'rat-1')];
  } else {
    pedals = d.pedals.map((p) => ({ ...p, params: { ...p.params } }));
  }

  return {
    input: { trim: clamp01(inp.trim, d.input.trim) },
    pedals,
    amp: {
      type: 'clean120',
      engaged: typeof a.engaged === 'boolean' ? a.engaged : d.amp.engaged,
      params: {
        volume: clamp01(ar.volume, d.amp.params.volume),
        bass: clamp01(ar.bass, d.amp.params.bass),
        middle: clamp01(ar.middle, d.amp.params.middle),
        treble: clamp01(ar.treble, d.amp.params.treble),
        bright: toggle01(ar.bright, d.amp.params.bright),
        cab: toggle01(ar.cab, d.amp.params.cab),
        // Migration seam: a pre-M6.3 rig has no chorus fields — old saved rigs
        // load with chorus OFF and the default speed/depth.
        speed: clamp01(ar.speed, d.amp.params.speed),
        depth: clamp01(ar.depth, d.amp.params.depth),
        chorusMode: mode012(ar.chorusMode, d.amp.params.chorusMode),
      },
    },
    oversampling,
    source,
  };
}

// JSON <-> RigState. serializeRig(deserializeRig(x)) is stable; a valid rig
// survives a serialize/deserialize round-trip byte-for-byte.
export function serializeRig(rig: RigState): string {
  return JSON.stringify(rig);
}

export function deserializeRig(json: string): RigState {
  return normalizeRig(JSON.parse(json));
}

// Persist to / restore from localStorage. Restore falls back to defaults on any
// parse failure or absent key.
export function saveRig(rig: RigState): void {
  try {
    localStorage.setItem(STORAGE_KEY, serializeRig(rig));
  } catch {
    /* storage may be unavailable (private mode, quota) — non-fatal */
  }
}

// A fresh, deep-copied default rig (no shared references into DEFAULT_RIG).
export function freshDefaultRig(): RigState {
  return {
    input: { ...INPUT_DEFAULTS },
    pedals: [makePedal('rat')],
    amp: { type: 'clean120', engaged: true, params: { ...AMP_KNOB_DEFAULTS } },
    oversampling: DEFAULT_RIG.oversampling,
    source: DEFAULT_RIG.source,
  };
}

export function loadRig(): RigState {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return freshDefaultRig();
    return deserializeRig(raw);
  } catch {
    return freshDefaultRig();
  }
}
