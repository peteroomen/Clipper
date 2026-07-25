// Knob — the approved design's full anatomy (knurled skirt, domed cap, ink
// pointer, red value arc floating off the body) bound to a 0..1 value.
//
// Interactions ported from the design artifact:
//   - drag vertically with pointer capture (up = increase)
//   - HOLD SHIFT for a fine drag (5× the travel per unit)
//   - mouse wheel (Shift = fine)
//   - double-click resets to a per-knob default
//   - ArrowUp/Right / ArrowDown/Left when focused (Shift = fine), PageUp/PageDown
//     for a coarse jump, Home/End for the ends of the range
//   - a very quiet tick as the value crosses a detent
//
// The value arc + pointer read `--deg` (0..270°, from 225°) which inherits from
// the .k-stack element. It is a controlled component: `value` comes from props
// and every change is reported through `onChange` so it can flow to the worklet.
//
// 2026-07-25 (audit UI/UX, docs §38) — this control was the audit's headline
// accessibility gap: "the most numerous tab stop and the only control with no
// focus ring, no fine-adjust (1.6 px/unit), and no Home/End/PageUp". The ring is
// in pedal.css (`.knob:focus-visible`); the rest is below.

import { useCallback, useEffect, useRef } from 'react';
import type { PointerEvent as ReactPointerEvent, KeyboardEvent as ReactKeyboardEvent, CSSProperties } from 'react';
import { tick } from '../ui-sound';

const RANGE_PX = 160; // vertical pixels for a full 0..1 sweep
// FINE drag: 5× the travel, i.e. 800 px for a full sweep (8.0 px per 1 %, against
// 1.6 px per 1 % coarse). At the coarse rate a single screen pixel is 0.6 % and a
// hand tremor is a tone change; at the fine rate the same 40 px gesture resolves
// 5 % instead of 25 %.
const FINE_RANGE_PX = 800;
const WHEEL_STEP = 0.03;
const FINE_WHEEL_STEP = 0.006;
const KEY_STEP = 0.05; // Arrow — the everyday nudge
const FINE_KEY_STEP = 0.01; // Shift+Arrow
const PAGE_STEP = 0.2; // PageUp/PageDown — the ARIA slider convention is a step
//                        materially larger than the arrows'
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

  // The drag is INCREMENTAL, not anchored to the press point. An anchored drag
  // (`startV + (startY - clientY) / range`) cannot express a mid-drag sensitivity
  // change: the instant Shift goes down the same pointer offset means a different
  // value and the knob jumps. Accumulating `dy / range` per move makes Shift a
  // live sensitivity control with no discontinuity. The cost is that overshoot at
  // an end is not remembered — dragging past 100 % and back comes straight off
  // the stop, which is how a real detent-less pot behaves anyway.
  const drag = useRef<{ active: boolean; lastY: number; acc: number }>({
    active: false,
    lastY: 0,
    acc: value,
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
      apply(valueRef.current - Math.sign(e.deltaY) * (e.shiftKey ? FINE_WHEEL_STEP : WHEEL_STEP));
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => el.removeEventListener('wheel', onWheel);
  }, [apply]);

  const onPointerDown = (e: ReactPointerEvent<HTMLDivElement>) => {
    drag.current = { active: true, lastY: e.clientY, acc: clamp01(valueRef.current) };
    e.currentTarget.setPointerCapture(e.pointerId);
    e.preventDefault();
  };
  const onPointerMove = (e: ReactPointerEvent<HTMLDivElement>) => {
    const d = drag.current;
    if (!d.active) return;
    const range = e.shiftKey ? FINE_RANGE_PX : RANGE_PX;
    d.acc = clamp01(d.acc + (d.lastY - e.clientY) / range);
    d.lastY = e.clientY;
    apply(d.acc);
  };
  const endDrag = (e: ReactPointerEvent<HTMLDivElement>) => {
    drag.current.active = false;
    try {
      e.currentTarget.releasePointerCapture(e.pointerId);
    } catch {
      /* capture may already be gone */
    }
  };
  // Full ARIA slider keyboard contract. `Home`/`End` slam to the ends of the
  // range and `PageUp`/`PageDown` take a coarse step; before this slice a keyboard
  // player needed 20 arrow presses to cross the sweep and had no way to reach an
  // end exactly (0.05 steps from an arbitrary persisted value never land on 0).
  const onKeyDown = (e: ReactKeyboardEvent<HTMLDivElement>) => {
    const step = e.shiftKey ? FINE_KEY_STEP : KEY_STEP;
    let next: number | null = null;
    switch (e.key) {
      case 'ArrowUp':
      case 'ArrowRight':
        next = valueRef.current + step;
        break;
      case 'ArrowDown':
      case 'ArrowLeft':
        next = valueRef.current - step;
        break;
      case 'PageUp':
        next = valueRef.current + PAGE_STEP;
        break;
      case 'PageDown':
        next = valueRef.current - PAGE_STEP;
        break;
      case 'Home':
        next = 0;
        break;
      case 'End':
        next = 1;
        break;
      default:
        return;
    }
    apply(next);
    e.preventDefault();
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
      aria-orientation="vertical"
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
