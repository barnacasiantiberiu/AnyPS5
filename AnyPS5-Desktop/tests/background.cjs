const { _electron: electron } = require('playwright');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');
const { createHash } = require('node:crypto');
const root = path.resolve(__dirname, '..');

(async () => {
  const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-background-ui-'));
  let desktop;
  try {
    const profile = path.join(temporary, 'profile');
    const game = path.join(temporary, 'game');
    await fs.mkdir(profile); await fs.mkdir(game);
    await fs.writeFile(path.join(game, 'app.exe'), 'Background test fixture; never launched.');
    await fs.writeFile(path.join(profile, 'library.json'), JSON.stringify({ settings: {}, games: [
      { id: 'ambient-fixture', title: 'Ambient test', directory: game, executable: 'app.exe', installedAt: '2026-10-12T12:00:00Z' }
    ] }));
    desktop = await electron.launch({ executablePath: process.env.ANYPS5_TEST_APP || require('electron'), args: process.env.ANYPS5_TEST_APP ? [] : [root], env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: profile } });
    const page = await desktop.firstWindow(); page.setDefaultTimeout(10000);
    const errors = []; page.on('pageerror', error => errors.push(error.message));
    // Exercise normal production scheduling and OS media preference, with no throttling overrides.
    await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].show());
    await page.waitForFunction(() => document.getElementById('ambient-canvas').dataset.ready === 'true');
    assert.equal(await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].isFullScreen()), false);
    assert.equal(await page.locator('body').evaluate(element => element.classList.contains('fullscreen')), false);
    const noOuterScroll = async () => {
      assert.ok(await page.evaluate(() => document.documentElement.scrollHeight <= innerHeight && document.documentElement.scrollWidth <= innerWidth && document.body.scrollHeight <= innerHeight), 'The document must fit the window without a right scrollbar.');
    };
    await noOuterScroll();
    assert.ok(await page.locator('.titlebar').isVisible());
    console.log('PASS: native windowed startup with title bar and no document scrollbar.');
    const canvas = page.locator('#ambient-canvas');
    await page.waitForFunction(() => document.getElementById('ambient-canvas').dataset.ready === 'true');
    const snapshot = async () => createHash('sha256').update(await canvas.evaluate(element => element.toDataURL())).digest('hex');
    const presentedFrame = () => desktop.evaluate(async ({ BrowserWindow }) => {
      const window = BrowserWindow.getAllWindows()[0];
      const [width, height] = window.getContentSize();
      // Crop away text, clock and controls; compare pixels actually composited for the window.
      const frame = await window.webContents.capturePage({ x: Math.round(width * .70), y: Math.round(height * .32), width: Math.round(width * .24), height: Math.round(height * .48) });
      return Array.from(frame.toBitmap());
    });
    const visibleFirst = await presentedFrame();
    const first = await snapshot();
    await page.waitForTimeout(1200);
    assert.notEqual(await snapshot(), first, 'Ambient pixels must move while visible.');
    const visibleNext = await presentedFrame();
    let changedPixels = 0;
    for (let n = 0; n < visibleFirst.length; n += 4) {
      if (Math.max(Math.abs(visibleFirst[n] - visibleNext[n]), Math.abs(visibleFirst[n+1] - visibleNext[n+1]), Math.abs(visibleFirst[n+2] - visibleNext[n+2])) >= 4) changedPixels++;
    }
    assert.ok(changedPixels > 100, 'Visible background crop must change, not only the canvas buffer.');
    console.log('PASS: ' + changedPixels + ' presented background pixels moved over 1.2 seconds.');
    assert.equal(await canvas.getAttribute('data-running'), 'true');
    const pixels = await canvas.evaluate(element => {
      const ctx = element.getContext('2d');
      const a = [...ctx.getImageData(10, 10, 1, 1).data];
      const b = [...ctx.getImageData(element.width - 10, element.height - 10, 1, 1).data];
      return { a, b, tabIndex: element.tabIndex, pointerEvents: getComputedStyle(element).pointerEvents };
    });
    assert.notDeepEqual(pixels.a, pixels.b, 'Background must have tonal depth.');
    assert.equal(pixels.tabIndex, -1); assert.equal(pixels.pointerEvents, 'none');
    await fs.mkdir(path.join(root, 'artifacts'), { recursive: true });
    await page.waitForTimeout(1400);
    await page.screenshot({ path: path.join(root, 'artifacts', '12-animated-background.png') });
    console.log('PASS: canvas renders moving pixels with tonal depth and does not capture focus/pointer input.');

    await page.emulateMedia({ reducedMotion: 'reduce' });
    await page.waitForFunction(() => document.getElementById('ambient-canvas').dataset.motion === 'reduced', null, { polling: 50 });
    const still = await snapshot(); await page.waitForTimeout(180);
    assert.notEqual(await snapshot(), still, 'Reduced motion must keep the requested background gently animated.');
    assert.equal(await canvas.getAttribute('data-running'), 'true');
    await page.emulateMedia({ reducedMotion: 'no-preference' });
    await page.waitForFunction(() => document.getElementById('ambient-canvas').dataset.running === 'true', null, { polling: 50 });
    await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].minimize());
    await page.waitForFunction(() => document.getElementById('ambient-canvas').dataset.running === 'false', null, { polling: 50 });
    assert.equal(await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].isMinimized()), true);
    const hidden = await snapshot(); await page.waitForTimeout(180);
    assert.equal(await snapshot(), hidden, 'Minimized windows must pause.');
    await desktop.evaluate(({ BrowserWindow }) => { const window = BrowserWindow.getAllWindows()[0]; window.restore(); window.show(); });
    await page.waitForFunction(() => document.getElementById('ambient-canvas').dataset.running === 'true', null, { polling: 50 });
    await page.waitForTimeout(180);
    assert.notEqual(await snapshot(), hidden, 'Restoring the window must resume animation.');
    console.log('PASS: reduced-motion gentle animation and native minimize/restore pause/resume.');

    await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].setSize(1040, 720));
    await page.waitForFunction(() => innerWidth <= 1040);
    await noOuterScroll();
    for (const selector of ['#empty-convert', '#empty-import', '#footer-import']) {
      assert.ok(await page.locator(selector).evaluate(element => { const r=element.getBoundingClientRect(); return r.top >= 0 && r.bottom <= innerHeight && r.right <= innerWidth; }), selector + ' must fit the compact window.');
    }
    await page.screenshot({ path: path.join(root, 'artifacts', '13-windowed-no-scrollbar.png') });
    await page.keyboard.press('F11');
    await page.waitForFunction(() => document.body.classList.contains('fullscreen'));
    await noOuterScroll();
    await page.keyboard.press('F11');
    await page.waitForFunction(() => !document.body.classList.contains('fullscreen'));
    console.log('PASS: F11 switches fullscreen/windowed and compact controls remain visible without outer scrollbars.');

    for (const size of [{ width: 1040, height: 720 }, { width: 2560, height: 1440 }]) {
      await page.setViewportSize(size);
      await page.waitForFunction(() => {
        const c = document.getElementById('ambient-canvas'), r = c.getBoundingClientRect();
        const scale = Math.min(devicePixelRatio || 1, 1.5, 1920 / r.width, 1080 / r.height);
        return c.width === Math.round(r.width * scale) && c.height === Math.round(r.height * scale);
      });
      const dimensions = await canvas.evaluate(element => ({ width: element.width, height: element.height }));
      assert.ok(dimensions.width <= 1920 && dimensions.height <= 1080);
    }
    await page.setViewportSize({ width: 1400, height: 900 });
    const artworkFolder = path.join(game, 'app0', 'sce_sys');
    await fs.mkdir(artworkFolder, { recursive: true });
    await fs.writeFile(path.join(artworkFolder, 'pic1.png'), Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jcL8AAAAASUVORK5CYII=', 'base64'));
    await page.locator('[data-game-id="ambient-fixture"]').click();
    await page.waitForFunction(() => document.body.classList.contains('has-game-artwork'));
    assert.match(await page.locator('#backdrop').evaluate(element => element.style.backgroundImage), /data:image/);
    await page.locator('#nav-convert').click();
    await page.waitForFunction(() => !document.body.classList.contains('has-game-artwork'));
    assert.equal(await page.locator('#backdrop').evaluate(element => element.style.backgroundImage), '');
    assert.deepEqual(errors, []);
    await page.evaluate(() => window.dispatchEvent(new Event('pagehide')));
    assert.equal(await canvas.getAttribute('data-running'), 'false');
    const disposed = await snapshot(); await page.waitForTimeout(100);
    assert.equal(await snapshot(), disposed);
    console.log('PASS: compact/large resizing, backing-resolution cap, artwork transitions and disposal; no renderer errors.');
  } finally {
    if (desktop) await desktop.close();
    if (path.dirname(temporary) === os.tmpdir() && path.basename(temporary).startsWith('anyps5-background-ui-')) await fs.rm(temporary, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
