#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>

struct TestIovec { void* base; std::size_t length; };

extern "C" {
int APS5_VABI sceKernelOpen(const char*, int, std::uint16_t);
int APS5_VABI sceKernelClose(int);
std::int64_t APS5_VABI sceKernelRead(int, void*, std::size_t);
std::int64_t APS5_VABI sceKernelWrite(int, const void*, std::size_t);
std::int64_t APS5_VABI sceKernelPread(int, void*, std::size_t, std::int64_t);
std::int64_t APS5_VABI sceKernelPwrite(int, const void*, std::size_t, std::int64_t);
std::int64_t APS5_VABI sceKernelLseek(int, std::int64_t, int);
std::int64_t APS5_VABI preadv_nid_postfix(int, const TestIovec*, int, std::int64_t);
std::int64_t APS5_VABI pwritev_nid_postfix(int, const TestIovec*, int, std::int64_t);
std::int64_t APS5_VABI readv_nid_postfix(int, const TestIovec*, int);
std::int64_t APS5_VABI writev_nid_postfix(int, const TestIovec*, int);
int APS5_VABI dup_nid_postfix(int);
int APS5_VABI dup2_nid_postfix(int, int);
int APS5_VABI fcntl_nid_postfix(int, int, ...);
int APS5_VABI ioctl_nid_postfix(int, std::uint64_t, void*);
int APS5_VABI ftruncate_nid_postfix(int, std::int64_t);
int APS5_VABI pipe_nid_postfix(int*);
int APS5_VABI sceKernelCreateEqueue(KernelEqueue*, const char*);
int APS5_VABI sceKernelAddReadEvent(KernelEqueue, int, std::size_t, void*);
int APS5_VABI sceKernelWaitEqueue(KernelEqueue, KernelEvent*, int, int*, const KernelUseconds*);
int APS5_VABI sceKernelDeleteEqueue(KernelEqueue);
int APS5_VABI sceKernelAioSubmitReadCommands(KernelAioRwRequest*, std::int32_t, std::int32_t, std::int32_t*);
int APS5_VABI sceKernelAioSubmitWriteCommands(KernelAioRwRequest*, std::int32_t, std::int32_t, std::int32_t*);
int APS5_VABI sceKernelAioWaitRequest(std::int32_t, std::int32_t*, std::uint32_t*);
int APS5_VABI sceKernelAioDeleteRequest(std::int32_t, std::int32_t*);
}

static void Require(bool result, int line) {
    if (!result) {
        std::fprintf(stderr, "Positioned concurrency failure at line %d\n", line);
        std::abort();
    }
}
#define REQUIRE(value) Require((value), __LINE__)

static std::int64_t Aio(int descriptor, void* buffer, std::size_t count, bool write) {
    KernelAioResult result{-1, 0};
    KernelAioRwRequest request{0, count, buffer, &result, descriptor};
    std::int32_t id = 0, state = 0, removed = -1;
    REQUIRE((write ? sceKernelAioSubmitWriteCommands(&request, 1, 0, &id) :
        sceKernelAioSubmitReadCommands(&request, 1, 0, &id)) == 0);
    std::uint32_t timeout = 5000000;
    REQUIRE(sceKernelAioWaitRequest(id, &state, &timeout) == 0);
    REQUIRE(state == 3 && result.state == 3);
    REQUIRE(sceKernelAioDeleteRequest(id, &removed) == 0);
    return result.return_value;
}

int main(int argc, char** argv) {
    REQUIRE(argc == 3);
    const std::string mode = argv[1], duplication = argv[2];
    REQUIRE(mode == "read" || mode == "write" || mode == "vector" || mode == "aio" ||
        mode == "observe" || mode == "truncate" || mode == "isolation");
    const auto root = std::filesystem::temp_directory_path() /
        ("anyps5-positioned-" + mode + "-" + duplication + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    constexpr std::size_t BlockSize = 4096, TransferSize = 64;
    const auto path = root / "data.bin";
    {
        std::ofstream file(path, std::ios::binary);
        file << std::string(BlockSize, 'A') << std::string(BlockSize, 'Z');
        REQUIRE(static_cast<bool>(file));
    }
    AddPathAlias_nid_no_patch("/positioned-test", root.string().c_str());
    const int original = sceKernelOpen("/positioned-test/data.bin", SCE_KERNEL_O_RDWR, 0);
    REQUIRE(original >= 0);
    int duplicate = original;
    if (duplication == "dup") duplicate = dup_nid_postfix(original);
    else if (duplication == "dup2") {
        duplicate = sceKernelOpen("/positioned-test/data.bin", SCE_KERNEL_O_RDWR, 0);
        REQUIRE(duplicate >= 0 && duplicate != original);
        REQUIRE(dup2_nid_postfix(original, duplicate) == duplicate);
    } else if (duplication == "fcntl") duplicate = fcntl_nid_postfix(original, 0, original + 1);
    else REQUIRE(duplication == "same");
    REQUIRE(duplicate >= 0);

    if (mode == "isolation") {
        int pipe[2]{};
        REQUIRE(pipe_nid_postfix(pipe) == 0);
        std::promise<void> entered;
        std::thread reader([&] {
            entered.set_value();
            char data = '?';
            REQUIRE(sceKernelRead(pipe[0], &data, 1) == 1 && data == 'P');
        });
        entered.get_future().wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        char data = '?';
        REQUIRE(sceKernelPread(original, &data, 1, 0) == 1 && data == 'A');
        REQUIRE(sceKernelWrite(pipe[1], "P", 1) == 1);
        reader.join();
        REQUIRE(sceKernelClose(pipe[0]) == 0 && sceKernelClose(pipe[1]) == 0);
    }
    KernelEqueue queue = 0;
    if (mode == "observe") {
        REQUIRE(sceKernelCreateEqueue(&queue, "positioned-observer") == 0);
        REQUIRE(sceKernelAddReadEvent(queue, duplicate, 0, nullptr) == 0);
    }

    std::barrier start(2);
    std::atomic<bool> stop{false}, failure{false};
    std::atomic<unsigned> sequentialCount{0};
    std::thread sequential([&] {
        std::array<char, TransferSize> data{};
        std::array<char, TransferSize> expected{};
        expected.fill('Z');
        TestIovec vectors[]{{data.data(), 17}, {data.data() + 17, TransferSize - 17}};
        start.arrive_and_wait();
        while (!stop.load()) {
            if (sceKernelLseek(duplicate, BlockSize, 0) != BlockSize) { failure = true; break; }
            if (mode == "observe") {
                int available = -1, count = -1;
                KernelEvent event{};
                constexpr KernelUseconds poll = 0;
                if (ioctl_nid_postfix(duplicate, 0x4004667f, &available) != 0 || available != BlockSize ||
                    sceKernelWaitEqueue(queue, &event, 1, &count, &poll) != 0 || count != 1 || event.data != BlockSize) {
                    failure = true;
                    break;
                }
                ++sequentialCount;
                continue;
            }
            if (mode == "truncate" && ftruncate_nid_postfix(duplicate, BlockSize * 2) != 0) { failure = true; break; }
            const auto read = mode == "vector" ? readv_nid_postfix(duplicate, vectors, 2) :
                sceKernelRead(duplicate, data.data(), data.size());
            if (read != TransferSize || data != expected) {
                failure = true;
                break;
            }
            if (sceKernelLseek(duplicate, BlockSize, 0) != BlockSize) { failure = true; break; }
            const auto written = mode == "vector" ? writev_nid_postfix(duplicate, vectors, 2) :
                sceKernelWrite(duplicate, data.data(), data.size());
            if (written != TransferSize) { failure = true; break; }
            ++sequentialCount;
        }
    });
    start.arrive_and_wait();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sequentialCount.load() == 0 && !failure.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    for (int index = 0; index < 10000 && !failure.load(); ++index) {
        std::array<char, TransferSize> data{};
        data.fill('A');
        const auto expected = data;
        const bool write = mode == "write" || ((mode == "vector" || mode == "aio") && index % 2 != 0);
        if (!write) data.fill('?');
        TestIovec vectors[]{{data.data(), 13}, {data.data() + 13, TransferSize - 13}};
        const auto result = mode == "aio" ? Aio(original, data.data(), data.size(), write) :
            mode == "vector" ? (write ? pwritev_nid_postfix(original, vectors, 2, 0) : preadv_nid_postfix(original, vectors, 2, 0)) :
            write ? sceKernelPwrite(original, data.data(), data.size(), 0) : sceKernelPread(original, data.data(), data.size(), 0);
        if (result != TransferSize || data != expected) failure = true;
    }
    stop = true;
    sequential.join();
    REQUIRE(!failure.load() && sequentialCount.load() > 0);
    if (queue != 0) REQUIRE(sceKernelDeleteEqueue(queue) == 0);
    std::array<char, BlockSize * 2> contents{};
    REQUIRE(sceKernelPread(original, contents.data(), contents.size(), 0) == contents.size());
    for (std::size_t index = 0; index < contents.size(); ++index)
        REQUIRE(contents[index] == (index < BlockSize ? 'A' : 'Z'));
    REQUIRE(sceKernelLseek(duplicate, 123, 0) == 123);
    char byte = '?';
    REQUIRE(sceKernelPread(original, &byte, 1, 0) == 1 && byte == 'A');
    REQUIRE(sceKernelLseek(duplicate, 0, 1) == 123);
    REQUIRE(sceKernelPread(original, &byte, 1, contents.size()) == 0);
    REQUIRE(sceKernelLseek(duplicate, 0, 1) == 123);
    if (duplicate != original) REQUIRE(sceKernelClose(duplicate) == 0);
    REQUIRE(sceKernelClose(original) == 0);
    RemovePathAlias_nid_no_patch("/positioned-test");
    REQUIRE(std::filesystem::remove(path) && std::filesystem::remove(root));
}
