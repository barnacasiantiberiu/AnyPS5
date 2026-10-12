#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "prx/libkernel/System/include/Coredump.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestProcess.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#elif !defined(__APPLE__)
#include <sys/uio.h>
#include <unistd.h>
#endif

extern "C" {
int APS5_VABI scePthreadAttrInit(PthreadAttr*);
int APS5_VABI scePthreadAttrDestroy(PthreadAttr*);
int APS5_VABI scePthreadAttrSetstacksize(PthreadAttr*, std::size_t);
int APS5_VABI scePthreadCreate(Pthread*, const PthreadAttr*, PthreadEntry, void*, const char*);
int APS5_VABI scePthreadJoin(Pthread, void**);
}

namespace {

constexpr int InvalidParameter = static_cast<int>(0x81180000u);
constexpr int NotRegistered = static_cast<int>(0x81180001u);
constexpr int AlreadyRegistered = static_cast<int>(0x81180002u);
constexpr int NotInHandler = static_cast<int>(0x81180003u);

struct Attachment {
    std::string kind;
    std::uint32_t tag;
    std::string name;
    std::vector<char> bytes;
};

struct Report {
    GuestCoredumpStopInfoCpu stop;
    GuestCoredumpThreadContextInfo context;
    std::filesystem::path directory;
    std::vector<char> data;
    std::vector<char> text;
    std::vector<Attachment> attachments;
    int handlerResult = 0;
};

struct ReportTag;

Report*& CurrentReport() {
    return HostThreadLocal<Report*, ReportTag>();
}

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    GuestCoredumpHandler handler = nullptr;
    void* common = nullptr;
    Pthread worker = nullptr;
    Report* pending = nullptr;
    bool stopping = false;
    bool running = false;
    bool completed = false;
    bool prepared = false;
    std::exception_ptr failure;
};

bool PrepareFork(void* context) {
    auto& state = *static_cast<State*>(context);
    state.prepared = state.mutex.try_lock();
    if (!state.prepared || state.handler) {
        errno = 35;
        return false;
    }
    return true;
}

bool ReleaseFork(void* context) noexcept {
    auto& state = *static_cast<State*>(context);
    if (state.prepared) {
        state.prepared = false;
        state.mutex.unlock();
    }
    return true;
}

bool ChildFork(void* context) noexcept {
    auto& state = *static_cast<State*>(context);
    if (state.prepared) new (&state.changed) std::condition_variable;
    return ReleaseFork(context);
}

State& CoredumpState() {
    static auto* value = [] {
        auto* state = new State;
        const GuestForkHooks hooks{325, PrepareFork,
            +[](void* context, bool) noexcept { return ReleaseFork(context); }, ChildFork, state};
        if (GuestRegisterForkHooks_nid_no_patch(&hooks) != 0) {
            delete state;
            throw std::runtime_error("coredump: registering fork owner failed");
        }
        return state;
    }();
    return *value;
}

bool ReadMemory(void* output, const void* input, std::size_t size) {
    if (size == 0) return true;
    const auto address = reinterpret_cast<std::uintptr_t>(input);
    if (!input || size > std::numeric_limits<std::uintptr_t>::max() - address) return false;
#ifdef _WIN32
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), input, output, size, &read) && read == size;
#elif defined(__APPLE__)
    (void)output;
    throw std::runtime_error("coredump: safe memory snapshots on macOS are unavailable");
#else
    iovec local{output, size};
    iovec remote{const_cast<void*>(input), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
#endif
}

bool ReadString(const char* input, std::string& output) {
    if (!input) return false;
    for (std::size_t i = 0; i < 4096; ++i) {
        char value;
        if (!ReadMemory(&value, input + i, 1)) return false;
        if (!value) return true;
        output.push_back(value);
    }
    return false;
}

void WriteBytes(const std::filesystem::path& path, const void* bytes, std::size_t size) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (size != 0) stream.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    stream.close();
    if (!stream) throw std::runtime_error("coredump: writing report failed");
}

void Persist(const Report& report) {
    std::filesystem::create_directories(report.directory);
    WriteBytes(report.directory / "cpu.bin", &report.stop, sizeof(report.stop));
    WriteBytes(report.directory / "registers.bin", &report.context, sizeof(report.context));
    WriteBytes(report.directory / "userdata.bin", report.data.data(), report.data.size());
    WriteBytes(report.directory / "debug.txt", report.text.data(), report.text.size());
    std::ofstream manifest(report.directory / "attachments.txt");
    manifest << "AnyPS5 CPU report 1\nhandler-result " << report.handlerResult << '\n';
    for (std::size_t i = 0; i < report.attachments.size(); ++i) {
        const auto& attachment = report.attachments[i];
        const auto filename = "attachment-" + std::to_string(i) + ".bin";
        WriteBytes(report.directory / filename, attachment.bytes.data(), attachment.bytes.size());
        manifest << filename << ' ' << attachment.kind << ' ' << attachment.tag << ' '
                 << attachment.bytes.size() << ' ' << std::quoted(attachment.name) << '\n';
    }
    manifest.close();
    if (!manifest) throw std::runtime_error("coredump: writing attachment manifest failed");
}

void* APS5_VABI RunHandler_nid_no_patch(void* context) {
    auto& state = *static_cast<State*>(context);
    std::unique_lock lock(state.mutex);
    for (;;) {
        state.changed.wait(lock, [&] { return state.stopping || state.pending; });
        if (state.stopping) return nullptr;
        auto* report = state.pending;
        const auto handler = state.handler;
        auto* common = state.common;
        lock.unlock();
        std::exception_ptr failure;
        try {
            CurrentReport() = report;
            report->handlerResult = handler(common);
            CurrentReport() = nullptr;
            Persist(*report);
        } catch (...) {
            CurrentReport() = nullptr;
            failure = std::current_exception();
        }
        lock.lock();
        state.failure = failure;
        state.pending = nullptr;
        state.completed = true;
        state.changed.notify_all();
    }
}

int AttachMemory(std::uint32_t tag, const void* memory, std::size_t size, const char* name, const char* kind) {
    auto* report = CurrentReport();
    if (!report) return NotInHandler;
    if (size > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) return InvalidParameter;
    Attachment attachment{kind, tag, {}, {}};
    if (name && !ReadString(name, attachment.name)) return InvalidParameter;
    attachment.bytes.resize(size);
    if (!ReadMemory(attachment.bytes.data(), memory, size)) return InvalidParameter;
    report->attachments.push_back(std::move(attachment));
    return 0;
}

}

extern "C" int GuestCoredumpCapture_nid_no_patch(const GuestCoredumpStopInfoCpu* stop,
    const GuestCoredumpThreadContextInfo* context, const char* hostDirectory) {
    if (!stop || !context) return InvalidParameter;
    if (CurrentReport()) throw std::runtime_error("coredump: recursive capture");
    auto& state = CoredumpState();
    std::unique_lock lock(state.mutex);
    if (!state.handler || state.stopping) return NotRegistered;
    if (state.running) throw std::runtime_error("coredump: concurrent capture");
    static std::atomic<std::uint64_t> sequence{0};
    const auto ticks = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::path(hostDirectory ? hostDirectory : "coredumps");
    Report report{*stop, *context, root / ("cpu-" + std::to_string(ticks) + "-" + std::to_string(sequence.fetch_add(1))), {}, {}, {}};
    state.failure = nullptr;
    state.completed = false;
    state.running = true;
    state.pending = &report;
    state.changed.notify_all();
    state.changed.wait(lock, [&] { return state.completed; });
    state.running = false;
    const auto failure = std::exchange(state.failure, nullptr);
    if (failure) std::rethrow_exception(failure);
    return 0;
}

extern "C" {

int APS5_VABI sceCoredumpRegisterCoredumpHandler(GuestCoredumpHandler handler, std::size_t stackSize, void* common) {
    auto& state = CoredumpState();
    std::lock_guard lock(state.mutex);
    if (state.handler) return AlreadyRegistered;
    if (!handler || stackSize < 16384 || stackSize >= 0x20000000) return InvalidParameter;
    PthreadAttr attr = nullptr;
    if (scePthreadAttrInit(&attr) != 0) throw std::runtime_error("coredump: allocating thread attributes failed");
    try {
        if (scePthreadAttrSetstacksize(&attr, stackSize) != 0)
            throw std::runtime_error("coredump: configuring thread stack failed");
        state.handler = handler;
        state.common = common;
        state.stopping = false;
        if (scePthreadCreate(&state.worker, &attr, RunHandler_nid_no_patch, &state, "SceCoredump") != 0)
            throw std::runtime_error("coredump: creating handler thread failed");
    } catch (...) {
        state.handler = nullptr;
        state.common = nullptr;
        scePthreadAttrDestroy(&attr);
        throw;
    }
    scePthreadAttrDestroy(&attr);
    return 0;
}

int APS5_VABI sceCoredumpUnregisterCoredumpHandler() {
    auto& state = CoredumpState();
    std::unique_lock lock(state.mutex);
    if (!state.handler || state.stopping) return NotRegistered;
    if (state.running) throw std::runtime_error("coredump: unregistering during capture");
    state.stopping = true;
    const auto worker = state.worker;
    state.changed.notify_all();
    lock.unlock();
    if (scePthreadJoin(worker, nullptr) != 0) throw std::runtime_error("coredump: joining handler thread failed");
    lock.lock();
    state.worker = nullptr;
    state.handler = nullptr;
    state.common = nullptr;
    state.stopping = false;
    return 0;
}

int APS5_VABI sceKernelDebugWriteCppExceptionInfo(const void* exception, std::uint64_t unknown, const char* typeName, const char* what) {
    (void)unknown;
    std::fprintf(stderr, "[coredump] uncaught C++ exception %p of type %s%s%s\n", exception,
        typeName ? typeName : "(unknown)", what ? ", what(): " : "", what ? what : "");
    return 0;
}

int APS5_VABI sceCoredumpAttachUserFile(std::uint32_t tag, const char* path) {
    auto* report = CurrentReport();
    if (!report) return NotInHandler;
    std::string guestPath;
    if (!ReadString(path, guestPath) || guestPath.empty()) return InvalidParameter;
    std::ifstream stream(ResolvePath_nid_no_patch(guestPath.c_str()), std::ios::binary);
    if (!stream) throw std::runtime_error("coredump: opening guest attachment failed");
    Attachment attachment{"file", tag, guestPath, {}};
    for (char bytes[4096]; stream;) {
        stream.read(bytes, sizeof(bytes));
        attachment.bytes.insert(attachment.bytes.end(), bytes, bytes + stream.gcount());
    }
    if (!stream.eof()) throw std::runtime_error("coredump: reading guest attachment failed");
    report->attachments.push_back(std::move(attachment));
    return 0;
}

int APS5_VABI sceCoredumpGetStopInfoGpu_Agc() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceCoredumpAttachMemoryRegionAsUserFile(std::uint32_t tag, const void* memory, std::size_t size, const char* name) {
    if (!CurrentReport()) return NotInHandler;
    if (!name) return InvalidParameter;
    return AttachMemory(tag, memory, size, name, "named-memory");
}

int APS5_VABI sceCoredumpSetUserDataType() {
    NotImplemented_nid_no_patch("Uxqkdta7wEg");
    return 0;
}

std::intptr_t APS5_VABI sceCoredumpWriteUserData(const void* data, std::size_t size) {
    auto* report = CurrentReport();
    if (!report) return NotInHandler;
    if (size > static_cast<std::size_t>(std::numeric_limits<std::intptr_t>::max())) return InvalidParameter;
    std::vector<char> bytes(size);
    if (!ReadMemory(bytes.data(), data, size)) return InvalidParameter;
    report->data.insert(report->data.end(), bytes.begin(), bytes.end());
    return static_cast<std::intptr_t>(size);
}

void APS5_VABI sceCoredumpDebugTextOut(const char* text, int length) {
    auto* report = CurrentReport();
    if (!report) throw std::runtime_error("coredump: text outside handler");
    if (length < 0) throw std::runtime_error("coredump: negative text length");
    std::vector<char> bytes(static_cast<std::size_t>(length));
    if (!ReadMemory(bytes.data(), text, bytes.size())) throw std::runtime_error("coredump: unreadable text");
    report->text.insert(report->text.end(), bytes.begin(), bytes.end());
}

int APS5_VABI sceCoredumpGetStopInfoCpu(GuestCoredumpStopInfoCpu* info, std::size_t size) {
    const auto* report = CurrentReport();
    if (!report) return NotInHandler;
    if (!info || size != sizeof(*info)) return InvalidParameter;
    std::memcpy(info, &report->stop, sizeof(*info));
    return 0;
}

int APS5_VABI sceCoredumpGetThreadContextInfo(GuestCoredumpThreadContextInfo* info, std::size_t size) {
    const auto* report = CurrentReport();
    if (!report) return NotInHandler;
    if (!info || size != sizeof(*info)) return InvalidParameter;
    std::memcpy(info, &report->context, sizeof(*info));
    return 0;
}

int APS5_VABI sceCoredumpWriteUserString() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceCoredumpAttachUserMemoryFile(std::uint32_t tag, const void* memory, std::size_t size) {
    return AttachMemory(tag, memory, size, nullptr, "memory-file");
}

int APS5_VABI sceCoredumpAttachMemoryRegion(std::uint32_t tag, const void* memory, std::size_t size) {
    return AttachMemory(tag, memory, size, nullptr, "memory-region");
}

}
