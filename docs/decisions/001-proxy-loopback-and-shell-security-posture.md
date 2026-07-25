# ADR 001: The proxy binds loopback, and the app's CSP lives in the server, not the HTML

Date: 2026-07-25
Status: Accepted

## Context

`server/index.mjs` is an **unauthenticated** relay to the developer's Anthropic API
key. It called `server.listen(PORT, cb)` with no host, which binds `0.0.0.0`/`::`.
The 2026-07-24 audit (finding 17) verified both `/api/health` and `/api/chat`
answering from a non-loopback address, while the startup banner printed
`http://localhost:8787` — so the exposure was invisible in the one place a
developer looks. `electron/serve.mjs` had bound `127.0.0.1` correctly all along,
so this was a divergence between two implementations of the same glue, not a
considered decision.

Separately, the Electron main window renders assistant output — model-influenced
text — with no Content-Security-Policy, while the much less exposed
`key-prompt.html` had one. Adding a CSP raised a real question of *where* the
policy should live, because the same `web/` tree is served three ways: by `vite
dev` (HMR), by `vite preview` / any static host for the browser build, and by
`electron/serve.mjs` for the desktop shell.

Two options were considered for the bind address: hardcode loopback with no
override, or default to loopback with an env-var opt-out. And two for the CSP: a
`<meta http-equiv>` tag in `web/index.html`, or a response header from the static
server.

## Decision

**1. Loopback by default, `HOST` to opt out.** `HOST` defaults to `127.0.0.1`. A
non-loopback value is honoured but the startup banner prints a warning naming the
consequence (no auth, no rate limit, anyone who reaches the port spends the key).
An unresolvable `HOST` fails the bind with a non-zero exit rather than falling
back to every interface. The banner always reports the address read back from
`server.address()`, never a hardcoded string.

A hard-coded loopback bind was rejected: exposing the proxy to a phone or a second
machine on a trusted LAN is a legitimate thing to want, and someone who cannot do
it through a documented env var will do it by editing the `listen` call — which is
strictly worse, because it is invisible and it drifts.

**2. The CSP is a response header from `electron/serve.mjs`, keyed on `.html`.**
Not a `<meta>` tag in `web/index.html`. A meta tag would also apply under `vite
dev`, where `script-src 'self'` fights HMR, and it would silently become the
policy for any static host — the browser deploy's CSP belongs to whatever serves
it, where it can also carry the headers a meta tag cannot express
(`frame-ancestors`, and eventually HSTS/COOP). The policy string is exported as
`CSP` so the test asserts the value actually served rather than a copy of it.

**3. Every CSP relaxation is justified by a measurement, not by assumption.** The
policy is A/B'd against the real `vite build` output in Chromium: removing a
directive must produce a named, observable failure. This is how
`'wasm-unsafe-eval'` (Chromium refuses **all** Wasm compilation once `script-src`
exists) and `font-src data:` (the one `@font-face` in `tokens.css` is a base64
data URI) earned their place, and it is what keeps the next person from "tidying"
either one away.

## Consequences

- The largest exposure in the audit is closed by configuration rather than by
  code paths that can rot: the default is safe, and the unsafe choice announces
  itself at every startup.
- `server/index.mjs` grew exports (`resolveHost`, `resolvePort`,
  `createProxyServer`, `startProxy`, `startupBanner`) and now self-`listen`s only
  when it is the process entry point. That is what makes the bind address
  testable at all — previously the file could not be imported without starting a
  server. `npm run server` is unchanged.
- `PORT=0` now means "ephemeral" rather than 8787, because the old
  `Number(env.PORT) || 8787` truthy-tested a valid port. A deployment that
  relied on `PORT=0` meaning 8787 would change behaviour; none exists.
- **The desktop shell and the browser build now have different CSP posture.** The
  shell is covered; the browser build is not, and will not be until whoever
  deploys it sets headers. That asymmetry is deliberate but it is a real gap, and
  it should be recorded in the deploy notes rather than rediscovered.
- Loopback binding is a *mitigation*, not a fix, for having no auth and no rate
  limit on the proxy. Those remain open. Anyone who sets `HOST=0.0.0.0` before
  they land is fully exposed, which is exactly why the warning names them.
- Pinning `sandbox: true` and denying `window.open` mean any future need for a
  real popup window or an external link must be added deliberately, via the
  `shell.openExternal` allowlist. That is the intended cost.
