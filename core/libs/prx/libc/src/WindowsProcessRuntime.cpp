#include "prx/libc/include/WindowsProcessRuntime.hpp"

#ifdef _WIN32
#include <winternl.h>
#include <cstring>

namespace HostProcess::WindowsRuntime {
namespace {

template<class T> T& Field(HMODULE module, std::uintptr_t rva) noexcept {
    return *reinterpret_cast<T*>(reinterpret_cast<unsigned char*>(module) + rva);
}

bool Range(std::size_t size, std::size_t start, std::size_t bytes) noexcept {
    return start <= size && bytes <= size - start;
}

bool Matches(HMODULE module, const SymbolIdentity& identity) noexcept {
    if (!module) return false;
    const auto* bytes = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes + dos->e_lfanew);
    return MatchesImage_nid_no_patch(module, nt->OptionalHeader.SizeOfImage, identity);
}

bool Mapped(const void* pointer) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    return pointer && VirtualQuery(pointer, &memory, sizeof(memory)) &&
        memory.State == MEM_COMMIT && memory.Type == MEM_MAPPED &&
        (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

bool AcquireOptional(const wchar_t* name, HMODULE& module) noexcept {
    // GetModuleHandleEx increments the reference atomically under the loader
    // lock. A borrowed handle can be unmapped before identity validation.
    if (GetModuleHandleExW(0, name, &module)) return true;
    module = nullptr;
    return GetLastError() == ERROR_MOD_NOT_FOUND;
}

std::size_t SharedExtent(const void* base, const void* staticData) noexcept {
    MEMORY_BASIC_INFORMATION shared{}, data{};
    if (!base || !staticData || !VirtualQuery(base, &shared, sizeof(shared)) ||
        !VirtualQuery(staticData, &data, sizeof(data))) return 0;
    if (shared.State != MEM_COMMIT || shared.Type != MEM_MAPPED || shared.AllocationBase != base ||
        shared.BaseAddress != base || data.AllocationBase != base || data.BaseAddress != base ||
        data.State != MEM_COMMIT || data.Type != MEM_MAPPED ||
        (shared.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 ||
        (data.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return 0;
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    const auto pointer = reinterpret_cast<std::uintptr_t>(staticData);
    return pointer >= start && pointer - start < shared.RegionSize ? shared.RegionSize : 0;
}

}

Preparation::~Preparation() { Reset_nid_no_patch(); }

void Preparation::Reset_nid_no_patch() noexcept {
    if (imm32) FreeLibrary(imm32);
    if (user32) FreeLibrary(user32);
    ntdll = nullptr;
    kernelbase = nullptr;
    user32 = nullptr;
    imm32 = nullptr;
    connect = nullptr;
    registerImm = nullptr;
    controlRoutine = nullptr;
    oldSharedBase = nullptr;
    oldStaticData = nullptr;
    oldSharedBytes = 0;
    directory.fill(0);
}

bool MatchesImage_nid_no_patch(const void* image, std::size_t size, const SymbolIdentity& identity) noexcept {
    if (!image || size < sizeof(IMAGE_DOS_HEADER)) return false;
    const auto* bytes = static_cast<const unsigned char*>(image);
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, bytes, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        !Range(size, static_cast<std::size_t>(dos.e_lfanew), sizeof(IMAGE_NT_HEADERS64))) return false;
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, bytes + dos.e_lfanew, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG) return false;
    const auto& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!directory.Size || directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY) != 0 ||
        !Range(size, directory.VirtualAddress, directory.Size)) return false;
    for (std::size_t offset = 0; offset < directory.Size; offset += sizeof(IMAGE_DEBUG_DIRECTORY)) {
        IMAGE_DEBUG_DIRECTORY debug{};
        std::memcpy(&debug, bytes + directory.VirtualAddress + offset, sizeof(debug));
        if (debug.Type != IMAGE_DEBUG_TYPE_CODEVIEW || debug.SizeOfData < 24 ||
            !Range(size, debug.AddressOfRawData, debug.SizeOfData)) continue;
        const auto* data = bytes + debug.AddressOfRawData;
        std::uint32_t age = 0;
        std::memcpy(&age, data + 20, sizeof(age));
        if (std::memcmp(data, "RSDS", 4) == 0 &&
            std::memcmp(data + 4, identity.guid.data(), identity.guid.size()) == 0 && age == identity.age) return true;
    }
    return false;
}

std::uint32_t Prepare_nid_no_patch(Preparation& preparation) noexcept {
    preparation.Reset_nid_no_patch();
    struct Rollback {
        Preparation& preparation;
        bool complete = false;
        ~Rollback() { if (!complete) preparation.Reset_nid_no_patch(); }
    } rollback{preparation};
    preparation.ntdll = GetModuleHandleW(L"ntdll.dll");
    preparation.kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (!AcquireOptional(L"user32.dll", preparation.user32) ||
        !AcquireOptional(L"imm32.dll", preparation.imm32)) return NotImplemented;
    if (!Matches(preparation.ntdll, NtdllIdentity) || !Matches(preparation.kernelbase, KernelbaseIdentity) ||
        (preparation.user32 && !Matches(preparation.user32, User32Identity)) ||
        (preparation.imm32 && (!preparation.user32 || !Matches(preparation.imm32, Imm32Identity)))) return NotImplemented;
    const auto connect = GetProcAddress(preparation.ntdll, "CsrClientConnectToServer");
    if (reinterpret_cast<std::uintptr_t>(connect) != reinterpret_cast<std::uintptr_t>(preparation.ntdll) + CsrClientConnectToServer)
        return NotImplemented;
    std::memcpy(&preparation.connect, &connect, sizeof(connect));
    preparation.controlRoutine = reinterpret_cast<void*>(GetProcAddress(preparation.kernelbase, "CtrlRoutine"));
    if (!preparation.controlRoutine) return NotImplemented;
    if (preparation.user32 && reinterpret_cast<std::uintptr_t>(GetProcAddress(preparation.user32, "gSharedInfo")) !=
        reinterpret_cast<std::uintptr_t>(preparation.user32) + UserSharedInfo) return NotImplemented;
    if (preparation.imm32) {
        const auto registerImm = GetProcAddress(preparation.imm32, "ImmRegisterClient");
        std::memcpy(&preparation.registerImm, &registerImm, sizeof(registerImm));
        if (!preparation.registerImm) return NotImplemented;
    }
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return NotImplemented;
    // Avoid CRT formatting/locale locks while runtime owners are quiesced.
    constexpr wchar_t prefix[] = L"\\Sessions\\";
    constexpr wchar_t suffix[] = L"\\Windows";
    std::size_t at = sizeof(prefix) / sizeof(wchar_t) - 1;
    std::memcpy(preparation.directory.data(), prefix, at * sizeof(wchar_t));
    std::array<wchar_t, 10> digits{};
    std::size_t count = 0;
    do {
        digits[count++] = static_cast<wchar_t>(L'0' + session % 10);
        session /= 10;
    } while (session != 0);
    while (count != 0) preparation.directory[at++] = digits[--count];
    std::memcpy(preparation.directory.data() + at, suffix, sizeof(suffix));
    const auto* peb = reinterpret_cast<const unsigned char*>(NtCurrentTeb()->ProcessEnvironmentBlock);
    preparation.oldSharedBase = *reinterpret_cast<void* const*>(peb + PebReadOnlySharedMemoryBase);
    preparation.oldStaticData = Field<void*>(preparation.kernelbase, BaseStaticServerData);
    preparation.oldSharedBytes = SharedExtent(preparation.oldSharedBase, preparation.oldStaticData);
    if (!preparation.oldSharedBytes) return NotImplemented;
    rollback.complete = true;
    return 0;
}

std::uint32_t Reconnect_nid_no_patch(const Preparation& preparation) noexcept {
    // Another host thread may have loaded/unloaded USER between preparation
    // and the native clone acquiring the loader lock. Such a different module
    // snapshot has no validated initialization plan and must fail closed.
    if (GetModuleHandleW(L"ntdll.dll") != preparation.ntdll ||
        GetModuleHandleW(L"kernelbase.dll") != preparation.kernelbase ||
        GetModuleHandleW(L"user32.dll") != preparation.user32 ||
        GetModuleHandleW(L"imm32.dll") != preparation.imm32) return NotImplemented;
    if (!Matches(preparation.ntdll, NtdllIdentity) || !Matches(preparation.kernelbase, KernelbaseIdentity) ||
        (preparation.user32 && !Matches(preparation.user32, User32Identity)) ||
        (preparation.imm32 && !Matches(preparation.imm32, Imm32Identity))) return NotImplemented;
    // RtlCloneUserProcess omits ViewUnmap CSR/USER sections. Reinitialize only
    // the four named CSR fields needed by this exact ntdll build. Do not copy
    // WINNIE's broad zero range, whose layout differs across Windows versions.
    Field<void*>(preparation.ntdll, CsrServerApiRoutine) = nullptr;
    Field<unsigned char>(preparation.ntdll, CsrInitOnceDone) = 0;
    Field<unsigned char>(preparation.ntdll, CsrClientProcess) = 0;
    Field<HANDLE>(preparation.ntdll, CsrPortHandle) = nullptr;
    BOOLEAN server = FALSE;
    auto control = preparation.controlRoutine;
    auto* directory = const_cast<wchar_t*>(preparation.directory.data());
    LONG status = preparation.connect(directory, 1, &control, sizeof(control), &server);
    if (status < 0) return static_cast<std::uint32_t>(status);
    if (server) return NotImplemented;
    const auto* peb = reinterpret_cast<const unsigned char*>(NtCurrentTeb()->ProcessEnvironmentBlock);
    const auto sharedBase = *reinterpret_cast<void* const*>(peb + PebReadOnlySharedMemoryBase);
    // The server reconnects this read-only region at the prior client address.
    // Reject a different layout instead of leaving other cached pointers stale.
    if (sharedBase != preparation.oldSharedBase ||
        SharedExtent(sharedBase, preparation.oldStaticData) != preparation.oldSharedBytes) return NotImplemented;
    if (!preparation.user32) return 0;
    alignas(16) std::array<unsigned char, 0x248> connection{};
    status = preparation.connect(directory, 3, connection.data(), static_cast<ULONG>(connection.size()), &server);
    if (status < 0) return static_cast<std::uint32_t>(status);
    if (server) return NotImplemented;
    void* serverInfo = nullptr;
    std::memcpy(&serverInfo, connection.data() + 8, sizeof(serverInfo));
    if (!Mapped(serverInfo)) return NotImplemented;
    auto* shared = reinterpret_cast<unsigned char*>(preparation.user32) + UserSharedInfo;
    // This is the same 0x240-byte shared-info copy performed by USER32's own
    // initialization after its 0x248-byte CSR server-3 request on this build.
    std::memcpy(shared, connection.data() + 8, 0x240);
    Field<void*>(preparation.user32, UserServerInfo) = serverInfo;
    if (preparation.registerImm && !preparation.registerImm(shared, preparation.imm32)) return NotImplemented;
    return 0;
}

}
#endif
