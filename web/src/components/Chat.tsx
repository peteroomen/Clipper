// The liquid-glass tone-assistant chat (M6). Ports the approved glass design:
// glass panel with backdrop blur / specular edge / top sheen, assistant bubbles
// on glass, user bubbles as inset wells, applied tool calls as chips in the
// flow, a typing indicator, and a pill input. Conversation lives in memory.

import { useEffect, useRef, useState, type FormEvent } from 'react';
import {
  runAssistant,
  AssistantError,
  AssistantAborted,
  type ApiMessage,
} from '../assistant/client';
import { TOOLS, type RigController } from '../assistant/tools';
import { buildSystem, buildContextPreamble } from '../assistant/prompt';
import { PICKUP_TYPES, type GuitarProfile, type PickupType } from '../guitar';

type Item =
  | { id: number; kind: 'ai'; text: string }
  | { id: number; kind: 'user'; text: string }
  | { id: number; kind: 'chips'; chips: string[] }
  | { id: number; kind: 'notice'; variant: 'error' | 'refusal' | 'truncated' | 'nokey'; text: string };

export interface ChatProps {
  controller: RigController;
  guitar: GuitarProfile;
  onGuitarChange: (g: GuitarProfile) => void;
}

const GREETING =
  "I'm your tone coach. Tell me the sound you're chasing — a song, an artist, or just " +
  '"tighter, less fizzy" — and I\'ll dial it in and explain what I\'m doing. Set your guitar ' +
  'below so the advice fits your instrument.';

export function Chat({ controller, guitar, onGuitarChange }: ChatProps) {
  const [items, setItems] = useState<Item[]>([{ id: 0, kind: 'ai', text: GREETING }]);
  const [input, setInput] = useState('');
  const [busy, setBusy] = useState(false);
  const [settingsOpen, setSettingsOpen] = useState(false);

  // The API conversation history (content blocks preserved). Kept in a ref so
  // the async loop always sees the latest without re-render churn.
  const historyRef = useRef<ApiMessage[]>([]);
  const idRef = useRef(1);
  const scrollRef = useRef<HTMLDivElement>(null);
  // Index of the AI bubble currently being streamed into (or null).
  const streamIdRef = useRef<number | null>(null);

  const nextId = () => idRef.current++;
  const healthCheckedRef = useRef(false);
  // The live turn's abort controller (audit UI/UX finding 3: a turn reshaping the
  // rig used to be unstoppable). Null when no turn is in flight.
  const abortRef = useRef<AbortController | null>(null);
  // The chat input, so a cancel can put the caret back where the player left it
  // if focus has moved on since.
  const inputRef = useRef<HTMLInputElement>(null);

  function cancelTurn() {
    abortRef.current?.abort();
  }

  // Health check, fired LAZILY the first time the user engages the chat (focus /
  // send), not on mount — so pages that never touch the assistant make no /api
  // request. Surfaces a clear notice when the server has no key. All failures
  // are swallowed (the first real send surfaces a definitive error anyway).
  async function checkHealth() {
    if (healthCheckedRef.current) return;
    healthCheckedRef.current = true;
    try {
      const resp = await fetch('/api/health');
      if (!resp.ok) return;
      const j = await resp.json();
      if (j && j.hasKey === false) {
        // Mock mode (MOCK=1, no key) is a supported dev state — show a friendly
        // notice, not the "no key" error, since the coach still works (canned).
        const text = j.mock
          ? '[mock] Running in mock mode — no API key needed. Replies are canned for local dev.'
          : 'The assistant server is running but has no API key. Export ANTHROPIC_API_KEY ' +
            'and restart the server (npm run server) to enable the coach.';
        setItems((prev) => [
          { id: nextId(), kind: 'notice', variant: 'nokey', text },
          ...prev,
        ]);
      }
    } catch {
      /* server not reachable — the first send surfaces a clear error */
    }
  }

  // Auto-scroll to the newest message.
  useEffect(() => {
    const el = scrollRef.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [items, busy]);

  function appendTextDelta(delta: string) {
    setItems((prev) => {
      const streamId = streamIdRef.current;
      if (streamId !== null) {
        return prev.map((it) =>
          it.id === streamId && it.kind === 'ai' ? { ...it, text: it.text + delta } : it
        );
      }
      const id = nextId();
      streamIdRef.current = id;
      return [...prev, { id, kind: 'ai', text: delta }];
    });
  }

  async function handleSend(e?: FormEvent) {
    e?.preventDefault();
    const text = input.trim();
    if (!text || busy) return;
    void checkHealth();
    setInput('');
    setBusy(true);

    setItems((prev) => [...prev, { id: nextId(), kind: 'user', text }]);

    // Volatile context (rig + guitar + live input peak) goes in the USER turn so
    // the system prompt stays cacheable.
    const preamble = buildContextPreamble(
      controller.getRig(),
      guitar,
      controller.getPeakDbFs?.(),
      controller.getTunerReading?.()
    );
    // Where to rewind the API history to if the turn is cancelled: a half-turn
    // (a user message with no reply, or an assistant tool_use with no matching
    // tool_result) would make the NEXT request invalid.
    const historyMark = historyRef.current.length;
    historyRef.current.push({
      role: 'user',
      content: [
        { type: 'text', text: preamble },
        { type: 'text', text },
      ],
    });
    streamIdRef.current = null;

    const ac = new AbortController();
    abortRef.current = ac;
    // ONE undo entry for the whole turn (docs §39): the coach may fire several
    // tool calls, and undoing must return the rig to where it was BEFORE the
    // turn, not to some intermediate state the player never saw. Nothing is
    // pushed if the turn changes nothing (text-only reply, error, cancel).
    controller.beginUndo?.('Assistant turn');

    try {
      await runAssistant(
        controller,
        buildSystem(),
        TOOLS,
        historyRef.current,
        {
          onTextDelta: appendTextDelta,
          onToolChips: (chips) => {
            streamIdRef.current = null; // text after tools starts a fresh bubble
            setItems((prev) => [...prev, { id: nextId(), kind: 'chips', chips }]);
          },
          onNotice: (variant, noticeText) =>
            setItems((prev) => [
              ...prev,
              { id: nextId(), kind: 'notice', variant, text: noticeText },
            ]),
        },
        ac.signal
      );
    } catch (err) {
      if (err instanceof AssistantAborted) {
        // Drop the cancelled exchange from the API history and say so in the
        // flow. Any tool call that DID complete before the cancel stays applied
        // (it is real, and it is inside the turn's single undo entry).
        historyRef.current.length = historyMark;
        setItems((prev) => [
          ...prev,
          {
            id: nextId(),
            kind: 'notice',
            variant: 'truncated',
            text: '(stopped — nothing further was changed.)',
          },
        ]);
      } else {
        const message =
          err instanceof AssistantError
            ? err.message
            : 'Something went wrong talking to the assistant.';
        setItems((prev) => [
          ...prev,
          { id: nextId(), kind: 'notice', variant: 'error', text: message },
        ]);
      }
    } finally {
      controller.endUndo?.();
      abortRef.current = null;
      streamIdRef.current = null;
      setBusy(false);
      // The input is never `disabled` mid-turn (that is what dropped focus to
      // <body>), but if focus DID move elsewhere while the turn ran, put it back
      // where the player was typing.
      const el = inputRef.current;
      if (el && document.activeElement === document.body) el.focus();
    }
  }

  // Typing indicator while we await the model (last visible item is a prompt or
  // a batch of tool chips, i.e. no assistant text yet for this segment).
  const last = items[items.length - 1];
  const showTyping = busy && (!last || last.kind === 'user' || last.kind === 'chips');

  return (
    <aside className="chat" data-testid="chat" aria-label="Tone assistant">
      <div className="chat-head">
        <span className="avatar" aria-hidden="true" />
        <span className="who">Tone assistant</span>
        <button
          type="button"
          className="chat-gear"
          aria-label="Guitar profile"
          aria-expanded={settingsOpen}
          data-testid="chat-settings-toggle"
          onClick={() => setSettingsOpen((v) => !v)}
        >
          Guitar
        </button>
      </div>

      {settingsOpen && (
        <div className="chat-settings well" data-testid="guitar-form">
          <label className="gf-field">
            <span>Guitar</span>
            <input
              type="text"
              value={guitar.model}
              placeholder="Fender Stratocaster"
              aria-label="Guitar model"
              onChange={(ev) => onGuitarChange({ ...guitar, model: ev.target.value })}
            />
          </label>
          <label className="gf-field">
            <span>Pickups</span>
            <select
              value={guitar.pickupType}
              aria-label="Pickup type"
              onChange={(ev) =>
                onGuitarChange({ ...guitar, pickupType: ev.target.value as PickupType })
              }
            >
              {PICKUP_TYPES.map((p) => (
                <option key={p} value={p}>
                  {p}
                </option>
              ))}
            </select>
          </label>
          <label className="gf-field">
            <span>Position</span>
            <input
              type="text"
              value={guitar.pickupPosition}
              placeholder="bridge / 4"
              aria-label="Pickup position"
              onChange={(ev) => onGuitarChange({ ...guitar, pickupPosition: ev.target.value })}
            />
          </label>
        </div>
      )}

      <div className="chat-scroll" ref={scrollRef} data-testid="chat-scroll">
        {items.map((it) => {
          if (it.kind === 'user')
            return (
              <div key={it.id} className="bubble user">
                {it.text}
              </div>
            );
          if (it.kind === 'ai')
            return (
              <div key={it.id} className="bubble ai" data-testid="ai-bubble">
                {it.text}
              </div>
            );
          if (it.kind === 'chips')
            return (
              <div key={it.id} className="chips" data-testid="tool-chips">
                {it.chips.map((c, i) => (
                  <span key={i} className="chip">
                    {c}
                  </span>
                ))}
              </div>
            );
          return (
            <div key={it.id} className={`chat-notice ${it.variant}`} data-testid="chat-notice">
              {it.text}
            </div>
          );
        })}
        {showTyping && (
          <div className="bubble ai typing" data-testid="typing" aria-label="Assistant is typing">
            <i />
            <i />
            <i />
          </div>
        )}
      </div>

      <form className="chat-input" onSubmit={handleSend}>
        {/* READ-ONLY, never `disabled`, while a turn runs. `disabled` removes the
            element from the focus order, which dropped focus to <body> mid-turn
            and lost the player's place (audit UI/UX finding 3). readOnly keeps
            the caret, the selection and the tab stop. Escape cancels the turn. */}
        <input
          ref={inputRef}
          type="text"
          value={input}
          placeholder="Give me the rhythm tone from The Bends…"
          aria-label="Message the tone assistant"
          data-testid="chat-input"
          readOnly={busy}
          aria-busy={busy}
          onFocus={() => void checkHealth()}
          onChange={(ev) => setInput(ev.target.value)}
          onKeyDown={(ev) => {
            if (ev.key === 'Escape' && busy) {
              ev.preventDefault();
              cancelTurn();
            }
          }}
        />
        {/* While busy this is a STOP button rather than a disabled Send: a turn
            that is rearranging the rig must be interruptible, and swapping the
            handler in place (instead of disabling) also means focus never leaves
            it if the player clicked to send. */}
        {busy ? (
          <button
            type="button"
            className="send stop"
            aria-label="Stop the assistant"
            data-testid="chat-stop"
            onClick={cancelTurn}
          >
            ■
          </button>
        ) : (
          <button
            type="submit"
            className="send"
            aria-label="Send"
            data-testid="chat-send"
            disabled={!input.trim()}
          >
            ↑
          </button>
        )}
      </form>
    </aside>
  );
}
