// node --test — the proxy's HTTP glue: WHERE it binds and WHAT it says it bound.
//
// `handler.test.mjs` covers request shaping. This file covers the one property
// that decides whether the developer's Anthropic key is exposed to the LAN: the
// bind address. It asserts the observable thing (a connect from a non-loopback
// address is refused), not just the string `server.address()` reports.
//
// Nothing here calls Anthropic — every request used is /api/health.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { networkInterfaces } from 'node:os';
import { connect } from 'node:net';
import { DEFAULT_HOST, resolveHost, startProxy, startupBanner } from './index.mjs';

// The first non-internal IPv4 address of this machine, or null when there is
// none (a container with only `lo`). Used to prove LAN unreachability.
function externalIpv4() {
  for (const list of Object.values(networkInterfaces())) {
    for (const nic of list || []) {
      if (nic.family === 'IPv4' && !nic.internal) return nic.address;
    }
  }
  return null;
}

// Resolve to the connect error code, or 'connected' if the socket opened.
function probe(host, port, timeoutMs = 2000) {
  return new Promise((resolve) => {
    const sock = connect({ host, port });
    const done = (v) => {
      sock.destroy();
      resolve(v);
    };
    sock.setTimeout(timeoutMs, () => done('ETIMEDOUT'));
    sock.on('connect', () => done('connected'));
    sock.on('error', (err) => done(err.code || 'ERROR'));
  });
}

test('resolveHost defaults to loopback and honours HOST', () => {
  assert.equal(resolveHost({}), '127.0.0.1');
  assert.equal(DEFAULT_HOST, '127.0.0.1');
  assert.equal(resolveHost({ HOST: '' }), '127.0.0.1');
  assert.equal(resolveHost({ HOST: '   ' }), '127.0.0.1');
  assert.equal(resolveHost({ HOST: ' 0.0.0.0 ' }), '0.0.0.0');
  assert.equal(resolveHost({ HOST: '::1' }), '::1');
});

test('by default the proxy is reachable on loopback and NOT from the LAN', async () => {
  const h = await startProxy({ env: { PORT: '0', MOCK: '1' } });
  try {
    assert.equal(h.host, '127.0.0.1');
    assert.equal(h.server.address().address, '127.0.0.1');

    // It answers on loopback.
    const res = await fetch(`http://127.0.0.1:${h.port}/api/health`);
    assert.equal(res.status, 200);
    assert.equal((await res.json()).ok, true);

    // The property that matters: a connect to this machine's own routable
    // address is refused, so nobody on the same network can reach the relay.
    const lan = externalIpv4();
    if (lan) {
      assert.equal(await probe(lan, h.port), 'ECONNREFUSED');
    }
  } finally {
    await h.close();
  }
});

test('HOST=0.0.0.0 deliberately opens the proxy to every interface', async () => {
  const h = await startProxy({ env: { PORT: '0', MOCK: '1', HOST: '0.0.0.0' } });
  try {
    assert.equal(h.host, '0.0.0.0');
    assert.equal(h.server.address().address, '0.0.0.0');

    // The override is real, not cosmetic: the LAN address now accepts.
    const lan = externalIpv4();
    if (lan) {
      assert.equal(await probe(lan, h.port), 'connected');
    }
  } finally {
    await h.close();
  }
});

test('the startup banner reports the address actually bound, never a guess', () => {
  const lan = startupBanner({ host: '0.0.0.0', port: 8787, model: 'm', maxTokens: 1, hasKey: false });
  // The old log hardcoded "localhost" regardless of the real bind address.
  assert.ok(lan.includes('http://0.0.0.0:8787'), lan);
  assert.doesNotMatch(lan, /localhost/);
  // A non-loopback bind must be called out as an exposure, not printed silently.
  assert.match(lan, /all interfaces|WARNING/i);

  const local = startupBanner({ host: '127.0.0.1', port: 8787, model: 'm', maxTokens: 1, hasKey: true });
  assert.ok(local.includes('http://127.0.0.1:8787'), local);
  assert.doesNotMatch(local, /WARNING/i);
});
