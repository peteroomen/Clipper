// Menu — the shared popover: a trigger button plus a dismissible, focus-trapped
// panel.
//
// WHY THIS EXISTS (2026-07-24 audit, UI/UX · docs §38). The app had three
// popovers — the pedal swap menu, the gear tray, and the amp/cab menu — all
// hand-rolled in Board.tsx as `{open && <div className="unit-menu" role="menu">}`.
// None of them had **Escape**, **click-outside dismissal**, or a **focus trap**,
// which is a keyboard dead end: `role="menu"` announces a menu, Tab then walks the
// focus straight out of it and behind it, and the only way back to the rest of the
// page is to find and re-activate the trigger. There was also no way to close one
// at all without picking an item.
//
// This component owns all four behaviours in one place so a fourth popover cannot
// ship without them:
//
//   1. **Escape** closes and returns focus to the trigger (the listener is on the
//      document, because focus may legitimately be on the trigger rather than
//      inside the panel).
//   2. **Pointer-down outside** closes. The trigger is excluded from "outside", or
//      the sequence pointerdown(close) → click(toggle) would reopen what the
//      player just dismissed.
//   3. **Focus moves into the panel** on open, and **Tab/Shift+Tab cycle within
//      it** — a real trap, computed live from the panel's focusable children so it
//      cannot drift as items are added.
//   4. `aria-haspopup` / `aria-expanded` on the trigger, always in step with the
//      `open` prop, because it is the same prop that renders the panel.
//
// It deliberately renders the trigger itself rather than taking a render prop: all
// three call sites differ only in classes, label and inner content, and owning the
// button is what lets the ref plumbing stay inside this file.
//
// Not done here (deferred, docs §38): arrow-key roving focus with a single tab
// stop, which is what `role="menu"` canonically wants. Tab-cycling is correct and
// escapable, which is what the audit asked for.

import { useEffect, useRef, type ReactNode } from 'react';

const FOCUSABLE =
  'button:not([disabled]), [href], input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])';

export interface MenuProps {
  open: boolean;
  /** Toggle from the trigger (open ⇄ closed). */
  onToggle: () => void;
  /** Dismiss without choosing (Escape, pointer outside). */
  onClose: () => void;

  triggerClassName?: string;
  triggerAriaLabel: string;
  triggerTestId?: string;
  triggerContent: ReactNode;

  menuClassName?: string;
  menuTestId?: string;
  /** Accessible name for the panel itself; defaults to the trigger's name. */
  menuAriaLabel?: string;
  children: ReactNode;
}

export function Menu({
  open,
  onToggle,
  onClose,
  triggerClassName,
  triggerAriaLabel,
  triggerTestId,
  triggerContent,
  menuClassName,
  menuTestId,
  menuAriaLabel,
  children,
}: MenuProps) {
  const triggerRef = useRef<HTMLButtonElement>(null);
  const panelRef = useRef<HTMLDivElement>(null);

  // Keep the latest onClose without re-subscribing the document listeners on
  // every parent render (they are attached for as long as the panel is open).
  const onCloseRef = useRef(onClose);
  onCloseRef.current = onClose;

  const focusables = () =>
    panelRef.current
      ? Array.from(panelRef.current.querySelectorAll<HTMLElement>(FOCUSABLE)).filter(
          (el) => el.offsetParent !== null || el === document.activeElement
        )
      : [];

  // Move focus into the panel on open. Without this, Escape-to-close and the trap
  // are unreachable for a keyboard player: the trigger keeps focus, Tab leaves for
  // whatever follows the trigger in the DOM, and the panel is never entered.
  useEffect(() => {
    if (!open) return;
    const first = focusables()[0];
    if (first) first.focus();
  }, [open]);

  // Escape (document-level: focus may be on the trigger or inside the panel) and
  // pointer-down outside. Both are subscribed only while the panel is open.
  useEffect(() => {
    if (!open) return;

    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key !== 'Escape') return;
      e.stopPropagation();
      onCloseRef.current();
      // Escape is a deliberate dismissal, so focus goes back where the player was
      // before they opened the panel — anything else drops them to <body>.
      triggerRef.current?.focus();
    };

    const onPointerDown = (e: PointerEvent) => {
      const t = e.target as Node | null;
      if (!t) return;
      if (panelRef.current?.contains(t)) return;
      // The trigger is NOT "outside": closing here would be undone by its own
      // click handler a moment later, which reads as the menu refusing to close.
      if (triggerRef.current?.contains(t)) return;
      onCloseRef.current();
    };

    document.addEventListener('keydown', onKeyDown);
    document.addEventListener('pointerdown', onPointerDown, true);
    return () => {
      document.removeEventListener('keydown', onKeyDown);
      document.removeEventListener('pointerdown', onPointerDown, true);
    };
  }, [open]);

  // The trap itself. Computed from the live DOM on every Tab so it cannot go stale
  // as menu items appear (a custom cab entry, a new pedal type).
  const onPanelKeyDown = (e: React.KeyboardEvent<HTMLDivElement>) => {
    if (e.key !== 'Tab') return;
    const items = focusables();
    if (items.length === 0) return;
    const first = items[0];
    const last = items[items.length - 1];
    const active = document.activeElement;
    if (e.shiftKey && active === first) {
      last.focus();
      e.preventDefault();
    } else if (!e.shiftKey && active === last) {
      first.focus();
      e.preventDefault();
    }
  };

  return (
    <>
      <button
        type="button"
        ref={triggerRef}
        className={triggerClassName}
        aria-label={triggerAriaLabel}
        aria-haspopup="menu"
        aria-expanded={open}
        data-testid={triggerTestId}
        onClick={onToggle}
      >
        {triggerContent}
      </button>
      {open && (
        <div
          ref={panelRef}
          className={menuClassName}
          role="menu"
          aria-label={menuAriaLabel ?? triggerAriaLabel}
          data-testid={menuTestId}
          onKeyDown={onPanelKeyDown}
        >
          {children}
        </div>
      )}
    </>
  );
}
