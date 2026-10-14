const { _electron: electron } = require('playwright');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');

// Conversion and file-picker IPC are simulated. This exercises the production
// renderer/preload without launching a converter or touching the user's profile.
(async () => {
  const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-progress-ui-'));
  const artifacts = path.join(root, 'artifacts');
  let desktop;
  let page;
  try {
    const profile = path.join(temporary, 'profile');
    const source = path.join(temporary, 'source');
    const entry = path.join(source, 'eboot.bin');
    const selectedEntry = path.join(source, 'manual.elf');
    const canonicalEntry = path.join(source, 'resolved', 'manual.elf');
    const invalidEntry = path.join(temporary, 'outside.elf');
    await fs.mkdir(profile);
    await fs.mkdir(source);
    await fs.mkdir(artifacts, { recursive: true });
    await fs.writeFile(path.join(profile, 'library.json'), JSON.stringify({ settings: { destinationParent: path.join(temporary, 'games') }, games: [] }));
    desktop = await electron.launch({
      executablePath: process.env.ANYPS5_TEST_APP || require('electron'),
      args: process.env.ANYPS5_TEST_APP ? [] : [root],
      env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: profile }
    });
    page = await desktop.firstWindow();
    page.setDefaultTimeout(10000);
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.locator('#library-empty h1').waitFor();
    // Present the isolated window so Chromium can capture actual composited frames.
    await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].showInactive());
    await desktop.evaluate(({ ipcMain }, fixture) => {
      globalThis.progressTest = { installs: 0, cancellations: 0, checkedEntries: [], requests: [], entrySelections: [fixture.invalidEntry, fixture.selectedEntry] };
      const replace = (name, handler) => { ipcMain.removeHandler(name); ipcMain.handle(name, handler); };
      replace('dialog:pick', (_event, type) => ({
        ok: true,
        value: type === 'source' ? fixture.source : type === 'entry' ? globalThis.progressTest.entrySelections.shift() : null
      }));
      replace('source:inspect', () => ({
        ok: true,
        value: { source: fixture.source, title: 'Progress test fixture', titleId: '', executables: [fixture.entry], entry: fixture.entry, encrypted: false }
      }));
      replace('source:check-entry', (_event, request) => {
        globalThis.progressTest.checkedEntries.push(request);
        return new Promise(resolve => { globalThis.progressTest.resolveEntry = resolve; });
      });
      replace('installation:preflight', () => ({
        ok: true,
        value: { totalBytes: 1024, freeBytes: 1024 ** 3, resources: 1, extraModules: 0 }
      }));
      replace('installation:start', (_event, request) => {
        globalThis.progressTest.installs++;
        globalThis.progressTest.requests.push(request);
        return new Promise(resolve => {
          globalThis.progressTest.resolveInstall = response => {
            globalThis.progressTest.resolveInstall = null;
            resolve(response);
          };
        });
      });
      replace('installation:cancel', () => {
        globalThis.progressTest.cancellations++;
        globalThis.progressTest.resolveInstall?.({ ok: false, error: 'Conversion was cancelled by the progress test.' });
        return { ok: true };
      });
    }, { source, entry, selectedEntry, canonicalEntry, invalidEntry });
    await page.locator('#empty-convert').click();
    await page.locator('#pick-source').click();
    await page.waitForFunction(() => !document.getElementById('to-step-2').disabled);
    assert.equal(await page.locator('#entry-input').inputValue(), entry);

    const waitForCheckedEntry = async count => {
      // IPC calls and DOM events travel in different processes; wait for both.
      for (let attempt = 0; attempt < 100; attempt++) {
        if (await desktop.evaluate(() => globalThis.progressTest.checkedEntries.length) === count) return;
        await page.waitForTimeout(20);
      }
      assert.fail('Executable validation IPC was not received.');
    };
    await page.locator('#pick-entry').click();
    await waitForCheckedEntry(1);
    assert.equal(await page.locator('#to-step-2').isDisabled(), true, 'Continue must be blocked while executable validation is pending.');
    await desktop.evaluate(() => globalThis.progressTest.resolveEntry({ ok: false, error: 'The executable must be a decrypted ELF file inside the game folder.' }));
    await page.locator('#wizard-error').filter({ hasText: 'The executable must be a decrypted ELF file inside the game folder.' }).waitFor();
    assert.equal(await page.locator('#entry-input').inputValue(), '', 'A rejected manual executable must clear the old valid choice.');
    assert.equal(await page.locator('#to-step-2').isDisabled(), true);
    await page.locator('#pick-entry').click();
    await waitForCheckedEntry(2);
    assert.equal(await page.locator('#to-step-2').isDisabled(), true);
    await desktop.evaluate((_electron, value) => globalThis.progressTest.resolveEntry({ ok: true, value }), canonicalEntry);
    await page.waitForFunction(value => document.getElementById('entry-input').value === value && !document.getElementById('to-step-2').disabled, canonicalEntry);
    assert.equal(await page.locator('#wizard-error').isHidden(), true);
    assert.deepEqual(await desktop.evaluate(() => globalThis.progressTest.checkedEntries), [
      { source, entry: invalidEntry },
      { source, entry: selectedEntry }
    ]);
    console.log('PASS: manual executable validation blocks Continue while pending, clears a rejected choice, and uses the confirmed canonical path.');

    await page.locator('#to-step-2').click();
    const state = () => page.evaluate(() => {
      const track = document.getElementById('progress-track');
      const fill = document.getElementById('progress-fill');
      const current = document.getElementById('progress-current');
      const currentStyle = getComputedStyle(current);
      return {
        label: document.getElementById('phase-label').textContent,
        stage: document.getElementById('phase-stage').textContent,
        percent: document.getElementById('phase-percent').textContent,
        status: document.getElementById('progress-status').textContent,
        role: track.getAttribute('role'),
        minimum: track.getAttribute('aria-valuemin'),
        maximum: track.getAttribute('aria-valuemax'),
        now: track.getAttribute('aria-valuenow'),
        fill: fill.style.width,
        currentHidden: currentStyle.display === 'none' || currentStyle.visibility === 'hidden' || Number(currentStyle.opacity) === 0,
        currentBackground: currentStyle.backgroundImage,
        currentWidth: current.getBoundingClientRect().width,
        trackWidth: track.getBoundingClientRect().width,
        fillWidth: fill.getBoundingClientRect().width
      };
    });
    const assertProgress = async ({ label, stage, filled, copyPercent = '', complete = false }) => {
      await page.waitForFunction(expected => {
        const track = document.getElementById('progress-track');
        const fill = document.getElementById('progress-fill');
        return track?.getAttribute('aria-valuenow') === String(expected.filled)
          && (!expected.label || document.getElementById('phase-label').textContent === expected.label)
          && Math.abs(fill.getBoundingClientRect().width / track.getBoundingClientRect().width * 100 - expected.filled) < 1;
      }, { label, filled });
      const current = await state();
      if (label) assert.equal(current.label, label);
      assert.equal(current.stage, 'Stage ' + stage + ' of 5');
      assert.equal(current.percent, copyPercent);
      assert.equal(current.status, complete ? 'Complete' : 'Running…');
      assert.equal(current.role, 'progressbar');
      assert.equal(current.minimum, '0');
      assert.equal(current.maximum, '100');
      assert.equal(current.now, String(filled));
      assert.equal(current.fill, filled + '%');
      if (complete) {
        assert.ok(current.currentHidden || current.currentWidth === 0, 'There must be no active stage segment after completion.');
      } else {
        assert.equal(current.currentHidden, false, 'The current stage must remain visibly distinct from completed stages.');
        assert.notEqual(current.currentBackground, 'none', 'The current stage uses a striped background.');
        assert.ok(Math.abs(current.currentWidth / current.trackWidth * 100 - 20) < 1, 'The active stage occupies one of five equal segments.');
      }
    };
    const begin = async count => {
      await page.locator('#begin-install').click();
      await page.locator('#step-3').waitFor({ state: 'visible' });
      for (let attempt = 0; attempt < 100; attempt++) {
        if (await desktop.evaluate(() => globalThis.progressTest.installs) === count) break;
        await page.waitForTimeout(20);
      }
      assert.equal(await desktop.evaluate(() => globalThis.progressTest.installs), count);
      assert.equal(await desktop.evaluate(() => globalThis.progressTest.requests.at(-1).entry), canonicalEntry);
      await assertProgress({ stage: 1, filled: 0 });
      assert.equal(await page.locator('#install-log').textContent(), '', 'A new attempt must clear the previous log.');
    };
    const phase = async (stage, stageIndex, label, progress = null, complete = false) => {
      await desktop.evaluate(({ BrowserWindow }, event) => BrowserWindow.getAllWindows()[0].webContents.send('installation:event', event), {
        kind: 'phase', stage, stageIndex, stageCount: 5, label, progress, complete
      });
    };
    await begin(1);
    await phase('checking', 1, 'Checking files');
    await assertProgress({ label: 'Checking files', stage: 1, filled: 0 });
    await phase('copying', 2, 'Copying game files', 0);
    await assertProgress({ label: 'Copying game files', stage: 2, filled: 20, copyPercent: '0% of bytes copied' });
    await phase('copying', 2, 'Copying game files', 63);
    await assertProgress({ label: 'Copying game files', stage: 2, filled: 20, copyPercent: '63% of bytes copied' });
    await phase('copying', 2, 'Copying game files', 100);
    await assertProgress({ label: 'Copying game files', stage: 2, filled: 20, copyPercent: '100% of bytes copied' });
    console.log('PASS: 100% of copied bytes only completes the copy count; the full conversion stays Running at stage 2 with one completed segment.');

    await phase('converting', 3, 'Converting executable');
    await assertProgress({ label: 'Converting executable', stage: 3, filled: 40 });
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await assertProgress({ label: 'Converting executable', stage: 3, filled: 40 });
    await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].webContents.send('installation:event', { kind: 'log', text: 'Simulated converter is still working.\n' }));
    await page.locator('#install-log').filter({ hasText: 'Simulated converter is still working.' }).waitFor();
    await page.screenshot({ path: path.join(artifacts, '17-progress-converting-reduced-motion.png') });
    await phase('libraries', 4, 'Copying Windows libraries');
    await assertProgress({ label: 'Copying Windows libraries', stage: 4, filled: 60 });
    await phase('finishing', 5, 'Finishing installation');
    await assertProgress({ label: 'Finishing installation', stage: 5, filled: 80 });
    console.log('PASS: converting, libraries and finishing show stage-based progress with no fake percentage; reduced motion keeps the active segment distinguishable.');

    // Reject a late conversion, then leave the replacement IPC promise pending:
    // this proves reset happens locally, even before the next backend event.
    await desktop.evaluate(() => globalThis.progressTest.resolveInstall({ ok: false, error: 'Simulated converter failure.' }));
    await page.locator('#step-2').waitFor({ state: 'visible' });
    await page.locator('#wizard-error').filter({ hasText: 'Simulated converter failure.' }).waitFor();
    await begin(2);
    await phase('converting', 3, 'Converting executable');
    await assertProgress({ label: 'Converting executable', stage: 3, filled: 40 });
    await page.locator('#cancel-install').click();
    await page.locator('#step-2').waitFor({ state: 'visible' });
    assert.equal(await desktop.evaluate(() => globalThis.progressTest.cancellations), 1);
    await page.locator('#wizard-error').filter({ hasText: 'Conversion was cancelled by the progress test.' }).waitFor();
    await begin(3);
    console.log('PASS: failed and cancelled attempts return to review; each retry resets to stage 1, zero completed progress and an empty log.');

    await phase('finishing', 5, 'Finishing installation');
    await assertProgress({ label: 'Finishing installation', stage: 5, filled: 80 });
    await phase('complete', 5, 'Conversion complete', 100, true);
    await assertProgress({ label: 'Conversion complete', stage: 5, filled: 100, complete: true });
    await page.screenshot({ path: path.join(artifacts, '18-progress-complete.png') });
    await desktop.evaluate((_electron, game) => globalThis.progressTest.resolveInstall({ ok: true, value: { game, warning: '' } }), {
      id: 'progress-fixture', title: 'Progress test fixture', directory: path.join(temporary, 'games', 'Progress test fixture'), executable: 'app.exe'
    });
    await page.locator('#step-4').waitFor({ state: 'visible' });
    assert.equal(await page.locator('#complete-title').textContent(), 'Progress test fixture');
    assert.deepEqual(errors, []);
    console.log('PASS: only a complete event fills all five segments and says Complete; successful install response opens the completion screen. No renderer errors.');
  } catch (error) {
    if (page && !page.isClosed()) await page.screenshot({ path: path.join(artifacts, 'progress-failure.png') }).catch(() => {});
    throw error;
  } finally {
    if (desktop) await desktop.close();
    if (path.dirname(temporary) === os.tmpdir() && path.basename(temporary).startsWith('anyps5-progress-ui-')) await fs.rm(temporary, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
