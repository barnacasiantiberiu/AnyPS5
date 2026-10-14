// Opt-in real Windows ISO lifecycle check. Uses only tiny synthetic images in one
// unique temp folder. No user image, real converter, software install or OS setting.
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const crypto = require('node:crypto');
const { execFile } = require('node:child_process');
const { promisify } = require('node:util');
const exec = promisify(execFile);
const { inspectIso, withMountedIso, resolveIsoRequest } = require('../src/iso-source.cjs');
const { inspectSource, validate, install, DLLS } = require('../src/installer.cjs');
const powershell = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
const ELF = Buffer.from([127, 69, 76, 70, 2, 1, 1, 0]);
const SELF = Buffer.from([0x4f, 0x15, 0x3d, 0x1d, 0, 0, 0, 0]);
const PE = Buffer.alloc(256); PE.write('MZ'); PE.writeUInt32LE(128, 60); PE.write('PE\0\0', 128, 'binary'); PE.writeUInt16LE(0x8664, 132);
const resource = Buffer.from('Synthetic resource from the full ISO root.\n'.repeat(400));
async function write(file, bytes) { await fs.mkdir(path.dirname(file), { recursive: true }); await fs.writeFile(file, bytes); }
async function hash(file) { return crypto.createHash('sha256').update(await fs.readFile(file)).digest('hex'); }
function ownedImage(root, image) {
  assert.equal(path.dirname(path.resolve(image)), path.resolve(root));
  assert.ok(['plain.iso', 'overlay.iso'].includes(path.basename(image)));
}
async function native(root, image, action) {
  ownedImage(root, image);
  const operations = {
    status: '$disk = Get-DiskImage -ImagePath $env:ANYPS5_NATIVE_FIXTURE_ISO -ErrorAction Stop; @{ attached = [bool]$disk.Attached } | ConvertTo-Json -Compress',
    mount: 'Mount-DiskImage -ImagePath $env:ANYPS5_NATIVE_FIXTURE_ISO -StorageType ISO -Access ReadOnly -ErrorAction Stop | Out-Null',
    unmount: 'Dismount-DiskImage -ImagePath $env:ANYPS5_NATIVE_FIXTURE_ISO -ErrorAction Stop | Out-Null'
  };
  assert.ok(Object.hasOwn(operations, action));
  const code = "$ErrorActionPreference = 'Stop'; " + operations[action];
  const result = await exec(powershell, ['-NoLogo', '-NoProfile', '-NonInteractive', '-EncodedCommand', Buffer.from(code, 'utf16le').toString('base64')], {
    windowsHide: true, timeout: 60000, maxBuffer: 1024 * 1024,
    env: { ...process.env, ANYPS5_NATIVE_FIXTURE_ISO: image }
  });
  return action === 'status' ? JSON.parse(result.stdout.trim()).attached : undefined;
}
async function createFixture(root, variant) {
  const source = path.join(root, variant, 'Game files');
  await write(path.join(source, 'eboot.bin'), variant === 'overlay' ? SELF : ELF);
  await write(path.join(source, 'sce_module', 'game.prx'), variant === 'overlay' ? SELF : ELF);
  await write(path.join(source, 'Media', 'Resources', 'level.data'), resource);
  await fs.mkdir(path.join(source, 'Media', 'Empty'), { recursive: true });
  await write(path.join(source, 'sce_sys', 'param.json'), JSON.stringify({ titleId: 'TESTISO01', localizedParameters: { 'en-US': { titleName: 'Native ISO ' + variant } } }));
  if (variant === 'overlay') {
    await write(path.join(source, 'decrypted', 'eboot.bin'), ELF);
    await write(path.join(source, 'decrypted', 'sce_module', 'game.prx'), ELF);
  }
  const helper = await fs.readFile(path.join(__dirname, 'fixtures', 'create-native-iso.ps1'), 'utf8');
  const code = '& {\n' + helper + '\n} -FixtureRoot $env:ANYPS5_NATIVE_FIXTURE_ROOT -Variant $env:ANYPS5_NATIVE_FIXTURE_VARIANT';
  const result = await exec(powershell, ['-NoLogo', '-NoProfile', '-NonInteractive', '-EncodedCommand', Buffer.from(code, 'utf16le').toString('base64')], {
    windowsHide: true, timeout: 60000, maxBuffer: 1024 * 1024,
    env: { ...process.env, ANYPS5_NATIVE_FIXTURE_ROOT: root, ANYPS5_NATIVE_FIXTURE_VARIANT: variant }
  });
  const generated = JSON.parse(result.stdout.trim());
  const image = path.join(root, variant + '.iso');
  assert.equal(generated.image, image);
  assert.ok(generated.bytes > 32768 && generated.bytes < 8 * 1024 * 1024);
  return { image, variant, originalHash: await hash(image) };
}
async function verifyImage(root, fixture, runtime) {
  const { image, variant } = fixture;
  assert.equal(await native(root, image, 'status'), false);
  const inspected = await inspectIso(image);
  assert.equal(inspected.source, await fs.realpath(image));
  assert.equal(inspected.sourceType, 'iso');
  assert.equal(inspected.title, 'Native ISO ' + variant);
  assert.equal(inspected.titleId, 'TESTISO01');
  assert.equal(inspected.encrypted, false);
  assert.ok(inspected.entry.endsWith('eboot.bin'));
  assert.equal(path.isAbsolute(inspected.entry), false);
  assert.equal(inspected.sourceFolder, 'Game files');
  if (variant === 'overlay') assert.ok(inspected.entry.includes('decrypted'));
  assert.equal(await native(root, image, 'status'), false, 'Inspection must detach its own image.');
  let mountedRoot;
  await withMountedIso(image, async volumeRoot => {
    mountedRoot = volumeRoot;
    const info = await inspectSource(volumeRoot);
    assert.equal(info.title, inspected.title);
    assert.deepEqual(await fs.readFile(path.join(info.source, 'Media', 'Resources', 'level.data')), resource);
    await assert.rejects(fs.writeFile(path.join(volumeRoot, 'must-stay-readonly.txt'), 'This synthetic image must be read-only.'));
  });
  assert.equal(await native(root, image, 'status'), false);
  assert.ok(mountedRoot, 'Native mount must expose a readable volume.');
  const request = { ...runtime, source: image, sourceType: 'iso', entry: inspected.entry, title: 'Installed ISO ' + variant, titleId: inspected.titleId, toIntel: false };
  const installed = await resolveIsoRequest(request, async resolved => {
    assert.equal(resolved.sourceType, 'folder');
    assert.ok(path.isAbsolute(resolved.entry));
    const plan = await validate(resolved);
    assert.equal(plan.normalizedInput, variant === 'overlay');
    assert.ok(plan.resourceFiles.some(file => /Media[\\/]Resources[\\/]level\.data$/i.test(file.relative)));
    return install(resolved, { run: async (_exe, args, options, log) => {
      const output = args.at(-1), entry = args.at(-2);
      assert.deepEqual(await fs.readFile(entry), ELF);
      const source = args[args.indexOf('--sce-module-path') + 1];
      assert.deepEqual(await fs.readFile(path.join(source, 'sce_module', 'game.prx')), ELF);
      assert.deepEqual(await fs.readFile(path.join(options.cwd, 'app0', 'Media', 'Resources', 'level.data')), resource);
      await write(output, PE);
      await write(path.join(options.cwd, 'app0', 'sce_module', 'game.prx.guest.prx'), PE);
      log('Synthetic native-ISO converter completed.\n');
    } });
  });
  assert.equal(await native(root, image, 'status'), false, 'Conversion must detach its own image.');
  assert.deepEqual(await fs.readFile(path.join(installed.directory, 'app0', 'Media', 'Resources', 'level.data')), resource);
  assert.ok((await fs.stat(path.join(installed.directory, 'app0', 'Media', 'Empty'))).isDirectory());
  assert.deepEqual(await fs.readFile(path.join(installed.directory, 'app0', 'sce_module', 'game.prx.guest.prx')), PE);
  await assert.rejects(fs.access(path.join(installed.directory, 'app0', 'eboot.bin')));
  if (variant === 'overlay') await assert.rejects(fs.access(path.join(installed.directory, 'app0', 'decrypted')));
  assert.equal(await hash(image), fixture.originalHash);
  console.log('PASS: ' + variant + ' native read-only ISO detection, full resources, fake conversion, own-mount detach and unchanged SHA-256.');
}
async function verifyDesktopIpc(root, fixture, runtime) {
  const { _electron: electron } = require('playwright');
  const project = path.resolve(__dirname, '..');
  const gcc = process.env.ANYPS5_TEST_GCC || path.resolve(project, '..', 'AnyPS5', 'build_tools', 'winlibs', 'mingw64', 'bin', 'gcc.exe');
  const cSource = path.join(root, 'native-iso-converter.c');
  const converter = path.join(root, 'native-iso-converter.exe');
  await fs.writeFile(cSource, String.raw`#include <stdio.h>
#include <string.h>
#include <windows.h>
static int elf(const char *p){unsigned char h[4];FILE *f=fopen(p,"rb");if(!f)return 0;size_t n=fread(h,1,4,f);fclose(f);return n==4&&!memcmp(h,"\177ELF",4);}
static int output(const char *p){unsigned char b[256]={77,90};b[60]=128;b[128]=80;b[129]=69;b[132]=100;b[133]=134;FILE *f=fopen(p,"wb");if(!f)return 0;size_t n=fwrite(b,1,256,f);fclose(f);return n==256;}
int main(int argc,char **argv){char file[32768];const char *source=0;if(argc<3||!elf(argv[argc-2]))return 21;for(int i=1;i<argc-1;i++)if(!strcmp(argv[i],"--sce-module-path"))source=argv[i+1];if(!source)return 22;snprintf(file,sizeof(file),"%s\\sce_module\\game.prx",source);if(!elf(file))return 23;snprintf(file,sizeof(file),"%s\\Media\\Resources\\level.data",source);FILE *resource=fopen(file,"rb");if(!resource)return 24;fclose(resource);CreateDirectoryA("app0\\sce_module",NULL);if(!output("app0\\sce_module\\game.prx.guest.prx")||!output(argv[argc-1]))return 25;puts("SIMULATED CONVERTER: native ISO IPC fixture only");return 0;}
`);
  await exec(gcc, ['-static', cSource, '-o', converter], { windowsHide: true, timeout: 60000, env: { ...process.env, PATH: path.dirname(gcc) + path.delimiter + process.env.PATH } });
  const profile = path.join(root, 'electron-profile');
  await fs.mkdir(profile);
  let desktop;
  const errors = [];
  try {
    desktop = await electron.launch({ executablePath: process.env.ANYPS5_TEST_APP || require('electron'), args: process.env.ANYPS5_TEST_APP ? [] : [project], env: { ...process.env, ANYPS5_DESKTOP_TEST_DATA: profile } });
    const page = await desktop.firstWindow(); page.setDefaultTimeout(15000);
    page.on('pageerror', error => errors.push(error.message));
    await page.waitForFunction(() => typeof window.desktop?.inspectIso === 'function');
    assert.equal(await desktop.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].isVisible()), false, 'The ISO IPC fixture window must remain hidden.');
    const title = 'Installed via native ISO IPC';
    await desktop.evaluate(({ BrowserWindow }, fixture) => {
      const { execFileSync } = process.getBuiltinModule('node:child_process');
      const fs = process.getBuiltinModule('node:fs');
      const path = process.getBuiltinModule('node:path');
      const contents = BrowserWindow.getAllWindows()[0].webContents;
      const send = contents.send.bind(contents);
      globalThis.nativeIsoCompletionSamples = [];
      contents.send = (channel, event, ...remaining) => {
        if (channel === 'installation:event' && event?.kind === 'phase' && event.complete) {
          // Sample synchronously before allowing main to continue: an early event
          // cannot be concealed by an eventual detach during the PowerShell query.
          const code = "$ErrorActionPreference = 'Stop'; $disk = Get-DiskImage -ImagePath $env:ANYPS5_NATIVE_FIXTURE_ISO -ErrorAction Stop; @{ attached = [bool]$disk.Attached } | ConvertTo-Json -Compress";
          const text = execFileSync(fixture.powershell, ['-NoLogo', '-NoProfile', '-NonInteractive', '-EncodedCommand', Buffer.from(code, 'utf16le').toString('base64')], { windowsHide: true, timeout: 60000, encoding: 'utf8', env: { ...process.env, ANYPS5_NATIVE_FIXTURE_ISO: fixture.image } });
          const library = JSON.parse(fs.readFileSync(path.join(fixture.profile, 'library.json'), 'utf8'));
          globalThis.nativeIsoCompletionSamples.push({ attached: JSON.parse(text).attached, librarySaved: library.games.some(game => game.title === fixture.title), complete: event.complete });
        }
        return send(channel, event, ...remaining);
      };
    }, { image: fixture.image, profile, title, powershell });
    await page.evaluate(async settings => {
      const current = await window.desktop.state();
      await window.desktop.saveSettings({ ...current.settings, ...settings, dllDirectory: '' });
      window.nativeIsoEvents = [];
      window.desktop.onProgress(event => window.nativeIsoEvents.push(event));
    }, { ...runtime, relinker: converter });
    const inspected = await page.evaluate(image => window.desktop.inspectIso(image), fixture.image);
    assert.equal(inspected.sourceType, 'iso');
    assert.equal(inspected.title, 'Native ISO ' + fixture.variant);
    assert.equal(await native(root, fixture.image, 'status'), false, 'IPC inspection must release its mount.');
    const request = { source: inspected.source, sourceType: 'iso', entry: inspected.entry, title, titleId: inspected.titleId, destinationParent: runtime.destinationParent, toIntel: false };
    const plan = await page.evaluate(request => window.desktop.preflight(request), request);
    assert.ok(plan.resources >= 2);
    assert.equal(await native(root, fixture.image, 'status'), false, 'IPC preflight must release its mount.');
    const result = await page.evaluate(request => window.desktop.install(request), request);
    assert.equal(result.librarySaved, true);
    assert.equal(result.warning, '');
    assert.equal(result.game.title, title);
    const samples = await desktop.evaluate(() => globalThis.nativeIsoCompletionSamples);
    assert.deepEqual(samples, [{ attached: false, librarySaved: true, complete: true }], 'Completion must be sent only after native detach and library persistence.');
    const library = await page.evaluate(() => window.desktop.state());
    const saved = library.games.find(game => game.id === result.game.id);
    assert.ok(saved?.available);
    assert.equal(saved.directory, result.game.directory);
    assert.deepEqual(await fs.readFile(path.join(saved.directory, 'app0', 'Media', 'Resources', 'level.data')), resource);
    assert.deepEqual(await fs.readFile(path.join(saved.directory, 'app0', 'sce_module', 'game.prx.guest.prx')), PE);
    assert.equal(await native(root, fixture.image, 'status'), false);
    assert.equal(await hash(fixture.image), fixture.originalHash);
    assert.deepEqual(errors, []);
    console.log('PASS: ' + (process.env.ANYPS5_TEST_APP ? 'packaged' : 'source') + ' Electron native ISO inspect/preflight/install IPC, real fixture converter, full resources, persisted library and final completion after actual detach.');
  } finally { if (desktop) await desktop.close(); }
}
async function main() {
  if (process.platform !== 'win32') { console.log('SKIP: native ISO verification requires Windows.'); return; }
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'anyps5-iso-native-'));
  const fixtures = [];
  console.log('Native ISO fixtures: ' + root);
  try {
    const runtime = { libs: path.join(root, 'runtime'), destinationParent: path.join(root, 'installed'), relinker: path.join(root, 'relinker.exe') };
    await write(runtime.relinker, PE);
    for (const name of ['libkernel.prx', ...DLLS]) await write(path.join(runtime.libs, name), PE);
    await fs.mkdir(runtime.destinationParent);
    const ipcOnly = process.env.ANYPS5_ISO_IPC_ONLY === '1';
    for (const variant of ipcOnly ? ['overlay'] : ['plain', 'overlay']) {
      const fixture = await createFixture(root, variant); fixtures.push(fixture);
      if (!ipcOnly) await verifyImage(root, fixture, runtime);
    }
    await verifyDesktopIpc(root, fixtures.at(-1), runtime);
    if (ipcOnly) return;
    const fixture = fixtures[0];
    const sentinel = new Error('Intentional native fixture callback failure.');
    await assert.rejects(withMountedIso(fixture.image, async () => { throw sentinel; }), error => error === sentinel);
    assert.equal(await native(root, fixture.image, 'status'), false, 'Callback failure must detach its own mount.');
    console.log('PASS: native callback failure detaches its own image.');
    await native(root, fixture.image, 'mount');
    try {
      assert.equal(await native(root, fixture.image, 'status'), true);
      await withMountedIso(fixture.image, async volumeRoot => {
        assert.equal((await inspectSource(volumeRoot)).title, 'Native ISO plain');
      });
      assert.equal(await native(root, fixture.image, 'status'), true, 'An existing mount must remain mounted.');
      await assert.rejects(withMountedIso(fixture.image, async () => { throw sentinel; }), error => error === sentinel);
      assert.equal(await native(root, fixture.image, 'status'), true, 'An existing mount must also survive callback failure.');
    } finally { await native(root, fixture.image, 'unmount'); }
    assert.equal(await native(root, fixture.image, 'status'), false);
    for (const item of fixtures) assert.equal(await hash(item.image), item.originalHash);
    console.log('PASS: pre-mounted fixture stays mounted after success/failure; test-owned final detach and ISO SHA-256 verified.');
  } finally {
    let safeToRemove = true;
    for (const variant of ['plain', 'overlay']) {
      const image = path.join(root, variant + '.iso');
      if (await fs.access(image).then(() => true, () => false)) {
        try { if (await native(root, image, 'status')) await native(root, image, 'unmount'); }
        catch (error) { safeToRemove = false; console.error('Fixture detach failed; preserving ' + image + ': ' + error.message); }
      }
    }
    const resolved = path.resolve(root);
    assert.equal(path.dirname(resolved), path.resolve(os.tmpdir()));
    assert.match(path.basename(resolved), /^anyps5-iso-native-[a-zA-Z0-9-]+$/);
    if (safeToRemove) await fs.rm(resolved, { recursive: true, force: true, maxRetries: 3, retryDelay: 250 });
    else throw new Error('Owned native fixture requires cleanup: ' + root);
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
