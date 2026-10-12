#pragma once

#include <array>
#include <cstdint>

namespace HostProcess::WindowsRuntime {

struct SymbolIdentity {
    std::array<unsigned char, 16> guid;
    std::uint32_t age;
};

// Windows x64 profile: ntdll/kernelbase/imm32 10.0.26100.9278,
// user32 10.0.26100.8875. Each RVA was resolved from the matching
// Microsoft PDB and its access width/calling convention checked in that DLL.
// The RSDS identity is checked before any internal state is accessed. New
// Windows builds need a separately validated profile, never guessed offsets.
// Symbol provenance: https://msdl.microsoft.com/download/symbols/
// ntdll.pdb/C3093720177DA6851519DCFFE8DF53FE1/ntdll.pdb
// kernelbase.pdb/72CCE506944DE0267C172AD64287F9BD1/kernelbase.pdb
// user32.pdb/CDB947D1B96B87977915DE04CF54279C1/user32.pdb
// imm32.pdb/FCE5E7A9B4504EB08BF6492A4AD1E07B1/imm32.pdb
inline constexpr SymbolIdentity NtdllIdentity{{0x20,0x37,0x09,0xc3,0x7d,0x17,0x85,0xa6,0x15,0x19,0xdc,0xff,0xe8,0xdf,0x53,0xfe}, 1};
inline constexpr SymbolIdentity KernelbaseIdentity{{0x06,0xe5,0xcc,0x72,0x4d,0x94,0x26,0xe0,0x7c,0x17,0x2a,0xd6,0x42,0x87,0xf9,0xbd}, 1};
inline constexpr SymbolIdentity User32Identity{{0xd1,0x47,0xb9,0xcd,0x6b,0xb9,0x97,0x87,0x79,0x15,0xde,0x04,0xcf,0x54,0x27,0x9c}, 1};
inline constexpr SymbolIdentity Imm32Identity{{0xa9,0xe7,0xe5,0xfc,0x50,0xb4,0xb0,0x4e,0x8b,0xf6,0x49,0x2a,0x4a,0xd1,0xe0,0x7b}, 1};
inline constexpr std::uintptr_t CsrServerApiRoutine = 0x1d4df0;
inline constexpr std::uintptr_t CsrClientProcess = 0x1d4df8;
inline constexpr std::uintptr_t CsrInitOnceDone = 0x1d4df9;
inline constexpr std::uintptr_t CsrPortHandle = 0x1d4e20;
inline constexpr std::uintptr_t CsrClientConnectToServer = 0xdb7a0;
inline constexpr std::uintptr_t BaseStaticServerData = 0x3aedb8;
inline constexpr std::uintptr_t UserSharedInfo = 0xda350;
inline constexpr std::uintptr_t UserServerInfo = 0xda590;
inline constexpr std::uintptr_t PebReadOnlySharedMemoryBase = 0x88;
inline constexpr std::uint32_t NotImplemented = 0xc0000002u;

}
