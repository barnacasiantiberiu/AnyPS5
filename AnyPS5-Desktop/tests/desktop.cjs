const { _electron: electron } = require('playwright');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
(async () => {
  const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-desktop-ui-'));
  const artifacts = path.join(root, 'artifacts'); await fs.mkdir(artifacts, { recursive: true });
  let desktop;
  try {
    desktop = await electron.launch({ executablePath: process.env.ANYPS5_TEST_APP || require('electron'), args: process.env.ANYPS5_TEST_APP ? [] : [root], env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: path.join(temporary, 'profile') } });
    await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].showInactive());
    let page = await desktop.firstWindow(); const errors = [];
    page.setDefaultTimeout(12000);
    page.on('pageerror', error => errors.push(error.message));
    page.on('console', message => { if (message.type() === 'error') errors.push(message.text()); });
    await page.locator('#library-empty h1').waitFor();
    await page.screenshot({ timeout: 12000, path: path.join(artifacts, '01-library-empty.png') });
    await page.locator('#empty-convert').click();
    await page.locator('#step-1 h2').waitFor();
    await page.screenshot({ timeout: 12000, path: path.join(artifacts, '02-conversion.png') });
    await page.locator('#settings-open').click();
    await page.locator('#settings-dialog h2').waitFor();
    await page.screenshot({ timeout: 12000, path: path.join(artifacts, '03-settings.png') });
    assert.equal(await page.locator('html').getAttribute('lang'), 'en');
    assert.equal(await page.locator('[id^="player-"][id$="-source"]').count(), 4);
    assert.equal(await page.locator('#player-1-source').inputValue(), 'keyboardMouse');
    assert.equal(await page.locator('#player-4-source').inputValue(), 'disabled');
    assert.equal(await page.locator('#player-2-source option[value="keyboardMouse"]').isDisabled(), true);
    await page.locator('#settings-tab-input').click();
    await page.locator('#input-left-deadzone').fill('12');
    await page.locator('#input-right-deadzone').fill('8');
    await page.locator('#input-mouse-sensitivity').fill('2.5');
    await page.locator('#settings-tab-players').click();
    await page.locator('#player-2-source').selectOption('dualsense-1');
    await page.locator('#player-3-source').selectOption('dualsense-2');
    await page.locator('#player-4-source').selectOption('dualsense-3');
    assert.equal(await page.locator('#player-3-source option[value="dualsense-1"]').isDisabled(), true);
    // Keyboard assignment may move to any player after freeing the current owner.
    await page.locator('#player-1-source').selectOption('disabled');
    await page.locator('#player-4-source').selectOption('keyboardMouse');
    assert.equal(await page.locator('#player-1-source option[value="keyboardMouse"]').isDisabled(), true);
    await page.locator('#player-4-source').selectOption('dualsense-3');
    await page.locator('#player-1-source').selectOption('keyboardMouse');
    // Simulated Gamepad API devices validate detection, not physical hardware.
    await page.evaluate(() => {
      const pad = (index, id) => ({ index, id, connected: true, mapping: 'standard', axes: [0, 0, 0, 0], buttons: Array.from({ length: 18 }, () => ({ pressed: false, touched: false, value: 0 })) });
      window.testGamepads = [
        pad(0, 'DualSense Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 0ce6)'),
        pad(1, 'Xbox Controller (STANDARD GAMEPAD Vendor: 045e Product: 02ea)'),
        pad(2, 'Generic USB Gamepad')
      ];
      Object.defineProperty(navigator, 'getGamepads', { configurable: true, value: () => window.testGamepads });
      window.dispatchEvent(new Event('gamepadconnected'));
    });
    await page.waitForFunction(() => document.getElementById('controllers-count').textContent.includes('3 / 3'));
    assert.match(await page.locator('#player-2-status').textContent(), /connected/i);
    assert.match(await page.locator('#controller-2').textContent(), /Xbox Controller/);
    assert.match(await page.locator('#controller-3').textContent(), /Generic USB Gamepad/);
    await page.evaluate(() => { window.testGamepads[0].connected = false; window.dispatchEvent(new Event('gamepaddisconnected')); });
    await page.waitForFunction(() => document.getElementById('controllers-count').textContent.includes('2 / 3'));
    assert.match(await page.locator('#player-2-status').textContent(), /disconnected|not connected/i);
    assert.doesNotMatch(await page.locator('#player-3-status').textContent(), /disconnected|not connected/i);
    await page.evaluate(() => { window.testGamepads[0].connected = true; window.dispatchEvent(new Event('gamepadconnected')); });
    await page.waitForFunction(() => document.getElementById('controllers-count').textContent.includes('3 / 3'));
    const playerProfile = [
      { type: 'keyboardMouse', controllerSlot: null },
      { type: 'dualsense', controllerSlot: 1 },
      { type: 'dualsense', controllerSlot: 2 },
      { type: 'dualsense', controllerSlot: 3 }
    ];
    await page.locator('#settings-form button[type=submit]').click();
    await page.locator('#settings-dialog').waitFor({ state: 'hidden' });
    assert.deepEqual(JSON.parse(await fs.readFile(path.join(temporary, 'profile', 'library.json'), 'utf8')).settings.players, playerProfile);
    // A real application restart verifies persistence and starts with no simulated devices.
    await desktop.close();
    desktop = await electron.launch({ executablePath: process.env.ANYPS5_TEST_APP || require('electron'), args: process.env.ANYPS5_TEST_APP ? [] : [root], env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: path.join(temporary, 'profile') } });
    await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].showInactive());
    page = await desktop.firstWindow(); page.setDefaultTimeout(12000);
    page.on('pageerror', error => errors.push(error.message));
    page.on('console', message => { if (message.type() === 'error') errors.push(message.text()); });
    await page.locator('#library-empty h1').waitFor();
    await page.locator('#settings-open').click();
    for (const [index, value] of ['keyboardMouse', 'dualsense-1', 'dualsense-2', 'dualsense-3'].entries()) assert.equal(await page.locator('#player-' + (index + 1) + '-source').inputValue(), value);
    assert.deepEqual((await page.evaluate(() => window.desktop.state())).settings.players, playerProfile);
    await page.screenshot({ timeout: 12000, path: path.join(artifacts, '08-player-settings.png') });
    assert.doesNotMatch(await page.locator('body').innerText(), /[ăâîșțĂÂÎȘȚ]/);
    await page.locator('#settings-close').click();
    await page.locator('#empty-convert').click();
    await page.locator('#settings-open').click();
    await page.locator('#settings-tab-engine').click();

    const source = path.join(temporary, 'source'); const libs = path.join(temporary, 'libs'); const engine = path.join(temporary, 'engine'); const games = path.join(temporary, 'games');
    for (const dir of [source, libs, engine, games, path.join(source, 'sce_module')]) await fs.mkdir(dir, { recursive: true });
    await fs.writeFile(path.join(source, 'eboot.bin'), Buffer.from([127, 69, 76, 70, 2, 1, 1, 0]));
    await fs.writeFile(path.join(source, 'data.txt'), 'fixture resource');
    const gcc = process.env.ANYPS5_TEST_GCC || path.resolve(root, '..', 'AnyPS5', 'build_tools', 'winlibs', 'mingw64', 'bin', 'gcc.exe');
    await fs.writeFile(path.join(engine, 'game.c'), '#include <stdio.h>\n#include <stdlib.h>\nint main(){const char*p=getenv("ANYPS5_INPUT_CONFIG");if(!p)return 2;FILE*i=fopen(p,"rb");if(!i)return 3;FILE*f=fopen("input-received.txt","wb");if(!f)return 4;int c;while((c=fgetc(i))!=EOF)fputc(c,f);fclose(i);fclose(f);f=fopen("launched.txt","w");if(!f)return 1;fputs("launched",f);fclose(f);return 0;}');
    const gameExe = path.join(engine, 'game.exe'); execFileSync(gcc, ['-static', path.join(engine, 'game.c'), '-o', gameExe], { windowsHide: true, env: { ...process.env, PATH: path.dirname(gcc) + path.delimiter + process.env.PATH } });
    await fs.writeFile(path.join(engine, 'converter.c'), '#include <stdio.h>\n#include <string.h>\nint main(int argc,char**argv){char p[32768];strcpy(p,argv[0]);char*s=strrchr(p,92);if(!s)return 3;strcpy(s+1,"game.exe");FILE*i=fopen(p,"rb");FILE*o=fopen(argv[argc-1],"wb");if(!i||!o)return 4;int c;while((c=fgetc(i))!=EOF)fputc(c,o);fclose(i);fclose(o);puts("SIMULATED RELINKER: desktop integration fixture only");return 0;}');
    const relinker = path.join(engine, 'fixture-relinker.exe'); execFileSync(gcc, ['-static', path.join(engine, 'converter.c'), '-o', relinker], { windowsHide: true, env: { ...process.env, PATH: path.dirname(gcc) + path.delimiter + process.env.PATH } });
    for (const name of ['libkernel.prx', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll']) await fs.copyFile(gameExe, path.join(libs, name));
    await page.locator('#setting-relinker').fill(relinker); await page.locator('#setting-libs').fill(libs); await page.locator('#setting-dllDirectory').fill('');
    await page.locator('#settings-form button[type=submit]').click();
    await desktop.evaluate(({ dialog }, selections) => { dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [selections.shift()] }); }, [source, games]);
    await page.locator('#pick-source').click(); await page.locator('#source-details').waitFor({ state: 'visible' });
    await page.locator('#game-name-input').fill('Desktop integration fixture');
    await page.locator('#to-step-2').click();
    await page.locator('#pick-destination').click();
    await page.screenshot({ timeout: 12000, path: path.join(artifacts, '04-destination.png') });
    await page.locator('#begin-install').click();
    await page.locator('#step-4 h2').waitFor({ timeout: 20000 });
    await page.screenshot({ timeout: 12000, path: path.join(artifacts, '05-complete.png') });
    const installed = path.join(games, 'Desktop integration fixture');
    assert.equal(await fs.readFile(path.join(installed, 'app0', 'data.txt'), 'utf8'), 'fixture resource');
    assert.ok((await fs.readFile(path.join(source, 'eboot.bin')))[0] === 127);
    const shortcutDirectory = path.join(temporary, 'desktop'); await fs.mkdir(shortcutDirectory);
    await desktop.evaluate(({ app }, directory) => app.setPath('desktop', directory), shortcutDirectory);
    await page.locator('#complete-shortcut').click();
    await page.locator('#toast').filter({ hasText: /shortcut/i }).waitFor();
    await fs.access(path.join(shortcutDirectory, 'Desktop integration fixture.lnk'));
    const shortcut = await desktop.evaluate(({ shell }, link) => shell.readShortcutLink(link), path.join(shortcutDirectory, 'Desktop integration fixture.lnk'));
    assert.equal(shortcut.target.toLowerCase(), path.join(installed, 'app.exe').toLowerCase()); assert.equal(shortcut.cwd.toLowerCase(), installed.toLowerCase());
    await page.locator('#complete-library').click();
    await page.locator('#game-title').waitFor({ state: 'visible' });
    await page.screenshot({ timeout: 12000, path: path.join(artifacts, '06-library-game.png') });
    await page.locator('#game-launch').click();
    await page.waitForFunction(() => /started/i.test(document.getElementById('toast').textContent));
    for (let wait = 0; wait < 80 && !await fs.access(path.join(installed, 'launched.txt')).then(() => true, () => false); wait++) await page.waitForTimeout(50);
    assert.equal(await fs.readFile(path.join(installed, 'launched.txt'), 'utf8'), 'launched');
    const receivedInput = await fs.readFile(path.join(installed, 'input-received.txt'), 'utf8');
    assert.match(receivedInput, /LeftStickDeadzone = 12/); assert.match(receivedInput, /RightStickDeadzone = 8/); assert.match(receivedInput, /MouseSensitivity = 2.5/);
    const customInput = 'Cross = KEY:F\nLeftStickDeadzone = 19\n';
    await fs.writeFile(path.join(installed, 'anyps5-input.ini'), customInput);
    await page.evaluate(() => window.desktop.state().then(state => window.desktop.launch(state.games[0].id)));
    for (let wait = 0; wait < 80 && await fs.readFile(path.join(installed, 'input-received.txt'), 'utf8') !== customInput; wait++) await page.waitForTimeout(50);
    assert.equal(await fs.readFile(path.join(installed, 'input-received.txt'), 'utf8'), customInput);
    assert.equal(await fs.readFile(path.join(installed, 'anyps5-input.ini'), 'utf8'), customInput);
    await page.reload(); await page.locator('#library-empty h1').waitFor();
    assert.equal(await page.locator('[data-game-id]').count(), 1);
    await page.locator('#search-toggle').click();
    await page.locator('#search-input').fill('no matching game');
    assert.equal(await page.locator('[data-game-id]').count(), 0);
    await page.locator('#search-input').fill('desktop');
    assert.equal(await page.locator('[data-game-id]').count(), 1);
    await page.locator('#search-toggle').click();
    await page.locator('[data-game-id]').click();
    await page.locator('#game-menu-toggle').click();
    await desktop.evaluate(({ dialog }) => { dialog.showMessageBox = async () => ({ response: 1 }); });
    await page.locator('#game-remove').click();
    await page.locator('#library-empty h1').waitFor();
    assert.equal(await page.locator('[data-game-id]').count(), 0);
    await fs.access(path.join(installed, 'app.exe'));
    await desktop.evaluate(({ dialog }, directory) => { dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [directory] }); }, installed);
    await page.locator('#empty-import').click();
    await page.locator('#game-title').waitFor({ state: 'visible' });
    assert.equal(await page.locator('[data-game-id]').count(), 1);
    await page.setViewportSize({ width: 1040, height: 720 });
    await page.locator('#nav-convert').click();
    await page.screenshot({ timeout: 12000, path: path.join(artifacts, '07-compact-window.png') });
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth));
    await page.locator('#settings-open').click();
    await page.locator('#settings-tab-players').click();
    await page.locator('#player-4-source').scrollIntoViewIfNeeded();
    const dialogFits = await page.locator('#settings-dialog').evaluate(element => { const r = element.getBoundingClientRect(); return r.left >= 0 && r.top >= 0 && r.right <= window.innerWidth && r.bottom <= window.innerHeight && element.scrollWidth <= element.clientWidth; });
    assert.ok(dialogFits, 'Settings stays within the compact window');
    await page.screenshot({ timeout: 12000, path: path.join(artifacts, '09-player-settings-compact.png') });

    assert.deepEqual(errors, []);
    console.log('PASS: English Electron UI, four-player profile validation and restart persistence, simulated DualSense connection/disconnection, compact settings, source selection, conversion orchestration, atomic installation, library persistence, search, remove/import without deleting files, real .lnk creation, executable launch and compact layout. Relinker fixture is simulated, not a PS5 game.');
  } finally {
    if (desktop) await desktop.close();
    if (path.dirname(temporary) === os.tmpdir() && path.basename(temporary).startsWith('anyps5-desktop-ui-')) await fs.rm(temporary, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
