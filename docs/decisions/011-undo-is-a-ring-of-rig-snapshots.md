# ADR 011: Undo is a bounded ring of rig-state snapshots, and one assistant turn is one entry

Date: 2026-07-25
Status: Accepted

## Context

The 2026-07-24 audit's UI/UX section raised two findings that are really one:

- **Remove and Swap are instantly destructive with no undo anywhere in the project.** Swap
  replaces a dialled-in pedal with fresh defaults (`App.tsx:242`) and the autosave has already
  committed it, so the settings existed nowhere.
- **Assistant tool calls rearrange the live rig with no cancel and no undo.** `runAssistant`
  took no `AbortSignal`, and the chat input was merely `disabled` while a turn ran — which also
  drops focus to `<body>`, losing the player's place mid-turn.

The app had exactly one implicit save slot, mutated in place by both the player and the model.
CLAUDE.md already stated the rule ("Destructive actions … need an undo path. There is none
today — don't add more destructive surface without one"), so this was blocking further UI work
as well as being a bug in its own right.

Constraints that shaped the decision:

- The rig-state JSON (`web/src/rig.ts`) is already the canonical serialization and the intended
  preset format.
- Knob moves arrive at pointer rate, and the project forbids React state updates at that rate.
- A restored snapshot is **untrusted input**; audit finding 1 (ADR 002) established that a
  non-finite parameter latches permanently in recursive DSP state.
- An assistant turn may fire several tool calls; the rig passes through intermediate states the
  player never sees.

## Decision

**1. Snapshots, not a command log.** `web/src/undo.ts` keeps a bounded ring (capacity **32**) of
`serializeRig()` **strings** — the state *before* each action. Restore is
`deserializeRig()` → whole-rig push to the engine.

**2. The restore path re-validates.** Every restore goes through `normalizeRig()`, the same
clamp / coerce / migrate path a rig loaded from localStorage gets, before anything reaches the
engine. Undo is not a hole in the "allowlisted and clamped" boundary.

**3. Coalescing by key, with a thunk.** `record(label, key)` drops a push whose key matches the
last one inside 600 ms (the older entry is the true pre-gesture state) and only invokes the
snapshot thunk when it is actually going to push. Continuous knobs pass a key; discrete actions
(stomp, toggle, chain edit, amp/cab swap) pass `null` and always get their own entry.

**4. One assistant turn is one entry.** `begin(label)` / `end()` bracket a transaction that
suppresses inner pushes and pushes a single pre-turn snapshot **only if the rig changed**.
`undo()` is refused while a transaction is open.

**5. Redo is in scope.** An undo with no redo is itself a destructive action. Any new record
clears the redo branch.

**6. A real `AbortSignal` through `runAssistant`, and `readOnly` instead of `disabled`.** The
signal is checked before each iteration, after the stream, and before each tool call in a
batch; on abort the partial turn is dropped from the API history (an assistant `tool_use` with
no `tool_result` would invalidate the next request). The input stays focusable and merely
read-only mid-turn, and the send button *becomes* a Stop button rather than being disabled.

## Alternatives rejected

- **A command/inverse log** (record `swapPedal(id, from→to)` and invert it). Smaller entries and
  a natural place to hang labels, but it needs an inverse per action across six pedal types,
  four amp voices, the cab selector and the chain editor, and every new pedal is a chance to
  forget one — a silently wrong inverse is worse than no undo. Reconsider only if entry size
  ever becomes a real constraint; it is 415 B–1.1 KB today.
- **Persisting the ring** across reloads. Needs a second storage key, a size budget, and makes
  the "snapshot is untrusted" property load-bearing rather than defensive. Deferred to the
  preset/save-slot slice; a reload is a history boundary for now.
- **Undo state in React state** instead of a ref. Would put a state update on every knob move.
- **Time-based coalescing only** (no key). A drag on Dist followed within the window by a drag
  on Level would collapse into one entry, which is not what the player did.
- **Per-tool-call undo entries for an assistant turn.** Simpler, but an undo would land the rig
  in a state that never existed as far as the player is concerned.
- **Leaving the input `disabled` and adding a separate Cancel button.** Keeps the focus bug: a
  disabled element leaves the focus order. `readOnly` + a Stop button in the send button's own
  DOM node fixes the cause rather than working around it.
- **Cancelling by ignoring the response** (no `AbortSignal`). The request keeps running, the
  tool calls still arrive, and the rig still moves. Not a cancel.

## Consequences

**Easier.** Every rig-changing action in the web app is reversible, including the two the audit
named. The affordance names what it will undo, works from the keyboard, and is reachable with
`Cmd/Ctrl-Z`. An assistant turn is stoppable, and a stopped turn provably leaves the rig alone.
The snapshot format being the rig JSON means the preset/save-slot work inherits the same
serialization and the same validation.

**Costs, measured.** ~830 B (UTF-16, in-memory) per default-rig entry, ~2.3 KB for an 8-pedal
rig; a full 32-entry ring of worst-case rigs is **73 344 B**. A restore is a whole-rig push:
one declick fade plus 13 amp params, with the cab IR and amp voice sent only when they differ.
A 3-second knob drag pushes **1** entry and does **1** `JSON.stringify` (a naive ring would do
180 of each).

**Harder / watch out.**

- Anything outside the rig JSON is not undoable: the custom-IR samples, the guitar profile, the
  theme, engine start/stop, the chat transcript. Undoing an IR upload restores the cab
  *selection* only. Say so in the UI if that ever surprises someone.
- `rigRef` is now the synchronous source of truth (`commitRig()` writes it eagerly). A new
  mutator that calls `setRig` directly will both break undo snapshots and reintroduce the
  batching bug this slice found (two chain edits in one tick clobbering each other).
- A new rig field must be handled by `normalizeRig` — an unnormalized field silently resets on
  every undo.
- Native (JUCE) has no undo; parity is a separate slice against its own state tree.
