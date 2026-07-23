// Parameter ids — must mirror clipper::dsp::RatModel::ParamId in
// core/include/clipper/dsp/RatModel.h and the worklet's PARAM_* constants.
export const PARAM_DISTORTION = 0;
export const PARAM_FILTER = 1;
export const PARAM_LEVEL = 2;

// Map a rig param name to its worklet/core id. Keeps the RigState shape
// (named params) decoupled from the numeric ABI the worklet speaks.
export const PARAM_ID = {
  distortion: PARAM_DISTORTION,
  filter: PARAM_FILTER,
  level: PARAM_LEVEL,
} as const;

// Valid oversampling factors for the nonlinear stage (default 4x). Other values
// snap down in the core; the UI offers exactly these.
export const OVERSAMPLING_FACTORS = [1, 2, 4, 8] as const;

// Absolute URL of the copied worklet module (served from public/generated).
export const WORKLET_URL = '/generated/clipper-processor.js';
