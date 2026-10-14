const { _electron: electron } = require('playwright');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');

// Only the native dialog is simulated. The production picker IPC and state
// persistence run unchanged against an isolated profile, including a real restart.
(async () => {
  const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-browse-ui-'));
  const profile = path.join(temporary, 'profile');
  const libraryFile = path.join(profile, 'library.json');
  let desktop;
  let page;
  const errors = [];
  try {
    const sourceA = path.join(temporary, 'source-a');
    const sourceB = path.join(temporary, 'source-b');
    const entryFolder = path.join(sourceA, 'manual');
    const destinationA = path.join(temporary, 'games-a');
    const destinationB = path.join(temporary, 'games-b');
    const engineA = path.join(temporary, 'engine-a');
    const engineB = path.join(temporary, 'engine-b');
    const libsA = path.join(temporary, 'libs-a');
    const libsB = path.join(temporary, 'libs-b');
    const dllA = path.join(temporary, 'dll-a');
    const dllB = path.join(temporary, 'dll-b');
    const converted = path.join(temporary, 'converted');
    const gameDirectory = path.join(temporary, 'existing-game');
    for (const directory of [profile, sourceA, sourceB, entryFolder, destinationA, destinationB, engineA, engineB, libsA, libsB, dllA, dllB, converted, gameDirectory]) await fs.mkdir(directory, { recursive: true });
    const firstEntry = path.join(sourceA, 'eboot.bin');
    const secondEntry = path.join(entryFolder, 'manual.elf');
    const oldRelinker = path.join(engineA, 'relinker.exe');
    const newRelinker = path.join(engineB, 'relinker.exe');
    for (const file of [firstEntry, secondEntry, oldRelinker, newRelinker, path.join(gameDirectory, 'app.exe')]) await fs.writeFile(file, 'Browse fixture; never executed.');
    const settings = {
      relinker: oldRelinker, libs: libsA, dllDirectory: dllA, destinationParent: destinationA, toIntel: false,
      players: [{ type: 'keyboardMouse', controllerSlot: null }, ...Array.from({ length: 3 }, () => ({ type: 'disabled', controllerSlot: null }))],
      fixtureSetting: 'preserve existing preferences'
    };
    const games = [{ id: 'browse-existing', title: 'Existing game', directory: gameDirectory, executable: 'app.exe', installedAt: '2026-10-13T08:00:00Z' }];
    await fs.writeFile(libraryFile, JSON.stringify({ settings, games }));
    const readLibrary = async () => JSON.parse(await fs.readFile(libraryFile, 'utf8'));
    const normalize = value => path.resolve(value).toLowerCase();
    const samePath = (actual, expected, message) => assert.equal(normalize(actual), normalize(expected), message);
    const launch = async () => {
      desktop = await electron.launch({
        executablePath: process.env.ANYPS5_TEST_APP || require('electron'),
        args: process.env.ANYPS5_TEST_APP ? [] : [root],
        env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: profile }
      });
      page = await desktop.firstWindow(); page.setDefaultTimeout(10000);
      page.on('pageerror', error => errors.push(error.message));
      await page.locator('#library-empty h1').waitFor();
      await desktop.evaluate(({ dialog }) => {
        globalThis.browseTest = { responses: [], dialogs: [] };
        dialog.showOpenDialog = async (_window, options) => {
          globalThis.browseTest.dialogs.push({ defaultPath: options.defaultPath, title: options.title, properties: options.properties });
          const response = globalThis.browseTest.responses.shift();
          if (!response) throw new Error('The browse test did not provide a dialog response.');
          return response;
        };
      });
    };
    const pick = async (type, selected = null, expectedDefault) => {
      const response = selected === null ? { canceled: true, filePaths: [] } : { canceled: false, filePaths: [selected] };
      await desktop.evaluate((_electron, result) => globalThis.browseTest.responses.push(result), response);
      const value = await page.evaluate(kind => window.desktop.pick(kind), type);
      assert.equal(value, selected, 'Picker must return the native selection without changing it.');
      const options = await desktop.evaluate(() => globalThis.browseTest.dialogs.at(-1));
      if (expectedDefault !== undefined) samePath(options.defaultPath, expectedDefault, type + ' picker defaultPath');
      assert.ok(options.properties.includes(['entry', 'relinker', 'iso'].includes(type) ? 'openFile' : 'openDirectory'));
      return options;
    };

    await launch();
    const staleSettings = (await page.evaluate(() => window.desktop.state())).settings;
    const untouched = await readLibrary();
    await pick('destination', null, destinationA);
    await pick('libs', null, libsA);
    await pick('dllDirectory', null, dllA);
    const initialRelinker = await pick('relinker');
    assert.ok([oldRelinker, engineA].map(normalize).includes(normalize(initialRelinker.defaultPath)), 'The first relinker dialog must use the configured executable or its folder.');
    assert.deepEqual(await readLibrary(), untouched, 'Cancelled dialogs must not rewrite the library or create remembered selections.');

    await pick('source', sourceA);
    await pick('iso', null, sourceA);
    await pick('iso', path.join(engineA, 'fixture.iso'), sourceA);
    await pick('iso', null, engineA);
    await pick('entry', null, sourceA);
    await pick('destination', destinationB, destinationA);
    await pick('source', null, sourceA);
    await pick('entry', firstEntry, sourceA);
    await pick('source', sourceB, sourceA);
    await pick('entry', null, sourceA);
    await pick('entry', secondEntry, sourceA);
    await pick('destination', destinationA, destinationB);
    await pick('relinker', newRelinker);
    await pick('libs', libsB, libsA);
    await pick('dllDirectory', dllB, dllA);
    await pick('converted', converted);
    const remembered = { iso: engineA, source: sourceB, destination: destinationA, entry: entryFolder, relinker: engineB, libs: libsB, dllDirectory: dllB, converted };
    assert.deepEqual((await readLibrary()).browsePaths, remembered, 'Each picker must remember its own folder; file picks remember their parent directory.');
    assert.deepEqual((await readLibrary()).games, games);
    assert.deepEqual((await readLibrary()).settings, staleSettings, 'Browsing alone must not change app settings, including loaded defaults.');
    console.log('PASS: native defaultPath uses configured paths, independent source/destination/entry memory, and folder parents for file selections.');

    await page.evaluate(saved => window.desktop.saveSettings({ ...saved, toIntel: true }), staleSettings);
    const afterSettings = await readLibrary();
    assert.deepEqual(afterSettings.browsePaths, remembered, 'Saving a settings snapshot taken before browsing must preserve the latest picker memory.');
    assert.deepEqual(afterSettings.settings, { ...staleSettings, toIntel: true });
    assert.deepEqual(afterSettings.games, games);
    await desktop.evaluate((_electron, file) => globalThis.browseTest.responses.push({ canceled: true, filePaths: [file] }), sourceA);
    assert.equal(await page.evaluate(() => window.desktop.pick('source')), null);
    assert.deepEqual(await readLibrary(), afterSettings, 'Cancel must keep prior memory even if a native result includes a filePaths value.');
    console.log('PASS: stale settings saves preserve browser memory; cancelling preserves memory, game entries, and preferences.');

    await desktop.close(); desktop = null;
    await launch();
    for (const [kind, directory] of Object.entries(remembered)) await pick(kind, null, directory);
    assert.deepEqual(await readLibrary(), afterSettings, 'Remembered paths must survive a real restart without cancellation changing the saved state.');
    console.log('PASS: all eight picker folders survive a real isolated-profile restart.');

    const missingParent = path.join(sourceB, 'removed');
    const missingFolder = path.join(missingParent, 'nested');
    await fs.mkdir(missingFolder, { recursive: true });
    await pick('source', missingFolder, sourceB);
    // These are empty test directories whose resolved paths are verified below.
    for (const directory of [missingFolder, missingParent]) {
      const relative = path.relative(temporary, path.resolve(directory));
      assert.ok(relative && !relative.startsWith('..') && !path.isAbsolute(relative));
      await fs.rmdir(directory);
    }
    await pick('source', null, sourceB);
    const finalState = await readLibrary();
    assert.equal(finalState.browsePaths.source, missingFolder, 'A cancelled fallback dialog must not overwrite the last confirmed selection.');
    assert.deepEqual(finalState.browsePaths, { ...remembered, source: missingFolder });
    assert.deepEqual(finalState.games, games);
    assert.deepEqual(finalState.settings, { ...staleSettings, toIntel: true });
    assert.deepEqual(errors, []);
    console.log('PASS: missing remembered subfolders fall back to the nearest existing ancestor; unrelated selections/settings/games remain intact. No renderer errors.');
  } finally {
    if (desktop) await desktop.close();
    if (path.dirname(temporary) === os.tmpdir() && path.basename(temporary).startsWith('anyps5-browse-ui-')) await fs.rm(temporary, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
