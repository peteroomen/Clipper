// Knob — the approved design's full anatomy (knurled skirt, domed cap, ink
// pointer, red value arc floating off the body) bound to a 0..1 value.
//
// Interactions ported from the design artifact:
//   - drag vertically with pointer capture (up = increase)
//   - mouse wheel
//   - double-click resets to a per-knob default
//   - ArrowUp/Right / ArrowDown/Left when focused
//   - a very quiet tick as the value crosses a detent
//
// The value arc + pointer read `--deg` (0..270°, from 225°) which inherits from
// the .k-stack element. It is a controlled component: `value` comes from props
// and every change is reported through `onChange` so it can flow to the worklet.

import { useCallback, useEffect, useRef } from 'react';
import type { PointerEvent as ReactPointerEvent, KeyboardEvent as ReactKeyboardEvent, CSSProperties } from 'react';
import { tick } from '../ui-sound';

const RANGE_PX = 160; // vertical pixels for a full 0..1 sweep
const WHEEL_STEP = 0.03;
const KEY_STEP = 0.05;
const TICK_DETENT = 0.04;

export interface KnobProps {
  value: number; // 0..1
  defaultValue: number; // 0..1, snap target for double-click
  name: string; // short label shown under the knob (e.g. "Dist")
  ariaLabel: string; // accessible name (e.g. "Distortion")
  onChange: (value: number) => void;
  testId?: string;
}

// NaN-rejecting (2026-07-24 audit, finding 1): Math.min/Math.max pass NaN through,
// so a non-finite value (a bad persisted rig, a programmatic setter) would reach
// the engine AND leave the knob's rotation transform unrenderable.
const clamp01 = (v: number) => Math.min(1, Math.max(0, Number.isFinite(v) ? v : 0));

export function Knob({ value, defaultValue, name, ariaLabel, onChange, testId }: KnobProps) {
  const rootRef = useRef<HTMLDivElement>(null);

  // Refs mirror the latest props so the native (non-passive) wheel listener and
  // the stable `apply` callback never read stale closure values.
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

  // Wheel needs a non-passive listener to call preventDefault; React's onWheel
  // is passive, so attach natively.
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

  const readout = Math.round(value * 100);
  const deg = (value * 270).toFixed(1);

  return (
    <div
      ref={rootRef}
      className="knob"
      role="slider"
      tabIndex={0}
      aria-label={ariaLabel}
      aria-valuemin={0}
      aria-valuemax={100}
      aria-valuenow={readout}
      aria-valuetext={`${readout}%`}
      data-testid={testId}
      data-value={value}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={endDrag}
      onPointerCancel={endDrag}
      onDoubleClick={() => apply(defaultValue)}
      onKeyDown={onKeyDown}
    >
      <div className="k-stack" style={{ '--deg': deg } as CSSProperties}>
        <div className="k-arc" />
        <div className="k-body" />
        <div className="k-knurl" />
        <div className="k-cap" />
        <div className="k-ptr" />
      </div>
      <span className="k-name">{name}</span>
      <span className="k-val mono" data-testid={testId ? `${testId}-value` : undefined}>
        {readout}
      </span>
    </div>
  );
}
