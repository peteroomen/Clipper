// Pedal — the CLIPPER pedal faces (M6.8 identity pass).
//
// SHARED CHASSIS, DISTINCT SOULS. Every knob pedal is sculpted from the same
// neumorphic chassis (the `.pedal raised` shell), but each TYPE declares a FACE
// DEFINITION that gives it a soul on three axes:
//   1. enclosure tint  — a desaturated full-body wash (--rat-tint / --sd1-tint,
//      layered over the neumorphic base in pedal.css so BOTH themes work),
//   2. face LAYOUT      — a referential geometry ('stack' vs 'compact'),
//   3. typography       — the hero wordmark treatment (condensed vs Boss-plate).
//
// Two dirt variants today (M8 shared the SAME three params; the SD-1 relabels the
// slots so distortion==Drive, filter==Tone):
//   rat  — 'stack'   : tall charcoal box, big centered 3-knob trio, stark condensed
//                      "Clipper" logo, round stomp. It IS the reference.
//   sd1  — 'compact' : warm-amber Boss-compact HOMAGE — a compact knob row across
//                      the top over a wide flat hinged TREADLE pad (the footswitch
//                      is the treadle, not a round button), small model line.
//
// Homage, never replica: no real trademarks/logos/exact trade dress. The knob's
// onChange still writes the underlying ParamName while its label differs, so
// nothing about the audio ABI changes. When bypassed the LED goes dark and the
// value arcs dim (via `.pedal.on`). Everything reads/writes the RigState the
// parent owns; nothing is stored here.

import { Knob } from './Knob';
import { thunk } from '../ui-sound';
import { PEDAL_KNOB_DEFAULTS, type PedalState, type ParamName, type PedalType } from '../rig';

export interface PedalProps {
  pedal: PedalState;
  onParam: (name: ParamName, value: number) => void;
  onToggleEngaged: () => void;
}

// Per-type face: the layout variant, the model line, the hero wordmark, and the
// three knobs (label / aria / underlying param / testId). The underlying `param`
// is the shared PedalParams slot the knob writes.
interface KnobSpec {
  name: string;
  aria: string;
  param: ParamName;
  testId: string;
}
type FaceLayout = 'stack' | 'compact';
interface PedalFace {
  layout: FaceLayout;
  model: string; // small model line (top eyebrow)
  wordmark: string; // hero text: the stack logo / the treadle plate name
  knobs: [KnobSpec, KnobSpec, KnobSpec];
}
// Every KNOB pedal gets a faceplate here; the tuner renders via its own component
// (Tuner.tsx) and never reaches Pedal. To add a future pedal's face, add an entry
// here and (if a new layout) a `[data-face]` variant in pedal.css — see docs §17.
const FACES: Record<Exclude<PedalType, 'tuner'>, PedalFace> = {
  rat: {
    layout: 'stack',
    model: 'DIRT Nº1 · RAT-TYPE',
    wordmark: 'Clipper',
    knobs: [
      { name: 'Dist', aria: 'Distortion', param: 'distortion', testId: 'knob-distortion' },
      { name: 'Filter', aria: 'Filter', param: 'filter', testId: 'knob-filter' },
      { name: 'Level', aria: 'Level', param: 'level', testId: 'knob-level' },
    ],
  },
  sd1: {
    layout: 'compact',
    model: 'DRIVE Nº2 · SD-TYPE',
    wordmark: 'Super Drive',
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

  const knobs = (
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
  );

  // The footswitch is shared behavior (role/testid/aria identical across faces);
  // only its SHAPE changes — round stomp on 'stack', wide treadle pad on 'compact'.
  const footswitch = (
    <button
      type="button"
      className={`fsw${face.layout === 'compact' ? ' fsw-treadle' : ''}`}
      role="switch"
      aria-checked={engaged}
      aria-label="Bypass footswitch"
      data-testid="footswitch"
      onPointerDown={() => thunk(true)}
      onClick={() => {
        onToggleEngaged();
        thunk(false);
      }}
    >
      {face.layout === 'compact' && (
        <span className="treadle-wordmark" aria-hidden="true">
          {face.wordmark}
        </span>
      )}
    </button>
  );

  return (
    <div
      className={`pedal raised${engaged ? ' on' : ''}`}
      data-testid="pedal"
      data-engaged={engaged}
      data-pedal-type={type}
      data-face={face.layout}
    >
      <div className="pedal-top">
        <span className="pedal-model">{face.model}</span>
        <span className="led" data-testid="pedal-led" aria-hidden="true" />
      </div>

      {face.layout === 'stack' ? (
        <>
          <div className="pedal-logo display">{face.wordmark}</div>
          {knobs}
          <div className="fsw-zone">
            {footswitch}
            <span className="fsw-label">Stomp</span>
          </div>
        </>
      ) : (
        // Boss-compact homage: knobs ride the top edge; the treadle owns the body.
        <>
          {knobs}
          <div className="fsw-zone treadle-zone">
            {footswitch}
            <span className="fsw-label">Stomp</span>
          </div>
        </>
      )}
    </div>
  );
}
