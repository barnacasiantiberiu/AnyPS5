#include "SceTypes.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" {
int APS5_VABI kqueue_nid_postfix();
int APS5_VABI kevent_nid_postfix(int, const KernelEvent*, int, KernelEvent*, int, const KernelTimespec*);
int APS5_VABI close_nid_postfix(int);
int APS5_VABI dup_nid_postfix(int);
int APS5_VABI dup2_nid_postfix(int, int);
int APS5_VABI fcntl_nid_postfix(int, int, ...);
int APS5_VABI flock_nid_postfix(int, int);
int APS5_VABI futimes_nid_postfix(int, const KernelTimeval*);
int APS5_VABI ioctl_nid_postfix(int, std::uint64_t, void*);
std::int64_t APS5_VABI fstat_nid_disambig1_nid_postfix(int, FileStat*);
int APS5_VABI sceKernelFstat(int, FileStat*);
int APS5_VABI select_nid_postfix(int, void*, void*, void*, const void*);
int APS5_VABI poll_nid_postfix(void*, std::uint32_t, int);
std::int64_t APS5_VABI read_nid_postfix(int, void*, std::uint64_t);
std::int64_t APS5_VABI write_nid_postfix(int, const char*, std::int64_t);
int* APS5_VABI __error_nid_postfix();
}

static void Require(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "GuestKqueue failure at line %d, guest errno %d\n", line, *__error_nid_postfix());
        std::abort();
    }
}
#define REQUIRE(value) Require((value), __LINE__)

constexpr std::int16_t User = -11;
constexpr std::int16_t Timer = -7;
constexpr std::uint16_t Add = 1, Delete = 2, Enable = 4, Disable = 8, Oneshot = 16, Clear = 32, Receipt = 64, Dispatch = 128, Error = 0x4000;
constexpr std::uint32_t Trigger = 0x01000000, Copy = 0xc0000000, Or = 0x80000000, And = 0x40000000;
constexpr KernelTimespec Poll{0, 0};
constexpr KernelTimespec Wait{1, 0};

static KernelEvent Event(std::uintptr_t id, std::int16_t filter, std::uint16_t flags, std::uint32_t fflags = 0, std::intptr_t data = 0, void* udata = nullptr) {
    return {id, filter, flags, fflags, data, udata};
}

static void CreationAndAliases() {
    const int queue = kqueue_nid_postfix();
    REQUIRE(queue >= 0);
    REQUIRE(fcntl_nid_postfix(queue, 1) == 0);
    REQUIRE(fcntl_nid_postfix(queue, 2, 1) == 0);
    REQUIRE(fcntl_nid_postfix(queue, 1) == 1);
    const int alias = dup_nid_postfix(queue);
    REQUIRE(alias >= 0 && alias != queue);
    REQUIRE(fcntl_nid_postfix(alias, 1) == 0);
    REQUIRE(fcntl_nid_postfix(alias, 3) == 2);
    REQUIRE(fcntl_nid_postfix(alias, 4, 4) == -1 && *__error_nid_postfix() == 25);
    REQUIRE(fcntl_nid_postfix(queue, 3) == 6);
    REQUIRE(fcntl_nid_postfix(alias, 4, 0) == -1 && *__error_nid_postfix() == 25);
    REQUIRE(fcntl_nid_postfix(queue, 3) == 2);
    REQUIRE(fcntl_nid_postfix(alias, 5) == -1 && *__error_nid_postfix() == 25);
    REQUIRE(fcntl_nid_postfix(alias, 15, 1024) == -1 && *__error_nid_postfix() == 9);
    REQUIRE(flock_nid_postfix(alias, 8) == -1 && *__error_nid_postfix() == 45);
    REQUIRE(futimes_nid_postfix(alias, nullptr) == -1 && *__error_nid_postfix() == 22);
    const int replacement = kqueue_nid_postfix();
    REQUIRE(replacement >= 0);
    REQUIRE(dup2_nid_postfix(alias, replacement) == replacement);
    REQUIRE(close_nid_postfix(queue) == 0);
    KernelEvent change = Event(5, User, Add | Clear, Trigger, 71);
    REQUIRE(kevent_nid_postfix(alias, &change, 1, nullptr, 0, nullptr) == 0);
    KernelEvent output{};
    REQUIRE(kevent_nid_postfix(replacement, nullptr, 0, &output, 1, &Poll) == 1);
    REQUIRE(output.ident == 5 && output.data == 71);
    REQUIRE(kevent_nid_postfix(alias, nullptr, 0, &output, 1, &Poll) == 0);
    char byte = 0;
    REQUIRE(read_nid_postfix(alias, &byte, 1) == -1 && *__error_nid_postfix() == 6);
    REQUIRE(write_nid_postfix(alias, "x", 1) == -1 && *__error_nid_postfix() == 6);
    REQUIRE(close_nid_postfix(alias) == 0);
    REQUIRE(close_nid_postfix(replacement) == 0);
    REQUIRE(kevent_nid_postfix(replacement, nullptr, 0, &output, 1, &Poll) == -1 && *__error_nid_postfix() == 9);
}

static void ChangesAndReceipts() {
    const int queue = kqueue_nid_postfix();
    REQUIRE(queue >= 0);
    int token = 0;
    KernelEvent changes[3] = {
        Event(7, User, Add | Disable | Clear | Receipt, 5, 9, &token),
        Event(8, User, Delete | Receipt),
        Event(9, 123, Add | Receipt)
    };
    REQUIRE(kevent_nid_postfix(queue, changes, 3, changes, 3, &Wait) == 3);
    REQUIRE(changes[0].flags == Error && changes[0].data == 0 && changes[0].udata == &token);
    REQUIRE(changes[1].flags == Error && changes[1].data == 2);
    REQUIRE(changes[2].flags == Error && changes[2].data == 22);
    KernelEvent change = Event(7, User, 0, Trigger | Or | 2, 23, &token);
    REQUIRE(kevent_nid_postfix(queue, &change, 1, nullptr, 0, nullptr) == 0);
    KernelEvent output[2]{};
    output[1] = Event(0xfeed, User, 0, 0xa55a, 33, &token);
    const auto guard = output[1];
    REQUIRE(kevent_nid_postfix(queue, nullptr, 0, output, 1, &Poll) == 0);
    change = Event(7, User, Enable, And | 3, 24, &token);
    REQUIRE(kevent_nid_postfix(queue, &change, 1, output, 1, &Poll) == 1);
    REQUIRE(output[0].fflags == 3 && output[0].data == 24 && output[0].udata == &token);
    REQUIRE(std::memcmp(&output[1], &guard, sizeof(guard)) == 0);
    REQUIRE(kevent_nid_postfix(queue, nullptr, 0, output, 1, &Poll) == 0);
    change = Event(7, User, 0, Trigger | Copy | 0x4567, 80);
    REQUIRE(kevent_nid_postfix(queue, &change, 1, output, 1, &Poll) == 1);
    REQUIRE(output[0].fflags == 0x4567 && output[0].data == 80);
    change.flags = Delete;
    REQUIRE(kevent_nid_postfix(queue, &change, 1, nullptr, 0, nullptr) == 0);
    REQUIRE(kevent_nid_postfix(queue, &change, 1, nullptr, 0, nullptr) == -1 && *__error_nid_postfix() == 2);
    REQUIRE(close_nid_postfix(queue) == 0);
}

static void DeliveryModes() {
    const int queue = kqueue_nid_postfix();
    KernelEvent changes[3] = {Event(1, User, Add, Trigger), Event(2, User, Add | Oneshot, Trigger), Event(3, User, Add | Dispatch, Trigger)};
    REQUIRE(kevent_nid_postfix(queue, changes, 3, nullptr, 0, &Wait) == 0);
    KernelEvent events[4]{};
    REQUIRE(kevent_nid_postfix(queue, nullptr, 0, events, 4, &Poll) == 3);
    REQUIRE(kevent_nid_postfix(queue, nullptr, 0, events, 4, &Poll) == 1 && events[0].ident == 1);
    auto change = Event(2, User, Enable);
    REQUIRE(kevent_nid_postfix(queue, &change, 1, nullptr, 0, nullptr) == -1 && *__error_nid_postfix() == 2);
    change = Event(3, User, Enable);
    REQUIRE(kevent_nid_postfix(queue, &change, 1, events, 4, &Poll) == 2);
    REQUIRE(close_nid_postfix(queue) == 0);
}

static void ExistingUserFlags() {
    const int queue = kqueue_nid_postfix();
    REQUIRE(queue >= 0);
    KernelEvent changes[3] = {Event(1, User, Add | Clear), Event(2, User, Add | Oneshot), Event(3, User, Add | Dispatch)};
    REQUIRE(kevent_nid_postfix(queue, changes, 3, nullptr, 0, nullptr) == 0);
    changes[0] = Event(1, User, Add | Dispatch, Trigger, 10);
    changes[1] = Event(2, User, Add, Trigger, 20);
    changes[2] = Event(3, User, Add | Oneshot, Trigger, 30);
    KernelEvent output[3]{};
    REQUIRE(kevent_nid_postfix(queue, changes, 3, output, 3, &Poll) == 3);
    REQUIRE(output[0].ident == 1 && output[0].flags == Clear && output[0].data == 10);
    REQUIRE(output[1].ident == 2 && output[1].flags == Oneshot && output[1].data == 20);
    REQUIRE(output[2].ident == 3 && output[2].flags == Dispatch && output[2].data == 30);
    REQUIRE(kevent_nid_postfix(queue, nullptr, 0, output, 3, &Poll) == 0);
    auto change = Event(3, User, Enable);
    REQUIRE(kevent_nid_postfix(queue, &change, 1, output, 3, &Poll) == 1 && output[0].ident == 3);
    change = Event(1, User, Clear, Trigger | Copy | 7, 40);
    REQUIRE(kevent_nid_postfix(queue, &change, 1, output, 3, &Poll) == 0);
    change.flags = 0;
    REQUIRE(kevent_nid_postfix(queue, &change, 1, output, 3, &Poll) == 1);
    REQUIRE(output[0].fflags == 7 && output[0].data == 40);
    REQUIRE(close_nid_postfix(queue) == 0);
}

static void WaitAndTimers() {
    const int queue = kqueue_nid_postfix();
    KernelEvent change = Event(3, User, Add | Clear);
    REQUIRE(kevent_nid_postfix(queue, &change, 1, nullptr, 0, nullptr) == 0);
    std::thread trigger([queue] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto change = Event(3, User, 0, Trigger, 42);
        REQUIRE(kevent_nid_postfix(queue, &change, 1, nullptr, 0, nullptr) == 0);
    });
    KernelEvent event{};
    REQUIRE(kevent_nid_postfix(queue, nullptr, 0, &event, 1, &Wait) == 1);
    trigger.join();
    REQUIRE(event.ident == 3 && event.data == 42);
    change = Event(4, Timer, Add | Oneshot, 0, 5);
    REQUIRE(kevent_nid_postfix(queue, &change, 1, &event, 1, &Wait) == 1);
    REQUIRE(event.ident == 4 && event.filter == Timer && event.data == 1);
    REQUIRE(kevent_nid_postfix(queue, nullptr, 0, &event, 1, &Poll) == 0);
    const KernelTimespec shortWait{0, 1500000};
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(kevent_nid_postfix(queue, nullptr, 0, &event, 1, &shortWait) == 0);
    REQUIRE(std::chrono::steady_clock::now() - start >= std::chrono::nanoseconds(1500000));
    REQUIRE(close_nid_postfix(queue) == 0);
}

static void InvalidArguments() {
    const int queue = kqueue_nid_postfix();
    KernelEvent event{};
    REQUIRE(kevent_nid_postfix(queue, nullptr, -1, nullptr, 0, nullptr) == -1 && *__error_nid_postfix() == 22);
    REQUIRE(kevent_nid_postfix(queue, nullptr, 0, nullptr, -1, nullptr) == -1 && *__error_nid_postfix() == 22);
    REQUIRE(kevent_nid_postfix(queue, nullptr, 1, &event, 1, &Poll) == -1 && *__error_nid_postfix() == 14);
    REQUIRE(kevent_nid_postfix(queue, nullptr, 0, nullptr, 1, &Poll) == -1 && *__error_nid_postfix() == 14);
    for (const auto timeout : {KernelTimespec{-1, 0}, KernelTimespec{0, -1}, KernelTimespec{0, 1000000000}})
        REQUIRE(kevent_nid_postfix(queue, nullptr, 0, &event, 1, &timeout) == -1 && *__error_nid_postfix() == 22);
    auto change = Event(9, User, Add | Receipt);
    REQUIRE(kevent_nid_postfix(queue, &change, 1, nullptr, 0, nullptr) == 0);
    change = Event(9, User, Delete);
    REQUIRE(kevent_nid_postfix(queue, &change, 1, nullptr, 0, nullptr) == 0);
    change = Event(9, 0, Add | Receipt);
    REQUIRE(kevent_nid_postfix(queue, &change, 1, &event, 1, &Poll) == 0);
    change = Event(9, -32768, Add | Receipt);
    REQUIRE(kevent_nid_postfix(queue, &change, 1, &event, 1, &Poll) == 1 && event.flags == Error && event.data == 22);
    change = Event(9, -5, Add);
    bool rejected = false;
    try {
        kevent_nid_postfix(queue, &change, 1, nullptr, 0, nullptr);
    } catch (const std::exception& exception) {
        rejected = std::string(exception.what()).find("filter runtime integration") != std::string::npos;
    }
    REQUIRE(rejected);
    REQUIRE(close_nid_postfix(queue) == 0);
}

static void StatusAndReadiness() {
    const int queue = kqueue_nid_postfix();
    REQUIRE(queue >= 0 && queue < 1024);
    FileStat expected;
    std::memset(&expected, 0, sizeof(expected));
    expected.st_mode = 010000;
    FileStat status;
    std::memset(&status, 0x5a, sizeof(status));
    REQUIRE(fstat_nid_disambig1_nid_postfix(queue, &status) == 0);
    REQUIRE(std::memcmp(&status, &expected, sizeof(status)) == 0);
    std::memset(&status, 0x5a, sizeof(status));
    REQUIRE(sceKernelFstat(queue, &status) == 0);
    REQUIRE(std::memcmp(&status, &expected, sizeof(status)) == 0);
    int available = 99;
    REQUIRE(ioctl_nid_postfix(queue, 0x4004667f, &available) == -1 && *__error_nid_postfix() == 25);
    REQUIRE(available == 99);
    struct PollDescriptor { int descriptor; std::int16_t events; std::int16_t revents; } request{queue, 0x45, -1};
    REQUIRE(poll_nid_postfix(&request, 1, 0) == 0 && request.revents == 0);
    auto change = Event(1, User, Add | Oneshot, Trigger);
    REQUIRE(kevent_nid_postfix(queue, &change, 1, nullptr, 0, nullptr) == 0);
    REQUIRE(poll_nid_postfix(&request, 1, 0) == 1 && request.revents == 0x41);
    REQUIRE(poll_nid_postfix(&request, 1, 0) == 1 && request.revents == 0x41);
    std::array<std::uint64_t, 16> read{}, write{}, except{};
    const std::uint64_t bit = std::uint64_t{1} << (queue % 64);
    read[queue / 64] = write[queue / 64] = except[queue / 64] = bit;
    const KernelTimeval poll{0, 0};
    REQUIRE(select_nid_postfix(queue + 1, read.data(), write.data(), except.data(), &poll) == 1);
    REQUIRE(read[queue / 64] == bit && write[queue / 64] == 0 && except[queue / 64] == 0);
    KernelEvent event{};
    REQUIRE(kevent_nid_postfix(queue, nullptr, 0, &event, 1, &Poll) == 1);
    REQUIRE(event.ident == 1);
    REQUIRE(poll_nid_postfix(&request, 1, 0) == 0 && request.revents == 0);
    std::thread trigger([queue] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto change = Event(2, Timer, Add | Oneshot, 0, 5);
        REQUIRE(kevent_nid_postfix(queue, &change, 1, nullptr, 0, nullptr) == 0);
    });
    REQUIRE(poll_nid_postfix(&request, 1, 1000) == 1 && request.revents == 0x41);
    trigger.join();
    REQUIRE(kevent_nid_postfix(queue, nullptr, 0, &event, 1, &Poll) == 1 && event.ident == 2);
    REQUIRE(close_nid_postfix(queue) == 0);
}

int main() {
    CreationAndAliases();
    ChangesAndReceipts();
    DeliveryModes();
    ExistingUserFlags();
    WaitAndTimers();
    InvalidArguments();
    StatusAndReadiness();
}
