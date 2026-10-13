const fs = require('node:fs/promises');
const path = require('node:path');

const MODULE_DIRS = ['sce_module', 'sce_modules', 'prx'];
const candidateName = name => /\.elf$/i.test(name) || /^(eboot\.bin|eboot)$/i.test(name);
const within = (parent, child) => {
  const relative = path.relative(path.resolve(parent), path.resolve(child));
  return relative === '' || (!relative.startsWith(`..${path.sep}`) && relative !== '..' && !path.isAbsolute(relative));
};
async function magic(file) {
  const handle = await fs.open(file, 'r');
  try { const buffer = Buffer.alloc(4); await handle.read(buffer, 0, 4, 0); return buffer; }
  finally { await handle.close(); }
}
const elfMagic = header => header.equals(Buffer.from([127, 69, 76, 70]));
const selfMagic = header => header.equals(Buffer.from([0x4f, 0x15, 0x3d, 0x1d])) || header.equals(Buffer.from([0x54, 0x14, 0xf5, 0xee]));
const isElf = async file => elfMagic(await magic(file));
const rootEntries = directory => fs.readdir(directory, { withFileTypes: true });
const hasExecutable = entries => entries.some(e => e.isFile() && candidateName(e.name));
const hasModules = entries => entries.some(e => e.isDirectory() && MODULE_DIRS.includes(e.name));

async function resolveSourceRoot(folder) {
  if (typeof folder !== 'string' || !folder.trim()) throw new Error('Choose the game folder.');
  let source = await fs.realpath(folder);
  if (!(await fs.stat(source)).isDirectory()) throw new Error('Choose the game folder.');
  // A decrypted overlay is only a set of replacement binaries, not the game resources.
  if (path.basename(source).toLowerCase() === 'decrypted') {
    const parent = path.dirname(source);
    const entries = await rootEntries(parent);
    if (hasExecutable(entries) && hasModules(entries)) source = parent;
  }
  for (let depth = 0; depth < 4; depth++) {
    const entries = await rootEntries(source);
    if (hasExecutable(entries) || hasModules(entries)) break;
    const children = entries.filter(e => e.isDirectory() && !e.isSymbolicLink());
    if (children.length !== 1) break;
    source = path.join(source, children[0].name);
  }
  return source;
}
async function executableCandidates(directory) {
  const entries = (await rootEntries(directory)).filter(e => e.isFile() && candidateName(e.name));
  entries.sort((a, b) => Number(!/^eboot/i.test(a.name)) - Number(!/^eboot/i.test(b.name)) || a.name.localeCompare(b.name));
  return Promise.all(entries.map(async entry => {
    const file = path.join(directory, entry.name);
    const header = await magic(file);
    return { file, elf: elfMagic(header), self: selfMagic(header) };
  }));
}
async function inspectSource(folder) {
  const source = await resolveSourceRoot(folder);
  const candidates = await executableCandidates(source);
  const directories = await rootEntries(source);
  const overlay = directories.find(e => e.isDirectory() && !e.isSymbolicLink() && e.name.toLowerCase() === 'decrypted');
  const overlayRoot = overlay ? path.join(source, overlay.name) : '';
  const overlayCandidates = overlayRoot ? await executableCandidates(overlayRoot) : [];
  // Only recognize this layout when a replacement for a root executable is present.
  const usableOverlay = overlayCandidates.some(e => e.elf && candidates.some(c => path.basename(c.file).toLowerCase() === path.basename(e.file).toLowerCase()));
  const decryptedRoot = usableOverlay ? overlayRoot : '';
  const executables = [...candidates, ...(usableOverlay ? overlayCandidates : [])].filter(e => e.elf).map(e => e.file)
    .sort((a, b) => Number(!/^eboot/i.test(path.basename(a))) - Number(!/^eboot/i.test(path.basename(b))));
  let title = path.basename(source);
  let titleId = '';
  try {
    const param = JSON.parse(await fs.readFile(path.join(source, 'sce_sys', 'param.json'), 'utf8'));
    const texts = param.localizedParameters || {};
    title = texts['en-US']?.titleName || texts['en-GB']?.titleName || Object.values(texts).find(v => v?.titleName)?.titleName || title;
    titleId = param.titleId || '';
  } catch {}
  const encrypted = !executables.length && candidates.some(e => e.self);
  const diagnostic = executables.length ? '' : encrypted
    ? 'The game executable is a SELF file. Select a decrypted ELF executable; AnyPS5 cannot decrypt SELF files.'
    : candidates.length ? 'The game executable is not a recognized ELF file. Select a valid decrypted ELF executable.'
    : 'No executable was found. Choose the folder containing eboot.bin and the game modules, or select an ELF executable.';
  return { source, title, titleId, executables, entry: executables[0] || '', encrypted, decryptedRoot, diagnostic,
    note: decryptedRoot ? 'Using the decrypted executable and modules with the original game resources.' : '' };
}
async function checkEntry({ source, entry }) {
  const root = await fs.realpath(source);
  if (!(await fs.stat(root)).isDirectory()) throw new Error('Choose the game folder.');
  if (typeof entry !== 'string' || !entry.trim()) throw new Error('Choose a decrypted ELF executable.');
  const file = await fs.realpath(entry).catch(() => { throw new Error('The selected executable could not be found. Choose it again.'); });
  if (!within(root, file)) throw new Error('The selected executable is outside the game folder. Choose a file inside the selected game folder.');
  if (!(await fs.stat(file)).isFile()) throw new Error('Choose an executable file, not a folder.');
  const header = await magic(file);
  if (selfMagic(header)) throw new Error(`${path.relative(root, file)} is a SELF executable. Select its decrypted ELF counterpart; AnyPS5 cannot decrypt SELF files.`);
  if (!elfMagic(header)) throw new Error(`${path.relative(root, file)} is not a recognized ELF executable. Choose a valid decrypted ELF file.`);
  return file;
}
module.exports = { MODULE_DIRS, within, magic, elfMagic, selfMagic, isElf, inspectSource, checkEntry };
