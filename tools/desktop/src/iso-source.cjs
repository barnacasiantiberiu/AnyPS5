const fs = require('node:fs/promises');
const syncFs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { inspectSource, checkEntry, within } = require('./source-layout.cjs');

const activeImages = new Set();
function aborted(signal) {
  if (signal?.aborted) {
    const error = new Error('ISO operation was cancelled. Waiting for the temporary mount to be released.');
    error.name = 'AbortError';
    throw error;
  }
}
function emit(options, event) {
  try { options.onEvent?.(event); } catch {}
}
async function validateImage(imagePath) {
  if (typeof imagePath !== 'string' || !path.isAbsolute(imagePath) || path.extname(imagePath).toLowerCase() !== '.iso') {
    throw new Error('Choose an absolute path to an ISO image.');
  }
  const info = await fs.lstat(imagePath).catch(() => null);
  if (!info?.isFile()) throw new Error('The ISO image must be an existing regular file.');
  if (!info.size) throw new Error('The ISO image is empty. Wait for it to finish copying or extracting.');
  return fs.realpath(imagePath);
}
function runPowerShell(action, imagePath, mount) {
  if (process.platform !== 'win32') return Promise.reject(new Error('Reading ISO images requires Windows.'));
  const script = syncFs.readFileSync(path.join(__dirname, 'iso-source.ps1'), 'utf8');
  const executable = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
  return new Promise((resolve, reject) => {
    // Fixed code only. Paths never become PowerShell syntax, including paths with quotes or brackets.
    const child = spawn(executable, ['-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', Buffer.from(script, 'utf16le').toString('base64')], {
      windowsHide: true, shell: false, stdio: ['ignore', 'pipe', 'pipe'],
      env: { ...process.env, ANYPS5_ISO_ACTION: action, ANYPS5_ISO_PATH: imagePath, ANYPS5_ISO_DEVICE: mount?.devicePath || '' }
    });
    let output = '', errors = '', spawnError;
    child.stdout.setEncoding('utf8'); child.stderr.setEncoding('utf8');
    child.stdout.on('data', text => { output = (output + text).slice(-128 * 1024); });
    child.stderr.on('data', text => { errors = (errors + text).slice(-16 * 1024); });
    child.on('error', error => { spawnError = error; });
    // Do not kill mount/dismount on cancellation: wait for process close, obtain ownership, then detach.
    child.on('close', code => {
      if (spawnError) { reject(new Error('Windows ISO support could not start: ' + spawnError.message)); return; }
      let response;
      try { response = JSON.parse(output.trim().replace(/^\uFEFF/, '')); } catch {}
      if (response?.ok === true && code === 0) { resolve(response); return; }
      const error = new Error(response?.error || 'Windows could not ' + (action === 'mount' ? 'mount the ISO read-only' : 'detach the temporary ISO mount') + (errors.trim() ? ': ' + errors.trim() : '.'));
      if (response?.cleanupWarning) error.cleanupWarning = response.cleanupWarning;
      reject(error);
    });
  });
}
const windowsAdapter = {
  mount: imagePath => runPowerShell('mount', imagePath),
  unmount: (imagePath, mount) => runPowerShell('unmount', imagePath, mount)
};

async function withMountedIso(imagePath, callback, options = {}) {
  if (typeof callback !== 'function') throw new TypeError('An ISO operation callback is required.');
  aborted(options.signal);
  const image = await validateImage(imagePath);
  aborted(options.signal);
  const key = process.platform === 'win32' ? image.toLowerCase() : image;
  if (activeImages.has(key)) throw new Error('This ISO is already being used by another operation. Wait for it to finish.');
  activeImages.add(key);
  const adapter = options.adapter || windowsAdapter;
  let mount, result, failure, cleanupWarning;
  try {
    emit(options, { kind: 'log', text: 'Opening ISO read-only: ' + image + '\n' });
    mount = await adapter.mount(image);
    if (!mount || typeof mount.root !== 'string' || typeof mount.owned !== 'boolean') throw new Error('Windows returned an invalid ISO mount result.');
    aborted(options.signal);
    const root = await fs.realpath(mount.root).catch(() => { throw new Error('The mounted ISO has no readable filesystem volume. It may be incomplete or use an unsupported filesystem.'); });
    if (!(await fs.stat(root)).isDirectory()) throw new Error('The mounted ISO volume is not a readable folder.');
    // The callback owns its commit boundary. A late cancel must not turn a published install into failure.
    result = await callback(root);
  } catch (error) {
    failure = error;
    if (error.cleanupWarning) cleanupWarning = error.cleanupWarning;
  } finally {
    if (mount?.owned === true) {
      try {
        await adapter.unmount(image, mount);
        emit(options, { kind: 'log', text: 'Temporary ISO mount released.\n' });
      } catch (error) {
        cleanupWarning = 'Windows could not detach the temporary ISO mount after this operation. You can eject it in File Explorer. ' + error.message;
      }
    }
    activeImages.delete(key);
    if (cleanupWarning) {
      emit(options, { kind: 'warning', text: cleanupWarning, warning: cleanupWarning });
      if (failure) failure.cleanupWarning = cleanupWarning;
    }
  }
  if (failure) throw failure;
  return cleanupWarning && result && typeof result === 'object' && !Array.isArray(result) ? { ...result, cleanupWarning } : result;
}
function relativePath(root, file) {
  if (!within(root, file)) throw new Error('The detected game path is outside the mounted ISO volume.');
  return path.relative(root, file).split(path.sep).join('\\');
}
function entryParts(entry) {
  if (typeof entry !== 'string' || !entry.trim()) throw new Error('Choose a decrypted ELF executable inside the ISO.');
  if (path.win32.isAbsolute(entry) || path.posix.isAbsolute(entry) || /[:\x00-\x1f]/.test(entry)) {
    throw new Error('The ISO executable must be a relative path inside the image.');
  }
  const parts = entry.split(/[\\/]/);
  if (parts.some(part => !part || part === '..' || part === '.' || /[. ]$/.test(part))) throw new Error('The ISO executable path contains an invalid or parent-directory component.');
  return parts;
}
const pkgDiagnostic = location => 'This ISO contains PKG packages' + (location ? ' in ' + location : '') + ', not an extracted game folder. Extracted game files with a decrypted ELF executable are required; PKG extraction and decryption are not supported.';
async function inspectVolume(root) {
  const inspection = await inspectSource(root);
  if (!within(root, inspection.source)) throw new Error('The detected game folder is outside the mounted ISO volume.');
  if (inspection.entry) return inspection;
  const entries = await fs.readdir(inspection.source, { withFileTypes: true });
  let packageLocation = entries.some(entry => entry.isFile() && /\.pkg$/i.test(entry.name)) ? relativePath(root, inspection.source) : null;
  // PS5 discs can have PS5/, bd/ and license/ beside app/<title>. Probe only that known level.
  // Never recurse through package contents, or open the large .pkg payloads.
  const rootEntries = inspection.source === root ? entries : await fs.readdir(root, { withFileTypes: true });
  const appDirectory = rootEntries.find(entry => entry.isDirectory() && !entry.isSymbolicLink() && entry.name.toLowerCase() === 'app');
  if (appDirectory) {
    const app = path.join(root, appDirectory.name);
    const appEntries = await fs.readdir(app, { withFileTypes: true });
    if (appEntries.some(entry => entry.isFile() && /\.pkg$/i.test(entry.name))) packageLocation ??= relativePath(root, app);
    const children = appEntries.filter(entry => entry.isDirectory() && !entry.isSymbolicLink());
    if (children.length > 64) return {
      ...inspection, entry: '', executables: [], ambiguous: true,
      diagnostic: 'This ISO contains too many app folders to select a game safely. Choose a single extracted game folder instead.'
    };
    const games = [];
    for (const child of children) {
      const directory = path.join(app, child.name);
      const childEntries = await fs.readdir(directory, { withFileTypes: true });
      if (childEntries.some(entry => entry.isFile() && /\.pkg$/i.test(entry.name))) packageLocation ??= relativePath(root, directory);
      const hasExecutable = childEntries.some(entry => entry.isFile() && (/\.elf$/i.test(entry.name) || /^(eboot\.bin|eboot)$/i.test(entry.name)));
      if (!hasExecutable) continue;
      const game = await inspectSource(directory);
      if (!within(root, game.source)) throw new Error('The detected game folder is outside the mounted ISO volume.');
      games.push(game);
    }
    if (games.length > 1) return {
      ...inspection, entry: '', executables: [], ambiguous: true,
      diagnostic: 'This ISO contains multiple extracted game folders under app. Choose a single extracted game folder instead.'
    };
    if (games.length === 1) return games[0];
  }
  if (packageLocation !== null) inspection.diagnostic = pkgDiagnostic(packageLocation);
  return inspection;
}
async function inspectIso(imagePath, options = {}) {
  return withMountedIso(imagePath, async root => {
    const inspection = await inspectVolume(root);
    return {
      ...inspection,
      source: await fs.realpath(imagePath), sourceType: 'iso',
      title: inspection.title && !(inspection.source === root && !inspection.entry && !inspection.titleId)
        ? inspection.title : path.basename(imagePath, path.extname(imagePath)),
      sourceFolder: relativePath(root, inspection.source),
      entry: inspection.entry ? relativePath(root, inspection.entry) : '',
      executables: inspection.executables.map(file => relativePath(root, file)),
      decryptedRoot: inspection.decryptedRoot ? relativePath(root, inspection.decryptedRoot) : '',
      note: ['The ISO is opened read-only. Its original files remain unchanged.', inspection.note].filter(Boolean).join(' ')
    };
  }, options);
}
function isIsoRequest(request) {
  return request?.sourceType === 'iso' || (request?.sourceType !== 'folder' && typeof request?.source === 'string' && path.extname(request.source).toLowerCase() === '.iso');
}
async function resolveIsoRequest(request, callback, options = {}) {
  if (!isIsoRequest(request)) { aborted(options.signal); return callback(request); }
  const parts = entryParts(request.entry);
  return withMountedIso(request.source, async root => {
    const inspection = await inspectVolume(root);
    if (inspection.ambiguous) throw new Error(inspection.diagnostic);
    const candidate = path.resolve(root, ...parts);
    if (!within(root, candidate)) throw new Error('The ISO executable is outside the mounted image.');
    const realEntry = await fs.realpath(candidate).catch(() => { throw new Error('The selected executable could not be found inside the ISO. Inspect the image again.'); });
    if (!within(root, realEntry)) throw new Error('The ISO executable resolves outside the mounted image.');
    const entry = await checkEntry({ source: inspection.source, entry: realEntry });
    return callback({ ...request, source: inspection.source, sourceType: 'folder', entry }, { volumeRoot: root, isoSource: request.source });
  }, options);
}
async function checkIsoEntry(request, options = {}) {
  return resolveIsoRequest({ ...request, sourceType: 'iso' }, (resolved, details) => relativePath(details.volumeRoot, resolved.entry), options);
}

module.exports = { withMountedIso, inspectIso, resolveIsoRequest, checkIsoEntry, isIsoRequest, windowsAdapter };
