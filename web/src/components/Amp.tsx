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
//     a REVERB knob (M10.1 usability add — the real 2204 has none) · no bright/chorus ·
//     the Cab lever + Power rocker stay (the cab applies to all heads). It is a
//     MONO valve head that makes its own distortion (gain = preamp drive, master =
//     power-amp drive, presence = power-amp HF lift).
//
//   Twin (amp.type === 'twin'): the Fender blackface "Twin-style" CLEAN benchmark —
//     a knowing homage "Twin Sixty-Five" (model line COMBO Nº3 · BLACK-PANEL) · a cool
//     silver-blue accent · the control row VOLUME · BASS · MIDDLE · TREBLE · REVERB ·
//     a BRIGHT switch · a TREMOLO row (SPEED · INTENSITY — the optical "vibrato") ·
//     no gain/master/presence (this amp makes clean headroom, not preamp gain) and
//     no chorus mode. Cab lever + Power rocker stay.
//
//   Orange (amp.type === 'orange'): the OR120 "Overdrive" head — the MID-FORWARD
//   voice. VOLUME (no master), BASS/TREBLE (no mid — a James stack), F.A.C., HF
//   DRIVE (the shared 'presence' slot) and REVERB. Docs §57.
//   AC30 (amp.type === 'ac30'): the Vox AC30 "top boost" class-A CHIME/JANGLE combo —
//     a knowing homage "Thirty" (model line COMBO Nº4 · TOP-BOOST) · a warm COPPER
//     accent · the control row VOLUME · BASS · TREBLE · CUT · REVERB. VOLUME is the
//     overdrive (crank for class-A grind); CUT binds to the shared 'presence' param
//     (the C ABI routes id 11 to the AC30 top cut) but is LABELED "Cut". No middle/
//     gain/master/bright/chorus. Cab lever + Power rocker stay (no bright switch).
//
// When powered off the jewel goes dark and the value arcs dim (via `.amp.on`), and
// the worklet bypasses amp+cab. Everything reads/writes the RigState the parent
// owns; nothing is stored locally.

import { Knob } from './Knob';
import { RotarySelector, SegmentSwitch } from './Selector';
import {
  MESA_MODES,
  MESA_RECTIFIERS,
  MESA_POWER_MODES,
  ORANGE_FAC_POSITIONS,
} from '../params';
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
        {/* M10.1 — a spring REVERB knob added as a usability convenience. The real
            2204 has no reverb; the app adds one anyway (docs §19 note). */}
        <Knob
          name="Reverb"
          ariaLabel="Reverb"
          value={params.reverb}
          defaultValue={AMP_KNOB_DEFAULTS.reverb}
          onChange={(v) => onParam('reverb', v)}
          testId="knob-reverb"
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

// The Twin face — the Fender blackface "Twin-style" CLEAN benchmark. A knowing
// homage "Twin Sixty-Five" (model line COMBO Nº3 · BLACK-PANEL) · a cool silver-
// blue accent (--accent-twin) · the control row VOLUME · BASS · MIDDLE · TREBLE ·
// REVERB, a BRIGHT switch, and a TREMOLO row (SPEED · INTENSITY — the optical
// "vibrato"). Hidden: gain/master/presence (this amp makes no preamp gain) and the
// chorus mode (its modulation is tremolo, not chorus). The panel stays light/bench-
// style exactly like the Eight Hundred face — no new panel philosophy.
function TwinFace({ amp, onParam, onToggle, onTogglePower, onChorusMode }: AmpProps) {
  const { params } = amp;
  // The Twin has no chorus — the chorusMode slot is reused as its TREMOLO ON/OFF
  // (0 = off, 1 = on; same per-voice slot-reuse pattern as presence→CUT on the
  // Thirty). Off is a bit-exact bypass in the core; the toggle is click-free.
  const tremOn = (params.chorusMode ?? 0) >= 1;
  return (
    <div
      className={`amp raised twin${amp.engaged ? ' on' : ''}`}
      data-testid="amp"
      data-engaged={amp.engaged}
      data-amp-type="twin"
    >
      <div className="amp-head">
        <div className="amp-name display" data-testid="amp-name">
          Twin Sixty-Five<small>Combo Nº3 · Black-Panel</small>
        </div>
      </div>

      {/* Blackface panel order: Volume · Bass · Middle · Treble · Reverb. */}
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

      {/* The famous "vibrato" (a misnomer — it is amplitude TREMOLO): SPEED +
          INTENSITY, plus an ON/OFF switch (the real amp gates the circuit from a
          footswitch/panel — and defaulting OFF keeps existing rigs untouched).
          Reuses the shared speed/depth mod knobs + the chorusMode slot, routed to
          the opto tremolo (per-model routing in the C ABI). */}
      <div className="amp-chorus" data-testid="tremolo">
        <div className="amp-chorus-label display">Tremolo</div>
        <div className="amp-chorus-controls">
          <Knob
            name="Speed"
            ariaLabel="Tremolo speed"
            value={params.speed}
            defaultValue={AMP_KNOB_DEFAULTS.speed}
            onChange={(v) => onParam('speed', v)}
            testId="knob-speed"
          />
          <Knob
            name="Intensity"
            ariaLabel="Tremolo intensity"
            value={params.depth}
            defaultValue={AMP_KNOB_DEFAULTS.depth}
            onChange={(v) => onParam('depth', v)}
            testId="knob-depth"
          />

          <div
            className="mode-switch"
            role="radiogroup"
            aria-label="Tremolo on/off"
            data-testid="trem-switch"
          >
            {[
              { value: 0, label: 'Off' },
              { value: 1, label: 'On' },
            ].map((m) => (
              <button
                key={m.value}
                type="button"
                role="radio"
                aria-checked={tremOn === (m.value === 1)}
                className={`mode-opt${tremOn === (m.value === 1) ? ' on' : ''}`}
                data-testid={`trem-${m.label.toLowerCase()}`}
                onClick={() => {
                  onChorusMode(m.value);
                  thunk(false);
                }}
              >
                {m.label}
              </button>
            ))}
            <span className="k-name">Trem</span>
          </div>
        </div>
      </div>
    </div>
  );
}

// The AC30 face — a Vox "top boost" class-A CHIME/JANGLE combo. Modeled on the
// TwinFace (its closest sibling — a bright clean/crunch combo). A knowing homage
// "Thirty" (model line COMBO Nº4 · TOP-BOOST) · a warm COPPER accent (--accent-ac30).
// Control row: VOLUME · BASS · TREBLE · CUT · REVERB. The VOLUME knob IS the
// overdrive (crank it for the class-A grind). The CUT knob binds to the shared
// 'presence' param (the C ABI routes id 11 to the AC30's top CUT) but is LABELED
// "Cut". Hidden: middle, gain, master, bright, chorus (the top-boost face has none
// of those). Cab lever + Power rocker stay (showBright={false} — no bright switch).
function Ac30Face({ amp, onParam, onToggle, onTogglePower }: AmpProps) {
  const { params } = amp;
  return (
    <div
      className={`amp raised ac30${amp.engaged ? ' on' : ''}`}
      data-testid="amp"
      data-engaged={amp.engaged}
      data-amp-type="ac30"
    >
      <div className="amp-head">
        <div className="amp-name display" data-testid="amp-name">
          Thirty<small>Combo Nº4 · Top-Boost</small>
        </div>
      </div>

      {/* Top-boost panel order: Volume · Bass · Treble · Cut · Reverb. */}
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
          name="Treble"
          ariaLabel="Treble"
          value={params.treble}
          defaultValue={AMP_KNOB_DEFAULTS.treble}
          onChange={(v) => onParam('treble', v)}
          testId="knob-treble"
        />
        {/* CUT (top cut): reuses the shared 'presence' AmpParams field/param name —
            the C ABI routes id 11 to the AC30's top CUT — but is LABELED "Cut". It is
            INVERTED in feel: higher CUT = darker (tames the top without losing chime). */}
        <Knob
          name="Cut"
          ariaLabel="Cut"
          value={params.presence}
          defaultValue={AMP_KNOB_DEFAULTS.presence}
          onChange={(v) => onParam('presence', v)}
          testId="knob-presence"
        />
        <Knob
          name="Reverb"
          ariaLabel="Reverb"
          value={params.reverb}
          defaultValue={AMP_KNOB_DEFAULTS.reverb}
          onChange={(v) => onParam('reverb', v)}
          testId="knob-reverb"
        />

        {/* Cab lever + Power rocker only — no Bright (the top-boost face has none). */}
        <AmpRight amp={amp} onToggle={onToggle} onTogglePower={onTogglePower} showBright={false} />
      </div>
    </div>
  );
}

// The Champ face (docs §73) — the tweed 5F1, and THE SPARSEST PANEL IN THE APP.
// A knowing homage: "Cadet" (model line COMBO Nº8 · TWEED) · a lacquered wheat
// accent (--accent-champ). Control row: VOLUME · REVERB, and that is the whole
// amp.
//
// WHAT IS ABSENT IS THE POINT, so it is listed rather than left to be noticed as a
// gap: there is NO tone stack on a 5F1 — no bass, no middle, no treble — because
// Fender did not put one on a Champ until the 1964 blackface AA764. There is no
// gain, no master, no presence, no bright switch and no chorus either. The one
// knob sits BETWEEN the two preamp triodes with nothing downstream to trim, so how
// far it is up IS how much distortion you get, and how hard you pick is the rest
// of the tone control. REVERB is the §19 usability convenience every voice here
// carries (a real 5F1 has no tank).
//
// The knob binds to `champVolume`, its OWN param — not the shared `volume` — so it
// can carry its own default. See AMP_PARAM_CHAMP_VOLUME in params.ts.
function ChampFace({ amp, onParam, onToggle, onTogglePower }: AmpProps) {
  const { params } = amp;
  return (
    <div
      className={`amp raised champ${amp.engaged ? ' on' : ''}`}
      data-testid="amp"
      data-engaged={amp.engaged}
      data-amp-type="champ"
    >
      <div className="amp-head">
        <div className="amp-name display" data-testid="amp-name">
          Cadet<small>Combo Nº8 · Tweed</small>
        </div>
      </div>

      {/* The entire 5F1 panel: one volume knob. Reverb is ours, not Fender's. */}
      <div className="amp-controls">
        <Knob
          name="Vol"
          ariaLabel="Volume"
          value={params.champVolume}
          defaultValue={AMP_KNOB_DEFAULTS.champVolume}
          onChange={(v) => onParam('champVolume', v)}
          testId="knob-champ-volume"
        />
        <Knob
          name="Reverb"
          ariaLabel="Reverb"
          value={params.reverb}
          defaultValue={AMP_KNOB_DEFAULTS.reverb}
          onChange={(v) => onParam('reverb', v)}
          testId="knob-reverb"
        />

        {/* Cab lever + Power rocker only — a 5F1 has no bright switch. */}
        <AmpRight amp={amp} onToggle={onToggle} onTogglePower={onTogglePower} showBright={false} />
      </div>
    </div>
  );
}

// The Orange OR120 face — an early-70s "Overdrive" HEAD, the MID-FORWARD voice
// (docs §57). A knowing homage: "Overdrive" (model line HEAD Nº5 · ONE-TWENTY) ·
// a saturated ORANGE accent (--accent-orange). Control row: GAIN · BASS ·
// TREBLE · F.A.C. · H.F. BOOST · REVERB.
//
// What the face DOES NOT have is as load-bearing as what it does: there is NO
// MASTER (the OR120 has none — the single GAIN knob is the whole amp and the
// power section is the overdrive), NO MID (the James/Baxandall stack is bass +
// treble only), and NO BRIGHT switch (the brightness control is H.F. BOOST — and
// per §57's schematic correction that is a SERIES-RESONANT network at the
// driver's cathode, ~5.2 kHz, not a shelf in the feedback path). Hidden
// accordingly: middle, gain, master, bright, chorus.
//
// F.A.C. is a SIX-POSITION rotary on the real amp, and it is now drawn as one:
// a detented selector reading the panel's own position number. It used to be a
// continuous 0-100 knob that the core snapped to the nearest of six detents —
// the audio was right, but the readout named nothing and the travel gave no clue
// where the clicks were (measured: 17.2 dB of low-E across the switch).
// The PANEL NAMES come from the Field Guide (docs §57.11): the OR120 reads
// Input - F.A.C. - Bass - Treble - H.F.Boost - Gain - Reverb. So the volume
// control is printed GAIN and the presence control is printed H.F. BOOST. Both
// bind to the SHARED amp param slots (0 = volume, 11 = presence, the same slot
// the AC30 prints as TOP CUT) — the label is per voice, the slot is not, so the
// rig JSON, the testIds and the C ABI ids are untouched by the naming.
function OrangeFace({ amp, onParam, onToggle, onTogglePower }: AmpProps) {
  const { params } = amp;
  return (
    <div
      className={`amp raised orange${amp.engaged ? ' on' : ''}`}
      data-testid="amp"
      data-engaged={amp.engaged}
      data-amp-type="orange"
    >
      <div className="amp-head">
        <div className="amp-name display" data-testid="amp-name">
          Overdrive<small>Head Nº5 · One-Twenty</small>
        </div>
      </div>

      <div className="amp-controls">
        {/* The panel calls this GAIN, not VOLUME — see the comment above. The rig
            param and the testId stay `volume`, because they are the shared amp
            slot 0 that every voice writes; only the printed label is the OR120's. */}
        <Knob
          name="Gain"
          ariaLabel="Gain"
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
          name="Treble"
          ariaLabel="Treble"
          value={params.treble}
          defaultValue={AMP_KNOB_DEFAULTS.treble}
          onChange={(v) => onParam('treble', v)}
          testId="knob-treble"
        />
        <RotarySelector
          name="F.A.C."
          ariaLabel="Frequency Analysing Control — six-position rotary"
          positions={ORANGE_FAC_POSITIONS}
          value={params.fac}
          defaultValue={AMP_KNOB_DEFAULTS.fac}
          onChange={(v) => onParam('fac', v)}
          testId="knob-fac"
        />
        {/* H.F. BOOST on the panel. It binds to the shared 'presence' slot (id 11),
            the same slot the AC30 prints as TOP CUT — the label is per voice, the
            slot is not. It is NOT a presence control in the Marshall sense: §57's
            correction found a 1 k pot + 2 mH choke + 0.47 uF series-resonant at
            5191 Hz on the driver's cathode, not a shelf in the feedback path. */}
        <Knob
          name="H.F. Boost"
          ariaLabel="H.F. Boost"
          value={params.presence}
          defaultValue={AMP_KNOB_DEFAULTS.presence}
          onChange={(v) => onParam('presence', v)}
          testId="knob-presence"
        />
        <Knob
          name="Reverb"
          ariaLabel="Reverb"
          value={params.reverb}
          defaultValue={AMP_KNOB_DEFAULTS.reverb}
          onChange={(v) => onParam('reverb', v)}
          testId="knob-reverb"
        />

        {/* Cab lever + Power rocker only — no Bright (the OR120 has none). */}
        <AmpRight amp={amp} onToggle={onToggle} onTogglePower={onTogglePower} showBright={false} />
      </div>
    </div>
  );
}

// The Orange Rockerverb 100 face — the MODERN Orange head (docs §63), and the
// deliberate counterweight to the OR120 the way the OR120 is to the JCM800. A
// knowing homage: "Rocker Verb" (model line HEAD Nº6 · ONE-HUNDRED) · the same
// saturated ORANGE accent (--accent-orange) — it IS an Orange.
//
// Control row: GAIN · BASS · MIDDLE · TREBLE · VOLUME · REVERB. Every one of
// those differences from the OrangeFace above is a CIRCUIT difference, not a
// styling choice:
//   * there IS a MIDDLE, because this amp's stack is a Marshall-lineage FMV with
//     a real 25k mid pot — the OR120's James/Baxandall network has no mid at all;
//   * there IS a VOLUME, and it is a MASTER: it sits AFTER the tone stack, so
//     GAIN and level are independent. The OR120 has no master by design;
//   * there is NO F.A.C. (that is the vintage amp's rotary), NO H.F. BOOST and no
//     presence of any kind (the Rockerverb's panel has none), and no bright
//     switch or chorus.
//
// SLOT NOTE, and it matters for a stale rig state: the knob printed VOLUME binds
// to the shared `master` slot (id 12), NOT to `volume` (id 0). The panel word is
// the Rockerverb's; the slot is chosen by FUNCTION, and the function is a master
// volume — the same slot the JCM800 prints as MASTER. This voice never reads
// slot 0.
function RockerverbFace({ amp, onParam, onToggle, onTogglePower }: AmpProps) {
  const { params } = amp;
  return (
    <div
      className={`amp raised orange${amp.engaged ? ' on' : ''}`}
      data-testid="amp"
      data-engaged={amp.engaged}
      data-amp-type="rockerverb"
    >
      <div className="amp-head">
        <div className="amp-name display" data-testid="amp-name">
          Rocker Verb<small>Head Nº6 · One-Hundred</small>
        </div>
      </div>

      <div className="amp-controls">
        <Knob
          name="Gain"
          ariaLabel="Gain"
          value={params.gain}
          defaultValue={AMP_KNOB_DEFAULTS.gain}
          onChange={(v) => onParam('gain', v)}
          testId="knob-gain"
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
          name="Middle"
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
        {/* Printed VOLUME on the panel; bound to the shared MASTER slot (id 12)
            because that is what it is — see the comment above. */}
        <Knob
          name="Volume"
          ariaLabel="Volume"
          value={params.master}
          defaultValue={AMP_KNOB_DEFAULTS.master}
          onChange={(v) => onParam('master', v)}
          testId="knob-master"
        />
        <Knob
          name="Reverb"
          ariaLabel="Reverb"
          value={params.reverb}
          defaultValue={AMP_KNOB_DEFAULTS.reverb}
          onChange={(v) => onParam('reverb', v)}
          testId="knob-reverb"
        />

        {/* Cab lever + Power rocker only — no Bright. */}
        <AmpRight amp={amp} onToggle={onToggle} onTogglePower={onTogglePower} showBright={false} />
      </div>
    </div>
  );
}

// The Mesa/Boogie Dual Rectifier face (docs §69) — the GRUNGE / 90s-metal head,
// and the first amp voice in this repo TRANSCRIBED from a complete factory
// drawing set rather than reconstructed from prose. A knowing homage:
// "Dual Rectifier" (model line HEAD Nº7 · SOLO) on a slate panel.
//
// Control row: GAIN · BASS · MIDDLE · TREBLE · MASTER · PRESENCE · MODE ·
// RECTIFIER · POWER. The three switches are rendered as knobs, exactly as the
// OR120's six-position F.A.C. is, and the C ABI quantizes them at its boundary
// so the model never sees an in-between state.
//
// WHAT EACH SWITCH ACTUALLY DOES, because two of them are routinely misdescribed:
//   * MODE has FIVE positions off the drawing's own truth table (sheet mbdr7) —
//     Clean · Vintage · Modern · Red Vintage · Red Modern. It is NOT a channel
//     plus a mode: the sheet enumerates the combinations that exist, and two
//     conceivable ones (RED CLEAN, ORANGE VINTAGE) do not. The two MODERN
//     positions switch the power amp's global feedback OFF entirely.
//   * RECTIFIER is silicon vs 5U4 valve. It changes the rail AND its source
//     impedance, so it moves SAG, not level alone.
//   * POWER is SPONGY vs BOLD — a SEPARATE mains-primary-side switch. It is not
//     the rectifier selector, though it is constantly confused with one.
//
// PRESENCE binds to the shared slot (id 11), like the JCM's. In the two MODERN
// modes the feedback loop is OPEN, so the knob correctly does nothing there —
// that is the circuit, not dead UI.
function MesaFace({ amp, onParam, onToggle, onTogglePower }: AmpProps) {
  const { params } = amp;
  return (
    <div
      className={`amp raised${amp.engaged ? ' on' : ''}`}
      data-testid="amp"
      data-engaged={amp.engaged}
      data-amp-type="mesa"
    >
      <div className="amp-head">
        <div className="amp-name display" data-testid="amp-name">
          Dual Rectifier<small>Head Nº7 · Solo</small>
        </div>
      </div>

      <div className="amp-controls">
        <Knob
          name="Gain"
          ariaLabel="Gain"
          value={params.gain}
          defaultValue={AMP_KNOB_DEFAULTS.gain}
          onChange={(v) => onParam('gain', v)}
          testId="knob-gain"
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
          name="Middle"
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
          name="Presence"
          ariaLabel="Presence"
          value={params.presence}
          defaultValue={AMP_KNOB_DEFAULTS.presence}
          onChange={(v) => onParam('presence', v)}
          testId="knob-presence"
        />
        {/* The three SWITCHES. All three are discrete in the model — MODE picks
            one of the five states sheet `mbdr7` enumerates, RECT and POWER are
            two-position — so none of them is a pot. MODE has five positions,
            which is past what a segmented switch can show, so it takes the
            detented rotary; the two-state pair take the carved switch the
            silverface Twin already uses. */}
        <RotarySelector
          name="Mode"
          ariaLabel="Mode: Clean, Vintage, Modern, Red Vintage, Red Modern"
          positions={MESA_MODES}
          value={params.mesaMode}
          defaultValue={AMP_KNOB_DEFAULTS.mesaMode}
          onChange={(v) => onParam('mesaMode', v)}
          testId="knob-mesa-mode"
        />
        <SegmentSwitch
          name="Rect"
          ariaLabel="Rectifier: silicon or 5U4 valve"
          positions={MESA_RECTIFIERS}
          value={params.rectifier}
          onChange={(v) => onParam('rectifier', v)}
          testId="switch-mesa-rectifier"
        />
        <SegmentSwitch
          name="Power"
          ariaLabel="Power mode: bold or spongy"
          positions={MESA_POWER_MODES}
          value={params.powerMode}
          onChange={(v) => onParam('powerMode', v)}
          testId="switch-mesa-power"
        />

        {/* Cab lever + Power rocker only — a Recto has no bright switch and no
            reverb tank (that is the Trem-O-Verb, a different amp). */}
        <AmpRight amp={amp} onToggle={onToggle} onTogglePower={onTogglePower} showBright={false} />
      </div>
    </div>
  );
}

export function Amp(props: AmpProps) {
  return (
    <div className="amp-wing">
      {props.amp.type === 'jcm800' ? (
        <Jcm800Face {...props} />
      ) : props.amp.type === 'twin' ? (
        <TwinFace {...props} />
      ) : props.amp.type === 'ac30' ? (
        <Ac30Face {...props} />
      ) : props.amp.type === 'orange' ? (
        <OrangeFace {...props} />
      ) : props.amp.type === 'rockerverb' ? (
        <RockerverbFace {...props} />
      ) : props.amp.type === 'mesa' ? (
        <MesaFace {...props} />
      ) : props.amp.type === 'champ' ? (
        <ChampFace {...props} />
      ) : (
        <Clean120Face {...props} />
      )}
    </div>
  );
}
