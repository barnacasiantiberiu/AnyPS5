#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
int APS5_VABI sceKernelCreateEqueue(KernelEqueue*, const char*);
int APS5_VABI sceKernelDeleteEqueue(KernelEqueue);
int APS5_VABI sceKernelWaitEqueue(KernelEqueue, KernelEvent*, int, int*, const KernelUseconds*);
int APS5_VABI sceKernelAddReadEvent(KernelEqueue, int, std::size_t, void*);
int APS5_VABI sceKernelAddWriteEvent(KernelEqueue, int, std::size_t, void*);
int APS5_VABI sceKernelDeleteReadEvent(KernelEqueue, int);
int APS5_VABI sceKernelDeleteWriteEvent(KernelEqueue, int);
int APS5_VABI sceKernelOpen(const char*, int, std::uint16_t);
std::int64_t APS5_VABI sceKernelLseek(int, std::int64_t, int);
int APS5_VABI close_nid_postfix(int);
int APS5_VABI dup2_nid_postfix(int, int);
int APS5_VABI pipe_nid_postfix(int*);
int APS5_VABI socketpair_nid_postfix(int, int, int, int*);
int APS5_VABI socket_nid_postfix(int, int, int);
int APS5_VABI bind_nid_postfix(int, const void*, std::uint32_t);
int APS5_VABI listen_nid_postfix(int, int);
int APS5_VABI shutdown_nid_postfix(int, int);
int APS5_VABI kqueue_nid_postfix();
int APS5_VABI kevent_nid_postfix(int, const KernelEvent*, int, KernelEvent*, int, const KernelTimespec*);
std::int64_t APS5_VABI read_nid_postfix(int, void*, std::uint64_t);
std::int64_t APS5_VABI write_nid_postfix(int, const char*, std::int64_t);
}

static void Require(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "GuestEqueueDescriptors failure at line %d\n", line);
        std::abort();
    }
}
#define REQUIRE(value) Require((value), __LINE__)

constexpr int Timeout = static_cast<int>(0x8002003c);
constexpr int BadDescriptor = static_cast<int>(0x80020009);
constexpr int Invalid = static_cast<int>(0x80020016);
constexpr KernelUseconds Poll = 0, Wait = 1000000;

static KernelEvent Ready(KernelEqueue queue, int descriptor, std::int16_t filter) {
    KernelEvent event{};
    int count = -1;
    REQUIRE(sceKernelWaitEqueue(queue, &event, 1, &count, &Wait) == 0);
    REQUIRE(count == 1 && event.ident == static_cast<std::uintptr_t>(descriptor) && event.filter == filter);
    return event;
}

static void Empty(KernelEqueue queue) {
    KernelEvent event{};
    int count = -1;
    REQUIRE(sceKernelWaitEqueue(queue, &event, 1, &count, &Poll) == Timeout);
    REQUIRE(count == 0);
}

static void FilePositionAndGrowth() {
    const auto root = std::filesystem::temp_directory_path() /
        ("anyps5-equeue-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto file = root / "watched.bin";
    {
        std::ofstream stream(file, std::ios::binary);
        stream << "abcde";
        REQUIRE(static_cast<bool>(stream));
    }
    AddPathAlias_nid_no_patch("/anyps5-equeue-test", root.string().c_str());
    const int descriptor = sceKernelOpen("/anyps5-equeue-test/watched.bin", SCE_KERNEL_O_RDONLY, 0);
    REQUIRE(descriptor >= 0);
    KernelEqueue queue = 0;
    REQUIRE(sceKernelCreateEqueue(&queue, "file") == 0);
    int payload = 0;
    REQUIRE(sceKernelAddReadEvent(queue, descriptor, 100, &payload) == 0);
    auto event = Ready(queue, descriptor, -1);
    REQUIRE(event.data == 5 && event.udata == &payload && (event.flags & 0x8000) == 0);
    REQUIRE(sceKernelLseek(descriptor, 2, 0) == 2);
    REQUIRE(Ready(queue, descriptor, -1).data == 3);
    char data[3]{};
    REQUIRE(read_nid_postfix(descriptor, data, sizeof(data)) == 3);
    Empty(queue);
    REQUIRE(sceKernelLseek(descriptor, 8, 0) == 8);
    REQUIRE(Ready(queue, descriptor, -1).data == -3);
    REQUIRE(sceKernelLseek(descriptor, 5, 0) == 5);
    std::thread grow([file] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::ofstream stream(file, std::ios::binary | std::ios::app);
        stream << "fg";
        REQUIRE(static_cast<bool>(stream));
    });
    REQUIRE(Ready(queue, descriptor, -1).data == 2);
    grow.join();
    REQUIRE(sceKernelAddWriteEvent(queue, descriptor, 0, nullptr) == Invalid);
    REQUIRE(close_nid_postfix(descriptor) == 0);
    Empty(queue);
    REQUIRE(sceKernelAddReadEvent(queue, descriptor, 0, nullptr) == BadDescriptor);
    REQUIRE(sceKernelDeleteEqueue(queue) == 0);
    RemovePathAlias_nid_no_patch("/anyps5-equeue-test");
    REQUIRE(std::filesystem::remove(file));
    REQUIRE(std::filesystem::remove(root));
}

static void PipeDataAndClose() {
    int descriptors[2];
    REQUIRE(pipe_nid_postfix(descriptors) == 0);
    KernelEqueue queue = 0;
    REQUIRE(sceKernelCreateEqueue(&queue, "pipe-read") == 0);
    REQUIRE(sceKernelAddReadEvent(queue, descriptors[0], 20, nullptr) == 0);
    Empty(queue);
    REQUIRE(write_nid_postfix(descriptors[1], "abc", 3) == 3);
    REQUIRE(Ready(queue, descriptors[0], -1).data == 3);
    char bytes[3]{};
    REQUIRE(read_nid_postfix(descriptors[0], bytes, 1) == 1);
    REQUIRE(Ready(queue, descriptors[0], -1).data == 2);
    REQUIRE(close_nid_postfix(descriptors[1]) == 0);
    const auto end = Ready(queue, descriptors[0], -1);
    REQUIRE(end.data == 2 && (end.flags & 0x8000) != 0);
    REQUIRE(read_nid_postfix(descriptors[0], bytes, 2) == 2);
    REQUIRE(Ready(queue, descriptors[0], -1).data == 0);
    REQUIRE(close_nid_postfix(descriptors[0]) == 0);
    Empty(queue);
    REQUIRE(sceKernelDeleteEqueue(queue) == 0);
}

static void PipeWriteQuota() {
    int descriptors[2];
    REQUIRE(pipe_nid_postfix(descriptors) == 0);
    KernelEqueue queue = 0;
    REQUIRE(sceKernelCreateEqueue(&queue, "pipe-write") == 0);
    REQUIRE(sceKernelAddWriteEvent(queue, descriptors[1], 0, nullptr) == 0);
    const auto capacity = Ready(queue, descriptors[1], -2).data;
    REQUIRE(capacity >= 512 && capacity <= 1024 * 1024);
    std::vector<char> bytes(static_cast<std::size_t>(capacity), 'x');
    REQUIRE(write_nid_postfix(descriptors[1], bytes.data(), capacity) == capacity);
    Empty(queue);
    const auto released = std::min<std::intptr_t>(capacity, 4096);
    REQUIRE(read_nid_postfix(descriptors[0], bytes.data(), static_cast<std::size_t>(released)) == released);
    REQUIRE(Ready(queue, descriptors[1], -2).data == released);
    REQUIRE(close_nid_postfix(descriptors[0]) == 0);
    const auto end = Ready(queue, descriptors[1], -2);
    REQUIRE(end.data == 0 && (end.flags & 0x8000) != 0);
    REQUIRE(close_nid_postfix(descriptors[1]) == 0);
    Empty(queue);
    REQUIRE(sceKernelDeleteEqueue(queue) == 0);
}

static void SocketReadWatermark() {
    int descriptors[2];
    REQUIRE(socketpair_nid_postfix(1, 1, 0, descriptors) == 0);
    KernelEqueue queue = 0;
    REQUIRE(sceKernelCreateEqueue(&queue, "socket-low-watermark") == 0);
    int first = 0, second = 0;
    REQUIRE(sceKernelAddReadEvent(queue, descriptors[0], 3, &first) == 0);
    REQUIRE(write_nid_postfix(descriptors[1], "ab", 2) == 2);
    Empty(queue);
    REQUIRE(write_nid_postfix(descriptors[1], "c", 1) == 1);
    auto event = Ready(queue, descriptors[0], -1);
    REQUIRE(event.data == 3 && event.udata == &first);
    REQUIRE(sceKernelAddReadEvent(queue, descriptors[0], 4, &second) == 0);
    Empty(queue);
    REQUIRE(write_nid_postfix(descriptors[1], "d", 1) == 1);
    event = Ready(queue, descriptors[0], -1);
    REQUIRE(event.data == 4 && event.udata == &second);
    char data[4]{};
    REQUIRE(read_nid_postfix(descriptors[0], data, sizeof(data)) == 4);
    Empty(queue);
    REQUIRE(write_nid_postfix(descriptors[1], "ef", 2) == 2);
    REQUIRE(shutdown_nid_postfix(descriptors[1], 1) == 0);
    event = Ready(queue, descriptors[0], -1);
    REQUIRE(event.data == 2 && (event.flags & 0x8000) != 0);
    REQUIRE(read_nid_postfix(descriptors[0], data, sizeof(data)) == 2);
    event = Ready(queue, descriptors[0], -1);
    REQUIRE(event.data == 0 && (event.flags & 0x8000) != 0);
    REQUIRE(close_nid_postfix(descriptors[0]) == 0);
    REQUIRE(close_nid_postfix(descriptors[1]) == 0);
    REQUIRE(sceKernelDeleteEqueue(queue) == 0);
}

static void UnsupportedSocketWatermarks() {
    const int datagram = socket_nid_postfix(2, 2, 0);
    const int listener = socket_nid_postfix(2, 1, 0);
    REQUIRE(datagram >= 0 && listener >= 0);
    const std::array<std::uint8_t, 16> address{16, 2, 0, 0, 127, 0, 0, 1};
    REQUIRE(bind_nid_postfix(listener, address.data(), address.size()) == 0);
    REQUIRE(listen_nid_postfix(listener, 4) == 0);
    KernelEqueue queue = 0;
    REQUIRE(sceKernelCreateEqueue(&queue, "unsupported-watermark") == 0);
    const int descriptorQueue = kqueue_nid_postfix();
    REQUIRE(descriptorQueue >= 0);
    for (const int descriptor : {datagram, listener}) {
        bool rejected = false;
        try {
            sceKernelAddReadEvent(queue, descriptor, 2, nullptr);
        } catch (const std::exception& exception) {
            rejected = std::string(exception.what()).find("low watermark") != std::string::npos;
        }
        REQUIRE(rejected);
        const KernelEvent change{static_cast<std::uintptr_t>(descriptor), -1, 1, 1, 2, nullptr};
        rejected = false;
        try {
            kevent_nid_postfix(descriptorQueue, &change, 1, nullptr, 0, nullptr);
        } catch (const std::exception& exception) {
            rejected = std::string(exception.what()).find("low watermark") != std::string::npos;
        }
        REQUIRE(rejected);
        REQUIRE(close_nid_postfix(descriptor) == 0);
    }
    REQUIRE(close_nid_postfix(descriptorQueue) == 0);
    REQUIRE(sceKernelDeleteEqueue(queue) == 0);
}

static void SocketReplacementRemovesWatch() {
    int original[2], replacement[2];
    REQUIRE(socketpair_nid_postfix(1, 1, 0, original) == 0);
    REQUIRE(socketpair_nid_postfix(1, 1, 0, replacement) == 0);
    KernelEqueue queue = 0;
    REQUIRE(sceKernelCreateEqueue(&queue, "socket-replacement") == 0);
    REQUIRE(sceKernelAddReadEvent(queue, original[0], 1, nullptr) == 0);
    REQUIRE(dup2_nid_postfix(original[0], original[0]) == original[0]);
    REQUIRE(dup2_nid_postfix(-1, original[0]) == -1);
    REQUIRE(write_nid_postfix(original[1], "a", 1) == 1);
    REQUIRE(Ready(queue, original[0], -1).data == 1);
    char byte = 0;
    REQUIRE(read_nid_postfix(original[0], &byte, 1) == 1 && byte == 'a');
    REQUIRE(dup2_nid_postfix(replacement[0], original[0]) == original[0]);
    REQUIRE(write_nid_postfix(replacement[1], "b", 1) == 1);
    Empty(queue);
    REQUIRE(sceKernelDeleteReadEvent(queue, original[0]) == static_cast<int>(0x80020002));
    REQUIRE(sceKernelAddReadEvent(queue, original[0], 1, nullptr) == 0);
    REQUIRE(Ready(queue, original[0], -1).data == 1);
    REQUIRE(read_nid_postfix(original[0], &byte, 1) == 1 && byte == 'b');
    for (const int descriptor : {original[0], original[1], replacement[0], replacement[1]}) REQUIRE(close_nid_postfix(descriptor) == 0);
    REQUIRE(sceKernelDeleteEqueue(queue) == 0);
}

int main() {
    FilePositionAndGrowth();
    PipeDataAndClose();
    PipeWriteQuota();
    SocketReadWatermark();
    UnsupportedSocketWatermarks();
    SocketReplacementRemovesWatch();
}
