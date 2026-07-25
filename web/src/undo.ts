// The rig undo ring (docs §39, ADR 011).
//
// The 2026-07-24 audit's UI/UX findings 2 and 3: Remove and Swap are instantly
// destructive (swap replaces a dialled-in pedal with fresh defaults and the
// autosave has already committed it), and an assistant turn rearranges the live
// rig with no cancel and no undo. CLAUDE.md's rule — "Destructive actions
// (remove pedal, swap pedal) need an undo path" — was unmet.
//
// WHAT THIS IS: a bounded ring of whole-rig SNAPSHOTS, stored as the strings
// `serializeRig()` already produces. Not a command/inverse log.
//
//   - Cheap: no per-action inverse to write and keep in sync with six pedal
//     types, four amp voices, the cab selector and the chain editor.
//   - Safe: a stored string is immutable, so a later mutation of live state can
//     never rewrite history, and a restore goes back through `deserializeRig()`
//     -> `normalizeRig()` — the SAME validation an untrusted persisted rig gets.
//     A snapshot is untrusted input; the undo path must not become a way to put
//     a non-finite or out-of-range value into the engine.
//   - Costly in exactly one way: a restore is a whole-rig push (one declick
//     fade), not a surgical parameter write. Measured entry size is in §39.
//
// THREE RULES THIS CLASS EXISTS TO ENFORCE:
//
//  1. A KNOB DRAG IS ONE ENTRY. `record()` takes a THUNK and a coalescing key.
//     If the key matches the last push and it arrived inside `coalesceMs`, the
//     push is dropped — the older entry is the true pre-drag state — and the
//     window extends. The thunk is only invoked when a push actually happens,
//     so a coalesced pointer-move costs zero `JSON.stringify`. Discrete actions
//     (stomp, toggle, chain edit, amp/cab swap) pass a null key and NEVER
//     coalesce: two consecutive toggles must be two undos, or the first undo
//     looks like it did nothing.
//
//  2. AN ASSISTANT TURN IS ONE ENTRY. `begin()`/`end()` bracket a transaction:
//     `begin` captures the pre-turn snapshot and suppresses every push inside,
//     `end` pushes that one snapshot only if the rig actually CHANGED. A
//     text-only reply and a cancelled turn therefore leave no entry at all, and
//     an undo can never land the rig in a half-applied state the player never
//     saw.
//
//  3. UNDO IS NOT ITSELF DESTRUCTIVE. Redo is part of the ring: `undo()` pushes
//     the current state onto a redo stack, and any new `record()` clears it.
//
// Framework-free and side-effect-free apart from the injected `snapshot`
// provider, so it is testable without React or a DOM.

export interface UndoEntry {
  // The serialized rig BEFORE the action this entry undoes.
  json: string;
  // Short human label for the action, e.g. "Swap RAT → SD-1". Shown on the
  // Undo control so the affordance says what it will do.
  label: string;
  // Coalescing key, or null for a discrete action.
  key: string | null;
  // Push time (ms, from the injected clock) — the coalescing window's anchor.
  at: number;
}

export interface UndoRingOptions {
  // How many past states to keep. 32 is the shipped value; see §39 for the
  // measured bytes-per-entry this is budgeted against.
  capacity?: number;
  // Coalescing window in ms. A pointer drag emits changes every ~8-16 ms, so
  // 600 ms means "you let go and came back" rather than "you are still
  // dragging" — generous enough to survive a stalled frame mid-drag.
  coalesceMs?: number;
  // Snapshot provider: serialize the CURRENT rig. Called only when the ring has
  // decided it needs a snapshot.
  snapshot: () => string;
  // Injectable clock (tests).
  now?: () => number;
  // Called after any change to depth / redoDepth / top label, so a host can
  // update its (non-pointer-rate) UI state. Never called from a coalesced push.
  onChange?: () => void;
}

export const DEFAULT_CAPACITY = 32;
export const DEFAULT_COALESCE_MS = 600;

export class UndoRing {
  private past: UndoEntry[] = [];
  private future: UndoEntry[] = [];
  private capacity: number;
  private coalesceMs: number;
  private snapshot: () => string;
  private now: () => number;
  private onChange?: () => void;

  // Open transaction (assistant turn). Depth counts nesting so a stray extra
  // end() cannot re-open pushes mid-turn.
  private txDepth = 0;
  private txJson: string | null = null;
  private txLabel = '';

  // Instrumentation for the §39 measurements. `attempts` is what a naive
  // one-entry-per-change ring would have pushed; `pushed` is what this one did;
  // `serializations` counts actual `JSON.stringify` work done on behalf of undo.
  attempts = 0;
  pushed = 0;
  serializations = 0;
  coalesced = 0;
  dropped = 0; // entries evicted by the capacity bound

  constructor(opts: UndoRingOptions) {
    this.capacity = Math.max(1, opts.capacity ?? DEFAULT_CAPACITY);
    this.coalesceMs = Math.max(0, opts.coalesceMs ?? DEFAULT_COALESCE_MS);
    this.snapshot = opts.snapshot;
    this.now = opts.now ?? (() => Date.now());
    this.onChange = opts.onChange;
  }

  private take(): string {
    this.serializations += 1;
    return this.snapshot();
  }

  get depth(): number {
    return this.past.length;
  }

  get redoDepth(): number {
    return this.future.length;
  }

  get inTransaction(): boolean {
    return this.txDepth > 0;
  }

  // The label of the action the next undo would revert (null when empty).
  get topLabel(): string | null {
    return this.past.length ? this.past[this.past.length - 1].label : null;
  }

  // The label of the action the next redo would re-apply (null when empty).
  get redoLabel(): string | null {
    return this.future.length ? this.future[this.future.length - 1].label : null;
  }

  // Record the state BEFORE a rig change. `key` non-null enables coalescing
  // (continuous knob moves); null means a discrete action that always gets its
  // own entry. Returns true if an entry was actually pushed.
  record(label: string, key: string | null = null): boolean {
    this.attempts += 1;

    // Inside a transaction every push is suppressed: `begin` already captured
    // the pre-turn state, and the whole turn must undo as one step.
    if (this.txDepth > 0) return false;

    const t = this.now();
    if (key !== null && this.past.length > 0) {
      const top = this.past[this.past.length - 1];
      if (top.key === key && t - top.at <= this.coalesceMs) {
        // Same continuous gesture: keep the OLDER snapshot (the true pre-drag
        // state) and extend the window. No snapshot is taken, so a dragged knob
        // does no serialization work at all.
        top.at = t;
        top.label = label;
        this.coalesced += 1;
        return false;
      }
    }

    this.pushEntry({ json: this.take(), label, key, at: t });
    this.future = []; // a new action invalidates the redo branch
    this.onChange?.();
    return true;
  }

  private pushEntry(e: UndoEntry): void {
    this.past.push(e);
    this.pushed += 1;
    while (this.past.length > this.capacity) {
      this.past.shift();
      this.dropped += 1;
    }
  }

  // Open a transaction: one undo entry for everything that happens until end().
  // Nested begins share the outermost snapshot and label.
  begin(label: string): void {
    if (this.txDepth === 0) {
      this.txJson = this.take();
      this.txLabel = label;
    }
    this.txDepth += 1;
  }

  // Close a transaction. Pushes the pre-transaction snapshot ONLY if the rig
  // actually changed — so a text-only assistant reply, an errored turn and a
  // cancelled turn all leave the history untouched. Returns true if an entry
  // was pushed.
  end(): boolean {
    if (this.txDepth === 0) return false;
    this.txDepth -= 1;
    if (this.txDepth > 0) return false;

    const before = this.txJson;
    const label = this.txLabel;
    this.txJson = null;
    this.txLabel = '';
    if (before == null) return false;

    if (this.take() === before) return false; // nothing changed — no entry
    this.pushEntry({ json: before, label, key: null, at: this.now() });
    this.future = [];
    this.onChange?.();
    return true;
  }

  // Abandon an open transaction without pushing anything (an aborted turn that
  // is known not to have touched the rig; end() is the normal path and is safe
  // either way because it compares).
  cancelTransaction(): void {
    if (this.txDepth === 0) return;
    this.txDepth = 0;
    this.txJson = null;
    this.txLabel = '';
  }

  // Pop the most recent past state. The CURRENT state moves to the redo stack.
  // Refused while a transaction is open (the assistant is mid-turn: Stop first).
  // Returns the snapshot to apply, or null.
  undo(): UndoEntry | null {
    if (this.txDepth > 0) return null;
    const e = this.past.pop();
    if (!e) return null;
    this.future.push({ json: this.take(), label: e.label, key: null, at: this.now() });
    this.onChange?.();
    return e;
  }

  // Re-apply the most recently undone state. The CURRENT state goes back onto
  // the past stack (bypassing coalescing, so a redo is always its own step).
  redo(): UndoEntry | null {
    if (this.txDepth > 0) return null;
    const e = this.future.pop();
    if (!e) return null;
    this.past.push({ json: this.take(), label: e.label, key: null, at: this.now() });
    this.pushed += 1;
    while (this.past.length > this.capacity) {
      this.past.shift();
      this.dropped += 1;
    }
    this.onChange?.();
    return e;
  }

  clear(): void {
    this.past = [];
    this.future = [];
    this.cancelTransaction();
    this.onChange?.();
  }

  // ---- measurement helpers (docs §39) ----

  // Exact stored payload size. `utf8` is what the same JSON costs on the wire /
  // in localStorage; `utf16` is the JS string's own footprint (2 bytes per code
  // unit — the honest in-memory number for a ring held in a ref). Neither is an
  // estimate: both are computed from the actual stored strings.
  bytes(): { entries: number; utf8: number; utf16: number; largest: number } {
    let utf8 = 0;
    let utf16 = 0;
    let largest = 0;
    const enc = typeof TextEncoder !== 'undefined' ? new TextEncoder() : null;
    for (const e of [...this.past, ...this.future]) {
      const u8 = enc ? enc.encode(e.json).length : e.json.length;
      utf8 += u8;
      utf16 += e.json.length * 2;
      if (u8 > largest) largest = u8;
    }
    return { entries: this.past.length + this.future.length, utf8, utf16, largest };
  }

  // A read-only view of the labels, oldest first (diagnostics / tests).
  labels(): string[] {
    return this.past.map((e) => e.label);
  }

  // Push a RAW snapshot string as the newest past entry. TEST SEAM ONLY: it is
  // how the suite proves that the restore path re-validates a hostile snapshot
  // rather than trusting it. The app itself never calls this.
  injectRaw(json: string, label = 'injected'): void {
    this.pushEntry({ json, label, key: null, at: this.now() });
    this.future = [];
    this.onChange?.();
  }
}
