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

// Absolute URL of the copied worklet module (served from public/generated).
export const WORKLET_URL = '/generated/clipper-processor.js';
