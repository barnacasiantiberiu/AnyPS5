#include "BdaRangeCache.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

using namespace ShaderRecompiler;

namespace {

struct Range {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t device;
    std::uint32_t permission;
    std::uint32_t reserved = 0;
};

struct Backend {
    using Value = std::uint64_t;
    const std::vector<Range>& ranges;
    std::size_t reads = 0;
    Value U32(std::uint32_t value) { return value; }
    Value Bool(bool value) { return value; }
    Value And(Value left, Value right) { return left != 0u && right != 0u; }
    Value Nonzero(Value value) { return value != 0u; }
    Value LessEqual32(Value left, Value right) { return static_cast<std::uint32_t>(left) <= static_cast<std::uint32_t>(right); }
    Value Less32(Value left, Value right) { return static_cast<std::uint32_t>(left) < static_cast<std::uint32_t>(right); }
    Value Less64(Value left, Value right) { return left < right; }
    Value GreaterEqual64(Value left, Value right) { return left >= right; }
    Value Subtract32(Value left, Value right) { return static_cast<std::uint32_t>(left - right); }
    Value Select32(Value condition, Value yes, Value no) { return condition != 0u ? yes : no; }
    Value LoadBegin(Value index) { ++reads; return ranges.at(index).begin; }
    template<typename TYes, typename TNo>
    Value Choose32(Value condition, const TYes& yes, const TNo& no) { return condition != 0u ? yes() : no(); }
    template<typename TYes, typename TNo>
    Value ChooseBool(Value condition, const TYes& yes, const TNo& no) { return condition != 0u ? yes() : no(); }
};

struct Result {
    std::uint64_t address = 0;
    unsigned fault = 0;
    bool operator==(const Result&) const = default;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint32_t referenceIndex(const std::vector<Range>& ranges, std::uint64_t address) {
    return static_cast<std::uint32_t>(std::upper_bound(ranges.begin(), ranges.end(), address,
        [](std::uint64_t at, const Range& range) { return at < range.begin; }) - ranges.begin());
}

Result lookup(const std::vector<Range>& ranges, std::uint64_t address, std::uint32_t bytes,
        std::uint32_t permission, bool fast, std::uint32_t& cached, bool validHeader = true) {
    if (!validHeader) return {0u, 4u};
    const auto end = address + bytes;
    if (bytes == 0u || end <= address) return {0u, 3u};
    Backend backend{ranges};
    auto index = fast ? static_cast<std::uint32_t>(CachedBdaUpperBound(backend, address, ranges.size(), cached)) : 0u;
    if (index == 0u) index = referenceIndex(ranges, address);
    if (index == 0u) return {0u, 1u};
    const auto& range = ranges.at(index - 1u);
    if (range.reserved != 0u || range.begin >= range.end) return {0u, 4u};
    if (end > range.end) return {0u, 1u};
    if ((range.permission & permission) != permission) return {0u, 2u};
    if (range.device == 0u) return {0u, 4u};
    const auto result = range.device + address - range.begin;
    if (result < range.device || result + bytes <= result) return {0u, 3u};
    if (fast) cached = index;
    return {result, 0u};
}

void edgeCases() {
    std::vector<Range> ranges{{100u, 120u, 1000u, 1u}, {200u, 220u, 2000u, 3u}};
    for (const auto invalid : {0u, 3u, std::numeric_limits<std::uint32_t>::max()}) {
        Backend backend{ranges};
        require(CachedBdaUpperBound(backend, 101u, ranges.size(), invalid) == 0u && backend.reads == 0u, "invalid cache index was dereferenced");
    }
    Backend last{ranges};
    require(CachedBdaUpperBound(last, 201u, ranges.size(), 2u) == 2u && last.reads == 1u, "last entry read a nonexistent successor");
    std::uint32_t cached = 1u;
    require(lookup(ranges, 101u, 4u, 2u, true, cached).fault == 2u, "cache bypassed write permission");
    require(lookup(ranges, 119u, 4u, 1u, true, cached).fault == 1u, "cache bypassed end of range");
    require(lookup(ranges, 101u, 4u, 1u, true, cached, false).fault == 4u, "cache bypassed header validation");
    require(lookup(ranges, 101u, 0u, 1u, true, cached).fault == 3u, "zero length accepted");
    require(lookup(ranges, std::numeric_limits<std::uint64_t>::max(), 4u, 1u, true, cached).fault == 3u, "guest overflow accepted");
    ranges[0].device = std::numeric_limits<std::uint64_t>::max() - 1u;
    require(lookup(ranges, 101u, 4u, 1u, true, cached).fault == 3u, "device overflow accepted");
    ranges[0].device = 0u;
    require(lookup(ranges, 101u, 4u, 1u, true, cached).fault == 4u, "null device address accepted");
    ranges[0].device = 1000u;
    ranges[0].reserved = 1u;
    require(lookup(ranges, 101u, 4u, 1u, true, cached).fault == 4u, "reserved field accepted");
    ranges[0].reserved = 0u;
    ranges[0].begin = ranges[0].end;
    require(lookup(ranges, 120u, 4u, 1u, true, cached).fault == 4u, "invalid interval accepted");
    ranges = {{100u, 300u, 1000u, 3u}, {100u, 150u, 2000u, 1u}, {200u, 250u, 3000u, 3u}};
    Backend duplicates{ranges};
    require(CachedBdaUpperBound(duplicates, 110u, ranges.size(), 1u) == 0u, "duplicate starts accepted an earlier entry");
    cached = 1u;
    std::uint32_t unused = 0u;
    require(lookup(ranges, 210u, 4u, 1u, true, cached) == lookup(ranges, 210u, 4u, 1u, false, unused), "overlap changed upper_bound selection");
    ranges = {};
    Backend empty{ranges};
    require(CachedBdaUpperBound(empty, 0u, 0u, 1u) == 0u && empty.reads == 0u, "empty table read");
}

void differential() {
    std::mt19937_64 random(0x6264612026ull);
    for (unsigned table = 0; table < 2000u; ++table) {
        std::vector<Range> ranges;
        std::uint64_t begin = random() % 1000u;
        const auto count = random() % 128u;
        for (std::size_t i = 0; i < count; ++i) {
            begin += random() % 48u;
            ranges.push_back({begin, begin + random() % 80u, 0x100000u + random() % 100000u, static_cast<std::uint32_t>(random() % 4u)});
        }
        std::uint32_t cached = static_cast<std::uint32_t>(random() % (count + 3u));
        std::uint32_t unused = 0;
        for (unsigned query = 0; query < 500u; ++query) {
            const auto address = random() % (begin + 100u);
            const auto bytes = static_cast<std::uint32_t>(random() % 20u);
            const auto permission = static_cast<std::uint32_t>(random() % 3u + 1u);
            require(lookup(ranges, address, bytes, permission, true, cached) == lookup(ranges, address, bytes, permission, false, unused), "cached lookup disagrees with independent upper_bound");
        }
    }
}

void locality() {
    std::vector<Range> ranges;
    for (std::uint64_t i = 0; i < 1200u; ++i) ranges.push_back({i * 4096u, (i + 1u) * 4096u, 0x1000000u + i * 4096u, 3u});
    Backend backend{ranges};
    for (unsigned i = 0; i < 1024u; ++i) require(CachedBdaUpperBound(backend, 700u * 4096u + i * 4u, ranges.size(), 701u) == 701u, "same-range lookup missed");
    require(backend.reads == 2048u, "cache hit used more than two begin-address probes");
}

}

int main() {
    try {
        edgeCases();
        differential();
        locality();
        std::puts("PASS: production cache-decision template; 1000000 differential lookups; invalid indices, boundaries, faults, duplicates and locality");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
