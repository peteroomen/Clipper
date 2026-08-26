// Fader — a vertical slider for the M13.6 graphic EQ's ten bands.
//
// A graphic EQ's control IS a fader: the whole point of the pedal is that you
// read the curve off the physical positions at a glance, which a row of knobs
// cannot give you. So this is a new widget rather than a restyled Knob.
//
// It deliberately mirrors Knob.tsx's interaction contract EXACTLY, because a
// board where two controls answer the pointer differently is worse than one
// with a single control type:
//   - drag vertically with pointer capture (up = increase)
//   - mouse wheel (non-passive listener, so preventDefault works)
//   - double-click resets to the default (0.5 == flat, for every band)
//   - ArrowUp/Right / ArrowDown/Left when focused
//   - the same quiet detent tick
//   - role="slider" with live aria-valuenow / aria-valuetext
//
// The readout is in dB rather than percent, because that is the unit printed on
// the reference and the unit the assistant coaches in. 0.5 reads "0.0 dB".

import { useCallback, useEffect, useRef } from 'react';
import type {
  PointerEvent as ReactPointerEvent,
  KeyboardEvent as ReactKeyboardEvent,
  CSSProperties,
} from 'react';
import { tick } from '../ui-sound';

const RANGE_PX = 160; // vertical pixels for a full 0..1 sweep — Knob's value
const WHEEL_STEP = 0.03;
const KEY_STEP = 0.05;
const TICK_DETENT = 0.04;

export interface FaderProps {
  value: number; // 0..1
  defaultValue: number; // 0..1, snap target for double-click
  name: string; // the band label, e.g. "125"
  unit?: string; // small suffix under the label, e.g. "Hz"
  ariaLabel: string;
  rangeDb: number; // +/- this many dB at the travel ends, for the readout
  onChange: (value: number) => void;
  testId?: string;
}

// NaN-rejecting, for the same reason Knob's is (2026-07-24 audit, finding 1):
// Math.min/Math.max pass NaN straight through.
const clamp01 = (v: number) => Math.min(1, Math.max(0, Number.isFinite(v) ? v : 0));

export function Fader({
  value,
  defaultValue,
  name,
  unit,
  ariaLabel,
  rangeDb,
  onChange,
  testId,
}: FaderProps) {
  const rootRef = useRef<HTMLDivElement>(null);
  const valueRef = useRef(value);
  valueRef.current = value;
  const onChangeRef = useRef(onChange);
  onChangeRef.current = onChange;
  const lastTick = useRef(value);

  const drag = useRef<{ active: boolean; startY: number; startV: number }>({
    active: false,
    startY: 0,
    startV: value,
  });

  const apply = useCallback((next: number) => {
    const nv = clamp01(next);
    if (Math.abs(nv - lastTick.current) >= TICK_DETENT) {
      lastTick.current = nv;
      tick();
    }
    onChangeRef.current(nv);
  }, []);

  useEffect(() => {
    const el = rootRef.current;
    if (!el) return;
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      apply(valueRef.current - Math.sign(e.deltaY) * WHEEL_STEP);
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => el.removeEventListener('wheel', onWheel);
  }, [apply]);

  const onPointerDown = (e: ReactPointerEvent<HTMLDivElement>) => {
    drag.current = { active: true, startY: e.clientY, startV: valueRef.current };
    e.currentTarget.setPointerCapture(e.pointerId);
    e.preventDefault();
  };
  const onPointerMove = (e: ReactPointerEvent<HTMLDivElement>) => {
    if (!drag.current.active) return;
    apply(drag.current.startV + (drag.current.startY - e.clientY) / RANGE_PX);
  };
  const endDrag = (e: ReactPointerEvent<HTMLDivElement>) => {
    drag.current.active = false;
    try {
      e.currentTarget.releasePointerCapture(e.pointerId);
    } catch {
      /* capture may already be gone */
    }
  };
  const onKeyDown = (e: ReactKeyboardEvent<HTMLDivElement>) => {
    if (e.key === 'ArrowUp' || e.key === 'ArrowRight') {
      apply(valueRef.current + KEY_STEP);
      e.preventDefault();
    } else if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') {
      apply(valueRef.current - KEY_STEP);
      e.preventDefault();
    }
  };

  const db = (value - 0.5) * 2 * rangeDb;
  const shown = Math.abs(db) < 0.05 ? '0.0' : `${db > 0 ? '+' : ''}${db.toFixed(1)}`;

  return (
    <div
      ref={rootRef}
      className="fader"
      role="slider"
      tabIndex={0}
      aria-label={ariaLabel}
      aria-valuemin={-rangeDb}
      aria-valuemax={rangeDb}
      aria-valuenow={Number(db.toFixed(1))}
      aria-valuetext={`${shown} dB`}
      data-testid={testId}
      data-value={value}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={endDrag}
      onPointerCancel={endDrag}
      onDoubleClick={() => apply(defaultValue)}
      onKeyDown={onKeyDown}
    >
      <div className="f-track" style={{ '--pos': value.toFixed(4) } as CSSProperties}>
        <div className="f-slot" />
        <div className="f-centre" aria-hidden="true" />
        <div className="f-fill" />
        <div className="f-cap" />
      </div>
      <span className="f-name">{name}</span>
      {unit ? <span className="f-unit">{unit}</span> : null}
      <span className="f-val mono" data-testid={testId ? `${testId}-value` : undefined}>
        {shown}
      </span>
    </div>
  );
}
