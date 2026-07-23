// Preload for the key-prompt window. Exposes a single, narrow IPC bridge so the
// prompt page (no node integration, context-isolated) can hand the pasted key
// back to the main process.
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('clipperKeyPrompt', {
  submit: (key) => ipcRenderer.invoke('clipper:submit-key', key),
});
