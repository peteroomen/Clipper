// Guard the committed WASM artifact: fail the build/test if it is missing OR
// STALE relative to the sources it was built from.
//
// The artifact is produced by scripts/build-wasm.sh at the repo root, which also
// writes the build stamp this script verifies. See web/scripts/artifact-stamp.mjs
// for what the stamp covers and what it deliberately does not.
//
// Four sub-checks, all reported together rather than first-failure-wins, because
// a stale artifact and a diverged worklet have the same fix and you want to see
// both at once:
//
//   1. clipper.js and clipper-processor.js exist               (the original check)
//   2. generated/clipper-processor.js is BYTE-IDENTICAL to web/worklet/…
//      build-wasm.sh copies it with `cp`, so an exact comparison is possible and
//      a hash would be a strictly worse answer.
//   3. the stamp exists, parses, and its sourceHash matches a fresh recomputation
//      over core/src, core/include, the worklet, and the emcc flag region
//   4. ADVISORY: if emcc is on PATH, its version matches the recorded one
//
// Sub-check 3 needs no toolchain, which is the point — CI's web job has no emsdk
// and must still catch a stale artifact. Sub-check 4 is skipped with a printed
// notice when emcc is absent, and only ever WARNS when it mismatches: a newer
// emcc does not make the artifact stale relative to the source, it only means a
// rebuild would produce different bytes.
//
// Usage: node scripts/check-artifact.mjs [--repo-root DIR]
//   --repo-root exists so the test suite can point the real script at a synthetic
//   repo tree instead of re-implementing it (web/scripts/check-artifact.test.mjs).

import { existsSync, readFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join, resolve } from 'node:path';

import {
  STAMP_RELATIVE_PATH,
  computeSourceHash,
  diffFileHashes,
  readStamp,
} from './artifact-stamp.mjs';

const REBUILD_HINT =
  'Fix:  bash scripts/build-wasm.sh     (from the repo root)\n' +
  '      then commit the regenerated artifact + stamp in the SAME commit.\n' +
  '      (run scripts/setup-emsdk.sh once beforehand if emcc is absent)';

/** The recorded emcc version, or null when emcc is not installed. */
function detectEmccVersion() {
  const result = spawnSync('emcc', ['--version'], { encoding: 'utf8', timeout: 30_000 });
  if (result.error || result.status !== 0 || typeof result.stdout !== 'string') return null;
  const first = result.stdout.split('\n')[0].trim();
  return first === '' ? null : first;
}

/**
 * Run every sub-check against the repo at `repoRoot`.
 * Returns { failures: string[], warnings: string[], notes: string[] } — the
 * caller decides what to do with them, so this stays testable.
 */
export function checkArtifact(repoRoot, { emccVersion = detectEmccVersion } = {}) {
  const failures = [];
  const warnings = [];
  const notes = [];

  const artifact = join(repoRoot, 'web/public/generated/clipper.js');
  const workletCopy = join(repoRoot, 'web/public/generated/clipper-processor.js');
  const workletSource = join(repoRoot, 'web/worklet/clipper-processor.js');

  // --- 1. existence -------------------------------------------------------
  for (const [label, path] of [
    ['WASM module', artifact],
    ['worklet copy', workletCopy],
  ]) {
    if (!existsSync(path)) failures.push(`Missing ${label}: ${path}`);
  }

  // --- 2. the worklet copy is a byte-for-byte copy ------------------------
  if (existsSync(workletCopy) && existsSync(workletSource)) {
    const copy = readFileSync(workletCopy);
    const source = readFileSync(workletSource);
    if (!copy.equals(source)) {
      failures.push(
        'The committed worklet copy is NOT byte-identical to its source.\n' +
          `    source: web/worklet/clipper-processor.js        (${source.length} bytes)\n` +
          `    copy:   web/public/generated/clipper-processor.js (${copy.length} bytes)\n` +
          '    build-wasm.sh copies this file verbatim, so any difference means the\n' +
          '    committed copy is stale — the browser is running the OLD worklet.'
      );
    }
  } else if (!existsSync(workletSource)) {
    failures.push(`Missing worklet source: ${workletSource}`);
  }

  // --- 3. the stamp matches the sources -----------------------------------
  const stampResult = readStamp(repoRoot);
  if (!stampResult.ok) {
    if (stampResult.reason === 'missing') {
      failures.push(
        `Missing build stamp: ${STAMP_RELATIVE_PATH}\n` +
          '    Without it there is no way to tell whether the committed artifact was\n' +
          '    built from the sources in this tree. A rebuild generates it.'
      );
    } else {
      failures.push(
        `Unusable build stamp: ${STAMP_RELATIVE_PATH} (${stampResult.reason}` +
          `${stampResult.detail ? `: ${stampResult.detail}` : ''})\n` +
          '    The stamp is generated build output — do not hand-edit it. Rebuild.'
      );
    }
  } else {
    const { stamp } = stampResult;
    let current;
    try {
      current = computeSourceHash(repoRoot);
    } catch (error) {
      failures.push(`Cannot recompute the source hash: ${error.message}`);
    }
    if (current !== undefined) {
      if (current.sourceHash === stamp.sourceHash) {
        notes.push(
          `artifact is in step with ${Object.keys(current.files).length} hashed inputs ` +
            `(sourceHash ${current.sourceHash.slice(0, 12)}…)`
        );
      } else {
        const detail = [];
        if (stamp.files && typeof stamp.files === 'object') {
          const { changed, added, removed } = diffFileHashes(stamp.files, current.files);
          for (const path of changed) detail.push(`      changed: ${path}`);
          for (const path of added) detail.push(`      added:   ${path}`);
          for (const path of removed) detail.push(`      removed: ${path}`);
        }
        failures.push(
          'STALE ARTIFACT: web/public/generated/clipper.js was built from different sources\n' +
            '    than the ones in this tree, so the app is bound to an OLD engine — a knob\n' +
            '    you just wired up will do nothing.\n' +
            `      recorded sourceHash: ${stamp.sourceHash}\n` +
            `      current  sourceHash: ${current.sourceHash}` +
            (detail.length > 0
              ? `\n    Inputs that differ from the stamp:\n${detail.join('\n')}`
              : '\n    (the stamp records no per-file map, so the differing input cannot be named)')
        );
      }
    }
  }

  // --- 4. toolchain, advisory only ----------------------------------------
  const detected = typeof emccVersion === 'function' ? emccVersion() : emccVersion;
  const recorded = stampResult.ok ? stampResult.stamp.emccVersion : null;
  if (detected === null || detected === undefined) {
    notes.push(
      'emcc is not on PATH, so the toolchain sub-check was SKIPPED' +
        (recorded ? ` (stamp records: ${recorded})` : '') +
        '. The staleness check above does not need it.'
    );
  } else if (typeof recorded !== 'string' || recorded === '') {
    notes.push(`emcc present (${detected}) but the stamp records no version — nothing to compare.`);
  } else if (recorded !== detected) {
    warnings.push(
      'emcc version differs from the one that built the committed artifact.\n' +
        `      stamp: ${recorded}\n` +
        `      yours: ${detected}\n` +
        '    Not a staleness failure — the sources still match. But a rebuild here will\n' +
        '    produce different bytes for reasons unrelated to your change. The pinned\n' +
        '    version is EMSDK_VERSION in scripts/setup-emsdk.sh.'
    );
  } else {
    notes.push(`emcc matches the stamp (${detected}).`);
  }

  return { failures, warnings, notes };
}

function parseRepoRoot(argv) {
  const index = argv.indexOf('--repo-root');
  if (index >= 0 && argv[index + 1] !== undefined) return resolve(argv[index + 1]);
  // web/scripts/ → repo root
  return resolve(dirname(fileURLToPath(import.meta.url)), '../..');
}

function main() {
  const repoRoot = parseRepoRoot(process.argv.slice(2));
  const { failures, warnings, notes } = checkArtifact(repoRoot);

  for (const note of notes) console.log(`[clipper] ${note}`);
  for (const warning of warnings) console.warn(`\n[clipper] WARNING: ${warning}\n`);

  if (failures.length > 0) {
    console.error(`\n[clipper] The committed WASM artifact check failed:\n`);
    for (const failure of failures) console.error(`  ✗ ${failure}\n`);
    console.error(`${REBUILD_HINT}\n`);
    process.exit(1);
  }
  console.log('[clipper] WASM artifact + worklet present and in step with the sources.');
}

const invokedDirectly =
  process.argv[1] !== undefined && import.meta.url === `file://${process.argv[1]}`;
if (invokedDirectly) main();
