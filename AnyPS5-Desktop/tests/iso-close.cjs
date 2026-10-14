const { _electron: electron } = require('playwright');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');

// Keep real renderer/preload/main handlers. Only Windows mounting and native
// dialog responses are replaced; no ISO is mounted and no real source is used.
(async () => {
  const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-iso-close-'));
  let desktop;
  try {
    const profile = path.join(temporary, 'profile');
    const image = path.join(temporary, 'fixture.iso');
    const volume = path.join(temporary, 'volume');
    const journal = path.join(temporary, 'lifecycle.jsonl');
    await fs.mkdir(profile); await fs.mkdir(volume);
    await fs.writeFile(image, 'Synthetic fixture. This file must never be mounted.');
    await fs.writeFile(path.join(profile, 'library.json'), JSON.stringify({ settings: {}, games: [] }));
    desktop = await electron.launch({ executablePath: process.env.ANYPS5_TEST_APP || require('electron'), args: process.env.ANYPS5_TEST_APP ? [] : [root], env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: profile } });
    const page = await desktop.firstWindow(); page.setDefaultTimeout(10000);
    await page.locator('#library-empty h1').waitFor();
    const errors = []; page.on('pageerror', error => errors.push(error.message));
    await desktop.evaluate(({ app, BrowserWindow, dialog }, fixture) => {
      const loadModule = process.getBuiltinModule('node:module').createRequire(app.getAppPath() + '/src/main.cjs');
      const isoModule = loadModule(loadModule('node:path').join(app.getAppPath(), 'src', 'iso-source.cjs'));
      const record = event => loadModule('node:fs').appendFileSync(fixture.journal, JSON.stringify(event) + '\n');
      const test = globalThis.isoCloseTest = { phase: 'ready', attempts: 0, confirmations: 0 };
      isoModule.windowsAdapter.mount = image => {
        test.attempts++; const attempt = test.attempts; test.phase = 'mount'; record({ event: 'mount-pending', attempt, image });
        return new Promise(resolve => { test.resolveMount = () => { record({ event: 'mount-resolved', attempt }); resolve({ root: fixture.volume, owned: true, devicePath: 'test-device-' + attempt }); }; });
      };
      isoModule.windowsAdapter.unmount = (_image, mount) => {
        if (test.forceRelease) return Promise.resolve();
        const attempt = test.attempts; test.phase = 'unmount'; record({ event: 'unmount-pending', attempt, device: mount.devicePath });
        return new Promise(resolve => { test.resolveUnmount = () => { test.phase = 'ready'; record({ event: 'unmount-resolved', attempt }); resolve(); }; });
      };
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [fixture.image] });
      dialog.showMessageBox = async (_window, options) => { test.confirmations++; record({ event: 'confirm-close', message: options.message }); return { response: 1 }; };
      BrowserWindow.getAllWindows()[0].on('closed', () => record({ event: 'window-closed' }));
    }, { image, volume, journal });
    const waitPhase = async expected => {
      for (let attempt = 0; attempt < 200; attempt++) {
        if (await desktop.evaluate(() => globalThis.isoCloseTest.phase) === expected) return;
        await page.waitForTimeout(15);
      }
      assert.fail('ISO lifecycle did not reach ' + expected);
    };
    const assertAlive = async stage => {
      assert.equal(await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows().length), 1, 'Window must remain alive while ' + stage + ' is pending.');
      assert.ok(await page.locator('#pick-iso').isDisabled());
      assert.ok(await page.locator('#pick-source').isDisabled());
    };
    await page.locator('#nav-convert').click(); await page.locator('#pick-iso').click();
    await waitPhase('mount'); await assertAlive('mount');
    await page.evaluate(() => window.desktop.cancel());
    await desktop.evaluate(() => globalThis.isoCloseTest.resolveMount());
    await waitPhase('unmount'); await assertAlive('cancel cleanup');
    await desktop.evaluate(() => globalThis.isoCloseTest.resolveUnmount());
    await page.waitForFunction(() => !document.getElementById('pick-iso').disabled && !document.getElementById('pick-source').disabled);
    assert.match(await page.locator('#wizard-error').textContent(), /cancelled/i);
    assert.ok(await page.locator('#to-step-2').isDisabled());
    assert.equal(await desktop.evaluate(() => globalThis.isoCloseTest.confirmations), 0);
    console.log('PASS: real cancellation IPC waits for an in-flight mount and its cleanup; both source pickers recover afterward.');

    await page.locator('#pick-iso').click(); await waitPhase('mount');
    await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].close());
    assert.equal(await desktop.evaluate(() => globalThis.isoCloseTest.confirmations), 1);
    await assertAlive('mount after Stop and close');
    await desktop.evaluate(() => globalThis.isoCloseTest.resolveMount());
    await waitPhase('unmount'); await assertAlive('unmount after Stop and close');
    const closed = desktop.waitForEvent('close');
    await desktop.evaluate(() => { setTimeout(() => globalThis.isoCloseTest.resolveUnmount(), 25); });
    await closed; desktop = null;
    const events = (await fs.readFile(journal, 'utf8')).trim().split('\n').map(line => JSON.parse(line));
    assert.deepEqual(events.map(item => item.event), [
      'mount-pending', 'mount-resolved', 'unmount-pending', 'unmount-resolved',
      'mount-pending', 'confirm-close', 'mount-resolved', 'unmount-pending', 'unmount-resolved', 'window-closed'
    ]);
    assert.deepEqual(events.filter(item => item.event === 'mount-pending').map(item => item.image), [image, image]);
    assert.deepEqual(errors, []);
    console.log('PASS: native window close confirms stopping, preserves the window throughout mount/unmount, and exits only after cleanup finishes. No renderer errors.');
  } finally {
    if (desktop) {
      // Ensure a failed assertion cannot strand this test's deferred operation.
      await desktop.evaluate(() => { if (globalThis.isoCloseTest) globalThis.isoCloseTest.forceRelease = true; globalThis.isoCloseTest?.resolveMount?.(); globalThis.isoCloseTest?.resolveUnmount?.(); }).catch(() => {});
      await desktop.close();
    }
    if (path.dirname(temporary) === os.tmpdir() && path.basename(temporary).startsWith('anyps5-iso-close-')) await fs.rm(temporary, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
