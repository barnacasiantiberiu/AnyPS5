#include "prx/libSceAppContent/AppContent.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int Parameter = static_cast<int>(0x80d90002);
constexpr int Busy = static_cast<int>(0x80d90003);
constexpr int NotFound = static_cast<int>(0x80d90005);
constexpr int NoEntitlement = static_cast<int>(0x80d90007);

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

NpUnifiedEntitlementLabel Label(const char* text) {
    NpUnifiedEntitlementLabel label{};
    std::memcpy(label.data, text, 16);
    return label;
}

const auto First = Label("ADDCONT000000001");
const auto Second = Label("ADDCONT000000002");
const auto Absent = Label("ADDCONT000000099");

template<class T> struct Guarded {
    std::uint64_t before = 0x123456789abcdef0;
    T value;
    std::uint64_t after = 0xfedcba9876543210;
    Guarded() { std::memset(&value, 0x5a, sizeof(value)); }
    void Check() const {
        Require(before == 0x123456789abcdef0 && after == 0xfedcba9876543210, "output guard changed");
    }
    void Unchanged() const {
        Check();
        for (auto byte : std::as_bytes(std::span(&value, 1)))
            Require(byte == std::byte{0x5a}, "output changed on failure");
    }
};

struct Fixture {
    std::filesystem::path previous = std::filesystem::current_path();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("anyps5-addons-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture(const std::string& mode) {
        Require(std::filesystem::create_directory(root), "create temporary fixture");
        std::filesystem::current_path(root);
        if (mode == "missing") return;
        if (mode == "bad-root") {
            std::ofstream("installed") << "not a directory";
            return;
        }
        Write("installed/0/ADDCONT000000001/content.bin", "abcd");
        Write("installed/0/ADDCONT000000001/sub/data.bin", "1234567");
        Write("installed/0/ADDCONT000000002/empty.bin", "");
        Write("installed/9/ADDCONT000000001/other.bin", "different service");
        Write("installed/0/not-a-label/ignored.bin", "ignored");
        Write("installed/unrelated/ADDCONT000000001/ignored.bin", "ignored");
    }
    static void Write(const char* path, const char* data) {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        std::ofstream file(path, std::ios::binary);
        file << data;
        Require(static_cast<bool>(file), "write fixture");
    }
    ~Fixture() {
        std::filesystem::current_path(previous);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

void Queries() {
    std::uint32_t count = 99;
    Require(sceAppContentGetAddcontInfoList(0, nullptr, 0, &count) == 0 && count == 2, "count addons");
    Guarded<std::array<AppContentAddcontInfo, 3>> list;
    Require(sceAppContentGetAddcontInfoList(0, list.value.data(), 1, &count) == 0 && count == 1, "bounded list");
    Require(std::memcmp(&list.value[0].entitlement_label, &First, sizeof(First)) == 0, "sorted first label and padding");
    Require(list.value[0].status == 4, "installed status");
    for (auto byte : std::as_bytes(std::span(list.value.data() + 1, 2)))
        Require(byte == std::byte{0x5a}, "list overflow");
    Require(sceAppContentGetAddcontInfoList(0, list.value.data(), 3, &count) == 0 && count == 2, "full list");
    Require(std::memcmp(&list.value[1].entitlement_label, &Second, sizeof(Second)) == 0, "second label");
    list.Check();
    Guarded<AppContentAddcontInfo> info;
    Require(sceAppContentGetAddcontInfo(9, &First, &info.value) == 0 && info.value.status == 4, "service query");
    Require(std::memcmp(&info.value.entitlement_label, &First, sizeof(First)) == 0, "query label");
    info.Check();
    Require(sceAppContentGetAddcontInfoList(7, nullptr, 0, &count) == 0 && count == 0, "service isolation");
    Require(sceAppContentGetAddcontInfoList(0, list.value.data(), 1, nullptr) == 0, "optional count");
}

void Progress() {
    Guarded<AppContentAddcontDownloadProgress> progress;
    Require(sceAppContentAddcontEnqueueDownload(0, &First) == 0, "already installed request");
    Require(sceAppContentAddcontEnqueueDownload(0, &First) == 0, "repeat installed request");
    Require(sceAppContentGetAddcontDownloadProgress(0, &First, &progress.value) == 0, "progress query");
    Require(progress.value.dataSize == 11 && progress.value.downloadedSize == 11, "recursive byte progress");
    progress.Check();
    Require(sceAppContentGetAddcontDownloadProgress(0, &Second, &progress.value) == 0, "empty addon");
    Require(progress.value.dataSize == 0 && progress.value.downloadedSize == 0, "empty size");
    Require(sceAppContentGetAddcontDownloadProgress(9, &First, &progress.value) == 0, "other service progress");
    Require(progress.value.dataSize == 17 && progress.value.downloadedSize == 17, "service byte isolation");
    Fixture::Write("installed/0/ADDCONT000000001/new.bin", "abc");
    Require(sceAppContentGetAddcontDownloadProgress(0, &First, &progress.value) == 0 && progress.value.dataSize == 14,
        "progress uses current files");
}

void Mounts() {
    Guarded<AppContentMountPoint> mount;
    Require(sceAppContentAddcontMount(0, &First, &mount.value) == 0, "mount addon");
    mount.Check();
    const auto path = std::string(mount.value.data) + "/sub/data.bin";
    std::ifstream file(ResolvePath_nid_no_patch(path.c_str()), std::ios::binary);
    std::string contents;
    file >> contents;
    Require(contents == "1234567", "mounted file content");
    file.close();
    Guarded<AppContentMountPoint> duplicate;
    Require(sceAppContentAddcontMount(0, &First, &duplicate.value) == Busy, "duplicate mount");
    duplicate.Unchanged();
    Require(sceAppContentAddcontMount(9, &First, &duplicate.value) == 0, "mount other service");
    Require(std::strcmp(mount.value.data, duplicate.value.data) != 0, "distinct mount paths");
    Require(sceAppContentAddcontUnmount(&mount.value) == 0, "unmount");
    bool rejected = false;
    try { ResolvePath_nid_no_patch(path.c_str()); } catch (const std::filesystem::filesystem_error&) { rejected = true; }
    Require(rejected, "unmounted path blocked");
    Require(std::filesystem::exists("installed/0/ADDCONT000000001/sub/data.bin"), "unmount preserves files");
    Require(sceAppContentAddcontUnmount(&mount.value) == NotFound, "repeat unmount");
    Require(sceAppContentAddcontMount(0, &First, &mount.value) == 0, "remount");
    Require(std::filesystem::is_regular_file(ResolvePath_nid_no_patch((std::string(mount.value.data) + "/content.bin").c_str())),
        "remounted data");
    Require(sceAppContentAddcontUnmount(&mount.value) == 0 && sceAppContentAddcontUnmount(&duplicate.value) == 0, "cleanup mounts");
}

void Invalid() {
    Guarded<AppContentAddcontDownloadProgress> progress;
    Require(sceAppContentGetAddcontDownloadProgress(0, &Absent, &progress.value) == NoEntitlement, "absent progress");
    Require(sceAppContentGetAddcontDownloadProgress(7, &First, &progress.value) == NoEntitlement, "wrong service progress");
    Require(sceAppContentGetAddcontDownloadProgress(0, nullptr, &progress.value) == Parameter, "null label");
    Require(sceAppContentGetAddcontDownloadProgress(0, &First, nullptr) == Parameter, "null progress");
    progress.Unchanged();
    Require(sceAppContentAddcontEnqueueDownload(0, &Absent) == NoEntitlement, "no offline download source");
    Require(sceAppContentAddcontEnqueueDownload(0, nullptr) == Parameter, "null download label");
    auto malformed = First;
    malformed.data[16] = 'X';
    Require(sceAppContentAddcontEnqueueDownload(0, &malformed) == Parameter, "unterminated label");
    malformed = Label("../DCONT00000001");
    Require(sceAppContentAddcontEnqueueDownload(0, &malformed) == Parameter, "path traversal label");
    Guarded<AppContentAddcontInfo> info;
    Require(sceAppContentGetAddcontInfo(0, &Absent, &info.value) == NoEntitlement, "absent info");
    Require(sceAppContentGetAddcontInfo(0, &malformed, &info.value) == Parameter, "malformed info label");
    info.Unchanged();
    Guarded<AppContentMountPoint> mount;
    Require(sceAppContentAddcontMount(0, &Absent, &mount.value) == NotFound, "absent mount");
    Require(sceAppContentAddcontMount(0, &malformed, &mount.value) == Parameter, "malformed mount label");
    mount.Unchanged();
    Require(sceAppContentAddcontUnmount(&mount.value) == Parameter, "unterminated mount");
    std::uint32_t count = 99;
    Require(sceAppContentGetAddcontInfoList(0, nullptr, 5, &count) == 0 && count == 2, "null list count query");
}

void Concurrent() {
    std::atomic<bool> passed{true};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 6; ++worker) workers.emplace_back([&] {
        try {
            for (int repeat = 0; repeat < 30; ++repeat) {
                AppContentAddcontDownloadProgress progress{};
                std::uint32_t count = 0;
                if (sceAppContentGetAddcontDownloadProgress(0, &First, &progress) != 0 || progress.dataSize != 11 ||
                    sceAppContentGetAddcontInfoList(0, nullptr, 0, &count) != 0 || count != 2) passed = false;
                AppContentMountPoint mount{};
                const auto result = sceAppContentAddcontMount(0, &First, &mount);
                if (result == 0) {
                    if (sceAppContentAddcontUnmount(&mount) != 0) passed = false;
                } else if (result != Busy) passed = false;
            }
        } catch (...) { passed = false; }
    });
    for (auto& worker : workers) worker.join();
    Require(passed, "concurrent registry operations");
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "expected mode");
        const std::string mode = argv[1];
        Fixture fixture(mode);
        if (mode == "queries") Queries();
        else if (mode == "progress") Progress();
        else if (mode == "mounts") Mounts();
        else if (mode == "invalid") Invalid();
        else if (mode == "concurrent") Concurrent();
        else if (mode == "removed") {
            AppContentAddcontInfo info{};
            Require(sceAppContentGetAddcontInfo(0, &First, &info) == 0, "load catalog before removal");
            std::filesystem::rename("installed/0/ADDCONT000000001", "removed-addon");
            Guarded<AppContentAddcontDownloadProgress> progress;
            bool rejected = false;
            try { sceAppContentGetAddcontDownloadProgress(0, &First, &progress.value); }
            catch (const std::exception&) { rejected = true; }
            Require(rejected, "removed data must not report completion");
            progress.Unchanged();
            Guarded<AppContentMountPoint> mount;
            rejected = false;
            try { sceAppContentAddcontMount(0, &First, &mount.value); }
            catch (const std::exception&) { rejected = true; }
            Require(rejected, "removed data must not mount");
            mount.Unchanged();
            std::filesystem::rename("removed-addon", "installed/0/ADDCONT000000001");
            Require(sceAppContentAddcontMount(0, &First, &mount.value) == 0, "failed mount does not occupy state");
            Require(sceAppContentAddcontUnmount(&mount.value) == 0, "cleanup recovered mount");
        }
        else if (mode == "missing") {
            std::uint32_t count = 99;
            Require(sceAppContentGetAddcontInfoList(0, nullptr, 0, &count) == 0 && count == 0, "missing root list");
            Require(sceAppContentAddcontEnqueueDownload(0, &First) == NoEntitlement, "missing root download");
            Guarded<AppContentAddcontDownloadProgress> progress;
            Require(sceAppContentGetAddcontDownloadProgress(0, &First, &progress.value) == NoEntitlement, "missing root progress");
            progress.Unchanged();
        } else if (mode == "bad-root") {
            bool rejected = false;
            std::uint32_t count = 99;
            try { sceAppContentGetAddcontInfoList(0, nullptr, 0, &count); }
            catch (const std::exception&) { rejected = true; }
            Require(rejected && count == 99, "malformed configuration must fail");
            std::filesystem::remove("installed");
            Fixture::Write("installed/0/ADDCONT000000001/data.bin", "recovered");
            Require(sceAppContentGetAddcontInfoList(0, nullptr, 0, &count) == 0 && count == 1, "initialization retries after failure");
        } else throw std::runtime_error("unknown mode");
        std::cout << mode << ": PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
