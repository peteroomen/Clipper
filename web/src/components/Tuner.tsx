// Tuner (M7 · restyled M6.8) — a chromatic tuner in the CLIPPER pedal format,
// re-skinned "TC-style": a segmented LED meter and a segmented-display note
// readout, both in the shared neumorphic chassis.
//
//   model line "TUNE Nº0 · CHROMATIC" · lock LED (green in tune / red off) ·
//   a big 7-SEGMENT note letter on a recessed dark "screen" (unlit segments stay
//   faintly visible) with an octave + sharp indicator · a SEGMENTED METER — a
//   horizontal strip of discrete LEDs in recessed neumorphic wells: RED segments
//   spreading left (flat) / right (sharp) as a bar from center, a distinct GREEN
//   center segment that lights when |cents| ≤ 3 (with a short hold) · a ±cents
//   readout · a footswitch that MUTES the chain (stomp on to tune in silence).
//
// The reading comes from the main-thread pitch detector (see audio.ts / tuner.ts);
// this component only renders it. Reference pitch is fixed at A=440. Detection only
// runs while the tuner is engaged (muting), so when disengaged the meter rests dark
// and the note reads "—".

import { useEffect, useRef, useState } from 'react';
import { thunk } from '../ui-sound';
import { IN_TUNE_CENTS, type TunerReading } from '../tuner';

export interface TunerProps {
  reading: TunerReading | null; // latest detection (null = no clear pitch)
  engaged: boolean; // true = tuner active (muting the chain)
  onToggleEngaged: () => void;
}

// Segmented meter: 5 red wells per side + 1 green center = 11 discrete segments.
// Full scale is ±50 cents, so each side segment spans ~10 cents; lit segments form
// a bar from center outward toward the deviation (more lit = further out of tune).
const SEG_PER_SIDE = 5;
const MAX_CENTS = 50;
const SEG_INDICES = [-5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5]; // left(flat) → center → right(sharp)

// Hold the green lock for a beat once in tune, so it doesn't flicker on the edge.
const LOCK_HOLD_MS = 350;

type Led = 'off' | 'red' | 'green';

// 7-segment maps (a,b,c,d,e,f,g) for the seven chromatic note letters. B and D use
// the lowercase forms so all seven letters read distinctly on a 7-seg display; '-'
// is the no-signal placeholder (middle bar only).
const SEG7: Record<string, string> = {
  A: 'abcefg',
  B: 'cdefg', // lowercase b
  C: 'adef',
  D: 'bcdeg', // lowercase d
  E: 'adefg',
  F: 'aefg',
  G: 'acdef',
  '-': 'g',
};

export function Tuner({ reading, engaged, onToggleEngaged }: TunerProps) {
  const readingRef = useRef<TunerReading | null>(reading);
  const engagedRef = useRef(engaged);
  readingRef.current = reading;
  engagedRef.current = engaged;

  // Meter segments are driven imperatively each frame (like the old needle) so the
  // ~60 fps update never churns React. Index maps SEG_INDICES position → element.
  const segRefs = useRef<Array<HTMLSpanElement | null>>([]);
  const centsEasedRef = useRef(0); // eased cents for a smooth bar sweep
  const inTuneSinceRef = useRef<number | null>(null);
  const ledStateRef = useRef<Led>('off');
  const [led, setLed] = useState<Led>('off');

  // One rAF loop eases the meter bar and drives the lock (both need continuous
  // time, independent of how often readings arrive).
  useEffect(() => {
    let raf = 0;
    const tick = (t: number) => {
      const r = engagedRef.current ? readingRef.current : null;
      const target = r ? Math.max(-MAX_CENTS, Math.min(MAX_CENTS, r.cents)) : 0;
      centsEasedRef.current += (target - centsEasedRef.current) * 0.28; // ease
      const c = centsEasedRef.current;

      // Lock LED / center state (same dwell logic the needle build used).
      let next: Led = 'off';
      if (r) {
        if (Math.abs(r.cents) <= IN_TUNE_CENTS) {
          if (inTuneSinceRef.current == null) inTuneSinceRef.current = t;
          next = t - inTuneSinceRef.current >= LOCK_HOLD_MS ? 'green' : 'red';
        } else {
          inTuneSinceRef.current = null;
          next = 'red';
        }
      } else {
        inTuneSinceRef.current = null;
      }
      if (next !== ledStateRef.current) {
        ledStateRef.current = next;
        setLed(next);
      }

      // Light the segments. Center is green only when locked; otherwise the red
      // bar grows from center outward on the flat (left) or sharp (right) side.
      const locked = next === 'green';
      const hasSignal = r != null;
      // number of lit red wells on the active side (0 when in tune / no signal)
      const litCount =
        hasSignal && !locked && Math.abs(c) > IN_TUNE_CENTS
          ? Math.min(SEG_PER_SIDE, Math.max(1, Math.round(Math.abs(c) / 10)))
          : 0;
      const sharp = c > 0;
      for (let k = 0; k < SEG_INDICES.length; k++) {
        const idx = SEG_INDICES[k]; // -5..5
        const el = segRefs.current[k];
        if (!el) continue;
        let on = false;
        if (idx === 0) {
          on = locked;
        } else if (litCount > 0) {
          const onThisSide = sharp ? idx > 0 : idx < 0;
          on = onThisSide && Math.abs(idx) <= litCount;
        }
        if (el.dataset.on !== String(on)) el.dataset.on = String(on);
      }
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, []);

  const showReading = engaged && reading != null;
  const letter = showReading ? reading!.note[0] : '-';
  const hasSharp = showReading && reading!.note.includes('#');
  const octText = showReading ? String(reading!.octave) : '';
  const cents = showReading ? reading!.cents : 0;
  const centsLabel = showReading ? `${cents > 0 ? '+' : ''}${cents}` : '––';
  const flatSharp = !showReading || cents === 0 ? '' : cents < 0 ? 'flat' : 'sharp';
  // Text carried for the a11y/test seam (this element must read '—' with no pitch).
  const noteText = showReading ? reading!.note : '—';

  return (
    <div
      className={`pedal tuner raised${engaged ? ' on' : ''}`}
      data-testid="tuner"
      data-engaged={engaged}
      data-pedal-type="tuner"
      data-face="tuner"
      data-note={showReading ? `${reading!.note}${reading!.octave}` : ''}
      data-cents={showReading ? cents : ''}
    >
      <div className="pedal-top">
        <span className="pedal-model">TUNE Nº0 · CHROMATIC</span>
        <span className={`led tuner-led ${led}`} data-testid="tuner-led" data-led={led} aria-hidden="true" />
      </div>

      {/* Recessed dark "screen": the big 7-seg note + octave + sharp indicator. */}
      <div className="tuner-screen">
        <div
          className="tuner-note-display"
          data-testid="tuner-note"
          role="img"
          aria-label={showReading ? `Note ${reading!.note}${octText}` : 'No pitch detected'}
        >
          <Seg7 char={letter} lit={showReading} />
          <span className={`seg-sharp${hasSharp ? ' on' : ''}`} aria-hidden="true" />
          <span className="vh">{noteText}</span>
        </div>
        <span className="tuner-oct mono" aria-hidden="true">
          {octText}
        </span>
      </div>

      {/* Segmented meter: red wells left/right, green center. */}
      <div className="tuner-meter" data-testid="tuner-meter" aria-hidden="true">
        {SEG_INDICES.map((idx, k) => (
          <span
            key={idx}
            ref={(el) => {
              segRefs.current[k] = el;
            }}
            className={`seg${idx === 0 ? ' seg-green' : ' seg-red'}`}
            data-seg={idx}
            data-on="false"
          />
        ))}
      </div>

      <div className="tuner-cents-row">
        <span className={`tuner-cents mono ${flatSharp}`} data-testid="tuner-cents">
          {centsLabel}
        </span>
        <span className="tuner-cents-unit mono">cents</span>
      </div>

      <div className="fsw-zone">
        <button
          type="button"
          className="fsw"
          role="switch"
          aria-checked={engaged}
          aria-label="Tuner mute footswitch"
          data-testid="tuner-footswitch"
          onPointerDown={() => thunk(true)}
          onClick={() => {
            onToggleEngaged();
            thunk(false);
          }}
        />
        <span className="fsw-label">{engaged ? 'Muted' : 'Tune'}</span>
      </div>
    </div>
  );
}

// A single 7-segment digit as SVG. All seven segments always render; unlit ones
// stay faintly visible (the neumorphic/LCD charm), lit ones (per SEG7[char]) glow.
// `lit` false (no signal) shows the '-' placeholder.
function Seg7({ char, lit }: { char: string; lit: boolean }) {
  const on = lit ? SEG7[char] ?? SEG7['-'] : SEG7['-'];
  // Segment polygons on a 60×100 grid — clean hexagonal bars. a/g/d are the
  // horizontal bars (top/middle/bottom); f/b upper verticals, e/c lower verticals.
  const segs: Record<string, string> = {
    a: '14,8 46,8 50,12 46,16 14,16 10,12',
    b: '52,14 52,46 48,50 44,46 44,14 48,10',
    c: '52,54 52,86 48,90 44,86 44,54 48,50',
    d: '14,84 46,84 50,88 46,92 14,92 10,88',
    e: '8,54 12,50 16,54 16,86 12,90 8,86',
    f: '8,14 12,10 16,14 16,46 12,50 8,46',
    g: '14,46 46,46 50,50 46,54 14,54 10,50',
  };
  return (
    <svg className="seg7" viewBox="0 0 60 100" aria-hidden="true">
      {Object.entries(segs).map(([name, pts]) => (
        <polygon key={name} className={`seg7-part${on.includes(name) ? ' on' : ''}`} points={pts} />
      ))}
    </svg>
  );
}
