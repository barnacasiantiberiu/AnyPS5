const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');

function defaultInput() { return { leftStickDeadzone: 0, rightStickDeadzone: 0, mouseSensitivity: 1 }; }
function validateInput(value = defaultInput()) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('Invalid input settings.');
  for (const key of ['leftStickDeadzone', 'rightStickDeadzone']) {
    if (!Number.isInteger(value[key]) || value[key] < 0 || value[key] > 90) throw new Error('Stick deadzones must be whole percentages from 0 to 90.');
  }
  if (typeof value.mouseSensitivity !== 'number' || !Number.isFinite(value.mouseSensitivity) || value.mouseSensitivity <= 0 || value.mouseSensitivity > 10) throw new Error('Mouse sensitivity must be greater than 0 and at most 10.');
  return { leftStickDeadzone: value.leftStickDeadzone, rightStickDeadzone: value.rightStickDeadzone, mouseSensitivity: value.mouseSensitivity };
}
function profileText(value) {
  const settings = validateInput(value);
  return [
    '# AnyPS5 Desktop input profile',
    '# SDL maps the first recognized controller automatically.',
    '# A/B/X/Y = Cross/Circle/Square/Triangle; LB/RB = L1/R1;',
    '# LT/RT = analog L2/R2; Start = Options; sticks and D-pad pass through.',
    '# Omitted keyboard/mouse actions keep the AnyPS5 built-in bindings.',
    'LeftStickDeadzone = ' + settings.leftStickDeadzone,
    'RightStickDeadzone = ' + settings.rightStickDeadzone,
    'MouseSensitivity = ' + settings.mouseSensitivity,
    ''
  ].join('\n');
}
async function prepareInput(executable, userData, value, inherited = process.env) {
  const text = profileText(value);
  const gameConfig = path.join(path.dirname(executable), 'anyps5-input.ini');
  let configPath = gameConfig;
  let source = 'game';
  const existing = await fs.stat(gameConfig).catch(error => { if (error.code === 'ENOENT') return null; throw error; });
  if (existing && !existing.isFile()) throw new Error('The game input configuration must be a regular anyps5-input.ini file.');
  if (existing) await fs.access(gameConfig, require('node:fs').constants.R_OK);
  else {
    source = 'launcher';
    const directory = path.join(userData, 'input-profiles');
    await fs.mkdir(directory, { recursive: true });
    // Immutable profiles let games starting together use independent saved settings.
    const name = crypto.createHash('sha256').update(text).digest('hex') + '.ini';
    configPath = path.join(directory, name);
    const saved = await fs.readFile(configPath, 'utf8').catch(error => { if (error.code === 'ENOENT') return null; throw error; });
    if (saved !== null && saved !== text) throw new Error('The saved launcher input profile was modified. Remove that profile from ' + directory + ' and try again.');
    if (saved === null) {
      const temporary = path.join(directory, 'profile-' + crypto.randomUUID() + '.tmp');
      try {
        await fs.writeFile(temporary, text, { flag: 'wx' });
        try { await fs.rename(temporary, configPath); }
        catch (error) {
          // Windows can refuse replacing an identical profile another launch just published.
          if (!['EPERM', 'EEXIST', 'EACCES'].includes(error.code) || await fs.readFile(configPath, 'utf8').catch(() => null) !== text) throw error;
        }
      } finally { await fs.unlink(temporary).catch(error => { if (error.code !== 'ENOENT') throw error; }); }
    }
  }
  // Explicit per-child setting: never changes the Windows environment or game files.
  const env = Object.fromEntries(Object.entries(inherited).filter(([key]) => key.toUpperCase() !== 'ANYPS5_INPUT_CONFIG'));
  env.ANYPS5_INPUT_CONFIG = configPath;
  return { configPath, source, env };
}
module.exports = { defaultInput, validateInput, profileText, prepareInput };
