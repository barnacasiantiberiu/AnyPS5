#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>
#include <stdexcept>

namespace ModuleTest {

inline void Require(bool value, const std::source_location& location = std::source_location::current()) {
    if (value) return;
    std::fprintf(stderr, "%s:%u: check failed in %s\n", location.file_name(), location.line(), location.function_name());
    std::abort();
}

template <typename TFunction>
void RequireThrows(TFunction function, const char* reason = nullptr,
                   const std::source_location& location = std::source_location::current()) {
    try {
        function();
    } catch (const std::runtime_error& error) {
        if (!reason || std::strstr(error.what(), reason)) return;
        std::fprintf(stderr, "Unexpected error: %s; expected: %s\n", error.what(), reason);
        Require(false, location);
    }
    std::fprintf(stderr, "Expected std::runtime_error\n");
    Require(false, location);
}

}
