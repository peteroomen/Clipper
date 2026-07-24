// Pedal — the CLIPPER pedal faces (M6.8.1 doctrine revision).
//
// DARK CHASSIS FOR ALL, REFERENCE VIA ACCENT. Every pedal is sculpted from the
// SAME neumorphic chassis in ONE shared charcoal wash (the RAT's colour, which
// reads well against the lighter board). A full-body hue per pedal broke the
// neumorphism, so identity is now carried by three SUBTLE cues declared per TYPE:
//   1. ACCENT colour — a small-area, saturated colour on the dark chassis (knob
//      value arcs + value readouts + LED). RAT: red/orange · SD-1: yellow ·
//      tuner: green (its lock colour). Set as --pedal-accent in pedal.css.
//   2. one MORPHOLOGY cue — the SD-1's wide treadle vs the RAT's round stomp.
//   3. a knowing NAME — a wink a pedal-lover gets, no trademarks/exact names.
//
// Two dirt variants today (they share the SAME three params; the SD-1 relabels the
// slots so distortion==Drive, filter==Tone):
//   rat  — 'stack'   : tall box, big centered 3-knob trio, condensed "Rodent"
//                      logo, round stomp, red accent. It IS the reference.
//   sd1  — 'compact' : Boss-compact HOMAGE — a knob row with clear air across the
//                      top over a wide flat hinged TREADLE that owns the LOWER
//                      body (the footswitch is the treadle), yellow accent.
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
    // "Rodent" is the wink (the famous dirt box); "Clipper" is the app brand, not
    // this pedal. Model line references the type without the trademark.
    model: 'DIRT Nº1 · RODENT-TYPE',
    wordmark: 'Rodent',
    knobs: [
      { name: 'Dist', aria: 'Distortion', param: 'distortion', testId: 'knob-distortion' },
      { name: 'Filter', aria: 'Filter', param: 'filter', testId: 'knob-filter' },
      { name: 'Level', aria: 'Level', param: 'level', testId: 'knob-level' },
    ],
  },
  sd1: {
    layout: 'compact',
    // "Super Drive" + "YELLOW" wink at the classic yellow overdrive (and the new
    // yellow accent) without the trademark.
    model: 'DRIVE Nº2 · YELLOW',
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
        // Boss-compact homage: knobs ride the top edge with air; the treadle owns
        // the LOWER body and sits at the bottom — nothing below it (no caption;
        // the wordmark is embossed on the treadle itself).
        <>
          {knobs}
          <div className="fsw-zone treadle-zone">{footswitch}</div>
        </>
      )}
    </div>
  );
}
