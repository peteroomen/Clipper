// Rig state (M4): a single serializable structure describing the whole rig.
//
// This is deliberate groundwork. It becomes:
//   - the AI assistant's tool-call surface in M6 (read state / mutate params), and
//   - the preset format later.
//
// The shape is kept intentionally small and typed. It is "one pedal" today; the
// `pedal` field is the natural seam to grow into a `pedals: PedalState[]` chain
// when the M5 amp/cab arrive — deserializeRig() already normalizes unknown
// input, so widening the schema stays backward-compatible.

import { OVERSAMPLING_FACTORS } from './params';

export type SourceKind = 'test' | 'live';
export type PedalType = 'rat';
export type AmpType = 'clean120';

export type ParamName = 'distortion' | 'filter' | 'level';
export type AmpParamName = 'volume' | 'bass' | 'middle' | 'treble' | 'bright' | 'cab';

export interface PedalParams {
  distortion: number; // 0..1 knob position
  filter: number; // 0..1 knob position
  level: number; // 0..1 knob position
}

export interface PedalState {
  type: PedalType;
  engaged: boolean; // true = pedal active (LED lit); false = bypassed
  params: PedalParams;
}

// Amp params are all normalized 0..1. volume/bass/middle/treble are continuous
// knobs (tone controls flat at 0.5); bright and cab are 0/1 toggles.
export interface AmpParams {
  volume: number;
  bass: number;
  middle: number;
  treble: number;
  bright: number; // 0/1 bright-switch toggle
  cab: number; // 0/1 cab-convolver enable (1 = cab on)
}

export interface AmpState {
  type: AmpType;
  engaged: boolean; // true = amp powered (jewel lit); false = amp+cab bypassed
  params: AmpParams;
}

export interface RigState {
  pedal: PedalState;
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

// Amp defaults mirror the approved design's opening state: Vol 40, Bass/Mid 50
// (flat), Treble 60, bright off, cab on. Tone controls are flat at 0.5.
export const AMP_KNOB_DEFAULTS: AmpParams = {
  volume: 0.4,
  bass: 0.5,
  middle: 0.5,
  treble: 0.6,
  bright: 0,
  cab: 1,
};

export const DEFAULT_RIG: RigState = {
  pedal: {
    type: 'rat',
    engaged: true,
    params: { ...KNOB_DEFAULTS },
  },
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

// Coerce arbitrary parsed JSON into a valid RigState, filling any missing or
// invalid field from DEFAULT_RIG. A well-formed rig passes through unchanged, so
// serialize -> deserialize round-trips exactly.
export function normalizeRig(raw: unknown): RigState {
  const d = DEFAULT_RIG;
  const r = (raw ?? {}) as Record<string, unknown>;
  const p = (r.pedal ?? {}) as Record<string, unknown>;
  const pr = (p.params ?? {}) as Record<string, unknown>;
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

  return {
    pedal: {
      type: 'rat',
      engaged: typeof p.engaged === 'boolean' ? p.engaged : d.pedal.engaged,
      params: {
        distortion: clamp01(pr.distortion, d.pedal.params.distortion),
        filter: clamp01(pr.filter, d.pedal.params.filter),
        level: clamp01(pr.level, d.pedal.params.level),
      },
    },
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
    pedal: { type: 'rat', engaged: true, params: { ...KNOB_DEFAULTS } },
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
