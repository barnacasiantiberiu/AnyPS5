const $ = id => document.getElementById(id);
let state;
let selectedId = null;
let view = 'library';
let step = 1;
let source = null;
let sourceBusy = false;
let installing = false;
let preflighting = false;
let completedGame = null;
let logText = '';
let toastTimer;
let artworkVersion = 0;
const api = window.desktop;
const menuAudio = window.AnyPS5Audio;
const settingsTabs = ['players', 'engine', 'audio', 'input', 'graphics'];
const sound = name => menuAudio?.play(name);
api.onFullscreen(fullscreen => {
  if (state) state.fullscreen = fullscreen;
  document.body.classList.toggle('fullscreen', fullscreen);
});
function show(element, visible = true) { (typeof element === 'string' ? $(element) : element).classList.toggle('hidden', !visible); }
function toast(message, duration = 5000) { clearTimeout(toastTimer); $('toast').textContent = message; show('toast'); toastTimer = setTimeout(() => show('toast', false), duration); }
function error(message) { if (message) sound('error'); $('wizard-error').textContent = message; show('wizard-error', !!message); }
function formatBytes(bytes) { return new Intl.NumberFormat('en-GB', { maximumFractionDigits: 1 }).format(bytes / 1024 ** 3) + ' GB'; }
function shortDate(value) { return value ? new Intl.DateTimeFormat('en-GB', { day: 'numeric', month: 'short', year: 'numeric' }).format(new Date(value)) : 'Not played yet'; }
function on(id, event, action) { $(id).addEventListener(event, async e => { try { await action(e); } catch (err) { toast(err.message, 8000); } }); }
function setView(next) {
  if (preflighting) { toast('Checking the game source. Wait for this check to finish.'); return; }
  if (installing && next === 'library') { toast('Conversion is in progress. You can return to the library when it finishes.'); return; }
  show('game-menu', false); $('game-menu-toggle').setAttribute('aria-expanded', 'false');
  finishEditing(false);
  view = next;
  show('library-view', next === 'library'); show('convert-view', next === 'convert');
  $('nav-library').classList.toggle('active', next === 'library'); $('nav-convert').classList.toggle('active', next === 'convert');
  $('nav-library').toggleAttribute('aria-current', next === 'library'); $('nav-convert').toggleAttribute('aria-current', next === 'convert');
  if (next === 'convert') { $('backdrop').style.backgroundImage = ''; document.body.classList.remove('has-game-artwork'); ++artworkVersion; }
  else renderHero();
  queueMicrotask(focusView);
}
function setStep(next) {
  step = next;
  for (let n = 1; n <= 4; n++) {
    show(`step-${n}`, n === next);
    const item = document.querySelector(`[data-step="${n}"]`);
    item.classList.toggle('current', n === next); item.classList.toggle('done', n < next);
  }
  error('');
  if (view === 'convert') queueMicrotask(focusView);
}
function startWizard() {
  if (preflighting) { toast('Checking the game source. Wait for this check to finish.'); return; }
  if (!installing && step === 4) { setStep(1); completedGame = null; }
  setView('convert');
}
function gridGlyph() {
  const glyph = document.createElement('span'); glyph.className = 'library-glyph';
  for (let i = 0; i < 4; i++) glyph.append(document.createElement('i'));
  return glyph;
}
function tile(label, kind, action) {
  const button = document.createElement('button'); button.className = `game-tile ${kind}`; button.setAttribute('aria-label', label); button.title = label;
  const face = document.createElement('span'); face.className = 'tile-face'; button.append(face);
  button.addEventListener('click', action);
  return { button, face };
}
function renderShelf() {
  const shelf = $('game-shelf');
  const focusedKey = document.activeElement?.dataset.navKey;
  shelf.replaceChildren();
  const home = tile('Game library', selectedId ? '' : 'selected', () => { selectedId = null; renderShelf(); renderHero(); });
  home.button.dataset.navKey = 'library';
  home.face.classList.add('library-face'); home.face.append(gridGlyph());
  const label = document.createElement('span'); label.className = 'tile-name'; label.textContent = 'Game library'; home.button.append(label); shelf.append(home.button);
  const query = $('search-input').value.toLocaleLowerCase('en');
  const games = state.games.filter(game => game.title.toLocaleLowerCase('en').includes(query));
  for (const game of games) {
    const item = tile(game.title, game.id === selectedId ? 'selected' : '', () => { selectedId = game.id; renderShelf(); renderHero(); });
    item.button.dataset.gameId = game.id; item.button.dataset.navKey = `game:${game.id}`;
    if (game.cover) { const image = document.createElement('img'); image.src = game.cover; image.alt = ''; item.face.replaceWith(image); }
    else item.face.textContent = game.title.slice(0, 2).toLocaleUpperCase('en');
    const name = document.createElement('span'); name.className = 'tile-name'; name.textContent = game.title; item.button.append(name);
    shelf.append(item.button);
  }
  const add = tile('Convert a game', 'add-tile', startWizard); add.face.textContent = '+'; add.button.dataset.navKey = 'convert'; shelf.append(add.button);
  const imported = tile('Import a converted game', 'add-tile', importGame); imported.face.textContent = '↧'; imported.button.dataset.navKey = 'import'; shelf.append(imported.button);
  $('game-count').textContent = `${state.games.length} ${state.games.length === 1 ? 'GAME' : 'GAMES'}`;
  if (focusedKey) (findTile(focusedKey) || shelf.querySelector('.selected'))?.focus({ preventScroll: true });
}
async function renderHero() {
  const game = state.games.find(g => g.id === selectedId);
  const version = ++artworkVersion;
  show('game-menu', false); $('game-menu-toggle').setAttribute('aria-expanded', 'false');
  show('library-empty', !game); show('game-hero', !!game);
  if (!game) {
    $('backdrop').style.backgroundImage = ''; document.body.classList.remove('has-game-artwork');
    const heading = $('library-empty').querySelector('h1');
    heading.textContent = state.games.length ? 'Game library' : 'Welcome to your library.';
    $('library-empty').querySelector('.hero-copy > p').textContent = state.games.length ? `${state.games.length} ${state.games.length === 1 ? 'installed game' : 'installed games'}. Choose a game above to launch it.` : 'Your games, all in one place. Convert your first game or add one already prepared for Windows.';
    $('empty-convert').textContent = state.games.length ? 'Convert a game' : 'Convert your first game';
    return;
  }
  $('game-title').textContent = game.title;
  $('game-platform').textContent = game.titleId ? `PS5 · ${game.titleId}` : 'PS5 · ANYPS5';
  $('game-description').textContent = game.available ? 'Launch this game on your PC.' : 'The game files are no longer available. Check their location or import the game again.';
  $('game-launch').disabled = !game.available;
  $('game-date').textContent = shortDate(game.installedAt); $('game-last').textContent = shortDate(game.lastPlayed);
  try { const image = await api.artwork(game.id); if (version === artworkVersion && view === 'library') { $('backdrop').style.backgroundImage = image ? `url("${image}")` : ''; document.body.classList.toggle('has-game-artwork', !!image); } }
  catch { if (version === artworkVersion) { $('backdrop').style.backgroundImage = ''; document.body.classList.remove('has-game-artwork'); } }
}
function renderRuntime() {
  document.body.classList.toggle('fullscreen', !!state.fullscreen);
  const ready = state.runtimeReady;
  $('runtime-status').classList.toggle('ready', ready);
  $('runtime-status').replaceChildren(Object.assign(document.createElement('i'), {}), document.createTextNode(ready ? 'AnyPS5 connected' : 'Set up AnyPS5'));
  $('engine-status-text').textContent = ready ? 'AnyPS5 connected · checked before conversion' : 'Set up the AnyPS5 engine to continue';
}
async function refresh() {
  state = await api.state();
  menuAudio?.configure(state.settings.audio);
  if (!state.games.some(g => g.id === selectedId)) selectedId = null;
  renderShelf(); renderRuntime();
  if (view === 'library') await renderHero();
}
async function importGame() {
  try { const directory = await api.pick('converted'); if (!directory) return; state = await api.importGame(directory); selectedId = state.games.at(-1)?.id; renderShelf(); renderRuntime(); setView('library'); toast('The game has been added to your library.'); }
  catch (err) { toast(err.message, 8000); }
}
async function launch(id = selectedId) {
  if (!id) return;
  if (!state.games.find(game => game.id === id)?.available) { toast('This game is unavailable. Check its files or import it again.'); return; }
  toast('Starting game…'); const result = await api.launch(id); await refresh(); toast(result?.graphics?.message || 'Game started.', 10000);
}
const playerInputs = [
  { value: 'disabled', label: 'Disabled' },
  { value: 'keyboardMouse', label: 'Keyboard & mouse' },
  ...[1, 2, 3].map(slot => ({ value: `dualsense-${slot}`, label: `Controller ${slot}` }))
];
let playerDraft = ['keyboardMouse', 'disabled', 'disabled', 'disabled'];
const controllerSlots = [null, null, null];
let currentControllers = [null, null, null];
let controllerPoll;
const assignmentHelp = 'Each input can belong to one player. Disable its current player before moving it.';
function inputValue(player) {
  if (player?.type === 'keyboardMouse') return 'keyboardMouse';
  if (player?.type === 'dualsense' && [1, 2, 3].includes(player.controllerSlot)) return `dualsense-${player.controllerSlot}`;
  return 'disabled';
}
function playerSettings() {
  const values = [1, 2, 3, 4].map(player => $(`player-${player}-source`).value);
  const assigned = values.filter(value => value !== 'disabled');
  if (new Set(assigned).size !== assigned.length) throw new Error('An input is assigned to more than one player. Disable one assignment before saving.');
  return values.map(value => {
    if (!playerInputs.some(input => input.value === value)) throw new Error('Choose a valid input for every player.');
    if (value === 'keyboardMouse') return { type: 'keyboardMouse', controllerSlot: null };
    if (value.startsWith('dualsense-')) return { type: 'dualsense', controllerSlot: Number(value.slice(-1)) };
    return { type: 'disabled', controllerSlot: null };
  });
}
function updatePlayerOptions() {
  for (let index = 0; index < 4; index++) {
    const select = $(`player-${index + 1}-source`);
    for (const option of select.options) {
      const owner = option.value === 'disabled' ? -1 : playerDraft.indexOf(option.value);
      option.disabled = owner !== -1 && owner !== index;
      const name = playerInputs.find(input => input.value === option.value).label;
      option.textContent = option.disabled ? `${name} · Player ${owner + 1}` : name;
    }
    select.value = playerDraft[index];
  }
  updatePlayerStatuses();
}
function renderPlayerRows() {
  $('player-rows').replaceChildren();
  playerDraft.forEach((value, index) => {
    const number = index + 1;
    const row = document.createElement('div'); row.className = 'player-row';
    const avatar = document.createElement('span'); avatar.className = 'player-avatar'; avatar.textContent = String(number); avatar.setAttribute('aria-hidden', 'true');
    const details = document.createElement('div'); details.className = 'player-details';
    const label = document.createElement('label'); label.htmlFor = `player-${number}-source`; label.textContent = `Player ${number}`;
    const status = document.createElement('span'); status.id = `player-${number}-status`; status.className = 'player-status'; status.setAttribute('role', 'status');
    details.append(label, status);
    const select = document.createElement('select'); select.id = `player-${number}-source`; select.setAttribute('aria-describedby', `${status.id} player-assignment-feedback`);
    for (const input of playerInputs) { const option = document.createElement('option'); option.value = input.value; option.textContent = input.label; select.append(option); }
    select.value = value;
    select.addEventListener('change', () => {
      const owner = select.value === 'disabled' ? -1 : playerDraft.indexOf(select.value);
      if (owner !== -1 && owner !== index) {
        $('player-assignment-feedback').textContent = `This input belongs to Player ${owner + 1}. Set that player to Disabled first, then assign it here.`;
        select.value = playerDraft[index];
        return;
      }
      playerDraft[index] = select.value;
      $('player-assignment-feedback').textContent = select.value === 'disabled' ? `Player ${number} disabled. Their input is now available for another player.` : `Player ${number}: ${playerInputs.find(input => input.value === select.value).label}. Save settings to keep this assignment.`;
      updatePlayerOptions();
    });
    row.append(avatar, details, select); $('player-rows').append(row);
  });
  $('player-assignment-feedback').textContent = assignmentHelp;
  updatePlayerOptions();
}
function updatePlayerStatuses() {
  playerDraft.forEach((value, index) => {
    const status = $(`player-${index + 1}-status`);
    if (!status) return;
    const controller = value.startsWith('dualsense-') ? currentControllers[Number(value.slice(-1)) - 1] : null;
    const connected = value === 'keyboardMouse' || !!controller;
    const message = value === 'disabled' ? 'No input assigned' : value === 'keyboardMouse' ? 'This PC · Ready' : controller ? 'Connected' : 'Disconnected';
    if (status.textContent !== message) status.textContent = message;
    status.classList.toggle('connected', connected);
  });
}
function isDualSense(gamepad) {
  const id = gamepad.id || '';
  return /dualsense/i.test(id) || (/(?:sony|(?:^|[^0-9a-f])0?54c(?:[^0-9a-f]|$))/i.test(id) && /(?:^|[^0-9a-f])0(?:ce6|df2)(?:[^0-9a-f]|$)/i.test(id));
}
function controllerActivity(gamepad) {
  const pressed = gamepad.buttons.findIndex(button => button.pressed || button.value > 0.5);
  if (pressed !== -1) {
    const names = ['Cross', 'Circle', 'Square', 'Triangle', 'L1', 'R1', 'L2', 'R2', 'Create', 'Options', 'L3', 'R3', 'D-pad up', 'D-pad down', 'D-pad left', 'D-pad right', 'PS', 'Touchpad'];
    return gamepad.mapping === 'standard' && names[pressed] ? `${names[pressed]} pressed` : `Button ${pressed + 1} pressed`;
  }
  return gamepad.axes.some(axis => Math.abs(axis) > 0.25) ? 'Stick input detected' : 'Connected · Press a button to test';
}
function pollControllers() {
  if (!$('settings-dialog').open) return;
  let pads = [], unavailable = false;
  try { if (typeof navigator.getGamepads !== 'function') unavailable = true; else pads = Array.from(navigator.getGamepads()).filter(pad => pad?.connected); }
  catch { unavailable = true; }
  const supported = pads;
  const key = pad => `${pad.index}:${pad.id}`;
  // Keep each connected device in its slot if another controller disconnects.
  currentControllers = controllerSlots.map(identity => supported.find(pad => key(pad) === identity) || null);
  for (const pad of supported) {
    if (currentControllers.includes(pad)) continue;
    const empty = currentControllers.indexOf(null);
    if (empty === -1) break;
    controllerSlots[empty] = key(pad); currentControllers[empty] = pad;
  }
  const connected = currentControllers.filter(Boolean).length;
  const countText = `${connected} / 3 connected`;
  if ($('controllers-count').textContent !== countText) $('controllers-count').textContent = countText;
  for (let slot = 1; slot <= 3; slot++) {
    const pad = currentControllers[slot - 1];
    const row = $(`controller-${slot}`);
    row.classList.toggle('connected', !!pad);
    row.title = pad?.id || `Controller ${slot} is not connected`;
    row.querySelector('span').textContent = pad ? `Controller ${slot} · ${pad.id}` : `Controller ${slot}`;
    const status = $(`controller-${slot}-status`);
    const activity = pad ? (pad.mapping === 'standard' || isDualSense(pad) ? controllerActivity(pad) : 'Connected · SDL mapping unverified') : 'Disconnected';
    if (status.textContent !== activity) status.textContent = activity;
  }
  const unsupported = pads.filter(pad => pad.mapping !== 'standard' && !isDualSense(pad)).length;
  const note = unavailable ? 'Controller detection is unavailable in this window. You can still save assignments.' : unsupported ? `${unsupported} ${unsupported === 1 ? 'controller has' : 'controllers have'} an unverified browser mapping. Menu navigation requires a standard button mapping; in-game support depends on SDL recognizing the device.` : supported.length > 3 ? 'Only the first three controllers are available for assignment.' : '';
  if ($('controller-detection-note').textContent !== note) $('controller-detection-note').textContent = note;
  show('controller-detection-note', !!note);
  updatePlayerStatuses();
}
function initializeControllerMonitor() {
  if ($('controller-devices').childElementCount) return;
  for (let slot = 1; slot <= 3; slot++) {
    const row = document.createElement('div'); row.id = `controller-${slot}`; row.className = 'controller-device';
    const dot = document.createElement('i'); dot.setAttribute('aria-hidden', 'true');
    const label = document.createElement('span'); label.textContent = `Controller ${slot}`;
    const status = document.createElement('span'); status.id = `controller-${slot}-status`; status.className = 'controller-activity'; status.textContent = 'Disconnected';
    row.append(dot, label, status); $('controller-devices').append(row);
  }
}
function inputSettings() {
  return { leftStickDeadzone: Number($('input-left-deadzone').value), rightStickDeadzone: Number($('input-right-deadzone').value), mouseSensitivity: Number($('input-mouse-sensitivity').value) };
}
function fillInputSettings(input = { leftStickDeadzone: 0, rightStickDeadzone: 0, mouseSensitivity: 1 }) {
  $('input-left-deadzone').value = input.leftStickDeadzone;
  $('input-right-deadzone').value = input.rightStickDeadzone;
  $('input-mouse-sensitivity').value = input.mouseSensitivity;
}
on('input-defaults', 'click', () => fillInputSettings());
function audioSettings() {
  return { enabled: $('audio-enabled').checked, musicVolume: Number($('music-volume').value), effectsVolume: Number($('effects-volume').value) };
}
function previewAudioSettings() {
  const audio = audioSettings();
  $('music-volume-value').textContent = audio.musicVolume + '%';
  $('effects-volume-value').textContent = audio.effectsVolume + '%';
  $('music-volume').disabled = !audio.enabled; $('effects-volume').disabled = !audio.enabled;
  menuAudio?.configure(audio);
}
function fillAudioSettings(audio = { enabled: true, musicVolume: 25, effectsVolume: 45 }) {
  $('audio-enabled').checked = audio.enabled;
  $('music-volume').value = audio.musicVolume; $('effects-volume').value = audio.effectsVolume;
  previewAudioSettings();
}
on('audio-enabled', 'change', previewAudioSettings);
for (const id of ['music-volume', 'effects-volume']) on(id, 'input', () => { previewAudioSettings(); if (id === 'effects-volume') sound('navigate'); });
function setSettingsTab(name, focus = false) {
  for (const tabName of settingsTabs) {
    const selected = tabName === name;
    const tab = $(`settings-tab-${tabName}`);
    tab.classList.toggle('active', selected); tab.setAttribute('aria-selected', String(selected)); tab.tabIndex = selected ? 0 : -1;
    show(`settings-panel-${tabName}`, selected);
    if (selected && focus) tab.focus();
  }
  $('settings-dialog').querySelector('.settings-body').scrollTop = 0;
}
function openSettings(tabName = 'players') {
  if (installing || preflighting) { toast('Settings can be changed when the current check or conversion finishes.'); return; }
  settingsInvoker = document.activeElement;
  show('game-menu', false); $('game-menu-toggle').setAttribute('aria-expanded', 'false');
  finishEditing(false);
  for (const key of ['relinker', 'libs', 'dllDirectory']) $(`setting-${key}`).value = state.settings[key] || '';
  const saved = state.settings.players;
  const used = new Set();
  playerDraft = [0, 1, 2, 3].map(index => {
    const value = Array.isArray(saved) ? inputValue(saved[index]) : index === 0 ? 'keyboardMouse' : 'disabled';
    if (value !== 'disabled' && used.has(value)) return 'disabled';
    used.add(value); return value;
  });
  fillAudioSettings(state.settings.audio); fillInputSettings(state.settings.input);
  const resolution = $('graphics-resolution');
  resolution.replaceChildren(...state.resolutions.map(item => {
    const option = document.createElement('option'); option.value = item.value; option.textContent = item.label; return option;
  }));
  resolution.value = state.settings.graphics?.resolution || '1920x1080';
  renderPlayerRows(); initializeControllerMonitor();
  setSettingsTab(tabName); show('settings-error', false); $('settings-dialog').showModal();
  pollControllers(); clearInterval(controllerPoll); controllerPoll = setInterval(pollControllers, 150);
  focusControl($(`settings-tab-${tabName}`));
}
for (const name of settingsTabs) on(`settings-tab-${name}`, 'click', () => { finishEditing(false); setSettingsTab(name, true); });
$('settings-dialog').addEventListener('close', () => {
  clearInterval(controllerPoll); controllerPoll = null; finishEditing(false);
  menuAudio?.configure(state.settings.audio);
  if (settingsInvoker?.isConnected && visibleControl(settingsInvoker)) focusControl(settingsInvoker);
  else focusView();
});
$('settings-dialog').addEventListener('cancel', event => { event.preventDefault(); navigateBack(); });
window.addEventListener('gamepadconnected', pollControllers);
window.addEventListener('gamepaddisconnected', pollControllers);
on('nav-library', 'click', () => setView('library')); on('nav-convert', 'click', startWizard);
on('empty-convert', 'click', startWizard); on('empty-import', 'click', importGame); on('footer-import', 'click', importGame);
on('convert-back', 'click', () => setView('library'));
on('settings-open', 'click', () => openSettings('players')); on('wizard-settings', 'click', () => openSettings('engine'));
on('settings-close', 'click', () => $('settings-dialog').close());
for (const button of document.querySelectorAll('[data-setting]')) button.addEventListener('click', async () => { try { const file = await api.pick(button.dataset.setting); if (file) $(`setting-${button.dataset.setting}`).value = file; } catch (err) { toast(err.message); } });
on('settings-form', 'submit', async event => {
  event.preventDefault();
  try {
    const settings = { ...state.settings, players: playerSettings(), audio: audioSettings(), input: inputSettings(), graphics: { resolution: $('graphics-resolution').value } };
    for (const key of ['relinker', 'libs', 'dllDirectory']) settings[key] = $(`setting-${key}`).value;
    state = await api.saveSettings(settings); menuAudio?.configure(state.settings.audio); renderRuntime(); $('settings-dialog').close(); toast('Settings saved.');
  } catch (err) { $('settings-error').textContent = err.message; show('settings-error'); $('settings-error').scrollIntoView({ block: 'nearest' }); }
});
function sourceReady() { return !!source?.entry && !source.encrypted && !source.diagnostic; }
function setSourceBusy(busy) {
  sourceBusy = busy;
  $('pick-source').disabled = busy; $('pick-iso').disabled = busy;
  $('pick-entry').disabled = busy || source?.sourceType === 'iso';
  $('iso-entry-select').disabled = busy || !source?.executables?.length;
  $('to-step-2').disabled = busy || !sourceReady();
  $('source-summary').setAttribute('aria-busy', String(busy));
}
function relativeIsoEntry(value) {
  return typeof value === 'string' && !!value.trim() && !/^(?:[a-z]:|[\\/])/i.test(value) && !value.split(/[\\/]/).some(part => part === '..' || part === '.');
}
function showSourceDetails() {
  const iso = source.sourceType === 'iso';
  $('source-label').textContent = source.title;
  $('source-hint').textContent = source.source;
  $('game-name-input').value = source.title;
  $('entry-input').value = iso ? '' : source.entry || '';
  $('entry-label').htmlFor = iso ? 'iso-entry-select' : 'entry-input';
  show('folder-entry-field', !iso); show('iso-entry-select', iso);
  const select = $('iso-entry-select'); select.replaceChildren();
  if (iso) {
    const placeholder = new Option(source.executables.length ? 'Choose a decrypted ELF executable' : 'No decrypted ELF executable found', '');
    placeholder.disabled = true; select.append(placeholder);
    for (const entry of source.executables) select.append(new Option(entry, entry));
    select.value = source.entry || '';
  }
  const note = iso ? [source.note || 'ISO image · opened read-only. Your original image stays unchanged.', source.cleanupWarning].filter(Boolean).join(' ') : source.note || '';
  $('source-note').textContent = note; show('source-note', !!note);
  show('source-summary'); show('source-details');
  error(source.diagnostic || (source.encrypted ? 'The executable is not a decrypted ELF file. AnyPS5 cannot decrypt SELF files.' : !source.entry ? iso ? 'Choose a decrypted ELF executable from the image. Encrypted disc images and PKG files cannot be decrypted by AnyPS5.' : 'No executable was found automatically. Select an ELF file below.' : ''));
}
async function chooseSource(kind) {
  if (sourceBusy) return;
  const button = $(kind === 'iso' ? 'pick-iso' : 'pick-source');
  let selected = null;
  setSourceBusy(true);
  try {
    selected = await api.pick(kind === 'iso' ? 'iso' : 'source');
    if (!selected) return;
    source = null; $('entry-input').value = ''; $('iso-entry-select').replaceChildren();
    $('game-name-input').value = ''; show('source-details', false); show('source-note', false); error('');
    $('source-label').textContent = kind === 'iso' ? 'Checking ISO…' : 'Checking folder…';
    $('source-hint').textContent = selected; show('source-summary');
    const inspected = await (kind === 'iso' ? api.inspectIso(selected) : api.inspect(selected));
    if (!inspected || typeof inspected.source !== 'string' || !inspected.source.trim()) throw new Error(kind === 'iso' ? 'The ISO image did not contain a usable game source. Choose another image.' : 'The folder did not contain a usable game source. Choose another folder.');
    source = { ...inspected, sourceType: kind, title: inspected.title || (kind === 'iso' ? 'ISO game' : 'Untitled game') };
    if (kind === 'iso') {
      source.source = selected;
      source.executables = Array.isArray(inspected.executables) ? [...new Set(inspected.executables.filter(relativeIsoEntry))] : [];
      source.entry = source.executables.includes(inspected.entry) ? inspected.entry : '';
    }
    showSourceDetails();
  } catch (err) {
    source = null; $('entry-input').value = ''; $('iso-entry-select').replaceChildren();
    show('source-details', false); show('source-note', false);
    $('source-label').textContent = kind === 'iso' ? 'Could not open ISO image' : 'Could not open game folder';
    $('source-hint').textContent = selected || ''; show('source-summary', !!selected); error(err.message);
  } finally {
    setSourceBusy(false);
    focusControl(source ? $('game-name-input') : button);
  }
}
on('pick-source', 'click', () => chooseSource('folder'));
on('pick-iso', 'click', () => chooseSource('iso'));
on('iso-entry-select', 'change', () => {
  if (sourceBusy || source?.sourceType !== 'iso') return;
  const entry = $('iso-entry-select').value;
  source.entry = source.executables.includes(entry) ? entry : '';
  if (source.entry) { source.encrypted = false; source.diagnostic = ''; error(''); }
  $('to-step-2').disabled = !sourceReady();
});
on('pick-entry', 'click', async () => {
  if (sourceBusy || !source || source.sourceType === 'iso') return;
  const button = $('pick-entry'); let valid = false; let selected = false;
  setSourceBusy(true);
  try {
    const entry = await api.pick('entry'); if (!entry) return;
    selected = true; button.textContent = 'Checking…';
    source.entry = ''; $('entry-input').value = ''; $('to-step-2').disabled = true; error('');
    const canonicalEntry = await api.checkEntry({ source: source.source, entry });
    source.entry = canonicalEntry; source.encrypted = false; source.diagnostic = '';
    $('entry-input').value = canonicalEntry; valid = true;
  } catch (err) {
    source.entry = ''; $('entry-input').value = ''; error(err.message);
  } finally {
    button.textContent = 'Choose'; setSourceBusy(false);
    focusControl(selected && valid ? $('to-step-2') : button);
  }
});
on('to-step-2', 'click', () => {
  if (sourceBusy || !sourceReady() || !$('game-name-input').value.trim()) { error('Choose an executable and enter the game name.'); return; }
  $('destination-input').value ||= state.settings.destinationParent;
  $('destination-name').textContent = $('game-name-input').value.trim();
  $('intel-check').checked = state.settings.toIntel;
  $('cpu-label').textContent = `Detected: ${state.cpu}`;
  setStep(2);
});
on('back-step-1', 'click', () => setStep(1));
on('pick-destination', 'click', async () => { const folder = await api.pick('destination'); if (folder) $('destination-input').value = folder; });
function setPreflightBusy(busy) {
  preflighting = busy;
  for (const id of ['back-step-1', 'pick-destination', 'wizard-settings']) $(id).disabled = busy;
}
on('begin-install', 'click', async () => {
  if (preflighting || installing || sourceBusy) return;
  setPreflightBusy(true);
  const button = $('begin-install'); button.disabled = true; error(''); show('preflight-result', false); resetProgress();
  const request = { source: source?.source, sourceType: source?.sourceType || 'folder', entry: source?.entry, title: $('game-name-input').value.trim(), titleId: source?.titleId || '', destinationParent: $('destination-input').value, toIntel: $('intel-check').checked };
  try {
    button.textContent = 'Checking files…';
    const plan = await api.preflight(request);
    state = await api.saveSettings({ ...state.settings, destinationParent: request.destinationParent, toIntel: request.toIntel });
    $('preflight-result').textContent = `${formatBytes(plan.totalBytes)} required · ${formatBytes(plan.freeBytes)} available`; show('preflight-result');
    if (plan.cleanupWarning) toast(plan.cleanupWarning, 12000);
    setPreflightBusy(false); installing = true; logText = ''; $('install-log').textContent = '';
    $('cancel-install').disabled = false; $('cancel-install').textContent = 'Cancel';
    $('install-title').textContent = request.title; setStep(3);
    const result = await api.install(request);
    completedGame = result.game; installing = false;
    await refresh(); setStep(4); sound('success');
    $('complete-title').textContent = completedGame.title; $('complete-path').textContent = completedGame.directory;
    $('complete-shortcut').disabled = false; $('shortcut-state').textContent = '＋';
    if (result.warning) error(result.warning);
    $('complete-shortcut').disabled = result.librarySaved === false;
    $('complete-launch').disabled = result.librarySaved === false;

  } catch (err) {
    installing = false; setStep(2); error(err.message + (err.log ? `\n\nLatest messages:\n${err.log.slice(-1800)}` : ''));
  } finally { setPreflightBusy(false); button.disabled = false; button.textContent = 'Check and convert →'; }
});
const progressStages = ['checking', 'copying', 'converting', 'libraries', 'finishing'];
let progressFinished = false;
function renderProgress(event) {
  const complete = event.complete === true;
  if (progressFinished && !complete) return;
  const count = progressStages.length;
  const namedIndex = progressStages.indexOf(event.stage) + 1;
  const index = complete ? count : Math.max(1, Math.min(count, Number.isInteger(event.stageIndex) ? event.stageIndex : namedIndex || 1));
  const completedStages = complete ? count : index - 1;
  const overall = completedStages / count * 100;
  const label = event.label || (complete ? 'Conversion complete' : 'Checking files');
  progressFinished = complete;
  $('phase-label').textContent = label;
  $('phase-stage').textContent = `Stage ${index} of ${count}`;
  $('phase-percent').textContent = !complete && event.stage === 'copying' && Number.isFinite(event.progress) ? `${Math.round(Math.max(0, Math.min(100, event.progress)))}% of bytes copied` : '';
  $('progress-status').textContent = complete ? 'Complete' : 'Running…';
  const track = $('progress-track');
  track.dataset.stage = complete ? 'complete' : progressStages[index - 1]; track.dataset.complete = String(complete);
  track.setAttribute('aria-valuenow', String(overall)); track.setAttribute('aria-busy', String(!complete));
  track.setAttribute('aria-valuetext', `${completedStages} of ${count} stages completed. ${label}. ${complete ? 'Complete.' : 'Running.'}`);
  $('progress-fill').style.width = `${overall}%`;
  $('progress-current').style.left = `${overall}%`; show('progress-current', !complete);
}
function resetProgress() {
  progressFinished = false;
  renderProgress({ stage: 'checking', stageIndex: 1, stageCount: 5, label: 'Checking files', progress: null, complete: false });
  logText = ''; $('install-log').textContent = 'Waiting for the first step…';
}
api.onProgress(event => {
  if (event.kind === 'log') {
    logText = (logText + event.text).slice(-35000); $('install-log').textContent = logText; $('install-log').scrollTop = $('install-log').scrollHeight;
  } else if (event.kind === 'phase') renderProgress(event);
});
on('cancel-install', 'click', async () => { $('cancel-install').disabled = true; $('cancel-install').textContent = 'Stopping…'; await api.cancel(); toast('Stop requested. Waiting for the current operation to finish.'); });
on('complete-shortcut', 'click', async () => { await api.shortcut(completedGame.id); $('complete-shortcut').disabled = true; $('shortcut-state').textContent = '✓'; toast('The shortcut is on your desktop.'); });
on('complete-library', 'click', () => { selectedId = completedGame?.id; renderShelf(); setView('library'); });
on('complete-launch', 'click', () => launch(completedGame.id));
on('game-launch', 'click', () => launch());
on('game-menu-toggle', 'click', () => { const visible = $('game-menu').classList.contains('hidden'); show('game-menu', visible); $('game-menu-toggle').setAttribute('aria-expanded', String(visible)); if (visible) focusControl($('game-folder')); });
on('game-folder', 'click', () => api.folder(selectedId));
on('game-shortcut', 'click', async () => { await api.shortcut(selectedId); toast('The shortcut is on your desktop.'); show('game-menu', false); $('game-menu-toggle').setAttribute('aria-expanded', 'false'); focusControl($('game-menu-toggle')); });
on('game-remove', 'click', async () => { state = await api.remove(selectedId); if (!state.games.some(g => g.id === selectedId)) selectedId = null; renderShelf(); await renderHero(); focusView(); });
on('search-toggle', 'click', () => { const visible = $('search-box').classList.contains('hidden'); show('search-box', visible); if (visible) { focusControl($('search-input')); beginEditing($('search-input')); } else { $('search-input').value = ''; renderShelf(); } });
on('search-input', 'input', renderShelf);
let exitInvoker = null;
on('exit-cancel', 'click', () => $('exit-dialog').close());
on('exit-confirm', 'click', async () => { $('exit-dialog').close(); await api.close(); });
on('exit-dialog', 'cancel', event => { event.preventDefault(); $('exit-dialog').close(); });
on('exit-dialog', 'close', () => focusControl(visibleControl(exitInvoker) ? exitInvoker : findTile('library'), false));
function openExitMenu() {
  exitInvoker = document.activeElement;
  $('exit-dialog').showModal(); focusControl($('exit-cancel'));
}
let settingsInvoker = null;
let editingControl = null;
let originalInputValue = null;
let navigationOrigin = 'arrow';
let lastInputDevice = 'keyboard';
let controllerUsesSymbols = false;
const arrowKeys = ['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown'];
function findTile(key) { return Array.from($('game-shelf').children).find(tile => tile.dataset.navKey === key); }
function visibleControl(control) {
  return !!control && !control.disabled && !control.closest('.hidden, [inert]') && control.getClientRects().length > 0 && getComputedStyle(control).visibility !== 'hidden';
}
function textInput(control) { return control?.matches('input:not([type=checkbox]):not([type=radio]):not([type=range]):not([readonly]), textarea'); }
function rangeInput(control) { return control?.matches('input[type=range]'); }
function keyboardHint() {
  const control = document.activeElement;
  let message = $('exit-dialog').open ? '← → Choose · Enter Select · Esc Stay' : rangeInput(editingControl) ? '← → Adjust volume · Enter Done · Esc Revert' : editingControl?.tagName === 'SELECT' ? '↑ ↓ ← → Change selection · Enter Confirm · Esc Revert' : editingControl ? 'Type to edit · Enter Done · Esc Done' : rangeInput(control) ? '↑ ↓ ← → Navigate · Enter Adjust volume · Esc Back' : control?.tagName === 'SELECT' ? '↑ ↓ ← → Navigate · Enter Edit selection · Esc Back' : textInput(control) ? '↑ ↓ ← → Navigate · Enter Edit text · Esc Back' : control?.dataset.gameId ? '↑ ↓ ← → Navigate · Enter Play game · Esc Back' : view === 'library' && selectedId === null ? '↑ ↓ ← → Navigate · Enter Select · Esc Exit menu' : '↑ ↓ ← → Navigate · Enter Select · Esc Back';
  if (lastInputDevice === 'controller') {
    const select = controllerUsesSymbols ? 'Cross' : 'A', back = controllerUsesSymbols ? 'Circle' : 'B';
    message = editingControl ? (textInput(editingControl) && editingControl.type !== 'number' ? 'Keyboard to type' : 'D-pad / stick Adjust') + ' · ' + select + ' Done · ' + back + ' ' + (textInput(editingControl) && editingControl.type !== 'number' ? 'Done' : 'Revert')
      : 'D-pad / stick Navigate · ' + select + ' Select · ' + back + ($('exit-dialog').open ? ' Stay' : view === 'library' && selectedId === null && !$('settings-dialog').open ? ' Exit menu' : ' Back');
  }
  for (const id of ['keyboard-hint', 'settings-keyboard-hint', 'exit-input-hint']) if ($(id).textContent !== message) $(id).textContent = message;
}
function beginEditing(control) {
  if ((!textInput(control) && !rangeInput(control) && control?.tagName !== 'SELECT') || control.readOnly) return;
  if (editingControl && editingControl !== control) finishEditing(false);
  if (editingControl !== control) originalInputValue = control.value;
  editingControl = control; control.classList.add('keyboard-editing'); control.dataset.keyboardEditing = 'true';
  keyboardHint();
}
function finishEditing(revert) {
  if (!editingControl) return;
  const control = editingControl;
  if (revert && (control.tagName === 'SELECT' || rangeInput(control) || control.type === 'number') && control.value !== originalInputValue) { control.value = originalInputValue; control.dispatchEvent(new Event(rangeInput(control) || control.type === 'number' ? 'input' : 'change', { bubbles: true })); }
  control.classList.remove('keyboard-editing'); delete control.dataset.keyboardEditing; control.dataset.navMode = 'true';
  editingControl = null; originalInputValue = null; keyboardHint();
}
function focusControl(control, preview = true) {
  if (!visibleControl(control)) return;
  finishEditing(false); navigationOrigin = 'arrow'; document.body.classList.add('keyboard-navigation');
  const key = control.dataset.navKey;
  if (preview && key && (key === 'library' || key.startsWith('game:'))) {
    const id = control.dataset.gameId || null;
    if (selectedId !== id) { selectedId = id; renderShelf(); renderHero(); control = findTile(key); }
  }
  if (!control) return;
  control.dataset.navMode = 'true'; control.focus({ preventScroll: true });
  control.scrollIntoView({ inline: 'nearest', block: 'nearest' }); keyboardHint();
}
function focusView() {
  if ($('exit-dialog').open || $('settings-dialog').open) return;
  if (view === 'library') focusControl(document.querySelector('.game-tile.selected') || $('game-shelf').firstElementChild);
  else {
    const preferred = step === 1 ? $('pick-source') : step === 2 ? $('pick-destination') : step === 3 ? document.querySelector('#step-3 summary') : $('complete-launch');
    focusControl(visibleControl(preferred) ? preferred : document.querySelector(`#step-${step} button:not(:disabled)`));
  }
}
function navigationScope() {
  if ($('exit-dialog').open) return $('exit-dialog');
  if ($('settings-dialog').open) return $('settings-dialog');
  if (visibleControl($('game-menu'))) return $('game-menu');
  return document.body;
}
function navigationControls() {
  return Array.from(navigationScope().querySelectorAll('button, input:not([readonly]), select, summary, [tabindex]')).filter(control => visibleControl(control) && control.tabIndex >= 0);
}
function moveFocus(key) {
  const active = document.activeElement;
  if (active?.dataset.navKey && (key === 'ArrowLeft' || key === 'ArrowRight')) {
    const tiles = Array.from($('game-shelf').children).filter(visibleControl);
    const index = tiles.indexOf(active), next = index + (key === 'ArrowRight' ? 1 : -1);
    if (tiles[next]) focusControl(tiles[next]);
    return;
  }
  if (active?.closest('.navigation') && (key === 'ArrowLeft' || key === 'ArrowRight')) {
    const header = Array.from(document.querySelectorAll('.navigation button, .navigation input')).filter(visibleControl);
    const index = header.indexOf(active), next = index + (key === 'ArrowRight' ? 1 : -1);
    if (header[next]) focusControl(header[next]);
    return;
  }
  const controls = navigationControls();
  if (!controls.includes(active)) { focusControl(controls[0]); return; }
  const rect = active.getBoundingClientRect(), x = rect.x + rect.width / 2, y = rect.y + rect.height / 2;
  const horizontal = key === 'ArrowLeft' || key === 'ArrowRight', sign = key === 'ArrowLeft' || key === 'ArrowUp' ? -1 : 1;
  const candidates = controls.filter(control => control !== active).map(control => {
    const bounds = control.getBoundingClientRect(), dx = bounds.x + bounds.width / 2 - x, dy = bounds.y + bounds.height / 2 - y;
    const forward = (horizontal ? dx : dy) * sign, sideways = Math.abs(horizontal ? dy : dx);
    const aligned = horizontal ? bounds.bottom > rect.top && bounds.top < rect.bottom : bounds.right > rect.left && bounds.left < rect.right;
    return { control, forward, aligned, score: forward + sideways * 2 };
  }).filter(item => item.forward > 3).sort((a, b) => Number(b.aligned) - Number(a.aligned) || a.score - b.score);
  if (candidates.length) focusControl(candidates[0].control);
}
function navigateBack() {
  if (preflighting) { toast('Checking the game source. Wait for the check and temporary mount cleanup to finish.'); return; }
  if ($('exit-dialog').open) { $('exit-dialog').close(); return; }
  if (editingControl) { finishEditing(true); return; }
  const active = document.activeElement;
  if (textInput(active) && active.dataset.navMode !== 'true') { active.dataset.navMode = 'true'; keyboardHint(); return; }
  if ($('settings-dialog').open) { $('settings-dialog').close(); return; }
  if (visibleControl($('game-menu'))) { show('game-menu', false); $('game-menu-toggle').setAttribute('aria-expanded', 'false'); focusControl($('game-menu-toggle')); return; }
  if (!$('search-box').classList.contains('hidden')) { show('search-box', false); $('search-input').value = ''; renderShelf(); focusControl($('search-toggle')); return; }
  if (view === 'convert') {
    if (installing) { focusControl($('cancel-install')); toast('Conversion is still running. Select Cancel to request a stop.'); return; }
    if (step === 2) { setStep(1); return; }
    if (step === 4) { selectedId = completedGame?.id || null; renderShelf(); }
    setView('library'); return;
  }
  if (selectedId !== null) { selectedId = null; renderShelf(); renderHero(); focusControl(findTile('library')); return; }
  openExitMenu();
}
function cycleInput(control, direction) {
  const options = Array.from(control.options).filter(option => !option.disabled);
  const index = options.findIndex(option => option.value === control.value);
  control.value = options[(index + direction + options.length) % options.length].value;
  control.dispatchEvent(new Event('change', { bubbles: true }));
}
document.addEventListener('pointerdown', event => {
  lastInputDevice = 'mouse'; keyboardHint();
  navigationOrigin = 'pointer'; document.body.classList.remove('keyboard-navigation');
  if (event.target !== editingControl) finishEditing(false);
  if (textInput(event.target) || rangeInput(event.target) || event.target.tagName === 'SELECT') beginEditing(event.target);
});
document.addEventListener('beforeinput', event => {
  if (event.target === document.activeElement && textInput(event.target)) beginEditing(event.target);
});
document.addEventListener('input', event => {
  if (event.target === document.activeElement && textInput(event.target)) beginEditing(event.target);
});
document.addEventListener('focusin', event => {
  if (editingControl && editingControl !== event.target) finishEditing(false);
  if (navigationOrigin !== 'arrow') delete event.target.dataset.navMode;
  keyboardHint();
});
function handleNavigationKey(event, fromController = false) {
  if (event.defaultPrevented || event.isComposing || event.altKey || event.ctrlKey || event.metaKey) return;
  if (event.key === 'Tab') { finishEditing(false); navigationOrigin = 'tab'; document.body.classList.add('keyboard-navigation'); return; }
  if (event.key === 'F11') { event.preventDefault(); api.fullscreen().catch(err => toast(err.message)); return; }
  const active = document.activeElement;
  if (event.key === 'Escape') { event.preventDefault(); sound('back'); navigateBack(); return; }
  if (event.key === 'Enter' && event.repeat) { event.preventDefault(); return; }
  if (editingControl) {
    if (event.key === 'Enter') { event.preventDefault(); sound('confirm'); finishEditing(false); return; }
    if (editingControl.tagName === 'SELECT' && arrowKeys.includes(event.key)) { event.preventDefault(); sound('navigate'); cycleInput(editingControl, event.key === 'ArrowLeft' || event.key === 'ArrowUp' ? -1 : 1); return; }
    if (rangeInput(editingControl) && (arrowKeys.includes(event.key) || ['Home', 'End'].includes(event.key))) {
      event.preventDefault();
      const direction = event.key === 'ArrowLeft' || event.key === 'ArrowDown' ? -1 : 1;
      const value = event.key === 'Home' ? 0 : event.key === 'End' ? 100 : Number(editingControl.value) + direction * Number(editingControl.step || 1);
      editingControl.value = String(Math.max(0, Math.min(100, value)));
      editingControl.dispatchEvent(new Event('input', { bubbles: true }));
      if (editingControl.id !== 'effects-volume') sound('navigate');
      return;
    }
    if (fromController && editingControl.type === 'number' && arrowKeys.includes(event.key)) {
      event.preventDefault();
      const control = editingControl, delta = event.key === 'ArrowLeft' || event.key === 'ArrowDown' ? -1 : 1;
      const step = control.step === 'any' ? .1 : Number(control.step || 1);
      const min = control.min === '' ? -Infinity : Number(control.min), max = control.max === '' ? Infinity : Number(control.max);
      control.value = String(Math.max(min, Math.min(max, Number((Number(control.value || 0) + delta * step).toFixed(6)))));
      control.dispatchEvent(new Event('input', { bubbles: true })); sound('navigate'); return;
    }
    if (textInput(editingControl)) {
      if (!fromController) return;
      finishEditing(false);
    }
  }
  if (!fromController && textInput(active) && active.dataset.navMode !== 'true' && arrowKeys.includes(event.key)) return;
  if (active?.getAttribute('role') === 'tab' && ['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) {
    event.preventDefault(); const current = settingsTabs.indexOf(active.id.replace('settings-tab-', ''));
    const next = event.key === 'Home' ? 0 : event.key === 'End' ? settingsTabs.length - 1 : (current + (event.key === 'ArrowLeft' ? -1 : 1) + settingsTabs.length) % settingsTabs.length;
    sound('navigate'); setSettingsTab(settingsTabs[next], true); return;
  }
  if (arrowKeys.includes(event.key)) { event.preventDefault(); moveFocus(event.key); if (document.activeElement !== active) sound('navigate'); return; }
  if (event.key === 'Enter') {
    if (textInput(active) || rangeInput(active) || active?.tagName === 'SELECT') { event.preventDefault(); sound('confirm'); beginEditing(active); return; }
    if (active?.matches('input[type=checkbox]')) { event.preventDefault(); active.click(); return; }
    if (active?.dataset.gameId) { event.preventDefault(); sound('confirm'); launch(active.dataset.gameId).catch(err => toast(err.message)); return; }
    if (fromController && visibleControl(active) && active.matches('button, summary')) { event.preventDefault(); active.click(); return; }
    if (active === document.body || !visibleControl(active)) { event.preventDefault(); focusView(); }
    return;
  }
  if (textInput(active) && (event.key.length === 1 || event.key === 'Backspace' || event.key === 'Delete')) beginEditing(active);
}
document.addEventListener('keydown', event => {
  lastInputDevice = 'keyboard'; keyboardHint(); handleNavigationKey(event);
});
function navigateController(command) {
  lastInputDevice = 'controller'; controllerUsesSymbols = /dualsense|dualshock|playstation|sony|054c/i.test(command.id);
  document.body.classList.add('keyboard-navigation'); navigationOrigin = 'arrow'; keyboardHint();
  const active = document.activeElement;
  if (!navigationControls().includes(active) && !editingControl) {
    const scope = navigationScope();
    focusControl(scope === document.body ? (view === 'library' ? document.querySelector('.game-tile.selected') : scope.querySelector('button:not(:disabled)')) : navigationControls()[0]);
    // Establish focus without activating an arbitrary control on the first press.
    if (command.key === 'Enter') return;
  }
  handleNavigationKey({ key: command.key, preventDefault() {} }, true);
  keyboardHint();
}
// Sound follows actual controls/actions; typing and disabled controls remain quiet.
document.addEventListener('click', event => {
  const control = event.target.closest('button, summary, input[type=checkbox]');
  if (!control || !visibleControl(control)) return;
  const back = ['settings-close', 'exit-cancel', 'convert-back', 'back-step-1', 'complete-library'].includes(control.id);
  const open = ['settings-open', 'wizard-settings', 'game-menu-toggle', 'pick-source', 'pick-iso', 'pick-entry', 'pick-destination'].includes(control.id) || !!control.dataset.setting;
  sound(back ? 'back' : open ? 'open' : 'confirm');
}, true);
document.addEventListener('pointerover', event => {
  if (event.pointerType && event.pointerType !== 'mouse') return;
  const control = event.target.closest('button, summary');
  if (control && visibleControl(control) && control !== event.relatedTarget?.closest?.('button, summary')) sound('navigate');
});
document.addEventListener('focusin', () => { if (navigationOrigin === 'tab') sound('navigate'); });
function clock() { $('clock').textContent = new Intl.DateTimeFormat('en-GB', { hour: '2-digit', minute: '2-digit' }).format(new Date()); }
clock(); setInterval(clock, 15000);
refresh().then(() => { $('destination-input').value = state.settings.destinationParent; focusView(); window.AnyPS5ControllerNavigation.start(navigateController, api); }).catch(err => toast(err.message, 30000));
