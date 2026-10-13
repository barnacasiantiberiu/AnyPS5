const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const { withMountedIso, inspectIso, resolveIsoRequest, checkIsoEntry } = require('../src/iso-source.cjs');

const ELF = Buffer.from([127, 69, 76, 70, 2, 1, 1, 9]);
const SELF = Buffer.from([0x54, 0x14, 0xf5, 0xee, 0, 1, 1, 0x12]);
const deferred = () => {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return { promise, resolve };
};
async function fixture(t) {
  const parent = path.resolve(os.tmpdir());
  const temporary = await fs.mkdtemp(path.join(parent, 'anyps5-iso-unit-'));
  t.after(async () => {
    assert.equal(path.dirname(path.resolve(temporary)), parent);
    assert.ok(path.basename(temporary).startsWith('anyps5-iso-unit-'));
    await fs.rm(temporary, { recursive: true, force: true });
  });
  const image = path.join(temporary, "Game [test] & 'literal'.ISO");
  const volume = path.join(temporary, 'volume');
  const game = path.join(volume, 'Wrapper', 'Game');
  await fs.writeFile(image, 'synthetic ISO bytes: never actually mounted');
  await fs.mkdir(path.join(game, 'sce_module'), { recursive: true });
  await fs.mkdir(path.join(game, 'sce_sys'));
  await fs.writeFile(path.join(game, 'eboot.bin'), ELF);
  await fs.writeFile(path.join(game, 'sce_module', 'module.prx'), ELF);
  await fs.writeFile(path.join(game, 'level.data'), 'full game resources');
  await fs.writeFile(path.join(game, 'sce_sys', 'param.json'), JSON.stringify({ titleId: 'ISO000001', localizedParameters: { 'en-US': { titleName: 'ISO Test Game' } } }));
  const calls = [];
  const adapter = {
    mount: async file => { calls.push(['mount', file]); return { root: volume, owned: true, devicePath: 'mock-device' }; },
    unmount: async (file, mount) => { calls.push(['unmount', file, mount.devicePath]); }
  };
  return { temporary, image, volume, game, calls, adapter };
}
test('owned mount stays alive for callback and detaches afterward without changing image bytes', async t => {
  const f = await fixture(t);
  const before = await fs.readFile(f.image);
  const value = await withMountedIso(f.image, async root => {
    assert.equal(root, f.volume);
    assert.equal(f.calls.length, 1);
    assert.equal(await fs.readFile(path.join(f.game, 'level.data'), 'utf8'), 'full game resources');
    return { complete: true };
  }, { adapter: f.adapter });
  assert.deepEqual(value, { complete: true });
  assert.deepEqual(f.calls, [['mount', f.image], ['unmount', f.image, 'mock-device']]);
  assert.deepEqual(await fs.readFile(f.image), before);
});
test('a pre-existing mount is reused and left attached on success or callback failure', async t => {
  const f = await fixture(t);
  f.adapter.mount = async () => ({ root: f.volume, owned: false, devicePath: 'existing-device' });
  f.adapter.unmount = async () => assert.fail('pre-existing mount must not be detached');
  assert.equal(await withMountedIso(f.image, () => 42, { adapter: f.adapter }), 42);
  await assert.rejects(withMountedIso(f.image, () => { throw new Error('callback failure'); }, { adapter: f.adapter }), /callback failure/);
});
test('inspection returns metadata and durable relative paths instead of temporary drive paths', async t => {
  const f = await fixture(t);
  const result = await inspectIso(f.image, { adapter: f.adapter });
  assert.equal(result.source, f.image);
  assert.equal(result.sourceType, 'iso');
  assert.equal(result.sourceFolder, 'Wrapper\\Game');
  assert.equal(result.title, 'ISO Test Game');
  assert.equal(result.titleId, 'ISO000001');
  assert.equal(result.entry, 'Wrapper\\Game\\eboot.bin');
  assert.deepEqual(result.executables, [result.entry]);
  assert.match(result.note, /read-only/);
  assert.equal(f.calls.at(-1)[0], 'unmount');
});
test('encrypted and PKG-only ISO layouts produce distinct diagnostics rather than attempting extraction', async t => {
  const f = await fixture(t);
  await fs.writeFile(path.join(f.game, 'eboot.bin'), SELF);
  const encrypted = await inspectIso(f.image, { adapter: f.adapter });
  assert.equal(encrypted.entry, '');
  assert.equal(encrypted.encrypted, true);
  assert.match(encrypted.diagnostic, /SELF/);
  await fs.unlink(path.join(f.game, 'eboot.bin'));
  await fs.writeFile(path.join(f.game, 'game.pkg'), 'package fixture');
  const pkg = await inspectIso(f.image, { adapter: f.adapter });
  assert.equal(pkg.entry, '');
  assert.match(pkg.diagnostic, /PKG packages.*not supported/);
});
test('resolve maps ISO-relative entries to a checked folder request and passes ordinary folders unchanged', async t => {
  const f = await fixture(t);
  const request = { source: f.image, sourceType: 'iso', entry: 'Wrapper\\Game\\eboot.bin', title: 'Kept title' };
  const result = await resolveIsoRequest(request, async (resolved, details) => {
    assert.equal(resolved.source, f.game);
    assert.equal(resolved.sourceType, 'folder');
    assert.equal(resolved.entry, path.join(f.game, 'eboot.bin'));
    assert.equal(resolved.title, request.title);
    assert.equal(details.volumeRoot, f.volume);
    assert.equal(f.calls.length, 1);
    return 'done';
  }, { adapter: f.adapter });
  assert.equal(result, 'done');
  assert.equal(await checkIsoEntry(request, { adapter: f.adapter }), request.entry);
  const folder = { source: f.game, entry: path.join(f.game, 'eboot.bin') };
  await resolveIsoRequest(folder, actual => assert.equal(actual, folder), { adapter: { mount: () => assert.fail('folder should not mount') } });
});
test('absolute, parent traversal and alternate stream entries are rejected before mounting', async t => {
  const f = await fixture(t);
  for (const entry of ['..\\outside.elf', 'Wrapper\\..\\eboot.bin', 'C:\\outside.elf', 'C:outside.elf', '\\\\server\\share\\file.elf', '/etc/file.elf', 'Wrapper\\Game\\eboot.bin:stream', 'Wrapper\\Game\\', 'Wrapper\\.\\eboot.bin']) {
    await assert.rejects(resolveIsoRequest({ source: f.image, sourceType: 'iso', entry }, () => assert.fail('unsafe entry callback'), { adapter: f.adapter }), /relative path|invalid or parent-directory/);
  }
  assert.deepEqual(f.calls, []);
});
test('entry realpath cannot escape the mounted volume through a directory junction', async t => {
  const f = await fixture(t);
  const outside = path.join(f.temporary, 'outside');
  await fs.mkdir(outside);
  await fs.writeFile(path.join(outside, 'escape.elf'), ELF);
  await fs.symlink(outside, path.join(f.volume, 'escape'), process.platform === 'win32' ? 'junction' : 'dir');
  await assert.rejects(resolveIsoRequest({ source: f.image, entry: 'escape\\escape.elf' }, () => assert.fail('outside callback'), { adapter: f.adapter }), /resolves outside/);
  assert.equal(f.calls.at(-1)[0], 'unmount');
});
test('missing, empty, relative, wrong-extension and directory sources fail before mounting', async t => {
  const f = await fixture(t);
  const empty = path.join(f.temporary, 'empty.iso');
  const directory = path.join(f.temporary, 'directory.iso');
  await fs.writeFile(empty, '');
  await fs.mkdir(directory);
  for (const invalid of ['relative.iso', path.join(f.temporary, 'missing.iso'), path.join(f.temporary, 'wrong.txt'), empty, directory]) {
    await assert.rejects(withMountedIso(invalid, () => assert.fail('invalid callback'), { adapter: f.adapter }), /absolute path|existing regular file|empty/);
  }
  assert.deepEqual(f.calls, []);
});
test('cancellation during mounting waits for the returned ownership then detaches without running callback', async t => {
  const f = await fixture(t);
  const started = deferred(), mountFinished = deferred();
  const controller = new AbortController();
  f.adapter.mount = async () => { started.resolve(); await mountFinished.promise; return { root: f.volume, owned: true, devicePath: 'late-mount' }; };
  const pending = withMountedIso(f.image, () => assert.fail('aborted callback'), { adapter: f.adapter, signal: controller.signal });
  const rejected = assert.rejects(pending, /cancelled/);
  await started.promise;
  controller.abort();
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(f.calls, [], 'No detach may race the still-running mount command.');
  mountFinished.resolve();
  await rejected;
  assert.deepEqual(f.calls, [['unmount', f.image, 'late-mount']]);
});
test('cancellation before mounting and during callback always preserves mount ownership rules', async t => {
  const f = await fixture(t);
  const alreadyAborted = new AbortController(); alreadyAborted.abort();
  await assert.rejects(withMountedIso(f.image, () => {}, { adapter: f.adapter, signal: alreadyAborted.signal }), /cancelled/);
  assert.deepEqual(f.calls, []);
  const controller = new AbortController();
  await assert.rejects(withMountedIso(f.image, async () => { controller.abort(); throw new Error('conversion cancelled before commit'); }, { adapter: f.adapter, signal: controller.signal }), /cancelled/);
  assert.equal(f.calls.at(-1)[0], 'unmount');
});
test('same-image operations reject while callback or detach is pending, then release the lock', async t => {
  const f = await fixture(t);
  const callbackStarted = deferred(), finishCallback = deferred(), detachStarted = deferred(), finishDetach = deferred();
  f.adapter.unmount = async () => { detachStarted.resolve(); await finishDetach.promise; };
  const pending = withMountedIso(f.image, async () => { callbackStarted.resolve(); await finishCallback.promise; }, { adapter: f.adapter });
  await callbackStarted.promise;
  await assert.rejects(withMountedIso(f.image, () => {}, { adapter: f.adapter }), /already being used/);
  finishCallback.resolve();
  await detachStarted.promise;
  await assert.rejects(withMountedIso(f.image, () => {}, { adapter: f.adapter }), /already being used/);
  finishDetach.resolve();
  await pending;
  assert.equal(await withMountedIso(f.image, () => 'available', { adapter: f.adapter }), 'available');
});
test('mount failure propagates and successful callback is not reported as failed if detach fails', async t => {
  const f = await fixture(t);
  const events = [];
  f.adapter.mount = async () => { throw new Error('mount denied'); };
  await assert.rejects(withMountedIso(f.image, () => {}, { adapter: f.adapter }), /mount denied/);
  f.adapter.mount = async () => ({ root: f.volume, owned: true, devicePath: 'device' });
  f.adapter.unmount = async () => { throw new Error('device busy'); };
  const success = await withMountedIso(f.image, () => ({ complete: true }), { adapter: f.adapter, onEvent: event => events.push(event) });
  assert.equal(success.complete, true);
  assert.match(success.cleanupWarning, /device busy/);
  assert.ok(events.some(event => event.kind === 'warning'));
  const original = new Error('conversion failed');
  await assert.rejects(withMountedIso(f.image, () => { throw original; }, { adapter: f.adapter }), error => {
    assert.equal(error, original);
    assert.match(error.cleanupWarning, /device busy/);
    return true;
  });
});
test('owned mount is cleaned up when the returned volume is unreadable', async t => {
  const f = await fixture(t);
  f.adapter.mount = async () => ({ root: path.join(f.temporary, 'missing-volume'), owned: true, devicePath: 'missing' });
  await assert.rejects(withMountedIso(f.image, () => assert.fail('unreadable callback'), { adapter: f.adapter }), /no readable filesystem volume/);
  assert.equal(f.calls.at(-1)[0], 'unmount');
});


test('cancellation after the callback commits cannot turn a published result into failure', async t => {
  const f = await fixture(t);
  const controller = new AbortController();
  const result = await withMountedIso(f.image, async () => {
    controller.abort();
    return { complete: true, directory: 'already published' };
  }, { adapter: f.adapter, signal: controller.signal });
  assert.equal(result.complete, true);
  assert.equal(f.calls.at(-1)[0], 'unmount');
});


async function discFixture(t) {
  const f = await fixture(t);
  const volume = path.join(f.temporary, 'disc-volume');
  for (const name of ['PS5', 'app', 'bd', 'license']) await fs.mkdir(path.join(volume, name), { recursive: true });
  f.volume = volume;
  f.adapter.mount = async file => { f.calls.push(['mount', file]); return { root: volume, owned: true, devicePath: 'disc-device' }; };
  return f;
}
async function extractedDiscGame(volume, id, title) {
  const game = path.join(volume, 'app', id);
  await fs.mkdir(path.join(game, 'sce_module'), { recursive: true });
  await fs.mkdir(path.join(game, 'sce_sys'));
  await fs.writeFile(path.join(game, 'eboot.bin'), ELF);
  await fs.writeFile(path.join(game, 'sce_module', 'module.prx'), ELF);
  await fs.writeFile(path.join(game, 'sce_sys', 'param.json'), JSON.stringify({ titleId: id, localizedParameters: { 'en-US': { titleName: title } } }));
  return game;
}
test('disc app/title PKG layout is diagnosed without treating package payloads as executables', async t => {
  const f = await discFixture(t);
  const app = path.join(f.volume, 'app', 'PPSA04263');
  await fs.mkdir(app);
  await fs.writeFile(path.join(app, 'app.crc'), 'fixture checksum');
  await fs.writeFile(path.join(app, 'app.json'), '{}');
  const packageFiles = [
    ['app_0.pkg', Buffer.from([0x7f, 0x46, 0x49, 0x48, 0, 0, 0, 0])],
    ['app_sc.pkg', Buffer.from([0x7f, 0x43, 0x4e, 0x54, 0, 0, 0, 0])]
  ];
  for (const [name, header] of packageFiles) await fs.writeFile(path.join(app, name), header);
  const result = await inspectIso(f.image, { adapter: f.adapter });
  assert.equal(result.entry, '');
  assert.deepEqual(result.executables, []);
  assert.match(result.diagnostic, /PKG packages in app\\PPSA04263.*not supported/);
  assert.equal(result.title, path.basename(f.image, path.extname(f.image)));
  for (const [name, header] of packageFiles) assert.deepEqual(await fs.readFile(path.join(app, name)), header);
  assert.equal(f.calls.at(-1)[0], 'unmount');
});
test('one extracted app/title game is detected despite sibling disc metadata folders', async t => {
  const f = await discFixture(t);
  const game = await extractedDiscGame(f.volume, 'PPSA12345', 'Extracted Disc Game');
  await fs.mkdir(path.join(f.volume, 'app', 'metadata'));
  await fs.writeFile(path.join(f.volume, 'app', 'metadata', 'index.json'), '{}');
  const result = await inspectIso(f.image, { adapter: f.adapter });
  assert.equal(result.title, 'Extracted Disc Game');
  assert.equal(result.titleId, 'PPSA12345');
  assert.equal(result.sourceFolder, 'app\\PPSA12345');
  assert.equal(result.entry, 'app\\PPSA12345\\eboot.bin');
  await resolveIsoRequest({ source: f.image, entry: result.entry }, resolved => {
    assert.equal(resolved.source, game);
    assert.equal(resolved.entry, path.join(game, 'eboot.bin'));
  }, { adapter: f.adapter });
});
test('multiple extracted app games are refused during both inspection and resolution', async t => {
  const f = await discFixture(t);
  await extractedDiscGame(f.volume, 'PPSA11111', 'One');
  await extractedDiscGame(f.volume, 'PPSA22222', 'Two');
  const result = await inspectIso(f.image, { adapter: f.adapter });
  assert.equal(result.entry, '');
  assert.deepEqual(result.executables, []);
  assert.equal(result.ambiguous, true);
  assert.match(result.diagnostic, /multiple extracted game folders/);
  await assert.rejects(resolveIsoRequest({ source: f.image, entry: 'app\\PPSA11111\\eboot.bin' }, () => assert.fail('ambiguous game callback'), { adapter: f.adapter }), /multiple extracted game folders/);
});
test('known disc probing has a strict child-folder bound', async t => {
  const f = await discFixture(t);
  for (let index = 0; index < 65; index++) await fs.mkdir(path.join(f.volume, 'app', 'folder-' + index));
  const result = await inspectIso(f.image, { adapter: f.adapter });
  assert.equal(result.entry, '');
  assert.equal(result.ambiguous, true);
  assert.match(result.diagnostic, /too many app folders/);
});
