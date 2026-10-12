#include "ModuleChecks.hpp"
#include "prx/libc/include/ApplicationHeap.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestHeap.hpp"
#include "prx/libc/include/GuestProcess.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" {
char* APS5_VABI getcwd_nid_postfix(char*, std::size_t);
[[noreturn]] void APS5_VABI _exit_nid_postfix(int);
}

namespace {

using ModuleTest::Require;
HANDLE initializeEntered = nullptr;
HANDLE initializeRelease = nullptr;
bool forkDuringAllocation = false;
unsigned allocatorRejections = 0;
unsigned invalidatorRejections = 0;

void ExpectRejectedFork() {
    const int child = GuestFork_nid_no_patch();
    const int error = errno;
    if (child == 0) _exit_nid_postfix(96);
    if (child > 0) {
        int status = 0;
        GuestWaitPid_nid_no_patch(child, &status, 0);
    }
    Require(child == -1 && error == 35);
}

void APS5_VABI Initialize() {
    Require(SetEvent(initializeEntered));
    Require(WaitForSingleObject(initializeRelease, 3000) == WAIT_OBJECT_0);
}

void* APS5_VABI Allocate(std::size_t bytes) {
    if (forkDuringAllocation) {
        ExpectRejectedFork();
        ++allocatorRejections;
    }
    return GuestHeap::GuestHeapAllocate_nid_postfix(bytes);
}

void APS5_VABI Free(void* pointer) { GuestHeap::GuestHeapFree_nid_postfix(pointer); }
void* APS5_VABI Reallocate(void* pointer, std::size_t bytes) { return GuestHeap::GuestHeapReallocate_nid_postfix(pointer, bytes); }
void* APS5_VABI Calloc(std::size_t count, std::size_t bytes) {
    if (bytes && count > std::numeric_limits<std::size_t>::max() / bytes) throw std::bad_alloc();
    auto* pointer = Allocate(count * bytes);
    std::memset(pointer, 0, count * bytes);
    return pointer;
}
void* APS5_VABI Align(std::size_t alignment, std::size_t bytes) { return GuestHeap::GuestHeapAlign_nid_postfix(alignment, bytes); }
void* APS5_VABI Realign(void* pointer, std::size_t bytes, std::size_t alignment) {
    return GuestHeap::GuestHeapRealign_nid_postfix(pointer, bytes, alignment);
}
int APS5_VABI PosixAlign(void** pointer, std::size_t alignment, std::size_t bytes) {
    if (!pointer || alignment < sizeof(void*) || (alignment & (alignment - 1))) return 22;
    try { *pointer = Align(alignment, bytes); return 0; }
    catch (const std::bad_alloc&) { return 12; }
}

template<class Value, std::size_t Size>
void Write(std::array<std::byte, Size>& data, std::size_t offset, Value value) {
    Require(offset <= Size && sizeof(value) <= Size - offset);
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

void InitializeAllocator() {
    std::array<std::byte, 0x40> process{};
    std::array<std::byte, 0x38> libc{};
    std::array<std::byte, 0x78> replacement{};
    Write(process, 0, std::uint64_t{0x40});
    Write(process, 8, std::uint32_t{0x4942524f});
    Write(process, 0x38, libc.data());
    Write(libc, 0, std::uint64_t{0x38});
    Write(libc, 0x30, replacement.data());
    Write(replacement, 0, std::uint64_t{0x78});
    Write(replacement, 8, std::uint64_t{2});
    Write(replacement, 0x10, Initialize);
    const std::array<void*, 10> api{reinterpret_cast<void*>(Allocate), reinterpret_cast<void*>(Free),
        reinterpret_cast<void*>(Calloc), reinterpret_cast<void*>(Reallocate), reinterpret_cast<void*>(Align),
        reinterpret_cast<void*>(Realign), reinterpret_cast<void*>(PosixAlign)};
    std::memcpy(replacement.data() + 0x20, api.data(), sizeof(api));
    initializeEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    initializeRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Require(initializeEntered && initializeRelease);
    std::thread initializer([&] { ApplicationHeapInitialize_nid_no_patch(process.data()); });
    Require(WaitForSingleObject(initializeEntered, 3000) == WAIT_OBJECT_0);
    ExpectRejectedFork();
    Require(SetEvent(initializeRelease));
    initializer.join();
    CloseHandle(initializeEntered);
    CloseHandle(initializeRelease);
}

void Invalidate(std::uintptr_t, std::size_t) {
    ExpectRejectedFork();
    ++invalidatorRejections;
}

}

void GuestForkCallbacks_nid_no_patch() {
    InitializeAllocator();
    forkDuringAllocation = true;
    char* directory = getcwd_nid_postfix(nullptr, 0);
    forkDuringAllocation = false;
    Require(directory && allocatorRejections == 1);
    ApplicationHeapFree_nid_no_patch(directory);
    auto* memory = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Require(memory != nullptr);
    GuestAllocations::GuestAllocationsSetInvalidator_nid_postfix(Invalidate);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(memory, 4096, true, true, false);
    }
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(memory);
    }
    GuestAllocations::GuestAllocationsSetInvalidator_nid_postfix(nullptr);
    Require(invalidatorRejections == 2);
    Require(VirtualFree(memory, 0, MEM_RELEASE));
    const int child = GuestFork_nid_no_patch();
    Require(child >= 0);
    if (child == 0) _exit_nid_postfix(23);
    int status = 0;
    Require(GuestWaitPid_nid_no_patch(child, &status, 0) == child && status == (23 << 8));
    std::puts("Fork rejected initialization, cwd allocator and mutation callbacks; released owners remain usable");
}
