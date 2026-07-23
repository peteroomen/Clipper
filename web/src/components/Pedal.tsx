// Pedal — the CLIPPER pedal face from the approved design.
//
//   model line "DIRT Nº1 · RAT-TYPE" · Anton "Clipper" logo · red LED
//   three knobs (Dist / Filter / Level) · footswitch that toggles bypass
//
// When bypassed the LED goes dark and the value arcs dim (both via the CSS
// `.pedal.on` class from pedal.css). Everything reads/writes the RigState the
// parent owns; nothing is stored locally.

import { Knob } from './Knob';
import { thunk } from '../ui-sound';
import { KNOB_DEFAULTS, type PedalState, type ParamName } from '../rig';

export interface PedalProps {
  pedal: PedalState;
  onParam: (name: ParamName, value: number) => void;
  onToggleEngaged: () => void;
}

export function Pedal({ pedal, onParam, onToggleEngaged }: PedalProps) {
  const { engaged, params } = pedal;

  return (
    <div className={`pedal raised${engaged ? ' on' : ''}`} data-testid="pedal" data-engaged={engaged}>
      <div className="pedal-top">
        <span className="pedal-model">DIRT Nº1 · RAT-TYPE</span>
        <span className="led" data-testid="pedal-led" aria-hidden="true" />
      </div>

      <div className="pedal-logo display">Clipper</div>

      <div className="knob-row">
        <Knob
          name="Dist"
          ariaLabel="Distortion"
          value={params.distortion}
          defaultValue={KNOB_DEFAULTS.distortion}
          onChange={(v) => onParam('distortion', v)}
          testId="knob-distortion"
        />
        <Knob
          name="Filter"
          ariaLabel="Filter"
          value={params.filter}
          defaultValue={KNOB_DEFAULTS.filter}
          onChange={(v) => onParam('filter', v)}
          testId="knob-filter"
        />
        <Knob
          name="Level"
          ariaLabel="Level"
          value={params.level}
          defaultValue={KNOB_DEFAULTS.level}
          onChange={(v) => onParam('level', v)}
          testId="knob-level"
        />
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
