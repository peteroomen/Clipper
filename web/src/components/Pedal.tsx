// Pedal — the CLIPPER pedal face from the approved design.
//
// Two dirt variants share ONE enclosure (M8):
//   rat  — "DIRT Nº1 · RAT-TYPE",  red LED,   knobs Dist / Filter / Level
//   sd1  — "DRIVE Nº2 · SD-TYPE",  amber LED, knobs Drive / Tone / Level
//
// Both store the SAME three-knob params (distortion / filter / level — see
// rig.ts); for the SD-1 those slots READ as Drive / Tone / Level, so the knob's
// onChange still writes the underlying ParamName while its label differs. The
// amber accent is a CSS data-attribute swap (pedal.css / tokens.css), so nothing
// off-token is introduced.
//
// When bypassed the LED goes dark and the value arcs dim (via `.pedal.on`).
// Everything reads/writes the RigState the parent owns; nothing is stored here.

import { Knob } from './Knob';
import { thunk } from '../ui-sound';
import { PEDAL_KNOB_DEFAULTS, type PedalState, type ParamName, type PedalType } from '../rig';

export interface PedalProps {
  pedal: PedalState;
  onParam: (name: ParamName, value: number) => void;
  onToggleEngaged: () => void;
}

// Per-type face: model line + the three knobs (label / aria / underlying param /
// testId). The underlying `param` is the shared PedalParams slot the knob writes.
interface KnobSpec {
  name: string;
  aria: string;
  param: ParamName;
  testId: string;
}
interface PedalFace {
  model: string;
  knobs: [KnobSpec, KnobSpec, KnobSpec];
}
// Every KNOB pedal gets a faceplate here; the tuner renders via its own
// component (Tuner.tsx) and never reaches Pedal.
const FACES: Record<Exclude<PedalType, 'tuner'>, PedalFace> = {
  rat: {
    model: 'DIRT Nº1 · RAT-TYPE',
    knobs: [
      { name: 'Dist', aria: 'Distortion', param: 'distortion', testId: 'knob-distortion' },
      { name: 'Filter', aria: 'Filter', param: 'filter', testId: 'knob-filter' },
      { name: 'Level', aria: 'Level', param: 'level', testId: 'knob-level' },
    ],
  },
  sd1: {
    model: 'DRIVE Nº2 · SD-TYPE',
    knobs: [
      { name: 'Drive', aria: 'Drive', param: 'distortion', testId: 'knob-drive' },
      { name: 'Tone', aria: 'Tone', param: 'filter', testId: 'knob-tone' },
      { name: 'Level', aria: 'Level', param: 'level', testId: 'knob-level' },
    ],
  },
};

export function Pedal({ pedal, onParam, onToggleEngaged }: PedalProps) {
  const { engaged, params, type } = pedal;
  const face: PedalFace = type === 'sd1' ? FACES.sd1 : FACES.rat;
  const defaults = PEDAL_KNOB_DEFAULTS[type] ?? PEDAL_KNOB_DEFAULTS.rat;

  return (
    <div
      className={`pedal raised${engaged ? ' on' : ''}`}
      data-testid="pedal"
      data-engaged={engaged}
      data-pedal-type={type}
    >
      <div className="pedal-top">
        <span className="pedal-model">{face.model}</span>
        <span className="led" data-testid="pedal-led" aria-hidden="true" />
      </div>

      <div className="pedal-logo display">Clipper</div>

      <div className="knob-row">
        {face.knobs.map((k) => (
          <Knob
            key={k.testId}
            name={k.name}
            ariaLabel={k.aria}
            value={params[k.param]}
            defaultValue={defaults[k.param]}
            onChange={(v) => onParam(k.param, v)}
            testId={k.testId}
          />
        ))}
      </div>

      <div className="fsw-zone">
        <button
          type="button"
          className="fsw"
          role="switch"
          aria-checked={engaged}
          aria-label="Bypass footswitch"
          data-testid="footswitch"
          onPointerDown={() => thunk(true)}
          onClick={() => {
            onToggleEngaged();
            thunk(false);
          }}
        />
        <span className="fsw-label">Stomp</span>
      </div>
    </div>
  );
}
