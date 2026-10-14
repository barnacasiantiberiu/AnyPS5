const fs = require('node:fs');
const fsp = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const assert = require('node:assert/strict');

// Run with Node: the parent launches stock Electron with no debugger or scheduling flags.
// Keeping cleanup in the parent allows Chromium to release its profile locks first.
if (!process.versions.electron) {
  const { spawn, execFileSync } = require('node:child_process');
  (async () => {
    const temporaryParent = await fsp.realpath(os.tmpdir());
    const temporary = await fsp.mkdtemp(path.join(temporaryParent, 'anyps5-native-presentation-'));
    try {
      const childEnvironment = { ...process.env, ANYPS5_NATIVE_ROOT: temporary, ANYPS5_NATIVE_PROFILE: path.join(temporary, 'profile') };
      delete childEnvironment.ELECTRON_RUN_AS_NODE;
      const child = spawn(require('electron'), [__filename], {
        stdio: 'inherit', windowsHide: false,
        env: childEnvironment
      });
      const timeout = setTimeout(() => {
        console.error('FAIL: native presentation child timed out.');
        if (!child.pid) return;
        if (process.platform === 'win32') {
          try { execFileSync('taskkill', ['/pid', String(child.pid), '/t', '/f'], { windowsHide: true, stdio: 'ignore' }); }
          catch { child.kill(); }
        } else child.kill('SIGKILL');
      }, 30000);
      const code = await new Promise((resolve, reject) => {
        child.once('error', reject);
        child.once('exit', (code, signal) => resolve(signal || code === null ? 1 : code));
      }).finally(() => clearTimeout(timeout));
      process.exitCode = code;
    } finally {
      const actual = await fsp.realpath(temporary);
      assert.equal(path.dirname(actual).toLowerCase(), temporaryParent.toLowerCase());
      assert.ok(path.basename(actual).startsWith('anyps5-native-presentation-'));
      await fsp.rm(actual, { recursive: true, force: true, maxRetries: 8, retryDelay: 150 });
    }
  })().catch(error => { console.error('FAIL: native test launcher:', error); process.exitCode = 1; });
} else {
const { app, BrowserWindow } = require('electron');
const temporary = process.env.ANYPS5_NATIVE_ROOT;
const profile = process.env.ANYPS5_NATIVE_PROFILE;
if (!temporary || !profile) {
  console.error('Run this test with node tests/native-presentation.cjs so its parent can clean up the native profile after exit.');
  app.exit(1);
} else {
fs.mkdirSync(profile, { recursive: true });
process.env.ANYPS5_DESKTOP_TEST_DATA = profile;
app.setPath('sessionData', path.join(temporary, 'session'));
const mainFile = process.env.ANYPS5_NATIVE_MAIN
  ? path.resolve(process.env.ANYPS5_NATIVE_MAIN)
  : path.resolve(__dirname, '..', 'src', 'main.cjs');
const artifacts = path.resolve(__dirname, '..', 'artifacts');
const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
let finishing = false;
let handledWindow = false;
const timeout = setTimeout(() => finish(new Error('Native presentation test timed out.')), 25000);
function finish(error) {
  if (finishing) return;
  finishing = true;
  clearTimeout(timeout);
  if (error) console.error('FAIL: native presentation:', error.stack || error.message || error);
  app.exit(error ? 1 : 0);
}

function compare(first, second) {
  assert.deepEqual(second.getSize(), first.getSize(), 'The window dimensions changed between captures.');
  const { width, height } = first.getSize();
  const crop = {
    x: Math.round(width * .74), y: Math.round(height * .34),
    width: Math.round(width * .22), height: Math.round(height * .42)
  };
  const a = first.crop(crop).toBitmap();
  const b = second.crop(crop).toBitmap();
  assert.equal(a.length, b.length);
  let changedPixels = 0, highContrastPixels = 0, maxChannelDelta = 0;
  for (let offset = 0; offset < a.length; offset += 4) {
    let delta = 0;
    for (let channel = 0; channel < 3; channel++) delta = Math.max(delta, Math.abs(a[offset + channel] - b[offset + channel]));
    if (delta > 0) changedPixels++;
    if (delta >= 12) highContrastPixels++;
    maxChannelDelta = Math.max(maxChannelDelta, delta);
  }
  return { crop, totalPixels: a.length / 4, changedPixels, highContrastPixels, maxChannelDelta };
}

async function verify(window) {
  window.show();
  await delay(1500);
  console.log('Native window:', JSON.stringify({ visible: window.isVisible(), minimized: window.isMinimized(), fullscreen: window.isFullScreen() }));
  assert.ok(window.isVisible() && !window.isMinimized(), 'The native window must be visible.');
  const first = await window.webContents.capturePage();
  await delay(2000);
  const second = await window.webContents.capturePage();
  await fsp.mkdir(artifacts, { recursive: true });
  await fsp.writeFile(path.join(artifacts, '15-native-motion-before.png'), first.toPNG());
  await fsp.writeFile(path.join(artifacts, '16-native-motion-after.png'), second.toPNG());
  const result = compare(first, second);
  // Read state only after capturing presentation, so no renderer evaluation can wake it first.
  const renderer = await window.webContents.executeJavaScript("({ hidden: document.hidden, reducedMotion: matchMedia('(prefers-reduced-motion: reduce)').matches, canvas: { ...document.getElementById('ambient-canvas').dataset } })");
  console.log('Native presentation:', JSON.stringify({
    main: mainFile, fullscreen: window.isFullScreen(), visible: window.isVisible(),
    backgroundThrottling: window.webContents.backgroundThrottling, renderer, ...result
  }));
  assert.ok(result.highContrastPixels >= 100,
    'Expected at least 100 background pixels to change by 12/255 or more over two seconds; got ' + result.highContrastPixels + '.');
  console.log('PASS: native compositor presentation changes without Playwright, CDP, inspector or scheduling overrides.');
}

app.on('browser-window-created', (_event, window) => {
  if (handledWindow) return;
  handledWindow = true;
  window.webContents.once('did-fail-load', (_event, code, description) => finish(new Error('Page load failed: ' + code + ' ' + description)));
  window.webContents.once('render-process-gone', (_event, details) => finish(new Error('Renderer exited: ' + details.reason)));
  window.webContents.once('did-finish-load', () => verify(window).then(() => finish(), finish));
});
process.on('uncaughtException', finish);
process.on('unhandledRejection', finish);
try { require(mainFile); } catch (error) { finish(error); }

}
}
