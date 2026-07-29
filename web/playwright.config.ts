import { defineConfig, devices } from '@playwright/test';
import { readdirSync } from 'node:fs';
import { join } from 'node:path';

// The container ships a preinstalled Chromium under PLAYWRIGHT_BROWSERS_PATH
// whose build number may not match this @playwright/test version. Discover the
// full chrome binary and point Playwright straight at it instead of running
// `playwright install` (which is disabled here).
function findChromium(): string | undefined {
  const root = process.env.PLAYWRIGHT_BROWSERS_PATH || '/opt/pw-browsers';
  try {
    const dir = readdirSync(root).find((d) => /^chromium-\d+$/.test(d));
    if (dir) return join(root, dir, 'chrome-linux', 'chrome');
  } catch {
    /* fall through */
  }
  return undefined;
}

const executablePath = findChromium();

// Preview-server port. Overridable because parallel git worktrees (agents) each
// run this suite and all want 4173; `strictPort` + `reuseExistingServer: false`
// (below) turn a collision into a hard error, which is correct but leaves no way
// to run two suites at once. `PW_PORT=4174 npx playwright test` is that way.
const PORT = Number(process.env.PW_PORT || 4173);

// Serve the production build (which includes public/generated/*) and run the
// offline-audio test against it. Headless Chromium only.
export default defineConfig({
  testDir: './tests',
  timeout: 60_000,
  fullyParallel: false,
  workers: 1,
  // A flake is a failure here. This was `retries: 2`, justified by a comment claiming
  // the intermittent all-zero OfflineAudioContext render was an unfixable WebAudio
  // engine quirk and that retries could not mask a real break. Both claims were wrong
  // when measured (docs §41):
  //
  //  - The silence had a real, fixable cause — the AudioWorkletProcessor being
  //    installed onto the offline RENDER thread races `startRendering()`. It is fixed by
  //    `installOfflineRenderBarrier` in tests/support/render-guard.ts, which every spec
  //    that renders audio now installs. Measured under a fixed 6-way CPU load on 4
  //    cores, 5 full-suite runs each at `--retries=0`: 41 failures / 355 test-executions
  //    (5/5 runs red) BEFORE, 0 / 355 (5/5 runs green) AFTER.
  //  - "A genuine failure loses all attempts" is true only of a fault that reproduces on
  //    EVERY run. Measured with a fault injected at 20 % per render into one
  //    single-render test, 25 repetitions of each arm:
  //        --retries=0  ->  5 failed / 20 passed, exit 1   (fault caught)
  //        --retries=2  ->  0 failed, 6 flaky / 19 passed, exit 0   (suite GREEN)
  //    Same fault, same rate: retries:2 masked it completely. That is the whole
  //    argument — do not put this back to 2.
  //
  // If a residual flake ever forces this above 0, write the MEASURED rate here and use
  // 1, never 2.
  retries: 0,
  reporter: [['list']],
  use: {
    baseURL: `http://localhost:${PORT}`,
  },
  projects: [
    {
      name: 'chromium',
      use: {
        ...devices['Desktop Chrome'],
        launchOptions: {
          executablePath,
          // Fake media device + auto-accepted permission so the live-input
          // (getUserMedia) path can be smoke-tested headless. Harmless to the
          // offline-render tests, which never touch getUserMedia.
          args: [
            '--no-sandbox',
            '--use-fake-device-for-media-stream',
            '--use-fake-ui-for-media-stream',
          ],
        },
      },
    },
  ],
  webServer: {
    // Build already ran via `npm test`? No — run it here so `npm test` alone works.
    command: `npm run build && npm run preview -- --port ${PORT} --strictPort`,
    url: `http://localhost:${PORT}`,
    // NEVER reuse: with parallel git worktrees (agents) all binding 4173, reuse
    // silently adopts a FOREIGN server and tests someone else's build (observed:
    // main-tree run tested a worktree's dist -> phantom serializer failures).
    // strictPort + no-reuse means a collision fails loudly instead.
    reuseExistingServer: false,
    timeout: 120_000,
  },
});
