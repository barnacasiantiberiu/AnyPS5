#pragma once
#include <cstdint>
#include <string_view>

namespace HostContent {
inline std::int64_t Id_nid_no_patch(std::string_view directory) {
    if (!directory.starts_with("export-")) return 0;
    const auto token = directory.substr(7);
    const auto separator = token.find('-');
    if (separator == 0 || separator == token.npos || separator + 1 == token.size()) return 0;
    for (std::size_t i = 0; i < token.size(); ++i)
        if (i != separator && (token[i] < '0' || token[i] > '9')) return 0;
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char ch : directory) { hash ^= ch; hash *= 1099511628211ULL; }
    return static_cast<std::int64_t>((hash & 0x7fffffffffffffffULL) | 1);
}
}
