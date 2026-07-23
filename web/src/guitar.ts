// Guitar profile (M6): the small, user-entered description of their instrument
// that gets injected into the assistant's context so its advice is personal
// (e.g. "roll the tone back on your bridge humbucker" only makes sense if it
// knows what pickups you have). Stored in localStorage under a SEPARATE key from
// the rig, and editable anytime.

export type PickupType = 'SSS' | 'HSS' | 'HH' | 'P90' | 'other';

export const PICKUP_TYPES: readonly PickupType[] = ['SSS', 'HSS', 'HH', 'P90', 'other'];

export interface GuitarProfile {
  model: string; // free text, e.g. "Fender Stratocaster"
  pickupType: PickupType; // SSS / HSS / HH / P90 / other
  pickupPosition: string; // free text or 1-5, e.g. "bridge" / "4"
}

export const DEFAULT_GUITAR: GuitarProfile = {
  model: '',
  pickupType: 'SSS',
  pickupPosition: '',
};

const STORAGE_KEY = 'clipper.guitar.v1';

function normalize(raw: unknown): GuitarProfile {
  const r = (raw ?? {}) as Record<string, unknown>;
  const pickupType = (PICKUP_TYPES as readonly string[]).includes(r.pickupType as string)
    ? (r.pickupType as PickupType)
    : DEFAULT_GUITAR.pickupType;
  return {
    model: typeof r.model === 'string' ? r.model : DEFAULT_GUITAR.model,
    pickupType,
    pickupPosition:
      typeof r.pickupPosition === 'string' ? r.pickupPosition : DEFAULT_GUITAR.pickupPosition,
  };
}

export function loadGuitar(): GuitarProfile {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return { ...DEFAULT_GUITAR };
    return normalize(JSON.parse(raw));
  } catch {
    return { ...DEFAULT_GUITAR };
  }
}

export function saveGuitar(g: GuitarProfile): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(g));
  } catch {
    /* storage may be unavailable (private mode, quota) — non-fatal */
  }
}

// A human-readable line for the assistant's context preamble. Falls back
// gracefully when the user hasn't filled anything in.
export function describeGuitar(g: GuitarProfile): string {
  const model = g.model.trim() || 'unspecified guitar';
  const pos = g.pickupPosition.trim();
  const posText = pos ? `, pickup position: ${pos}` : '';
  return `${model} (${g.pickupType} pickups${posText})`;
}
