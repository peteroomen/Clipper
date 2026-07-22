// Fail the build/test with a clear message if the WASM artifact is missing.
// The artifact is produced by scripts/build-wasm.sh at the repo root.
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const artifact = resolve(here, '../public/generated/clipper.js');
const worklet = resolve(here, '../public/generated/clipper-processor.js');

for (const [label, path] of [
  ['WASM module', artifact],
  ['worklet', worklet],
]) {
  if (!existsSync(path)) {
    console.error(
      `\n[clipper] Missing ${label}: ${path}\n` +
        '          Build it first:  bash scripts/build-wasm.sh  (from repo root)\n' +
        '          (run scripts/setup-emsdk.sh once beforehand if emcc is absent)\n'
    );
    process.exit(1);
  }
}
console.log('[clipper] WASM artifact + worklet present.');
