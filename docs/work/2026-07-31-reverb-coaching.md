# Reverb coaching — the authentic blackface law, explained instead of re-tuned

**Date:** 2026-07-31
**Branch:** claude/reverb-coaching-6f557i
**Roadmap item:** round-3 feedback doc decision — "the top half of the reverb knob is too strong to ever use" (amends docs §48)

## Goal

The assistant tells players where the spring reverb is actually usable (15-35 classic,
~40+ surf), so the authentic blackface law reads as the amp being honest rather than as a
bug — with **no DSP change**.

## Approach

Docs, prose, one prompt block. **Not a DSP slice:** the §48 wet trim already answered the
level report with a measured −6 dB, and the research the owner commissioned says the
remaining "top half is unusable" is the real instrument: a blackface Twin's sweet spot is
1-2.5 of 10, past 3-4 is surf, and there is a whole dwell-mod industry for players who
want otherwise. Our measured parity (wet = dry at knob ~0.60) is if anything POLITE next
to a real Twin, which is drenched by ~0.35. So the fix is coaching, not `kWetGain`.

The prompt text goes in the **stable** `SYSTEM_PROMPT` block (extending the Twin's existing
reverb bullet, where the amp knob advice already lives) so the `cache_control` prefix is
unchanged in shape — no volatile rig-state path touched, no new tool, no block reordered.

## Steps

- [x] Write this plan file
- [x] Extend the Twin's `reverb` bullet in `web/src/assistant/prompt.ts` with the range
      coaching (15-35 / ~40+), the "this is real Twin behavior" framing, and a note that
      the same spring model backs every amp's reverb knob
- [x] Append a §48 amendment to `docs/DEVELOPMENT.md` recording the 2026-07-31 owner
      decision + the research summary
- [x] Add a CLAUDE.md Current State entry
- [x] Check whether `web/tests/assistant.spec.ts` or the node server suites assert prompt
      text; update if so
- [x] `cd web && npm run build` (tsc gate) + `npx playwright test tests/assistant.spec.ts`,
      and `npm run test:server` at the root

## How this will be measured

There is no audio measurement — nothing in the signal path changes, and that is the point.
The gates are: `git status` shows **only** `web/src/assistant/`, `docs/`, `CLAUDE.md`
(no `core/`, no `web/worklet/`, no WASM artifact); `tsc --noEmit` passes via
`npm run build`; the assistant Playwright spec and the node server suites stay green.

## Manual test steps

- [ ] Open the chat with the Twin selected and ask "how much reverb should I use?" — the
      coach should land on 15-35 for classic spring and call 40+ surf, and should say the
      real amp behaves this way.
- [ ] Edge case: a player insists "give me full-on surf" — the coach should NOT refuse or
      warn it off; 40+ is a legitimate deliberate setting, and the prompt says so.
- [ ] Edge case: the coach must not start claiming the reverb was rebalanced or that a
      future DSP change is coming — the law is deliberate and stays.

## Out of scope for this session

- Any change to `kWetGain`, the knob taper, or the spring model (§48 stands).
- A UI affordance (knob detent, printed range on the face) — prompt-only this time.
- Native/JUCE parity: the assistant is web-only, so there is nothing to mirror.

---

## What actually happened

Straight run of the plan, with two findings worth recording:

1. **The prompt lives client-side only.** `SYSTEM_PROMPT` is referenced from exactly one
   place outside its own file (`web/src/components/Chat.tsx` → `buildSystem()`), and the
   proxy is a pure forwarder: `server/handler.test.mjs` builds its own dummy
   `[{ type: 'text', text: 'coach', … }]` block and only asserts pass-through. So no
   server change was needed and the node suites were unaffected by the wording.
2. **Nothing snapshot-tests the prompt.** `web/tests/assistant.spec.ts` drives canned SSE
   tool-use responses and asserts rig effects (e.g. `set_param reverb` → 0.25); it never
   inspects prompt text. No test update was required — which also means the wording is
   only protected by review, worth knowing next time someone edits this block.

The coaching was placed on the Twin's reverb bullet (the amp whose springs are the
signature and the amp the report was about) with an explicit closing note that the same
spring model and the same numbers apply to the reverb knob on the other three heads —
one edit instead of four near-duplicates, and the cached stable block grows by one
sentence-group rather than being restructured.

## Measured results

Not a DSP slice — no audio numbers. Process gates, all green:

| Gate | Result |
| --- | --- |
| `git status` scope | only `web/src/assistant/prompt.ts`, `docs/`, `CLAUDE.md` — no `core/`, no `web/worklet/`, no `web/public/generated/` |
| `cd web && npm run build` (tsc --noEmit + vite) | pass |
| `npx playwright test tests/assistant.spec.ts` | pass |
| root `npm run test:server` | pass |

The figures the coaching is derived from (from §48 and the commissioned research, not
re-measured here): our wet reaches parity with dry at knob **~0.60**; a real blackface
Twin is drenched by **~0.35**; the real amp's owner-reported sweet spot is **1-2.5 of 10**,
surf past **3-4**.

## Files created / modified

- `web/src/assistant/prompt.ts` — reverb range coaching appended to the Twin's `reverb`
  bullet inside the stable `SYSTEM_PROMPT` block
- `docs/DEVELOPMENT.md` — §48 amendment (the 2026-07-31 decision + research summary)
- `CLAUDE.md` — Current State entry
- `docs/work/2026-07-31-reverb-coaching.md` — this file

## Deferred to next session

- No test protects the prompt's content. If the reverb coaching matters, a cheap unit
  assert (`SYSTEM_PROMPT` contains the range) would pin it — deliberately not added here
  to keep this slice prose-only.
- A UI hint on the reverb knob face (printed usable range / detent at ~35) is the obvious
  non-assistant version of the same fix, unclaimed.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
