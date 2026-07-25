// Board (M6.4) — the pedalboard visual pass.
//
// Lays the rig out as a left-to-right signal chain: a guitar-in jack, the ordered
// pedal instances (stackable, drag- and keyboard-reorderable, each removable /
// swappable), then the amp fixed at the end. Consecutive units are joined by
// NEUMORPHIC CABLES — SVG catenary paths with a soft cast shadow and a specular
// top highlight, styled from the token palette so they read as rubber patch
// cables lying on the surface, not wires on a schematic. The cables are measured
// from the live DOM and redraw during drag / reorder / add / remove / resize.
//
// An empty chain (no pedals) is valid — the guitar runs straight into the amp and
// a single cable spans source -> amp.
//
// Drag reorder is hand-rolled on pointer events (pointer capture, live reorder) —
// the same proven pattern as Knob.tsx. We deliberately do NOT pull in @dnd-kit:
// this is a single horizontal list of a few items, the repo has zero UI deps, and
// avoiding the dependency keeps the offline build hermetic. Keyboard reorder is
// provided by explicit move-left / move-right buttons on each pedal.

import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';
import { Pedal } from './Pedal';
import { Tuner } from './Tuner';
import { Amp } from './Amp';
import { Menu } from './Menu';
import type { TunerReading } from '../tuner';
import {
  AVAILABLE_PEDAL_TYPES,
  AVAILABLE_AMP_TYPES,
  AVAILABLE_CABS,
  type PedalInstance,
  type PedalType,
  type AmpState,
  type AmpType,
  type ParamName,
  type AmpParamName,
  type CabChoice,
} from '../rig';

export interface BoardProps {
  pedals: PedalInstance[];
  amp: AmpState;
  // Latest tuner reading (M7), rendered by any tuner instance on the board.
  tunerReading: TunerReading | null;
  onPedalParam: (id: string, name: ParamName, value: number) => void;
  onPedalToggle: (id: string) => void;
  onAddPedal: (type?: PedalType, position?: number) => void;
  onRemovePedal: (id: string) => void;
  onSwapPedal: (id: string, type: PedalType) => void;
  onMovePedal: (from: number, to: number) => void;
  onAmpParam: (name: AmpParamName, value: number) => void;
  onAmpToggle: (name: 'bright' | 'cab') => void;
  onAmpPower: () => void;
  // M9.4: swap the amp voice (clean120 | jcm800).
  onAmpModelSelect: (type: AmpType) => void;
  onChorusMode: (mode: number) => void;
  // Cab expansion: pick a built-in/custom cab, or upload a user IR (.wav).
  onCabSelect: (cab: CabChoice) => void;
  onUploadIr: (file: File) => void;
}

// A friendly display name per pedal type (for the gear tray + swap menu).
const PEDAL_TYPE_LABEL: Record<PedalType, string> = {
  rat: 'RAT-type distortion',
  sd1: 'SD-type overdrive',
  ts: 'Screamer overdrive (green)',
  muff: 'Pi fuzz (violet, big box)',
  gold: 'Myth overdrive (gold, transparent)',
  phaser: 'Script phaser',
  tuner: 'Chromatic tuner',
};
const AMP_TYPE_LABEL: Record<string, string> = {
  clean120: 'Clean 120 (JC-120 style)',
  jcm800: 'JCM800 (Marshall style)',
  twin: 'Twin Sixty-Five (Fender style)',
  ac30: 'AC30 (Vox top boost)',
};
const CAB_LABEL: Record<'clean212' | 'brit412', string> = {
  clean212: 'Clean 2×12 (JC platform)',
  brit412: 'Brit 4×12 (Marshall-style)',
};

interface Segment {
  d: string;
  x1: number;
  y1: number;
  x2: number;
  y2: number;
}

export function Board({
  pedals,
  amp,
  tunerReading,
  onPedalParam,
  onPedalToggle,
  onAddPedal,
  onRemovePedal,
  onSwapPedal,
  onMovePedal,
  onAmpParam,
  onAmpToggle,
  onAmpPower,
  onAmpModelSelect,
  onChorusMode,
  onCabSelect,
  onUploadIr,
}: BoardProps) {
  const boardRef = useRef<HTMLDivElement>(null);
  const [segments, setSegments] = useState<Segment[]>([]);
  const [svgSize, setSvgSize] = useState({ w: 0, h: 0 });
  const [draggingId, setDraggingId] = useState<string | null>(null);
  const [trayOpen, setTrayOpen] = useState(false);
  const [ampMenuOpen, setAmpMenuOpen] = useState(false);
  const [swapOpenId, setSwapOpenId] = useState<string | null>(null);

  const dragRef = useRef<{ id: string; pointerId: number } | null>(null);
  // The user-IR file input, kept out of the popover (see the render site).
  const irInputRef = useRef<HTMLInputElement>(null);

  // Build one catenary path between two jack centers (screen coords relative to
  // the board). Control points pull DOWN (gravity droop); sag grows with span.
  const cablePath = (x1: number, y1: number, x2: number, y2: number): Segment => {
    const dx = x2 - x1;
    const sag = Math.min(70, Math.max(16, Math.abs(dx) * 0.16 + 14));
    const lowY = Math.max(y1, y2) + sag;
    const cx1 = x1 + dx * 0.28;
    const cx2 = x1 + dx * 0.72;
    return { d: `M ${x1} ${y1} C ${cx1} ${lowY}, ${cx2} ${lowY}, ${x2} ${y2}`, x1, y1, x2, y2 };
  };

  // Measure jack positions from the live DOM and rebuild the cable segments. A
  // cable joins each unit's OUT jack to the next unit's IN jack:
  //   source.out -> pedal0.in -> (pedal0.out -> pedal1.in) -> ... -> lastOut -> amp.in
  const measure = useCallback(() => {
    const board = boardRef.current;
    if (!board) return;
    const brect = board.getBoundingClientRect();
    const center = (el: Element | null): { x: number; y: number } | null => {
      if (!el) return null;
      const r = el.getBoundingClientRect();
      // A jack that is present in the DOM but NOT LAID OUT (an ancestor with
      // `display: none`) reports an all-zero DOMRect, and the null check above
      // happily accepts it: the returned point is {-brect.left, -brect.top}, i.e.
      // the viewport origin expressed in board coordinates. That is a perfectly
      // truthy point, so a cable was drawn from off the board to the first pedal
      // (2026-07-24 audit, UI/UX). It is live below 760 px, where board.css hides
      // `.board-source` — the audit says the stray cable comes from "below", but
      // the viewport origin is above and left of the board, which is where it
      // actually renders. Either way: an unlaid-out jack has no position, so say
      // so and let the caller skip the segment.
      if (r.width === 0 && r.height === 0) return null;
      return { x: r.left + r.width / 2 - brect.left, y: r.top + r.height / 2 - brect.top };
    };
    const q = (sel: string) => center(board.querySelector(sel));

    // Ordered chain of OUT points followed by the next IN point.
    const outs: Array<{ x: number; y: number } | null> = [q('.board-source .jack-out')];
    const ins: Array<{ x: number; y: number } | null> = [];
    for (const p of pedals) {
      ins.push(q(`[data-unit-id="${CSS.escape(p.id)}"] .jack-in`));
      outs.push(q(`[data-unit-id="${CSS.escape(p.id)}"] .jack-out`));
    }
    ins.push(q('.board-amp .jack-in'));

    const segs: Segment[] = [];
    for (let i = 0; i < ins.length; i++) {
      const a = outs[i];
      const b = ins[i];
      if (a && b) segs.push(cablePath(a.x, a.y, b.x, b.y));
    }
    setSegments(segs);
    setSvgSize({ w: brect.width, h: brect.height });
  }, [pedals]);

  // Remeasure on layout changes (chain edits) and whenever the board resizes.
  useLayoutEffect(() => {
    measure();
  }, [measure, amp.engaged]);

  useEffect(() => {
    const board = boardRef.current;
    if (!board || typeof ResizeObserver === 'undefined') return;
    const ro = new ResizeObserver(() => measure());
    ro.observe(board);
    // Fonts / async layout can shift positions after first paint.
    const t = setTimeout(measure, 120);
    window.addEventListener('resize', measure);
    return () => {
      ro.disconnect();
      clearTimeout(t);
      window.removeEventListener('resize', measure);
    };
  }, [measure]);

  // ---- drag reorder (hand-rolled) ----
  const onGripDown = (e: React.PointerEvent, id: string) => {
    e.preventDefault();
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    dragRef.current = { id, pointerId: e.pointerId };
    setDraggingId(id);
  };
  const onGripMove = (e: React.PointerEvent) => {
    const drag = dragRef.current;
    const board = boardRef.current;
    if (!drag || !board) return;
    const units = Array.from(board.querySelectorAll<HTMLElement>('.board-unit.is-pedal'));
    const fromIndex = units.findIndex((u) => u.dataset.unitId === drag.id);
    if (fromIndex < 0) return;
    let to = units.length - 1;
    for (let i = 0; i < units.length; i++) {
      const r = units[i].getBoundingClientRect();
      if (e.clientX < r.left + r.width / 2) {
        to = i;
        break;
      }
    }
    if (to !== fromIndex) {
      onMovePedal(fromIndex, to);
      // Redraw on the next frame once React has committed the new order.
      requestAnimationFrame(measure);
    }
  };
  const onGripUp = (e: React.PointerEvent) => {
    try {
      (e.currentTarget as HTMLElement).releasePointerCapture(e.pointerId);
    } catch {
      /* capture may already be gone */
    }
    dragRef.current = null;
    setDraggingId(null);
    requestAnimationFrame(measure);
  };

  return (
    <div className="board" ref={boardRef} data-testid="board">
      {/* Cable layer — sits BEHIND the units (see board.css z-index). */}
      <svg
        className="board-cables"
        data-testid="board-cables"
        width={svgSize.w}
        height={svgSize.h}
        viewBox={`0 0 ${svgSize.w} ${svgSize.h}`}
        aria-hidden="true"
      >
        {segments.map((s, i) => (
          <g className="cable" key={i}>
            <path className="cable-body" d={s.d} />
            <path className="cable-hi" d={s.d} />
            <circle className="cable-plug" cx={s.x1} cy={s.y1} r={5} />
            <circle className="cable-plug" cx={s.x2} cy={s.y2} r={5} />
          </g>
        ))}
      </svg>

      <div className="board-track">
        {/* Guitar-in source jack */}
        <div className="board-source" data-testid="board-source">
          <span className="src-label mono">In</span>
          <span className="jack jack-out" aria-hidden="true" />
        </div>

        {pedals.map((p, i) => (
          <div
            className={`board-unit is-pedal${draggingId === p.id ? ' dragging' : ''}`}
            data-unit-id={p.id}
            data-testid={`board-unit-${i}`}
            key={p.id}
          >
            <span className="jack jack-in" aria-hidden="true" />
            <span className="jack jack-out" aria-hidden="true" />

            {/* Chain-position control rack (float above the pedal; does not shift
                layout). Grip = drag; ◀ ▶ = keyboard-accessible reorder; swap; ✕. */}
            <div className="unit-rack" data-testid={`unit-rack-${i}`}>
              <button
                type="button"
                className="rack-grip"
                aria-label={`Drag to reorder ${p.type} pedal`}
                data-testid={`pedal-grip-${i}`}
                onPointerDown={(e) => onGripDown(e, p.id)}
                onPointerMove={onGripMove}
                onPointerUp={onGripUp}
                onPointerCancel={onGripUp}
              >
                <span className="grip-dots" aria-hidden="true" />
              </button>
              <span className="rack-pos mono" aria-hidden="true">
                {i + 1}
              </span>
              <button
                type="button"
                className="rack-btn"
                aria-label={`Move ${p.type} pedal earlier in the chain`}
                data-testid={`pedal-move-left-${i}`}
                disabled={i === 0}
                onClick={() => onMovePedal(i, i - 1)}
              >
                ◀
              </button>
              <button
                type="button"
                className="rack-btn"
                aria-label={`Move ${p.type} pedal later in the chain`}
                data-testid={`pedal-move-right-${i}`}
                disabled={i === pedals.length - 1}
                onClick={() => onMovePedal(i, i + 1)}
              >
                ▶
              </button>
              <div className="rack-swap">
                <Menu
                  open={swapOpenId === p.id}
                  onToggle={() => setSwapOpenId((cur) => (cur === p.id ? null : p.id))}
                  onClose={() => setSwapOpenId(null)}
                  triggerClassName="rack-btn"
                  triggerAriaLabel={`Swap ${p.type} pedal`}
                  triggerTestId={`pedal-swap-${i}`}
                  triggerContent="⇄"
                  menuClassName="unit-menu"
                  menuTestId={`pedal-swap-menu-${i}`}
                >
                  <div className="unit-menu-head">Swap for</div>
                  {AVAILABLE_PEDAL_TYPES.map((t) => (
                    <button
                      key={t}
                      type="button"
                      role="menuitemradio"
                      aria-checked={t === p.type}
                      className={`unit-menu-item${t === p.type ? ' current' : ''}`}
                      data-testid={`pedal-swap-${i}-${t}`}
                      onClick={() => {
                        if (t !== p.type) onSwapPedal(p.id, t);
                        setSwapOpenId(null);
                      }}
                    >
                      {PEDAL_TYPE_LABEL[t]}
                      {t === p.type ? ' ✓' : ''}
                    </button>
                  ))}
                  <div className="unit-menu-note">More pedals coming (BD-2, DD-3…)</div>
                </Menu>
              </div>
              <button
                type="button"
                className="rack-btn rack-remove"
                aria-label={`Remove ${p.type} pedal`}
                data-testid={`pedal-remove-${i}`}
                onClick={() => onRemovePedal(p.id)}
              >
                ✕
              </button>
            </div>

            {p.type === 'tuner' ? (
              <Tuner
                reading={tunerReading}
                engaged={p.engaged}
                onToggleEngaged={() => onPedalToggle(p.id)}
              />
            ) : (
              <Pedal
                pedal={p}
                onParam={(name, value) => onPedalParam(p.id, name, value)}
                onToggleEngaged={() => onPedalToggle(p.id)}
              />
            )}
          </div>
        ))}

        {/* Gear tray — add a pedal before the amp. */}
        <div className="board-tray" data-testid="gear-tray">
          <Menu
            open={trayOpen}
            onToggle={() => setTrayOpen((v) => !v)}
            onClose={() => setTrayOpen(false)}
            triggerClassName="tray-add"
            triggerAriaLabel="Add a pedal"
            triggerTestId="add-pedal"
            triggerContent={
              <>
                <span className="tray-plus" aria-hidden="true">
                  +
                </span>
                <span className="tray-text">Add pedal</span>
              </>
            }
            menuClassName="unit-menu tray-menu"
            menuTestId="add-pedal-menu"
            menuAriaLabel="Available pedals"
          >
            <div className="unit-menu-head">Available pedals</div>
            {AVAILABLE_PEDAL_TYPES.map((t) => (
              <button
                key={t}
                type="button"
                role="menuitem"
                className="unit-menu-item"
                data-testid={`add-pedal-${t}`}
                onClick={() => {
                  onAddPedal(t);
                  setTrayOpen(false);
                }}
              >
                {PEDAL_TYPE_LABEL[t]}
              </button>
            ))}
            <div className="unit-menu-note">More coming: BD-2, DD-3…</div>
          </Menu>
          {pedals.length === 0 && (
            <span className="tray-empty mono" data-testid="empty-chain-note">
              Empty chain — guitar straight into the amp
            </span>
          )}
        </div>

        {/* Amp — fixed at the end of the chain. */}
        <div className="board-unit board-amp" data-unit-id="amp" data-testid="board-amp">
          <span className="jack jack-in" aria-hidden="true" />
          <div className="amp-slot-head">
            {/* The file input lives OUTSIDE the popover on purpose. It used to be a
                child of the menu, so any state change that closed the menu while
                the OS file dialog was still open would unmount the input mid-
                dialog and silently drop the selection. Out here it also survives
                the menu closing the instant the button is pressed. */}
            <input
              ref={irInputRef}
              type="file"
              accept=".wav,audio/wav,audio/x-wav,audio/*"
              className="vh-input"
              tabIndex={-1}
              aria-hidden="true"
              data-testid="cab-upload-input"
              onChange={(e) => {
                const f = e.target.files?.[0];
                e.target.value = ''; // allow re-uploading the same file
                if (f) {
                  onUploadIr(f);
                  setAmpMenuOpen(false);
                }
              }}
            />
            <Menu
              open={ampMenuOpen}
              onToggle={() => setAmpMenuOpen((v) => !v)}
              onClose={() => setAmpMenuOpen(false)}
              triggerClassName="amp-slot-select"
              triggerAriaLabel="Change amp"
              triggerTestId="amp-select"
              triggerContent={
                <>
                  {AMP_TYPE_LABEL[amp.type]} <span aria-hidden="true">▾</span>
                </>
              }
              menuClassName="unit-menu"
              menuTestId="amp-menu"
              menuAriaLabel="Amp and cabinet"
            >
              <>
                <div className="unit-menu-head">Amp</div>
                {AVAILABLE_AMP_TYPES.map((t) => (
                  <button
                    key={t}
                    type="button"
                    role="menuitemradio"
                    aria-checked={t === amp.type}
                    className={`unit-menu-item${t === amp.type ? ' current' : ''}`}
                    data-testid={`amp-${t}`}
                    onClick={() => {
                      if (t !== amp.type) onAmpModelSelect(t);
                      setAmpMenuOpen(false);
                    }}
                  >
                    {AMP_TYPE_LABEL[t]}
                    {t === amp.type ? ' ✓' : ''}
                  </button>
                ))}

                {/* Cab expansion: pick the speaker cab IR or upload a user IR. */}
                <div className="unit-menu-head">Cab</div>
                {AVAILABLE_CABS.map((c) => (
                  <button
                    key={c}
                    type="button"
                    role="menuitemradio"
                    aria-checked={amp.cabModel === c}
                    className={`unit-menu-item${amp.cabModel === c ? ' current' : ''}`}
                    data-testid={`cab-${c}`}
                    onClick={() => {
                      onCabSelect(c);
                      setAmpMenuOpen(false);
                    }}
                  >
                    {CAB_LABEL[c]}
                    {amp.cabModel === c ? ' ✓' : ''}
                  </button>
                ))}
                {amp.customCabLabel && (
                  <button
                    type="button"
                    role="menuitemradio"
                    aria-checked={amp.cabModel === 'custom'}
                    className={`unit-menu-item${amp.cabModel === 'custom' ? ' current' : ''}`}
                    data-testid="cab-custom"
                    onClick={() => {
                      onCabSelect('custom');
                      setAmpMenuOpen(false);
                    }}
                  >
                    {amp.customCabLabel} (custom)
                    {amp.cabModel === 'custom' ? ' ✓' : ''}
                  </button>
                )}
                {/* Was a <label> wrapping a `display: none` input — so it had no
                    role, no accessible name and no tab stop: mouse-only, and
                    absent from the accessibility tree entirely (2026-07-24 audit,
                    UI/UX). A real button forwards to the hoisted input above, so
                    the control is now reachable, named, and Enter/Space operable
                    like every other item in the menu. */}
                <button
                  type="button"
                  role="menuitem"
                  className="unit-menu-item"
                  data-testid="cab-upload"
                  onClick={() => irInputRef.current?.click()}
                >
                  Upload IR…
                </button>
              </>
            </Menu>
          </div>
          <Amp
            amp={amp}
            onParam={onAmpParam}
            onToggle={onAmpToggle}
            onTogglePower={onAmpPower}
            onChorusMode={onChorusMode}
          />
        </div>
      </div>
    </div>
  );
}
