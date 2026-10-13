#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "tests/ModuleChecks.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

extern "C" {
int APS5_VABI sceContentDeleteInitialize(const ContentDeleteInitParam*);
int APS5_VABI sceContentDeleteTerminate();
int APS5_VABI sceContentDeleteByPath(const char*);
int APS5_VABI sceContentDeleteById(std::int64_t);
int APS5_VABI sceContentExportInit2(const ContentExportInitParam2*);
int APS5_VABI sceContentExportTerm();
int APS5_VABI sceContentExportStart();
int APS5_VABI sceContentExportFinish(int);
int APS5_VABI sceContentExportFromFileWithThumbnail(int, const ContentExportParam*, const char*, const char*, char*, std::size_t);
}
namespace fs = std::filesystem;
using ModuleTest::Require;
template<class F> void Fails(F function) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    Require(failed);
}
void* APS5_VABI Allocate(std::size_t size, void*) { return std::malloc(size); }
void APS5_VABI Release(void* data, void*) { std::free(data); }
std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(input.good());
    return {std::istreambuf_iterator<char>(input), {}};
}
struct Fixture {
    fs::path root = fs::current_path() / ("content-delete-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ContentDeleteInitParam init{};
    Fixture() {
        fs::create_directories(root / "media");
        std::ofstream(root / "source.png", std::ios::binary) << "original payload";
        std::ofstream(root / "thumb.png", std::ios::binary) << "thumbnail payload";
        AddPathAlias_nid_no_patch("/_media", (root / "media").string().c_str());
        AddPathAlias_nid_no_patch("/delete-test", root.string().c_str());
        ContentExportInitParam2 config{};
        config.malloc_func = reinterpret_cast<void*>(Allocate);
        config.free_func = reinterpret_cast<void*>(Release);
        Require(sceContentExportInit2(&config) == 0);
    }
    ~Fixture() {
        Require(sceContentExportTerm() == 0);
        RemovePathAlias_nid_no_patch("/_media");
        RemovePathAlias_nid_no_patch("/delete-test");
        Require(root.parent_path() == fs::current_path() && root.filename().string().starts_with("content-delete-test-"));
        fs::remove_all(root);
    }
    std::string Export() {
        ContentExportParam metadata{};
        std::strcpy(metadata.content_type, "image/png");
        char path[1024]{};
        const int session = sceContentExportStart();
        Require(sceContentExportFromFileWithThumbnail(session, &metadata, "/delete-test/source.png", "/delete-test/thumb.png", path, sizeof(path)) == 0);
        Require(sceContentExportFinish(session) == 0);
        return path;
    }
    fs::path Bundle(const std::string& path) { return ResolvePath_nid_no_patch(path.c_str()).parent_path(); }
    std::int64_t Id(const std::string& path) {
        const auto json = Read(Bundle(path) / "metadata.json");
        const std::string key = "\"contentId\":\"";
        const auto start = json.find(key);
        Require(start != json.npos);
        const auto id = std::stoll(json.substr(start + key.size()));
        Require(id > 0);
        return id;
    }
    void CheckSource() {
        Require(Read(root / "source.png") == "original payload");
        Require(Read(root / "thumb.png") == "thumbnail payload");
    }
};
void Lifecycle(Fixture& f) {
    const auto path = f.Export();
    Fails([&] { sceContentDeleteByPath(path.c_str()); });
    Fails([&] { sceContentDeleteById(f.Id(path)); });
    Fails([] { sceContentDeleteTerminate(); });
    Fails([] { sceContentDeleteInitialize(nullptr); });
    for (int i = 0; i < 2; ++i) {
        Require(sceContentDeleteInitialize(&f.init) == 0);
        Fails([&] { sceContentDeleteInitialize(&f.init); });
        Fails([] { sceContentDeleteByPath(nullptr); });
        Fails([] { sceContentDeleteById(0); });
        Fails([] { sceContentDeleteById(-1); });
        Require(fs::exists(f.Bundle(path)));
        Require(sceContentDeleteTerminate() == 0);
    }
}
void Path(Fixture& f) {
    Require(sceContentDeleteInitialize(&f.init) == 0);
    const auto first = f.Export(), second = f.Export();
    const auto bundle = f.Bundle(first), neighbor = f.Bundle(second);
    Require(fs::exists(bundle / "thumbnail.png"));
    Require(sceContentDeleteByPath(first.c_str()) == 0);
    Require(!fs::exists(bundle));
    Require(Read(neighbor / "content.png") == "original payload");
    Require(Read(neighbor / "thumbnail.png") == "thumbnail payload");
    Fails([&] { sceContentDeleteByPath(first.c_str()); });
    Require(sceContentDeleteByPath(second.c_str()) == 0);
    Require(fs::is_empty(f.root / "media"));
    Require(sceContentDeleteTerminate() == 0);
}
void Id(Fixture& f) {
    const auto first = f.Export(), second = f.Export();
    const auto id = f.Id(first), secondId = f.Id(second);
    Require(id != secondId);
    Require(sceContentDeleteInitialize(&f.init) == 0);
    Require(sceContentDeleteTerminate() == 0);
    Require(sceContentDeleteInitialize(&f.init) == 0);
    Fails([] { sceContentDeleteById(2); });
    Require(sceContentDeleteById(id) == 0);
    Require(!fs::exists(f.Bundle(first)) && fs::exists(f.Bundle(second)));
    Fails([&] { sceContentDeleteById(id); });
    Require(sceContentDeleteById(secondId) == 0);
    const auto legacy = f.root / "media/export-123-0";
    fs::create_directory(legacy);
    std::ofstream(legacy / "content.jpg") << "legacy";
    std::ofstream(legacy / "metadata.json") << "{}";
    Require(sceContentDeleteById(8339945704589211195LL) == 0);
    Require(!fs::exists(legacy));
    Require(sceContentDeleteTerminate() == 0);
}
void Protection(Fixture& f) {
    Require(sceContentDeleteInitialize(&f.init) == 0);
    const auto path = f.Export(), bundle = f.Bundle(path).string();
    const auto id = f.Id(path);
    const std::string rejected[] = {"", "/", "/_media", "/_media/", "/delete-test/source.png",
        "/_media/../source.png", "/_media/export-123-0/../source.png", "/_media/export-123-0/content.png/extra",
        "/_media/export-123-0\\content.png", "/_media/export-x-0/content.png", "/_media/export-1-2-3/content.png",
        path.substr(0, path.rfind('/')), path + ":stream", path.substr(0, path.rfind('/')) + "/metadata.json", bundle + "/content.png"};
    for (const auto& invalid : rejected) Fails([&] { sceContentDeleteByPath(invalid.c_str()); });
    Fails([&] { sceContentDeleteByPath((path.substr(0, path.rfind('/')) + "/content.jpg").c_str()); });
    fs::create_directories(fs::path(bundle) / "nested");
    std::ofstream(fs::path(bundle) / "nested/keep") << "untouched";
    Fails([&] { sceContentDeleteById(id); });
    Require(Read(fs::path(bundle) / "nested/keep") == "untouched");
    fs::remove(fs::path(bundle) / "nested/keep");
    fs::remove(fs::path(bundle) / "nested");
    std::ofstream(fs::path(bundle) / "extra.txt") << "untouched";
    Fails([&] { sceContentDeleteByPath(path.c_str()); });
    Require(Read(fs::path(bundle) / "content.png") == "original payload");
    fs::remove(fs::path(bundle) / "extra.txt");
    fs::rename(fs::path(bundle) / "metadata.json", f.root / "metadata.backup");
    Fails([&] { sceContentDeleteById(id); });
    Require(fs::exists(fs::path(bundle) / "content.png"));
    fs::rename(f.root / "metadata.backup", fs::path(bundle) / "metadata.json");
    const auto linked = f.Export();
    const auto linkedFile = f.Bundle(linked) / "content.png";
    fs::remove(linkedFile);
    fs::create_hard_link(f.root / "source.png", linkedFile);
    Require(sceContentDeleteByPath(linked.c_str()) == 0);
    f.CheckSource();
#ifdef _WIN32
    const auto locked = f.Export();
    const auto lockedFile = f.Bundle(locked) / "content.png";
    const auto handle = CreateFileW(lockedFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(handle != INVALID_HANDLE_VALUE);
    Fails([&] { sceContentDeleteByPath(locked.c_str()); });
    Require(Read(lockedFile) == "original payload");
    Require(CloseHandle(handle) != 0);
#endif
    fs::create_directory(f.root / "media/.pending-123-0");
    std::ofstream(f.root / "media/.pending-123-0/content.png") << "pending";
    Require(sceContentDeleteById(id) == 0);
    Require(Read(f.root / "media/.pending-123-0/content.png") == "pending");
    Require(sceContentDeleteTerminate() == 0);
}
void Links(Fixture& f) {
    Require(sceContentDeleteInitialize(&f.init) == 0);
    const auto original = f.Export();
    const auto target = f.Bundle(original);
    const auto link = f.root / "media/export-123-0";
#ifdef _WIN32
    fs::create_directory(link);
    const auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Require(handle != INVALID_HANDLE_VALUE);
    struct Reparse {
        DWORD tag;
        WORD length, reserved, substituteOffset, substituteLength, printOffset, printLength;
        wchar_t paths[4096];
    } data{};
    const auto substitute = std::wstring(L"\\??\\") + target.native();
    const auto print = target.native();
    Require(substitute.size() + print.size() + 2 < 4096);
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.printOffset = data.substituteLength + sizeof(wchar_t);
    data.printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
    data.length = 8 + data.printOffset + data.printLength + sizeof(wchar_t);
    std::memcpy(data.paths, substitute.c_str(), data.substituteLength + sizeof(wchar_t));
    std::memcpy(reinterpret_cast<char*>(data.paths) + data.printOffset, print.c_str(), data.printLength + sizeof(wchar_t));
    DWORD returned = 0;
    const auto created = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, data.length + 8, nullptr, 0, &returned, nullptr);
    Require(CloseHandle(handle) != 0);
    Require(created != 0);
#else
    fs::create_directory_symlink(target, link);
#endif
    Fails([] { sceContentDeleteByPath("/_media/export-123-0/content.png"); });
    Fails([] { sceContentDeleteById(8339945704589211195LL); });
    Require(Read(target / "content.png") == "original payload");
    Require(Read(target / "thumbnail.png") == "thumbnail payload");
    Require(fs::remove(link));
    Require(sceContentDeleteByPath(original.c_str()) == 0);
    Require(sceContentDeleteTerminate() == 0);
}
void Concurrent(Fixture& f) {
    Require(sceContentDeleteInitialize(&f.init) == 0);
    const auto path = f.Export(), neighbor = f.Export();
    const auto id = f.Id(path);
    std::atomic<int> success{0}, failure{0};
    std::atomic<bool> start{false};
    auto remove = [&](bool byId) {
        while (!start.load()) std::this_thread::yield();
        try { if (byId) sceContentDeleteById(id); else sceContentDeleteByPath(path.c_str()); ++success; }
        catch (const std::exception&) { ++failure; }
    };
    std::thread first(remove, true), second(remove, false);
    start = true;
    first.join(); second.join();
    Require(success == 1 && failure == 1);
    Require(!fs::exists(f.Bundle(path)));
    Require(Read(f.Bundle(neighbor) / "content.png") == "original payload");
    Require(sceContentDeleteById(f.Id(neighbor)) == 0);
    Require(sceContentDeleteTerminate() == 0);
}
int main(int argc, char** argv) {
    Require(argc == 2);
    Fixture fixture;
    const std::string mode(argv[1]);
    if (mode == "lifecycle") Lifecycle(fixture);
    else if (mode == "path") Path(fixture);
    else if (mode == "id") Id(fixture);
    else if (mode == "protection") Protection(fixture);
    else if (mode == "links") Links(fixture);
    else if (mode == "concurrent") Concurrent(fixture);
    else Require(false);
    fixture.CheckSource();
}
