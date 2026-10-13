#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

extern "C" std::filesystem::path ResolvePathNoFollow_nid_no_patch(const char* path);
extern "C" std::filesystem::path ResolveNativeLinks_nid_no_patch(const std::filesystem::path& path, bool followFinal);
extern "C" std::optional<std::string> ReadStoredGuestLink_nid_no_patch(const std::filesystem::path& path);
inline constexpr wchar_t GuestLinkStream[] = L":AnyPS5.GuestSymlink.v1";
inline constexpr std::string_view GuestLinkMagic = "AnyPS5 symbolic link v1\n";

namespace GuestLinks {
inline constexpr std::string_view Prefix = ".anyps5-absolute-v1/";

inline std::string Encode_nid_no_patch(std::string_view target) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(Prefix);
    for (unsigned char byte : target) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

inline std::optional<std::string> Decode_nid_no_patch(std::string_view target) {
    if (!target.starts_with(Prefix)) return std::nullopt;
    target.remove_prefix(Prefix.size());
    const auto invalid = [] {
        throw std::filesystem::filesystem_error("Malformed guest symbolic link", std::make_error_code(std::errc::invalid_argument));
    };
    if (target.empty() || target.size() % 2 != 0 || target.size() > 2046) invalid();
    const auto nibble = [&](char c) -> unsigned {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        invalid();
        return 0;
    };
    std::string result;
    for (std::size_t i = 0; i < target.size(); i += 2) {
        const char byte = static_cast<char>((nibble(target[i]) << 4) | nibble(target[i + 1]));
        if (!byte) invalid();
        result += byte;
    }
    return result;
}
}
