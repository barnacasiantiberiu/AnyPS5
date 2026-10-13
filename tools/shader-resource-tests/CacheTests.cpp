#include "BudgetedShaderCache.hpp"
#include <barrier>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace ShaderRecompiler;

namespace {

struct Value {
    std::once_flag ready;
    std::uint64_t result = 0;
    std::function<void()> onDestroy;
    ~Value() { if (onDestroy) onDestroy(); }
};

using Key = std::vector<std::uint64_t>;
using Cache = BudgetedShaderCache<Key, Value>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::size_t charge() {
    Cache cache(1u << 20u);
    auto value = cache.GetOrCreate({1u});
    cache.Complete({1u}, value, sizeof(Value));
    return cache.Stats().retainedBytes;
}

void lru() {
    const auto unit = charge();
    Cache cache(2u * unit);
    auto first = cache.GetOrCreate({1u});
    auto second = cache.GetOrCreate({2u});
    cache.Complete({1u}, first, sizeof(Value));
    cache.Complete({2u}, second, sizeof(Value));
    require(cache.GetOrCreate({1u}) == first, "hit changed identity");
    auto third = cache.GetOrCreate({3u});
    cache.Complete({3u}, third, sizeof(Value));
    require(cache.Stats().evictions == 1u, "expected one eviction");
    require(cache.GetOrCreate({1u}) == first, "MRU was evicted");
    require(cache.GetOrCreate({2u}) != second, "LRU was retained");
    require(cache.Stats().retainedBytes == 2u * unit, "byte accounting drifted");
    require(cache.Stats().pending == 1u, "pending item missing");
    cache.Complete({1u}, first, sizeof(Value));
    require(cache.Stats().retainedBytes == 2u * unit, "duplicate publication was charged twice");
}

void lifetimeAndOversize() {
    Cache cache(charge());
    auto first = cache.GetOrCreate({1u});
    first->result = 123u;
    cache.Complete({1u}, first, sizeof(Value));
    std::weak_ptr<Value> weak = first;
    auto second = cache.GetOrCreate({2u});
    cache.Complete({2u}, second, sizeof(Value));
    require(!weak.expired() && first->result == 123u, "eviction invalidated active object");
    cache.Complete({1u}, first, sizeof(Value));
    require(cache.Stats().entries == 1u, "stale publication reinserted evicted object");
    first.reset();
    require(weak.expired(), "cache retained evicted object");
    auto giant = cache.GetOrCreate({3u});
    cache.Complete({3u}, giant, std::numeric_limits<std::size_t>::max());
    require(cache.Stats().oversized == 1u && cache.Stats().entries == 1u, "oversized module was retained");
    require(cache.Stats().retainedBytes <= cache.Stats().budgetBytes, "budget exceeded");
}

void inFlightAndUnlimited() {
    Cache cache(charge());
    auto pending = cache.GetOrCreate({1u});
    for (std::uint64_t i = 2u; i < 100u; ++i) {
        auto other = cache.GetOrCreate({i});
        cache.Complete({i}, other, sizeof(Value));
    }
    require(cache.GetOrCreate({1u}) == pending, "in-flight entry was evicted");
    require(cache.Stats().pending == 1u, "in-flight count wrong");
    Cache unlimited(0u);
    for (std::uint64_t i = 0; i < 1000u; ++i) {
        auto value = unlimited.GetOrCreate({i});
        unlimited.Complete({i}, value, std::numeric_limits<std::size_t>::max());
    }
    require(unlimited.Stats().entries == 1000u && unlimited.Stats().evictions == 0u, "unlimited mode evicted objects");
}

void destructorReentry() {
    Cache cache(charge());
    bool destroyed = false;
    auto first = cache.GetOrCreate({1u});
    first->onDestroy = [&] {
        destroyed = true;
        static_cast<void>(cache.Stats());
    };
    cache.Complete({1u}, first, sizeof(Value));
    first.reset();
    auto second = cache.GetOrCreate({2u});
    cache.Complete({2u}, second, sizeof(Value));
    require(destroyed, "evicted object was not destroyed");
}

void concurrency() {
    Cache cache(64u * charge());
    std::atomic<unsigned> builds{0u};
    std::atomic<bool> correct{true};
    std::barrier rendezvous(12);
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < 12u; ++thread) {
        workers.emplace_back([&] {
            auto value = cache.GetOrCreate({999u});
            rendezvous.arrive_and_wait();
            std::call_once(value->ready, [&] {
                builds.fetch_add(1u);
                value->result = 999u;
                cache.Complete({999u}, value, sizeof(Value));
            });
            if (value->result != 999u) correct.store(false);
        });
    }
    for (auto& worker : workers) worker.join();
    require(builds.load() == 1u && correct.load(), "same-key single-flight failed");
    workers.clear();
    for (unsigned thread = 0; thread < 12u; ++thread) {
        workers.emplace_back([&, thread] {
            for (std::uint64_t i = 0; i < 8000u; ++i) {
                const auto key = (i * 17u + thread * 31u) % 257u;
                auto value = cache.GetOrCreate({key});
                std::call_once(value->ready, [&] {
                    value->result = key;
                    cache.Complete({key}, value, sizeof(Value));
                });
                if (value->result != key || cache.Stats().retainedBytes > cache.Stats().budgetBytes) correct.store(false);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    require(correct.load(), "concurrent access corrupted cache or exceeded budget");
    require(cache.Stats().pending == 0u && cache.Stats().evictions != 0u, "concurrent churn did not complete");
}

void parsing() {
    require(ParseShaderCacheMiB(nullptr) == 0u && ParseShaderCacheMiB("0") == 0u, "zero/unset parsing failed");
    require(ParseShaderCacheMiB("64") == 64u * 1024u * 1024u, "MiB conversion failed");
    for (const char* invalid : {"", "-1", "+1", "1x", " 1", "1 ", "1.5", "18446744073709551615"}) {
        bool rejected = false;
        try { static_cast<void>(ParseShaderCacheMiB(invalid)); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "invalid budget was accepted");
    }
    require(AddCacheBytes(std::numeric_limits<std::size_t>::max(), 1u) == std::numeric_limits<std::size_t>::max(), "byte count wrapped");
    std::vector<bool> bits(65u);
    require(CacheVectorBytes(bits) >= 9u, "packed bits were undercounted");
}

}

int main() {
    try {
        lru();
        lifetimeAndOversize();
        inFlightAndUnlimited();
        destructorReentry();
        concurrency();
        parsing();
        std::puts("PASS: LRU, bytes, active lifetime, oversize, pending, unlimited, destructor reentry, 96000 concurrent accesses, parsing");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
