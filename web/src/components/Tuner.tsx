// Tuner (M7) — a chromatic needle tuner in the CLIPPER pedal format.
//
//   model line "TUNE Nº0 · CHROMATIC" · lock LED (green in tune / red off) ·
//   big note name + octave · an SVG arc needle that sweeps with the cents
//   deviation (smoothed to ~60 fps) · a ±cents readout · a footswitch that MUTES
//   the chain (true tuner-pedal behavior: stomp on to tune in silence).
//
// The reading comes from the main-thread pitch detector (see audio.ts / tuner.ts);
// this component only renders it. Reference pitch is fixed at A=440. Detection only
// runs while the tuner is engaged (muting), so when disengaged the needle rests at
// center and the note reads "—".

import { useEffect, useRef, useState } from 'react';
import { thunk } from '../ui-sound';
import { IN_TUNE_CENTS, type TunerReading } from '../tuner';

export interface TunerProps {
  reading: TunerReading | null; // latest detection (null = no clear pitch)
  engaged: boolean; // true = tuner active (muting the chain)
  onToggleEngaged: () => void;
}

// Needle sweep: ±50 cents maps to ±48° from vertical.
const MAX_CENTS = 50;
const MAX_ANGLE = 48;
const centsToAngle = (c: number) => (Math.max(-MAX_CENTS, Math.min(MAX_CENTS, c)) / MAX_CENTS) * MAX_ANGLE;

// Hold the green lock for a beat once in tune, so it doesn't flicker on the edge.
const LOCK_HOLD_MS = 350;

type Led = 'off' | 'red' | 'green';

export function Tuner({ reading, engaged, onToggleEngaged }: TunerProps) {
  const readingRef = useRef<TunerReading | null>(reading);
  const engagedRef = useRef(engaged);
  readingRef.current = reading;
  engagedRef.current = engaged;

  const needleRef = useRef<SVGGElement>(null);
  const angleRef = useRef(0); // current (eased) needle angle
  const inTuneSinceRef = useRef<number | null>(null);
  const ledStateRef = useRef<Led>('off');
  const [led, setLed] = useState<Led>('off');

  // One rAF loop drives the smooth needle sweep and the lock LED (both need
  // continuous time, independent of how often readings arrive).
  useEffect(() => {
    let raf = 0;
    const tick = (t: number) => {
      const r = engagedRef.current ? readingRef.current : null;
      const target = r ? centsToAngle(r.cents) : 0;
      angleRef.current += (target - angleRef.current) * 0.28; // ease
      const a = angleRef.current;
      needleRef.current?.setAttribute('transform', `rotate(${a.toFixed(2)} 100 104)`);

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
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, []);

  const showReading = engaged && reading != null;
  const noteText = showReading ? reading!.note : '—';
  const octText = showReading ? String(reading!.octave) : '';
  const cents = showReading ? reading!.cents : 0;
  const centsLabel = showReading ? `${cents > 0 ? '+' : ''}${cents}` : '––';
  const flatSharp = !showReading || cents === 0 ? '' : cents < 0 ? 'flat' : 'sharp';

  return (
    <div
      className={`pedal tuner raised${engaged ? ' on' : ''}`}
      data-testid="tuner"
      data-engaged={engaged}
      data-note={showReading ? `${reading!.note}${reading!.octave}` : ''}
      data-cents={showReading ? cents : ''}
    >
      <div className="pedal-top">
        <span className="pedal-model">TUNE Nº0 · CHROMATIC</span>
        <span className={`led tuner-led ${led}`} data-testid="tuner-led" data-led={led} aria-hidden="true" />
      </div>

      <div className="tuner-note-row">
        <span className="tuner-note display" data-testid="tuner-note">
          {noteText}
        </span>
        <span className="tuner-oct mono">{octText}</span>
      </div>

      <svg className="tuner-gauge" viewBox="0 0 200 118" data-testid="tuner-gauge" aria-hidden="true">
        {/* Track arc (radius 84 about pivot 100,104), spanning ±MAX_ANGLE. */}
        <path className="gauge-track" d={arc(100, 104, 84, -MAX_ANGLE, MAX_ANGLE)} />
        {/* In-tune center zone. */}
        <path
          className="gauge-zone"
          d={arc(100, 104, 84, -centsToAngle(IN_TUNE_CENTS), centsToAngle(IN_TUNE_CENTS))}
        />
        {/* Ticks. */}
        {[-50, -25, -10, 0, 10, 25, 50].map((c) => {
          const a = (centsToAngle(c) * Math.PI) / 180;
          const r0 = c === 0 ? 68 : 74;
          const sx = 100 + Math.sin(a) * r0;
          const sy = 104 - Math.cos(a) * r0;
          const ex = 100 + Math.sin(a) * 84;
          const ey = 104 - Math.cos(a) * 84;
          return <line key={c} className={`gauge-tick${c === 0 ? ' center' : ''}`} x1={sx} y1={sy} x2={ex} y2={ey} />;
        })}
        {/* Needle (rotated each frame by the rAF loop). */}
        <g ref={needleRef} transform="rotate(0 100 104)">
          <line className="gauge-needle" x1="100" y1="104" x2="100" y2="22" />
          <circle className="gauge-hub" cx="100" cy="104" r="6" />
        </g>
      </svg>

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

// Build an SVG arc path between two angles (degrees from vertical, + = clockwise)
// on a circle centered at (cx,cy) with the given radius.
function arc(cx: number, cy: number, r: number, a0: number, a1: number): string {
  const p = (deg: number) => {
    const a = (deg * Math.PI) / 180;
    return [cx + Math.sin(a) * r, cy - Math.cos(a) * r];
  };
  const [x0, y0] = p(a0);
  const [x1, y1] = p(a1);
  const large = Math.abs(a1 - a0) > 180 ? 1 : 0;
  return `M ${x0.toFixed(2)} ${y0.toFixed(2)} A ${r} ${r} 0 ${large} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
}
