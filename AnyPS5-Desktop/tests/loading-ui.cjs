const { _electron: electron } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const { desktopLaunchOptions } = require('../scripts/launch.cjs');
(async () => {
  const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-loading-ui-'));
  let desktop;
  try {
    const environment = { ...process.env, ANYPS5_DESKTOP_EXE: process.env.ANYPS5_TEST_APP || '', ANYPS5_DESKTOP_TEST_DATA: temporary, ELECTRON_RUN_AS_NODE: '1', electron_run_as_node: '1' };
    const options = desktopLaunchOptions([], environment);
    assert.equal(Object.keys(options.env).some(key => key.toUpperCase() === 'ELECTRON_RUN_AS_NODE'), false);
    assert.equal(environment.ELECTRON_RUN_AS_NODE, '1');
    desktop = await electron.launch(options);
    await desktop.firstWindow();
    const statusPath = await desktop.evaluate(async ({ app }) => {
      const { createLoadingOverlay } = process.mainModule.require(app.getAppPath() + '/src/loading-overlay.cjs');
      globalThis.testLoading = await createLoadingOverlay(app.getPath('userData'));
      return globalThis.testLoading.statusPath;
    });
    let frames = 10, focused = true, completed = 7, elapsedMs = 5000, lastCompletedMs = 4900, active = [1, 0, 0, 0];
    const write = async () => {
      const updatedAt = Date.now();
      await fs.writeFile(statusPath + '.work', JSON.stringify({ version: 1, updatedAt, elapsedMs, lastCompletedMs, completed, failed: 0, active }));
      await fs.writeFile(statusPath + '.window', JSON.stringify({ version: 1, updatedAt, frames, focused, x: 200, y: 100, width: 1000, height: 700 }));
    };
    await write();
    const timer = setInterval(() => write().catch(() => {}), 350);
    try {
      const overlay = await desktop.waitForEvent('window', { timeout: 8000 });
      await overlay.locator('#title').waitFor();
      await overlay.waitForFunction(() => document.getElementById('title').textContent === 'Compiling shaders');
      assert.equal(await overlay.locator('#completed').textContent(), '7');
      assert.equal(await overlay.locator('#elapsed').textContent(), '5s');
      frames = 11;
      await new Promise(resolve => setTimeout(resolve, 700));
      assert.equal(await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows().find(w => w.getTitle() === 'AnyPS5 loading status').isVisible()), true);
      assert.equal(await overlay.locator('[role=progressbar]').getAttribute('aria-valuenow'), null);
      assert.equal(await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows().find(w => w.getTitle() === 'AnyPS5 loading status').isFocusable()), false);
      if (process.env.ANYPS5_LOADING_SCREENSHOT) await overlay.screenshot({ path: process.env.ANYPS5_LOADING_SCREENSHOT });
      elapsedMs = 24000;
      await overlay.waitForFunction(() => document.body.classList.contains('warning'));
      assert.match(await overlay.locator('#detail').textContent(), /No compilation completed/);
      assert.equal(await overlay.locator('body').evaluate(body => body.classList.contains('active')), false);
      focused = false;
      await new Promise(resolve => setTimeout(resolve, 700));
      assert.equal(await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows().find(w => w.getTitle() === 'AnyPS5 loading status').isVisible()), false);
      focused = true; frames = 12; active = [0, 0, 0, 0]; lastCompletedMs = elapsedMs;
      await overlay.waitForFunction(() => document.getElementById('title').textContent === 'Compilation idle');
      await desktop.evaluate(async () => globalThis.testLoading.stop());
      assert.equal(await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows().some(w => w.getTitle() === 'AnyPS5 loading status')), false);
      const records = await fs.readdir(path.join(temporary, 'loading-history'));
      const record = JSON.parse(await fs.readFile(path.join(temporary, 'loading-history', records.find(name => name.endsWith('.json'))), 'utf8'));
      assert.equal(record.completed, 7);
      assert.equal(record.lastCompletedMs, 24000);
      assert.equal(record.status, 'exited');
      console.log('PASS: GUI launch from a Node-mode environment, overlay, moving-frame compilation, real timings, stalled work, focus, recovery, persistent history and cleanup.');
    } finally { clearInterval(timer); }
  } finally {
    if (desktop) await desktop.close();
    await fs.rm(temporary, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
