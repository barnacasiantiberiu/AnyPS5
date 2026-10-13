#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "tests/ModuleChecks.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using Provide = int (APS5_VABI*)(void**, std::size_t*, void*);
extern "C" {
int APS5_VABI sceContentExportInit2(const ContentExportInitParam2*);
int APS5_VABI sceContentExportTerm();
int APS5_VABI sceContentExportStart();
int APS5_VABI sceContentExportFinish(int);
int APS5_VABI sceContentExportGetProgress(int);
int APS5_VABI sceContentExportCancel(int);
int APS5_VABI sceContentExportFromFile(int, const ContentExportParam*, const char*, char*, std::size_t);
int APS5_VABI sceContentExportFromData(int, const ContentExportParam*, std::size_t, Provide, void*, char*, std::size_t);
int APS5_VABI sceContentExportFromFileWithThumbnail(int, const ContentExportParam*, const char*, const char*, char*, std::size_t);
int APS5_VABI sceContentExportFromDataWithThumbnail(int, const ContentExportParam*, const ContentExportCallbackParam*, const char*, const ContentExportCallbackParam*, char*, std::size_t);
}
using ModuleTest::Require;
namespace fs = std::filesystem;
template<class F> void Fails(F function) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    Require(failed);
}
struct Allocator {
    std::mutex mutex;
    std::set<void*> live;
    bool fail = false;
    int allocated = 0, released = 0;
};
void* APS5_VABI Allocate(std::size_t size, void* user) {
    auto& allocator = *static_cast<Allocator*>(user);
    std::lock_guard lock(allocator.mutex);
    Require(size == 7);
    if (allocator.fail) return nullptr;
    auto* memory = std::malloc(size);
    Require(memory && allocator.live.insert(memory).second);
    ++allocator.allocated;
    return memory;
}
void APS5_VABI Release(void* memory, void* user) {
    auto& allocator = *static_cast<Allocator*>(user);
    std::lock_guard lock(allocator.mutex);
    Require(allocator.live.erase(memory) == 1);
    ++allocator.released;
    std::free(memory);
}
ContentExportParam Metadata(const char* type = "image/png") {
    ContentExportParam param{};
    std::strcpy(param.title, "../outside/\"title");
    std::strcpy(param.comment, "line\nsecond");
    std::strcpy(param.content_type, type);
    return param;
}
std::string ReadFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::size_t EntryCount(const fs::path& root) {
    if (!fs::exists(root)) return 0;
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(root)) {
        Require(!entry.path().filename().string().starts_with(".pending-"));
        ++count;
    }
    return count;
}
struct Provider {
    std::string bytes = std::string("data\0binary\n123456789", 21);
    std::size_t offset = 0;
    int session = 0;
    int previous = 0;
    int calls = 0;
    int failure = 0;
    std::atomic<bool>* entered = nullptr;
    std::atomic<bool>* proceed = nullptr;
    bool cancel = false;
};
int APS5_VABI Supply(void** data, std::size_t* size, void* user) {
    auto& provider = *static_cast<Provider*>(user);
    ++provider.calls;
    const int progress = sceContentExportGetProgress(provider.session);
    Require(progress >= provider.previous && progress < 100);
    provider.previous = progress;
    if (provider.entered) {
        provider.entered->store(true);
        while (!provider.proceed->load()) std::this_thread::yield();
    }
    if (provider.cancel) Require(sceContentExportCancel(provider.session) == 0);
    if (provider.failure == 1) { *data = nullptr; *size = 1; return 1; }
    if (provider.failure == 2) { *data = provider.bytes.data(); *size = 1000; return 1; }
    if (provider.failure == 3) { *data = provider.bytes.data(); *size = 1; return 1; }
    if (provider.failure == 4) return -123;
    if (provider.failure == 5) return 0;
    if (provider.failure == 6) throw std::runtime_error("provider exception");
    *size = std::min<std::size_t>(11, provider.bytes.size() - provider.offset);
    *data = provider.bytes.data() + provider.offset;
    provider.offset += *size;
    return provider.offset == provider.bytes.size() ? 1 : 0;
}
void Lifecycle(const ContentExportInitParam2& config) {
    Require(sceContentExportTerm() == 0);
    Fails([] { sceContentExportStart(); });
    Fails([] { sceContentExportTerm(); });
    Fails([] { sceContentExportInit2(nullptr); });
    auto invalid = config;
    invalid.free_func = nullptr;
    Fails([&] { sceContentExportInit2(&invalid); });
    invalid = config; invalid.reserved1 = 1;
    Fails([&] { sceContentExportInit2(&invalid); });
    Require(sceContentExportInit2(&config) == 0);
    Fails([&] { sceContentExportInit2(&config); });
    std::set<int> ids;
    for (int i = 0; i < 10; ++i) {
        const int id = sceContentExportStart();
        Require(id > 0 && ids.insert(id).second);
        Require(sceContentExportGetProgress(id) == 0);
        Fails([&] { sceContentExportCancel(id); });
    }
    Fails([] { sceContentExportStart(); });
    Fails([] { sceContentExportTerm(); });
    for (int id : ids) {
        Require(sceContentExportFinish(id) == 0);
        Fails([&] { sceContentExportFinish(id); });
        Fails([&] { sceContentExportGetProgress(id); });
    }
    const int newId = sceContentExportStart();
    Require(!ids.contains(newId));
    Require(sceContentExportFinish(newId) == 0);
}
void Files(const fs::path& root) {
    const auto source = root / "source.raw", thumb = root / "preview.PNG";
    const std::string bytes("file\0payload\n0123456789", 22);
    std::ofstream(source, std::ios::binary).write(bytes.data(), bytes.size());
    std::ofstream(thumb, std::ios::binary) << "thumbnail";
    const int id = sceContentExportStart();
    std::array<char, 1026> output;
    output.fill('#');
    auto param = Metadata();
    Require(sceContentExportFromFile(id, &param, source.generic_string().c_str(), output.data() + 1, 1024) == 0);
    Require(output.front() == '#' && output.back() == '#');
    const auto exported = ResolvePath_nid_no_patch(output.data() + 1);
    Require(ReadFile(exported) == bytes && ReadFile(source) == bytes);
    Require(sceContentExportGetProgress(id) == 100);
    const auto metadata = ReadFile(exported.parent_path() / "metadata.json");
    Require(metadata.find("\\\"title") != std::string::npos && metadata.find("line\\u000asecond") != std::string::npos);
    Require(sceContentExportFromFileWithThumbnail(id, &param, source.generic_string().c_str(), thumb.generic_string().c_str(), output.data() + 1, 1024) == 0);
    const auto withThumb = ResolvePath_nid_no_patch(output.data() + 1);
    Require(withThumb != exported && ReadFile(withThumb) == bytes);
    Require(ReadFile(withThumb.parent_path() / "thumbnail.png") == "thumbnail");
    Require(EntryCount(root / "media") == 2);
    output.fill('#');
    Fails([&] { sceContentExportFromFile(id, &param, source.generic_string().c_str(), output.data(), 1); });
    Require(std::all_of(output.begin(), output.end(), [](char c) { return c == '#'; }));
    Fails([&] { sceContentExportFromFile(id, &param, "missing-content-file", output.data(), output.size()); });
    Fails([&] { sceContentExportFromFile(id, &param, root.generic_string().c_str(), output.data(), output.size()); });
    Fails([&] { sceContentExportFromFile(id, nullptr, source.generic_string().c_str(), output.data(), output.size()); });
    std::memset(param.title, 'x', sizeof(param.title));
    Fails([&] { sceContentExportFromFile(id, &param, source.generic_string().c_str(), output.data(), output.size()); });
    Require(EntryCount(root / "media") == 2);
    param = Metadata();
    AddPathAlias_nid_no_patch("/_media", source.generic_string().c_str());
    Fails([&] { sceContentExportFromFile(id, &param, source.generic_string().c_str(), output.data(), output.size()); });
    Require(ReadFile(source) == bytes);
    AddPathAlias_nid_no_patch("/_media", (root / "media").generic_string().c_str());
    Require(sceContentExportFinish(id) == 0);
}
void Callbacks(const fs::path& root, Allocator& allocator) {
    const int id = sceContentExportStart();
    std::array<char, 1024> output{};
    for (const char* type : {"image/png", "image/jpeg", "image/gif", "video/mp4", "video/webm"}) {
        auto param = Metadata(type);
        Provider provider; provider.session = id;
        Require(sceContentExportFromData(id, &param, provider.bytes.size(), Supply, &provider, output.data(), output.size()) == 0);
        Require(ReadFile(ResolvePath_nid_no_patch(output.data())) == provider.bytes && provider.calls == 2 && provider.previous > 0);
    }
    auto param = Metadata();
    Provider content, thumbnail;
    content.session = thumbnail.session = id;
    thumbnail.bytes = "thumb";
    ContentExportCallbackParam c{content.bytes.size(), reinterpret_cast<void*>(Supply), &content};
    ContentExportCallbackParam t{thumbnail.bytes.size(), reinterpret_cast<void*>(Supply), &thumbnail};
    Require(sceContentExportFromDataWithThumbnail(id, &param, &c, "image/jpeg", &t, output.data(), output.size()) == 0);
    const auto path = ResolvePath_nid_no_patch(output.data());
    Require(ReadFile(path) == content.bytes && ReadFile(path.parent_path() / "thumbnail.jpg") == "thumb");
    const auto before = EntryCount(root / "media");
    content.offset = 0;
    content.previous = 0;
    thumbnail.failure = 4;
    output.fill('#');
    Fails([&] { sceContentExportFromDataWithThumbnail(id, &param, &c, "image/jpeg", &t, output.data(), output.size()); });
    Require(EntryCount(root / "media") == before && allocator.live.empty());
    Require(std::all_of(output.begin(), output.end(), [](char ch) { return ch == '#'; }));
    Provider invalid; invalid.session = id;
    Fails([&] { sceContentExportFromData(id, &param, invalid.bytes.size(), nullptr, &invalid, output.data(), output.size()); });
    Fails([&] { sceContentExportFromData(id, &param, invalid.bytes.size(), Supply, &invalid, nullptr, output.size()); });
    Fails([&] { sceContentExportFromData(id, &param, invalid.bytes.size(), Supply, &invalid, output.data(), 0); });
    const auto badType = Metadata("application/unknown");
    Fails([&] { sceContentExportFromData(id, &badType, invalid.bytes.size(), Supply, &invalid, output.data(), output.size()); });
    Require(invalid.calls == 0 && EntryCount(root / "media") == before);
    for (int failure = 1; failure <= 6; ++failure) {
        Provider bad; bad.session = id; bad.failure = failure;
        output.fill('#');
        Fails([&] { sceContentExportFromData(id, &param, bad.bytes.size(), Supply, &bad, output.data(), output.size()); });
        Require(std::all_of(output.begin(), output.end(), [](char c) { return c == '#'; }));
        Require(EntryCount(root / "media") == before && allocator.live.empty());
    }
    Provider empty; empty.session = id; empty.bytes.clear();
    Require(sceContentExportFromData(id, &param, 0, Supply, &empty, output.data(), output.size()) == 0);
    Require(fs::file_size(ResolvePath_nid_no_patch(output.data())) == 0);
    allocator.fail = true;
    Provider rejected; rejected.session = id;
    Fails([&] { sceContentExportFromData(id, &param, rejected.bytes.size(), Supply, &rejected, output.data(), output.size()); });
    Require(rejected.calls == 0);
    allocator.fail = false;
    Require(sceContentExportFinish(id) == 0);
}
void Cancellation(const fs::path& root) {
    const int id = sceContentExportStart(), other = sceContentExportStart();
    auto param = Metadata();
    std::array<char, 1024> output;
    output.fill('#');
    std::atomic<bool> entered = false, proceed = false, failed = false;
    Provider provider; provider.session = id; provider.entered = &entered; provider.proceed = &proceed;
    std::thread worker([&] {
        try { sceContentExportFromData(id, &param, provider.bytes.size(), Supply, &provider, output.data(), output.size()); }
        catch (const std::exception&) { failed = true; }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!entered && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    Require(entered && sceContentExportGetProgress(id) == 0 && sceContentExportGetProgress(other) == 0);
    Fails([&] { sceContentExportFinish(id); });
    Fails([&] { sceContentExportFromData(id, &param, 0, Supply, &provider, output.data(), output.size()); });
    Provider second; second.session = other;
    std::array<char, 1024> secondPath{};
    Require(sceContentExportFromData(other, &param, second.bytes.size(), Supply, &second, secondPath.data(), secondPath.size()) == 0);
    Require(sceContentExportCancel(id) == 0);
    proceed = true;
    worker.join();
    Require(failed && EntryCount(root / "media") == 1);
    Require(std::all_of(output.begin(), output.end(), [](char c) { return c == '#'; }));
    Provider reentrant; reentrant.session = id; reentrant.cancel = true;
    Fails([&] { sceContentExportFromData(id, &param, reentrant.bytes.size(), Supply, &reentrant, output.data(), output.size()); });
    Require(EntryCount(root / "media") == 1);
    Provider retry; retry.session = id;
    Require(sceContentExportFromData(id, &param, retry.bytes.size(), Supply, &retry, output.data(), output.size()) == 0);
    Require(ReadFile(ResolvePath_nid_no_patch(output.data())) == retry.bytes && sceContentExportGetProgress(other) == 100);
    Require(sceContentExportFinish(id) == 0 && sceContentExportFinish(other) == 0);
}
int main(int argc, char** argv) {
    Require(argc == 2);
    const auto parent = fs::current_path();
    const auto root = parent / ("content-export-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(fs::create_directory(root));
    AddPathAlias_nid_no_patch("/_media", (root / "media").generic_string().c_str());
    Allocator allocator;
    const ContentExportInitParam2 config{reinterpret_cast<void*>(Allocate), reinterpret_cast<void*>(Release), &allocator, 7, 0, 0};
    Require(sceContentExportInit2(&config) == 0);
    if (std::strcmp(argv[1], "lifecycle") == 0) Lifecycle(config);
    else if (std::strcmp(argv[1], "files") == 0) Files(root);
    else if (std::strcmp(argv[1], "callbacks") == 0) Callbacks(root, allocator);
    else { Require(std::strcmp(argv[1], "cancel") == 0); Cancellation(root); }
    Require(sceContentExportTerm() == 0);
    Require(allocator.live.empty() && allocator.allocated == allocator.released);
    RemovePathAlias_nid_no_patch("/_media");
    Require(root.parent_path() == parent && root.filename().string().starts_with("content-export-test-"));
    fs::remove_all(root);
}
