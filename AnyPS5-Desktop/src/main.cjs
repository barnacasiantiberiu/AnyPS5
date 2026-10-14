const { app, BrowserWindow, ipcMain, dialog, shell } = require('electron');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const { pathToFileURL } = require('node:url');
const crypto = require('node:crypto');
const { spawn } = require('node:child_process');
const { inspectSource, checkEntry, install, requirePe, within, safeName } = require('./installer.cjs');
const { defaultInput, validateInput, prepareInput } = require('./input-profile.cjs');
const { inspectIso, resolveIsoRequest } = require('./iso-source.cjs');
const { resolutions, defaultGraphics, validateGraphics, prepareGraphics } = require('./graphics-settings.cjs');
const { defaultPlayers, validatePlayers, migratePlayerSettings } = require('./player-settings.cjs');

app.setName('AnyPS5 Desktop');
const portableData = path.join(path.dirname(app.getPath('exe')), 'portable-data');
const dataDirectory = process.env.ANYPS5_DESKTOP_TEST_DATA || process.env.ANYPS5_DESKTOP_DATA || (app.isPackaged && require('node:fs').existsSync(portableData) ? portableData : null);
if (dataDirectory) app.setPath('userData', dataDirectory);
const { createLoadingOverlay } = require('./loading-overlay.cjs');
const loadingOverlays = new Set();
const runningGames = new Set();
let window;
let activeJob;
let closeAfterJob = false;
let saveQueue = Promise.resolve();
const rendererPath = path.join(__dirname, 'ui', 'index.html');
const pageUrl = pathToFileURL(rendererPath).href;
const ancestors = [];
for (let directory = app.isPackaged ? path.dirname(app.getPath('exe')) : __dirname; path.dirname(directory) !== directory; directory = path.dirname(directory)) ancestors.push(directory);
const repository = ancestors.find(directory => require('node:fs').existsSync(path.join(directory, 'core', 'relinker', 'CMakeLists.txt'))) || path.dirname(app.getPath('exe'));
const defaults = () => ({ relinker: path.join(repository, 'build', 'core', 'relinker', 'relinker.exe'), libs: path.join(repository, 'build', 'core', 'libs', 'libs'), dllDirectory: path.join(repository, 'build_tools', 'winlibs', 'mingw64', 'bin'), destinationParent: path.join(app.getPath('documents'), 'AnyPS5 Games'), toIntel: /Intel/i.test(os.cpus()[0]?.model || ''), players: defaultPlayers(), graphics: defaultGraphics(), input: defaultInput(), audio: { enabled: true, musicVolume: 25, effectsVolume: 45 } });
async function loadState() {
  try {
    const data = JSON.parse(await fs.readFile(path.join(app.getPath('userData'), 'library.json'), 'utf8'));
    if (!Array.isArray(data.games)) throw new Error('Unknown library format.');
    return { settings: { ...defaults(), ...migratePlayerSettings(data.settings) }, games: data.games, browsePaths: data.browsePaths && typeof data.browsePaths === 'object' && !Array.isArray(data.browsePaths) ? data.browsePaths : {} };
  } catch (error) {
    if (error.code === 'ENOENT') return { settings: defaults(), games: [], browsePaths: {} };
    throw new Error(`The library could not be read. Your saved data has been preserved. ${error.message}`);
  }
}
function updateState(update) {
  const result = saveQueue.then(async () => {
    const state = await loadState();
    await update(state);
    const directory = app.getPath('userData');
    await fs.mkdir(directory, { recursive: true });
    const temporary = path.join(directory, `library-${crypto.randomUUID()}.tmp`);
    await fs.writeFile(temporary, JSON.stringify(state, null, 2));
    await fs.rename(temporary, path.join(directory, 'library.json'));
    return state;
  });
  saveQueue = result.catch(() => {});
  return result;
}
async function artwork(directory, background = false) {
  const candidates = background ? ['pic1.png', 'pic0.png', 'icon0.png'] : ['icon0.png'];
  for (const name of candidates) {
    const file = path.join(directory, 'app0', 'sce_sys', name);
    try {
      const info = await fs.stat(file);
      if (info.size > 12 * 1024 * 1024) continue;
      const bytes = await fs.readFile(file);
      if (bytes.subarray(0, 8).equals(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]))) return `data:image/png;base64,${bytes.toString('base64')}`;
    } catch {}
  }
  return null;
}
async function viewState() {
  const state = await loadState();
  const ready = await Promise.all([
    requirePe(state.settings.relinker, 'Relinker').then(() => true, () => false),
    fs.readdir(state.settings.libs).then(files => files.some(file => /\.prx$/i.test(file)), () => false)
  ]);
  return { ...state, resolutions, runtimeReady: ready.every(Boolean), cpu: os.cpus()[0]?.model || '', version: app.getVersion(), fullscreen: window?.isFullScreen() || false, games: await Promise.all(state.games.map(async game => ({ ...game, cover: await artwork(game.directory), available: await fs.access(path.join(game.directory, game.executable)).then(() => true, () => false) }))) };
}
async function knownGame(id) {
  const game = (await loadState()).games.find(g => g.id === id);
  if (!game || !within(game.directory, path.join(game.directory, game.executable))) throw new Error('The game is not in your library.');
  return game;
}
function handle(name, handler) {
  ipcMain.handle(name, async (event, ...args) => {
    if (event.sender !== window.webContents || event.senderFrame !== window.webContents.mainFrame || event.senderFrame.url !== pageUrl) throw new Error('Unauthorized window.');
    try { return { ok: true, value: await handler(...args) }; }
    catch (error) { return { ok: false, error: [error.message, error.cleanupWarning].filter(Boolean).join('\n'), log: error.log || '' }; }
  });
}
async function browseDirectory(candidates) {
  for (const candidate of candidates) {
    if (typeof candidate !== 'string' || !path.isAbsolute(candidate)) continue;
    let current = path.resolve(candidate);
    while (true) {
      const info = await fs.stat(current).catch(() => null);
      if (info?.isDirectory()) return current;
      const parent = path.dirname(current);
      if (parent === current) break;
      current = parent;
    }
  }
  return app.getPath('documents');
}
async function withActiveJob(kind, callback) {
  if (activeJob) throw new Error('Another operation is already in progress. Wait for it to finish.');
  const controller = new AbortController();
  controller.kind = kind;
  activeJob = controller;
  try { return await callback(controller.signal); }
  finally {
    activeJob = null;
    if (closeAfterJob) window.close();
  }
}
function sendProgress(event) {
  if (window && !window.isDestroyed()) window.webContents.send('installation:event', event);
}
function registerHandlers() {
  handle('state:get', viewState);
  handle('window:close', () => { setImmediate(() => window.close()); return true; });
  handle('window:visibility', () => window.isVisible() && !window.isMinimized());
  handle('window:fullscreen', async () => { window.setFullScreen(!window.isFullScreen()); return window.isFullScreen(); });
  handle('dialog:pick', async type => {
    const configs = {
      source: { title: 'Choose the PS5 game folder', properties: ['openDirectory'] },
      iso: { title: 'Choose the game ISO image', properties: ['openFile'], filters: [{ name: 'ISO image', extensions: ['iso'] }] },
      destination: { title: 'Choose where to install your games', properties: ['openDirectory', 'createDirectory'] },
      relinker: { title: 'Choose relinker.exe for Windows', properties: ['openFile'], filters: [{ name: 'Windows executable', extensions: ['exe'] }] },
      libs: { title: 'Choose the AnyPS5 libraries built for Windows', properties: ['openDirectory'] },
      dllDirectory: { title: 'Choose the bin folder containing the MinGW DLLs', properties: ['openDirectory'] },
      entry: { title: 'Choose the ELF or unencrypted SELF executable', properties: ['openFile'], filters: [{ name: 'PS5 executable', extensions: ['elf', 'self', 'bin'] }, { name: 'All files', extensions: ['*'] }] },
      converted: { title: 'Choose a converted game folder', properties: ['openDirectory'] }
    };
    if (!Object.hasOwn(configs, type)) throw new Error('Unknown selection.');
    const state = await loadState();
    const settingsPath = { destination: state.settings.destinationParent, converted: state.settings.destinationParent, relinker: state.settings.relinker, libs: state.settings.libs, dllDirectory: state.settings.dllDirectory, entry: state.browsePaths.source, iso: state.browsePaths.source }[type];
    const defaultPath = await browseDirectory([state.browsePaths[type], settingsPath]);
    const result = await dialog.showOpenDialog(window, { ...configs[type], defaultPath });
    const selected = result.canceled ? null : result.filePaths[0];
    if (selected) await updateState(current => {
      current.browsePaths[type] = configs[type].properties.includes('openFile') ? path.dirname(selected) : selected;
    });
    return selected || null;
  });
  handle('source:inspect', inspectSource);
  handle('source:inspect-iso', image => withActiveJob('inspection', signal => inspectIso(image, { signal })));
  handle('source:check-entry', checkEntry);
  handle('settings:save', async settings => {
    if (!settings || typeof settings !== 'object' || Array.isArray(settings)) throw new Error('Invalid settings.');
    const clean = {};
    for (const key of ['relinker', 'libs', 'dllDirectory', 'destinationParent']) {
      if (typeof settings[key] !== 'string' || settings[key].length > 32760) throw new Error('Invalid path.');
      clean[key] = settings[key].trim();
    }
    clean.toIntel = Boolean(settings.toIntel);
    clean.players = validatePlayers(settings.players);
    if (settings.graphics !== undefined) clean.graphics = validateGraphics(settings.graphics);
    if (settings.input !== undefined) clean.input = validateInput(settings.input);
    if (settings.audio !== undefined) {
      const audio = settings.audio;
      if (!audio || typeof audio !== 'object' || typeof audio.enabled !== 'boolean' || !['musicVolume', 'effectsVolume'].every(key => Number.isInteger(audio[key]) && audio[key] >= 0 && audio[key] <= 100)) throw new Error('Audio volumes must be between 0 and 100.');
      clean.audio = { enabled: audio.enabled, musicVolume: audio.musicVolume, effectsVolume: audio.effectsVolume };
    }
    await updateState(state => { state.settings = { ...state.settings, ...clean }; });
    return viewState();
  });
  handle('installation:preflight', request => withActiveJob('checking', async signal => {
    const { validate } = require('./installer.cjs');
    const settings = (await loadState()).settings;
    const parent = request.destinationParent || settings.destinationParent;
    return resolveIsoRequest({ ...request, ...settings, destinationParent: parent, toIntel: Boolean(request.toIntel) }, async resolved => {
      const plan = await validate(resolved, signal);
      return { target: plan.target, totalBytes: plan.totalBytes, freeBytes: plan.freeBytes, resources: plan.resourceFiles.length, extraModules: plan.moduleDirs.length };
    }, { signal });
  }));
  handle('installation:start', request => withActiveJob('conversion', async signal => {
    const settings = (await loadState()).settings;
    const parent = request.destinationParent || settings.destinationParent;
    let completeEvent;
    const onEvent = event => {
      // Final completion includes releasing the ISO and recording the installed game.
      if (event.kind === 'phase' && event.complete) completeEvent = event;
      else sendProgress(event.kind === 'warning' ? { kind: 'log', text: event.text + '\n' } : event);
    };
    const result = await resolveIsoRequest({ ...request, ...settings, destinationParent: parent, toIntel: Boolean(request.toIntel) }, resolved => install(resolved, { signal, onEvent }), { signal, onEvent });
    const { cleanupWarning, ...game } = result;
    let warning = cleanupWarning || '';
    let librarySaved = true;
    try { await updateState(state => { state.games.push(game); }); }
    catch (error) {
      librarySaved = false;
      warning = [warning, 'The game was converted to ' + game.directory + ', but could not be added to your library: ' + error.message + '. Use Import converted game.'].filter(Boolean).join('\n');
    }
    if (completeEvent) sendProgress(completeEvent);
    return { game, warning, librarySaved };
  }));
  handle('installation:cancel', async () => { activeJob?.abort(); });
  handle('game:artwork', async id => artwork((await knownGame(id)).directory, true));
  handle('game:launch', async id => {
    const game = await knownGame(id);
    const executable = path.join(game.directory, game.executable);
    await requirePe(executable, 'The game');
    if (runningGames.has(executable)) throw new Error('This game is already running. Close it before changing resolution or launching it again.');
    runningGames.add(executable);
    let started = false;
    let loading;
    try {
      const settings = (await loadState()).settings;
      const input = await prepareInput(executable, app.getPath('userData'), settings.input);
      const graphics = await prepareGraphics(executable, settings.graphics);
      loading = await createLoadingOverlay(app.getPath('userData'), { gameId: game.id, title: game.title, executable });
      loadingOverlays.add(loading);
      await new Promise((resolve, reject) => {
        const child = spawn(executable, [], { cwd: game.directory, env: { ...input.env, ANYPS5_LOADING_STATUS: loading.statusPath }, detached: true, stdio: 'ignore', shell: false });
        child.once('error', reject);
        child.once('exit', (code, signal) => { runningGames.delete(executable); loading.stop(true, code === 0 && !signal ? 'exited' : 'interrupted').finally(() => loadingOverlays.delete(loading)).catch(() => {}); });
        child.once('spawn', () => { started = true; child.unref(); resolve(); });
      });
      await updateState(state => { state.games.find(g => g.id === id).lastPlayed = new Date().toISOString(); });
      return { inputSource: input.source, inputConfig: input.configPath, graphics };
    } finally {
      if (!started) {
        runningGames.delete(executable);
        if (loading) { loadingOverlays.delete(loading); await loading.stop(true, 'launch-failed'); }
      }
    }
  });
  handle('game:folder', async id => {
    const result = await shell.openPath((await knownGame(id)).directory);
    if (result) throw new Error(result);
  });
  handle('game:shortcut', async id => {
    const game = await knownGame(id);
    const file = path.join(app.getPath('desktop'), `${safeName(game.title)}.lnk`);
    if (await fs.access(file).then(() => true, () => false)) throw new Error('A shortcut with this name already exists on your desktop. It has not been replaced.');
    const executable = path.join(game.directory, game.executable);
    await requirePe(executable, 'The game');
    const args = `${app.isPackaged ? '' : `"${app.getAppPath()}" `}--loading-game "${executable}"`;
    if (!shell.writeShortcutLink(file, 'create', { target: app.getPath('exe'), args, cwd: game.directory, description: game.title, icon: executable, iconIndex: 0 })) throw new Error('The shortcut could not be created.');
    return file;
  });
  handle('game:remove', async id => {
    const game = await knownGame(id);
    const { response } = await dialog.showMessageBox(window, { type: 'question', title: 'Remove from library', message: `Remove “${game.title}” from your library?`, detail: 'The game files will remain on disk.', buttons: ['Keep', 'Remove from library'], cancelId: 0, defaultId: 0 });
    if (response === 1) await updateState(state => { state.games = state.games.filter(g => g.id !== id); });
    return viewState();
  });
  handle('game:import', async directory => {
    const real = await fs.realpath(directory);
    let metadata = {};
    try { metadata = JSON.parse(await fs.readFile(path.join(real, 'anyps5-game.json'), 'utf8')); }
    catch (error) { if (error.code !== 'ENOENT') throw new Error('The anyps5-game.json file is invalid.'); }
    let executable = metadata.executable || 'app.exe';
    if (!metadata.executable && !(await fs.access(path.join(real, 'app.exe')).then(() => true, () => false))) {
      const candidates = (await fs.readdir(real, { withFileTypes: true })).filter(entry => entry.isFile() && /\.exe$/i.test(entry.name));
      if (candidates.length === 1) executable = candidates[0].name;
      else {
        const selection = await dialog.showOpenDialog(window, { title: 'Choose the converted game executable', defaultPath: real, properties: ['openFile'], filters: [{ name: 'Windows game', extensions: ['exe'] }] });
        if (selection.canceled) throw new Error('Import was cancelled.');
        executable = path.relative(real, selection.filePaths[0]);
      }
    }
    if (typeof executable !== 'string' || path.isAbsolute(executable) || !within(real, path.join(real, executable))) throw new Error('The executable path in the manifest is invalid.');
    await requirePe(path.join(real, executable), 'The executable');
    for (const sub of ['libs', 'app0']) if (!(await fs.stat(path.join(real, sub)).catch(() => null))?.isDirectory()) throw new Error(`The ${sub} folder is missing. Choose the root folder of the converted game.`);
    const game = { id: crypto.randomUUID(), title: String(metadata.title || path.basename(real)), directory: real, executable, titleId: String(metadata.titleId || ''), installedAt: metadata.installedAt || new Date().toISOString(), compatibility: 'untested' };
    await updateState(state => {
      if (state.games.some(g => path.resolve(g.directory).toLowerCase() === real.toLowerCase())) throw new Error('The game is already in your library.');
      state.games.push(game);
    });
    return viewState();
  });
}
async function createWindow() {
  window = new BrowserWindow({ width: 1400, height: 900, fullscreen: false, minWidth: 1040, minHeight: 720, show: !process.env.ANYPS5_DESKTOP_TEST_DATA, backgroundColor: '#080c17', title: 'AnyPS5 Desktop', titleBarStyle: 'hidden', titleBarOverlay: { color: '#080c17', symbolColor: '#cad2e4', height: 38 }, webPreferences: { autoplayPolicy: 'no-user-gesture-required', backgroundThrottling: false, preload: path.join(__dirname, 'preload.cjs'), contextIsolation: true, nodeIntegration: false, sandbox: true, spellcheck: false } });
  // Native visibility pauses the ambience; keep Chromium from freezing visible frames.
  // Native event state can settle after the event; send its intended state explicitly.
  for (const event of ['hide', 'minimize']) window.on(event, () => window.webContents.send('window:visibility-changed', false));
  for (const event of ['show', 'restore', 'focus']) window.on(event, () => window.webContents.send('window:visibility-changed', true));
  window.removeMenu();
  window.on('enter-full-screen', () => window.webContents.send('window:fullscreen-changed', true));
  window.on('leave-full-screen', () => window.webContents.send('window:fullscreen-changed', false));
  window.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  window.webContents.on('will-navigate', event => event.preventDefault());
  let closing = false;
  window.on('close', event => {
    if (!activeJob) {
      if (closing) { event.preventDefault(); return; }
      if (loadingOverlays.size) {
        event.preventDefault();
        closing = true;
        Promise.allSettled([...loadingOverlays].map(loading => loading.stop(false))).finally(() => window.destroy());
        loadingOverlays.clear();
      }
      return;
    }
    event.preventDefault();
    dialog.showMessageBox(window, { type: 'question', title: 'Operation in progress', message: 'Stop the current operation and close the app?', detail: 'Wait for temporary files and ISO mounts to be released. Your source files will remain unchanged.', buttons: ['Continue', 'Stop and close'], defaultId: 0, cancelId: 0 }).then(({ response }) => { if (response === 1) { closeAfterJob = true; activeJob?.abort(); if (!activeJob) window.close(); } });
  });
  await window.loadFile(rendererPath);
}
app.whenReady().then(async () => {
  const launchIndex = process.argv.indexOf('--loading-game');
  if (launchIndex !== -1) {
    const executable = process.argv[launchIndex + 1];
    if (!executable || !path.isAbsolute(executable)) throw new Error('--loading-game requires an absolute game executable path.');
    await requirePe(executable, 'The game');
    const { launchWithLoadingOverlay } = require('./loading-launcher.cjs');
    return launchWithLoadingOverlay(executable, app.getPath('userData'));
  }
  registerHandlers();
  return createWindow();
}).catch(error => { dialog.showErrorBox('AnyPS5 Desktop', error.message); app.quit(); });
app.on('window-all-closed', () => app.quit());
