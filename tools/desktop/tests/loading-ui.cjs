const { _electron: electron } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
(async () => {
  const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-loading-ui-'));
  let desktop;
  try {
    desktop = await electron.launch({ executablePath: process.env.ANYPS5_TEST_APP || require('electron'), args: process.env.ANYPS5_TEST_APP ? [] : [path.resolve(__dirname, '..')], env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: temporary } });
    await desktop.firstWindow();
    const statusPath = await desktop.evaluate(async ({ app }) => {
      const { createLoadingOverlay } = process.mainModule.require(app.getAppPath() + '/src/loading-overlay.cjs');
      globalThis.testLoading = await createLoadingOverlay(app.getPath('userData'));
      return globalThis.testLoading.statusPath;
    });
    let frames = 10, focused = true, completed = 7, elapsedMs = 5000, lastCompletedMs = 4900;
    const write = async () => {
      const updatedAt = Date.now();
      await fs.writeFile(statusPath + '.work', JSON.stringify({ version: 1, updatedAt, elapsedMs, lastCompletedMs, completed, failed: 0, active: [1, 0, 0, 0] }));
      await fs.writeFile(statusPath + '.window', JSON.stringify({ version: 1, updatedAt, frames, focused, x: 200, y: 100, width: 1000, height: 700 }));
    };
    await write();
    const timer = setInterval(() => write().catch(() => {}), 350);
    try {
      const overlay = await desktop.waitForEvent('window', { timeout: 8000 });
      await overlay.locator('#title').waitFor();
      await overlay.waitForFunction(() => document.getElementById('title').textContent === 'Compiling shaders');
      assert.equal(await overlay.locator('#completed').textContent(), '7');
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
      focused = true; frames = 11;
      await new Promise(resolve => setTimeout(resolve, 700));
      assert.equal(await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows().find(w => w.getTitle() === 'AnyPS5 loading status').isVisible()), false);
      await desktop.evaluate(async () => globalThis.testLoading.stop());
      assert.equal(await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows().some(w => w.getTitle() === 'AnyPS5 loading status')), false);
      console.log('PASS: engine-format telemetry -> overlay, counts, no fake percent, stalled work, focus, frame recovery and cleanup.');
    } finally { clearInterval(timer); }
  } finally {
    if (desktop) await desktop.close();
    await fs.rm(temporary, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
