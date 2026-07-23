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

export type ParamName = 'distortion' | 'filter' | 'level';

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

export interface RigState {
  pedal: PedalState;
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

export const DEFAULT_RIG: RigState = {
  pedal: {
    type: 'rat',
    engaged: true,
    params: { ...KNOB_DEFAULTS },
  },
  oversampling: 4,
  source: 'test',
};

const STORAGE_KEY = 'clipper.rig.v1';

function clamp01(n: unknown, fallback: number): number {
  return typeof n === 'number' && Number.isFinite(n) && n >= 0 && n <= 1 ? n : fallback;
}

// Coerce arbitrary parsed JSON into a valid RigState, filling any missing or
// invalid field from DEFAULT_RIG. A well-formed rig passes through unchanged, so
// serialize -> deserialize round-trips exactly.
export function normalizeRig(raw: unknown): RigState {
  const d = DEFAULT_RIG;
  const r = (raw ?? {}) as Record<string, unknown>;
  const p = (r.pedal ?? {}) as Record<string, unknown>;
  const pr = (p.params ?? {}) as Record<string, unknown>;

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

export function loadRig(): RigState {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return { ...DEFAULT_RIG, pedal: { ...DEFAULT_RIG.pedal, params: { ...KNOB_DEFAULTS } } };
    return deserializeRig(raw);
  } catch {
    return { ...DEFAULT_RIG, pedal: { ...DEFAULT_RIG.pedal, params: { ...KNOB_DEFAULTS } } };
  }
}
