// Cab expansion — USER IR upload pipeline + custom-IR persistence.
//
// Everything here runs on the MAIN thread. A user picks a .wav; we decode it,
// mono-ize it, resample it to the engine's sample rate, cap its length, and hand
// the raw Float32 samples to the worklet, which copies them onto the WASM heap
// and calls amp_load_custom_ir — where the CORE peak-normalizes it (M6.6: the
// engine never trusts a file's level, so a loud cab can't boost/fizz).
//
// The IR SAMPLES are persisted in their OWN localStorage key (base64), never in
// the rig/preset JSON — a preset records only cabModel:'custom' + a short label.
// A rig that references a custom IR whose data is missing (e.g. shared to another
// browser) falls back to the Clean 2x12 with a UI note.

// Cap: the convolver partitions at 128 and handles longer IRs fine, but a guitar
// cab IR is short; 4096 samples (~85 ms @48k) is plenty and bounds CPU/memory.
export const MAX_IR_SAMPLES = 4096;
// Fade-out applied over the last FADE samples when we truncate, so a hard cut
// doesn't add a click/High-frequency spray to the IR.
const TRUNCATE_FADE = 128;

const STORAGE_KEY = 'clipper.customCab.v1';

// What we keep for a loaded custom IR: the engine-rate samples (base64), the rate
// they were resampled to, a short label, and the pre-cap length (for the UI).
export interface StoredCustomIr {
  label: string;
  sampleRate: number; // rate `samples` are at
  originalLength: number; // resampled length BEFORE the 4096 cap (UI reports this)
  samples: Float32Array; // mono, resampled, capped, NOT normalized (core does that)
}

// The result of decoding+processing an uploaded file, ready to send to the engine
// and persist.
export interface ProcessedIr extends StoredCustomIr {
  truncated: boolean; // true if the source was longer than MAX_IR_SAMPLES
}

// Average all channels of an AudioBuffer into one mono Float32Array.
function monoize(buf: AudioBuffer): Float32Array {
  const n = buf.length;
  const out = new Float32Array(n);
  const ch = buf.numberOfChannels;
  for (let c = 0; c < ch; c++) {
    const data = buf.getChannelData(c);
    for (let i = 0; i < n; i++) out[i] += data[i];
  }
  if (ch > 1) for (let i = 0; i < n; i++) out[i] /= ch;
  return out;
}

// Resample a mono buffer from `fromRate` to `toRate` via an OfflineAudioContext
// (browser-quality sample-rate conversion). Returns the mono result. If the rates
// already match, returns the input unchanged.
export async function resampleMono(
  samples: Float32Array,
  fromRate: number,
  toRate: number
): Promise<Float32Array> {
  if (Math.abs(fromRate - toRate) < 1e-6 || samples.length === 0) return samples;
  const outLen = Math.max(1, Math.round((samples.length * toRate) / fromRate));
  const offline = new OfflineAudioContext(1, outLen, toRate);
  const src = offline.createBuffer(1, samples.length, fromRate);
  src.getChannelData(0).set(samples);
  const node = offline.createBufferSource();
  node.buffer = src;
  node.connect(offline.destination);
  node.start();
  const rendered = await offline.startRendering();
  return rendered.getChannelData(0).slice();
}

// Cap to MAX_IR_SAMPLES with a short raised-cosine fade-out on the tail so a hard
// truncation doesn't click. Returns {samples, truncated}.
function capLength(samples: Float32Array): { samples: Float32Array; truncated: boolean } {
  if (samples.length <= MAX_IR_SAMPLES) return { samples, truncated: false };
  const out = samples.slice(0, MAX_IR_SAMPLES);
  const start = MAX_IR_SAMPLES - TRUNCATE_FADE;
  for (let i = start; i < MAX_IR_SAMPLES; i++) {
    const t = (i - start) / TRUNCATE_FADE; // 0..1
    out[i] *= 0.5 * (1 + Math.cos(Math.PI * t));
  }
  return { samples: out, truncated: true };
}

// Decode + process an uploaded IR file into engine-rate mono samples. The passed
// AudioContext supplies decodeAudioData and the target (engine) sample rate.
export async function processIrFile(file: File, engineRate: number): Promise<ProcessedIr> {
  const bytes = await file.arrayBuffer();
  // A fresh short-lived context for decoding; decodeAudioData is independent of
  // the live engine context and works offline.
  const decodeCtx = new (window.AudioContext ||
    (window as unknown as { webkitAudioContext: typeof AudioContext }).webkitAudioContext)();
  let decoded: AudioBuffer;
  try {
    decoded = await decodeCtx.decodeAudioData(bytes);
  } finally {
    void decodeCtx.close();
  }
  const mono = monoize(decoded);
  const resampled = await resampleMono(mono, decoded.sampleRate, engineRate);
  const originalLength = resampled.length;
  const { samples, truncated } = capLength(resampled);
  const label = file.name.replace(/\.[^.]+$/, '').slice(0, 40) || 'Custom IR';
  return { label, sampleRate: engineRate, originalLength, samples, truncated };
}

// --- Base64 <-> Float32Array (for localStorage) --------------------------------
function float32ToBase64(arr: Float32Array): string {
  const bytes = new Uint8Array(arr.buffer, arr.byteOffset, arr.byteLength);
  let bin = '';
  const CHUNK = 0x8000;
  for (let i = 0; i < bytes.length; i += CHUNK) {
    bin += String.fromCharCode(...bytes.subarray(i, i + CHUNK));
  }
  return btoa(bin);
}

function base64ToFloat32(b64: string): Float32Array {
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  // Copy into an aligned buffer (byte offset 0) so the Float32 view is valid.
  return new Float32Array(bytes.buffer.slice(0));
}

// --- Persistence ---------------------------------------------------------------
export function saveCustomIr(ir: StoredCustomIr): void {
  try {
    localStorage.setItem(
      STORAGE_KEY,
      JSON.stringify({
        label: ir.label,
        sampleRate: ir.sampleRate,
        originalLength: ir.originalLength,
        samples: float32ToBase64(ir.samples),
      })
    );
  } catch {
    /* storage may be unavailable (private mode, quota) — non-fatal */
  }
}

export function loadCustomIr(): StoredCustomIr | null {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return null;
    const o = JSON.parse(raw) as Record<string, unknown>;
    if (typeof o.samples !== 'string') return null;
    const samples = base64ToFloat32(o.samples);
    if (samples.length === 0) return null;
    return {
      label: typeof o.label === 'string' ? o.label : 'Custom IR',
      sampleRate: typeof o.sampleRate === 'number' ? o.sampleRate : 48000,
      originalLength: typeof o.originalLength === 'number' ? o.originalLength : samples.length,
      samples,
    };
  } catch {
    return null;
  }
}

export function clearCustomIr(): void {
  try {
    localStorage.removeItem(STORAGE_KEY);
  } catch {
    /* non-fatal */
  }
}

// Ensure stored samples are at `engineRate` (resample if the session's rate
// differs from when they were saved), for handing to the worklet.
export async function customIrAtRate(
  ir: StoredCustomIr,
  engineRate: number
): Promise<Float32Array> {
  if (Math.abs(ir.sampleRate - engineRate) < 1e-6) return ir.samples;
  return resampleMono(ir.samples, ir.sampleRate, engineRate);
}
