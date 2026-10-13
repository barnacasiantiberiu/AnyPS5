const { _electron: electron } = require('playwright');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');

// Exercise actual Web Audio DSP with speakers muted. Packaged mode reads the
// module from that application's ASAR; the stock Electron harness stays isolated.
(async () => {
  const tempParent = path.resolve(os.tmpdir());
  const temporary = await fs.mkdtemp(path.join(tempParent, 'anyps5-audio-test-'));
  let desktop;
  try {
    const packagedApp = process.env.ANYPS5_TEST_APP && path.resolve(process.env.ANYPS5_TEST_APP);
    if (packagedApp) assert.ok((await fs.stat(packagedApp)).isFile(), 'ANYPS5_TEST_APP must identify the packaged executable.');
    const audioPath = packagedApp
      ? path.join(path.dirname(packagedApp), 'resources', 'app.asar', 'src', 'ui', 'audio.js')
      : path.join(__dirname, '..', 'src', 'ui', 'audio.js');
    const main = path.join(temporary, 'main.cjs');
    await fs.writeFile(main, [
      "const { app, BrowserWindow } = require('electron');",
      "app.setPath('userData', process.env.ANYPS5_AUDIO_PROFILE);",
      "globalThis.audioTestSource = require('node:fs').readFileSync(process.env.ANYPS5_AUDIO_SOURCE, 'utf8');",
      "app.whenReady().then(async () => {",
      "  const window = new BrowserWindow({ width: 640, height: 400, show: true, webPreferences: { contextIsolation: true, nodeIntegration: false, backgroundThrottling: false, autoplayPolicy: 'no-user-gesture-required' } });",
      "  window.removeMenu(); window.webContents.setAudioMuted(true);",
      "  await window.loadURL('data:text/html,<title>AnyPS5 audio test</title><button>Audio lifecycle test - speakers muted</button>');",
      "  window.show(); window.focus();",
      "});",
      "app.on('window-all-closed', () => app.quit());"
    ].join('\n'));
    desktop = await electron.launch({
      executablePath: require('electron'),
      args: [main],
      env: { ...process.env, ANYPS5_AUDIO_PROFILE: path.join(temporary, 'profile'), ANYPS5_AUDIO_SOURCE: audioPath }
    });
    const page = await desktop.firstWindow();
    page.setDefaultTimeout(12000);
    assert.equal(await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].webContents.isAudioMuted()), true);
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.evaluate(() => {
      const listeners = new Set();
      window.desktop = {
        windowVisibility: async () => true,
        onWindowVisibility: listener => { listeners.add(listener); return () => listeners.delete(listener); }
      };
      window.__audioTestVisibility = value => { for (const listener of listeners) listener(value); };
      window.__audioTestListenerCount = () => listeners.size;
      const NativeContext = window.AudioContext;
      const connect = AudioNode.prototype.connect;
      const analysers = new Map();
      // Tap whatever final node feeds the destination, without requiring a
      // particular synthesis graph, instrument count, or limiter arrangement.
      AudioNode.prototype.connect = function (target, ...ports) {
        if (target !== this.context.destination) return connect.call(this, target, ...ports);
        let analyser = analysers.get(this.context);
        if (!analyser) {
          analyser = this.context.createAnalyser(); analyser.fftSize = 2048;
          analysers.set(this.context, analyser); connect.call(analyser, target);
        }
        connect.call(this, analyser, ...ports);
        return target;
      };
      const sources = new Set();
      const trackSource = source => {
        const start = source.start.bind(source);
        source.start = (...args) => { const result = start(...args); sources.add(source); return result; };
        source.addEventListener('ended', () => sources.delete(source));
        return source;
      };
      window.AudioContext = class extends NativeContext {
        createOscillator() { return trackSource(super.createOscillator()); }
        createBufferSource() { return trackSource(super.createBufferSource()); }
      };
      window.__audioTestSources = () => sources.size;
      window.__audioTestSignal = () => {
        let peak = 0, energy = 0, count = 0, finite = true;
        for (const analyser of analysers.values()) {
          const samples = new Float32Array(analyser.fftSize);
          analyser.getFloatTimeDomainData(samples);
          for (const value of samples) { finite &&= Number.isFinite(value); peak = Math.max(peak, Math.abs(value)); energy += value * value; count++; }
        }
        return { peak, rms: count ? Math.sqrt(energy / count) : 0, finite };
      };
      window.__audioTestCapture = (milliseconds = 900) => new Promise(resolve => {
        let peak = 0, rms = 0, frames = 0, finite = true;
        const sample = () => { const signal = __audioTestSignal(); peak = Math.max(peak, signal.peak); rms = Math.max(rms, signal.rms); finite &&= signal.finite; frames++; };
        sample(); const timer = setInterval(sample, 15);
        setTimeout(() => { clearInterval(timer); sample(); resolve({ peak, rms, frames, finite }); }, milliseconds);
      });
    });
    const source = await desktop.evaluate(() => globalThis.audioTestSource);
    assert.ok(source.includes('AnyPS5Audio'), 'The selected audio module must be loaded successfully.');
    await page.addScriptTag({ content: source });
    console.log('Testing ' + (packagedApp ? 'packaged ASAR' : 'workspace') + ' audio module: ' + audioPath);
    const boundedSignal = (signal, label) => {
      assert.equal(signal.finite, true, label + ': every measured sample must be finite.');
      assert.ok(signal.frames >= 3, label + ': sample multiple DSP frames.');
      assert.ok(signal.peak > .00005 && signal.rms > .00001, label + ': expected a nonzero signal.');
      assert.ok(signal.peak < .95 && signal.rms < .5, label + ': signal must remain bounded below clipping.');
    };
    const waitRunning = () => page.waitForFunction(() => AnyPS5Audio.getState().contextState === 'running');
    const waitSuspended = () => page.waitForFunction(() => AnyPS5Audio.getState().contextState === 'suspended');
    await page.evaluate(() => {
      window.dispatchEvent(new Event('focus'));
      AnyPS5Audio.configure({ enabled: false, musicVolume: 25, effectsVolume: 45 });
    });
    assert.equal((await page.evaluate(() => AnyPS5Audio.getState())).contextCount, 0, 'Saved mute must prevent graph creation and autoplay.');
    assert.equal(await page.evaluate(() => AnyPS5Audio.play('confirm')), false);
    await page.evaluate(() => AnyPS5Audio.configure({ enabled: true, musicVolume: 25, effectsVolume: 45 }));
    await waitRunning();
    await page.waitForFunction(() => __audioTestSignal().peak > .00005);
    let state = await page.evaluate(() => AnyPS5Audio.getState());
    assert.equal(state.contextCount, 1);
    assert.ok(state.musicVoices > 0, 'A music voice must exist; its arrangement and count are not part of this contract.');
    assert.equal(state.lastError, '');
    boundedSignal(await page.evaluate(() => __audioTestCapture()), 'Default-volume music');
    await page.evaluate(() => AnyPS5Audio.configure({ musicVolume: 100, effectsVolume: 0 }));
    await page.waitForTimeout(9000);
    const loudMusic = await page.evaluate(() => __audioTestCapture(1000)); boundedSignal(loudMusic, 'Full-volume ambient music');
    console.log('Full-volume ambient samples: ' + JSON.stringify(loudMusic));
    const initialSources = await page.evaluate(() => __audioTestSources());
    await page.waitForFunction(() => AnyPS5Audio.getState().musicPhrases >= 2, { }, { timeout: 15000 });
    boundedSignal(await page.evaluate(() => __audioTestCapture(8500)), 'Chord transition with melody at full volume');
    assert.equal(await page.evaluate(() => __audioTestSources()), initialSources, 'Evolving music must reuse a bounded source pool.');
    console.log('PASS: ambient phrases advance through a full chord crossfade with bounded output and no accumulating sources.');
    await page.evaluate(() => AnyPS5Audio.configure({ effectsVolume: 45 }));
    console.log('PASS: saved mute prevents autoplay; music at the default 25% produces finite, nonzero, bounded samples.');

    await page.evaluate(() => AnyPS5Audio.configure({ musicVolume: 0 }));
    await page.waitForFunction(() => __audioTestSignal().peak < .00005);
    const steadyMusicSources = await page.evaluate(() => __audioTestSources());
    for (const cue of ['navigate', 'confirm', 'back', 'open', 'error', 'success']) {
      const result = await page.evaluate(async name => {
        const signal = __audioTestCapture();
        const played = await AnyPS5Audio.play(name);
        return { played, signal: await signal };
      }, cue);
      assert.equal(result.played, true, cue);
      boundedSignal(result.signal, 'Default-volume ' + cue);
      await page.waitForFunction(() => AnyPS5Audio.getState().activeEffects === 0);
      assert.equal(await page.evaluate(() => __audioTestSources()), steadyMusicSources, 'Finished cues must release their native audio sources.');
    }
    const beforeRepeat = await page.evaluate(() => AnyPS5Audio.getState().effectsPlayed);
    await page.evaluate(() => Promise.all(Array.from({ length: 40 }, () => AnyPS5Audio.play('navigate'))));
    state = await page.evaluate(() => AnyPS5Audio.getState());
    assert.ok(state.effectsPlayed - beforeRepeat <= 1, 'Arrow repeats must be coalesced.');
    assert.ok(state.activeEffects <= 12);
    console.log('PASS: all six cues at default effects volume produce bounded signals and release sources; rapid repeats are coalesced.');

    // Repeated suspension must reuse the graph and discard any interrupted cue.
    for (let cycle = 0; cycle < 3; cycle++) {
      await page.evaluate(() => AnyPS5Audio.configure({ enabled: false }));
      await waitSuspended();
      assert.equal(await page.evaluate(() => AnyPS5Audio.play('success')), false);
      assert.equal((await page.evaluate(() => AnyPS5Audio.getState())).activeEffects, 0);
      await page.evaluate(() => AnyPS5Audio.configure({ enabled: true, musicVolume: 25 }));
      await waitRunning();
      await page.waitForFunction(() => __audioTestSignal().peak > .00005);
      state = await page.evaluate(() => AnyPS5Audio.getState());
      assert.equal(state.contextCount, 1, 'Mute/resume must not accumulate AudioContexts.');
      assert.equal(state.lastError, '');
      assert.equal(await page.evaluate(() => __audioTestSources()), steadyMusicSources, 'Mute/resume must not accumulate music or abandoned cue sources.');
      await page.evaluate(() => AnyPS5Audio.play('success'));
    }
    await page.evaluate(() => __audioTestVisibility(false));
    await waitSuspended();
    assert.equal((await page.evaluate(() => AnyPS5Audio.getState())).activeEffects, 0, 'Hiding the app must release an interrupted cue.');
    await page.evaluate(() => { AnyPS5Audio.suspend('game'); __audioTestVisibility(true); });
    assert.equal((await page.evaluate(() => AnyPS5Audio.getState())).contextState, 'suspended', 'Native restore must not override explicit game pause.');
    await page.evaluate(() => AnyPS5Audio.release('game')); await waitRunning();
    await page.evaluate(() => window.dispatchEvent(new Event('blur'))); await waitSuspended();
    await page.evaluate(() => window.dispatchEvent(new Event('focus'))); await waitRunning();
    state = await page.evaluate(() => AnyPS5Audio.getState());
    assert.equal(state.contextCount, 1);
    const normalized = await page.evaluate(() => AnyPS5Audio.configure({ musicVolume: -8, effectsVolume: 120 }));
    assert.equal(normalized.musicVolume, 0); assert.equal(normalized.effectsVolume, 100);
    await page.evaluate(() => AnyPS5Audio.configure({ musicVolume: 0, effectsVolume: 0 }));
    await waitSuspended();
    assert.equal(await page.evaluate(() => AnyPS5Audio.play('confirm')), false, 'Both channels at zero must stop DSP and reject cues.');
    console.log('PASS: repeated mute/resume, visibility, focus and explicit pause gates reuse one context without accumulating sources.');

    await page.evaluate(() => window.dispatchEvent(new Event('pagehide')));
    await page.waitForFunction(() => AnyPS5Audio.getState().contextState === 'closed');
    assert.equal((await page.evaluate(() => AnyPS5Audio.getState())).disposed, true);
    assert.equal((await page.evaluate(() => AnyPS5Audio.getState())).activeEffects, 0);
    assert.equal(await page.evaluate(() => __audioTestListenerCount()), 0);
    assert.equal(await page.evaluate(() => AnyPS5Audio.play('confirm')), false);
    assert.deepEqual(errors, []);
    console.log('PASS: pagehide closes audio and unsubscribes listeners. This measures generated samples, not musical quality or OS speaker output.');
  } finally {
    if (desktop) await desktop.close();
    assert.equal(path.dirname(path.resolve(temporary)), tempParent);
    assert.ok(path.basename(temporary).startsWith('anyps5-audio-test-'));
    await fs.rm(temporary, { recursive: true, force: true, maxRetries: 5, retryDelay: 150 });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
