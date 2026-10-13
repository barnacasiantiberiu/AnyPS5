const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const { install, validate, inspectSource, safeName, DLLS, runProcess } = require('../src/installer.cjs');
const ELF = Buffer.from([127, 69, 76, 70, 2, 1, 1, 0]);
const PE = Buffer.alloc(256); PE.write('MZ'); PE.writeUInt32LE(128, 60); PE.write('PE\0\0', 128, 'binary'); PE.writeUInt16LE(0x8664, 132);
async function fixture(t) {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-desktop-test-'));
  t.after(async () => { if (path.dirname(root) === os.tmpdir() && path.basename(root).startsWith('anyps5-desktop-test-')) await fs.rm(root, { recursive: true, force: true }); });
  const source = path.join(root, 'Source'); const libs = path.join(root, 'runtime'); const destinationParent = path.join(root, 'games'); const relinker = path.join(root, 'relinker.exe');
  await fs.mkdir(path.join(source, 'sce_module'), { recursive: true }); await fs.mkdir(path.join(source, 'Media', 'Plugins'), { recursive: true }); await fs.mkdir(libs); await fs.mkdir(destinationParent);
  const entry = path.join(source, 'eboot.bin'); await fs.writeFile(entry, ELF); await fs.writeFile(path.join(source, 'sce_module', 'game.prx'), ELF); await fs.writeFile(path.join(source, 'Media', 'Plugins', 'plugin.prx'), ELF);
  await fs.writeFile(path.join(source, 'level.data'), 'source stays intact');
  await fs.writeFile(relinker, PE); await fs.writeFile(path.join(libs, 'libkernel.prx'), PE);
  for (const name of DLLS) await fs.writeFile(path.join(libs, name), PE);
  return { root, source, libs, destinationParent, relinker, entry, title: 'My game', toIntel: true };
}
const fakeRun = async (_exe, args, options, log) => {
  assert.equal(args[0], '--windows'); assert.ok(args.includes('--registry')); assert.ok(args.includes('--to-intel'));
  assert.ok(args.includes(path.join('Media', 'Plugins')));
  await fs.writeFile(args.at(-1), PE);
  await fs.mkdir(path.join(options.cwd, 'app0', 'sce_module'), { recursive: true });
  await fs.writeFile(path.join(options.cwd, 'app0', 'sce_module', 'game.prx.guest.prx'), PE);
  log('fixture conversion completed\n');
};
test('installs resources, converted modules and DLLs; leaves source untouched', async t => {
  const request = await fixture(t); const phases = [];
  const result = await install(request, { run: fakeRun, onEvent: event => phases.push(event) });
  assert.equal(result.directory, path.join(request.destinationParent, 'My game'));
  assert.deepEqual(await fs.readFile(request.entry), ELF);
  assert.equal(await fs.readFile(path.join(result.directory, 'app0', 'level.data'), 'utf8'), 'source stays intact');
  await assert.rejects(fs.access(path.join(result.directory, 'app0', 'eboot.bin')));
  await assert.rejects(fs.access(path.join(result.directory, 'app0', 'sce_module', 'game.prx')));
  assert.deepEqual(await fs.readFile(path.join(result.directory, 'app0', 'sce_module', 'game.prx.guest.prx')), PE);
  assert.deepEqual(await fs.readFile(path.join(result.directory, 'libs', DLLS[0])), PE);
  const manifest = JSON.parse(await fs.readFile(path.join(result.directory, 'anyps5-game.json')));
  assert.equal(manifest.executable, 'app.exe'); assert.equal(manifest.compatibility, 'untested');
  assert.ok(phases.some(e => e.kind === 'phase' && e.label === 'Conversion complete'));
});
test('rejects output already present without touching its data', async t => {
  const request = await fixture(t); const target = path.join(request.destinationParent, request.title); await fs.mkdir(target); await fs.writeFile(path.join(target, 'keep'), 'keep');
  await assert.rejects(install(request, { run: fakeRun }), /folder with this name already exists/);
  assert.equal(await fs.readFile(path.join(target, 'keep'), 'utf8'), 'keep');
});
test('rejects destination inside source', async t => {
  const request = await fixture(t); request.destinationParent = request.source;
  await assert.rejects(validate(request), /must be separate/);
});
test('preflight never creates a destination; installs into a new parent after validation', async t => {
  const request = await fixture(t);
  request.destinationParent = path.join(request.root, 'New library', 'games');
  await validate(request);
  await assert.rejects(fs.access(request.destinationParent));
  await install(request, { run: fakeRun });
  await fs.access(path.join(request.destinationParent, request.title, 'app.exe'));
});
test('invalid nested destination leaves no new folders in the original source', async t => {
  const request = await fixture(t); request.destinationParent = path.join(request.source, 'new folder');
  await assert.rejects(install(request, { run: fakeRun }), /must be separate/);
  await assert.rejects(fs.access(request.destinationParent));
});
test('preserves empty resource folders', async t => {
  const request = await fixture(t); await fs.mkdir(path.join(request.source, 'EmptySaveFolder'));
  const result = await install(request, { run: fakeRun });
  assert.ok((await fs.stat(path.join(result.directory, 'app0', 'EmptySaveFolder'))).isDirectory());
});
test('rejects ELF outside the source root', async t => {
  const request = await fixture(t); request.entry = path.join(request.root, 'outside.elf'); await fs.writeFile(request.entry, ELF);
  await assert.rejects(validate(request), /outside the game folder/);
});
test('rejects an encrypted executable and encrypted modules', async t => {
  const request = await fixture(t); const self = Buffer.from([0x4f, 0x15, 0x3d, 0x1d]);
  await fs.writeFile(request.entry, self); await assert.rejects(validate(request), /decrypted ELF/);
  await fs.writeFile(request.entry, ELF); await fs.writeFile(path.join(request.source, 'sce_module', 'game.prx'), self);
  await assert.rejects(validate(request), /is a SELF file/);
});
test('requires Windows libraries and the three MinGW dependencies', async t => {
  const request = await fixture(t); await fs.writeFile(path.join(request.libs, 'libkernel.prx'), ELF);
  await assert.rejects(validate(request), /Windows x64/);
  await fs.writeFile(path.join(request.libs, 'libkernel.prx'), PE); await fs.unlink(path.join(request.libs, DLLS[0]));
  await assert.rejects(validate(request), /libgcc.*is missing/);
});
test('can find matching-toolchain DLLs outside libs', async t => {
  const request = await fixture(t); const bin = path.join(request.root, 'bin'); await fs.mkdir(bin);
  for (const name of DLLS) await fs.rename(path.join(request.libs, name), path.join(bin, name));
  const plan = await validate({ ...request, dllDirectory: bin });
  assert.equal(plan.runtimeFiles.length, 4);
});
test('cleans only its own staging folder when relinker fails', async t => {
  const request = await fixture(t); await fs.mkdir(path.join(request.destinationParent, '.anyps5-install-user-data'));
  await assert.rejects(install(request, { run: async () => { throw new Error('rel linker failure'); } }), /rel linker failure/);
  assert.deepEqual(await fs.readdir(request.destinationParent), ['.anyps5-install-user-data']);
  assert.deepEqual(await fs.readFile(request.entry), ELF);
});
test('cancelling during conversion never publishes a game', async t => {
  const request = await fixture(t); const controller = new AbortController();
  await assert.rejects(install(request, { signal: controller.signal, run: async () => { controller.abort(); } }), /cancelled/);
  assert.deepEqual(await fs.readdir(request.destinationParent), []);
});
test('successful exit without a real PE output is a failure', async t => {
  const request = await fixture(t);
  await assert.rejects(install(request, { run: async () => {} }), /Windows x64/);
  assert.deepEqual(await fs.readdir(request.destinationParent), []);
});
test('detects source ELF and metadata; rejects Windows reserved names', async t => {
  const request = await fixture(t); await fs.mkdir(path.join(request.source, 'sce_sys'));
  await fs.writeFile(path.join(request.source, 'sce_sys', 'param.json'), JSON.stringify({ titleId: 'TEST00001', localizedParameters: { 'en-US': { titleName: 'The Test Game' } } }));
  const result = await inspectSource(request.source); assert.equal(result.title, 'The Test Game'); assert.equal(result.entry, request.entry);
  assert.throws(() => safeName('CON'), /valid game name/); assert.equal(safeName('Game: Two?'), 'Game Two');
});
test('process execution keeps arguments literal and reports failures', async t => {
  const request = await fixture(t); let output = '';
  await runProcess(process.execPath, ['-e', 'process.stdout.write(process.argv[1])', 'a & b; $(example)'], { cwd: request.root }, text => { output += text; });
  assert.equal(output, 'a & b; $(example)');
  await assert.rejects(runProcess(process.execPath, ['-e', 'process.exit(7)'], { cwd: request.root }, () => {}), /code 7/);
});
