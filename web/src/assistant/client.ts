// Client-side tool-use loop (M6). Talks to the same-origin proxy (/api/chat),
// streams the SSE response, renders text as it arrives, and — when the model
// asks for tools — executes them locally against the live rig and continues the
// loop until end_turn (capped). The proxy injects model / max_tokens / thinking;
// this file owns the conversation.

import { executeTool, type RigController } from './tools';

const MAX_ITERATIONS = 6;

// Anthropic message shape (role + content blocks). We keep content blocks
// verbatim so thinking blocks (with signatures) are preserved when echoed back.
export interface ApiMessage {
  role: 'user' | 'assistant';
  content: unknown[];
}

export interface RunCallbacks {
  onTextDelta: (delta: string) => void; // streamed assistant text
  onToolChips: (chips: string[]) => void; // applied tool calls for one turn
  onNotice: (kind: 'refusal' | 'truncated', text: string) => void;
}

// A user-facing error (proxy down, no key, upstream error). The chat surfaces
// `.message` with fix instructions.
export class AssistantError extends Error {}

interface StreamedTurn {
  content: unknown[];
  stopReason: string | null;
}

// Parse the SSE stream for one model turn, accumulating content blocks and
// streaming text deltas out. Returns the full content array + stop_reason.
async function streamTurn(
  body: ReadableStream<Uint8Array>,
  onTextDelta: (delta: string) => void
): Promise<StreamedTurn> {
  const reader = body.getReader();
  const decoder = new TextDecoder();
  let buffer = '';

  // Working blocks keyed by index. tool_use blocks accumulate a JSON string in
  // `_json` that we parse at content_block_stop.
  interface WorkBlock {
    type: string;
    text?: string;
    thinking?: string;
    signature?: string;
    data?: string;
    id?: string;
    name?: string;
    input?: unknown;
    _json?: string;
  }
  const blocks: Record<number, WorkBlock> = {};
  let stopReason: string | null = null;

  function handleEvent(json: Record<string, unknown>) {
    const type = json.type as string;
    if (type === 'content_block_start') {
      const index = json.index as number;
      const cb = (json.content_block ?? {}) as Record<string, unknown>;
      const block: WorkBlock = { type: cb.type as string };
      if (block.type === 'text') block.text = (cb.text as string) ?? '';
      else if (block.type === 'thinking') {
        block.thinking = (cb.thinking as string) ?? '';
        block.signature = (cb.signature as string) ?? '';
      } else if (block.type === 'redacted_thinking') block.data = cb.data as string;
      else if (block.type === 'tool_use') {
        block.id = cb.id as string;
        block.name = cb.name as string;
        block._json = '';
      }
      blocks[index] = block;
    } else if (type === 'content_block_delta') {
      const index = json.index as number;
      const delta = (json.delta ?? {}) as Record<string, unknown>;
      const block = blocks[index];
      if (!block) return;
      if (delta.type === 'text_delta') {
        const t = (delta.text as string) ?? '';
        block.text = (block.text ?? '') + t;
        if (t) onTextDelta(t);
      } else if (delta.type === 'thinking_delta') {
        block.thinking = (block.thinking ?? '') + ((delta.thinking as string) ?? '');
      } else if (delta.type === 'signature_delta') {
        block.signature = (block.signature ?? '') + ((delta.signature as string) ?? '');
      } else if (delta.type === 'input_json_delta') {
        block._json = (block._json ?? '') + ((delta.partial_json as string) ?? '');
      }
    } else if (type === 'content_block_stop') {
      const index = json.index as number;
      const block = blocks[index];
      if (block && block.type === 'tool_use') {
        try {
          block.input = block._json ? JSON.parse(block._json) : {};
        } catch {
          block.input = {};
        }
        delete block._json;
      }
    } else if (type === 'message_delta') {
      const delta = (json.delta ?? {}) as Record<string, unknown>;
      if (delta.stop_reason) stopReason = delta.stop_reason as string;
    }
    // message_start / message_stop / ping: nothing to accumulate.
  }

  // Process a raw SSE `data:` payload (one JSON object per event).
  function processData(payload: string) {
    if (payload === '[DONE]') return;
    let json: Record<string, unknown>;
    try {
      json = JSON.parse(payload);
    } catch {
      return;
    }
    handleEvent(json);
  }

  // Read + split on blank-line-delimited SSE events; within each, use `data:`.
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    buffer += decoder.decode(value, { stream: true });
    let sep;
    while ((sep = buffer.indexOf('\n\n')) !== -1) {
      const rawEvent = buffer.slice(0, sep);
      buffer = buffer.slice(sep + 2);
      for (const line of rawEvent.split('\n')) {
        if (line.startsWith('data:')) processData(line.slice(5).trim());
      }
    }
  }
  // Flush any trailing event without a final blank line.
  if (buffer.trim()) {
    for (const line of buffer.split('\n')) {
      if (line.startsWith('data:')) processData(line.slice(5).trim());
    }
  }

  // Materialize the ordered, clean content array (drop internal fields).
  const content: unknown[] = Object.keys(blocks)
    .map(Number)
    .sort((a, b) => a - b)
    .map((i) => {
      const b = blocks[i];
      if (b.type === 'text') return { type: 'text', text: b.text ?? '' };
      if (b.type === 'thinking')
        return { type: 'thinking', thinking: b.thinking ?? '', signature: b.signature ?? '' };
      if (b.type === 'redacted_thinking') return { type: 'redacted_thinking', data: b.data };
      if (b.type === 'tool_use')
        return { type: 'tool_use', id: b.id, name: b.name, input: b.input ?? {} };
      return b;
    });

  return { content, stopReason };
}

// POST the conversation to the proxy and return the streamed turn.
async function requestTurn(
  system: unknown,
  tools: unknown,
  messages: ApiMessage[],
  onTextDelta: (delta: string) => void
): Promise<StreamedTurn> {
  let resp: Response;
  try {
    resp = await fetch('/api/chat', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ system, tools, messages }),
    });
  } catch {
    throw new AssistantError(
      "Couldn't reach the assistant server. Start it with `npm run server` " +
        '(and make sure ANTHROPIC_API_KEY is exported).'
    );
  }

  if (!resp.ok) {
    let detail = `The assistant server returned ${resp.status}.`;
    try {
      const j = await resp.json();
      if (j?.error) detail = j.error;
    } catch {
      /* non-JSON error body */
    }
    throw new AssistantError(detail);
  }
  if (!resp.body) throw new AssistantError('The assistant response had no body.');

  return streamTurn(resp.body, onTextDelta);
}

// Drive the full tool-use loop over an existing message history (mutated in
// place and returned). Executes ALL tool_use blocks in a turn and returns their
// results in ONE user message before continuing.
export async function runAssistant(
  controller: RigController,
  system: unknown,
  tools: unknown,
  messages: ApiMessage[],
  cb: RunCallbacks
): Promise<ApiMessage[]> {
  for (let iter = 0; iter < MAX_ITERATIONS; iter++) {
    const turn = await requestTurn(system, tools, messages, cb.onTextDelta);
    messages.push({ role: 'assistant', content: turn.content });

    if (turn.stopReason === 'tool_use') {
      const toolUses = turn.content.filter(
        (b): b is { type: 'tool_use'; id: string; name: string; input: Record<string, unknown> } =>
          (b as { type?: string }).type === 'tool_use'
      );
      const results: unknown[] = [];
      const chips: string[] = [];
      for (const tu of toolUses) {
        const exec = executeTool(controller, tu.name, tu.input ?? {});
        chips.push(exec.chip);
        results.push({ type: 'tool_result', tool_use_id: tu.id, content: exec.content });
      }
      if (chips.length) cb.onToolChips(chips);
      messages.push({ role: 'user', content: results });
      continue; // loop for the model's follow-up
    }

    if (turn.stopReason === 'refusal') {
      cb.onNotice('refusal', "I can't help with that one.");
    } else if (turn.stopReason === 'max_tokens') {
      cb.onNotice('truncated', '(response was cut off — ask me to continue.)');
    }
    break; // end_turn (or a handled terminal reason)
  }
  return messages;
}
