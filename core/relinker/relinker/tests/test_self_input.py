from pathlib import Path
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

from test_optional_plt import fixture


def wrap_self(elf, magic):
    count = struct.unpack_from('<H', elf, 56)[0]
    headers = [struct.unpack_from('<II6Q', elf, 64 + i * 56) for i in range(count)]
    payloads = [(i, ph) for i, ph in enumerate(headers) if ph[0] == 1 and ph[5]]
    elf_offset = 32 + 32 * len(payloads)
    header_size = (elf_offset + 64 + count * 56 + 15) & ~15
    result = bytearray(header_size)
    struct.pack_into('<I4BIHHQHHI', result, 0, magic, 0, 1, 1, 0x12, 0x101,
                     header_size, 0, 0, len(payloads), 0x22, 0)
    result[elf_offset:elf_offset + 64 + count * 56] = elf[:64 + count * 56]
    for index, (segment, ph) in enumerate(payloads):
        struct.pack_into('<4Q', result, 32 + index * 32,
                         0x2804 | (segment << 20), len(result), ph[5], ph[5])
        result.extend(elf[ph[2]:ph[2] + ph[5]])
    struct.pack_into('<Q', result, 16, len(result))
    return result


def main():
    relinker = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='anyps5-self-') as directory:
        root = Path(directory)
        source = root / 'input.elf'
        output = root / 'output.exe'
        elf = fixture()
        elf[58:64] = bytes(6)
        source.write_bytes(elf)

        def convert(file):
            result = subprocess.run([str(relinker), '--windows', '--to-intel', '--skip-sce-module',
                                     str(file), str(output)], capture_output=True, text=True, timeout=30)
            assert result.returncode == 0, (result.stdout, result.stderr)
            data = output.read_bytes()
            assert data[:2] == b'MZ'
            return data

        reference = convert(source)
        for magic in (0x1d3d154f, 0xeef51454):
            source = root / 'eboot.bin'
            data = wrap_self(elf, magic)
            source.write_bytes(data)
            expected_hash = hashlib.sha256(data).digest()
            assert convert(source) == reference, 'SELF and ELF generated different Windows binaries'
            assert hashlib.sha256(source.read_bytes()).digest() == expected_hash
        if os.name == 'nt':
            executed = subprocess.run([str(output)], capture_output=True, text=True, timeout=20)
            assert executed.returncode == 42, (executed.returncode, executed.stdout, executed.stderr)
        for magic in (0x1d3d154f, 0xeef51454):
            for flag, message in ((2, 'Encrypted SELF payload'), (8, 'Compressed SELF payload')):
                data = wrap_self(elf, magic)
                flags, = struct.unpack_from('<Q', data, 32)
                struct.pack_into('<Q', data, 32, flags | flag)
                source.write_bytes(data)
                rejected_output = root / 'rejected.exe'
                result = subprocess.run([str(relinker), '--windows', '--skip-sce-module',
                                         str(source), str(rejected_output)], capture_output=True,
                                        text=True, timeout=30)
                assert result.returncode != 0
                assert message in result.stdout + result.stderr
                if flag == 2:
                    assert 'cannot decrypt' in result.stdout + result.stderr
                assert not rejected_output.exists()
                assert source.read_bytes() == data
    print('PASS: ELF/SELF output equivalence, unchanged inputs, and explicit encrypted/compressed rejection')


if __name__ == '__main__':
    main()
