# SELF executable input

The relinker accepts ELF and unencrypted, uncompressed SELF containers with either
the PS4 or PS5 container signature. This is container support, not PS4 game support
or SELF decryption. The Desktop folder and ISO flows recognize these inputs and
offer `.self`, `.elf`, and `eboot.bin` files.

`ElfReader::DecodeExecutable` reconstructs an ELF in memory from the embedded
header, program table, and SELF data-segment mappings before Intel instruction
conversion or guest module parsing. The input is never rewritten. Container
bounds, segment indexes, overlapping mappings, size agreement, and coverage of
runtime segments are checked. Reconstructed images are limited to 1 GiB.

SELF generally omits the ELF section table. Its section references are cleared.
Unmapped NOTE/version metadata absent from data segments has its file range
cleared; absent runtime segment data remains an error. Metadata signatures are
not authenticated. Encrypted and compressed payloads have separate diagnostics.
No keys, proprietary executables, or additional dependencies are included.

Module backups ending in `.esbak` remain resources in Desktop installations but
are not discovered as additional guest modules. This avoids loading both a SELF
module and its older ELF backup.

Validation: `elf_reader_bounds_tests` includes reconstruction, plaintext ELF
preservation, encrypted/compressed rejection, and malformed ranges. Optional
file arguments exercise real samples; adjacent `.esbak` files, when present,
are compared against every reconstructed load segment.
`test_self_input.py <relinker>` checks that ELF and both SELF signatures produce
identical Windows PE output through the Intel conversion path. Desktop tests
cover folder detection, installation orchestration, and ISO inspection. The CLI
test also verifies explicit encrypted/compressed errors and that no output is
created on rejection. `AnyPS5-Desktop/tests/self-input-ui.cjs` checks the visible
encrypted-input message and disabled Continue button for both SELF signatures,
then verifies that selecting an unencrypted input clears the error.

Format references:
- [OpenOrbis SELF writer and flag definitions](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/scripts/make_fself.py)
- [Measured PS4/PS5 container layouts](https://github.com/boykopovar/AnyPS5/discussions/2433)

Accepting a container does not establish that the game will convert or run with
the available system libraries and graphics implementation.
