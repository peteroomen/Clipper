// Amp — the CLEAN 120 panel from the approved design.
//
//   Anton name "Clean 120" (+ "Solid State · Stereo") · four small knobs
//   (Vol / Bass / Mid / Treble) · Bright lever · Cab lever · Power rocker with a
//   jewel lamp (lit when the amp is powered = engaged).
//
// The amp is modeled LINEAR (JC-120-style clean platform); all drive comes from
// the pedal in front. When powered off the jewel goes dark, the value arcs dim
// (via the CSS `.amp.on` class), and the worklet bypasses amp+cab (passing the
// pedal output straight through). Everything reads/writes the RigState the parent
// owns; nothing is stored locally.

import { Knob } from './Knob';
import { thunk } from '../ui-sound';
import { AMP_KNOB_DEFAULTS, type AmpState, type AmpParamName } from '../rig';

export interface AmpProps {
  amp: AmpState;
  onParam: (name: AmpParamName, value: number) => void;
  onToggle: (name: 'bright' | 'cab') => void; // flip a 0/1 toggle
  onTogglePower: () => void; // amp engaged (power)
}

export function Amp({ amp, onParam, onToggle, onTogglePower }: AmpProps) {
  const { engaged, params } = amp;

  return (
    <div className="amp-wing">
      <div className={`amp raised${engaged ? ' on' : ''}`} data-testid="amp" data-engaged={engaged}>
        <div className="amp-head">
          <div className="amp-name display">
            Clean 120<small>Solid State · Stereo</small>
          </div>
        </div>

        <div className="amp-controls">
          <Knob
            name="Vol"
            ariaLabel="Volume"
            value={params.volume}
            defaultValue={AMP_KNOB_DEFAULTS.volume}
            onChange={(v) => onParam('volume', v)}
            testId="knob-volume"
          />
          <Knob
            name="Bass"
            ariaLabel="Bass"
            value={params.bass}
            defaultValue={AMP_KNOB_DEFAULTS.bass}
            onChange={(v) => onParam('bass', v)}
            testId="knob-bass"
          />
          <Knob
            name="Mid"
            ariaLabel="Middle"
            value={params.middle}
            defaultValue={AMP_KNOB_DEFAULTS.middle}
            onChange={(v) => onParam('middle', v)}
            testId="knob-middle"
          />
          <Knob
            name="Treble"
            ariaLabel="Treble"
            value={params.treble}
            defaultValue={AMP_KNOB_DEFAULTS.treble}
            onChange={(v) => onParam('treble', v)}
            testId="knob-treble"
          />

          <div className="amp-right">
            <button
              type="button"
              className={`toggle${params.bright >= 0.5 ? ' on' : ''}`}
              role="switch"
              aria-checked={params.bright >= 0.5}
              aria-label="Bright"
              data-testid="bright-toggle"
              onClick={() => {
                onToggle('bright');
                thunk(false);
              }}
            >
              <div className="t-slot">
                <div className="t-lever" />
              </div>
              <span className="k-name">Bright</span>
            </button>

            <button
              type="button"
              className={`toggle${params.cab >= 0.5 ? ' on' : ''}`}
              role="switch"
              aria-checked={params.cab >= 0.5}
              aria-label="Cab"
              data-testid="cab-toggle"
              onClick={() => {
                onToggle('cab');
                thunk(false);
              }}
            >
              <div className="t-slot">
                <div className="t-lever" />
              </div>
              <span className="k-name">Cab</span>
            </button>

            <button
              type="button"
              className="power"
              role="switch"
              aria-checked={engaged}
              aria-label="Power"
              data-testid="power"
              onPointerDown={() => thunk(true)}
              onClick={() => {
                onTogglePower();
                thunk(false);
              }}
            >
              <div className="jewel" data-testid="amp-jewel" aria-hidden="true" />
              <div className="rocker" />
              <span className="k-name">Power</span>
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}
