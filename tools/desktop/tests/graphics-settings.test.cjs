const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const { resolutions, defaultGraphics, validateGraphics, rewriteBootConfig, prepareGraphics } = require('../src/graphics-settings.cjs');

test('graphics presets apply to game startup, restore original bytes and preserve external edits', async () => {
  assert.deepEqual(defaultGraphics(), { resolution: '1920x1080' });
  assert.equal(resolutions.length, 6);
  assert.equal(rewriteBootConfig('\ufeffplatform-ps5-video-out-width=3840\r\n', 1920), '\ufeffplatform-ps5-video-out-width=1920\r\n');
  for (const value of [null, [], {}, { resolution: '9999x9999' }, { resolution: 1080 }]) assert.throws(() => validateGraphics(value));
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-graphics-'));
  try {
    const executable = path.join(root, 'app.exe');
    const directory = path.join(root, 'app0', 'Media');
    await fs.mkdir(directory, { recursive: true });
    const config = path.join(directory, 'boot.config');
    const original = '\ufeffgfx-enable-gfx-jobs=1\r\nplatform-ps5-video-out-width=3840\r\nplatform-ps5-standardmode-video-out-width=3840\r\nunrelated=value\r\n';
    await fs.writeFile(config, original);
    for (const preset of resolutions.filter(item => item.width)) {
      const result = await prepareGraphics(executable, { resolution: preset.value });
      assert.equal(result.source, 'unity');
      assert.equal(await fs.readFile(config, 'utf8'), original.replaceAll('=3840', '=' + preset.width));
      assert.equal(JSON.parse(await fs.readFile(config + '.anyps5-resolution.json', 'utf8')).original, original);
    }
    await prepareGraphics(executable, { resolution: 'native' });
    assert.equal(await fs.readFile(config, 'utf8'), original);
    await prepareGraphics(executable, defaultGraphics());
    const changed = (await fs.readFile(config, 'utf8')) + 'external-edit=keep\r\n';
    await fs.writeFile(config, changed);
    await assert.rejects(prepareGraphics(executable, { resolution: '1280x720' }), /preserved/);
    assert.equal(await fs.readFile(config, 'utf8'), changed);
    const unknown = path.join(root, 'unknown', 'app.exe');
    const unsupported = await prepareGraphics(unknown, defaultGraphics());
    assert.equal(unsupported.source, 'native');
    assert.match(unsupported.message, /unchanged/);
    const recovery = JSON.parse(await fs.readFile(config + '.anyps5-resolution.json', 'utf8'));
    await fs.writeFile(config, recovery.previousApplied);
    await prepareGraphics(executable, { resolution: 'native' });
    assert.equal(await fs.readFile(config, 'utf8'), original);
  } finally { await fs.rm(root, { recursive: true, force: true }); }
});
