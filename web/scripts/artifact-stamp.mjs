// The build stamp for the committed WASM artifact.
//
// WHY THIS EXISTS
// ---------------
// web/public/generated/clipper.js is committed build output, so a `git pull`
// updates the engine without an Emscripten toolchain. The contract (CLAUDE.md →
// "The committed WASM artifact") is: change core/ or web/worklet/, rebuild, and
// commit the regenerated artifact in the same commit. Until this module existed
// nothing enforced it — check-artifact.mjs called existsSync and passed for an
// arbitrarily old artifact.
//
// It is not a theoretical hole. Two PRs each changed core/ and each rebuilt
// clipper.js; the merge conflicted on the binary, and taking either side would
// have produced a main whose committed engine contained ONE of the two fixes
// while the source contained both. Nothing in the repo could have detected it.
//
// HOW IT WORKS
// ------------
// scripts/build-wasm.sh writes .build-stamp.json next to the artifact, holding a
// SHA-256 over the CONTENTS of every input that affects the artifact.
// check-artifact.mjs recomputes that hash from the working tree and fails on a
// mismatch. Both directions call the functions below, so there is exactly one
// definition of "the hash" — note that sharing the algorithm is not a
// self-consistency hole, because the INPUTS are the real files on disk in both
// directions.
//
// The hash is deterministic and order-independent: the file list is sorted by
// POSIX-relative path and only file CONTENTS are hashed, never mtimes.
//
// WHAT IS COVERED, AND WHAT IS DELIBERATELY NOT
// ---------------------------------------------
// Covered:
//   - every C/C++ source and header under core/src/ and core/include/
//   - web/worklet/clipper-processor.js (the file build-wasm.sh copies)
//   - the emcc source-list + flag region of scripts/build-wasm.sh (see
//     extractMarkedRegion below) — a flag change alters the artifact as surely
//     as a source change
// NOT covered, on purpose:
//   - core/tools/ and core/tests/. Measured, not assumed: `g++ -std=c++17 -MM`
//     over the exact 26 translation units build-wasm.sh hands to emcc, with the
//     same -I core/include, yields a 62-file closure containing ZERO files from
//     either directory. Hashing them would fire the staleness check on every
//     test edit, which is how a guard gets disabled.
//   - core/CMakeLists.txt, and with it the pinned chowdsp_wdf commit SHA. A pin
//     bump does change the artifact and this stamp will not notice; hashing the
//     whole file would fire on unrelated test-target edits. See ADR 004.
//   - the emcc version, which is RECORDED but not hashed. Recomputing the source
//     hash must work with no toolchain installed (CI's web job has no emsdk), so
//     the toolchain comparison is a separate, skippable sub-check.
//
// The 26/26 core/src and 36/37 core/include closure numbers are in
// docs/work/2026-07-25-artifact-staleness.md.

import { createHash } from 'node:crypto';
import { readFileSync, readdirSync, statSync, existsSync, writeFileSync } from 'node:fs';
import { join, relative, sep } from 'node:path';

export const STAMP_RELATIVE_PATH = 'web/public/generated/.build-stamp.json';
export const STAMP_VERSION = 1;

// The build script is the single source of truth for the emcc invocation, so the
// flag string is hashed out of it rather than self-reported into the stamp.
// Self-reported flags cannot detect their own staleness: edit a flag without
// rebuilding and the stamp would still agree with itself.
export const BUILD_SCRIPT_RELATIVE_PATH = 'scripts/build-wasm.sh';
export const EMCC_ARGS_BEGIN_MARKER = 'STAMP:EMCC-ARGS BEGIN';
export const EMCC_ARGS_END_MARKER = 'STAMP:EMCC-ARGS END';
// The pseudo-entry name under which that region appears in the file map.
export const EMCC_ARGS_ENTRY = 'scripts/build-wasm.sh#emcc-args';

// Only files that can reach the compiler. A stray README under core/src/ must not
// invalidate the artifact.
const SOURCE_EXTENSIONS = new Set([
  '.c', '.cc', '.cpp', '.cxx',
  '.h', '.hh', '.hpp', '.hxx',
  '.inl', '.ipp',
]);

// Directories walked in full, plus single files, that the artifact is built from.
export const HASHED_DIRECTORIES = ['core/src', 'core/include'];
export const HASHED_FILES = ['web/worklet/clipper-processor.js'];

function sha256(buffer) {
  return createHash('sha256').update(buffer).digest('hex');
}

function hasSourceExtension(name) {
  const dot = name.lastIndexOf('.');
  return dot > 0 && SOURCE_EXTENSIONS.has(name.slice(dot).toLowerCase());
}

/** Every source/header file under `dir`, as repo-relative POSIX paths. */
function walkSources(repoRoot, dir) {
  const absolute = join(repoRoot, dir);
  if (!existsSync(absolute)) return [];
  const found = [];
  const stack = [absolute];
  while (stack.length > 0) {
    const current = stack.pop();
    for (const entry of readdirSync(current, { withFileTypes: true })) {
      const path = join(current, entry.name);
      if (entry.isDirectory()) {
        stack.push(path);
      } else if (entry.isFile() && hasSourceExtension(entry.name)) {
        found.push(relative(repoRoot, path).split(sep).join('/'));
      }
    }
  }
  return found;
}

/**
 * The substantive text between two marker lines in `text`, with comment-only and
 * blank lines dropped and trailing whitespace trimmed. Dropping comments is what
 * keeps the stamp from firing when someone edits a comment inside the emcc block;
 * the trade is that a change hidden entirely in a comment is invisible, which is
 * fine because a comment cannot change the artifact.
 *
 * Returns null when either marker is absent.
 */
export function extractMarkedRegion(text, beginMarker, endMarker) {
  const lines = text.replace(/\r\n/g, '\n').split('\n');
  const begin = lines.findIndex((line) => line.includes(beginMarker));
  if (begin < 0) return null;
  const end = lines.findIndex((line, i) => i > begin && line.includes(endMarker));
  if (end < 0) return null;
  return lines
    .slice(begin + 1, end)
    .map((line) => line.replace(/\s+$/, ''))
    .filter((line) => line.trim() !== '' && !line.trim().startsWith('#'))
    .join('\n');
}

/**
 * Read the emcc source-list + flag region out of scripts/build-wasm.sh.
 * Throws when the markers are missing — that means the build script and this
 * module have drifted apart, which must be loud rather than silently unhashed.
 */
export function readEmccArgsRegion(repoRoot) {
  const path = join(repoRoot, BUILD_SCRIPT_RELATIVE_PATH);
  if (!existsSync(path)) {
    throw new Error(`cannot hash the emcc flags: ${BUILD_SCRIPT_RELATIVE_PATH} not found`);
  }
  const region = extractMarkedRegion(
    readFileSync(path, 'utf8'),
    EMCC_ARGS_BEGIN_MARKER,
    EMCC_ARGS_END_MARKER
  );
  if (region === null) {
    throw new Error(
      `cannot hash the emcc flags: ${BUILD_SCRIPT_RELATIVE_PATH} is missing the ` +
        `"${EMCC_ARGS_BEGIN_MARKER}" / "${EMCC_ARGS_END_MARKER}" markers. ` +
        'They delimit the region web/scripts/artifact-stamp.mjs hashes; restore them.'
    );
  }
  return region;
}

/**
 * The per-input SHA-256 map for a repo tree: repo-relative path → content hash,
 * plus the emcc-args pseudo-entry. Sorted by key.
 */
export function computeFileHashes(repoRoot) {
  const paths = [];
  for (const dir of HASHED_DIRECTORIES) paths.push(...walkSources(repoRoot, dir));
  for (const file of HASHED_FILES) {
    if (existsSync(join(repoRoot, file))) paths.push(file);
  }
  paths.sort();

  const hashes = {};
  for (const path of paths) hashes[path] = sha256(readFileSync(join(repoRoot, path)));
  // Insertion order does not matter: foldFileHashes re-sorts the keys.
  hashes[EMCC_ARGS_ENTRY] = sha256(Buffer.from(readEmccArgsRegion(repoRoot), 'utf8'));
  return hashes;
}

/**
 * Fold a file-hash map into one digest. Order-independent by construction: the
 * keys are sorted and both the path and its hash are fed in with separators, so
 * no two distinct maps can produce the same byte stream.
 */
export function foldFileHashes(fileHashes) {
  const hash = createHash('sha256');
  hash.update(`clipper-artifact-stamp/v${STAMP_VERSION}\n`);
  for (const path of Object.keys(fileHashes).sort()) {
    hash.update(`${path}\0${fileHashes[path]}\n`);
  }
  return hash.digest('hex');
}

/** `{ sourceHash, files }` for the tree at `repoRoot`. */
export function computeSourceHash(repoRoot) {
  const files = computeFileHashes(repoRoot);
  return { sourceHash: foldFileHashes(files), files };
}

/**
 * Compare a recorded file map against a freshly computed one.
 * Returns the changed/added/removed repo-relative paths.
 */
export function diffFileHashes(recorded, current) {
  const changed = [];
  const added = [];
  const removed = [];
  for (const path of Object.keys(current).sort()) {
    if (!(path in recorded)) added.push(path);
    else if (recorded[path] !== current[path]) changed.push(path);
  }
  for (const path of Object.keys(recorded).sort()) {
    if (!(path in current)) removed.push(path);
  }
  return { changed, added, removed };
}

/**
 * Write web/public/generated/.build-stamp.json for the tree at `repoRoot`.
 * Called by scripts/build-wasm.sh immediately after a successful emcc run.
 */
export function writeStamp(repoRoot, { emccVersion = null, artifactPath = null } = {}) {
  const { sourceHash, files } = computeSourceHash(repoRoot);
  const stamp = {
    version: STAMP_VERSION,
    comment:
      'Generated by scripts/build-wasm.sh. Verified by web/scripts/check-artifact.mjs. ' +
      'Do not hand-edit: a hand-written stamp is a lie about which sources built the artifact.',
    generatedAt: new Date().toISOString(),
    // Recorded for the human reading a failure, and compared only when emcc is
    // installed. Never hashed — the source hash must be recomputable with no
    // toolchain present.
    emccVersion,
    sourceHash,
    fileCount: Object.keys(files).length,
    files,
  };
  if (artifactPath !== null && existsSync(artifactPath)) {
    const bytes = readFileSync(artifactPath);
    stamp.artifact = { bytes: bytes.length, sha256: sha256(bytes) };
  }
  const path = join(repoRoot, STAMP_RELATIVE_PATH);
  writeFileSync(path, `${JSON.stringify(stamp, null, 2)}\n`);
  return { path, stamp };
}

/** Read + validate the stamp. Returns `{ ok: false, reason }` rather than throwing. */
export function readStamp(repoRoot) {
  const path = join(repoRoot, STAMP_RELATIVE_PATH);
  if (!existsSync(path)) return { ok: false, reason: 'missing', path };
  let stamp;
  try {
    stamp = JSON.parse(readFileSync(path, 'utf8'));
  } catch (error) {
    return { ok: false, reason: 'unparseable', path, detail: error.message };
  }
  if (stamp === null || typeof stamp !== 'object') {
    return { ok: false, reason: 'unparseable', path, detail: 'not a JSON object' };
  }
  if (stamp.version !== STAMP_VERSION) {
    return {
      ok: false,
      reason: 'version',
      path,
      detail: `stamp version ${JSON.stringify(stamp.version)}, expected ${STAMP_VERSION}`,
    };
  }
  if (typeof stamp.sourceHash !== 'string' || stamp.sourceHash.length !== 64) {
    return { ok: false, reason: 'unparseable', path, detail: 'no usable sourceHash' };
  }
  return { ok: true, path, stamp };
}

// --- CLI: `node web/scripts/artifact-stamp.mjs --write [--repo-root DIR] [--emcc-version STR]`
function parseArgs(argv) {
  const args = { write: false, repoRoot: null, emccVersion: null, artifactPath: null };
  for (let i = 0; i < argv.length; i += 1) {
    if (argv[i] === '--write') args.write = true;
    else if (argv[i] === '--repo-root') args.repoRoot = argv[++i];
    else if (argv[i] === '--emcc-version') args.emccVersion = argv[++i];
    else if (argv[i] === '--artifact') args.artifactPath = argv[++i];
  }
  return args;
}

const invokedDirectly =
  process.argv[1] !== undefined && import.meta.url === `file://${process.argv[1]}`;

if (invokedDirectly) {
  const args = parseArgs(process.argv.slice(2));
  const repoRoot = args.repoRoot ?? process.cwd();
  if (!args.write) {
    const { sourceHash, files } = computeSourceHash(repoRoot);
    console.log(`${sourceHash}  (${Object.keys(files).length} inputs)`);
  } else {
    const { path, stamp } = writeStamp(repoRoot, {
      emccVersion: args.emccVersion,
      artifactPath: args.artifactPath,
    });
    console.log(
      `Wrote build stamp: ${path}\n` +
        `  sourceHash: ${stamp.sourceHash}\n` +
        `  inputs:     ${stamp.fileCount}` +
        (stamp.artifact ? `\n  artifact:   ${stamp.artifact.bytes} bytes` : '')
    );
  }
}
