// Conversation-history trimming (post-M6). Ported in spirit from Riff's
// `trimMessages` (app/api/chat/route.ts), adapted to Clipper's hand-rolled
// Anthropic Messages shape (role + content blocks) — NOT Vercel's UIMessage.
//
// Why: an unbounded history grows the request every turn (cost + latency) and
// re-ships stale tool_result payloads and old thinking blocks the model no
// longer needs. This caps the window and, for OLDER (completed) turns, replaces
// tool_result content with a tiny marker (the model still sees a tool ran, just
// not its bytes) and drops thinking blocks (only needed while a turn is live).
//
// Correctness invariants this function guarantees (see history.test.ts):
//   - Never exceeds MAX_MESSAGES.
//   - The window ALWAYS starts on a genuine user turn (Anthropic requires the
//     first message to be `user`), and never on an orphaned tool_result whose
//     matching tool_use was capped away.
//   - tool_use/tool_result pairing is preserved: a surviving assistant tool_use
//     keeps its tool_use block, and its matching user tool_result survives with
//     at least the marker.
//   - The CURRENT in-flight turn is never touched — thinking blocks (with their
//     signatures) and full tool_results in the live tool-use loop are echoed
//     back verbatim, as the Anthropic API requires. Only COMPLETED prior turns
//     are trimmed.
//
// Pure function: it never mutates its input. Untouched (recent / in-flight)
// messages are returned by reference; trimmed ones are fresh objects.

// Cap total messages in a request. Matches Riff's default.
export const MAX_MESSAGES = 20;
// Keep this many trailing messages fully intact (tool_results + thinking),
// regardless of turn boundaries. Matches Riff's `keepFull`.
export const KEEP_FULL = 5;
// The stand-in that replaces a trimmed tool_result's content.
export const TRIMMED_MARKER = '[trimmed]';

export interface HistoryMessage {
  role: 'user' | 'assistant';
  content: unknown[];
}

type Block = Record<string, unknown>;

function blocks(msg: HistoryMessage): Block[] | null {
  return Array.isArray(msg.content) ? (msg.content as Block[]) : null;
}

function hasToolResult(msg: HistoryMessage): boolean {
  const bs = blocks(msg);
  return !!bs && bs.some((b) => b && b.type === 'tool_result');
}

// A "clean" user turn is a genuine human message (text / context), NOT a
// tool_result carrier. These are the only valid conversation-start points and
// the only markers of a new exchange.
function isCleanUserTurn(msg: HistoryMessage): boolean {
  return msg.role === 'user' && !hasToolResult(msg);
}

// Strip a single COMPLETED message: replace tool_result payloads with the
// marker, and drop thinking / redacted_thinking from assistant turns. Returns a
// fresh message; the original is never mutated.
function trimMessage(msg: HistoryMessage): HistoryMessage {
  const bs = blocks(msg);
  if (!bs) return msg; // string content or malformed — leave alone

  if (msg.role === 'user') {
    const content = bs.map((b) =>
      b && b.type === 'tool_result'
        ? { ...b, content: TRIMMED_MARKER } // keep type + tool_use_id, shed bytes
        : b
    );
    return { role: 'user', content };
  }

  // assistant: drop thinking blocks (only needed during a live turn), keep text
  // + tool_use (tool_use must survive to stay paired with its tool_result).
  const content = bs.filter(
    (b) => !(b && (b.type === 'thinking' || b.type === 'redacted_thinking'))
  );
  // Guard: never empty an assistant turn (Anthropic rejects empty content). If
  // stripping would leave nothing, keep the original block set.
  if (content.length === 0) return msg;
  return { role: 'assistant', content };
}

// Cap + trim a history for one outgoing request. See module header for the
// invariants. `messages` is treated as read-only.
export function trimHistory(messages: HistoryMessage[]): HistoryMessage[] {
  if (!Array.isArray(messages) || messages.length === 0) return [];

  // 1) Hard cap to the most recent MAX_MESSAGES.
  const capped = messages.slice(-MAX_MESSAGES);

  // 2) Advance to the first genuine user turn. This drops any leading assistant
  //    turn (user-first requirement) AND any orphaned tool_result whose
  //    tool_use was capped away (which would 400).
  let start = capped.findIndex(isCleanUserTurn);
  if (start === -1) {
    // No clean user turn in the window — fall back to the first user turn of any
    // kind; if there is none at all, the history is unusable, return empty.
    start = capped.findIndex((m) => m.role === 'user');
    if (start === -1) return [];
  }
  const windowMsgs = capped.slice(start);

  // 3) The current in-flight exchange begins at the LAST clean user turn.
  //    Everything from there on is live and must not be touched.
  let currentTurnStart = -1;
  for (let i = windowMsgs.length - 1; i >= 0; i--) {
    if (isCleanUserTurn(windowMsgs[i])) {
      currentTurnStart = i;
      break;
    }
  }
  if (currentTurnStart === -1) currentTurnStart = 0;

  // 4) Protect BOTH the live turn and the trailing KEEP_FULL messages: trim only
  //    strictly before whichever boundary is earlier.
  const keepFromKeepFull = windowMsgs.length - KEEP_FULL;
  let protectedStart = Math.min(currentTurnStart, keepFromKeepFull);
  if (protectedStart < 0) protectedStart = 0;

  return windowMsgs.map((msg, i) => (i < protectedStart ? trimMessage(msg) : msg));
}
