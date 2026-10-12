#include "prx/libSceSsl/SslTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "SslCertificateFixture.hpp"

#include <cstdio>
#include <atomic>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
int APS5_VABI sceSslInit_nid_postfix(std::uint64_t);
int APS5_VABI sceSslTerm_nid_postfix(int);
int APS5_VABI sceSslGetMemoryPoolStats(int, SceSsl::MemoryPoolStats*);
int APS5_VABI sceSslLoadCert(int, int, SceSsl::Data**, SceSsl::Data*, SceSsl::Data*);
int APS5_VABI sceSslUnloadCert(int);
int APS5_VABI sceSslGetCaList(int, SceSsl::CaList*);
int APS5_VABI sceSslFreeCaList(int, SceSsl::CaList*);
int APS5_VABI sceSslGetCaCerts(int, SceSsl::CaCerts*);
int APS5_VABI sceSslFreeCaCerts(int, SceSsl::CaCerts*);
}

namespace {

constexpr int NotFound = static_cast<int>(0x8095F004);

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename TAction>
void Throws(TAction action) {
    bool caught = false;
    try { action(); } catch (const std::exception&) { caught = true; }
    Check(caught, "unsupported or malformed load was accepted");
}

SceSsl::Data Data(const char* text) {
    return {reinterpret_cast<std::uint8_t*>(const_cast<char*>(text)), std::strlen(text)};
}

SceSsl::MemoryPoolStats Stats(int id) {
    SceSsl::MemoryPoolStats stats{};
    Check(sceSslGetMemoryPoolStats(id, &stats) == 0, "stats failed");
    Check(stats.currentInuseSize <= stats.maxInuseSize && stats.maxInuseSize <= stats.poolSize && !stats.reserved,
        "pool accounting bounds failed");
    return stats;
}

void CheckRoot(const SceSsl::CaCerts& snapshot) {
    Check(snapshot.num == 1 && snapshot.certs[0].size == RootDer.size(), "CA count or DER size differs");
    Check(std::memcmp(snapshot.certs[0].ptr, RootDer.data(), RootDer.size()) == 0, "CA bytes changed");
}

void Lifecycle() {
    const int id = sceSslInit_nid_postfix(1024 * 1024);
    std::string original = RootPem;
    auto data = Data(original.c_str());
    auto* pointer = &data;
    Check(sceSslLoadCert(id, 1, &pointer, nullptr, nullptr) == 0, "initial CA load failed");
    original.assign(original.size(), '?');
    Check(Stats(id).currentInuseSize > RootDer.size(), "loaded certificate is not retained/accounted");
    SceSsl::CaCerts bytes{};
    Check(sceSslGetCaCerts(id, &bytes) == 0, "encoded CA query failed");
    CheckRoot(bytes);
    SceSsl::CaList handles{};
    Check(sceSslGetCaList(id, &handles) == 0 && handles.num == 1 && handles.certs[0], "CA handle absent");
    const int other = sceSslInit_nid_postfix(1024 * 1024);
    SceSsl::CaList empty{};
    Check(sceSslGetCaList(other, &empty) == NotFound, "CA escaped its context");
    auto foreign = handles;
    Throws([&] { sceSslFreeCaList(other, &foreign); });
    Check(sceSslUnloadCert(id) == 0, "unload failed");
    Check(sceSslGetCaList(id, &empty) == NotFound && !empty.certs && !empty.num, "unload retained active CA");
    CheckRoot(bytes);
    Check(sceSslFreeCaList(id, &handles) == 0 && !handles.certs && !handles.num, "handle snapshot release failed");
    Check(sceSslFreeCaCerts(id, &bytes) == 0 && !bytes.certs && !bytes.pool && !bytes.num, "encoded snapshot release failed");
    Check(Stats(id).currentInuseSize == 0, "certificate allocation leaked after release");
    Throws([&] { sceSslFreeCaList(id, &foreign); });
    Check(sceSslTerm_nid_postfix(id) == 0 && sceSslTerm_nid_postfix(other) == 0, "term failed");
}

void AtomicFailures() {
    const int id = sceSslInit_nid_postfix(1024 * 1024);
    SceSsl::Data der{const_cast<std::uint8_t*>(RootDer.data()), RootDer.size()};
    auto* pointer = &der;
    Check(sceSslLoadCert(id, 1, &pointer, nullptr, nullptr) == 0, "DER CA load failed");
    const auto before = Stats(id).currentInuseSize;
    auto bad = Data("not a certificate");
    SceSsl::Data* mixed[]{&der, &bad};
    Throws([&] { sceSslLoadCert(id, 2, mixed, nullptr, nullptr); });
    Check(Stats(id).currentInuseSize == before, "failed chain changed live allocation");
    SceSsl::CaCerts bytes{};
    Check(sceSslGetCaCerts(id, &bytes) == 0, "CA vanished after failure");
    CheckRoot(bytes);
    Check(sceSslFreeCaCerts(id, &bytes) == 0, "snapshot free failed");
    Throws([&] { sceSslLoadCert(id, -1, nullptr, nullptr, nullptr); });
    Throws([&] { sceSslLoadCert(id, 1, nullptr, nullptr, nullptr); });
    SceSsl::Data* absent = nullptr;
    Throws([&] { sceSslLoadCert(id, 1, &absent, nullptr, nullptr); });
    Throws([&] { sceSslLoadCert(id, 0, nullptr, &der, nullptr); });
    SceSsl::Data empty{nullptr, 0};
    pointer = &empty;
    Throws([&] { sceSslLoadCert(id, 1, &pointer, nullptr, nullptr); });
    std::vector<std::uint8_t> trailing(RootDer.begin(), RootDer.end());
    trailing.push_back(0);
    SceSsl::Data extra{trailing.data(), trailing.size()};
    pointer = &extra;
    Throws([&] { sceSslLoadCert(id, 1, &pointer, nullptr, nullptr); });
    std::string embedded = RootPem;
    embedded[embedded.size() / 2] = 0;
    SceSsl::Data nul{reinterpret_cast<std::uint8_t*>(embedded.data()), embedded.size()};
    pointer = &nul;
    Throws([&] { sceSslLoadCert(id, 1, &pointer, nullptr, nullptr); });
    pointer = &der;
    Check(Stats(id).currentInuseSize == before, "argument failure changed retained CA");
    Check(sceSslTerm_nid_postfix(id) == 0, "term failed");
    Throws([&] { sceSslLoadCert(id, 1, &pointer, nullptr, nullptr); });
    const int tiny = sceSslInit_nid_postfix(32);
    Throws([&] { sceSslLoadCert(tiny, 1, &pointer, nullptr, nullptr); });
    Check(Stats(tiny).currentInuseSize == 0, "failed small-pool load leaked");
    Check(sceSslTerm_nid_postfix(tiny) == 0, "small-pool term failed");
}

void ClientIdentity() {
    const int id = sceSslInit_nid_postfix(1024 * 1024);
    auto client = Data(ClientPem);
    auto key = Data(ClientKey);
    auto wrong = Data(RootKey);
    Check(sceSslLoadCert(id, 0, nullptr, &client, &key) == 0, "client identity load failed");
    const auto before = Stats(id).currentInuseSize;
    Check(before > 0, "identity not retained");
    Throws([&] { sceSslLoadCert(id, 0, nullptr, &client, &wrong); });
    auto root = Data(RootPem);
    auto* pointer = &root;
    Throws([&] { sceSslLoadCert(id, 1, &pointer, &client, &wrong); });
    SceSsl::CaList empty{};
    Check(sceSslGetCaList(id, &empty) == NotFound, "failed identity import committed its CA");
    Check(Stats(id).currentInuseSize == before, "mismatched key changed identity");
    Check(sceSslUnloadCert(id) == 0 && Stats(id).currentInuseSize == 0, "identity release failed");
    Check(sceSslTerm_nid_postfix(id) == 0, "identity term failed");
}

void SnapshotsAndBudget() {
    const int id = sceSslInit_nid_postfix(65536);
    auto root = Data(RootPem);
    auto* pointer = &root;
    Check(sceSslLoadCert(id, 1, &pointer, nullptr, nullptr) == 0, "snapshot CA load failed");
    SceSsl::CaList first{};
    Check(sceSslGetCaList(id, &first) == 0 && first.num == 1, "first snapshot failed");
    const auto firstHandle = first.certs[0];
    std::string chain = std::string(RootPem) + ClientPem;
    auto both = Data(chain.c_str());
    pointer = &both;
    Check(sceSslLoadCert(id, 1, &pointer, nullptr, nullptr) == 0, "PEM chain import failed");
    SceSsl::CaList second{};
    Check(sceSslGetCaList(id, &second) == 0 && second.num == 3, "PEM chain count differs");
    Check(first.num == 1 && first.certs[0] == firstHandle && second.certs[0] == firstHandle,
        "later import changed an earlier snapshot");
    auto corrupt = second;
    ++corrupt.num;
    const auto before = Stats(id).currentInuseSize;
    Throws([&] { sceSslFreeCaList(id, &corrupt); });
    Check(Stats(id).currentInuseSize == before && corrupt.num == 4, "invalid free changed allocation");
    std::vector<SceSsl::CaCerts> allocations;
    bool exhausted = false;
    constexpr std::uint64_t guard = 0xfeed0123456789abULL;
    for (int n = 0; n < 256; ++n) {
        struct Guarded { std::uint64_t before; SceSsl::CaCerts value; std::uint64_t after; } output{guard, {}, guard};
        const auto used = Stats(id).currentInuseSize;
        try {
            Check(sceSslGetCaCerts(id, &output.value) == 0, "snapshot query failed");
        } catch (const std::bad_alloc&) {
            Check(!output.value.certs && !output.value.pool && !output.value.num, "failed query modified output");
            Check(Stats(id).currentInuseSize == used, "failed query leaked memory");
            exhausted = true;
        }
        Check(output.before == guard && output.after == guard, "snapshot query overwrote guard");
        if (exhausted) break;
        Check(output.value.num == 3, "snapshot lost a certificate");
        Check(output.value.certs[0].size == RootDer.size() &&
            std::memcmp(output.value.certs[0].ptr, RootDer.data(), RootDer.size()) == 0, "snapshot content corrupted");
        allocations.push_back(output.value);
    }
    Check(exhausted, "SSL pool budget was not enforced");
    Check(sceSslUnloadCert(id) == 0, "snapshot unload failed");
    Check(first.certs[0] == firstHandle, "unload invalidated outstanding snapshot");
    for (auto& bytes : allocations) Check(sceSslFreeCaCerts(id, &bytes) == 0, "snapshot release failed");
    Check(sceSslFreeCaList(id, &first) == 0 && sceSslFreeCaList(id, &second) == 0, "list release failed");
    Check(Stats(id).currentInuseSize == 0, "snapshot budget not reclaimed");
    pointer = &root;
    Check(sceSslLoadCert(id, 1, &pointer, nullptr, nullptr) == 0, "reclaimed pool cannot be reused");
    Check(sceSslTerm_nid_postfix(id) == 0, "snapshot context term failed");
}

void SharedContext() {
    const int id = sceSslInit_nid_postfix(1024 * 1024);
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
        try {
            for (int n = 0; n < 8; ++n) {
                auto root = Data(RootPem);
                auto* pointer = &root;
                Check(sceSslLoadCert(id, 1, &pointer, nullptr, nullptr) == 0, "shared CA load failed");
                SceSsl::CaList list{};
                Check(sceSslGetCaList(id, &list) == 0 && list.num > 0, "shared snapshot failed");
                Check(sceSslFreeCaList(id, &list) == 0, "shared snapshot release failed");
                Stats(id);
            }
        } catch (...) { ++failures; }
    });
    for (auto& worker : workers) worker.join();
    Check(failures == 0, "concurrent operation failed");
    SceSsl::CaList list{};
    Check(sceSslGetCaList(id, &list) == 0 && list.num == 32, "concurrent import lost certificates");
    Check(sceSslFreeCaList(id, &list) == 0 && sceSslUnloadCert(id) == 0, "shared cleanup failed");
    Check(Stats(id).currentInuseSize == 0, "shared context leaked");
    Check(sceSslTerm_nid_postfix(id) == 0, "shared context term failed");
}

}

int main() {
    try {
        Lifecycle();
        AtomicFailures();
        ClientIdentity();
        SnapshotsAndBudget();
        SharedContext();
        std::vector<std::thread> workers;
        for (int i = 0; i < 3; ++i) workers.emplace_back([] { for (int n = 0; n < 4; ++n) Lifecycle(); });
        for (auto& worker : workers) worker.join();
        std::puts("SSL load lifecycle passed");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SSL load lifecycle failed: %s\n", error.what());
        return 1;
    }
}
