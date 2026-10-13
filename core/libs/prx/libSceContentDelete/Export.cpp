#include "SceTypes.hpp"
#include "HostContent.hpp"
#include "prx/libc/include/General.hpp"
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
std::mutex stateMutex;
bool initialized = false;

void RequireInitialized() {
    if (!initialized) throw std::logic_error("Content deletion is not initialized");
}
fs::file_status Status(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error) throw std::runtime_error("Cannot inspect content deletion target");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::invalid_argument("Content deletion rejects reparse points");
#endif
    if (fs::is_symlink(status)) throw std::invalid_argument("Content deletion rejects symbolic links");
    return status;
}
fs::path Root() {
    auto root = ResolvePath_nid_no_patch("/_media");
    if (!fs::is_directory(Status(root))) throw std::invalid_argument("Invalid content library directory");
    return root;
}
std::vector<fs::path> Entries(const fs::path& path) {
    std::error_code error;
    fs::directory_iterator iterator(path, error), end;
    if (error) throw std::runtime_error("Cannot enumerate content library");
    std::vector<fs::path> entries;
    for (; iterator != end; iterator.increment(error)) {
        if (error) throw std::runtime_error("Cannot enumerate content library");
        entries.push_back(iterator->path());
    }
    if (error) throw std::runtime_error("Cannot enumerate content library");
    return entries;
}
bool IsMedia(const std::string& name) {
    return name == "content.png" || name == "content.jpg" || name == "content.gif" ||
           name == "content.mp4" || name == "content.webm";
}
void Delete(const fs::path& bundle, const std::string& requested = {}) {
    if (!fs::is_directory(Status(bundle))) throw std::invalid_argument("Content bundle is not a directory");
    const auto files = Entries(bundle);
    unsigned media = 0, thumbnails = 0, metadata = 0;
    bool found = requested.empty();
    for (const auto& file : files) {
        if (!fs::is_regular_file(Status(file))) throw std::invalid_argument("Content bundle contains a non-regular file");
        const auto name = file.filename().string();
        if (IsMedia(name)) { ++media; if (name == requested) found = true; }
        else if (name == "metadata.json") ++metadata;
        else if (name == "thumbnail.png" || name == "thumbnail.jpg" || name == "thumbnail.gif") ++thumbnails;
        else throw std::invalid_argument("Content bundle contains an unknown file");
    }
    if (media != 1 || metadata != 1 || thumbnails > 1 || !found)
        throw std::invalid_argument("Incomplete or invalid content bundle");
    std::error_code error;
    for (const auto& file : files)
        if (!fs::remove(file, error) || error) throw std::runtime_error("Cannot remove content file");
    if (!fs::remove(bundle, error) || error) throw std::runtime_error("Cannot remove content directory");
}
}

extern "C" {
int APS5_VABI sceContentDeleteInitialize(const ContentDeleteInitParam* initParam) {
    if (!initParam) throw std::invalid_argument("Missing content deletion initialization parameters");
    std::lock_guard lock(stateMutex);
    if (initialized) throw std::logic_error("Content deletion is already initialized");
    initialized = true;
    return 0;
}
int APS5_VABI sceContentDeleteByPath(const char* contentPath) {
    std::lock_guard lock(stateMutex);
    RequireInitialized();
    if (!contentPath) throw std::invalid_argument("Missing content deletion path");
    const std::string path(contentPath);
    if (!path.starts_with("/_media/") || path.find('\\') != path.npos)
        throw std::invalid_argument("Content path is outside the host media library");
    const auto relative = path.substr(8);
    const auto separator = relative.find('/');
    if (separator == relative.npos || relative.find('/', separator + 1) != relative.npos)
        throw std::invalid_argument("Expected an exported content file path");
    const auto directory = relative.substr(0, separator), file = relative.substr(separator + 1);
    if (!HostContent::Id_nid_no_patch(directory) || !IsMedia(file))
        throw std::invalid_argument("Invalid exported content path");
    Delete(Root() / directory, file);
    return 0;
}
int APS5_VABI sceContentDeleteById(std::int64_t contentId) {
    std::lock_guard lock(stateMutex);
    RequireInitialized();
    if (contentId <= 0) throw std::invalid_argument("Invalid content id");
    fs::path match;
    for (const auto& entry : Entries(Root())) {
        if (HostContent::Id_nid_no_patch(entry.filename().string()) != contentId) continue;
        if (!match.empty()) throw std::runtime_error("Ambiguous host content id");
        match = entry;
    }
    if (match.empty()) throw std::invalid_argument("Unknown host content id");
    Delete(match);
    return 0;
}
int APS5_VABI sceContentDeleteTerminate() {
    std::lock_guard lock(stateMutex);
    RequireInitialized();
    initialized = false;
    return 0;
}
}
