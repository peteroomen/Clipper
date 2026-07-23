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

// Serve the production build (which includes public/generated/*) and run the
// offline-audio test against it. Headless Chromium only.
export default defineConfig({
  testDir: './tests',
  timeout: 60_000,
  fullyParallel: false,
  workers: 1,
  reporter: [['list']],
  use: {
    baseURL: 'http://localhost:4173',
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
    command: 'npm run build && npm run preview -- --port 4173 --strictPort',
    url: 'http://localhost:4173',
    reuseExistingServer: !process.env.CI,
    timeout: 120_000,
  },
});
