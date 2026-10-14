const { BrowserWindow, screen } = require('electron');
const fs = require('node:fs/promises');
const path = require('node:path');
const { LoadingModel } = require('./loading-model.cjs');
const { createLoadingHistory } = require('./loading-history.cjs');

async function readStatus(file) {
  try {
    const stat = await fs.stat(file);
    if (stat.size > 4096) return null;
    return JSON.parse(await fs.readFile(file, 'utf8'));
  } catch { return null; }
}
async function createLoadingOverlay(profile, metadata = {}) {
  const root = path.join(profile, 'loading-status');
  await fs.mkdir(root, { recursive: true });
  const directory = await fs.mkdtemp(path.join(root, 'session-'));
  const statusPath = path.join(directory, 'status');
  const model = new LoadingModel();
  const history = await createLoadingHistory(profile, metadata);
  let overlay, stopped = false, pending, stopping, work, window, lastSaved = 0, historyError = false;
  async function collect() {
    const [nextWork, nextWindow] = await Promise.all([readStatus(statusPath + '.work'), readStatus(statusPath + '.window')]);
    if (nextWork) work = nextWork;
    if (nextWindow) window = nextWindow;
    history.update(work, window);
  }
  async function update() {
    try {
      await collect();
      if (Date.now() - lastSaved >= 2000) {
        try { await history.save(); historyError = false; }
        catch { historyError = true; }
        lastSaved = Date.now();
      }
      const state = model.update(work, window);
      if (stopped) return;
      if (!state.visible) { overlay?.hide(); return; }
      if (!overlay) {
        overlay = new BrowserWindow({ width: 460, height: 248, frame: false, transparent: true, resizable: false, show: false, focusable: false, skipTaskbar: true, hasShadow: false, alwaysOnTop: true, title: 'AnyPS5 loading status', webPreferences: { preload: path.join(__dirname, 'loading-preload.cjs'), contextIsolation: true, sandbox: true, nodeIntegration: false, backgroundThrottling: false } });
        overlay.setIgnoreMouseEvents(true);
        overlay.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
        overlay.webContents.on('will-navigate', event => event.preventDefault());
        await overlay.loadFile(path.join(__dirname, 'ui', 'loading.html'));
        if (stopped) return;
      }
      const bounds = process.platform === 'win32' ? screen.screenToDipRect(null, state.bounds) : state.bounds;
      const width = Math.min(460, bounds.width - 24);
      const height = Math.min(248, bounds.height - 16);
      if (width < 280 || height < 220) { overlay.hide(); return; }
      overlay.setBounds({ x: Math.round(bounds.x + (bounds.width - width) / 2), y: Math.round(bounds.y + bounds.height - height - 16), width, height });
      overlay.webContents.send('loading:state', { ...state, previous: history.previous, historyError });
      if (!overlay.isVisible()) overlay.showInactive();
    } catch { if (overlay && !overlay.isDestroyed()) overlay.hide(); }
  }
  function tick() {
    if (stopped || pending) return;
    pending = update().finally(() => { pending = null; });
  }
  const timer = setInterval(tick, 250);
  timer.unref();
  return {
    statusPath,
    historyPath: history.sessionPath,
    stop(removeFiles = true, reason = removeFiles ? 'exited' : 'monitor-closed') {
      if (stopping) return stopping;
      stopped = true;
      clearInterval(timer);
      stopping = (async () => {
        await pending;
        await collect();
        try { await history.stop(reason); }
        finally {
          if (overlay && !overlay.isDestroyed()) overlay.destroy();
          if (removeFiles) await fs.rm(directory, { recursive: true, force: true });
        }
      })();
      return stopping;
    }
  };
}
module.exports = { createLoadingOverlay };
