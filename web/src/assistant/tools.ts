// The assistant's hands (M6). A small, typed tool surface the coach calls to
// change the rig. Each tool operates on the SAME rig-state setters the App's
// knobs/switches use (threaded in as a RigController), so when the AI acts the
// knobs visibly move and the change flows to the worklet + localStorage.
//
// Tools (exactly these):
//   set_param   {unit: pedal|amp, param, value 0..1}  — clamps
//   set_engaged {unit: pedal|amp, engaged: bool}
//   set_switch  {name: bright|cab, on: bool}

import type { RigState } from '../rig';

export type Unit = 'pedal' | 'amp' | 'input';
export type SwitchName = 'bright' | 'cab';

// The seam between the assistant and the live rig. App implements this over its
// existing setters; the tool executor only ever touches the rig through here.
export interface RigController {
  getRig: () => RigState;
  // Current post-trim input peak in dBFS (null when not running), for the coach
  // to help calibrate the input trim.
  getPeakDbFs?: () => number | null;
  // Apply a normalized param; returns the clamped value actually applied.
  setParam: (unit: Unit, param: string, value: number) => number;
  setEngaged: (unit: Unit, engaged: boolean) => void;
  setSwitch: (name: SwitchName, on: boolean) => void;
}

// JSON-schema tool definitions sent to the API (name/description/input_schema).
// Kept deterministic (stable order, additionalProperties:false) so they cache.
export const TOOLS = [
  {
    name: 'set_param',
    description:
      "Set a continuous knob on the rig to an absolute normalized value (0..1). " +
      "Pedal params: 'dist' (0=clean, 1=max saturation), 'filter' (0=bright, " +
      "1=dark — the RAT filter is a post-clipping low-pass, so clockwise/higher " +
      "tames fizz without changing how hard it clips), 'level' (output volume). " +
      "Amp params: 'volume', 'bass', 'middle', 'treble' (tone controls are flat " +
      "at 0.5). Input params: 'trim' — the rig-level INPUT gain BEFORE the pedal " +
      "(0..1 maps to -12..+24 dB, 1/3 = 0 dB). Raise it when the input peak is " +
      "weak (below ~-12 dBFS) so the guitar actually drives the diodes; lower it " +
      "if it's hot (near 0 dBFS). The knob moves visibly and the change is heard " +
      'immediately.',
    input_schema: {
      type: 'object',
      properties: {
        unit: { type: 'string', enum: ['pedal', 'amp', 'input'] },
        param: {
          type: 'string',
          enum: ['dist', 'filter', 'level', 'volume', 'bass', 'middle', 'treble', 'trim'],
        },
        value: { type: 'number', minimum: 0, maximum: 1 },
      },
      required: ['unit', 'param', 'value'],
      additionalProperties: false,
    },
  },
  {
    name: 'set_engaged',
    description:
      'Engage or bypass a unit. For the pedal, engaged=false is true bypass ' +
      '(clean signal, LED dark). For the amp, engaged=false powers the amp+cab ' +
      'off (signal passes straight through).',
    input_schema: {
      type: 'object',
      properties: {
        unit: { type: 'string', enum: ['pedal', 'amp'] },
        engaged: { type: 'boolean' },
      },
      required: ['unit', 'engaged'],
      additionalProperties: false,
    },
  },
  {
    name: 'set_switch',
    description:
      "Flip an amp switch. 'bright' adds a high-frequency shelf (extra sparkle/" +
      "presence). 'cab' toggles the 2x12 speaker cabinet simulation (off = raw, " +
      'fizzier, no speaker rolloff).',
    input_schema: {
      type: 'object',
      properties: {
        name: { type: 'string', enum: ['bright', 'cab'] },
        on: { type: 'boolean' },
      },
      required: ['name', 'on'],
      additionalProperties: false,
    },
  },
] as const;

// Maps the assistant's short param name to the RigState param name the setters
// expect. Pedal 'dist' -> 'distortion'; everything else is 1:1.
const PEDAL_PARAM: Record<string, string> = {
  dist: 'distortion',
  filter: 'filter',
  level: 'level',
};
const AMP_PARAM: Record<string, string> = {
  volume: 'volume',
  bass: 'bass',
  middle: 'middle',
  treble: 'treble',
};
const INPUT_PARAM: Record<string, string> = {
  trim: 'trim',
};

// A short, human-readable label for a param (for the in-flow chips).
const PARAM_LABEL: Record<string, string> = {
  dist: 'Dist',
  filter: 'Filter',
  level: 'Level',
  volume: 'Vol',
  bass: 'Bass',
  middle: 'Mid',
  treble: 'Treble',
  trim: 'Trim',
};

const clamp01 = (n: number) => Math.min(1, Math.max(0, typeof n === 'number' ? n : 0));
const to100 = (n: number) => Math.round(clamp01(n) * 100);

export interface ToolExecution {
  // The tool_result content string returned to the model (a short applied JSON).
  content: string;
  // A compact chip label rendered in the chat flow, e.g. "Dist 70 → 55".
  chip: string;
}

// Execute one tool_use locally against the live rig. Reads current state to
// compute the "from" value for the chip, applies via the controller, and
// returns both the model-facing tool_result content and a UI chip label.
export function executeTool(
  controller: RigController,
  name: string,
  input: Record<string, unknown>
): ToolExecution {
  if (name === 'set_param') {
    const unit: Unit =
      input.unit === 'amp' ? 'amp' : input.unit === 'input' ? 'input' : 'pedal';
    const param = String(input.param ?? '');
    const target = clamp01(Number(input.value));
    const rig = controller.getRig();
    const map = unit === 'pedal' ? PEDAL_PARAM : unit === 'amp' ? AMP_PARAM : INPUT_PARAM;
    const rigParam = map[param];
    if (!rigParam) {
      return {
        content: JSON.stringify({ error: `unknown ${unit} param '${param}'` }),
        chip: `? ${param}`,
      };
    }
    const params =
      unit === 'pedal' ? rig.pedal.params : unit === 'amp' ? rig.amp.params : rig.input;
    const from = to100((params as unknown as Record<string, number>)[rigParam] ?? 0);
    const applied = controller.setParam(unit, rigParam, target);
    const to = to100(applied);
    return {
      content: JSON.stringify({ applied: { unit, param, value: applied } }),
      chip: `${PARAM_LABEL[param] ?? param} ${from} → ${to}`,
    };
  }

  if (name === 'set_engaged') {
    const unit = input.unit === 'amp' ? 'amp' : 'pedal';
    const engaged = Boolean(input.engaged);
    controller.setEngaged(unit, engaged);
    const label = unit === 'pedal' ? 'Pedal' : 'Amp';
    const state =
      unit === 'pedal' ? (engaged ? 'engaged' : 'bypassed') : engaged ? 'on' : 'off';
    return {
      content: JSON.stringify({ applied: { unit, engaged } }),
      chip: `${label} ${state}`,
    };
  }

  if (name === 'set_switch') {
    const sw = input.name === 'cab' ? 'cab' : 'bright';
    const on = Boolean(input.on);
    controller.setSwitch(sw, on);
    const label = sw === 'cab' ? 'Cab' : 'Bright';
    return {
      content: JSON.stringify({ applied: { name: sw, on } }),
      chip: `${label} ${on ? 'on' : 'off'}`,
    };
  }

  return { content: JSON.stringify({ error: `unknown tool '${name}'` }), chip: '? tool' };
}
