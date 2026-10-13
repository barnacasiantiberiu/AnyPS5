#include "prx/libc/include/general/VabiMacros.hpp"
#include "ModuleChecks.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <string>
#ifdef _WIN32
#include <filesystem>
#include <windows.h>
#endif
extern "C" {
int APS5_VABI shm_open_nid_postfix(const char*, int, int);
int APS5_VABI shm_unlink_nid_postfix(const char*);
int APS5_VABI ftruncate_nid_postfix(int, std::int64_t);
std::int64_t APS5_VABI pread_nid_postfix(int, void*, std::size_t, std::int64_t);
std::int64_t APS5_VABI pwrite_nid_disambig1_nid_postfix(int, const void*, std::size_t, std::int64_t);
int APS5_VABI close_nid_postfix(int);
int* APS5_VABI __error_nid_postfix();
}
using ModuleTest::Require;
#ifdef _WIN32
struct FreshNamespace {
    std::filesystem::path root;
    std::wstring previous;
    FreshNamespace() {
        const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        Require(size > 0);
        previous.resize(size);
        const auto written = GetEnvironmentVariableW(L"LOCALAPPDATA", previous.data(), size);
        Require(written > 0 && written < size);
        previous.resize(written);
        root = std::filesystem::temp_directory_path() / ("anyps5-shm-test-" + std::to_string(GetCurrentProcessId()) +
            "-" + std::to_string(GetTickCount64()));
        Require(std::filesystem::create_directory(root));
        Require(SetEnvironmentVariableW(L"LOCALAPPDATA", (root / "profile").c_str()));
    }
    ~FreshNamespace() {
        Require(SetEnvironmentVariableW(L"LOCALAPPDATA", previous.c_str()));
        std::filesystem::remove_all(root);
    }
};
#endif
static const char* const anonymous = reinterpret_cast<const char*>(1);
static void RequireInvalid(const char* path, int flags) {
    *__error_nid_postfix() = 0;
    Require(shm_open_nid_postfix(path, flags, 0600) == -1);
    Require(*__error_nid_postfix() == 22);
}
int main() {
#ifdef _WIN32
    const FreshNamespace fresh;
#endif
    *__error_nid_postfix() = 13;
    const int first = shm_open_nid_postfix(anonymous, 0x100202, 0600);
    const int second = shm_open_nid_postfix(anonymous, 0x2, 0);
    const int third = shm_open_nid_postfix(anonymous, 0x100e02, 0400);
    Require(first >= 0 && second >= 0 && third >= 0);
    Require(first != second && second != third && first != third);
    Require(*__error_nid_postfix() == 13);
    Require(ftruncate_nid_postfix(first, 0x4000) == 0);
    const char text[] = "anonymous";
    Require(pwrite_nid_disambig1_nid_postfix(first, text, sizeof(text), 0x1000) == static_cast<std::int64_t>(sizeof(text)));
    char back[sizeof(text)] = {};
    Require(pread_nid_postfix(first, back, sizeof(back), 0x1000) == static_cast<std::int64_t>(sizeof(back)));
    Require(std::memcmp(back, text, sizeof(text)) == 0);
    char last = 1;
    Require(pread_nid_postfix(first, &last, 1, 0x3fff) == 1 && last == 0);
    Require(pread_nid_postfix(first, &last, 1, 0x4000) == 0);
    Require(pread_nid_postfix(second, back, sizeof(back), 0) == 0);
    Require(pwrite_nid_disambig1_nid_postfix(third, text, sizeof(text), 0) == 0);
    Require(ftruncate_nid_postfix(third, 0x4000) == 0);
    Require(pwrite_nid_disambig1_nid_postfix(third, text, sizeof(text), 0) == static_cast<std::int64_t>(sizeof(text)));
    Require(close_nid_postfix(first) == 0 && close_nid_postfix(second) == 0 && close_nid_postfix(third) == 0);
    const auto name = "/anyps5-shm-errno-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    *__error_nid_postfix() = 73;
    const int named = shm_open_nid_postfix(name.c_str(), 0xa02, 0600);
    const int creationError = *__error_nid_postfix();
    Require(named >= 0);
    *__error_nid_postfix() = 91;
    const int reopened = shm_open_nid_postfix(name.c_str(), 0x202, 0600);
    const int reopenError = *__error_nid_postfix();
    Require(reopened >= 0);
    Require(close_nid_postfix(reopened) == 0 && close_nid_postfix(named) == 0);
    Require(shm_unlink_nid_postfix(name.c_str()) == 0);
    Require(creationError == 73 && reopenError == 91);
    RequireInvalid(anonymous, 0x0);
    RequireInvalid(anonymous, 0x200);
    RequireInvalid(anonymous, 0x1);
    RequireInvalid(anonymous, 0x3);
    RequireInvalid(anonymous, 0x2 | 0x8);
    RequireInvalid(anonymous, 0x2 | 0x4);
    RequireInvalid("/anyps5-shm", 0x1);
    RequireInvalid("/anyps5-shm", 0x202 | 0x8);
    RequireInvalid("", 0x202);
}
