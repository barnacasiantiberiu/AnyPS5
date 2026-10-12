#include "ModuleChecks.hpp"
#include "SceTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <thread>

extern "C" {
int APS5_VABI _umtx_op_nid_postfix(void*, int, std::uint64_t, void*, void*);
int* APS5_VABI __error_nid_postfix();
int APS5_VABI clock_gettime_nid_postfix(int, KernelTimespec*);
int APS5_VABI fork_nid_postfix();
int APS5_VABI waitpid_nid_postfix(int, int*, int);
[[noreturn]] void APS5_VABI _exit_nid_postfix(int);
}

namespace {
using ModuleTest::Require;
using namespace std::chrono_literals;
constexpr int Wait = 23;
constexpr int Wake = 24;
constexpr std::uint32_t HasWaiters = 0x80000000u;
struct Semaphore { std::uint32_t count; std::uint32_t flags; };
struct Timeout { KernelTimespec time; std::uint32_t flags; std::uint32_t clock; };
static_assert(sizeof(Semaphore) == 8 && sizeof(Timeout) == 24);

int Call(void* semaphore, int operation, void* time = nullptr, std::size_t bytes = 0, std::uint64_t value = 0) {
    *__error_nid_postfix() = 731;
    const int result = _umtx_op_nid_postfix(semaphore, operation, value, reinterpret_cast<void*>(bytes), time);
    if (result == 0) Require(*__error_nid_postfix() == 731);
    return result;
}

template <class Predicate>
void Await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    Require(predicate());
}

void CountsAndErrors() {
    for (const std::uint32_t count : {1u, 17u, 0x7fffffffu, HasWaiters | 3u}) {
        Semaphore semaphore{count, 0};
        KernelTimespec immediate{};
        Require(Call(&semaphore, Wait, &immediate, sizeof(immediate), ~std::uint64_t{0}) == 0);
        Require(semaphore.count == count && semaphore.flags == 0);
    }
    Semaphore semaphore{HasWaiters | 7u, 0};
    Require(Call(&semaphore, Wake) == 0 && semaphore.count == (HasWaiters | 7u));
    for (const int operation : {Wait, Wake}) {
        Require(Call(nullptr, operation) == -1 && *__error_nid_postfix() == 14);
        alignas(Semaphore) std::array<std::byte, sizeof(Semaphore) + 1> unaligned{};
        bool rejected = false;
        try { Call(unaligned.data() + 1, operation); }
        catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected);
        Semaphore shared{HasWaiters | 2u, 1};
        ModuleTest::RequireThrows([&] { Call(&shared, operation); }, "process-shared");
        Require(shared.count == (HasWaiters | 2u) && shared.flags == 1);
    }
    for (const KernelTimespec invalid : {KernelTimespec{-1, 0}, KernelTimespec{0, -1}, KernelTimespec{0, 1000000000}}) {
        auto timeout = invalid;
        semaphore.count = 0;
        Require(Call(&semaphore, Wait, &timeout, sizeof(timeout)) == -1 && *__error_nid_postfix() == 22);
        Require(semaphore.count == 0);
    }
}

void TimeoutsAndReuse() {
    Semaphore semaphore{};
    KernelTimespec relative{0, 25000000};
    const auto start = std::chrono::steady_clock::now();
    Require(Call(&semaphore, Wait, &relative, sizeof(relative)) == -1 && *__error_nid_postfix() == 60);
    Require(std::chrono::steady_clock::now() - start >= 15ms);
    Require(semaphore.count == HasWaiters);
    Require(Call(&semaphore, Wake) == 0 && semaphore.count == HasWaiters);
    KernelTimespec immediate{};
    Require(Call(&semaphore, Wait, &immediate) == -1 && *__error_nid_postfix() == 60);
    Timeout absolute{};
    Require(clock_gettime_nid_postfix(4, &absolute.time) == 0);
    absolute.flags = 1;
    absolute.clock = 4;
    Require(Call(&semaphore, Wait, &absolute, sizeof(absolute)) == -1 && *__error_nid_postfix() == 60);
    Require(semaphore.count == HasWaiters);
    semaphore.count = 1;
    Require(Call(&semaphore, Wait) == 0 && semaphore.count == 1);
}

void IndependentQueuesAndWakeOne() {
    Semaphore semaphore{};
    Semaphore other{};
    std::atomic<int> started{0}, finished{0}, wordFinished{0}, otherFinished{0};
    std::array<int, 2> results{-2, -2};
    std::array<std::thread, 2> waiters;
    for (unsigned index = 0; index < waiters.size(); ++index) {
        waiters[index] = std::thread([&, index] {
            KernelTimespec limit{3, 0};
            ++started;
            results[index] = Call(&semaphore, Wait, &limit, sizeof(limit));
            ++finished;
        });
    }
    Await([&] { return started == 2 && std::atomic_ref(semaphore.count).load() == HasWaiters; });
    int otherResult = -2;
    std::thread otherWaiter([&] {
        KernelTimespec limit{3, 0};
        otherResult = Call(&other, Wait, &limit, sizeof(limit));
        ++otherFinished;
    });
    int wordResult = -2;
    std::thread wordWaiter([&] {
        KernelTimespec limit{3, 0};
        wordResult = Call(&semaphore.count, 15, &limit, sizeof(limit), HasWaiters);
        ++wordFinished;
    });
    Await([&] { return std::atomic_ref(other.count).load() == HasWaiters; });
    std::this_thread::sleep_for(35ms);
    Await([&] {
        Require(Call(&semaphore, 16, nullptr, 0, std::numeric_limits<std::int32_t>::max()) == 0);
        return wordFinished == 1;
    });
    Require(finished == 0 && otherFinished == 0);
    std::atomic_ref(semaphore.count).fetch_add(5);
    Require(Call(&semaphore, Wake, nullptr, 0, ~std::uint64_t{0}) == 0);
    Await([&] { return finished == 1; });
    std::this_thread::sleep_for(35ms);
    Require(finished == 1 && otherFinished == 0);
    Require(std::atomic_ref(semaphore.count).load() == (HasWaiters | 5u));
    Require(Call(&semaphore, Wake) == 0);
    Await([&] { return finished == 2; });
    Require(std::atomic_ref(semaphore.count).load() == 5u);
    Require(Call(&other, Wake) == 0);
    Await([&] { return otherFinished == 1; });
    for (auto& waiter : waiters) waiter.join();
    otherWaiter.join();
    wordWaiter.join();
    Require(results[0] == 0 && results[1] == 0 && otherResult == 0 && wordResult == 0);
    Require(other.count == 0);
}

void ForkRejectsActiveWaiters() {
    Semaphore semaphore{};
    int result = -2;
    std::thread waiter([&] {
        KernelTimespec limit{3, 0};
        result = Call(&semaphore, Wait, &limit, sizeof(limit));
    });
    Await([&] { return std::atomic_ref(semaphore.count).load() == HasWaiters; });
    *__error_nid_postfix() = 731;
    const int refused = fork_nid_postfix();
    if (refused == 0) _exit_nid_postfix(99);
    if (refused > 0) {
        int status = 0;
        Require(waitpid_nid_postfix(refused, &status, 0) == refused);
    }
    Require(refused == -1 && *__error_nid_postfix() == 35);
    Require(Call(&semaphore, Wake) == 0);
    waiter.join();
    Require(result == 0 && semaphore.count == 0);
    const int child = fork_nid_postfix();
    Require(child >= 0);
    if (child == 0) {
        KernelTimespec immediate{};
        const int waited = Call(&semaphore, Wait, &immediate, sizeof(immediate));
        _exit_nid_postfix(waited == -1 && *__error_nid_postfix() == 60 && semaphore.count == HasWaiters ? 23 : 24);
    }
    int status = -1;
    Require(waitpid_nid_postfix(child, &status, 0) == child && status == (23 << 8));
    Require(semaphore.count == 0);
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "fork") {
        ForkRejectsActiveWaiters();
        return 0;
    }
    CountsAndErrors();
    TimeoutsAndReuse();
    IndependentQueuesAndWakeOne();
}
