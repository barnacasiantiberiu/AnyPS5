#include "ModuleChecks.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

extern "C" {
void APS5_VABI sceLibcInternalBacktraceForGame_nid_postfix(const char* heapName);
void APS5_VABI sceLibcInternalHeapErrorReportForGame_nid_postfix(void* heap, void* block, std::uint32_t error);
int* APS5_VABI __error_nid_postfix();
}

namespace {

using ModuleTest::Require;

int Duplicate(int descriptor) {
#ifdef _WIN32
    return ::_dup(descriptor);
#else
    return ::dup(descriptor);
#endif
}

bool Redirect(int source, int destination) {
#ifdef _WIN32
    return ::_dup2(source, destination) == 0;
#else
    return ::dup2(source, destination) == destination;
#endif
}

int Descriptor(std::FILE* stream) {
#ifdef _WIN32
    return ::_fileno(stream);
#else
    return ::fileno(stream);
#endif
}

void CloseDescriptor(int descriptor) {
#ifdef _WIN32
    Require(::_close(descriptor) == 0);
#else
    Require(::close(descriptor) == 0);
#endif
}

template <typename TFunction>
std::string Capture(TFunction function) {
    auto* captured = std::tmpfile();
    Require(captured != nullptr);
    Require(std::fflush(stderr) == 0);
    const int saved = Duplicate(Descriptor(stderr));
    Require(saved >= 0 && Redirect(Descriptor(captured), Descriptor(stderr)));
    try {
        function();
    } catch (...) {
        Redirect(saved, Descriptor(stderr));
        CloseDescriptor(saved);
        std::fclose(captured);
        throw;
    }
    Require(std::fflush(stderr) == 0);
    Require(Redirect(saved, Descriptor(stderr)));
    CloseDescriptor(saved);
    std::rewind(captured);
    std::string result;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = std::fread(buffer.data(), 1, buffer.size(), captured);
        result.append(buffer.data(), count);
        if (count != buffer.size()) break;
    }
    Require(!std::ferror(captured) && std::fclose(captured) == 0);
    for (std::size_t position = 0; (position = result.find("\r\n", position)) != std::string::npos;)
        result.erase(position, 1);
    return result;
}

std::vector<std::uintptr_t> ParseFrames(const std::string& report, std::size_t begin, std::size_t end) {
    std::vector<std::uintptr_t> addresses;
    while (begin < end) {
        const auto newline = report.find('\n', begin);
        Require(newline != std::string::npos && newline < end);
        const auto line = report.substr(begin, newline - begin);
        unsigned int index = 0;
        unsigned long long address = 0;
        int consumed = 0;
        Require(std::sscanf(line.c_str(), "  #%u 0x%llx%n", &index, &address, &consumed) == 2);
        Require(consumed == static_cast<int>(line.size()) && index == addresses.size() && address != 0);
        addresses.push_back(static_cast<std::uintptr_t>(address));
        begin = newline + 1;
    }
    Require(addresses.size() >= 3 && addresses.size() <= 32);
    return addresses;
}

std::vector<std::uintptr_t> CheckReport(const std::string& report, const std::string& header) {
    Require(report.starts_with(header));
    return ParseFrames(report, header.size(), report.size());
}

std::string ErrorHeader(void* heap, void* block, std::uint32_t error) {
    std::array<char, 256> buffer{};
    const int length = std::snprintf(buffer.data(), buffer.size(),
                                    "[libc] heap error 0x%x: heap %p, block %p (host frames):\n", error, heap, block);
    Require(length > 0 && static_cast<std::size_t>(length) < buffer.size());
    return buffer.data();
}

void CheckNames() {
    for (const char* name : std::array<const char*, 4>{"SceLibcHeap", "", "%n%s%p", nullptr}) {
        std::array<unsigned char, 40> guarded{};
        guarded.fill(0xa5);
        char* input = reinterpret_cast<char*>(guarded.data() + 8);
        if (name) std::memcpy(input, name, std::strlen(name) + 1);
        const auto before = guarded;
        const auto report = Capture([&] {
            *__error_nid_postfix() = 0x1357;
            sceLibcInternalBacktraceForGame_nid_postfix(name ? input : nullptr);
            Require(*__error_nid_postfix() == 0x1357);
        });
        CheckReport(report, "[" + std::string(name ? name : "") + "] heap backtrace (host frames):\n");
        Require(guarded == before);
    }
}

void CheckErrors() {
    std::array<unsigned char, 64> heap{};
    std::array<unsigned char, 64> block{};
    heap.fill(0x5a);
    block.fill(0xa5);
    const auto heapBefore = heap;
    const auto blockBefore = block;
    for (const auto error : {0u, 1u, 0x1234u, 0x80000000u, std::numeric_limits<std::uint32_t>::max()}) {
        const auto report = Capture([&] {
            *__error_nid_postfix() = 0x2468;
            sceLibcInternalHeapErrorReportForGame_nid_postfix(heap.data(), block.data(), error);
            Require(*__error_nid_postfix() == 0x2468);
        });
        CheckReport(report, ErrorHeader(heap.data(), block.data(), error));
        Require(heap == heapBefore && block == blockBefore);
    }
    CheckReport(Capture([] { sceLibcInternalHeapErrorReportForGame_nid_postfix(nullptr, nullptr, 0); }),
                ErrorHeader(nullptr, nullptr, 0));
#ifdef _WIN32
    auto* inaccessible = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    Require(inaccessible != nullptr);
#else
    const auto pageSize = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    auto* inaccessible = mmap(nullptr, pageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Require(inaccessible != MAP_FAILED);
#endif
    CheckReport(Capture([&] { sceLibcInternalHeapErrorReportForGame_nid_postfix(inaccessible, inaccessible, 0xffffffffu); }),
                ErrorHeader(inaccessible, inaccessible, 0xffffffffu));
#ifdef _WIN32
    Require(VirtualFree(inaccessible, 0, MEM_RELEASE) != 0);
#else
    Require(munmap(inaccessible, pageSize) == 0);
#endif
}

using TraceFunction = void (*)(std::array<std::uintptr_t, 2>&, unsigned int&);

void TraceLeaf(std::array<std::uintptr_t, 2>& expected, unsigned int& returned) {
    expected[0] = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    sceLibcInternalBacktraceForGame_nid_postfix("NestedHeap");
    ++returned;
}

TraceFunction volatile traceLeaf = TraceLeaf;

void TraceMiddle(std::array<std::uintptr_t, 2>& expected, unsigned int& returned) {
    expected[1] = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    traceLeaf(expected, returned);
    ++returned;
}

TraceFunction volatile traceMiddle = TraceMiddle;

void CheckNestedFrames() {
    std::array<std::uintptr_t, 2> expected{};
    unsigned int returned = 0;
    const auto report = Capture([&] { traceMiddle(expected, returned); });
    const auto frames = CheckReport(report, "[NestedHeap] heap backtrace (host frames):\n");
    Require(returned == 2 && expected[0] != 0 && expected[1] != 0 && expected[0] != expected[1]);
    std::size_t matched = 0;
    for (const auto address : frames) {
        if (matched < expected.size() && address == expected[matched]) ++matched;
    }
    Require(matched == expected.size());
}

void TraceDeep(unsigned int depth, unsigned int& returned);

void (*volatile traceDeep)(unsigned int, unsigned int&) = TraceDeep;

void TraceDeep(unsigned int depth, unsigned int& returned) {
    if (depth == 0) sceLibcInternalBacktraceForGame_nid_postfix("DeepHeap");
    else traceDeep(depth - 1, returned);
    ++returned;
}

void CheckFrameLimit() {
    unsigned int returned = 0;
    const auto report = Capture([&] { traceDeep(48, returned); });
    const auto frames = CheckReport(report, "[DeepHeap] heap backtrace (host frames):\n");
    Require(returned == 49 && frames.size() == 32);
}

void CheckConcurrentReports() {
    constexpr std::size_t threadCount = 4;
    constexpr std::size_t repetitions = 8;
    const auto report = Capture([] {
        std::array<std::thread, threadCount> workers;
        for (std::size_t thread = 0; thread < workers.size(); ++thread) {
            workers[thread] = std::thread([thread] {
                const auto name = "ThreadHeap" + std::to_string(thread);
                for (std::size_t iteration = 0; iteration < repetitions; ++iteration) {
                    *__error_nid_postfix() = static_cast<int>(100 + thread);
                    sceLibcInternalBacktraceForGame_nid_postfix(name.c_str());
                    Require(*__error_nid_postfix() == static_cast<int>(100 + thread));
                    sceLibcInternalHeapErrorReportForGame_nid_postfix(nullptr, nullptr, static_cast<std::uint32_t>(0x7000 + thread));
                    Require(*__error_nid_postfix() == static_cast<int>(100 + thread));
                }
            });
        }
        for (auto& worker : workers) worker.join();
    });
    std::array<std::size_t, threadCount * 2> counts{};
    std::size_t position = 0;
    while (position < report.size()) {
        const auto newline = report.find('\n', position);
        Require(newline != std::string::npos);
        const auto header = report.substr(position, newline - position + 1);
        std::size_t matched = counts.size();
        for (std::size_t thread = 0; thread < threadCount; ++thread) {
            if (header == "[ThreadHeap" + std::to_string(thread) + "] heap backtrace (host frames):\n") matched = thread;
            if (header == ErrorHeader(nullptr, nullptr, static_cast<std::uint32_t>(0x7000 + thread))) matched = threadCount + thread;
        }
        Require(matched < counts.size());
        ++counts[matched];
        const auto next = report.find('[', newline + 1);
        const auto end = next == std::string::npos ? report.size() : next;
        ParseFrames(report, newline + 1, end);
        position = end;
    }
    for (const auto count : counts) Require(count == repetitions);
}

}

int main() {
    CheckNames();
    CheckErrors();
    CheckNestedFrames();
    CheckFrameLimit();
    CheckConcurrentReports();
}
