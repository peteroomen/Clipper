# Proxy loopback binding + Electron shell hardening

**Date:** 2026-07-25
**Branch:** fix/proxy-loopback
**Roadmap item:** `docs/audits/2026-07-24-project-audit.md` — finding 17 plus the five
items in the **Security & app layer** section (Electron navigation hardening, CSP,
`config.mjs` mode, `serve.mjs` containment check, body-limit socket leak).

## Goal

The assistant proxy is reachable only from the machine it runs on, the Electron shell
cannot be navigated or window-opened away from its own origin, and the stored API key
is `0600` even when the file already existed.

## Approach

Six independent fixes, all outside `core/` and `web/worklet/` — so **no WASM rebuild
is required** and there is no DSP/fidelity dimension to this slice at all.

1. **`server/index.mjs` — bind loopback.** `server.listen(PORT, cb)` binds `0.0.0.0`/`::`.
   Add `HOST` (default `127.0.0.1`) and pass it to `listen`. Print the address actually
   bound, from `server.address()`, not a hardcoded `localhost`. Print a one-line warning
   when `HOST` is a non-loopback address so deliberate LAN exposure is visible.

   To make this testable without the module self-starting on import, `index.mjs` grows a
   named export `createProxyServer({env})` returning an un-listened `http.Server`, plus
   `resolveHost(env)` and `startProxy({env})`. The `listen` call becomes conditional on
   the module being the process entry point (`process.argv[1]`), so `npm run server` is
   unchanged but `node --test` can import it. The request handler body is moved verbatim
   — no behaviour change to routing, CORS, or the upstream call.

2. **`electron/serve.mjs` — CSP header on HTML.** Add `Content-Security-Policy` to the
   `serveStatic` response when the resolved file is `.html`. Starting policy from the
   audit: `default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline';
   connect-src 'self'; img-src 'self' data:`.

   Two adjustments are expected to be necessary against the real Vite output and will be
   justified rather than waved through:
   - `'wasm-unsafe-eval'` in `script-src` — the engine is `WebAssembly.instantiate()` on
     an embedded binary (`web/public/generated/clipper.js`, Emscripten SINGLE_FILE).
     Chromium refuses Wasm compilation whenever a `script-src` directive is present and
     neither `'wasm-unsafe-eval'` nor `'unsafe-eval'` is listed. Without it the CSP
     silently kills all audio.
   - `font-src 'self' data:` — `web/src/styles/tokens.css` has one `@font-face` whose
     `src` is a base64 `data:font/woff2` URI, and `default-src 'self'` does not admit
     `data:`.

   Also `base-uri 'none'` (no fallback from `default-src`, and the window renders
   model-influenced text). Deliberately **not** adding a `<meta>` CSP to
   `web/index.html`: that would also apply in `vite dev`, where it fights HMR, and the
   deployed browser build's CSP belongs to whatever host serves it.

3. **`electron/main.mjs` — navigation hardening.** On the main window only:
   `will-navigate` denies anything whose origin is not the served origin;
   `setWindowOpenHandler` returns `{action:'deny'}` and hands `https:`/`http:` URLs to
   `shell.openExternal`; `session.setPermissionRequestHandler` grants only `media` and
   only to the served origin, denying everything else. `contextIsolation`, `nodeIntegration:
   false`, `sandbox` and the narrow preload stay exactly as they are.

4. **`electron/config.mjs` — explicit chmod.** `writeFileSync(..., {mode})` only applies
   the mode on creation. Add `chmodSync(configPath, 0o600)` after the write. Guarded so a
   platform without POSIX modes cannot turn a successful save into a thrown error.

5. **`electron/serve.mjs` — containment check.** `filePath.startsWith(distDir)` accepts a
   sibling directory whose name merely starts with `distDir`. Compare against
   `distDir + path.sep`, with the bare `distDir` itself still allowed.

6. **`server/index.mjs` — destroy the request on body overflow.** `readJsonBody` throws
   past the 5 MB limit and the handler replies 400 while the client keeps uploading into
   a socket nobody reads. Destroy `req` at the throw site.

## Steps

- [ ] Write the four new tests FIRST and record that each fails on unmodified code
- [ ] `server/index.mjs`: extract `resolveHost` / `createProxyServer` / `startProxy`, bind
      `HOST` (default loopback), log the bound address, warn on non-loopback
- [ ] `server/index.mjs`: `req.destroy()` on body-limit overflow
- [ ] `electron/serve.mjs`: CSP header for `.html`; fix the containment check
- [ ] `electron/config.mjs`: `chmodSync` after write
- [ ] `electron/main.mjs`: `will-navigate` / `setWindowOpenHandler` / permission handler
- [ ] Rebuild `web/dist` and load it through `startServer` in a real Chromium to prove the
      CSP admits the app (no violation, WASM instantiates, font loads)
- [ ] Update `docs/DEVELOPMENT.md` and `CLAUDE.md` (env table gains `HOST`)

## How this will be measured

Not a DSP slice, so the numbers are network- and filesystem-observable:

- `ss -ltnp` / `server.address()` reports `127.0.0.1:8787`, not `0.0.0.0:8787`;
  a connect to the machine's own non-loopback address is refused. With `HOST=0.0.0.0`
  the same connect succeeds — proving the override is real and not a no-op.
- `curl -sI` on `/` shows a `content-security-policy` header; a Chromium page load of the
  built app under that exact policy reports **zero** CSP violations and a live
  `AudioContext` + instantiated WASM module.
- `stat -c %a config.json` is `600` after `saveApiKey` over a pre-existing `0644` file.
- Each of the four new tests fails against `git stash`-ed source and passes after.
- `npm run test:server` (11 → 14), `cd electron && npm test` (16 → 19), `npm run test:history`.

## Manual test steps

- [ ] `npm run server`, then `curl -s localhost:8787/api/health` → `{"ok":true,...}`
- [ ] `curl -s <this-host-non-loopback-ip>:8787/api/health` → connection refused
- [ ] Startup log prints the bound address `http://127.0.0.1:8787`, not a guess
- [ ] `HOST=0.0.0.0 npm run server` → binds `0.0.0.0`, log prints it and a warning
- [ ] Edge case: `HOST=not-a-host npm run server` → fails loudly at bind, does not
      silently fall back to all-interfaces
- [ ] Edge case: pre-create `config.json` at `0644`, save a key, confirm `600`
- [ ] Edge case: request `/dist-evil/…`-shaped sibling path → SPA shell, never an escape

## Out of scope for this session

- **Authentication and rate limiting on the proxy.** Real gaps named in the same audit
  section; they change the client contract and deserve their own slice.
- The assistant's model id, request shape, `cache_control` placement — verified correct.
- The per-turn rig-JSON preamble never being trimmed (same audit section, different
  concern: token cost, not security).
- Keychain storage for the API key instead of plaintext `config.json`.
- A CSP for the browser-hosted build (belongs to the deploying host).
- Any `core/`, `web/worklet/`, or DSP change. No WASM rebuild in this slice.

---

<!-- Fill in below during/after the session -->

## What actually happened

All six fixes landed as planned. Four things worth recording:

1. **The CSP needed two relaxations, and both were proven necessary rather than
   assumed.** I served the real `vite build` output through `startServer` in a real
   Chromium and A/B'd single directives:
   - Without `'wasm-unsafe-eval'`: `CompileError: WebAssembly.instantiate(): Refused
     to compile or instantiate WebAssembly module because 'unsafe-eval' is not an
     allowed source of script`. The audit's suggested `script-src 'self'` would have
     shipped a desktop app that renders perfectly and makes **no sound at all** —
     and nothing in the current test suite would have caught it.
   - Without `font-src 'self' data:`: `Refused to load the font
     'data:font/woff2;base64,…'`. `default-src 'self'` does not admit `data:`, and
     `tokens.css` has exactly one such `@font-face`.

   Also confirmed the *good* news: the built `index.html` has **no inline script**
   (single chunk, so Vite injects no modulepreload polyfill), so `script-src` needs
   no `'unsafe-inline'` and no hash. That is the whole point of the directive here —
   assistant-authored `<script>` cannot run.

2. **`server/index.mjs` had to grow exports before the bind address was testable at
   all.** The file self-`listen`ed at import time, so no test could touch it. It now
   exports `resolveHost` / `resolvePort` / `createProxyServer` / `startProxy` /
   `startupBanner` and gates `listen` on being the process entry point. The request
   handler moved verbatim — routing, CORS, the upstream call and `cache_control`
   are untouched.

3. **One incidental bug found while extracting the port logic:** `Number(process.env.PORT) || 8787`
   truthy-tests the port, so `PORT=0` silently became 8787 instead of "ephemeral".
   Fixed in `resolvePort`, which is also what lets the new tests bind ephemerally
   from env alone.

4. **`setPermissionCheckHandler` needed origin normalisation.** Electron has passed
   `requestingOrigin` both bare and with a trailing slash across versions; a strict
   `===` would have silently denied `media` and killed microphone input — i.e. the
   product. Both sides now go through `originOf()`.

Deviation from plan: also added `object-src 'none'` / `frame-ancestors 'none'`
alongside the planned `base-uri 'none'`. All three are free (no app functionality
touches them, verified zero violations) and closed by `default-src` fallback only
in the `object-src` case.

## Measured results

**Bind address (the finding).** `/proc/net/tcp` local_address for port 8787 (`2253` hex):

| | before | after |
| --- | --- | --- |
| listening socket | `00000000:2253` (all interfaces) | `0100007F:2253` (127.0.0.1) |
| `curl http://127.0.0.1:8787/api/health` | `{"ok":true,…}` | `{"ok":true,…}` |
| `curl http://192.0.2.2:8787/api/health` (this host's own LAN address) | answered | **connection refused** (curl exit 7) |
| startup banner | `http://localhost:8787` regardless of real bind | `http://127.0.0.1:8787`, read from `server.address()` |

`HOST=0.0.0.0` → binds `00000000:2253`, LAN address answers again, and the banner
adds `WARNING: bound to 0.0.0.0 (all interfaces / non-loopback). This proxy has NO
auth and NO rate limit…`. `HOST=not-a-real-host` → `Clipper proxy failed to bind
not-a-real-host:8787 — getaddrinfo ENOTFOUND`, **exit 1**, no fallback bind.

**CSP, against the real built app in Chromium** (`web/dist` served by `serve.mjs`):

| policy | `#root` mounted | webfont | WASM | CSP violations |
| --- | --- | --- | --- | --- |
| as shipped | yes | loaded | **instantiated** | **0** |
| minus `'wasm-unsafe-eval'` | yes | loaded | **CompileError — refused** | 2 |
| minus `font-src data:` | yes | **refused** | instantiated | 1 |

**Body-limit overflow.** 6 MB POST to `/api/chat`: client now sees `ECONNRESET` and
the socket is closed within the same turn (`still-open after 3s? false`); the server
survives and `/api/health` still answers. Before, the 400 was written and the
request stream was left undrained.

**File mode.** Pre-existing `config.json` at `0644` → after `saveApiKey`, `stat`
reports `0600` (test asserts `mode & 0o777`; before: `420` = `0644`, expected `384`
= `0600`).

**Suites.**

| suite | before | after |
| --- | --- | --- |
| `npm run test:server` | 11 pass | **15 pass** |
| `npm run test:history` | 10 pass | 10 pass (untouched) |
| `cd electron && npm test` | 16 pass | **20 pass** |
| `ctest` (core, untouched by this slice) | 16/16 | 16/16 |
| `cd web && npm run build` (`tsc --noEmit` + `vite build`) | clean | clean |

**Each new test fails against unmodified source** — verified before writing any
implementation:

| new test | failure on unmodified code |
| --- | --- |
| loopback bind + `HOST` override + banner (4 tests, `server/index.test.mjs`) | `SyntaxError: The requested module './index.mjs' does not provide an export named 'DEFAULT_HOST'` — the file could not be imported at all |
| CSP header on HTML | `content-security-policy` header is `null` |
| containment rejects sibling prefix | `'/app/dist-evil/secret'.startsWith('/app/dist')` → `true`, test expects `false` |
| `0600` over a pre-existing `0644` | `AssertionError: expected 384, actual 420` |

Note the honest one: the *fresh-file* `0600` test passes before the fix too (mode
applies on creation). The pre-existing-`0644` case is the load-bearing assertion,
which is why the plan called it out specifically.

## Files created / modified

- `server/index.mjs` — `HOST`/loopback default, `resolvePort`, `startupBanner`,
  exports + entry-point-gated `listen`, `req.destroy()` on body overflow
- `server/index.test.mjs` — **new**, 4 tests (bind address, LAN unreachability,
  `HOST` override, banner honesty)
- `electron/serve.mjs` — exported `CSP` + header on `.html`, `isContainedIn`
- `electron/serve.test.mjs` — +2 tests (CSP header, containment predicate)
- `electron/config.mjs` — explicit `chmodSync(…, 0o600)` after write
- `electron/config.test.mjs` — +2 tests (mode over pre-existing `0644`, fresh file)
- `electron/main.mjs` — `will-navigate` deny, `setWindowOpenHandler` deny +
  `shell.openExternal` allowlist, `will-attach-webview` deny, media-only permission
  request/check handlers, explicit `sandbox: true`
- `docs/DEVELOPMENT.md` — §11 Security notes (binding, `HOST`, body limit, the new
  exports); Electron section (new **Shell hardening** subsection with the measured
  CSP relaxation table; `chmod` note; test counts)
- `docs/decisions/001-proxy-loopback-and-shell-security-posture.md` — **new** ADR
- `CLAUDE.md` — `HOST` in the env table, Current State
- `docs/work/2026-07-25-proxy-loopback.md` — this file

No change under `core/` or `web/worklet/`, so **no WASM rebuild** and no golden
re-blessing. No change under `web/src/`, so the web chain and the native chain
remain in parity — this slice does not touch the signal chain at all.

## Deferred to next session

- **Auth + rate limiting on the proxy** (audit, same section). Loopback binding is a
  mitigation, not a fix: `HOST=0.0.0.0` is still fully exposed, which is why the
  banner names the consequence. This changes the client contract, so it needs its
  own slice.
- **A CSP for the browser-hosted build.** The desktop shell is covered; `vite
  preview` / any static host is not. Belongs with the deploy notes (and the Vercel
  lift described in §11).
- **Keychain instead of plaintext `config.json`.** The `0600` is now reliable, but
  the key is still plain text on disk.
- **Playwright web suite not run here** — the container has Chromium build 1194 while
  this Playwright wants 1228, and the download is blocked. Unaffected by this slice
  (nothing under `web/` changed) and `npm run build` is clean, but it is unverified
  in this session. The CSP verification used the 1194 binary directly.
- **`electron/main.mjs` remains untested.** The hardening handlers are registered on
  real Electron objects, so a unit test would be a mock asserting itself. Verifying
  `will-navigate` / `setWindowOpenHandler` / the permission handler for real needs a
  launched Electron window — that belongs with the headless launch probe, on a
  machine that can run the Electron binary.
- The audit's three shipping-blockers (NaN parameters, cab-swap allocation,
  `CabConvolver` block size) are still untouched and remain ahead of everything else.

## Status

- [ ] In progress
- [x] Complete
- [ ] Partial — see deferred
