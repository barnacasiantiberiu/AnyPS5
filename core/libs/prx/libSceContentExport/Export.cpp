#include "SceTypes.hpp"
#include "HostContent.hpp"
#include "prx/libc/include/General.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
using Provide = int (APS5_VABI*)(void**, std::size_t*, void*);
using Allocate = void* (APS5_VABI*)(std::size_t, void*);
using Release = void (APS5_VABI*)(void*, void*);
struct Session {
    bool running = false;
    std::atomic<bool> cancelled{false};
    std::atomic<int> progress{0};
};
std::mutex stateMutex;
bool initialized = false;
ContentExportInitParam2 configuration{};
std::map<int, std::shared_ptr<Session>> sessions;
int lastSession = 0;
std::atomic<std::uint64_t> nextExport{0};

void RequireInitialized() {
    if (!initialized) throw std::logic_error("Content export is not initialized");
}
std::shared_ptr<Session> Find(int id) {
    RequireInitialized();
    const auto found = sessions.find(id);
    if (found == sessions.end()) throw std::invalid_argument("Unknown content export session");
    return found->second;
}
struct Operation {
    std::shared_ptr<Session> session;
    ContentExportInitParam2 config;
    explicit Operation(int id) {
        std::lock_guard lock(stateMutex);
        session = Find(id);
        if (session->running) throw std::logic_error("Content export session is busy");
        config = configuration;
        session->cancelled = false;
        session->progress = 0;
        session->running = true;
    }
    ~Operation() {
        std::lock_guard lock(stateMutex);
        session->running = false;
    }
    void CheckCancellation() const {
        if (session->cancelled) throw std::runtime_error("Content export cancelled");
    }
};
struct Buffer {
    ContentExportInitParam2 config;
    void* data;
    std::size_t size;
    explicit Buffer(const ContentExportInitParam2& value) : config(value), data(nullptr), size(value.buffer_size ? value.buffer_size : 65536) {
        if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) throw std::invalid_argument("Content export buffer is too large");
        data = reinterpret_cast<Allocate>(config.malloc_func)(size, config.user_data);
        if (!data) throw std::bad_alloc();
    }
    ~Buffer() { reinterpret_cast<Release>(config.free_func)(data, config.user_data); }
};
template<std::size_t Size> std::string Text(const char (&value)[Size]) {
    const auto end = static_cast<const char*>(std::memchr(value, 0, Size));
    if (!end) throw std::invalid_argument("Unterminated content export metadata");
    return {value, end};
}
std::string Extension(const std::string& type) {
    if (type == "image/png") return ".png";
    if (type == "image/jpeg") return ".jpg";
    if (type == "image/gif") return ".gif";
    if (type == "video/mp4") return ".mp4";
    if (type == "video/webm") return ".webm";
    throw std::invalid_argument("Unsupported content export media type");
}
std::string Json(const std::string& value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
        else if (ch < 32) { out += "\\u00"; out += hex[ch >> 4]; out += hex[ch & 15]; }
        else out += ch;
    }
    return out + '"';
}
struct Source {
    fs::path path;
    Provide callback = nullptr;
    void* user = nullptr;
    std::uint64_t length = 0;
};
Source FileSource(const char* path) {
    if (!path || !*path) throw std::invalid_argument("Missing content export source path");
    auto native = ResolvePath_nid_no_patch(path);
    std::error_code error;
    if (!fs::is_regular_file(native, error)) throw std::invalid_argument("Content export source is not a readable regular file");
    const auto length = fs::file_size(native, error);
    if (error) throw std::runtime_error("Cannot measure content export source");
    return {native, nullptr, nullptr, length};
}
Source CallbackSource(const ContentExportCallbackParam* param) {
    if (!param || !param->func) throw std::invalid_argument("Missing content export provider");
    return {{}, reinterpret_cast<Provide>(param->func), param->user_data, param->content_length};
}
void Copy(const Source& source, const fs::path& destination, Buffer& buffer, Operation& operation,
          std::uint64_t total, std::uint64_t& transferred) {
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot create content export file");
    std::uint64_t copied = 0;
    const auto write = [&](const void* data, std::size_t bytes) {
        if (bytes > source.length - copied || (bytes && !data)) throw std::invalid_argument("Invalid content export chunk");
        auto* next = static_cast<const char*>(data);
        while (bytes) {
            operation.CheckCancellation();
            const auto size = std::min(buffer.size, bytes);
            if (next != buffer.data) std::memcpy(buffer.data, next, size);
            output.write(static_cast<const char*>(buffer.data), static_cast<std::streamsize>(size));
            if (!output) throw std::runtime_error("Content export write failed");
            next += size;
            bytes -= size;
            copied += size;
            transferred += size;
            operation.session->progress = total ? static_cast<int>(static_cast<long double>(transferred) * 99 / total) : 0;
        }
    };
    if (source.callback) {
        for (;;) {
            operation.CheckCancellation();
            void* data = nullptr;
            std::size_t size = 0;
            const int status = source.callback(&data, &size, source.user);
            operation.CheckCancellation();
            if (status != 0 && status != 1) throw std::runtime_error("Content export provider failed");
            if (!size && status == 0) throw std::runtime_error("Content export provider made no progress");
            write(data, size);
            if (status == 1) break;
        }
    } else {
        std::ifstream input(source.path, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read content export source");
        while (copied < source.length) {
            operation.CheckCancellation();
            const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(source.length - copied, buffer.size));
            input.read(static_cast<char*>(buffer.data), static_cast<std::streamsize>(size));
            if (input.gcount() != static_cast<std::streamsize>(size)) throw std::runtime_error("Content export source changed or read failed");
            write(buffer.data, size);
        }
        if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Content export source grew during copy");
    }
    if (copied != source.length) throw std::runtime_error("Content export length does not match provider data");
    output.close();
    if (!output) throw std::runtime_error("Content export close failed");
}
struct Staging {
    fs::path path;
    bool published = false;
    ~Staging() { if (!published) { std::error_code error; fs::remove_all(path, error); } }
};
int Export(Operation& operation, const ContentExportParam* param, const Source& source,
           const Source* thumbnail, const std::string& thumbnailType, char* output, std::size_t capacity) {
    if (!param || !output || !capacity) throw std::invalid_argument("Missing content export output or metadata");
    const auto title = Text(param->title), comment = Text(param->comment), type = Text(param->content_type);
    const auto extension = Extension(type);
    const auto thumbnailExtension = thumbnail ? Extension(thumbnailType) : std::string{};
    if (thumbnail && !thumbnailType.starts_with("image/")) throw std::invalid_argument("Thumbnail must be an image");
    if (thumbnail && thumbnail->length > UINT64_MAX - source.length) throw std::length_error("Content export size overflow");
    const auto total = source.length + (thumbnail ? thumbnail->length : 0);
    const auto root = ResolvePath_nid_no_patch("/_media");
    const auto token = std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" + std::to_string(nextExport.fetch_add(1));
    const auto directory = "export-" + token;
    const auto guestPath = "/_media/" + directory + "/content" + extension;
    if (guestPath.size() + 1 > capacity || guestPath.size() >= 1024) throw std::length_error("Content export path buffer is too small");
    Buffer buffer(operation.config);
    operation.CheckCancellation();
    std::error_code error;
    fs::create_directories(root, error);
    if (error) throw std::runtime_error("Cannot create content export media directory");
    const auto temporary = root / (".pending-" + token);
    if (!fs::create_directory(temporary, error)) throw std::runtime_error("Cannot create content export staging directory");
    Staging staging{temporary};
    std::uint64_t transferred = 0;
    Copy(source, temporary / ("content" + extension), buffer, operation, total, transferred);
    if (thumbnail) Copy(*thumbnail, temporary / ("thumbnail" + thumbnailExtension), buffer, operation, total, transferred);
    std::ofstream metadata(temporary / "metadata.json", std::ios::binary);
    metadata << "{\"title\":" << Json(title) << ",\"comment\":" << Json(comment) << ",\"contentType\":" << Json(type)
             << ",\"thumbnailType\":" << Json(thumbnailType)
             << ",\"contentId\":\"" << HostContent::Id_nid_no_patch(directory) << "\"}\n";
    metadata.close();
    if (!metadata) throw std::runtime_error("Content export metadata write failed");
    {
        std::lock_guard lock(stateMutex);
        operation.CheckCancellation();
        fs::rename(temporary, root / directory, error);
        if (error) throw std::runtime_error("Cannot publish content export");
        staging.published = true;
        std::memcpy(output, guestPath.c_str(), guestPath.size() + 1);
        operation.session->progress = 100;
    }
    return 0;
}
std::string ThumbnailType(const fs::path& path) {
    auto extension = path.extension().string();
    for (auto& ch : extension) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    throw std::invalid_argument("Unknown thumbnail file type");
}
}

extern "C" {
int APS5_VABI sceContentExportInit2(const ContentExportInitParam2* param) {
    if (!param || !param->malloc_func || !param->free_func || param->reserved0 || param->reserved1)
        throw std::invalid_argument("Invalid content export initialization parameters");
    std::lock_guard lock(stateMutex);
    if (initialized) throw std::logic_error("Content export is already initialized");
    configuration = *param;
    initialized = true;
    return 0;
}
int APS5_VABI sceContentExportStart() {
    std::lock_guard lock(stateMutex);
    RequireInitialized();
    if (sessions.size() >= 10) throw std::runtime_error("Content export session limit reached");
    if (lastSession == std::numeric_limits<int>::max()) throw std::runtime_error("Content export session ids exhausted");
    sessions.emplace(++lastSession, std::make_shared<Session>());
    return lastSession;
}
int APS5_VABI sceContentExportFinish(int id) {
    std::lock_guard lock(stateMutex);
    if (Find(id)->running) throw std::logic_error("Content export session is busy");
    sessions.erase(id);
    return 0;
}
int APS5_VABI sceContentExportGetProgress(int id) {
    std::lock_guard lock(stateMutex);
    return Find(id)->progress;
}
int APS5_VABI sceContentExportCancel(int id) {
    std::lock_guard lock(stateMutex);
    const auto session = Find(id);
    if (!session->running || session->progress == 100) throw std::logic_error("No content export is running");
    session->cancelled = true;
    return 0;
}
int APS5_VABI sceContentExportFromFile(int id, const ContentExportParam* param, const char* source, char* path, std::size_t capacity) {
    Operation operation(id);
    return Export(operation, param, FileSource(source), nullptr, {}, path, capacity);
}
int APS5_VABI sceContentExportFromData(int id, const ContentExportParam* param, std::size_t length, Provide callback,
                                      void* user, char* path, std::size_t capacity) {
    Operation operation(id);
    const ContentExportCallbackParam source{length, reinterpret_cast<void*>(callback), user};
    return Export(operation, param, CallbackSource(&source), nullptr, {}, path, capacity);
}
int APS5_VABI sceContentExportFromFileWithThumbnail(int id, const ContentExportParam* param, const char* source,
                                                  const char* thumbnailPath, char* path, std::size_t capacity) {
    Operation operation(id);
    const auto thumbnail = FileSource(thumbnailPath);
    return Export(operation, param, FileSource(source), &thumbnail, ThumbnailType(thumbnail.path), path, capacity);
}
int APS5_VABI sceContentExportFromDataWithThumbnail(int id, const ContentExportParam* param, const ContentExportCallbackParam* source,
        const char* thumbnailType, const ContentExportCallbackParam* thumbnailParam, char* path, std::size_t capacity) {
    Operation operation(id);
    if (!thumbnailType) throw std::invalid_argument("Missing thumbnail media type");
    const auto thumbnail = CallbackSource(thumbnailParam);
    return Export(operation, param, CallbackSource(source), &thumbnail, thumbnailType, path, capacity);
}
int APS5_VABI sceContentExportTerm() {
    std::lock_guard lock(stateMutex);
    RequireInitialized();
    if (!sessions.empty()) throw std::logic_error("Content export has live sessions");
    initialized = false;
    configuration = {};
    return 0;
}
}
