// Tuner (M7) — the pure pitch→note math and the shared frame constants.
//
// Detection itself (McLeod pitch method) runs on the MAIN THREAD via the `pitchy`
// package, fed by ~FRAME_SIZE-sample frames the worklet taps off the post-input-
// trim, pre-chain signal (the raw guitar) and posts up whenever an engaged tuner
// is in the chain. This module holds only the pure, testable helpers: converting a
// detected frequency into a chromatic note name + cents deviation (reference pitch
// fixed at A=440), and the frame-size / hop constants shared with the worklet.
//
// FRAME SIZE — why 4096. The McLeod method needs the analysis window to contain a
// couple of periods of the note for the NSDF autocorrelation peak at that lag to
// have enough overlap to be reliable. The lowest note we care about is a 7-string
// low B (B0 ≈ 30.87 Hz), whose period is ~32 ms. A 2048-sample window (~43 ms at
// 48 k) is only ~1.3 periods of B0 — measured, it fails to lock at all on B0
// (30/30 misses) while being dead-on from low E (82.41 Hz) up. A 4096-sample
// window (~85 ms at 48 k) is ~2.6 periods of B0 and locks it to <0.1 cents, with
// no measurable cost elsewhere. The frame is only tapped/posted while a tuner is
// engaged, so the larger window is free the rest of the time. LOWEST RELIABLE
// NOTE at 4096/48k: B0 (~31 Hz). (A hop of every 4th 128-sample block ≈ 93
// readings/s — far above the 60 fps the needle interpolates at.)
export const TUNER_FRAME_SIZE = 4096;
export const TUNER_HOP_BLOCKS = 4;

// Reference pitch, fixed (the pedal has no calibration knob).
export const A4_HZ = 440;

// A reading counts as "in tune" within this many cents; the UI holds the green
// lock LED once the note has stayed in tune for a short dwell (see Tuner.tsx).
export const IN_TUNE_CENTS = 3;

// Detection gates: ignore sub-/super-guitar frequencies and low-confidence
// (noisy / no-note) frames. B0 ≈ 30.87 Hz is the lowest note we claim; the floor
// sits just below it. The ceiling clears the top of a 24-fret guitar (~1319 Hz).
export const TUNER_MIN_HZ = 27.5;
export const TUNER_MAX_HZ = 1400;
export const TUNER_MIN_CLARITY = 0.7;

// Chromatic note names, sharps (A=440 reference, 12-TET).
export const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'] as const;

export interface TunerReading {
  freq: number; // detected fundamental, Hz
  note: string; // e.g. 'E', 'A#'
  octave: number; // scientific pitch octave (A4 -> octave 4)
  cents: number; // signed deviation from the nearest note, rounded to the cent
  clarity: number; // 0..1 MPM clarity of the detection
}

// Convert a detected (frequency, clarity) into a chromatic reading, or null when
// the frame is out of range or too unclear to trust (so the UI shows no note).
// 12-TET around A4=440: MIDI = 69 + 12·log2(f/440); the nearest integer is the
// note, the fractional remainder is the cents deviation.
export function analyzePitch(freq: number, clarity: number): TunerReading | null {
  if (!Number.isFinite(freq) || freq < TUNER_MIN_HZ || freq > TUNER_MAX_HZ) return null;
  if (!Number.isFinite(clarity) || clarity < TUNER_MIN_CLARITY) return null;
  const midi = 69 + 12 * Math.log2(freq / A4_HZ);
  const nearest = Math.round(midi);
  const cents = Math.round((midi - nearest) * 100);
  const note = NOTE_NAMES[((nearest % 12) + 12) % 12];
  const octave = Math.floor(nearest / 12) - 1;
  return { freq, note, octave, cents, clarity };
}

// A compact label like "E2", "A#3" for a reading.
export function noteLabel(r: TunerReading): string {
  return `${r.note}${r.octave}`;
}
