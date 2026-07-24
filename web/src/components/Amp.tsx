// Amp — two selectable FACES on the shared neumorphic chassis (M5 Clean 120,
// M9.4 JCM800), per the M6.8.1 visual doctrine (docs §17): one dark chassis for
// both; identity comes from a small-area ACCENT colour + one morphology cue + a
// knowing name — homage, never replica.
//
//   Clean 120 (amp.type === 'clean120'): the JC-120-style CLEAN platform —
//     name "Clean 120" · Vol / Bass / Mid / Treble / Reverb knobs · Bright + Cab
//     levers · stereo Chorus/Vibrato row · red accent (the amp default). Modeled
//     LINEAR; all drive comes from the pedal in front.
//
//   JCM800 (amp.type === 'jcm800'): the Marshall JCM800 2204 valve head — a knowing
//     homage "Eight Hundred" (model line HEAD Nº2 · BRIT-TYPE) · a GOLD/BRASS accent
//     on the knob arcs/readouts · the era-correct control row PRESENCE · BASS ·
//     MIDDLE · TREBLE · MASTER · GAIN (real 2204 front-panel left-to-right order) ·
//     NO bright/chorus/reverb (a real 2204 has none — those controls are hidden) ·
//     the Cab lever + Power rocker stay (the cab applies to both heads). It is a
//     MONO valve head that makes its own distortion (gain = preamp drive, master =
//     power-amp drive, presence = power-amp HF lift).
//
// When powered off the jewel goes dark and the value arcs dim (via `.amp.on`), and
// the worklet bypasses amp+cab. Everything reads/writes the RigState the parent
// owns; nothing is stored locally.

import { Knob } from './Knob';
import { thunk } from '../ui-sound';
import { AMP_KNOB_DEFAULTS, type AmpState, type AmpParamName } from '../rig';

export interface AmpProps {
  amp: AmpState;
  onParam: (name: AmpParamName, value: number) => void;
  onToggle: (name: 'bright' | 'cab') => void; // flip a 0/1 toggle
  onTogglePower: () => void; // amp engaged (power)
  onChorusMode: (mode: number) => void; // 0 off | 1 chorus | 2 vibrato
}

const CHORUS_MODES: Array<{ value: number; label: string }> = [
  { value: 0, label: 'Off' },
  { value: 1, label: 'Chorus' },
  { value: 2, label: 'Vibrato' },
];

// Shared right-hand control cluster: the Cab lever + Power rocker (and, for the
// Clean 120 only, the Bright lever). Both faces share the Cab + Power controls.
function AmpRight({
  amp,
  onToggle,
  onTogglePower,
  showBright,
}: {
  amp: AmpState;
  onToggle: (name: 'bright' | 'cab') => void;
  onTogglePower: () => void;
  showBright: boolean;
}) {
  const { engaged, params } = amp;
  return (
    <div className="amp-right">
      {showBright && (
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
      )}

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
  );
}

// The JCM800 face — the era-correct 2204 control row + a gold accent + the knowing
// "Eight Hundred" wordmark. No bright/chorus/reverb (the real amp has none).
function Jcm800Face({ amp, onParam, onToggle, onTogglePower }: AmpProps) {
  const { engaged, params } = amp;
  return (
    <div
      className={`amp raised jcm800${engaged ? ' on' : ''}`}
      data-testid="amp"
      data-engaged={engaged}
      data-amp-type="jcm800"
    >
      <div className="amp-head">
        <div className="amp-name display" data-testid="amp-name">
          Eight Hundred<small>Head Nº2 · Brit-Type</small>
        </div>
      </div>

      {/* Era-correct 2204 panel order (left → right): Presence · Bass · Middle ·
          Treble · Master · Gain. */}
      <div className="amp-controls">
        <Knob
          name="Presence"
          ariaLabel="Presence"
          value={params.presence}
          defaultValue={AMP_KNOB_DEFAULTS.presence}
          onChange={(v) => onParam('presence', v)}
          testId="knob-presence"
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
        <Knob
          name="Master"
          ariaLabel="Master"
          value={params.master}
          defaultValue={AMP_KNOB_DEFAULTS.master}
          onChange={(v) => onParam('master', v)}
          testId="knob-master"
        />
        <Knob
          name="Gain"
          ariaLabel="Gain"
          value={params.gain}
          defaultValue={AMP_KNOB_DEFAULTS.gain}
          onChange={(v) => onParam('gain', v)}
          testId="knob-gain"
        />

        {/* Cab lever + Power rocker only — no Bright (the 2204 has none). */}
        <AmpRight amp={amp} onToggle={onToggle} onTogglePower={onTogglePower} showBright={false} />
      </div>
    </div>
  );
}

// The Clean 120 face — the original approved design (unchanged from M5/M6.3/M6.7).
function Clean120Face({ amp, onParam, onToggle, onTogglePower, onChorusMode }: AmpProps) {
  const { params } = amp;
  const mode = params.chorusMode ?? 0;
  return (
    <div
      className={`amp raised${amp.engaged ? ' on' : ''}`}
      data-testid="amp"
      data-engaged={amp.engaged}
      data-amp-type="clean120"
    >
      <div className="amp-head">
        <div className="amp-name display" data-testid="amp-name">
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
        {/* M6.7 — the JC-120's spring REVERB, a single MIX knob on the facia
            beside the tone controls (where the real amp's reverb pot sits). */}
        <Knob
          name="Reverb"
          ariaLabel="Reverb"
          value={params.reverb}
          defaultValue={AMP_KNOB_DEFAULTS.reverb}
          onChange={(v) => onParam('reverb', v)}
          testId="knob-reverb"
        />

        <AmpRight amp={amp} onToggle={onToggle} onTogglePower={onTogglePower} showBright={true} />
      </div>

      {/* M6.3 — JC-120 chorus/vibrato: a second facia row with Speed + Depth
          knobs and an Off/Chorus/Vibrato 3-way selector. */}
      <div className="amp-chorus" data-testid="chorus">
        <div className="amp-chorus-label display">Chorus</div>
        <div className="amp-chorus-controls">
          <Knob
            name="Speed"
            ariaLabel="Chorus speed"
            value={params.speed}
            defaultValue={AMP_KNOB_DEFAULTS.speed}
            onChange={(v) => onParam('speed', v)}
            testId="knob-speed"
          />
          <Knob
            name="Depth"
            ariaLabel="Chorus depth"
            value={params.depth}
            defaultValue={AMP_KNOB_DEFAULTS.depth}
            onChange={(v) => onParam('depth', v)}
            testId="knob-depth"
          />

          <div
            className="mode-switch"
            role="radiogroup"
            aria-label="Chorus mode"
            data-testid="chorus-mode"
          >
            {CHORUS_MODES.map((m) => (
              <button
                key={m.value}
                type="button"
                role="radio"
                aria-checked={mode === m.value}
                className={`mode-opt${mode === m.value ? ' on' : ''}`}
                data-testid={`chorus-mode-${m.label.toLowerCase()}`}
                onClick={() => {
                  onChorusMode(m.value);
                  thunk(false);
                }}
              >
                {m.label}
              </button>
            ))}
            <span className="k-name">Mode</span>
          </div>
        </div>
      </div>
    </div>
  );
}

export function Amp(props: AmpProps) {
  return (
    <div className="amp-wing">
      {props.amp.type === 'jcm800' ? <Jcm800Face {...props} /> : <Clean120Face {...props} />}
    </div>
  );
}
