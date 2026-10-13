#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "tests/ModuleChecks.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>
#include <vector>

extern "C" {
int APS5_VABI sceNpEntitlementAccessGetPftFlag(std::uint32_t*);
int APS5_VABI sceNpEntitlementAccessGetGameTrialsFlag(std::uint32_t*);
int APS5_VABI sceNpEntitlementAccessGetSkuFlag(std::uint32_t*);
int APS5_VABI sceNpEntitlementAccessGetAddcontEntitlementInfo(std::uint32_t, const NpUnifiedEntitlementLabel*, NpEntitlementAccessAddcontEntitlementInfo*);
int APS5_VABI sceNpEntitlementAccessGetAddcontEntitlementInfoList(std::uint32_t, NpEntitlementAccessAddcontEntitlementInfo*, std::uint32_t, std::uint32_t*);
int APS5_VABI sceNpEntitlementAccessGetEntitlementKey(std::uint32_t, const NpUnifiedEntitlementLabel*, NpEntitlementAccessEntitlementKey*);
int APS5_VABI sceNpEntitlementAccessInitialize(const NpEntitlementAccessInitParam*, NpEntitlementAccessBootParam*);
int APS5_VABI sceNpEntitlementAccessAbortRequest();
int APS5_VABI sceNpEntitlementAccessDeleteRequest();
int APS5_VABI sceNpEntitlementAccessGenerateTransactionId(void*);
int APS5_VABI sceNpEntitlementAccessPollConsumeEntitlement();
int APS5_VABI sceNpEntitlementAccessRequestConsumeUnifiedEntitlement();
int APS5_VABI sceNpEntitlementAccessRequestConsumeServiceEntitlement();
int APS5_VABI sceNpEntitlementAccessPollServiceEntitlementInfo();
int APS5_VABI sceNpEntitlementAccessPollUnifiedEntitlementInfo();
int APS5_VABI sceNpEntitlementAccessRequestServiceEntitlementInfo();
int APS5_VABI sceNpEntitlementAccessRequestUnifiedEntitlementInfo();
int APS5_VABI sceNpEntitlementAccessPollServiceEntitlementInfoList();
int APS5_VABI sceNpEntitlementAccessPollUnifiedEntitlementInfoList();
int APS5_VABI sceNpEntitlementAccessRequestServiceEntitlementInfoList();
int APS5_VABI sceNpEntitlementAccessRequestUnifiedEntitlementInfoList();
}
using ModuleTest::Require;
using ModuleTest::RequireThrows;
constexpr int Parameter = static_cast<int>(0x80558003u);
constexpr int TrialParameter = static_cast<int>(0x817D0002u);
constexpr int Missing = static_cast<int>(0x80558007u);
constexpr int NoKey = static_cast<int>(0x817D0007u);
constexpr int SignedOut = static_cast<int>(0x80550006u);
constexpr std::uint32_t Sentinel = 0xabababab;

NpUnifiedEntitlementLabel Label(const char* name) {
    NpUnifiedEntitlementLabel result{};
    Require(std::strlen(name) < sizeof(result.data));
    std::strcpy(result.data, name);
    return result;
}
template<class T> T Filled() { T value; std::memset(&value, 0xab, sizeof(value)); return value; }
template<class T> void Same(const T& actual, const T& expected) { Require(std::memcmp(&actual, &expected, sizeof(T)) == 0); }
void Flags() {
    using Query = int (APS5_VABI *)(std::uint32_t*);
    for (Query query : {sceNpEntitlementAccessGetPftFlag, sceNpEntitlementAccessGetGameTrialsFlag, sceNpEntitlementAccessGetSkuFlag}) {
        const bool sku = query == sceNpEntitlementAccessGetSkuFlag;
        Require(query(nullptr) == (sku ? Parameter : TrialParameter));
        for (std::uint32_t initial : {0u, 1u, 0xffffffffu}) {
            std::array<std::uint32_t, 3> guarded{Sentinel, initial, Sentinel};
            Require(query(&guarded[1]) == 0);
            Require(guarded[0] == Sentinel && guarded[1] == (sku ? 3u : 0u) && guarded[2] == Sentinel);
        }
    }
}
void Lists(bool empty) {
    for (std::uint32_t service : {0u, 0xffffffffu}) {
        std::array<std::uint32_t, 3> hits{Sentinel, Sentinel, Sentinel};
        Require(sceNpEntitlementAccessGetAddcontEntitlementInfoList(service, nullptr, 0, &hits[1]) == 0);
        Require(hits[0] == Sentinel && hits[1] == (empty ? 0u : 2u) && hits[2] == Sentinel);
        for (std::uint32_t capacity : {0u, 1u, 2u, 3u}) {
            auto list = Filled<std::array<NpEntitlementAccessAddcontEntitlementInfo, 5>>();
            auto expected = list;
            if (!empty) for (unsigned i = 0; i < std::min(capacity, 2u); ++i) {
                expected[i + 1] = {};
                expected[i + 1].entitlement_label = Label(i == 0 ? "addon.one" : "1234567890ABCDEF");
                expected[i + 1].package_type = 3;
                expected[i + 1].download_status = 4;
            }
            Require(sceNpEntitlementAccessGetAddcontEntitlementInfoList(service, list.data() + 1, capacity, &hits[1]) == 0);
            Same(list, expected);
            Require(hits[1] == (empty ? 0u : 2u));
        }
        auto info = Filled<std::array<NpEntitlementAccessAddcontEntitlementInfo, 3>>();
        auto expected = info;
        const auto label = Label("1234567890ABCDEF");
        Require(sceNpEntitlementAccessGetAddcontEntitlementInfo(service, &label, &info[1]) == (empty ? Missing : 0));
        if (!empty) expected[1] = {label, 3, 4};
        Same(info, expected);
        const auto absent = Label("not.installed");
        Require(sceNpEntitlementAccessGetAddcontEntitlementInfo(service, &absent, &info[1]) == Missing);
        Same(info, expected);
    }
}
void Invalid() {
    auto info = Filled<NpEntitlementAccessAddcontEntitlementInfo>();
    const auto original = info;
    auto label = Label("addon.one");
    Require(sceNpEntitlementAccessGetAddcontEntitlementInfo(0, nullptr, &info) == Parameter);
    Require(sceNpEntitlementAccessGetAddcontEntitlementInfo(0, &label, nullptr) == Parameter);
    Same(info, original);
    std::uint32_t count = Sentinel;
    Require(sceNpEntitlementAccessGetAddcontEntitlementInfoList(0, nullptr, 1, &count) == Parameter);
    Require(count == Sentinel);
    Require(sceNpEntitlementAccessGetAddcontEntitlementInfoList(0, &info, 1, nullptr) == Parameter);
    Same(info, original);
    std::memset(&label, 'A', sizeof(label));
    Require(sceNpEntitlementAccessGetAddcontEntitlementInfo(0, &label, &info) == Missing);
    Same(info, original);
}
void Offline() {
    auto boot = Filled<NpEntitlementAccessBootParam>();
    const auto original = boot;
    Require(sceNpEntitlementAccessInitialize(nullptr, &boot) == 0);
    Same(boot, original);
    Require(sceNpEntitlementAccessAbortRequest() == 0);
    Require(sceNpEntitlementAccessDeleteRequest() == 0);
    const auto label = Label("addon.one");
    auto key = Filled<std::array<NpEntitlementAccessEntitlementKey, 3>>();
    const auto keyOriginal = key;
    Require(sceNpEntitlementAccessGetEntitlementKey(0, &label, &key[1]) == NoKey);
    Require(sceNpEntitlementAccessGetEntitlementKey(0, nullptr, &key[1]) == TrialParameter);
    Require(sceNpEntitlementAccessGetEntitlementKey(0, &label, nullptr) == TrialParameter);
    Same(key, keyOriginal);
    auto transaction = Filled<std::array<std::uint8_t, 128>>();
    const auto saved = transaction;
    Require(sceNpEntitlementAccessGenerateTransactionId(transaction.data()) == SignedOut);
    Require(sceNpEntitlementAccessGenerateTransactionId(nullptr) == Parameter);
    Same(transaction, saved);
    using Query = int (APS5_VABI *)();
    for (Query query : {sceNpEntitlementAccessPollConsumeEntitlement, sceNpEntitlementAccessRequestConsumeUnifiedEntitlement,
         sceNpEntitlementAccessRequestConsumeServiceEntitlement, sceNpEntitlementAccessPollServiceEntitlementInfo,
         sceNpEntitlementAccessPollUnifiedEntitlementInfo, sceNpEntitlementAccessRequestServiceEntitlementInfo,
         sceNpEntitlementAccessRequestUnifiedEntitlementInfo, sceNpEntitlementAccessPollServiceEntitlementInfoList,
         sceNpEntitlementAccessPollUnifiedEntitlementInfoList, sceNpEntitlementAccessRequestServiceEntitlementInfoList,
         sceNpEntitlementAccessRequestUnifiedEntitlementInfoList}) Require(query() == SignedOut);
}
int main(int argc, char** argv) {
    Require(argc == 3);
    const std::string_view mode(argv[1]);
    if (mode == "missing") std::filesystem::remove(argv[2]);
    else {
        std::ofstream fixture(argv[2]);
        Require(fixture.good());
        if (mode == "malformed") fixture << "12345678901234567\n";
        else if (mode != "empty") fixture << "# generated test data\n addon.one ; comment\n\t1234567890ABCDEF\r\n\n";
        fixture.close();
        Require(fixture.good());
    }
    if (mode == "flags") Flags();
    else if (mode == "list" || mode == "empty") Lists(mode == "empty");
    else if (mode == "invalid") Invalid();
    else if (mode == "offline") Offline();
    else if (mode == "concurrent") {
        std::vector<std::thread> workers;
        for (unsigned i = 0; i < 6; ++i) workers.emplace_back([] { for (unsigned n = 0; n < 20; ++n) { Flags(); Lists(false); } });
        for (auto& worker : workers) worker.join();
    } else {
        Require(mode == "malformed" || mode == "missing");
        auto info = Filled<NpEntitlementAccessAddcontEntitlementInfo>();
        const auto original = info;
        std::uint32_t count = Sentinel;
        RequireThrows([&] { sceNpEntitlementAccessGetAddcontEntitlementInfoList(0, &info, 1, &count); }, mode == "missing" ? "cannot read" : "longer than 16");
        Require(count == Sentinel); Same(info, original);
    }
}
