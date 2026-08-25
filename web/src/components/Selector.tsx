// Selector — the two controls a DISCRETE parameter deserves, neither of which is
// a 0-100 pot.
//
// WHY THIS FILE EXISTS. Several core parameters are quantized: the drop-tune's
// AMOUNT is a NINE-position rotary (docs §70.2), the Mesa's MODE is FIVE states
// off sheet `mbdr7` and its RECTIFIER / POWER are two each (docs §69), the
// OR120's F.A.C. is a six-position rotary (docs §57), and the Lumen / Swirl /
// Ensemble MODE slots are genuine two-position switches. Every one of them was
// drawn as a continuous Knob reading 0-100. The source comments already said
// "genuinely DISCRETE" and "a 9-position ROTARY, not a continuous control" — the
// WIDGET simply never followed, so the panel showed a number that named nothing
// and the player could not tell a detent from a position between two.
//
//   SegmentSwitch  — 2..3 states, the carved segmented switch the silverface
//                    Twin's tremolo and the Clean 120's chorus mode already use.
//   RotarySelector — 4+ states: a detented rotary whose readout is the position
//                    NAME, with hard detent stops and one click per position.
//
// BOTH EMIT ONLY EXACT DETENT VALUES. That is the property worth protecting: the
// core quantizes anyway, so a UI that emits 0.63 is not wrong so much as unable
// to tell you what it did. Positions carry their value explicitly rather than
// computing i/(n-1) at the call site, because the Mesa's five modes and the
// drop's nine detents are the ABI's own centres and must not drift from it.

import { useCallback, useEffect, useRef } from 'react';
import type { PointerEvent as ReactPointerEvent, KeyboardEvent as ReactKeyboardEvent, CSSProperties } from 'react';
import { tick, thunk } from '../ui-sound';

export interface SelectorPosition {
  label: string; // what the player reads — "OCT+DRY", "5U4", "Red Modern"
  value: number; // the exact 0..1 detent centre the core quantizes to
}

// Same NaN rejection as Knob: Math.min/Math.max pass NaN straight through, and a
// non-finite value from a bad persisted rig would otherwise reach the engine and
// leave the rotation transform unrenderable (2026-07-24 audit, finding 1).
const clamp01 = (v: number) => Math.min(1, Math.max(0, Number.isFinite(v) ? v : 0));

// Which detent a stored value belongs to: nearest centre, so a rig written by an
// older build (or a host automating the raw 0..1 lane) always resolves to a real
// position rather than rendering as "between".
export function indexOfValue(positions: readonly SelectorPosition[], value: number): number {
  const v = clamp01(value);
  let best = 0;
  let bestD = Infinity;
  for (let i = 0; i < positions.length; i++) {
    const d = Math.abs(positions[i].value - v);
    if (d < bestD) {
      bestD = d;
      best = i;
    }
  }
  return best;
}

// --- 2..3 states: the carved segmented switch --------------------------------
export function SegmentSwitch({
  positions,
  value,
  name,
  ariaLabel,
  onChange,
  testId,
}: {
  positions: readonly SelectorPosition[];
  value: number;
  name: string;
  ariaLabel: string;
  onChange: (value: number) => void;
  testId?: string;
}) {
  const idx = indexOfValue(positions, value);
  return (
    <div className="selector-switch" data-testid={testId} data-value={value}>
      <div className="mode-switch" role="radiogroup" aria-label={ariaLabel}>
        {positions.map((p, i) => (
          <button
            key={p.label}
            type="button"
            role="radio"
            aria-checked={i === idx}
            className={`mode-opt${i === idx ? ' on' : ''}`}
            data-testid={testId ? `${testId}-${i}` : undefined}
            onClick={() => {
              if (i !== idx) onChange(p.value);
              thunk(false);
            }}
          >
            {p.label}
          </button>
        ))}
      </div>
      <span className="k-name">{name}</span>
    </div>
  );
}

// --- 4+ states: a detented rotary that reads its position by NAME -------------
// Drag/wheel/arrows step one detent at a time and a plain click advances by one
// (wrapping), which is how a real rotary selector behaves under a finger. There
// is no continuous travel to land between detents in: `apply` takes an INDEX.
const DRAG_PX_PER_STEP = 26; // vertical pixels per detent

export function RotarySelector({
  positions,
  value,
  defaultValue,
  name,
  ariaLabel,
  onChange,
  testId,
}: {
  positions: readonly SelectorPosition[];
  value: number;
  defaultValue: number;
  name: string;
  ariaLabel: string;
  onChange: (value: number) => void;
  testId?: string;
}) {
  const rootRef = useRef<HTMLDivElement>(null);
  const idx = indexOfValue(positions, value);
  const idxRef = useRef(idx);
  idxRef.current = idx;
  const onChangeRef = useRef(onChange);
  onChangeRef.current = onChange;
  const posRef = useRef(positions);
  posRef.current = positions;

  const drag = useRef({ active: false, startY: 0, startIdx: 0, moved: false });

  // Clamped, never wrapped, on drag/wheel/arrows: a rotary selector has hard end
  // stops, and wrapping under a drag would make a full-travel gesture ambiguous.
  const applyIndex = useCallback((next: number) => {
    const list = posRef.current;
    const i = Math.min(list.length - 1, Math.max(0, next));
    if (i === idxRef.current) return;
    tick();
    onChangeRef.current(list[i].value);
  }, []);

  useEffect(() => {
    const el = rootRef.current;
    if (!el) return;
    // Non-passive so preventDefault actually stops the page scrolling, same as Knob.
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      applyIndex(idxRef.current + (e.deltaY < 0 ? 1 : -1));
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => el.removeEventListener('wheel', onWheel);
  }, [applyIndex]);

  const onPointerDown = (e: ReactPointerEvent<HTMLDivElement>) => {
    drag.current = { active: true, startY: e.clientY, startIdx: idxRef.current, moved: false };
    e.currentTarget.setPointerCapture(e.pointerId);
    e.preventDefault();
  };
  const onPointerMove = (e: ReactPointerEvent<HTMLDivElement>) => {
    if (!drag.current.active) return;
    const steps = Math.round((drag.current.startY - e.clientY) / DRAG_PX_PER_STEP);
    if (steps !== 0) drag.current.moved = true;
    applyIndex(drag.current.startIdx + steps);
  };
  const endDrag = (e: ReactPointerEvent<HTMLDivElement>) => {
    const wasDrag = drag.current.moved;
    drag.current.active = false;
    try {
      e.currentTarget.releasePointerCapture(e.pointerId);
    } catch {
      /* capture may already be gone */
    }
    // A click that never moved advances one position and WRAPS — the way you
    // click round a real selector rather than dragging it.
    if (!wasDrag) {
      const list = posRef.current;
      onChangeRef.current(list[(idxRef.current + 1) % list.length].value);
      tick();
    }
  };
  const onKeyDown = (e: ReactKeyboardEvent<HTMLDivElement>) => {
    if (e.key === 'ArrowUp' || e.key === 'ArrowRight') {
      applyIndex(idxRef.current + 1);
      e.preventDefault();
    } else if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') {
      applyIndex(idxRef.current - 1);
      e.preventDefault();
    } else if (e.key === 'Home') {
      applyIndex(0);
      e.preventDefault();
    } else if (e.key === 'End') {
      applyIndex(posRef.current.length - 1);
      e.preventDefault();
    }
  };

  // The pointer sits at the DETENT, not at the raw value, so it always aims at a
  // real position even if the stored value came from an older continuous build.
  const span = positions.length > 1 ? positions.length - 1 : 1;
  const deg = ((idx / span) * 270).toFixed(1);

  return (
    <div
      ref={rootRef}
      className="knob selector-rotary"
      role="slider"
      tabIndex={0}
      aria-label={ariaLabel}
      aria-valuemin={0}
      aria-valuemax={positions.length - 1}
      aria-valuenow={idx}
      aria-valuetext={positions[idx].label}
      data-testid={testId}
      data-value={value}
      data-position={idx}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={endDrag}
      onPointerCancel={endDrag}
      onDoubleClick={() => onChangeRef.current(defaultValue)}
      onKeyDown={onKeyDown}
    >
      <div className="k-stack" style={{ '--deg': deg } as CSSProperties}>
        <div className="k-arc" />
        <div className="k-body" />
        <div className="k-knurl" />
        <div className="k-cap" />
        <div className="k-ptr" />
        {/* One tick mark per position, so the travel is legible before you touch it. */}
        <div className="k-detents" aria-hidden="true">
          {positions.map((p, i) => (
            <span
              key={p.label}
              className={`k-detent${i === idx ? ' on' : ''}`}
              style={{ '--d': `${((i / span) * 270 - 135).toFixed(1)}deg` } as CSSProperties}
            />
          ))}
        </div>
      </div>
      <span className="k-name">{name}</span>
      <span className="k-val mono" data-testid={testId ? `${testId}-value` : undefined}>
        {positions[idx].label}
      </span>
    </div>
  );
}
