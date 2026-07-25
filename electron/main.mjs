// Clipper desktop — Electron main process.
//
// On `ready`:
//   1. Ask macOS for microphone access (guitar input).
//   2. Resolve the Anthropic API key (env -> config.json -> in-app prompt),
//      unless MOCK=1, in which case the keyless demo runs.
//   3. Start the EXISTING proxy in-process (electron/serve.mjs, which reuses
//      server/handler.mjs) on an ephemeral localhost port, serving web/dist +
//      /api. This is behaviorally identical to `npm run server`.
//   4. Open a BrowserWindow at http://localhost:<port>/ — the app's relative
//      /api/* fetches hit the in-process server, no file:// hacks.
//
// Paths are app.isPackaged-aware: in dev, web/dist and server/ live next to the
// repo; when packaged, electron-builder copies them into the .app bundle's
// Resources dir (see the `build` config in package.json).

import { app, BrowserWindow, systemPreferences, ipcMain, dialog } from 'electron';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { existsSync } from 'node:fs';
import { startServer } from './serve.mjs';
import { resolveApiKey, saveApiKey } from './config.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const MOCK = process.env.MOCK === '1';

// Resolve the two external resource trees for both dev and packaged modes.
function resourcePaths() {
  if (app.isPackaged) {
    // extraResources in package.json copy these into Resources/.
    return {
      distDir: path.join(process.resourcesPath, 'web-dist'),
      handlerPath: path.join(process.resourcesPath, 'server', 'handler.mjs'),
    };
  }
  return {
    distDir: path.join(__dirname, '..', 'web', 'dist'),
    handlerPath: path.join(__dirname, '..', 'server', 'handler.mjs'),
  };
}

let serverHandle = null;

// Minimal modal window that asks the user to paste an Anthropic key. Resolves
// with the trimmed key, or null if the user closes/cancels.
function promptForApiKey() {
  return new Promise((resolve) => {
    const win = new BrowserWindow({
      width: 520,
      height: 340,
      resizable: false,
      title: 'Clipper — Anthropic API key',
      webPreferences: {
        preload: path.join(__dirname, 'key-prompt-preload.cjs'),
        contextIsolation: true,
        nodeIntegration: false,
      },
    });
    win.setMenuBarVisibility(false);
    win.loadFile(path.join(__dirname, 'key-prompt.html'));

    let settled = false;
    const finish = (value) => {
      if (settled) return;
      settled = true;
      ipcMain.removeHandler('clipper:submit-key');
      if (!win.isDestroyed()) win.close();
      resolve(value);
    };

    ipcMain.handle('clipper:submit-key', (_evt, key) => {
      const trimmed = typeof key === 'string' ? key.trim() : '';
      finish(trimmed || null);
    });
    win.on('closed', () => finish(null));
  });
}

function createMainWindow(url) {
  const win = new BrowserWindow({
    width: 1280,
    height: 860,
    minWidth: 900,
    minHeight: 640,
    backgroundColor: '#1b1e24',
    title: 'Clipper',
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  win.webContents.on('did-finish-load', () => {
    // Asserted on by the headless launch probe. Do not remove.
    console.log(`[clipper] window did-finish-load: ${url}`);
  });
  win.webContents.on('did-fail-load', (_e, code, desc) => {
    console.error(`[clipper] window did-fail-load: ${code} ${desc}`);
  });

  win.loadURL(url);
  return win;
}

async function bootstrap() {
  // macOS microphone permission (guitar input). No-op / throws on other OSes.
  if (process.platform === 'darwin') {
    try {
      await systemPreferences.askForMediaAccess('microphone');
    } catch (err) {
      console.error('[clipper] askForMediaAccess failed:', err?.message || err);
    }
  }

  const configPath = path.join(app.getPath('userData'), 'config.json');

  // Key resolution: env -> config.json. In MOCK mode we intentionally run keyless.
  let { key: apiKey } = resolveApiKey({ configPath });

  if (!apiKey && !MOCK) {
    apiKey = await promptForApiKey();
    if (apiKey) {
      try {
        saveApiKey({ configPath, apiKey });
      } catch (err) {
        console.error('[clipper] failed to save key:', err?.message || err);
      }
    } else {
      // User declined. Proceed keyless — the app loads and /api/chat returns a
      // clear 500 until a key is provided. (MOCK is off here.)
      console.warn('[clipper] no API key provided; chat will be unavailable.');
    }
  }

  const { distDir, handlerPath } = resourcePaths();
  if (!existsSync(distDir)) {
    dialog.showErrorBox(
      'Clipper — missing web build',
      `Could not find the built web app at:\n${distDir}\n\n` +
        'In dev, build it first: cd web && npm run build'
    );
    app.quit();
    return;
  }

  // Reuse server/handler.mjs verbatim (dynamic import so the packaged path,
  // which lives outside app.asar in Resources/, resolves correctly).
  const handler = await import(pathToFileURL(handlerPath).href);

  serverHandle = await startServer({
    handler,
    distDir,
    apiKey: apiKey || null,
    mock: MOCK,
    model: process.env.MODEL,
    maxTokens: Number(process.env.MAX_TOKENS) || undefined,
  });

  console.log(
    `[clipper] in-app server on ${serverHandle.url}  hasKey=${Boolean(apiKey)}  mock=${MOCK && !apiKey}`
  );

  createMainWindow(serverHandle.url);
}

app.whenReady().then(bootstrap).catch((err) => {
  console.error('[clipper] fatal bootstrap error:', err);
  app.quit();
});

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0 && serverHandle) {
    createMainWindow(serverHandle.url);
  }
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});

app.on('before-quit', async () => {
  if (serverHandle) {
    try {
      await serverHandle.close();
    } catch {
      /* ignore */
    }
  }
});
