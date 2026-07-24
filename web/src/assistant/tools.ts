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
import type { TunerReading } from '../tuner';

export type Unit = 'pedal' | 'amp' | 'input';
export type SwitchName = 'bright' | 'cab' | 'chorus' | 'vibrato';

// The seam between the assistant and the live rig. App implements this over its
// existing setters; the tool executor only ever touches the rig through here.
// M6.4: pedal ops address a pedal INSTANCE by its 0-based chain index, and the
// chain can be edited (add/remove/move).
export interface RigController {
  getRig: () => RigState;
  // Current post-trim input peak in dBFS (null when not running), for the coach
  // to help calibrate the input trim.
  getPeakDbFs?: () => number | null;
  // Latest tuner reading (M7), null when no tuner is engaged / no clear pitch, so
  // the coach can say "you're a few cents flat on the G".
  getTunerReading?: () => TunerReading | null;
  // Apply a normalized param; returns the clamped value actually applied.
  // pedalIndex targets a specific pedal instance (default 0) when unit==='pedal'.
  setParam: (unit: Unit, param: string, value: number, pedalIndex?: number) => number;
  setEngaged: (unit: Unit, engaged: boolean, pedalIndex?: number) => void;
  setSwitch: (name: SwitchName, on: boolean) => void;
  // Cab expansion: switch between the BUILT-IN cabs ('clean212' | 'brit412').
  // Never selects 'custom' (that needs a user file upload).
  setCab: (cab: 'clean212' | 'brit412') => void;
  // M9.4: swap the amp voice ('clean120' | 'jcm800').
  setAmp: (type: 'clean120' | 'jcm800') => void;
  // Chain edits (M6.4). addPedal returns the new instance's chain index.
  addPedal: (type: string, position?: number) => number;
  removePedal: (index: number) => void;
  movePedal: (from: number, to: number) => void;
}

// JSON-schema tool definitions sent to the API (name/description/input_schema).
// Kept deterministic (stable order, additionalProperties:false) so they cache.
export const TOOLS = [
  {
    name: 'set_param',
    description:
      "Set a continuous knob on the rig to an absolute normalized value (0..1). " +
      "RAT pedal params: 'dist' (0=clean, 1=max saturation), 'filter' (0=bright, " +
      "1=dark — the RAT filter is a post-clipping low-pass, so clockwise/higher " +
      "tames fizz without changing how hard it clips), 'level' (output volume). " +
      "SD-1 pedal params: 'drive' (0..1 overdrive amount), 'tone' (0=dark .. 1=bright, " +
      "0.5=flat — a treble tilt), 'level'. (For an SD-1 you may use 'dist' as an " +
      "alias for 'drive' and 'filter' for 'tone' — same slots.) " +
      "Phaser pedal param: 'speed' (its ONE knob — the LFO sweep rate; low = a " +
      "slow tape-warble, high = a fast Leslie-ish swirl). " +
      "Amp params: 'volume', 'bass', 'middle', 'treble' (tone controls are flat " +
      "at 0.5), plus the chorus/vibrato modulation 'speed' (LFO rate ~0.15-8 Hz) " +
      "and 'depth' (sweep amount / how deep the pitch wobble is) — these only do " +
      "something when the chorus or vibrato mode is on (set that with set_switch). " +
      "'reverb' is the JC-120 spring reverb MIX (0 = fully dry, 1 = drenched); the " +
      "decay is fixed (~1.5 s, spring-flavored) so this one knob is just how much " +
      "wet you blend in. Keep it low (10-30) for clarity and note definition, " +
      "higher for ballads/ambient washes. " +
      "JCM800 amp params (only when the JCM is the active amp — see set_amp): " +
      "'gain' (preamp drive — how much dirt/saturation the front end makes), " +
      "'master' (the power-amp drive — pushes the phase inverter / EL34s into " +
      "power-tube compression and grit; the loud, cranked-Marshall character lives " +
      "here, so gain vs master is preamp-dirt vs power-amp-feel), 'presence' (a " +
      "power-amp HIGH-frequency lift via the feedback loop — sharpens attack/edge " +
      "on top; distinct from 'treble', which shapes the preamp tone stack BEFORE " +
      "the distortion, while presence brightens the finished power-amp sound AFTER " +
      "it). The JCM also uses 'bass'/'middle'/'treble' (its Marshall tone stack) but " +
      "IGNORES volume/speed/depth/reverb (a real 2204 has no chorus, reverb, or " +
      "separate volume). " +
      "Input params: 'trim' — the rig-level INPUT gain BEFORE the pedal " +
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
          enum: [
            'dist',
            'drive',
            'filter',
            'tone',
            'level',
            'volume',
            'bass',
            'middle',
            'treble',
            'speed',
            'depth',
            'reverb',
            'gain',
            'presence',
            'master',
            'trim',
          ],
        },
        value: { type: 'number', minimum: 0, maximum: 1 },
        pedal: {
          type: 'integer',
          minimum: 0,
          description:
            "Which pedal INSTANCE (0-based position in the chain) when unit is 'pedal'. " +
            'Defaults to 0 (the first pedal). Ignored for amp/input.',
        },
      },
      required: ['unit', 'param', 'value'],
      additionalProperties: false,
    },
  },
  {
    name: 'set_engaged',
    description:
      'Engage or bypass a unit. For a pedal, engaged=false is true bypass ' +
      '(clean signal, LED dark) — address a specific pedal instance with `pedal`. ' +
      'For the amp, engaged=false powers the amp+cab off (signal passes straight ' +
      'through).',
    input_schema: {
      type: 'object',
      properties: {
        unit: { type: 'string', enum: ['pedal', 'amp'] },
        engaged: { type: 'boolean' },
        pedal: {
          type: 'integer',
          minimum: 0,
          description: "Which pedal instance (0-based) when unit is 'pedal'. Defaults to 0.",
        },
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
      "fizzier, no speaker rolloff). 'chorus' and 'vibrato' select the JC-120's " +
      "modulation: turning 'chorus' on gives the lush dry-left / wet-right stereo " +
      "bloom, 'vibrato' on gives a true pitch wobble on both sides; these two are " +
      "mutually exclusive (turning either off returns to no modulation). Use " +
      "set_param 'speed'/'depth' to shape the movement once one is on.",
    input_schema: {
      type: 'object',
      properties: {
        name: { type: 'string', enum: ['bright', 'cab', 'chorus', 'vibrato'] },
        on: { type: 'boolean' },
      },
      required: ['name', 'on'],
      additionalProperties: false,
    },
  },
  {
    name: 'set_cab',
    description:
      "Choose the speaker CABINET. Two built-ins: 'clean212' — the Clean 2×12, " +
      'the flat, open clean platform (pairs with the JC-120 amp); and ' +
      "'brit412' — a Marshall-style 4×12, thicker in the low-mids and noticeably " +
      'DARKER on top (a greenback-ish voicing), the classic rock/JCM cab. Reach ' +
      "for brit412 when the player wants a thicker, darker, rock voicing, and " +
      'clean212 for the pristine clean platform. This selects WHICH cab; the ' +
      "'cab' switch (set_switch) still bypasses the cab entirely. You cannot pick " +
      'a user-uploaded custom IR — the player loads that themselves from the amp menu.',
    input_schema: {
      type: 'object',
      properties: {
        cab: { type: 'string', enum: ['clean212', 'brit412'] },
      },
      required: ['cab'],
      additionalProperties: false,
    },
  },
  {
    name: 'set_amp',
    description:
      'Choose the AMP head. Two voices: ' +
      "'clean120' — the JC-120-style solid-state CLEAN platform (linear; all the " +
      'dirt comes from the pedals in front; has the bright switch, stereo chorus/' +
      'vibrato, and spring reverb). ' +
      "'jcm800' — a Marshall JCM800 2204: a real VALVE head with its own preamp " +
      'distortion (4× 12AX7 cascade) and cranked EL34 power-amp grit. It is a MONO ' +
      'head with GAIN + MASTER + a Marshall bass/mid/treble tone stack + PRESENCE, ' +
      'and NO chorus/reverb/bright/volume. Reach for the JCM when the player wants ' +
      'amp distortion / classic rock crunch rather than a pristine clean pedal ' +
      'platform. The canonical move is an SD-1 boosting a cranked JCM. Pairs best ' +
      'with the Brit 4×12 cab. Switching is click-free; the cab and pedals carry over.',
    input_schema: {
      type: 'object',
      properties: {
        type: { type: 'string', enum: ['clean120', 'jcm800'] },
      },
      required: ['type'],
      additionalProperties: false,
    },
  },
  {
    name: 'add_pedal',
    description:
      'Add a pedal to the chain. Signal runs guitar -> pedals in order -> amp, so ' +
      'ORDER matters: a dirt box earlier vs later in the chain hits the amp ' +
      "differently. Types: 'rat' (hard, aggressive, symmetric clipping — the " +
      "scooped, cutting RAT), 'sd1' (a Boss SD-1: soft, warm, asymmetric " +
      'overdrive with a mid-hump — the classic transparent boost, great in front ' +
      "of another dirt or a cranked amp), 'phaser' (a script-era 4-stage phaser — " +
      'the classic swirling/whooshing modulation with ONE knob, SPEED: placed ' +
      'AFTER the dirt it gives the vocal EVH swoosh, before the dirt it is subtler ' +
      'and blends into the note; slow = a gentle tape-warble, fast = a Leslie-ish ' +
      "rotary shimmer), and 'tuner' (a chromatic tuner — no " +
      'tone, but when stomped ON it MUTES the rig so the player can tune in ' +
      'silence; put it first in the chain by convention). Omit `position` to ' +
      'append at the end (just before the amp), or give a 0-based slot to insert.',
    input_schema: {
      type: 'object',
      properties: {
        type: { type: 'string', enum: ['rat', 'sd1', 'phaser', 'tuner'] },
        position: { type: 'integer', minimum: 0 },
      },
      required: ['type'],
      additionalProperties: false,
    },
  },
  {
    name: 'remove_pedal',
    description: 'Remove the pedal instance at the given 0-based chain index.',
    input_schema: {
      type: 'object',
      properties: { index: { type: 'integer', minimum: 0 } },
      required: ['index'],
      additionalProperties: false,
    },
  },
  {
    name: 'move_pedal',
    description:
      'Reorder the chain: move the pedal at 0-based index `from` to index `to`. ' +
      'Use this to change how pedals stack (e.g. put a booster before or after ' +
      'the dirt).',
    input_schema: {
      type: 'object',
      properties: {
        from: { type: 'integer', minimum: 0 },
        to: { type: 'integer', minimum: 0 },
      },
      required: ['from', 'to'],
      additionalProperties: false,
    },
  },
] as const;

// Maps the assistant's short param name to the RigState param name the setters
// expect. Pedal 'dist' -> 'distortion'; everything else is 1:1.
const PEDAL_PARAM: Record<string, string> = {
  dist: 'distortion',
  drive: 'distortion', // SD-1 Drive shares the distortion slot (id 0)
  speed: 'distortion', // phaser SPEED is the one knob — also slot 0
  filter: 'filter',
  tone: 'filter', // SD-1 Tone shares the filter slot (id 1)
  level: 'level',
};
const AMP_PARAM: Record<string, string> = {
  volume: 'volume',
  bass: 'bass',
  middle: 'middle',
  treble: 'treble',
  speed: 'speed',
  depth: 'depth',
  reverb: 'reverb',
  // M9.4 JCM800 knobs (1:1).
  gain: 'gain',
  presence: 'presence',
  master: 'master',
};
const INPUT_PARAM: Record<string, string> = {
  trim: 'trim',
};

// A short, human-readable label for a param (for the in-flow chips).
const PARAM_LABEL: Record<string, string> = {
  dist: 'Dist',
  drive: 'Drive',
  filter: 'Filter',
  tone: 'Tone',
  level: 'Level',
  volume: 'Vol',
  bass: 'Bass',
  middle: 'Mid',
  treble: 'Treble',
  speed: 'Speed',
  depth: 'Depth',
  reverb: 'Reverb',
  gain: 'Gain',
  presence: 'Presence',
  master: 'Master',
  trim: 'Trim',
};

const clamp01 = (n: number) => Math.min(1, Math.max(0, typeof n === 'number' ? n : 0));
const to100 = (n: number) => Math.round(clamp01(n) * 100);

// Resolve a tool's optional `pedal` (0-based instance index) to a valid chain
// index, clamped to [0, count-1] (0 when the chain is empty).
function pedalIndexOf(input: Record<string, unknown>, count: number): number {
  const raw = input.pedal;
  const i = typeof raw === 'number' && Number.isFinite(raw) ? raw | 0 : 0;
  if (count <= 0) return 0;
  return Math.min(count - 1, Math.max(0, i));
}

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
    const pedalIndex = pedalIndexOf(input, rig.pedals.length);
    let params: Record<string, number>;
    if (unit === 'pedal') {
      const inst = rig.pedals[pedalIndex];
      if (!inst) {
        return {
          content: JSON.stringify({ error: `no pedal at index ${pedalIndex}` }),
          chip: `? pedal ${pedalIndex + 1}`,
        };
      }
      params = inst.params as unknown as Record<string, number>;
    } else if (unit === 'amp') {
      params = rig.amp.params as unknown as Record<string, number>;
    } else {
      params = rig.input as unknown as Record<string, number>;
    }
    const from = to100(params[rigParam] ?? 0);
    const applied = controller.setParam(unit, rigParam, target, pedalIndex);
    const to = to100(applied);
    // Tag the chip with the pedal position when there is more than one pedal.
    const posTag = unit === 'pedal' && rig.pedals.length > 1 ? ` #${pedalIndex + 1}` : '';
    return {
      content: JSON.stringify({ applied: { unit, param, value: applied, pedal: unit === 'pedal' ? pedalIndex : undefined } }),
      chip: `${PARAM_LABEL[param] ?? param}${posTag} ${from} → ${to}`,
    };
  }

  if (name === 'set_engaged') {
    const unit = input.unit === 'amp' ? 'amp' : 'pedal';
    const engaged = Boolean(input.engaged);
    const rig = controller.getRig();
    const pedalIndex = pedalIndexOf(input, rig.pedals.length);
    controller.setEngaged(unit, engaged, pedalIndex);
    const posTag = unit === 'pedal' && rig.pedals.length > 1 ? ` #${pedalIndex + 1}` : '';
    const label = unit === 'pedal' ? `Pedal${posTag}` : 'Amp';
    const state =
      unit === 'pedal' ? (engaged ? 'engaged' : 'bypassed') : engaged ? 'on' : 'off';
    return {
      content: JSON.stringify({ applied: { unit, engaged, pedal: unit === 'pedal' ? pedalIndex : undefined } }),
      chip: `${label} ${state}`,
    };
  }

  if (name === 'add_pedal') {
    const type: 'rat' | 'sd1' | 'phaser' | 'tuner' =
      input.type === 'tuner' ? 'tuner'
      : input.type === 'sd1' ? 'sd1'
      : input.type === 'phaser' ? 'phaser'
      : 'rat';
    const rawPos = input.position;
    const position =
      typeof rawPos === 'number' && Number.isFinite(rawPos) ? Math.max(0, rawPos | 0) : undefined;
    const index = controller.addPedal(type, position);
    const label =
      type === 'tuner' ? 'Tuner' : type === 'sd1' ? 'SD-1' : type === 'phaser' ? 'Phaser' : 'RAT';
    return {
      content: JSON.stringify({ applied: { added: type, index } }),
      chip: `+ ${label} #${index + 1}`,
    };
  }

  if (name === 'remove_pedal') {
    const rig = controller.getRig();
    const index = Math.max(0, Number(input.index) | 0);
    if (index >= rig.pedals.length) {
      return {
        content: JSON.stringify({ error: `no pedal at index ${index}` }),
        chip: `? remove ${index + 1}`,
      };
    }
    controller.removePedal(index);
    return {
      content: JSON.stringify({ applied: { removed: index } }),
      chip: `− Pedal #${index + 1}`,
    };
  }

  if (name === 'move_pedal') {
    const rig = controller.getRig();
    const from = Math.max(0, Number(input.from) | 0);
    const to = Math.max(0, Number(input.to) | 0);
    if (from >= rig.pedals.length) {
      return {
        content: JSON.stringify({ error: `no pedal at index ${from}` }),
        chip: `? move ${from + 1}`,
      };
    }
    controller.movePedal(from, to);
    return {
      content: JSON.stringify({ applied: { moved: from, to } }),
      chip: `Move #${from + 1} → #${to + 1}`,
    };
  }

  if (name === 'set_cab') {
    const cab = input.cab === 'brit412' ? 'brit412' : 'clean212';
    controller.setCab(cab);
    return {
      content: JSON.stringify({ applied: { cab } }),
      chip: `Cab ${cab === 'brit412' ? 'Brit 4×12' : 'Clean 2×12'}`,
    };
  }

  if (name === 'set_amp') {
    const type = input.type === 'jcm800' ? 'jcm800' : 'clean120';
    controller.setAmp(type);
    return {
      content: JSON.stringify({ applied: { amp: type } }),
      chip: `Amp ${type === 'jcm800' ? 'JCM800' : 'Clean 120'}`,
    };
  }

  if (name === 'set_switch') {
    const valid: SwitchName[] = ['bright', 'cab', 'chorus', 'vibrato'];
    const sw = (valid.includes(input.name as SwitchName) ? input.name : 'bright') as SwitchName;
    const on = Boolean(input.on);
    controller.setSwitch(sw, on);
    const label = sw.charAt(0).toUpperCase() + sw.slice(1);
    return {
      content: JSON.stringify({ applied: { name: sw, on } }),
      chip: `${label} ${on ? 'on' : 'off'}`,
    };
  }

  return { content: JSON.stringify({ error: `unknown tool '${name}'` }), chip: '? tool' };
}
