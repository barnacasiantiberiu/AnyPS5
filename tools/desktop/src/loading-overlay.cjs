const { BrowserWindow, screen } = require('electron');
const fs = require('node:fs/promises');
const path = require('node:path');
const { LoadingModel } = require('./loading-model.cjs');

async function readStatus(file) {
  try {
    const stat = await fs.stat(file);
    if (stat.size > 4096) return null;
    return JSON.parse(await fs.readFile(file, 'utf8'));
  } catch { return null; }
}
async function createLoadingOverlay(profile) {
  const root = path.join(profile, 'loading-status');
  await fs.mkdir(root, { recursive: true });
  const directory = await fs.mkdtemp(path.join(root, 'session-'));
  const statusPath = path.join(directory, 'status');
  const model = new LoadingModel();
  let overlay, stopped = false, updating = false, work, window;
  async function tick() {
    if (stopped || updating) return;
    updating = true;
    try {
      const [nextWork, nextWindow] = await Promise.all([readStatus(statusPath + '.work'), readStatus(statusPath + '.window')]);
      if (nextWork) work = nextWork;
      if (nextWindow) window = nextWindow;
      const state = model.update(work, window);
      if (stopped) return;
      if (!state.visible) { overlay?.hide(); return; }
      if (!overlay) {
        overlay = new BrowserWindow({ width: 460, height: 208, frame: false, transparent: true, resizable: false, show: false, focusable: false, skipTaskbar: true, hasShadow: false, alwaysOnTop: true, title: 'AnyPS5 loading status', webPreferences: { preload: path.join(__dirname, 'loading-preload.cjs'), contextIsolation: true, sandbox: true, nodeIntegration: false, backgroundThrottling: false } });
        overlay.setIgnoreMouseEvents(true);
        overlay.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
        overlay.webContents.on('will-navigate', event => event.preventDefault());
        await overlay.loadFile(path.join(__dirname, 'ui', 'loading.html'));
        if (stopped) return;
      }
      const bounds = process.platform === 'win32' ? screen.screenToDipRect(null, state.bounds) : state.bounds;
      const width = Math.min(460, bounds.width - 24);
      const height = Math.min(208, bounds.height - 16);
      if (width < 280 || height < 180) { overlay.hide(); return; }
      overlay.setBounds({ x: Math.round(bounds.x + (bounds.width - width) / 2), y: Math.round(bounds.y + bounds.height - height - 16), width, height });
      overlay.webContents.send('loading:state', state);
      if (!overlay.isVisible()) overlay.showInactive();
    } catch { if (overlay && !overlay.isDestroyed()) overlay.hide(); }
    finally { updating = false; }
  }
  const timer = setInterval(tick, 250);
  timer.unref();
  return {
    statusPath,
    async stop(removeFiles = true) {
      stopped = true;
      clearInterval(timer);
      if (overlay && !overlay.isDestroyed()) overlay.destroy();
      if (removeFiles) await fs.rm(directory, { recursive: true, force: true });
    }
  };
}
module.exports = { createLoadingOverlay };
