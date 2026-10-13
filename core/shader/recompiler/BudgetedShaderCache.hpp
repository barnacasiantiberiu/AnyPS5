#ifndef CORE_SHADER_RECOMPILER_BUDGETEDSHADERCACHE_HPP
#define CORE_SHADER_RECOMPILER_BUDGETEDSHADERCACHE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ShaderRecompiler {

constexpr std::size_t AddCacheBytes(std::size_t left, std::size_t right) {
    const auto maximum = std::numeric_limits<std::size_t>::max();
    return right > maximum - left ? maximum : left + right;
}

template<typename TValue>
std::size_t CacheVectorBytes(const std::vector<TValue>& values) {
    if constexpr (std::is_same_v<TValue, bool>) return values.capacity() / 8u + (values.capacity() % 8u != 0u);
    const auto maximum = std::numeric_limits<std::size_t>::max();
    return values.capacity() > maximum / sizeof(TValue) ? maximum : values.capacity() * sizeof(TValue);
}

inline std::size_t ParseShaderCacheMiB(const char* text) {
    if (text == nullptr) return 0u;
    const std::string_view value(text);
    if (value.empty()) throw std::runtime_error("APS5_SPECIALIZATION_CACHE_MIB must be an unsigned decimal integer");
    constexpr std::size_t unit = 1024u * 1024u;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max() / unit;
    std::size_t mib = 0;
    for (const char digit : value) {
        if (digit < '0' || digit > '9' || mib > (maximum - static_cast<std::size_t>(digit - '0')) / 10u) {
            throw std::runtime_error("APS5_SPECIALIZATION_CACHE_MIB is invalid or too large");
        }
        mib = mib * 10u + static_cast<std::size_t>(digit - '0');
    }
    return mib * unit;
}

struct ShaderCacheStats {
    std::size_t budgetBytes = 0;
    std::size_t retainedBytes = 0;
    std::size_t entries = 0;
    std::size_t pending = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t oversized = 0;
};

template<typename TKey, typename TValue>
class BudgetedShaderCache {
public:
    explicit BudgetedShaderCache(std::size_t budgetBytes) : budgetBytes(budgetBytes) {
        mutex.lock();
        mutex.unlock();
    }

    std::shared_ptr<TValue> GetOrCreate(const TKey& key) {
        if (budgetBytes == 0u) {
            std::shared_lock lock(mutex);
            const auto found = entries.find(key);
            if (found != entries.end()) {
                hits.fetch_add(1u, std::memory_order_relaxed);
                return found->second.value;
            }
        }
        std::unique_lock lock(mutex);
        const auto found = entries.find(key);
        if (found != entries.end()) {
            hits.fetch_add(1u, std::memory_order_relaxed);
            if (found->second.position) recent.splice(recent.begin(), recent, *found->second.position);
            return found->second.value;
        }
        auto value = std::make_shared<TValue>();
        entries.emplace(key, Entry{value, 0u, std::nullopt});
        misses.fetch_add(1u, std::memory_order_relaxed);
        return value;
    }

    void Complete(const TKey& key, const std::shared_ptr<TValue>& value, std::size_t payloadBytes) {
        if (budgetBytes == 0u) return;
        std::vector<std::shared_ptr<TValue>> retired;
        std::unique_lock lock(mutex);
        const auto found = entries.find(key);
        if (found == entries.end() || found->second.value != value) return;
        auto& entry = found->second;
        if (entry.position) return;
        const auto weight = AddCacheBytes(payloadBytes, overhead(found->first));
        if (weight > budgetBytes) {
            ++oversized;
            entries.erase(found);
            return;
        }
        std::size_t remaining = retainedBytes;
        std::size_t victimCount = 0;
        for (auto it = recent.rbegin(); remaining > budgetBytes - weight; ++it) {
            remaining -= entries.find(*it)->second.bytes;
            ++victimCount;
        }
        retired.reserve(victimCount);
        recent.push_front(found->first);
        entry.position = recent.begin();
        entry.bytes = weight;
        while (retainedBytes > budgetBytes - weight) {
            const auto victim = entries.find(recent.back());
            retired.push_back(victim->second.value);
            retainedBytes -= victim->second.bytes;
            recent.pop_back();
            entries.erase(victim);
            ++evictions;
        }
        retainedBytes += weight;
    }

    bool Bounded() const { return budgetBytes != 0u; }

    ShaderCacheStats Stats() const {
        std::shared_lock lock(mutex);
        return {budgetBytes, retainedBytes, entries.size(), budgetBytes != 0u ? entries.size() - recent.size() : 0u,
                hits.load(std::memory_order_relaxed), misses.load(std::memory_order_relaxed), evictions, oversized};
    }

private:
    struct Entry {
        std::shared_ptr<TValue> value;
        std::size_t bytes;
        std::optional<typename std::list<TKey>::iterator> position;
    };

    static std::size_t overhead(const TKey& key) {
        auto bytes = sizeof(std::pair<const TKey, Entry>) + sizeof(TKey) + 8u * sizeof(void*);
        if constexpr (requires { key.capacity(); typename TKey::value_type; }) {
            const auto capacity = key.capacity();
            const auto maximum = std::numeric_limits<std::size_t>::max();
            const auto unit = 2u * sizeof(typename TKey::value_type);
            bytes = AddCacheBytes(bytes, capacity > maximum / unit ? maximum : capacity * unit);
        }
        return bytes;
    }

    const std::size_t budgetBytes;
    mutable std::shared_mutex mutex;
    std::map<TKey, Entry> entries;
    std::list<TKey> recent;
    std::size_t retainedBytes = 0;
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> misses{0};
    std::uint64_t evictions = 0;
    std::uint64_t oversized = 0;
};

}

#endif
