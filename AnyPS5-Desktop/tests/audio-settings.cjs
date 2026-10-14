const { _electron: electron } = require('playwright');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');

// Real keyboard controls, audio preview and settings IPC, isolated from the user profile.
(async () => {
  const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-audio-settings-'));
  const profile = path.join(temporary, 'profile');
  const initialAudio = { enabled: true, musicVolume: 23, effectsVolume: 41 };
  let desktop; let page;
  const errors = [];
  try {
    await fs.mkdir(profile);
    await fs.writeFile(path.join(profile, 'library.json'), JSON.stringify({ settings: { audio: initialAudio }, games: [] }));
    const launch = async () => {
      desktop = await electron.launch({
        executablePath: process.env.ANYPS5_TEST_APP || require('electron'),
        args: process.env.ANYPS5_TEST_APP ? [] : [root],
        env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: profile }
      });
      page = await desktop.firstWindow(); page.setDefaultTimeout(10000);
      page.on('pageerror', error => errors.push(error.message));
      await page.waitForFunction(() => window.AnyPS5Audio?.getState().configured);
      await page.waitForFunction(() => document.activeElement.dataset.navKey === 'library');
      assert.equal(await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].isVisible()), false, 'The isolated test window must remain hidden and silent.');
      await page.waitForFunction(() => !window.AnyPS5Audio.getState().nativeVisible);
    };
    const focus = () => page.evaluate(() => document.activeElement.id || document.activeElement.dataset.navKey || document.activeElement.type);
    const key = value => page.keyboard.press(value);
    const focused = expected => page.waitForFunction(value => (document.activeElement.id || document.activeElement.dataset.navKey || document.activeElement.type) === value, expected);
    const liveAudio = () => page.evaluate(() => window.AnyPS5Audio.getState().settings);
    const savedAudio = async () => (await page.evaluate(() => window.desktop.state())).settings.audio;
    const toward = async (target, direction, maximum = 6) => {
      const route = [await focus()];
      for (let index = 0; index < maximum && await focus() !== target; index++) { await key(direction); route.push(await focus()); }
      assert.equal(await focus(), target, direction + ' route: ' + route.join(' -> '));
    };
    const openAudio = async () => {
      if (await focus() === 'library') { await key('ArrowUp'); await focused('nav-library'); await toward('settings-open', 'ArrowRight'); }
      await focused('settings-open'); await key('Enter'); await focused('settings-tab-players');
      await key('ArrowRight'); await focused('settings-tab-engine');
      await key('ArrowRight'); await focused('settings-tab-audio');
      assert.equal(await page.locator('#settings-tab-audio').getAttribute('aria-selected'), 'true');
      assert.ok(await page.locator('#settings-panel-audio').isVisible());
      // Native Tab remains available alongside arrow navigation and enters the first field.
      await key('Tab'); await focused('audio-enabled');
    };
    const volumes = async (music, effects) => {
      assert.equal(await page.locator('#music-volume').inputValue(), String(music));
      assert.equal(await page.locator('#effects-volume').inputValue(), String(effects));
      assert.equal(await page.locator('#music-volume-value').textContent(), music + '%');
      assert.equal(await page.locator('#effects-volume-value').textContent(), effects + '%');
    };

    await launch();
    assert.deepEqual(await liveAudio(), initialAudio);
    await openAudio(); await volumes(23, 41);
    await key('ArrowDown'); await focused('music-volume');
    await key('Enter'); await key('ArrowRight'); await volumes(24, 41);
    assert.equal(await page.locator('#music-volume').getAttribute('data-keyboard-editing'), 'true');
    assert.deepEqual(await liveAudio(), { ...initialAudio, musicVolume: 24 }, 'Slider changes must preview immediately on the music channel.');
    assert.deepEqual(await savedAudio(), initialAudio, 'Live preview must not save the settings draft.');
    await key('Escape'); await focused('music-volume'); await volumes(23, 41);
    assert.equal(await page.locator('#music-volume').getAttribute('data-keyboard-editing'), null);
    assert.deepEqual(await liveAudio(), initialAudio, 'Esc while editing must revert the slider and its live preview.');
    assert.ok(await page.locator('#settings-dialog').isVisible());
    await key('Enter'); await key('ArrowRight'); await key('ArrowRight'); await key('Enter');
    await key('ArrowDown'); await focused('effects-volume');
    await key('Enter'); await key('ArrowLeft'); await key('ArrowLeft'); await key('Enter');
    await volumes(25, 39);
    assert.deepEqual(await liveAudio(), { enabled: true, musicVolume: 25, effectsVolume: 39 }, 'Music and menu effects must be independently adjustable.');
    assert.ok(await page.locator('#settings-dialog').isVisible(), 'Enter must end slider editing without submitting the form.');
    await key('Escape'); await page.locator('#settings-dialog').waitFor({ state: 'hidden' }); await focused('settings-open');
    await page.waitForFunction(expected => JSON.stringify(window.AnyPS5Audio.getState().settings) === JSON.stringify(expected), initialAudio);
    assert.deepEqual(await savedAudio(), initialAudio);
    console.log('PASS: Audio tab keyboard navigation, slider edit/confirm/revert, separate live channel previews, and cancel restoration.');

    await openAudio(); await volumes(23, 41);
    await key('Enter');
    assert.equal(await page.locator('#audio-enabled').isChecked(), false);
    assert.ok(await page.locator('#music-volume').isDisabled()); assert.ok(await page.locator('#effects-volume').isDisabled());
    assert.deepEqual(await liveAudio(), { ...initialAudio, enabled: false });
    await key('ArrowDown'); await focused('submit');
    await key('Enter'); await page.locator('#settings-dialog').waitFor({ state: 'hidden' }); await focused('settings-open');
    assert.deepEqual(await savedAudio(), { ...initialAudio, enabled: false }, 'Master audio disable must be saved without losing either volume.');

    await openAudio();
    assert.equal(await page.locator('#audio-enabled').isChecked(), false);
    await key('Enter'); await key('ArrowDown'); await focused('music-volume');
    await key('Enter'); await key('Home'); await key('Enter');
    await key('ArrowDown'); await focused('effects-volume');
    await key('Enter'); await key('End'); await key('ArrowLeft'); await key('Enter');
    await volumes(0, 99);
    await key('ArrowDown'); await focused('submit'); await key('Enter');
    await page.locator('#settings-dialog').waitFor({ state: 'hidden' });
    const saved = { enabled: true, musicVolume: 0, effectsVolume: 99 };
    assert.deepEqual(await savedAudio(), saved);
    assert.deepEqual(await liveAudio(), saved);
    const stored = JSON.parse(await fs.readFile(path.join(profile, 'library.json'), 'utf8'));
    assert.deepEqual(stored.settings.audio, saved);
    console.log('PASS: master toggle disables sliders, disabled controls are skipped, volume bounds work, and explicit Save persists preferences.');

    await desktop.close(); desktop = null;
    await launch();
    assert.deepEqual(await savedAudio(), saved); assert.deepEqual(await liveAudio(), saved);
    await openAudio(); await volumes(0, 99);
    assert.equal(await page.locator('#audio-enabled').isChecked(), true);
    const playback = await page.evaluate(() => window.AnyPS5Audio.getState());
    assert.notEqual(playback.contextState, 'running', 'A hidden isolated window must not play menu audio.');
    assert.equal(playback.activeEffects, 0);
    assert.deepEqual(errors, []);
    console.log('PASS: separate audio preferences survive a real restart and initialize the live module; hidden window stays silent. No renderer errors.');
  } finally {
    if (desktop) await desktop.close();
    if (path.dirname(temporary) === os.tmpdir() && path.basename(temporary).startsWith('anyps5-audio-settings-')) await fs.rm(temporary, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
