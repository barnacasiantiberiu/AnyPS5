const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const syncFs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const crypto = require('node:crypto');
const { inspectSource, checkEntry, validate, install, DLLS } = require('../src/installer.cjs');

const ELF = Buffer.from([127, 69, 76, 70, 2, 1, 1, 9]);
const { selfExecutable } = require('./fixtures/self.cjs');
const SELF = selfExecutable();
const PS4_SELF = Buffer.from([0x4f, 0x15, 0x3d, 0x1d, 0, 1, 1, 0x12]);
const PE = Buffer.alloc(256);
PE.write('MZ');
PE.writeUInt32LE(128, 60);
PE.write('PE\0\0', 128, 'binary');
PE.writeUInt16LE(0x8664, 132);
const binaries = ['eboot.bin', 'sce_module/game.prx', 'Media/Modules/Il2cppUserAssemblies.prx', 'Media/Plugins/plugin.prx'];
const resources = {
  'Media/Resources/level.data': Buffer.from('Full root resource, absent from decrypted overlay'),
  'Media/Metadata/global-metadata.dat': Buffer.from('Metadata stays with the game'),
  'sce_sys/param.json': Buffer.from(JSON.stringify({ titleId: 'TEST09904', localizedParameters: { 'en-US': { titleName: 'Overlay Test Game' } } })),
  'sce_sys/icon0.png': Buffer.from('fixture artwork'),
};
const nativePath = relative => relative.split('/').join(path.sep);
async function writeFile(file, content) {
  await fs.mkdir(path.dirname(file), { recursive: true });
  await fs.writeFile(file, content);
}
async function snapshot(directory) {
  const result = {};
  async function visit(current) {
    for (const entry of (await fs.readdir(current, { withFileTypes: true })).sort((a, b) => a.name.localeCompare(b.name))) {
      const full = path.join(current, entry.name);
      const relative = path.relative(directory, full);
      if (entry.isDirectory()) {
        result[relative + path.sep] = 'directory';
        await visit(full);
      } else {
        result[relative] = crypto.createHash('sha256').update(await fs.readFile(full)).digest('hex');
      }
    }
  }
  await visit(directory);
  return result;
}
async function fixture(t) {
  const tempParent = path.resolve(os.tmpdir());
  const root = await fs.mkdtemp(path.join(tempParent, 'anyps5-overlay-test-'));
  t.after(async () => {
    assert.equal(path.dirname(path.resolve(root)), tempParent);
    assert.ok(path.basename(root).startsWith('anyps5-overlay-test-'));
    await fs.rm(root, { recursive: true, force: true });
  });
  const wrapper = path.join(root, 'Download');
  const source = path.join(wrapper, 'Game folder');
  const overlay = path.join(source, 'decrypted');
  const libs = path.join(root, 'runtime');
  const destinationParent = path.join(root, 'installed');
  const relinker = path.join(root, 'relinker.exe');
  for (const [index, relative] of binaries.entries()) {
    await writeFile(path.join(source, nativePath(relative)), Buffer.concat([SELF, Buffer.from('original-' + index)]));
    await writeFile(path.join(overlay, nativePath(relative)), Buffer.concat([ELF, Buffer.from('replacement-' + index)]));
  }
  for (const [relative, content] of Object.entries(resources)) await writeFile(path.join(source, nativePath(relative)), content);
  await fs.mkdir(path.join(source, 'Media', 'EmptySaveFolder'));
  await fs.mkdir(destinationParent);
  for (const name of ['libkernel.prx', 'libSceAmpr.prx', 'libScePlayGo.prx', ...DLLS]) await writeFile(path.join(libs, name), PE);
  await writeFile(relinker, PE);
  await writeFile(path.join(source, 'fakelib', 'libSceAmpr.sprx'), PS4_SELF);
  await writeFile(path.join(source, 'fakelib', 'libScePlayGo.sprx'), PS4_SELF);
  return { root, wrapper, source, overlay, libs, destinationParent, relinker, entry: path.join(overlay, 'eboot.bin'), title: 'Installed overlay', titleId: 'TEST09904' };
}
async function assertSourceUnchanged(request, before) {
  assert.deepEqual(await snapshot(request.wrapper), before);
}
async function assertNoPublish(request, keep = []) {
  assert.deepEqual((await fs.readdir(request.destinationParent)).sort(), keep.slice().sort());
}
async function verifyStagedInput(request, args, options) {
  const app0 = path.join(options.cwd, 'app0');
  assert.equal(args[args.indexOf('--sce-module-path') + 1], app0);
  assert.equal(args.at(-2), path.join(app0, 'eboot.bin'));
  assert.equal(args.at(-1), path.join(options.cwd, 'app.exe'));
  assert.ok(args.includes(nativePath('Media/Modules')));
  assert.ok(args.includes(nativePath('Media/Plugins')));
  for (const relative of binaries) {
    assert.deepEqual(await fs.readFile(path.join(app0, nativePath(relative))), await fs.readFile(path.join(request.overlay, nativePath(relative))));
  }
  for (const [relative, content] of Object.entries(resources)) assert.deepEqual(await fs.readFile(path.join(app0, nativePath(relative))), content);
  assert.ok((await fs.stat(path.join(app0, 'Media', 'EmptySaveFolder'))).isDirectory());
  await assert.rejects(fs.access(path.join(app0, 'decrypted')));
  await assert.rejects(fs.access(path.join(app0, 'fakelib')));
}
async function writeConvertedOutput(args, options) {
  await fs.writeFile(args.at(-1), PE);
  for (const relative of binaries.slice(1)) await writeFile(path.join(options.cwd, 'app0', nativePath(relative) + '.guest.prx'), PE);
}

test('wrapper and decrypted selection resolve to the full resource root and metadata', async t => {
  const request = await fixture(t);
  const before = await snapshot(request.wrapper);
  for (const selected of [request.wrapper, request.source, request.overlay]) {
    const result = await inspectSource(selected);
    assert.equal(result.source, request.source);
    assert.equal(result.title, 'Overlay Test Game');
    assert.equal(result.titleId, 'TEST09904');
    assert.equal(result.entry, request.entry);
    assert.equal(result.decryptedRoot, request.overlay);
    assert.equal(result.encrypted, false);
    assert.match(result.note, /original game resources/);
    assert.deepEqual(result.executables, [request.entry]);
  }
  await assertSourceUnchanged(request, before);
});

test('wrapper search is bounded and never chooses among multiple child folders', async t => {
  const request = await fixture(t);
  const ambiguous = path.join(request.root, 'Ambiguous');
  await fs.mkdir(path.join(ambiguous, 'One'), { recursive: true });
  await fs.mkdir(path.join(ambiguous, 'Two'));
  assert.equal((await inspectSource(ambiguous)).source, ambiguous);
  const outer = path.join(request.root, 'Deep');
  const depthFour = path.join(outer, 'one', 'two', 'three', 'four');
  await writeFile(path.join(depthFour, 'five', 'eboot.bin'), ELF);
  const bounded = await inspectSource(outer);
  assert.equal(bounded.source, depthFour);
  assert.equal(bounded.entry, '');
  assert.match(bounded.diagnostic, /No executable was found/);
});

test('entry diagnostics distinguish SELF, invalid format and outside the resource root', async t => {
  const request = await fixture(t);
  await assert.rejects(checkEntry({ source: request.source, entry: path.join(request.source, 'eboot.bin') }), /Encrypted SELF payload.*decrypted ELF/);
  const invalid = path.join(request.source, 'invalid.elf');
  await fs.writeFile(invalid, Buffer.from('not an ELF'));
  await assert.rejects(checkEntry({ source: request.source, entry: invalid }), /Not a recognized ELF or SELF executable/);
  const outside = path.join(request.root, 'outside.elf');
  await fs.writeFile(outside, ELF);
  await assert.rejects(checkEntry({ source: request.source, entry: outside }), /outside the game folder/);
  assert.equal(await checkEntry({ source: request.source, entry: request.entry }), request.entry);
  const selfOnly = path.join(request.root, 'Encrypted only');
  await writeFile(path.join(selfOnly, 'eboot.bin'), SELF);
  await writeFile(path.join(selfOnly, 'a-invalid.elf'), Buffer.from('invalid executable'));
  const encrypted = await inspectSource(selfOnly);
  assert.equal(encrypted.encrypted, true);
  assert.match(encrypted.diagnostic, /Encrypted SELF payload.*cannot decrypt/);
});

test('overlay installation stages decrypted bytes and resources, preserves guest output and all originals', async t => {
  const request = await fixture(t);
  const before = await snapshot(request.wrapper);
  const events = [];
  const result = await install({ ...request, source: request.wrapper }, {
    onEvent: event => events.push(event),
    run: async (_exe, args, options, log) => {
      await verifyStagedInput(request, args, options);
      await writeConvertedOutput(args, options);
      log('all guest modules finished\n');
    },
  });
  assert.equal(result.directory, path.join(request.destinationParent, request.title));
  const app0 = path.join(result.directory, 'app0');
  for (const relative of binaries) await assert.rejects(fs.access(path.join(app0, nativePath(relative))));
  for (const relative of binaries.slice(1)) assert.deepEqual(await fs.readFile(path.join(app0, nativePath(relative) + '.guest.prx')), PE);
  for (const [relative, content] of Object.entries(resources)) assert.deepEqual(await fs.readFile(path.join(app0, nativePath(relative))), content);
  for (const helper of ['decrypted', 'fakelib']) await assert.rejects(fs.access(path.join(app0, helper)));
  for (const name of ['libSceAmpr.prx', 'libScePlayGo.prx', ...DLLS]) assert.deepEqual(await fs.readFile(path.join(result.directory, 'libs', name)), PE);
  const log = await fs.readFile(path.join(result.directory, 'conversion.log'), 'utf8');
  assert.match(log, /Helper folders excluded.*decrypted, fakelib/);
  assert.match(log, /all guest modules finished/);
  assert.equal(events.filter(event => event.complete === true).length, 1);
  await assertSourceUnchanged(request, before);
});

test('unknown fakelib helper is rejected even when a same-name runtime provider exists', async t => {
  const request = await fixture(t);
  await writeFile(path.join(request.source, 'fakelib', 'libUnknown.sprx'), SELF);
  await writeFile(path.join(request.libs, 'libUnknown.prx'), PE);
  const before = await snapshot(request.wrapper);
  await assert.rejects(validate(request), /fakelib.*recognized Windows runtime replacement.*libUnknown\.sprx/);
  await assertSourceUnchanged(request, before);
  await assertNoPublish(request);
});

test('known fakelib stubs require corresponding valid Windows runtime providers', async t => {
  const request = await fixture(t);
  await fs.unlink(path.join(request.libs, 'libScePlayGo.prx'));
  await assert.rejects(validate(request), /fakelib.*runtime replacement.*libScePlayGo\.sprx/);
  await writeFile(path.join(request.libs, 'libScePlayGo.prx'), ELF);
  await assert.rejects(validate(request), /not a valid Windows x64 executable.*libScePlayGo\.prx/);
  await writeFile(path.join(request.libs, 'libScePlayGo.prx'), PE);
  const plan = await validate(request);
  assert.equal(plan.normalizedInput, true);
  assert.ok(plan.ignoredHelpers.includes('fakelib'));
  await assertNoPublish(request);
});

test('a module with no decrypted counterpart fails before copying or publication', async t => {
  const request = await fixture(t);
  await fs.unlink(path.join(request.overlay, 'sce_module', 'game.prx'));
  const before = await snapshot(request.wrapper);
  await assert.rejects(install(request, { run: async () => assert.fail('converter must not start') }), /game\.prx.*Encrypted SELF payload/);
  await assertSourceUnchanged(request, before);
  await assertNoPublish(request);
});

test('decrypted binary without an original counterpart is rejected', async t => {
  const request = await fixture(t);
  await writeFile(path.join(request.overlay, 'Media', 'Plugins', 'unexpected.prx'), ELF);
  await assert.rejects(validate(request), /decrypted file has no matching original game file.*unexpected\.prx/);
  await assertNoPublish(request);
});

for (const outcome of ['failure', 'cancellation']) {
  test(outcome + ' during overlay conversion preserves sources and unrelated folders', async t => {
    const request = await fixture(t);
    const keep = '.anyps5-install-user-data';
    await writeFile(path.join(request.destinationParent, keep, 'keep.txt'), 'user file');
    const before = await snapshot(request.wrapper);
    const controller = new AbortController();
    await assert.rejects(install(request, {
      signal: controller.signal,
      run: async (_exe, args, options) => {
        await verifyStagedInput(request, args, options);
        await writeConvertedOutput(args, options);
        if (outcome === 'cancellation') controller.abort();
        else throw new Error('fixture converter failed');
      },
    }), outcome === 'cancellation' ? /cancelled/ : /fixture converter failed/);
    await assertNoPublish(request, [keep]);
    assert.equal(await fs.readFile(path.join(request.destinationParent, keep, 'keep.txt'), 'utf8'), 'user file');
    await assertSourceUnchanged(request, before);
  });
}

test('copying 100 percent is not completion; final log and published files precede complete', async t => {
  const request = await fixture(t);
  const events = [];
  let releaseRun;
  let markStarted;
  const heldRun = new Promise(resolve => { releaseRun = resolve; });
  const runStarted = new Promise(resolve => { markStarted = resolve; });
  const target = path.join(request.destinationParent, request.title);
  let completionSawPublishedFiles = false;
  let completionSawFinalLog = false;
  const pending = install(request, {
    onEvent: event => {
      events.push(event);
      if (event.complete) {
        completionSawPublishedFiles = syncFs.existsSync(path.join(target, 'app.exe')) && syncFs.existsSync(path.join(target, 'anyps5-game.json'));
        completionSawFinalLog = syncFs.readFileSync(path.join(target, 'conversion.log'), 'utf8').includes('LAST CONVERTER LOG');
      }
    },
    run: async (_exe, args, options, log) => {
      markStarted();
      log('converter still processing\n');
      await heldRun;
      await writeConvertedOutput(args, options);
      log('LAST CONVERTER LOG\n');
    },
  });
  try {
    await Promise.race([runStarted, pending.then(() => assert.fail('installation finished without invoking the converter'))]);
    const copyComplete = events.find(event => event.stage === 'copying' && event.progress === 100);
    assert.ok(copyComplete);
    assert.equal(copyComplete.complete, false);
    assert.equal(events.filter(event => event.kind === 'phase').at(-1).stage, 'converting');
    assert.equal(events.some(event => event.complete === true), false);
    await assert.rejects(fs.access(target));
  } finally {
    releaseRun();
    await pending;
  }
  const lastLog = events.findIndex(event => event.kind === 'log' && event.text.includes('LAST CONVERTER LOG'));
  const completed = events.findIndex(event => event.complete === true);
  assert.ok(lastLog >= 0 && completed > lastLog);
  assert.equal(events.at(-1).stage, 'complete');
  assert.equal(events.at(-1).progress, 100);
  assert.equal(completionSawPublishedFiles, true);
  assert.equal(completionSawFinalLog, true);
});

test('decrypted main executable takes precedence over unrelated root ELF modules', async t => {
  const request = await fixture(t);
  await fs.writeFile(path.join(request.source, 'module.elf'), ELF);
  const result = await inspectSource(request.wrapper);
  assert.equal(result.entry, request.entry);
  assert.ok(result.executables.includes(path.join(request.source, 'module.elf')));
});

test('preflight reserves space for temporary ELF copies alongside generated output', async t => {
  const request = await fixture(t);
  const plan = await validate(request);
  const runtimeBytes = (await Promise.all(plan.runtimeFiles.map(f => fs.stat(f.absolute)))).reduce((sum, value) => sum + value.size, 0);
  const inputBytes = plan.inputFiles.reduce((sum, file) => sum + file.size, 0);
  const resourcesBytes = plan.resourceFiles.reduce((sum, file) => sum + file.size, 0);
  const originalStatfs = fs.statfs;
  // Enough for resources, the expected output and safety allowance, but not temporary inputs too.
  fs.statfs = async () => ({ bavail: resourcesBytes + inputBytes + runtimeBytes + 128 * 1024 * 1024, bsize: 1 });
  try { await assert.rejects(validate(request), /not enough free space/); }
  finally { fs.statfs = originalStatfs; }
});
