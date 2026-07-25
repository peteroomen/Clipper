// Tests for the committed-artifact staleness guard.
//
// The whole point of this file is the second case: a change to core/src with no
// rebuild must fail the check. That is the failure the 2026-07-24 audit called out
// and that a real merge nearly shipped — two PRs each rebuilt clipper.js, the merge
// conflicted on the binary, and taking either side would have produced a main whose
// committed engine held ONE of the two fixes while the source held both.
//
// Method: build a synthetic repo tree in a temp dir (the same directory layout the
// real check walks), stamp it with the real writer, mutate one thing, and run the
// real CLI against it via `--repo-root`. Running the actual script as a subprocess
// rather than importing checkArtifact() is deliberate — it pins the exit code and
// the message a developer will actually see, and it let these same cases be pointed
// at the PRE-FIX script (see CLIPPER_CHECK_ARTIFACT_SCRIPT below) to confirm they
// fail against it. 3 of the 5 required cases do; the other two ("a good tree
// passes", "a core/tests edit does not fire") pass vacuously before the fix,
// because the old script had no notion of staleness to get wrong. They are here to
// pin the absence of false positives now that it does.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, rmSync, cpSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { dirname, join, resolve } from 'node:path';

import { writeStamp, STAMP_RELATIVE_PATH } from './artifact-stamp.mjs';
import { checkArtifact } from './check-artifact.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(here, '../..');

// Which script is under test. Overridable so the pre-fix version can be run
// through the identical cases; it must live in web/scripts/ for that to be a fair
// comparison, because the old script resolves the artifact relative to its own
// location.
const SCRIPT = process.env.CLIPPER_CHECK_ARTIFACT_SCRIPT
  ? resolve(process.env.CLIPPER_CHECK_ARTIFACT_SCRIPT)
  : join(here, 'check-artifact.mjs');

const WORKLET_BODY = '// synthetic worklet\nclass P extends AudioWorkletProcessor {}\n';

/**
 * A synthetic repo that the real check considers healthy: sources, a worklet and
 * its byte-identical copy, an artifact, a build script carrying the marker region,
 * and a matching stamp.
 */
function makeRepo() {
  const root = mkdtempSync(join(tmpdir(), 'clipper-artifact-'));
  const write = (relativePath, content) => {
    const path = join(root, relativePath);
    mkdirSync(dirname(path), { recursive: true });
    writeFileSync(path, content);
  };

  write('core/src/dsp/RatModel.cpp', '// rat\nfloat rat(float x) { return x; }\n');
  write('core/src/clipper_c_api.cpp', '// c abi\n');
  write('core/include/clipper/dsp/RatModel.h', '#pragma once\nfloat rat(float);\n');
  // Not compiled into the artifact — the check must NOT fire on these.
  write('core/tests/test_rat.cpp', '// a test\nint main() { return 0; }\n');
  write('core/tools/render.cpp', '// a harness\nint main() { return 0; }\n');
  write('web/worklet/clipper-processor.js', WORKLET_BODY);
  write('web/public/generated/clipper-processor.js', WORKLET_BODY);
  write('web/public/generated/clipper.js', 'export default function () {} // fake wasm\n');
  // The real build script, so the marker-region contract is exercised for real
  // rather than mocked.
  mkdirSync(join(root, 'scripts'), { recursive: true });
  cpSync(join(REPO_ROOT, 'scripts/build-wasm.sh'), join(root, 'scripts/build-wasm.sh'));

  writeStamp(root, { emccVersion: 'emcc (synthetic) 6.0.4' });
  return root;
}

function run(root) {
  const result = spawnSync(process.execPath, [SCRIPT, '--repo-root', root], {
    encoding: 'utf8',
    timeout: 60_000,
  });
  return { status: result.status, output: `${result.stdout ?? ''}${result.stderr ?? ''}` };
}

function withRepo(fn) {
  const root = makeRepo();
  try {
    return fn(root);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
}

const edit = (root, relativePath, content) => writeFileSync(join(root, relativePath), content);

test('passes on a tree whose stamp matches its sources', () => {
  withRepo((root) => {
    const { status, output } = run(root);
    assert.equal(status, 0, `expected exit 0, got ${status}:\n${output}`);
  });
});

test('FAILS when a core/src file changes without a rebuild', () => {
  // The load-bearing case. This is the merge hazard that motivated the slice.
  withRepo((root) => {
    edit(root, 'core/src/dsp/RatModel.cpp', '// rat\nfloat rat(float x) { return 2.0f * x; }\n');
    const { status, output } = run(root);
    assert.equal(status, 1, `expected exit 1 for a stale artifact, got ${status}:\n${output}`);
    assert.match(output, /STALE ARTIFACT/);
    // It must name the culprit, not just say "something changed".
    assert.match(output, /changed: core\/src\/dsp\/RatModel\.cpp/);
    assert.match(output, /bash scripts\/build-wasm\.sh/);
  });
});

test('FAILS when a core/include header changes without a rebuild', () => {
  withRepo((root) => {
    edit(root, 'core/include/clipper/dsp/RatModel.h', '#pragma once\nfloat rat(float, float);\n');
    const { status, output } = run(root);
    assert.equal(status, 1, `expected exit 1, got ${status}:\n${output}`);
    assert.match(output, /changed: core\/include\/clipper\/dsp\/RatModel\.h/);
  });
});

test('FAILS when the committed worklet copy diverges byte-wise from its source', () => {
  withRepo((root) => {
    edit(root, 'web/public/generated/clipper-processor.js', `${WORKLET_BODY}// drifted\n`);
    const { status, output } = run(root);
    assert.equal(status, 1, `expected exit 1 for a diverged worklet, got ${status}:\n${output}`);
    assert.match(output, /NOT byte-identical/);
  });
});

test('FAILS when the worklet source changes but the copy and stamp do not', () => {
  // The mirror of the case above: editing the authored worklet leaves the served
  // copy behind, so this must trip both the byte-identity and the source hash.
  withRepo((root) => {
    edit(root, 'web/worklet/clipper-processor.js', `${WORKLET_BODY}// new feature\n`);
    const { status, output } = run(root);
    assert.equal(status, 1, `expected exit 1, got ${status}:\n${output}`);
    assert.match(output, /NOT byte-identical/);
    assert.match(output, /STALE ARTIFACT/);
  });
});

test('FAILS when the stamp is missing entirely', () => {
  withRepo((root) => {
    rmSync(join(root, STAMP_RELATIVE_PATH));
    const { status, output } = run(root);
    assert.equal(status, 1, `expected exit 1 for a missing stamp, got ${status}:\n${output}`);
    assert.match(output, /Missing build stamp/);
    assert.match(output, /bash scripts\/build-wasm\.sh/);
  });
});

test('FAILS on a malformed stamp rather than silently passing', () => {
  withRepo((root) => {
    edit(root, STAMP_RELATIVE_PATH, '{ this is not json');
    const { status, output } = run(root);
    assert.equal(status, 1, `expected exit 1, got ${status}:\n${output}`);
    assert.match(output, /Unusable build stamp/);
  });
});

test('FAILS when an emcc flag changes without a rebuild', () => {
  // The flag string is hashed out of build-wasm.sh's marked region, so an
  // -O3 → -O2 edit is a staleness event even with every source byte untouched.
  withRepo((root) => {
    const path = join(root, 'scripts/build-wasm.sh');
    const patched = readFileSync(path, 'utf8').replace('\n    -O3\n', '\n    -O2\n');
    assert.notEqual(patched, readFileSync(path, 'utf8'), 'the -O3 flag line moved; fix this test');
    writeFileSync(path, patched);
    const { status, output } = run(root);
    assert.equal(status, 1, `expected exit 1 for a flag change, got ${status}:\n${output}`);
    assert.match(output, /changed: scripts\/build-wasm\.sh#emcc-args/);
  });
});

test('does NOT fire for an edit under core/tests/ or core/tools/', () => {
  // These are not compiled into the artifact (measured: `g++ -MM` over the 26 emcc
  // translation units yields a closure containing zero files from either). A guard
  // that fires on every test edit is a guard someone will delete.
  withRepo((root) => {
    edit(root, 'core/tests/test_rat.cpp', '// a test, now with more assertions\nint main(){}\n');
    edit(root, 'core/tools/render.cpp', '// a harness, rewritten\nint main(){}\n');
    const { status, output } = run(root);
    assert.equal(status, 0, `a test/tool edit must not fail the check, got ${status}:\n${output}`);
  });
});

// The toolchain sub-check is advisory by design, so it is pinned at the function
// level with an injected version rather than through the CLI: a mismatched emcc
// must WARN, and an absent emcc must be a skip note, but neither may fail the
// build. A different emcc does not make the artifact stale relative to the source,
// and CI's web job has no emsdk at all.
test('an emcc version mismatch warns but does not fail', () => {
  withRepo((root) => {
    const { failures, warnings } = checkArtifact(root, {
      emccVersion: () => 'emcc (Emscripten gcc/clang-like replacement) 7.1.0 (deadbeef)',
    });
    assert.deepEqual(failures, [], 'a toolchain mismatch must not fail the check');
    assert.equal(warnings.length, 1);
    assert.match(warnings[0], /emcc version differs/);
    assert.match(warnings[0], /Not a staleness failure/);
  });
});

test('an absent emcc is a skip note, not a failure or a warning', () => {
  withRepo((root) => {
    const { failures, warnings, notes } = checkArtifact(root, { emccVersion: () => null });
    assert.deepEqual(failures, []);
    assert.deepEqual(warnings, []);
    assert.ok(
      notes.some((note) => /toolchain sub-check was SKIPPED/.test(note)),
      `expected a skip note, got: ${JSON.stringify(notes)}`
    );
  });
});

test('FAILS when the artifact itself is missing (the original check, kept)', () => {
  withRepo((root) => {
    rmSync(join(root, 'web/public/generated/clipper.js'));
    const { status, output } = run(root);
    assert.equal(status, 1, `expected exit 1, got ${status}:\n${output}`);
    assert.match(output, /Missing WASM module/);
  });
});
