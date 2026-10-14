const { _electron: electron } = require('playwright');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');

(async () => {
  const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-keyboard-ui-'));
  let desktop; let page;
  try {
    const profile = path.join(temporary, 'profile');
    const game = path.join(temporary, 'game');
    const source = path.join(temporary, 'source');
    await fs.mkdir(profile); await fs.mkdir(game); await fs.mkdir(path.join(source, 'sce_module'), { recursive: true });
    await fs.writeFile(path.join(game, 'app.exe'), 'UI fixture; launch IPC is simulated');
    await fs.writeFile(path.join(source, 'eboot.bin'), Buffer.from([127, 69, 76, 70, 2, 1, 1, 0]));
    await fs.writeFile(path.join(profile, 'library.json'), JSON.stringify({ settings: {}, games: [
      { id: 'alpha', title: 'Alpha', directory: game, executable: 'app.exe', installedAt: '2026-10-12T12:00:00Z' },
      { id: 'missing', title: 'Missing game', directory: game, executable: 'missing.exe', installedAt: '2026-10-12T12:00:00Z' }
    ] }));
    desktop = await electron.launch({ executablePath: process.env.ANYPS5_TEST_APP || require('electron'), args: process.env.ANYPS5_TEST_APP ? [] : [root], env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: profile } });
    page = await desktop.firstWindow(); page.setDefaultTimeout(10000);
    const errors = []; page.on('pageerror', error => errors.push(error.message));
    await desktop.evaluate(({ ipcMain, dialog }, sourceFolder) => {
      globalThis.keyboardTest = { launches: [], cancellations: 0 };
      ipcMain.removeHandler('game:launch');
      ipcMain.handle('game:launch', (_event, id) => { globalThis.keyboardTest.launches.push(id); return { ok: true }; });
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [sourceFolder] });
      ipcMain.removeHandler('installation:preflight');
      ipcMain.handle('installation:preflight', () => ({ ok: true, value: { totalBytes: 0, freeBytes: 1024 ** 3, resources: 0, extraModules: 0 } }));
      ipcMain.removeHandler('installation:start');
      ipcMain.handle('installation:start', () => new Promise(resolve => { globalThis.keyboardTest.stop = () => resolve({ ok: false, error: 'Conversion cancelled by the keyboard test.' }); }));
      ipcMain.removeHandler('installation:cancel');
      ipcMain.handle('installation:cancel', () => { globalThis.keyboardTest.cancellations++; globalThis.keyboardTest.stop?.(); return { ok: true }; });
    }, source);
    const focus = () => page.evaluate(() => document.activeElement.id || document.activeElement.dataset.navKey || document.activeElement.tagName);
    const key = name => page.keyboard.press(name);
    const focused = async expected => {
      await page.waitForFunction(value => (document.activeElement.id || document.activeElement.dataset.navKey || document.activeElement.tagName) === value, expected);
    };
    const toward = async (target, direction, maximum = 8) => {
      const route = [await focus()];
      for (let n = 0; n < maximum && await focus() !== target; n++) { await key(direction); route.push(await focus()); }
      assert.equal(await focus(), target, 'Keyboard route with ' + direction + ': ' + route.join(' -> '));
    };
    await focused('library');
    await key('ArrowRight'); await focused('game:alpha');
    assert.equal(await page.locator('#game-title').textContent(), 'Alpha');
    await key('Enter');
    await page.waitForFunction(() => document.getElementById('toast').textContent.includes('started'));
    assert.deepEqual(await desktop.evaluate(() => globalThis.keyboardTest.launches), ['alpha']);
    await focused('game:alpha');
    await key('ArrowRight'); await focused('game:missing'); await key('Enter');
    assert.deepEqual(await desktop.evaluate(() => globalThis.keyboardTest.launches), ['alpha']);
    await key('Escape'); await focused('library');
    console.log('PASS: shelf preview, persistent focus, Enter activation and unavailable game protection.');
    await key('ArrowRight'); await focused('game:alpha');
    await key('ArrowDown'); await focused('game-launch');
    await key('ArrowRight'); await focused('game-menu-toggle');
    await key('Enter'); await focused('game-folder');
    await key('ArrowDown'); await focused('game-shortcut');
    await key('Escape'); await focused('game-menu-toggle');
    await key('Enter'); await focused('game-folder');
    // Mixed-input regression: pointer navigation must close the old keyboard menu scope.
    await page.locator('#nav-convert').click(); await focused('pick-source');
    assert.ok(await page.locator('#game-menu').isHidden());
    await key('ArrowUp'); assert.notEqual(await focus(), 'pick-source');
    await key('Escape'); await focused('game:alpha');
    await key('Escape'); await focused('library');
    console.log('PASS: menu arrow/Esc control and mixed mouse/keyboard view transition.');


    // Enter Settings using only arrows and Enter from the shelf.
    await key('ArrowUp'); await focused('nav-library');
    await toward('settings-open', 'ArrowRight'); await key('Enter');
    await focused('settings-tab-players');
    await key('ArrowDown'); await focused('player-1-source');
    await key('ArrowDown'); await focused('player-2-source');
    assert.equal(await page.locator('#player-2-source').inputValue(), 'disabled');
    await key('Enter'); await key('ArrowDown');
    assert.equal(await page.locator('#player-2-source').inputValue(), 'dualsense-1');
    await key('Escape'); await focused('player-2-source');
    assert.equal(await page.locator('#player-2-source').inputValue(), 'disabled');
    assert.ok(await page.locator('#settings-dialog').isVisible());
    await key('Enter'); await key('ArrowDown'); await key('Enter');
    assert.ok(await page.locator('#settings-dialog').isVisible(), 'Confirming a choice must not submit settings');
    await key('Escape'); await focused('settings-open');
    assert.equal((await page.evaluate(() => window.desktop.state())).settings.players[1].type, 'disabled');
    await key('Enter'); await focused('settings-tab-players');
    await key('ArrowDown'); await key('ArrowDown'); await focused('player-2-source');
    await key('Enter'); await key('ArrowDown'); await key('Enter');
    // Continue down through remaining players to the save action.
    await toward('player-4-source', 'ArrowDown'); await key('ArrowDown');
    assert.equal(await page.evaluate(() => document.activeElement.type), 'submit');
    await key('Enter'); await page.locator('#settings-dialog').waitFor({ state: 'hidden' });
    assert.deepEqual((await page.evaluate(() => window.desktop.state())).settings.players[1], { type: 'dualsense', controllerSlot: 1 });
    console.log('PASS: player choices, edit cancellation, modal cancellation and explicit save using keyboard only.');

    await focused('settings-open'); await key('Enter'); await focused('settings-tab-players');
    await key('ArrowRight'); await focused('settings-tab-engine');
    await key('ArrowDown'); await focused('setting-relinker');
    const originalRelinker = (await page.evaluate(() => window.desktop.state())).settings.relinker;
    await key('End'); await key('Backspace'); await key('ArrowLeft'); await focused('setting-relinker');
    assert.equal(await page.locator('#setting-relinker').getAttribute('data-keyboard-editing'), 'true');
    await key('Escape');

    await key('Enter'); await key('Control+A'); await page.keyboard.type('C:\\temporary\\relinker.exe');
    await key('ArrowLeft'); await focused('setting-relinker');
    assert.equal(await page.locator('#setting-relinker').evaluate(element => element.selectionStart), 'C:\\temporary\\relinker.exe'.length - 1);
    await key('Escape'); await focused('setting-relinker');
    assert.ok(await page.locator('#settings-dialog').isVisible());
    await key('Escape'); await focused('settings-open');
    assert.equal((await page.evaluate(() => window.desktop.state())).settings.relinker, originalRelinker);
    console.log('PASS: Settings tabs and text caret navigation; Esc exits editing before closing without saving.');

    // Search preserves text editing, then Esc returns control to the page.
    await key('ArrowLeft'); await focused('search-toggle'); await key('Enter'); await focused('search-input');
    await page.keyboard.type('Alpha'); await key('ArrowLeft'); await focused('search-input');
    assert.equal(await page.locator('[data-game-id]').count(), 1);
    for (let n = 0; n < 3 && await page.locator('#search-box').isVisible(); n++) await key('Escape');
    await focused('search-toggle');
    assert.ok(await page.locator('#search-box').isHidden());
    await key('ArrowLeft'); await focused('nav-convert'); await key('Enter'); await focused('pick-source');
    await key('Escape'); await page.locator('#library-view').waitFor({ state: 'visible' });
    // Return through the header and open a source via a simulated file dialog.
    await toward('nav-library', 'ArrowUp'); await key('ArrowRight'); await focused('nav-convert');
    await key('Enter'); await focused('pick-source'); await key('Enter');
    await page.locator('#source-details').waitFor({ state: 'visible' });
    await page.waitForFunction(() => !document.getElementById('pick-source').disabled && !document.getElementById('to-step-2').disabled);
    await toward('to-step-2', 'ArrowDown'); await key('Enter');
    await page.locator('#step-2').waitFor({ state: 'visible' });
    await key('Escape'); await page.locator('#step-1').waitFor({ state: 'visible' }); await focused('pick-source');
    await toward('to-step-2', 'ArrowDown'); await key('Enter');
    await focused('pick-destination');
    await toward('begin-install', 'ArrowDown', 10); await key('Enter');
    await page.locator('#step-3').waitFor({ state: 'visible' });
    await key('Escape'); await focused('cancel-install');
    assert.equal(await desktop.evaluate(() => globalThis.keyboardTest.cancellations), 0);
    assert.ok(await page.locator('#step-3').isVisible());
    await key('Enter'); await page.locator('#step-2').waitFor({ state: 'visible' });
    assert.equal(await desktop.evaluate(() => globalThis.keyboardTest.cancellations), 1);
    console.log('PASS: search, wizard Back navigation and explicit cancellation; Esc does not stop conversion.');

    await key('Escape'); await key('Escape');
    await page.setViewportSize({ width: 1040, height: 720 });
    await toward('nav-library', 'ArrowUp'); await toward('settings-open', 'ArrowRight'); await key('Enter');
    await key('ArrowDown'); await toward('player-4-source', 'ArrowDown');
    assert.ok(await page.locator('#player-4-source').evaluate(element => { const r = element.getBoundingClientRect(); return r.top >= 0 && r.bottom <= window.innerHeight; }));
    await fs.mkdir(path.join(root, 'artifacts'), { recursive: true });
    await page.screenshot({ path: path.join(root, 'artifacts', '11-keyboard-settings.png') });
    await key('Escape'); await focused('settings-open');
    await key('Escape'); await focused('exit-cancel');
    assert.ok(await page.locator('#exit-dialog').isVisible());
    await key('ArrowRight'); await focused('exit-confirm');
    await key('Escape'); await focused('settings-open');
    assert.ok(await page.locator('#exit-dialog').isHidden());
    await key('Escape'); await focused('exit-cancel'); await key('Enter');
    await focused('settings-open');
    assert.ok(await page.locator('#exit-dialog').isHidden());
    await key('Escape'); await focused('exit-cancel'); await key('ArrowRight'); await focused('exit-confirm');
    await page.screenshot({ path: path.join(root, 'artifacts', '14-exit-menu.png') });
    const appClosed = desktop.waitForEvent('close');
    await key('Enter'); await appClosed; desktop = null;
    console.log('PASS: Esc opens Exit from the main menu; arrows select Stay/Exit; Esc and Stay cancel; Enter on Exit closes the app.');
    assert.deepEqual(errors, []);
    console.log('PASS: compact-window focus remains visible. Main flows used arrows, Enter, Esc and typing; a mixed-input regression was also checked. Launch/conversion IPC was simulated.');
  } catch (error) {
    if (page && !page.isClosed()) { await page.screenshot({ path: path.join(root, 'artifacts', 'keyboard-failure.png') }).catch(() => {}); }
    throw error;
  } finally {
    if (desktop) await desktop.close();
    if (path.dirname(temporary) === os.tmpdir() && path.basename(temporary).startsWith('anyps5-keyboard-ui-')) await fs.rm(temporary, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
