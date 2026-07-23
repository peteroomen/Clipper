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

// 0..1 knob position -> dB trim.
export function trimKnobToDb(knob: number): number {
  const k = Math.min(1, Math.max(0, knob));
  return INPUT_TRIM_MIN_DB + (INPUT_TRIM_MAX_DB - INPUT_TRIM_MIN_DB) * k;
}

// 0..1 knob position -> linear gain (what the worklet multiplies input by).
export function trimKnobToGain(knob: number): number {
  return Math.pow(10, trimKnobToDb(knob) / 20);
}

// Absolute URL of the copied worklet module (served from public/generated).
export const WORKLET_URL = '/generated/clipper-processor.js';
