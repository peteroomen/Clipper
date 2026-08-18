// Pedal (RAT) parameter ids — must mirror clipper::dsp::RatModel::ParamId in
// core/include/clipper/dsp/RatModel.h and the worklet.
export const PARAM_DISTORTION = 0;
export const PARAM_FILTER = 1;
export const PARAM_LEVEL = 2;

// Amp parameter ids — must mirror clipper::dsp::AmpModel::ParamId in
// core/include/clipper/dsp/AmpModel.h, plus the chain-level cab toggle
// (AMP_PARAM_CAB == PARAM_COUNT == 5, handled by the C ABI wrapper).
export const AMP_PARAM_VOLUME = 0;
export const AMP_PARAM_BASS = 1;
export const AMP_PARAM_MIDDLE = 2;
export const AMP_PARAM_TREBLE = 3;
export const AMP_PARAM_BRIGHT = 4;
export const AMP_PARAM_CAB = 5;
// M6.3 chorus/vibrato ids (6/7/8), placed ABOVE the cab id so cab stays 5.
// speed/depth are 0..1 knobs; mode is an integer 0=off / 1=chorus / 2=vibrato.
export const AMP_PARAM_CHORUS_SPEED = 6;
export const AMP_PARAM_CHORUS_DEPTH = 7;
export const AMP_PARAM_CHORUS_MODE = 8;
// M6.7 spring reverb id (9), appended additively. 0..1 equal-power wet MIX
// (0 = dry). Routed into the owned ReverbModel by the C ABI, in the JC-120
// spring-tank position (preamp -> reverb -> chorus split).
export const AMP_PARAM_REVERB = 9;
// M9.4 JCM800 amp-specific ids (10/11/12), placed ABOVE every clean-120 id so the
// ABI stays purely additive. GAIN/PRESENCE/MASTER are JCM-only; the JCM's
// BASS/MIDDLE/TREBLE REUSE the clean-120 tone ids (1/2/3). The C ABI routes these
// to the active amp model (clean120 ignores 10/11/12; jcm800 ignores volume/bright/
// chorus/reverb). Must mirror kAmpParamJcm* in core/src/clipper_c_api.cpp.
export const AMP_PARAM_JCM_GAIN = 10;
export const AMP_PARAM_JCM_PRESENCE = 11;
export const AMP_PARAM_JCM_MASTER = 12;
// M10.3 Orange OR120: the F.A.C. six-position rotary (13). A NEW id rather than a
// reused knob slot, because no other voice has a switch there and a stale rig
// state must never silently mean something else. Everything else the OR120 needs
// is a documented reuse: volume (0), bass (1), treble (3), presence (11) as its
// H.F. BOOST, reverb (9). Must mirror kAmpParamOrangeFac in clipper_c_api.cpp.
export const AMP_PARAM_ORANGE_FAC = 13;
// M10.4 Mesa Dual Rectifier: THREE new param ids (14/15/16), for controls no
// other voice has. Same reasoning as the F.A.C.: reusing a knob slot would make a
// stale rig state silently mean something else.
//   MODE (14)      — the drawing's FIVE states, not a channel plus a mode. Sheet
//                    `mbdr7` enumerates the combinations that exist, and two
//                    conceivable ones (RED CLEAN, ORANGE VINTAGE) do not.
//   RECTIFIER (15) — silicon vs 5U4, the amp's signature switch.
//   POWERMODE (16) — SPONGY vs BOLD. A SEPARATE mains-primary-side switch,
//                    commonly confused with the rectifier selector.
// Everything else is a documented reuse: gain (10), master (12), bass/mid/treble
// (1/2/3 — this amp has a mid on both channels), presence (11), reverb (9).
// Must mirror kAmpParamMesa* in clipper_c_api.cpp.
export const AMP_PARAM_MESA_MODE = 14;
export const AMP_PARAM_MESA_RECTIFIER = 15;
export const AMP_PARAM_MESA_POWERMODE = 16;

// The five MODE positions, in the drawing's own order, as 0..1 knob values. The
// ABI quantizes with round(v * 4), so these are the exact centres.
export const MESA_MODES = [
  { id: 'orangeClean', label: 'Clean', value: 0.0 },
  { id: 'orangeNormal', label: 'Vintage', value: 0.25 },
  { id: 'orangeModern', label: 'Modern', value: 0.5 },
  { id: 'redVintage', label: 'Red Vintage', value: 0.75 },
  { id: 'redModern', label: 'Red Modern', value: 1.0 },
] as const;

// M10.7 Orange Rockerverb 100: NO new param id, deliberately. Its GAIN and its
// post-tone-stack VOLUME mean exactly what the JCM800's GAIN (10) and MASTER (12)
// mean to a player, and its BASS/MIDDLE/TREBLE are the shared tone ids (1/2/3) —
// it is the first Orange in this repo with a mid control. The panel WORD for slot
// 12 on this amp is "Volume"; the SLOT is the master slot because the FUNCTION is
// a master. It has no presence control at all, so id 11 never reaches it.

// The worklet's amp-model index (mirrors AmpModelId in clipper_c_api.cpp). M10.1
// adds the Twin as voice 2 (purely additive; clean120/jcm800 indices unchanged).
// v1.1 adds the AC30 "top boost" as voice 3 (additive; reuses the STABLE amp_*
// exports and the shared presence id 11 routed as its top CUT).
// M10.3 adds the Orange OR120 as voice 4 (additive; clean120/jcm800/twin/ac30
// indices unchanged).
// M10.7 adds the Orange Rockerverb 100 as voice 5 (additive; every existing index
// unchanged).
export const AMP_MODEL_INDEX: Record<
  'clean120' | 'jcm800' | 'twin' | 'ac30' | 'orange' | 'rockerverb' | 'mesa',
  number
> = {
  clean120: 0,
  jcm800: 1,
  twin: 2,
  ac30: 3,
  orange: 4,
  rockerverb: 5,
  mesa: 6,
};

// Chorus mode enum (mirrors ChorusModel::Mode). Kept as plain numbers so it flows
// through the numeric param ABI untouched.
export const CHORUS_OFF = 0;
export const CHORUS_CHORUS = 1;
export const CHORUS_VIBRATO = 2;

// Map a rig pedal param name to its worklet/core id. Keeps the RigState shape
// (named params) decoupled from the numeric ABI the worklet speaks.
export const PARAM_ID = {
  distortion: PARAM_DISTORTION,
  filter: PARAM_FILTER,
  level: PARAM_LEVEL,
} as const;

// Map a rig amp param name to its worklet/core id.
export const AMP_PARAM_ID = {
  volume: AMP_PARAM_VOLUME,
  bass: AMP_PARAM_BASS,
  middle: AMP_PARAM_MIDDLE,
  treble: AMP_PARAM_TREBLE,
  bright: AMP_PARAM_BRIGHT,
  cab: AMP_PARAM_CAB,
  speed: AMP_PARAM_CHORUS_SPEED,
  depth: AMP_PARAM_CHORUS_DEPTH,
  chorusMode: AMP_PARAM_CHORUS_MODE,
  reverb: AMP_PARAM_REVERB,
  // M9.4 JCM800 knobs.
  gain: AMP_PARAM_JCM_GAIN,
  presence: AMP_PARAM_JCM_PRESENCE,
  master: AMP_PARAM_JCM_MASTER,
  // M10.3 Orange OR120.
  fac: AMP_PARAM_ORANGE_FAC,
  // M10.4 Mesa Dual Rectifier.
  mesaMode: AMP_PARAM_MESA_MODE,
  rectifier: AMP_PARAM_MESA_RECTIFIER,
  powerMode: AMP_PARAM_MESA_POWERMODE,
} as const;

// Valid oversampling factors for the nonlinear stage (default 4x). Other values
// snap down in the core; the UI offers exactly these.
export const OVERSAMPLING_FACTORS = [1, 2, 4, 8] as const;

// --- Input trim (M6.1) --------------------------------------------------------
// A rig-level input gain applied in the worklet BEFORE the pedal chain, so a
// real-world interface signal (guitar DIs often arrive at 0.01..0.05, far below
// the model's 1.0 == 1 V diode reference) can be lifted to actually drive the
// clipper. Stored in RigState as a 0..1 knob position; mapped to a dB trim over
// [INPUT_TRIM_MIN_DB, INPUT_TRIM_MAX_DB]. The knob position for 0 dB (the
// default) is INPUT_TRIM_UNITY_KNOB.
export const INPUT_TRIM_MIN_DB = -12;
export const INPUT_TRIM_MAX_DB = 24;
export const INPUT_TRIM_UNITY_KNOB =
  (0 - INPUT_TRIM_MIN_DB) / (INPUT_TRIM_MAX_DB - INPUT_TRIM_MIN_DB); // = 1/3

// 0..1 knob position -> dB trim. NaN-rejecting (2026-07-24 audit, finding 1):
// Math.min/Math.max pass NaN through, and this feeds trimKnobToGain, i.e. the
// worklet's input multiply and the trim readout.
export function trimKnobToDb(knob: number): number {
  const k = Math.min(1, Math.max(0, Number.isFinite(knob) ? knob : 0));
  return INPUT_TRIM_MIN_DB + (INPUT_TRIM_MAX_DB - INPUT_TRIM_MIN_DB) * k;
}

// 0..1 knob position -> linear gain (what the worklet multiplies input by).
export function trimKnobToGain(knob: number): number {
  return Math.pow(10, trimKnobToDb(knob) / 20);
}

// Absolute URL of the copied worklet module (served from public/generated).
export const WORKLET_URL = '/generated/clipper-processor.js';
