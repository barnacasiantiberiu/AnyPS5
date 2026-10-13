#include "SceTypes.hpp"
#include "tests/ModuleChecks.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

extern "C" {
int APS5_VABI sceGameUpdateInitialize();
int APS5_VABI sceGameUpdateTerminate();
int APS5_VABI sceGameUpdateCreateRequest();
int APS5_VABI sceGameUpdateDeleteRequest(int);
int APS5_VABI sceGameUpdateAbortRequest(int);
int APS5_VABI sceGameUpdateCheck(int, const GameUpdateCheckParam*, GameUpdateCheckResult*);
int APS5_VABI sceGameUpdateGetAddcontLatestVersion(std::uint32_t, const void*, GameUpdateAddcontVersionInfo*);
}

using ModuleTest::Require;
constexpr int NotInitialized = static_cast<int>(0x80412801u);
constexpr int InvalidArgument = static_cast<int>(0x80412803u);
constexpr int InvalidSize = static_cast<int>(0x80412804u);
constexpr int Offline = static_cast<int>(0x80D50001u);
constexpr std::array<std::uint8_t, 16> Label{};
struct GuardedInfo {
    std::uint64_t before;
    GameUpdateAddcontVersionInfo info;
    std::array<std::uint8_t, 16> after;
};
static_assert(sizeof(GameUpdateAddcontVersionInfo) == 0x30);

GuardedInfo Fixture(std::size_t size = sizeof(GameUpdateAddcontVersionInfo)) {
    GuardedInfo result;
    std::memset(&result, 0xab, sizeof(result));
    result.info.size = size;
    return result;
}
void Unchanged(const GuardedInfo& actual, const GuardedInfo& expected) {
    Require(std::memcmp(&actual, &expected, sizeof(actual)) == 0);
}
void Query(std::size_t size, std::uint32_t service) {
    auto actual = Fixture(size), expected = actual;
    std::memset(&expected.info, 0, sizeof(expected.info));
    expected.info.size = size;
    Require(sceGameUpdateGetAddcontLatestVersion(service, Label.data(), &actual.info) == 0);
    Unchanged(actual, expected);
}
void Validation() {
    Require(sceGameUpdateInitialize() == 0);
    auto info = Fixture();
    const auto original = info;
    Require(sceGameUpdateGetAddcontLatestVersion(0, nullptr, &info.info) == InvalidArgument);
    Unchanged(info, original);
    Require(sceGameUpdateGetAddcontLatestVersion(0, Label.data(), nullptr) == InvalidArgument);
    Require(sceGameUpdateGetAddcontLatestVersion(0, nullptr, nullptr) == InvalidArgument);
    for (std::size_t size : {std::size_t{0}, std::size_t{8}, sizeof(info.info) - 1}) {
        info = Fixture(size);
        const auto saved = info;
        Require(sceGameUpdateGetAddcontLatestVersion(0, nullptr, &info.info) == InvalidArgument);
        Require(sceGameUpdateGetAddcontLatestVersion(0, Label.data(), &info.info) == InvalidSize);
        Unchanged(info, saved);
    }
    for (std::size_t size : {sizeof(info.info), sizeof(info.info) + 8, std::numeric_limits<std::size_t>::max()}) {
        Query(size, 0);
        Query(size, std::numeric_limits<std::uint32_t>::max());
    }
    Require(sceGameUpdateTerminate() == 0);
}
void Lifecycle() {
    auto info = Fixture();
    const auto original = info;
    Require(sceGameUpdateGetAddcontLatestVersion(0, Label.data(), &info.info) == NotInitialized);
    Require(sceGameUpdateGetAddcontLatestVersion(0, nullptr, nullptr) == NotInitialized);
    Unchanged(info, original);
    for (int cycle = 0; cycle < 3; ++cycle) {
        Require(sceGameUpdateInitialize() == 0);
        Require(sceGameUpdateInitialize() == 0);
        Query(sizeof(info.info), 0);
        Require(sceGameUpdateTerminate() == 0);
        Require(sceGameUpdateTerminate() == 0);
        Require(sceGameUpdateGetAddcontLatestVersion(0, Label.data(), &info.info) == NotInitialized);
        Unchanged(info, original);
    }
}
void OfflineRequests() {
    Require(sceGameUpdateInitialize() == 0);
    GameUpdateCheckParam param{};
    param.size = sizeof(param);
    struct GuardedResult { std::uint64_t before; GameUpdateCheckResult result; std::uint64_t after; } result;
    std::memset(&result, 0xab, sizeof(result));
    result.result.size = sizeof(result.result);
    const auto saved = result;
    Require(sceGameUpdateCreateRequest() == Offline);
    for (int id : {0, -1, std::numeric_limits<int>::max()}) {
        Require(sceGameUpdateCheck(id, &param, &result.result) == Offline);
        Require(std::memcmp(&result, &saved, sizeof(result)) == 0);
        Require(sceGameUpdateCheck(id, nullptr, nullptr) == Offline);
        Require(sceGameUpdateAbortRequest(id) == 0);
        Require(sceGameUpdateDeleteRequest(id) == 0);
    }
    Require(sceGameUpdateTerminate() == 0);
}
void ConcurrentQueries() {
    Require(sceGameUpdateInitialize() == 0);
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 4; ++worker) workers.emplace_back([worker] {
        for (int i = 0; i < 100; ++i) Query(sizeof(GameUpdateAddcontVersionInfo), worker);
    });
    for (auto& worker : workers) worker.join();
    Require(sceGameUpdateTerminate() == 0);
}
int main(int argc, char** argv) {
    Require(argc == 2);
    const std::string_view mode(argv[1]);
    if (mode == "validation") Validation();
    else if (mode == "lifecycle") Lifecycle();
    else if (mode == "offline") OfflineRequests();
    else { Require(mode == "concurrent"); ConcurrentQueries(); }
}
