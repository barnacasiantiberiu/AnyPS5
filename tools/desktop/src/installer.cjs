const fs = require('node:fs/promises');
const path = require('node:path');
const { spawn } = require('node:child_process');
const crypto = require('node:crypto');

const DLLS = ['libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll'];
const { MODULE_DIRS, within, magic, elfMagic, selfMagic, isElf, inspectSource, checkEntry } = require('./source-layout.cjs');
const exists = async p => !!(await fs.lstat(p).catch(() => null));
function checkAbort(signal) {
  if (signal?.aborted) throw new Error('Conversion was cancelled. Your source files remain unchanged.');
}
async function requirePe(file, label) {
  const invalid = () => new Error(`${label} is not a valid Windows x64 executable: ${file}`);
  let handle;
  try {
    handle = await fs.open(file, 'r');
    const header = Buffer.alloc(64);
    if ((await handle.read(header, 0, 64, 0)).bytesRead !== 64 || header[0] !== 77 || header[1] !== 90) throw invalid();
    const offset = header.readUInt32LE(60);
    const signature = Buffer.alloc(6);
    if (offset < 64 || (await handle.read(signature, 0, 6, offset)).bytesRead !== 6 || signature.readUInt32LE(0) !== 0x4550 || signature.readUInt16LE(4) !== 0x8664) throw invalid();
  } catch { throw invalid(); }
  finally { if (handle) await handle.close(); }
}
async function walk(root, signal) {
  const files = [];
  const directories = [];
  async function visit(directory) {
    checkAbort(signal);
    for (const entry of await fs.readdir(directory, { withFileTypes: true })) {
      const full = path.join(directory, entry.name);
      if (entry.isSymbolicLink()) throw new Error(`The folder contains a symbolic link: ${full}. Use a copy containing regular files.`);
      if (entry.isDirectory()) { directories.push(path.relative(root, full)); await visit(full); }
      else if (entry.isFile()) files.push({ absolute: full, relative: path.relative(root, full), size: (await fs.stat(full)).size });
    }
  }
  await visit(root);
  return { files, directories };
}
async function destinationPath(directory) {
  if (typeof directory !== 'string' || !path.isAbsolute(directory)) throw new Error('Choose an absolute path for the destination.');
  let ancestor = path.resolve(directory);
  const missing = [];
  while (!(await exists(ancestor))) {
    const parent = path.dirname(ancestor);
    if (parent === ancestor) throw new Error('The destination drive is unavailable.');
    missing.unshift(path.basename(ancestor)); ancestor = parent;
  }
  if (!(await fs.stat(ancestor)).isDirectory()) throw new Error('The destination is not a folder.');
  const real = await fs.realpath(ancestor);
  return { parent: path.join(real, ...missing), existing: real };
}
function safeName(title) {
  const result = String(title || '').trim().replace(/[<>:"/\\|?*\x00-\x1f]/g, '').replace(/[. ]+$/g, '').slice(0, 100).trim();
  if (!result || /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(result)) throw new Error('Choose a valid game name.');
  return result;
}
async function validate(request, signal) {
  checkAbort(signal);
  const inspection = await inspectSource(request.source);
  const source = inspection.source;
  const entry = await checkEntry({ source, entry: request.entry });
  const names = (await fs.readdir(source, { withFileTypes: true })).filter(e => e.isDirectory()).map(e => e.name);
  if (names.includes('sce_module') && names.includes('sce_modules')) throw new Error('The source contains both sce_module and sce_modules. AnyPS5 accepts only one of them.');
  if (!MODULE_DIRS.some(d => names.includes(d))) throw new Error('The game source must contain a sce_module, sce_modules or prx folder.');
  const relinker = await fs.realpath(request.relinker).catch(() => { throw new Error('Configure relinker.exe in Settings.'); });
  await requirePe(relinker, 'Relinker');
  const libs = await fs.realpath(request.libs).catch(() => { throw new Error('Configure the AnyPS5 libraries folder in Settings.'); });
  const libFiles = (await fs.readdir(libs, { withFileTypes: true })).filter(e => e.isFile() && /\.(prx|dll)$/i.test(e.name));
  if (!libFiles.some(e => /\.prx$/i.test(e.name))) throw new Error('The libraries folder contains no .prx files built for Windows.');
  const runtimeFiles = libFiles.map(e => ({ absolute: path.join(libs, e.name), name: e.name }));
  for (const file of runtimeFiles) await requirePe(file.absolute, 'The library');
  const candidates = [libs, path.dirname(relinker), request.dllDirectory].filter(Boolean);
  for (const name of DLLS) {
    if (runtimeFiles.some(f => f.name.toLowerCase() === name.toLowerCase())) continue;
    let found;
    for (const directory of candidates) if (await exists(path.join(directory, name))) { found = path.join(directory, name); break; }
    if (!found) throw new Error(`${name} is missing. Select the bin folder of the toolchain used to build AnyPS5 in Settings.`);
    await requirePe(found, 'DLL');
    runtimeFiles.push({ absolute: found, name });
  }
  const destination = await destinationPath(request.destinationParent);
  const destinationParent = destination.parent;
  const title = safeName(request.title);
  const target = path.join(destinationParent, title);
  if (within(source, target) || within(target, source) || within(target, libs) || within(target, relinker)) throw new Error('The destination must be separate from the game source and the AnyPS5 files.');
  if (await exists(target)) throw new Error('A folder with this name already exists at the destination. Choose a different name or destination.');
  const { files, directories } = await walk(source, signal);
  const overlayRoot = inspection.decryptedRoot;
  const overlayRelative = overlayRoot ? path.relative(source, overlayRoot) : '';
  const isUnder = (relative, directory) => relative.toLowerCase() === directory.toLowerCase() || relative.toLowerCase().startsWith(directory.toLowerCase() + path.sep);
  const helpers = new Set(overlayRoot ? [overlayRelative] : []);
  // Recognize only these dump helpers, and only with validated Windows providers.
  if (overlayRoot && names.includes('fakelib')) {
    const stubs = files.filter(f => isUnder(f.relative, 'fakelib'));
    const runtimeNames = new Set(runtimeFiles.map(f => f.name.replace(/\.(s?prx|dll)$/i, '').toLowerCase()));
    for (const stub of stubs) {
      checkAbort(signal);
      const name = path.basename(stub.relative).replace(/\.s?prx$/i, '').toLowerCase();
      if (path.dirname(stub.relative) !== 'fakelib' || !['libsceampr', 'libsceplaygo'].includes(name) || !/\.sprx$/i.test(stub.relative) || !selfMagic(await magic(stub.absolute)) || !runtimeNames.has(name)) {
        throw new Error('The fakelib helper contains a file without a recognized Windows runtime replacement: ' + stub.relative);
      }
    }
    helpers.add('fakelib');
  }
  const ordinary = files.filter(f => ![...helpers].some(dir => isUnder(f.relative, dir)));
  const originals = new Map(ordinary.map(f => [f.relative.toLowerCase(), f]));
  const replacements = new Map();
  if (overlayRoot) {
    for (const file of files.filter(f => within(overlayRoot, f.absolute))) {
      checkAbort(signal);
      if (!elfMagic(await magic(file.absolute))) continue;
      const relative = path.relative(overlayRoot, file.absolute);
      const original = originals.get(relative.toLowerCase());
      if (!original) throw new Error('The decrypted file has no matching original game file: ' + relative);
      const header = await magic(original.absolute);
      if (!elfMagic(header) && !selfMagic(header)) throw new Error('The decrypted file does not match a game executable or module: ' + relative);
      replacements.set(original.relative.toLowerCase(), { ...file, relative: original.relative });
    }
  }
  const entryRelative = overlayRoot && within(overlayRoot, entry) ? path.relative(overlayRoot, entry) : path.relative(source, entry);
  const effectiveFiles = ordinary.map(f => replacements.get(f.relative.toLowerCase()) || f);
  const moduleDirs = new Set(MODULE_DIRS.filter(d => names.includes(d)));
  const excluded = new Set();
  const inputFiles = [];
  for (const file of effectiveFiles) {
    checkAbort(signal);
    const isEntry = file.relative.toLowerCase() === entryRelative.toLowerCase();
    if (isEntry || /\.(prx|sprx|elf|so)$/i.test(file.relative) || MODULE_DIRS.includes(path.dirname(file.relative))) {
      const header = await magic(isEntry ? entry : file.absolute);
      if (selfMagic(header)) throw new Error('Module ' + file.relative + ' is a SELF file. Conversion requires decrypted ELF modules; its matching decrypted replacement is missing.');
      if (elfMagic(header)) {
        if (!isEntry) moduleDirs.add(path.dirname(file.relative));
        excluded.add(file.relative.toLowerCase());
        inputFiles.push(isEntry ? { absolute: entry, relative: file.relative, size: (await fs.stat(entry)).size } : file);
      }
    }
  }
  if (!inputFiles.some(f => f.relative.toLowerCase() === entryRelative.toLowerCase())) throw new Error('The selected executable is not part of the detected game layout.');
  const resourceFiles = effectiveFiles.filter(f => !excluded.has(f.relative.toLowerCase()));
  const resourceDirectories = directories.filter(dir => ![...helpers].some(helper => isUnder(dir, helper)));

  const runtimeBytes = (await Promise.all(runtimeFiles.map(f => fs.stat(f.absolute)))).reduce((n, s) => n + s.size, 0);
  const inputBytes = inputFiles.reduce((sum, file) => sum + file.size, 0);
  // Account for generated output plus the temporary ELF copies that coexist with it.
  const totalBytes = resourceFiles.reduce((sum, file) => sum + file.size, 0) + inputBytes + runtimeBytes + (overlayRoot ? inputBytes : 0);
  const disk = await fs.statfs(destination.existing);
  const freeBytes = Number(disk.bavail) * Number(disk.bsize);
  if (freeBytes < totalBytes + 128 * 1024 * 1024) throw new Error('There is not enough free space at the destination for the game and converted files.');
  return { source, entry, title, target, destinationParent, relinker, libs, runtimeFiles, resourceFiles, resourceDirectories, inputFiles, entryRelative, normalizedInput: Boolean(overlayRoot), ignoredHelpers: [...helpers], moduleDirs: [...moduleDirs].filter(d => !MODULE_DIRS.includes(d)), totalBytes, freeBytes };
}
function runProcess(executable, args, options, onLog, signal) {
  return new Promise((resolve, reject) => {
    checkAbort(signal);
    const child = spawn(executable, args, { cwd: options.cwd, shell: false, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
    const stop = () => {
      if (!child.pid) return;
      if (process.platform === 'win32') spawn('taskkill', ['/pid', String(child.pid), '/t', '/f'], { windowsHide: true, stdio: 'ignore' }).on('error', () => child.kill());
      else child.kill('SIGTERM');
    };
    signal?.addEventListener('abort', stop, { once: true });
    let settled = false;
    function finish(error) {
      if (settled) return;
      settled = true;
      signal?.removeEventListener('abort', stop);
      error ? reject(error) : resolve();
    }
    child.stdout.setEncoding('utf8'); child.stderr.setEncoding('utf8');
    child.stdout.on('data', onLog); child.stderr.on('data', onLog);
    child.on('error', error => finish(error));
    child.on('close', code => finish(signal?.aborted ? new Error('Conversion was cancelled.') : code === 0 ? null : new Error(`Relinker exited with code ${code}. See the log for details.`)));
    if (signal?.aborted) stop();
  });
}
async function install(request, options = {}) {
  const { signal, onEvent = () => {}, run = runProcess } = options;
  const logs = [];
  const log = text => { logs.push(String(text)); if (logs.length > 4000) logs.shift(); onEvent({ kind: 'log', text: String(text) }); };
  const stages = { checking: 1, copying: 2, converting: 3, libraries: 4, finishing: 5, complete: 5 };
  const phase = (stage, label, progress = null) => onEvent({ kind: 'phase', stage, stageIndex: stages[stage], stageCount: 5, label, progress, complete: stage === 'complete' });
  let staging;
  let plan;
  try {
    phase('checking', 'Checking game files');
    plan = await validate(request, signal);
    checkAbort(signal);
    await fs.mkdir(plan.destinationParent, { recursive: true });
    staging = await fs.mkdtemp(path.join(plan.destinationParent, '.anyps5-install-'));
    const app0 = path.join(staging, 'app0');
    await fs.mkdir(app0);
    phase('copying', 'Copying game files', 0);
    const copyFiles = plan.resourceFiles.concat(plan.normalizedInput ? plan.inputFiles : []);
    if (plan.normalizedInput) log('Using decrypted binaries with the original game resources.\nHelper folders excluded from the installed game: ' + plan.ignoredHelpers.join(', ') + '\n');
    for (const relative of plan.resourceDirectories) {
      checkAbort(signal);
      const target = path.join(app0, relative);
      if (!within(app0, target)) throw new Error('Invalid resource path.');
      await fs.mkdir(target, { recursive: true });
    }
    let copied = 0;
    const copyBytes = copyFiles.reduce((sum, file) => sum + file.size, 0);
    let lastUpdate = 0;
    for (const file of copyFiles) {
      checkAbort(signal);
      const target = path.join(app0, file.relative);
      if (!within(app0, target)) throw new Error('Invalid resource path.');
      await fs.mkdir(path.dirname(target), { recursive: true });
      await fs.copyFile(file.absolute, target);
      copied += file.size;
      if (Date.now() - lastUpdate > 120) { phase('copying', 'Copying game files', copyBytes ? Math.round(copied * 100 / copyBytes) : 100); lastUpdate = Date.now(); }
    }
    checkAbort(signal);
    phase('copying', 'Copying game files', 100);
    phase('converting', 'Converting the executable and modules');
    const conversionSource = plan.normalizedInput ? app0 : plan.source;
    const conversionEntry = plan.normalizedInput ? path.join(app0, plan.entryRelative) : plan.entry;
    const args = ['--windows', '--windows-gui', '--windows-diagnostics', '--registry', '--sce-module-path', conversionSource];
    if (request.toIntel) args.push('--to-intel');
    for (const directory of plan.moduleDirs) args.push('--module-dir', directory);
    args.push(conversionEntry, path.join(staging, 'app.exe'));
    log(`AnyPS5: ${plan.relinker}\nSource: ${plan.entry}\n`);
    await run(plan.relinker, args, { cwd: staging }, log, signal);
    checkAbort(signal);
    await requirePe(path.join(staging, 'app.exe'), 'The converted executable');
    if (plan.normalizedInput) {
      // These are copies created above inside this install's freshly-created staging folder.
      const stagingRoot = await fs.realpath(staging);
      const app0Root = await fs.realpath(app0);
      if (path.dirname(stagingRoot) !== plan.destinationParent || !path.basename(stagingRoot).startsWith('.anyps5-install-') || app0Root !== path.join(stagingRoot, 'app0') || within(plan.source, stagingRoot)) throw new Error('Invalid temporary input cleanup path.');
      for (const input of plan.inputFiles) {
        checkAbort(signal);
        const temporaryInput = await fs.realpath(path.join(app0Root, input.relative));
        if (!within(app0Root, temporaryInput) || temporaryInput === app0Root || within(plan.source, temporaryInput)) throw new Error('Invalid temporary input path.');
        await fs.unlink(temporaryInput);
      }
    }
    phase('libraries', 'Installing Windows libraries');
    await fs.mkdir(path.join(staging, 'libs'));
    for (const file of plan.runtimeFiles) { checkAbort(signal); await fs.copyFile(file.absolute, path.join(staging, 'libs', file.name)); }
    phase('finishing', 'Finishing installation');
    const manifest = { schemaVersion: 1, id: crypto.randomUUID(), title: plan.title, titleId: String(request.titleId || ''), executable: 'app.exe', installedAt: new Date().toISOString(), toIntel: Boolean(request.toIntel), compatibility: 'untested' };
    await fs.writeFile(path.join(staging, 'anyps5-game.json'), JSON.stringify(manifest, null, 2));
    await fs.writeFile(path.join(staging, 'conversion.log'), logs.join(''));
    checkAbort(signal);
    if (await exists(plan.target)) throw new Error('The destination folder was created during conversion. No existing files were replaced.');
    await fs.rename(staging, plan.target);
    staging = null;
    phase('complete', 'Conversion complete', 100);
    return { ...manifest, directory: plan.target };
  } catch (error) {
    error.log = logs.join('');
    throw error;
  } finally {
    if (staging && plan && path.dirname(staging) === plan.destinationParent && path.basename(staging).startsWith('.anyps5-install-')) {
      const actual = await fs.realpath(staging).catch(() => null);
      if (actual && within(plan.destinationParent, actual) && actual !== plan.destinationParent) await fs.rm(actual, { recursive: true, force: true });
    }
  }
}
module.exports = { DLLS, inspectSource, checkEntry, safeName, within, isElf, requirePe, validate, install, runProcess };
