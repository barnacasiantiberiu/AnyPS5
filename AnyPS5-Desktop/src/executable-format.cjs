const fs = require('node:fs/promises');

const elfMagic = header => header.length >= 4 && header.subarray(0, 4).equals(Buffer.from([127, 69, 76, 70]));
const selfMagic = header => header.length >= 4 && [0x1d3d154f, 0xeef51454].includes(header.readUInt32LE(0));

async function inspectExecutable(file) {
  const handle = await fs.open(file, 'r');
  try {
    const header = Buffer.alloc(32);
    const { bytesRead } = await handle.read(header, 0, header.length, 0);
    if (elfMagic(header)) return { elf: true, self: false, supported: true, encrypted: false, diagnostic: '' };
    if (!selfMagic(header)) return { elf: false, self: false, supported: false, encrypted: false, diagnostic: 'Not a recognized ELF or SELF executable.' };
    const result = { elf: false, self: true, supported: false, encrypted: false, diagnostic: '' };
    const invalid = message => ({ ...result, diagnostic: message });
    if (bytesRead !== 32) return invalid('Truncated SELF header. Select a complete SELF or ELF executable.');
    if (header[4] !== 0 || header[6] !== 1) return invalid('Unsupported SELF version or byte order.');
    const size = (await handle.stat()).size;
    const headerSize = header.readUInt16LE(12);
    const payloadStart = headerSize + header.readUInt16LE(14);
    const fileSize = header.readBigUInt64LE(16);
    const count = header.readUInt16LE(24);
    const elfOffset = 32 + count * 32;
    if (fileSize > BigInt(size) || BigInt(payloadStart) > fileSize || elfOffset + 64 > headerSize) return invalid('Invalid SELF header or file size.');
    const entries = Buffer.alloc(headerSize - 32);
    if ((await handle.read(entries, 0, entries.length, 32)).bytesRead !== entries.length) return invalid('Truncated SELF segment table.');
    const elf = entries.subarray(count * 32);
    if (!elfMagic(elf) || elf[4] !== 2 || elf[5] !== 1 || elf[6] !== 1 || elf.readUInt16LE(18) !== 62) return invalid('SELF does not contain a little-endian x86-64 ELF executable.');
    let compressed = false;
    for (let i = 0; i < count; i++) {
      const flags = entries.readBigUInt64LE(i * 32);
      if (!(flags & 0x800n)) continue;
      if (flags & 2n) result.encrypted = true;
      if (flags & 8n) compressed = true;
    }
    if (result.encrypted) return invalid('Encrypted SELF payload. Select a decrypted ELF or unencrypted SELF counterpart; AnyPS5 cannot decrypt this file.');
    if (compressed) return invalid('Compressed SELF payload is not supported yet. Select an uncompressed SELF or ELF counterpart.');
    return { ...result, supported: true };
  } finally { await handle.close(); }
}

module.exports = { elfMagic, selfMagic, inspectExecutable };
