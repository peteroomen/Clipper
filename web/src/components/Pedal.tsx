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
//   gold — 'plate'   : the gold-box legend — knob row over an ENGRAVED NAMEPLATE
//                      band, round stomp, GOLD accent. Type only on the plate: the
//                      original's engraved figure IS its trademark.
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
type FaceLayout = 'stack' | 'compact' | 'slim' | 'single' | 'wide' | 'plate' | 'rocker' | 'bank';
interface PedalFace {
  layout: FaceLayout;
  model: string; // small model line (top eyebrow)
  wordmark: string; // hero text: the stack logo / the treadle plate name
  // 1..3 knobs. The dirt faces carry three; the phaser's 'single' face carries
  // exactly one (the iconic one-knob face).
  knobs: KnobSpec[];
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
  // v1.1 TS808 "Screamer": the GREEN box. "Screamer" + "GREEN" wink at the most
  // famous overdrive without the trademark. Its OWN 'slim' face (an Ibanez-format
  // box — knob row across the top, a rectangular footswitch PAD in the lower body,
  // slimmer than the RAT stack) sets it apart from the Boss-compact SD-1 treadle
  // at a glance. Green accent (arcs/readouts/LED — the green box, everyone gets it).
  ts: {
    layout: 'slim',
    model: 'DRIVE Nº3 · GREEN',
    wordmark: 'Screamer',
    knobs: [
      { name: 'Drive', aria: 'Drive', param: 'distortion', testId: 'knob-drive' },
      { name: 'Tone', aria: 'Tone', param: 'filter', testId: 'knob-tone' },
      { name: 'Level', aria: 'Level', param: 'level', testId: 'knob-level' },
    ],
  },
  // v1.1 item 4 Muff "Pi": the physically HUGE fuzz — its morphology cue is a
  // 'wide' face (broader enclosure) with the three knobs in the classic TRIANGLE
  // (SUSTAIN top-left, VOLUME top-right, TONE centered below) and a big round stomp
  // low-center. VIOLET accent (the "violet era" wink). Hero wordmark "Pi" (the
  // π-era graphics wink without trade dress); model line names the type. No EHX/Big
  // Muff text anywhere. Knob array order is the triangle placement (grid in CSS):
  // 1=Sustain (top-left), 2=Volume (top-right), 3=Tone (bottom-center).
  muff: {
    layout: 'wide',
    model: 'FUZZ Nº5 · PI',
    wordmark: 'Pi',
    knobs: [
      { name: 'Sustain', aria: 'Sustain', param: 'distortion', testId: 'knob-sustain' },
      { name: 'Volume', aria: 'Volume', param: 'level', testId: 'knob-volume' },
      { name: 'Tone', aria: 'Tone', param: 'filter', testId: 'knob-tone' },
    ],
  },
  // v1.1 item 6 GOLD: the hoarded gold-box overdrive. Its morphology cue is the
  // 'plate' face — an ENGRAVED NAMEPLATE band across the mid body (the original is
  // known for its engraved enclosure art; we take the ENGRAVING IDEA, never the
  // artwork), knob row above it, round stomp below. GOLD accent (the one colour
  // everyone names when they name this pedal — a colour is fair game). "Myth" is the
  // wink: the pedal that became a legend, hoarded and flipped for absurd money. No
  // Klon/Centaur/KTR text and emphatically NO centaur/horse graphic — that figure is
  // the trademark, so the plate carries type only.
  gold: {
    layout: 'plate',
    model: 'DRIVE Nº6 · GOLD',
    wordmark: 'Myth',
    knobs: [
      { name: 'Gain', aria: 'Gain', param: 'distortion', testId: 'knob-gain' },
      { name: 'Treble', aria: 'Treble', param: 'filter', testId: 'knob-treble' },
      { name: 'Output', aria: 'Output', param: 'level', testId: 'knob-output' },
    ],
  },
  // M13.1 "Squash": the first DYNAMICS pedal and the first non-dirt box on the
  // board. Morphology cue is the small MXR-format enclosure with exactly TWO
  // knobs side by side over a big round stomp — it shares the 'stack' anatomy
  // with the RAT but reads instantly different because two knobs is a compressor
  // and three is a dirt box. TEAL accent: the real pedal is red, but the RAT
  // already owns red here and two red boxes on one board is a usability bug, so
  // the colour is chosen for distinguishability (see tokens.css). Wordmark
  // "Squash" is the wink at what the pedal is famous for doing; the model line
  // names the type. No MXR/Dyna Comp/Ross text anywhere.
  comp: {
    layout: 'stack',
    model: 'DYNAMICS Nº7 · SQUASH',
    wordmark: 'Squash',
    knobs: [
      // TWO real knobs. Slot 0 (distortion) is SUSTAIN — which is NOT a
      // threshold: it sets the OTA's idle bias current, so it buys compression,
      // make-up gain and noise together. Slot 2 (level) is the output pot. Slot 1
      // is carried and unused, exactly as the phaser carries slots 1/2.
      { name: 'Sustain', aria: 'Sustain', param: 'distortion', testId: 'knob-sustain' },
      { name: 'Level', aria: 'Level', param: 'level', testId: 'knob-level' },
    ],
  },
  // M13.6a "Curfew": the lineup's first UTILITY — it makes no sound of its own,
  // it takes one away. Morphology cue is the SLATE accent and a two-knob face on
  // the small 'stack' anatomy: deliberately the most boring box on the board,
  // because that is what a gate is. Wordmark "Curfew" is the wink (the thing that
  // makes the noise stop at a set hour); the model line names the type. No
  // Boss/NS-2/ISP/Decimator text anywhere.
  gate: {
    layout: 'stack',
    model: 'UTILITY Nº8 · GATE',
    wordmark: 'Curfew',
    knobs: [
      // TWO real knobs, because the reference has two. Slot 0 (distortion) is
      // THRESHOLD — and unlike the compressor's SUSTAIN it really is a threshold
      // (docs §61.4). Slot 2 (level) is DECAY, the fade time after the note
      // stops. Slot 1 is carried and unused. The reference's third control is a
      // MODE switch that changes what the FOOTSWITCH does rather than the audio,
      // and this board's footswitch is bypass on every pedal — so it is
      // deliberately not shipped rather than shipped as a knob that lies.
      { name: 'Thresh', aria: 'Threshold', param: 'distortion', testId: 'knob-threshold' },
      { name: 'Decay', aria: 'Decay', param: 'level', testId: 'knob-decay' },
    ],
  },
  // Post-v1.1 wah "Weeper": the lineup's FIRST FILTER pedal, and the first one
  // whose real-world enclosure is not a box you press but a ROCKING TREADLE you
  // stand on. That is its morphology cue and it gets its own 'rocker' layout: a
  // wide, low chassis dominated by a full-width RIBBED footplate hinged at the
  // back, with the three small knobs on a strip along the top edge. TEAL accent —
  // deliberately the furthest hue from the six dirt/mod pedals, because this is
  // the first member of a new family. "Weeper" is the wink; no Dunlop / Cry Baby /
  // Vox / Mu-Tron wording anywhere on the face (docs §17 doctrine).
  wah: {
    layout: 'rocker',
    model: 'FILTER Nº7 · TREADLE',
    wordmark: 'Weeper',
    knobs: [
      // Slot 0 = POSITION (heel -> toe: an ordinary automatable parameter, and the
      // reason the treadle is not the only way to play this). Slot 1 = SENSE (0 is
      // exactly the manual pedal; above 0 the envelope sweeps the same tank).
      // Slot 2 = VOICE (the documented "vocal mod" — width only).
      { name: 'Position', aria: 'Position', param: 'distortion', testId: 'knob-position' },
      { name: 'Sense', aria: 'Sensitivity', param: 'filter', testId: 'knob-sense' },
      { name: 'Voice', aria: 'Voice', param: 'level', testId: 'knob-voice' },
    ],
  },
  // M13.4 "Echoman": the lineup's FIRST DELAY, and the first member of a new DSP
  // family. Morphology cue is the WIDE, low chassis of a Memory Man-format
  // enclosure — a delay is not a small box, and it reads as one at a glance among
  // the compact dirt stack. DEEP BLUE accent (tokens.css --accent-delay): the
  // furthest hue from every dirt/mod/filter box on the board, chosen so a player
  // finds the repeats knob without reading. "Echoman" is the wink; no
  // Electro-Harmonix / Memory Man / Deluxe wording anywhere on the face (docs §17).
  delay: {
    layout: 'bank',
    model: 'DELAY Nº8 · BUCKET BRIGADE',
    wordmark: 'Echoman',
    knobs: [
      // Slot 0 = DELAY, and on a bucket brigade that knob is the CLOCK: turning it
      // up slows the device down, which is why the repeats also get darker.
      // Slot 1 = FEEDBACK (repeats). Slot 2 = BLEND — at 0 the pedal is bit-exactly
      // your dry signal.
      { name: 'Delay', aria: 'Delay', param: 'distortion', testId: 'knob-delay' },
      { name: 'Feedback', aria: 'Feedback', param: 'filter', testId: 'knob-feedback' },
      { name: 'Blend', aria: 'Blend', param: 'level', testId: 'knob-blend' },
    ],
  },
  // M13.7 CE-1 "Ensemble": the world's FIRST chorus pedal, and the first pedal
  // here whose circuit the project already owned (it is the JC-120 amp's chorus
  // in a floor box — docs §62). The real CE-1 is a big, wide, low-slung enclosure
  // with two footswitches, which is why it takes the 'plate' anatomy rather than
  // the compact 'stack': it reads as a large box, not a dirt pedal. MAGENTA accent
  // — the phaser already owns orange and this is the second MODULATION pedal, so
  // the hue separates the family members (tokens.css --accent-chorus). "Ensemble"
  // is the wink at the name; no Boss / Roland / CE-1 wording anywhere.
  chorus: {
    layout: 'plate',
    model: 'MODULATION Nº8 · ENSEMBLE',
    wordmark: 'Ensemble',
    knobs: [
      // Slot 0 = RATE. Its RANGE depends on MODE (1.0-3.0 Hz chorus, 3.2-11.6 Hz
      // vibrato) — the two ranges do not overlap on the real pedal, which is why
      // the modes read as different effects rather than two speeds of one.
      // Slot 1 = DEPTH (the 3-5 .. 3-7 ms delay window; it never reaches zero).
      // Slot 2 = MODE, and it is genuinely DISCRETE: < 0.5 chorus, >= 0.5 vibrato.
      // The real CE-1 gangs rate+depth onto one INTENSITY knob in chorus mode; we
      // keep them independent so no knob is dead in either mode (docs §62.6).
      { name: 'Rate', aria: 'Rate', param: 'distortion', testId: 'knob-rate' },
      { name: 'Depth', aria: 'Depth', param: 'filter', testId: 'knob-depth' },
      { name: 'Mode', aria: 'Mode (chorus / vibrato)', param: 'level', testId: 'knob-mode' },
    ],
  },
  phaser: {
    // The iconic ONE-KNOB face: a single big centered SPEED knob on a dark chassis
    // with an ORANGE accent (the orange box, instantly read). "Ninety" is the wink
    // (the script-logo Phase 90); model line names the type without the trademark.
    layout: 'single',
    model: 'PHASER Nº4 · SCRIPT',
    wordmark: 'Ninety',
    knobs: [
      // ONE real knob. It writes the shared slot 0 (rig param 'distortion'), which
      // the phaser core reads as SPEED. Slots 1/2 are unused-but-carried.
      { name: 'Speed', aria: 'Speed', param: 'distortion', testId: 'knob-speed' },
    ],
  },
};

export function Pedal({ pedal, onParam, onToggleEngaged }: PedalProps) {
  const { engaged, params, type } = pedal;
  const face: PedalFace = (FACES as Partial<Record<PedalType, PedalFace>>)[type] ?? FACES.rat;
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
  // only its SHAPE changes — round stomp on 'stack', wide treadle pad on 'compact',
  // a rectangular hinged PAD on 'slim' (the Ibanez-format stomp).
  const fswShape =
    face.layout === 'compact' ? ' fsw-treadle'
      : face.layout === 'slim' ? ' fsw-pad'
      : face.layout === 'rocker' ? ' fsw-rocker'
      : '';
  const footswitch = (
    <button
      type="button"
      className={`fsw${fswShape}`}
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
      {(face.layout === 'compact' || face.layout === 'rocker') && (
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

      {face.layout === 'compact' ? (
        // Boss-compact homage: knobs ride the top edge with air; the treadle owns
        // the LOWER body and sits at the bottom — nothing below it (no caption;
        // the wordmark is embossed on the treadle itself).
        <>
          {knobs}
          <div className="fsw-zone treadle-zone">{footswitch}</div>
        </>
      ) : face.layout === 'slim' ? (
        // Ibanez-format 'slim' box (the GREEN Screamer): a knob row across the TOP,
        // the script wordmark on the mid body, and a RECTANGULAR hinged stomp pad
        // in the lower body — slimmer than the RAT stack and unmistakably NOT the
        // Boss-compact SD-1 treadle at a glance.
        <>
          {knobs}
          <div className="slim-wordmark display">{face.wordmark}</div>
          <div className="fsw-zone pad-zone">{footswitch}</div>
        </>
      ) : face.layout === 'rocker' ? (
        // 'rocker' (the wah): a low, wide chassis with the three small knobs on a
        // strip across the top and a full-width RIBBED footplate, hinged at the
        // back, owning the rest of the body — the silhouette of a treadle pedal
        // rather than a box. The footswitch IS the footplate.
        <>
          {knobs}
          <div className="fsw-zone rocker-zone">{footswitch}</div>
        </>
      ) : face.layout === 'plate' ? (
        // 'plate' (the GOLD box): knob row on top, an ENGRAVED NAMEPLATE band across
        // the mid body carrying the wordmark, round stomp below. Type only — the
        // original's engraved figure is its trademark and is deliberately absent.
        <>
          {knobs}
          <div className="name-plate" aria-hidden="true">
            <span className="name-plate-text">{face.wordmark}</span>
          </div>
          <div className="fsw-zone">
            {footswitch}
            <span className="fsw-label">Stomp</span>
          </div>
        </>
      ) : (
        // 'stack' (RAT three-knob trio) and 'single' (phaser one-knob face) share
        // the vertical layout: hero wordmark, knob(s), round stomp. The 'single'
        // face centers its one big SPEED knob (styled via [data-face="single"]).
        <>
          <div className="pedal-logo display">{face.wordmark}</div>
          {knobs}
          <div className="fsw-zone">
            {footswitch}
            <span className="fsw-label">Stomp</span>
          </div>
        </>
      )}
    </div>
  );
}
