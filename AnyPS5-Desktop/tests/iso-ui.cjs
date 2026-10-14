const { _electron: electron } = require('playwright');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');

// Source inspection and conversion IPC are simulated; no image is mounted and
// no converter is run. The real renderer/preload and settings persistence run.
(async () => {
  const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-iso-ui-'));
  let desktop; let page;
  try {
    const profile = path.join(temporary, 'profile'); await fs.mkdir(profile);
    const iso = path.join(temporary, 'Example game.iso');
    const folder = path.join(temporary, 'folder-game');
    const folderEntry = path.join(folder, 'eboot.bin');
    const manualEntry = path.join(folder, 'manual.elf');
    const canonicalEntry = path.join(folder, 'decrypted', 'manual.elf');
    const primaryEntry = 'Game/decrypted/eboot.bin';
    const alternateEntry = 'Game/alternate.elf';
    const validIso = { source: iso, sourceType: 'iso', title: 'ISO example', titleId: 'ISO123', entry: primaryEntry, executables: [primaryEntry, alternateEntry], encrypted: false, diagnostic: '', note: 'ISO image opened read-only. Using the decrypted executable with original game resources.', cleanupWarning: 'Test cleanup warning: eject the image from Windows if it remains mounted.' };
    await fs.writeFile(path.join(profile, 'library.json'), JSON.stringify({ settings: { destinationParent: path.join(temporary, 'games') }, games: [] }));
    desktop = await electron.launch({ executablePath: process.env.ANYPS5_TEST_APP || require('electron'), args: process.env.ANYPS5_TEST_APP ? [] : [root], env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: profile } });
    page = await desktop.firstWindow(); page.setDefaultTimeout(10000);
    const errors = []; page.on('pageerror', error => errors.push(error.message));
    await page.locator('#library-empty h1').waitFor();
    await desktop.evaluate(({ ipcMain }, fixture) => {
      const test = globalThis.isoTest = { picks: [], inspections: [], entryChecks: [], preflights: [], installs: [], holdIso: true, holdPreflight: true, nextIso: { ok: true, value: fixture.validIso }, cancelPick: false };
      const replace = (name, handler) => { ipcMain.removeHandler(name); ipcMain.handle(name, handler); };
      replace('dialog:pick', (_event, kind) => {
        test.picks.push(kind);
        if (test.cancelPick) { test.cancelPick = false; return { ok: true, value: null }; }
        return { ok: true, value: { iso: fixture.iso, source: fixture.folder, entry: fixture.manualEntry }[kind] || null };
      });
      replace('source:inspect', () => ({ ok: true, value: { source: fixture.folder, title: 'Folder example', entry: fixture.folderEntry, executables: [fixture.folderEntry], encrypted: false } }));
      replace('source:inspect-iso', (_event, image) => {
        test.inspections.push(image);
        if (test.holdIso) return new Promise(resolve => { test.resolveIso = resolve; });
        return test.nextIso;
      });
      replace('source:check-entry', (_event, request) => { test.entryChecks.push(request); return { ok: true, value: fixture.canonicalEntry }; });
      replace('installation:preflight', (_event, request) => {
        test.preflights.push(request);
        const result = { ok: true, value: { totalBytes: 1024, freeBytes: 1024 ** 3, resources: 2, extraModules: 0, cleanupWarning: 'Test preflight cleanup warning.' } };
        if (test.holdPreflight) return new Promise(resolve => { test.resolvePreflight = () => { test.holdPreflight = false; resolve(result); }; });
        return result;
      });
      replace('installation:start', (_event, request) => {
        test.installs.push(request);
        return new Promise(resolve => { test.resolveInstall = resolve; });
      });
    }, { iso, folder, folderEntry, manualEntry, canonicalEntry, validIso });
    const ready = () => page.waitForFunction(() => !document.getElementById('pick-source').disabled && !document.getElementById('pick-iso').disabled);
    const nextIso = async response => {
      await desktop.evaluate((_electron, result) => { globalThis.isoTest.holdIso = false; globalThis.isoTest.nextIso = result; }, response);
      await page.locator('#pick-iso').click(); await ready();
    };
    await page.locator('#nav-convert').click();
    assert.equal(await page.locator('#pick-iso').textContent().then(value => value.includes('Choose ISO image')), true);
    await page.locator('#pick-source').click(); await ready();
    assert.equal(await page.locator('#entry-input').inputValue(), folderEntry);
    assert.ok(await page.locator('#to-step-2').isEnabled());
    assert.ok(await page.locator('#iso-entry-select').isHidden());
    await desktop.evaluate(() => { globalThis.isoTest.cancelPick = true; });
    await page.locator('#pick-iso').click(); await ready();
    assert.equal(await page.locator('#entry-input').inputValue(), folderEntry, 'Cancelling ISO selection must retain the chosen folder.');
    assert.ok(await page.locator('#to-step-2').isEnabled());

    await page.locator('#pick-iso').click();
    await page.waitForFunction(() => document.getElementById('source-label').textContent === 'Checking ISO…');
    assert.ok(await page.locator('#pick-source').isDisabled()); assert.ok(await page.locator('#pick-iso').isDisabled());
    assert.ok(await page.locator('#to-step-2').isDisabled()); assert.ok(await page.locator('#source-details').isHidden());
    assert.equal(await page.locator('#entry-input').inputValue(), '', 'A new image check must clear the prior folder executable.');
    await desktop.evaluate((_electron, value) => globalThis.isoTest.resolveIso({ ok: true, value }), validIso);
    await ready();
    assert.equal(await page.locator('#source-label').textContent(), validIso.title);
    assert.equal(await page.locator('#source-hint').textContent(), iso);
    assert.match(await page.locator('#source-note').textContent(), /ISO image.*read-only/);
    assert.match(await page.locator('#source-note').textContent(), /Test cleanup warning/);
    assert.ok(await page.locator('#pick-entry').isHidden()); assert.ok(await page.locator('#pick-entry').isDisabled());
    assert.ok(await page.locator('#iso-entry-select').isVisible());
    assert.equal(await page.locator('#iso-entry-select').inputValue(), primaryEntry);
    await page.locator('#iso-entry-select').focus();
    await page.keyboard.press('Enter'); await page.keyboard.press('ArrowDown');
    assert.equal(await page.locator('#iso-entry-select').inputValue(), alternateEntry);
    await page.keyboard.press('Escape');
    assert.equal(await page.locator('#iso-entry-select').inputValue(), primaryEntry);
    await page.keyboard.press('Enter'); await page.keyboard.press('ArrowDown'); await page.keyboard.press('Enter');
    assert.equal(await page.locator('#iso-entry-select').inputValue(), alternateEntry);
    await page.locator('#to-step-2').click(); await page.locator('#begin-install').click();
    await page.waitForFunction(() => document.getElementById('begin-install').textContent === 'Checking files…');
    for (const id of ['back-step-1', 'pick-destination', 'wizard-settings']) assert.ok(await page.locator('#' + id).isDisabled());
    await page.locator('#nav-library').click(); await page.keyboard.press('Escape'); await page.locator('#nav-convert').click();
    await page.locator('#settings-open').click();
    assert.ok(await page.locator('#step-2').isVisible(), 'Pending ISO preflight must keep the review visible and prevent a hidden conversion.');
    assert.ok(await page.locator('#settings-dialog').isHidden());
    await desktop.evaluate(() => globalThis.isoTest.resolvePreflight());
    await page.locator('#step-3').waitFor({ state: 'visible' });
    const requests = await desktop.evaluate(() => ({ preflight: globalThis.isoTest.preflights[0], install: globalThis.isoTest.installs[0] }));
    for (const request of [requests.preflight, requests.install]) {
      assert.equal(request.sourceType, 'iso'); assert.equal(request.source, iso); assert.equal(request.entry, alternateEntry);
      assert.equal(request.title, validIso.title);
    }
    assert.deepEqual(await desktop.evaluate(() => globalThis.isoTest.inspections), [iso]);
    assert.equal((await desktop.evaluate(() => globalThis.isoTest.picks)).filter(kind => kind === 'entry').length, 0, 'ISO executable selection must never open a native file picker.');
    assert.match(await page.locator('#toast').textContent(), /preflight cleanup warning/);
    await desktop.evaluate(() => globalThis.isoTest.resolveInstall({ ok: false, error: 'ISO test stops before conversion.' }));
    await page.locator('#step-2').waitFor({ state: 'visible' }); await page.locator('#back-step-1').click();
    console.log('PASS: ISO selection shows pending state, original image path and read-only note; relative executable choices support Enter/arrows/Esc and reach preflight/install intact.');

    for (const diagnostic of ['This image contains encrypted SELF executables. AnyPS5 cannot decrypt them.', 'This image contains PKG files only. Extract decrypted game files first.']) {
      await nextIso({ ok: true, value: { ...validIso, entry: '', executables: [], encrypted: true, diagnostic, note: '', cleanupWarning: '' } });
      assert.ok(await page.locator('#to-step-2').isDisabled()); assert.ok(await page.locator('#iso-entry-select').isDisabled());
      assert.equal(await page.locator('#wizard-error').textContent(), diagnostic);
    }
    await nextIso({ ok: false, error: 'The selected image is malformed or unsupported.' });
    assert.ok(await page.locator('#to-step-2').isDisabled()); assert.ok(await page.locator('#source-details').isHidden());
    assert.equal(await page.locator('#wizard-error').textContent(), 'The selected image is malformed or unsupported.');
    await nextIso({ ok: true, value: { ...validIso, entry: folderEntry, executables: [folderEntry, '../outside.elf'], note: '', cleanupWarning: '' } });
    assert.ok(await page.locator('#to-step-2').isDisabled(), 'Absolute and escaping executable paths must not be treated as ISO-relative choices.');
    assert.ok(await page.locator('#iso-entry-select').isDisabled());
    await nextIso({ ok: true, value: { title: 'Invalid inspection response' } });
    assert.ok(await page.locator('#to-step-2').isDisabled());
    assert.match(await page.locator('#wizard-error').textContent(), /did not contain a usable game source/);
    console.log('PASS: encrypted, PKG-only, malformed and invalid ISO responses clear stale choices and block Continue with an explanation.');

    await page.locator('#pick-source').click(); await ready();
    assert.ok(await page.locator('#iso-entry-select').isHidden()); assert.ok(await page.locator('#pick-entry').isVisible());
    assert.ok(await page.locator('#pick-entry').isEnabled());
    await page.locator('#pick-entry').click(); await ready();
    assert.equal(await page.locator('#entry-input').inputValue(), canonicalEntry);
    assert.deepEqual(await desktop.evaluate(() => globalThis.isoTest.entryChecks), [{ source: folder, entry: manualEntry }]);
    await page.locator('#to-step-2').click(); await page.locator('#begin-install').click();
    await page.locator('#step-3').waitFor({ state: 'visible' });
    const folderRequest = await desktop.evaluate(() => globalThis.isoTest.installs.at(-1));
    assert.equal(folderRequest.sourceType, 'folder'); assert.equal(folderRequest.source, folder); assert.equal(folderRequest.entry, canonicalEntry);
    await desktop.evaluate((_electron, game) => globalThis.isoTest.resolveInstall({ ok: true, value: { game, librarySaved: true, warning: 'Image cleanup needs attention, but the game was saved.' } }), { id: 'iso-ui-fixture', title: 'Folder example', directory: path.join(temporary, 'games', 'example') });
    await page.locator('#step-4').waitFor({ state: 'visible' });
    assert.ok(await page.locator('#complete-launch').isEnabled()); assert.ok(await page.locator('#complete-shortcut').isEnabled());
    assert.match(await page.locator('#wizard-error').textContent(), /cleanup needs attention/);
    assert.deepEqual(errors, []);
    console.log('PASS: folder picking/manual entry validation still uses canonical absolute paths; cleanup-only warnings retain launch actions. No renderer errors.');
  } finally {
    if (desktop) await desktop.close();
    if (path.dirname(temporary) === os.tmpdir() && path.basename(temporary).startsWith('anyps5-iso-ui-')) await fs.rm(temporary, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
