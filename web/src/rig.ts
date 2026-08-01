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
// M7 adds 'tuner' (chromatic needle tuner: no audio DSP, engaged = mutes the
// chain). M8 adds 'sd1' (Boss SD-1 Super Overdrive). Both dirt pedals share the
// SAME three-knob param shape (PedalParams below) and the same numeric worklet
// ABI (id 0/1/2); for an SD-1 those three knobs READ as Drive / Tone / Level
// (the Pedal component relabels them), so distortion==Drive and filter==Tone.
// One param shape keeps the chain/worklet/serializer pedal-agnostic.
// v1.1 adds 'ts' (a TS808-style "Screamer" overdrive) — the SYMMETRIC-clipping
// sibling of the SD-1, sharing the exact same three-knob param shape + ABI (its
// slots also READ as Drive / Tone / Level) — and 'phaser' (the script-era
// 4-stage allpass phaser, "Ninety"): same shape/ABI, but ONE real knob — slot 0
// (distortion) carries SPEED; slots 1/2 are unused-but-carried.
// v1.1 item 4 adds 'muff' (the four-transistor "Pi" fuzz — a Big-Muff-family wall
// of sustain): SAME three-knob shape/ABI, its slots reading as SUSTAIN / TONE /
// VOLUME (distortion==Sustain, filter==Tone, level==Volume). It is the first BJT
// (Ebers-Moll) voice; the ABI stays pedal-agnostic.
// v1.1 item 6 adds 'gold' (the GOLD "transparent" overdrive — the gold-box legend):
// SAME three-knob shape/ABI, its slots reading as GAIN / TREBLE / OUTPUT
// (distortion==Gain, filter==Treble, level==Output). Its architecture is the odd one
// out: a PARALLEL clean/dirt blend cross-faded by a dual-ganged gain pot, with
// germanium clippers — so at GAIN 0 it is a genuinely clean buffer/boost.
// M13.1 adds 'comp' (the "Squash" OTA compressor — the first DYNAMICS pedal, and
// the first that is not dirt): SAME three-slot shape/ABI, but only TWO of the
// slots are real knobs — slot 0 (distortion) is SUSTAIN and slot 2 (level) is
// LEVEL. Slot 1 is carried and unused, exactly as the phaser carries 1 and 2.
// SUSTAIN is NOT a threshold: it sets the CA3080 gain cell's idle bias current,
// so it buys compression, make-up gain and noise together.
// Post-v1.1 adds 'wah' (the "Weeper" — the lineup's FIRST FILTER pedal, a
// GCB-95-class Cry Baby): SAME three-knob shape/ABI, its slots reading as
// POSITION / SENSE / VOICE (distortion==Position, filter==Sense, level==Voice).
// POSITION is an ordinary automatable parameter (a treadle you can sweep); SENSE
// at 0 is exactly that manual pedal and above 0 hands the SAME resonant tank to
// an envelope follower (Mu-Tron-style auto-wah); VOICE is the documented "vocal
// mod" — it changes the resonance's WIDTH only, not its centre. Docs §58.
// M13.4 adds 'delay' (the "Echoman" — the lineup's FIRST DELAY, and a whole new
// DSP family): a Deluxe Memory Man-class BUCKET-BRIGADE analog delay. SAME
// three-knob shape/ABI, its slots reading as DELAY / FEEDBACK / BLEND
// (distortion==Delay, filter==Feedback, level==Blend). The delay time changes
// because the BBD's CLOCK changes while its filters stay put, which is why the
// repeats get darker the longer you set it; the repeats also degrade cumulatively,
// because each one goes back through the whole device. BLEND 0 is bit-identical
// to bypass. Docs §60.
// M13.6a adds 'gate' (the "Curfew" noise gate — the lineup's first UTILITY, and
// the pedal that makes a high-gain rig playable). SAME three-slot shape/ABI, but
// only TWO of the slots are real knobs — slot 0 (distortion) is THRESHOLD and
// slot 2 (level) is DECAY. Slot 1 is carried and unused, exactly as the
// compressor carries it. THRESHOLD really IS a threshold: it moves the level at
// which the gate opens by 40 dB and moves the gain the pedal applies when open by
// 0.00 dB (docs §61.4) — the exact opposite of the compressor's SUSTAIN.
// M13.3 adds 'opto' (the "Lumen" OPTICAL compressor — the SECOND dynamics pedal,
// and the first whose slot 1 carries a real control). It is not the Squash with
// different constants: its time constants are a photocell's, so its release is
// two-stage and PROGRAM DEPENDENT (measured 2.892x longer after a long passage
// than after a stab at the same depth, against the OTA voice's 1.000x — docs
// §64.4), its attack does not move with the knob, and its PEAK REDUCTION moves a
// THRESHOLD by 45.44 dB where the Squash's SUSTAIN moves GAIN by 25.33 dB.
export type PedalType = 'rat' | 'sd1' | 'ts' | 'muff' | 'gold' | 'comp' | 'opto' | 'gate' | 'phaser' | 'wah' | 'chorus' | 'delay' | 'tuner';
// M9.4: the JCM800 2204 joins the Clean 120 as a selectable amp voice. 'clean120'
// is the JC-120-style linear clean platform (chorus/reverb/bright + volume live
// here); 'jcm800' is the Marshall JCM800 (a mono valve head: gain/master/bass/mid/
// treble/presence). M10.1: the 'twin' — a Fender blackface "Twin-style" combo (the
// CLEAN benchmark): volume/bass/middle/treble/bright + spring reverb + optical
// tremolo (reusing the speed/depth mod knobs as tremolo SPEED/INTENSITY). All three
// share the cab pair; all three now have a REVERB knob (the JCM's is a usability
// add — the real 2204 has none). No new params: the Twin reuses existing knobs.
// v1.1 adds 'ac30' — a Vox AC30 "top boost" style class-A combo (the CHIME/JANGLE
// voice): VOLUME is the overdrive (crank it for the class-A grind), bass/treble drive
// the top-boost stack, and the shared 'presence' param (id 11) is REUSED as the AC30's
// top CUT control. Reuses existing knobs (no new AmpParams).
// M10.3 adds 'orange' — an early-70s Orange OR120 "Overdrive" head (the
// MID-FORWARD voice): a James/passive-Baxandall stack with BASS + TREBLE and no
// mid, the six-position F.A.C. rotary (its own param, `fac`), NO master volume
// (VOLUME is the whole amp — the power section is the overdrive), and the shared
// 'presence' param (id 11) REUSED as its H.F. BOOST. Docs §57.
// M10.7 adds 'rockerverb' — the MODERN Orange (Rockerverb 100, dirty channel):
// FOUR gain stages behind a GANGED dual GAIN pot, a Marshall-lineage FMV tone
// stack (so it has a MIDDLE control, unlike the OR120), a real post-stack MASTER
// so drive and level are independent, and an authentic valve-driven spring
// reverb. No new params: gain (10), master (12) and bass/middle/treble (1/2/3)
// all mean what they already meant. Docs §63.
export type AmpType = 'clean120' | 'jcm800' | 'twin' | 'ac30' | 'orange' | 'rockerverb';

// Cab expansion: which speaker cabinet IR the amp runs. 'clean212' is the
// built-in Clean 2x12 (the JC-120 platform), 'brit412' the darker/thicker Brit
// 4x12 (pairs with a Marshall-style amp), and 'custom' a user-uploaded IR (its
// samples live in a SEPARATE localStorage key, never in the rig/preset JSON —
// see cab.ts; the rig only records the choice + a short label).
export type CabChoice = 'clean212' | 'brit412' | 'orange412' | 'custom';
// The two BUILT-IN cabs offered in the amp menu (and the ones the assistant may
// switch between). 'custom' is reached only via Upload IR, never listed here.
export const AVAILABLE_CABS: readonly ('clean212' | 'brit412' | 'orange412')[] = [
  'clean212',
  'brit412',
  'orange412',
];
// The worklet's built-in cab index (mirrors CabBuiltin in clipper_c_api.cpp).
export const CAB_BUILTIN_INDEX: Record<'clean212' | 'brit412' | 'orange412', number> = {
  clean212: 0,
  brit412: 1,
  orange412: 2,
};

// The pedal types that can be added from the gear tray (M6.4).
export const AVAILABLE_PEDAL_TYPES: readonly PedalType[] = ['rat', 'sd1', 'ts', 'muff', 'gold', 'comp', 'opto', 'gate', 'phaser', 'wah', 'chorus', 'delay', 'tuner'];
// The amp types that can be selected in the amp slot (M6.4 / M9.4 / M10.1): the
// Clean 120, the Marshall JCM800, and the Fender-blackface Twin.
export const AVAILABLE_AMP_TYPES: readonly AmpType[] = [
  'clean120',
  'jcm800',
  'twin',
  'ac30',
  'orange',
  'rockerverb',
];

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
  | 'chorusMode'
  | 'reverb'
  // M9.4 JCM800-only knobs (ignored by clean120).
  | 'gain'
  | 'presence'
  | 'master'
  // M10.3 Orange-only knob (ignored by every other voice).
  | 'fac';

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
// chorus/vibrato, M6.3). On the Twin the chorusMode slot is REUSED as the
// TREMOLO ON/OFF (0=off, 1=on — the Twin has no chorus; docs §20 amendment).
export interface AmpParams {
  volume: number;
  bass: number;
  middle: number;
  treble: number;
  bright: number; // 0/1 bright-switch toggle
  cab: number; // 0/1 cab-convolver enable (1 = cab on)
  speed: number; // 0..1 chorus/vibrato LFO rate knob
  depth: number; // 0..1 chorus/vibrato sweep-depth knob
  chorusMode: number; // 0 off | 1 chorus | 2 vibrato (Twin: 0 trem off | 1 trem on)
  reverb: number; // 0..1 spring-reverb wet/dry mix (M6.7; 0 = dry)
  // M9.4 JCM800 knobs (additive; clean120 ignores these, jcm800 ignores
  // volume/bright/speed/depth/chorusMode/reverb). bass/middle/treble are SHARED —
  // both amps use them. gain = preamp drive, master = power-amp drive (how hard
  // the phase inverter is pushed), presence = power-amp HF feedback lift.
  gain: number; // 0..1 JCM preamp GAIN (drive)
  presence: number; // 0..1 JCM power-amp presence
  master: number; // 0..1 JCM MASTER volume
  // M10.3 Orange OR120 F.A.C.: a 0..1 knob that the core rounds to one of SIX
  // detents. Its own param rather than a reused slot — see params.ts.
  fac: number;
}

export interface AmpState {
  type: AmpType;
  engaged: boolean; // true = amp powered (jewel lit); false = amp+cab bypassed
  // Cab expansion: which speaker cab IR is loaded (SEPARATE from the `cab` 0/1
  // ENABLE lever in params — that bypasses the convolver; this selects the IR).
  // 'custom' means a user IR whose samples are persisted under a separate
  // localStorage key; `customCabLabel` is a short human name for it (e.g. the
  // file name), shown in the UI and rig context. Presets/rig JSON never embed IR
  // data.
  cabModel: CabChoice;
  customCabLabel?: string;
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

// TS808 "Screamer" (v1.1) opening state. Same param slots (distortion==Drive,
// filter==Tone, level==Level): a moderate Drive, Tone at noon (transparent), and
// a slightly hotter Level than the SD-1 (0.75) — the Screamer's calling card is
// the mid-hump clean BOOST into a pushed amp, so it opens ready to push.
export const TS_KNOB_DEFAULTS: PedalParams = {
  distortion: 0.5,
  filter: 0.5,
  level: 0.75,
};

// Muff "Pi" (v1.1 item 4) opening state. Same param slots, reading as SUSTAIN /
// TONE / VOLUME: SUSTAIN 0.6 (plenty of the wall-of-sustain fuzz without pinning
// it), TONE 0.5 (the mid-scoop at noon), VOLUME 0.6 (a Muff is LOUD — leave the
// downstream limiter room). These are the shipped defaults the round-trip test pins.
export const MUFF_KNOB_DEFAULTS: PedalParams = {
  distortion: 0.6,
  filter: 0.5,
  level: 0.6,
};

// GOLD (v1.1 item 6) opening state. Same param slots, reading as GAIN / TREBLE /
// OUTPUT: GAIN 0.35 (the way the pedal is actually used — a mostly-clean blend with
// a little grit riding on top, the "always-on" setting), TREBLE 0.5 (flat — the tilt
// is exactly unity at noon), OUTPUT 0.7 (above the unity point at 0.5, so it opens
// as a modest boost, which is what this box is for).
export const GOLD_KNOB_DEFAULTS: PedalParams = {
  distortion: 0.35,
  filter: 0.5,
  level: 0.7,
};

// "Squash" compressor (M13.1) opening state. SUSTAIN 0.5 (mid travel — plenty of
// squash without the top of the knob's noise), LEVEL 0.4, which measures UNITY:
// a 0.15 V-peak 220 Hz note comes out at 0.157 V (+0.37 dB), so the pedal opens
// as a level-neutral always-on rather than a boost. Slot 1 (filter) is carried
// and unused. See docs §59.
export const COMP_KNOB_DEFAULTS: PedalParams = {
  distortion: 0.5,
  filter: 0.5,
  level: 0.4,
};

// Phaser ("Ninety", v1.1) opening state. ONE real knob: slot 0 (distortion) is
// SPEED — opens at a slow/medium ~0.35 (a classic gentle sweep, ~0.7 Hz on the
// log map). Slots 1/2 are carried but unused (fixed script-authentic depth, no
// feedback). See PhaserModel.
export const PHASER_KNOB_DEFAULTS: PedalParams = {
  distortion: 0.35,
  filter: 0.5,
  level: 0.5,
};

// Wah ("Weeper") opening state. POSITION 0.35 is a low-mid vowel (~816 Hz on the
// derived sweep law) — a musical resting place that is obviously a wah the moment
// you sweep it. SENSE 0 opens in MANUAL mode (the envelope contributes exactly
// nothing until you ask for it). VOICE 0.5 is the STOCK 33 kOhm R7. See WahModel.
export const WAH_KNOB_DEFAULTS: PedalParams = {
  distortion: 0.35,
  filter: 0.0,
  level: 0.5,
};

// Delay ("Echoman", M13.4) opening state. DELAY 0.35 puts the BBD clock at
// 19.9 kHz = 212 ms, a musical eighth-note-ish echo that is obviously an analog
// delay without being a wash. FEEDBACK 0.30 gives a few audible repeats that die
// away rather than a self-oscillating swamp. BLEND 0.35 keeps the dry in front —
// the repeats sit behind the note, which is how a Memory Man is normally used.
// See DelayModel / docs §60.
export const DELAY_KNOB_DEFAULTS: PedalParams = {
  distortion: 0.35,
  filter: 0.3,
  level: 0.35,
};

// CE-1 Chorus Ensemble (M13.7) opening state. RATE 0.35 lands at ~1.30 Hz in
// chorus mode — the slow, wide 1970s shimmer the pedal is famous for, and well
// inside its 1.0-3.0 Hz range. DEPTH 0.5 is the middle of the 3-6 ms delay
// window. MODE 0 is CHORUS; >= 0.5 selects VIBRATO (a DISCRETE two-state switch,
// matching the real pedal's footswitch). See Ce1Model / docs §62.
export const CHORUS_KNOB_DEFAULTS: PedalParams = {
  distortion: 0.35,
  filter: 0.5,
  level: 0.0,
};

// Per-type opening knob positions (gear tray "add" / swap use this).
// "Curfew" noise gate (M13.6a) opening state. THRESHOLD 0.35, which measures a
// −45.05 dBV open point: a realistic single-coil hiss floor (~−60 dBV) stays shut
// and a normally played note (0.15 V peak) opens it, so the pedal is useful the
// moment it lands on the board. DECAY 0.5 = a ~390 ms fade, close to the 300 ms
// the gate literature recommends as a starting release. Slot 1 (filter) is
// carried and unused. See docs §61.
export const GATE_KNOB_DEFAULTS: PedalParams = {
  distortion: 0.35,
  filter: 0.5,
  level: 0.5,
};

// "Lumen" optical compressor (M13.3) opening state. PEAK REDUCTION 0.50 puts the
// 3 dB point at -25.05 dBV, so a normally played note is levelled by about 5 dB
// without the pedal ever sounding like it is working. MODE 0 = COMPRESS (the
// gentler, 100 %-feed-back position; >= 0.5 is LIMIT, which mixes 1/25 of the
// input into the sidechain and clamps up to 7.71 dB harder). GAIN 0.62 measures
// UNITY: a 0.15 V-peak 220 Hz note comes out at -16.52 dBV against an input of
// -16.48 dBV (+0.04 dB). See docs §64.
export const OPTO_KNOB_DEFAULTS: PedalParams = {
  distortion: 0.5,
  filter: 0.0,
  level: 0.62,
};

export const PEDAL_KNOB_DEFAULTS: Record<PedalType, PedalParams> = {
  rat: KNOB_DEFAULTS,
  sd1: SD1_KNOB_DEFAULTS,
  ts: TS_KNOB_DEFAULTS,
  muff: MUFF_KNOB_DEFAULTS,
  gold: GOLD_KNOB_DEFAULTS,
  comp: COMP_KNOB_DEFAULTS,
  opto: OPTO_KNOB_DEFAULTS,
  gate: GATE_KNOB_DEFAULTS,
  phaser: PHASER_KNOB_DEFAULTS,
  wah: WAH_KNOB_DEFAULTS,
  chorus: CHORUS_KNOB_DEFAULTS,
  delay: DELAY_KNOB_DEFAULTS,
  // The tuner has no knobs; params are unused but keep the shared shape.
  tuner: KNOB_DEFAULTS,
};

// Amp defaults mirror the approved design's opening state: Vol 40, Bass/Mid 50
// (flat), Treble 60, bright off, cab on. Tone controls are flat at 0.5. Chorus
// ships OFF (mode 0) with a musical speed/depth ready for when it is engaged.
// Reverb ships at 0 (dry) — the JC-120's single REVERB knob all the way down (M6.7).
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
  reverb: 0,
  // M9.4 JCM800 defaults: a musical crunch opening — GAIN/PRESENCE at noon, MASTER
  // at 0.4 (matches the JCM's real "cranked but not slammed" full-send region).
  // bass/middle/treble default 0.5/0.5/0.6 above, shared with the clean amp.
  gain: 0.5,
  presence: 0.5,
  master: 0.4,
  // M10.3: F.A.C. position 2 of 6 (knob 0.2) — the fat, usable setting an OR120
  // spends most of its life on; clicking right thins it out.
  fac: 0.2,
};

// Default input trim: unity (0 dB).
export const INPUT_DEFAULTS: InputState = {
  trim: INPUT_TRIM_UNITY_KNOB,
};

// A fresh pedal instance of the given type, with that type's default knobs and a
// new unique id. The gear tray's "add pedal" and swap flows use this. A tuner has
// no params (they're carried but ignored) and starts DISENGAGED, so dropping one
// on the board doesn't silence the rig — you stomp it on to tune.
export function makePedal(type: PedalType = 'rat'): PedalInstance {
  return {
    id: newPedalId(type),
    type,
    engaged: type !== 'tuner',
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
    cabModel: 'clean212',
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
  // Known types round-trip; anything else coerces to the RAT.
  const type: PedalType =
    p.type === 'tuner' ? 'tuner'
    : p.type === 'sd1' ? 'sd1'
    : p.type === 'ts' ? 'ts'
    : p.type === 'muff' ? 'muff'
    : p.type === 'gold' ? 'gold'
    : p.type === 'comp' ? 'comp'
    : p.type === 'opto' ? 'opto'
    : p.type === 'gate' ? 'gate'
    : p.type === 'phaser' ? 'phaser'
    : p.type === 'wah' ? 'wah'
    : p.type === 'chorus' ? 'chorus'
    : p.type === 'delay' ? 'delay'
    : 'rat';
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

  // Cab selection migration (cab expansion): old rigs (no cabModel) -> the built-
  // in Clean 2x12. A rig that references 'custom' keeps that here; whether the
  // custom IR data actually EXISTS is resolved at load time (App falls back to
  // clean212 with a note if it's missing) — the rig JSON alone can't know.
  const cabModel: CabChoice =
    a.cabModel === 'brit412' || a.cabModel === 'orange412' || a.cabModel === 'custom'
      ? a.cabModel
      : 'clean212';
  const customCabLabel =
    typeof a.customCabLabel === 'string' ? a.customCabLabel : undefined;

  // Amp type migration (M9.4 / M10.1): known 'jcm800' / 'twin' round-trip; anything
  // else (incl. an old rig with no type) coerces to the Clean 120, so pre-M9.4 saved
  // rigs load unchanged.
  const ampType: AmpType =
    a.type === 'jcm800' ? 'jcm800'
    : a.type === 'twin' ? 'twin'
    : a.type === 'ac30' ? 'ac30'
    : a.type === 'orange' ? 'orange'
    : a.type === 'rockerverb' ? 'rockerverb'
    : 'clean120';

  return {
    input: { trim: clamp01(inp.trim, d.input.trim) },
    pedals,
    amp: {
      type: ampType,
      engaged: typeof a.engaged === 'boolean' ? a.engaged : d.amp.engaged,
      cabModel,
      ...(customCabLabel !== undefined ? { customCabLabel } : {}),
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
        // Migration seam: a pre-M6.7 rig has no reverb field — old saved rigs
        // load with reverb at 0 (dry).
        reverb: clamp01(ar.reverb, d.amp.params.reverb),
        // Migration seam: a pre-M9.4 rig has no JCM fields — old saved rigs load
        // with the JCM defaults (harmless: they only matter when type is jcm800).
        gain: clamp01(ar.gain, d.amp.params.gain),
        presence: clamp01(ar.presence, d.amp.params.presence),
        master: clamp01(ar.master, d.amp.params.master),
        fac: clamp01(ar.fac, d.amp.params.fac),
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
    amp: { type: 'clean120', engaged: true, cabModel: 'clean212', params: { ...AMP_KNOB_DEFAULTS } },
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
