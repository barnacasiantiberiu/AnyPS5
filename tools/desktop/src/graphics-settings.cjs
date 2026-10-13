const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');

const resolutions = [
  { value: 'native', label: 'Original game resolution' },
  { value: '1280x720', label: '1280 x 720 (720p)', width: 1280, height: 720 },
  { value: '1600x900', label: '1600 x 900 (900p)', width: 1600, height: 900 },
  { value: '1920x1080', label: '1920 x 1080 (Full HD)', width: 1920, height: 1080 },
  { value: '2560x1440', label: '2560 x 1440 (1440p)', width: 2560, height: 1440 },
  { value: '3840x2160', label: '3840 x 2160 (4K)', width: 3840, height: 2160 }
];
const widthKeys = ['platform-ps5-video-out-width', 'platform-ps5-standardmode-video-out-width'];
const defaultGraphics = () => ({ resolution: '1920x1080' });

function validateGraphics(value = defaultGraphics()) {
  if (!value || typeof value !== 'object' || Array.isArray(value) || !resolutions.some(item => item.value === value.resolution)) {
    throw new Error('Choose a resolution from the Graphics list.');
  }
  return { resolution: value.resolution };
}
function rewriteBootConfig(text, width) {
  let found = false;
  const rewritten = text.replace(/^(\uFEFF?platform-ps5-(?:standardmode-)?video-out-width)=(\d+)(\r?)$/gm, (line, key, previous, end) => {
    found = true;
    return key + '=' + width + end;
  });
  return found ? rewritten : null;
}
async function optionalRead(file) {
  try {
    const info = await fs.lstat(file);
    if (!info.isFile() || info.size > 1024 * 1024) throw new Error('Invalid graphics configuration file: ' + file);
    return new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(await fs.readFile(file));
  } catch (error) {
    if (error.code === 'ENOENT') return null;
    throw error;
  }
}
async function writeAtomic(file, text) {
  const temporary = file + '.' + crypto.randomUUID() + '.tmp';
  try {
    await fs.writeFile(temporary, text, { flag: 'wx' });
    await fs.rename(temporary, file);
  } finally {
    await fs.unlink(temporary).catch(error => { if (error.code !== 'ENOENT') throw error; });
  }
}
async function prepareGraphics(executable, value) {
  const settings = validateGraphics(value);
  const selected = resolutions.find(item => item.value === settings.resolution);
  const directory = path.dirname(executable);
  for (const relative of ['app0/Media/boot.config', 'app0/boot.config']) {
    const file = path.join(directory, relative);
    const current = await optionalRead(file);
    if (current === null || !widthKeys.some(key => current.includes(key + '='))) continue;
    const backupFile = file + '.anyps5-resolution.json';
    const saved = await optionalRead(backupFile);
    let backup;
    if (saved !== null) {
      backup = JSON.parse(saved);
      if (backup.version !== 1 || !['original', 'lastApplied', 'previousApplied'].every(key => typeof backup[key] === 'string')) {
        throw new Error('Invalid graphics recovery file: ' + backupFile);
      }
      if (![backup.original, backup.lastApplied, backup.previousApplied].includes(current)) {
        throw new Error('The game boot configuration changed outside AnyPS5 Desktop. Your changes were preserved; review ' + backupFile + ' before changing resolution.');
      }
    } else {
      if (settings.resolution === 'native') return { source: 'native', message: 'Using the original game resolution.' };
      backup = { version: 1, original: current, lastApplied: current, previousApplied: current };
    }
    const next = settings.resolution === 'native' ? backup.original : rewriteBootConfig(backup.original, selected.width);
    if (next === null) throw new Error('The saved game configuration has no supported resolution setting.');
    if (next !== current) {
      await writeAtomic(backupFile, JSON.stringify({ ...backup, previousApplied: current, lastApplied: next }, null, 2) + '\n');
      await writeAtomic(file, next);
    }
    return {
      source: 'unity',
      resolution: settings.resolution,
      message: settings.resolution === 'native' ? 'Original game resolution restored.' : 'Requested ' + selected.label + '. The game may adjust its internal rendering resolution.'
    };
  }
  return {
    source: 'native',
    message: settings.resolution === 'native' ? 'Using the original game resolution.' : 'This game has no supported resolution configuration. Its original resolution is unchanged.'
  };
}
module.exports = { resolutions, defaultGraphics, validateGraphics, rewriteBootConfig, prepareGraphics };
