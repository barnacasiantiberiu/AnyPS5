#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <filesystem>
#include <optional>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <system_error>
#include <array>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestLinks.hpp"
#include "prx/libc/include/ApplicationHeap.hpp"
#include "prx/libc/include/GuestProcess.hpp"

namespace {
// Guest prefixes (without leading slashes) mapped to host directories, e.g. save-data mount points:
// the PS5 hands the title a short mount point ("/savedata0") whose files live under _sd/<dir name>.
struct PathAliases {
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> entries;
    std::vector<std::string> blocked;
};

PathAliases& Aliases() {
    static PathAliases aliases;
    return aliases;
}

std::string TrimSlashes(const char* path) {
    std::string s(path);
    std::size_t start = 0;
    while (start < s.size() && (s[start] == '/' || s[start] == '\\')) {
        ++start;
    }
    std::size_t end = s.size();
    while (end > start && (s[end - 1] == '/' || s[end - 1] == '\\')) {
        --end;
    }
    return s.substr(start, end - start);
}

bool SameName(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    const auto fold = [](unsigned char value) { return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value; };
    for (std::size_t i = 0; i < left.size(); ++i) if (fold(left[i]) != fold(right[i])) return false;
    return true;
}

bool HasPrefix(const std::string& path, const std::string& prefix) {
    return path.size() >= prefix.size() && SameName(std::string_view(path).substr(0, prefix.size()), prefix) &&
           (path.size() == prefix.size() || path[prefix.size()] == '/');
}

std::string PathUtf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}
std::filesystem::path ResolveHostPath(std::filesystem::path root, const std::filesystem::path& relative) {
    const auto direct = root / relative;
    std::error_code error;
    if (std::filesystem::exists(direct, error)) return direct;
    for (const auto& part : relative) {
        const auto candidate = root / part;
        const auto status = std::filesystem::symlink_status(candidate, error);
        if (std::filesystem::exists(status) || (error && error != std::errc::no_such_file_or_directory)) {
            root = candidate;
            continue;
        }
        std::optional<std::filesystem::path> matched;
        const auto name = PathUtf8(part);
        std::filesystem::directory_iterator entry(root, error), end;
        for (; !error && entry != end; entry.increment(error)) {
            if (!SameName(PathUtf8(entry->path().filename()), name)) continue;
            if (matched) throw std::runtime_error("Ambiguous case-insensitive guest path: " + candidate.string());
            matched = entry->path();
        }
        root = !error && matched ? *matched : candidate;
    }
    return root;
}

std::optional<std::filesystem::path> ResolveAlias(const std::string& guestPath) {
    const auto relative = TrimSlashes(guestPath.c_str());
    auto& aliases = Aliases();
    std::lock_guard lock(aliases.mutex);
    for (const auto& prefix : aliases.blocked) {
        if (HasPrefix(relative, prefix))
            throw std::filesystem::filesystem_error("Guest mount is unavailable", guestPath, std::make_error_code(std::errc::no_such_file_or_directory));
    }
    for (const auto& [prefix, host] : aliases.entries) {
        if (!HasPrefix(relative, prefix)) continue;
        if (relative.size() == prefix.size()) return std::filesystem::path(reinterpret_cast<const char8_t*>(host.c_str())).make_preferred();
        std::filesystem::path result = ResolveHostPath(std::filesystem::path(reinterpret_cast<const char8_t*>(host.c_str())), std::filesystem::path(reinterpret_cast<const char8_t*>(relative.c_str() + prefix.size() + 1)));
        return result.make_preferred();
    }
    return std::nullopt;
}

struct WorkingDirectory {
    std::mutex mutex;
    const std::filesystem::path root = std::filesystem::canonical(std::filesystem::current_path());
    std::filesystem::path current = root;
};
WorkingDirectory& Directories() { static WorkingDirectory state; return state; }
std::filesystem::path HostPath(WorkingDirectory& state, const std::filesystem::path& guest) {
    if (auto aliased = ResolveAlias(PathUtf8(guest.relative_path()))) return *aliased;
    return ResolveHostPath(state.root, guest.relative_path()).make_preferred();
}
std::filesystem::path Resolve(WorkingDirectory& state, const char* path, bool followFinal, unsigned& links);
std::filesystem::path FollowLinks(WorkingDirectory& state, const std::filesystem::path& path, bool followFinal, unsigned& links) {
    auto prefix = path.root_path();
    const auto relative = path.relative_path();
    for (auto part = relative.begin(); part != relative.end(); ++part) {
        prefix /= *part;
        auto next = part;
        ++next;
        if (next == relative.end() && !followFinal) return prefix;
        std::error_code error;
        const auto stored = ReadStoredGuestLink_nid_no_patch(prefix);
        if (!stored && !std::filesystem::is_symlink(std::filesystem::symlink_status(prefix, error))) continue;
        if (++links > 32) throw std::filesystem::filesystem_error("Too many symbolic links", path,
            std::make_error_code(std::errc::too_many_symbolic_link_levels));
        const auto target = stored ? std::filesystem::path(reinterpret_cast<const char8_t*>(stored->c_str())) : std::filesystem::read_symlink(prefix, error);
        if (error) throw std::filesystem::filesystem_error("Reading symbolic link", prefix, error);
        const auto guest = GuestLinks::Decode_nid_no_patch(stored ? *stored : PathUtf8(target));
        auto expanded = guest ? (guest->front() == '/' ? Resolve(state, guest->c_str(), false, links) :
            prefix.parent_path() / std::filesystem::path(reinterpret_cast<const char8_t*>(guest->c_str()))) :
            (target.is_absolute() ? target : prefix.parent_path() / target);
        for (; next != relative.end(); ++next) expanded /= *next;
        return FollowLinks(state, expanded, followFinal, links);
    }
    return prefix;
}
void RequireHostDirectory(WorkingDirectory& state, const std::filesystem::path& guest, unsigned& links) {
    const auto host = FollowLinks(state, HostPath(state, guest), true, links);
    std::error_code error;
    const auto status = std::filesystem::status(host, error);
    if (std::filesystem::is_directory(status)) return;
    if (!error) error = std::make_error_code(std::filesystem::exists(status) ? std::errc::not_a_directory : std::errc::no_such_file_or_directory);
#ifdef _WIN32
    if (error == std::errc::no_such_file_or_directory) {
        for (auto prefix = host.parent_path(); prefix.has_relative_path(); prefix = prefix.parent_path()) {
            std::error_code prefixError;
            const auto prefixStatus = std::filesystem::status(prefix, prefixError);
            if (!std::filesystem::exists(prefixStatus)) continue;
            if (!std::filesystem::is_directory(prefixStatus)) error = std::make_error_code(std::errc::not_a_directory);
            break;
        }
    }
#endif
    throw std::filesystem::filesystem_error("Guest path component is not a directory", host, error);
}
std::filesystem::path Resolve(WorkingDirectory& state, const char* path, bool followFinal, unsigned& links) {
    std::string text(path);
    for (auto& character : text) if (character == '\\') character = '/';
    std::filesystem::path input(reinterpret_cast<const char8_t*>(text.c_str()));
#ifdef _WIN32
    // Preserve the existing ability to pass explicit native drive paths.
    if (input.has_root_name()) return FollowLinks(state, input, followFinal, links);
#endif
    const auto current = std::filesystem::path("/") / state.current.lexically_relative(state.root);
    const auto relative = (input.is_absolute() ? input : current / input).relative_path();
    std::filesystem::path guest("/");
    for (auto part = relative.begin(); part != relative.end(); ++part) {
        if (*part == "..") {
            const auto parent = guest.lexically_normal();
            if (parent.has_relative_path()) RequireHostDirectory(state, parent, links);
        }
        guest /= *part;
    }
    return FollowLinks(state, HostPath(state, guest.lexically_normal()), followFinal, links);
}
#ifdef _WIN32
using WriteMark = FILETIME;
WriteMark Now() {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return now;
}
bool NotBefore(const WriteMark& time, const WriteMark& mark) {
    return CompareFileTime(&time, &mark) >= 0;
}
#else
using WriteMark = timespec;
WriteMark Now() {
    timespec now;
    clock_gettime(CLOCK_REALTIME_COARSE, &now);
    return now;
}
bool NotBefore(const WriteMark& time, const WriteMark& mark) {
    if (time.tv_sec == mark.tv_sec) return time.tv_nsec >= mark.tv_nsec;
    return time.tv_sec >= mark.tv_sec;
}
#endif
struct WrittenPathRegistry {
    std::mutex syncMutex;
    std::mutex mutex;
    std::map<std::filesystem::path, bool> dirty;
    WriteMark mark = Now();
};
WrittenPathRegistry& Written() { static WrittenPathRegistry registry; return registry; }

bool pathsPrepared = false;

bool PreparePaths(void*) {
    auto& directories = Directories();
    auto& aliases = Aliases();
    auto& written = Written();
    if (std::try_lock(directories.mutex, aliases.mutex, written.syncMutex, written.mutex) != -1) {
        errno = 35;
        return false;
    }
    pathsPrepared = true;
    return true;
}

bool ReleasePaths(void*) noexcept {
    if (!pathsPrepared) return true;
    pathsPrepared = false;
    Written().mutex.unlock();
    Written().syncMutex.unlock();
    Aliases().mutex.unlock();
    Directories().mutex.unlock();
    return true;
}

bool ParentPaths(void* context, bool) noexcept { return ReleasePaths(context); }

struct PathForkRegistration {
    PathForkRegistration() {
        const GuestForkHooks hooks{200, PreparePaths, ParentPaths, ReleasePaths, nullptr};
        if (GuestRegisterForkHooks_nid_no_patch(&hooks) != 0)
            throw std::runtime_error("libc: registering path fork hooks failed");
    }
} pathForkRegistration;
#ifdef _WIN32
std::optional<WriteMark> WriteTime(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return data.ftLastWriteTime;
    const auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return std::nullopt;
    throw std::system_error(static_cast<int>(error), std::system_category(), "Reading the write time of " + path.string());
}
void FlushPath(const std::filesystem::path& path) {
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;
        if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION) return;
        throw std::system_error(static_cast<int>(error), std::system_category(), "Opening " + path.string() + " for sync");
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> owner(handle, &CloseHandle);
    if (!FlushFileBuffers(handle))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Flushing " + path.string());
}
#else
std::optional<WriteMark> WriteTime(const std::filesystem::path& path) {
    struct stat data;
    if (::stat(path.c_str(), &data) == 0) return data.st_mtim;
    if (errno == ENOENT || errno == ENOTDIR) return std::nullopt;
    throw std::system_error(errno, std::generic_category(), "Reading the write time of " + path.string());
}
void FlushPath(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
        if (errno == ENOENT || errno == ENOTDIR || errno == EACCES) return;
        throw std::system_error(errno, std::generic_category(), "Opening " + path.string() + " for sync");
    }
    const int result = ::fsync(descriptor);
    const int error = errno;
    ::close(descriptor);
    if (result != 0 && error != EINVAL) throw std::system_error(error, std::generic_category(), "Flushing " + path.string());
}
#endif
bool ChangedSince(const std::filesystem::path& path, const WriteMark& mark) {
    const auto time = WriteTime(path);
    return time && NotBefore(*time, mark);
}
int DirectoryFailure(const std::error_code& error) {
    if (error == std::errc::permission_denied) return 13;
    if (error == std::errc::not_a_directory) return 20;
    if (error == std::errc::no_such_file_or_directory) return 2;
    if (error == std::errc::filename_too_long) return 63;
    if (error == std::errc::too_many_symbolic_link_levels) return 62;
    return 5;
}
int SetWorkingDirectory(WorkingDirectory& state, const std::filesystem::path& resolved) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(resolved, error);
    if (error) return DirectoryFailure(error);
    if (!std::filesystem::is_directory(canonical, error)) return error ? DirectoryFailure(error) : 20;
    const auto relative = canonical.lexically_relative(state.root);
    if (relative.empty() || *relative.begin() == "..") return 45;
    state.current = canonical;
    return 0;
}
}

extern "C" void AddPathAlias_nid_no_patch(const char* guestPrefix, const char* hostPath) {
    if (guestPrefix == nullptr || hostPath == nullptr) {
        APS5_INVALID_ARG_EX;
    }
    auto& aliases = Aliases();
    std::lock_guard lock(aliases.mutex);
    const auto prefix = TrimSlashes(guestPrefix);
    std::erase_if(aliases.blocked, [&](const auto& entry) { return SameName(entry, prefix); });
    for (auto& entry : aliases.entries) {
        if (SameName(entry.first, prefix)) {
            entry.second = hostPath;
            return;
        }
    }
    aliases.entries.emplace_back(prefix, hostPath);
}

extern "C" void RemovePathAlias_nid_no_patch(const char* guestPrefix) {
    if (guestPrefix == nullptr) {
        APS5_INVALID_ARG_EX;
    }
    auto& aliases = Aliases();
    std::lock_guard lock(aliases.mutex);
    const auto prefix = TrimSlashes(guestPrefix);
    std::erase_if(aliases.entries, [&](const auto& entry) { return SameName(entry.first, prefix); });
}

extern "C" void RecordWrittenPath_nid_no_patch(const std::filesystem::path& path) {
    auto& registry = Written();
    std::lock_guard lock(registry.mutex);
    registry.dirty[path.lexically_normal()] = true;
}

extern "C" std::vector<std::filesystem::path> WrittenPaths_nid_no_patch() {
    auto& registry = Written();
    std::lock_guard lock(registry.mutex);
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : registry.dirty) paths.push_back(entry.first);
    return paths;
}

extern "C" void SyncWrittenPaths_nid_no_patch() {
    auto& registry = Written();
    std::lock_guard serial(registry.syncMutex);
    const auto start = Now();
    WriteMark mark;
    std::vector<std::pair<std::filesystem::path, bool>> entries;
    {
        std::lock_guard lock(registry.mutex);
        mark = registry.mark;
        registry.mark = start;
        for (auto& [path, dirty] : registry.dirty) {
            entries.emplace_back(path, dirty);
            dirty = false;
        }
    }
    std::set<std::filesystem::path> flushed;
    std::vector<std::filesystem::path> missing;
    for (const auto& [path, dirty] : entries) {
        const auto time = WriteTime(path);
        if (!time) missing.push_back(path);
        if (!dirty && !(time && NotBefore(*time, mark))) continue;
        if (time && flushed.insert(path).second) FlushPath(path);
        for (auto ancestor = path.parent_path(); ancestor.has_relative_path() && !flushed.contains(ancestor) && ChangedSince(ancestor, mark);
             ancestor = ancestor.parent_path()) {
            flushed.insert(ancestor);
            FlushPath(ancestor);
        }
    }
    std::lock_guard lock(registry.mutex);
    for (const auto& path : missing) {
        const auto entry = registry.dirty.find(path);
        if (entry != registry.dirty.end() && !entry->second) registry.dirty.erase(entry);
    }
}

extern "C" void BlockPathAlias_nid_no_patch(const char* guestPrefix) {
    if (guestPrefix == nullptr) { APS5_INVALID_ARG_EX; }
    const auto prefix = TrimSlashes(guestPrefix);
    if (prefix.empty()) { APS5_INVALID_ARG_EX; }
    auto& aliases = Aliases();
    std::lock_guard lock(aliases.mutex);
    std::erase_if(aliases.entries, [&](const auto& entry) { return SameName(entry.first, prefix); });
    std::erase_if(aliases.blocked, [&](const auto& entry) { return SameName(entry, prefix); });
    aliases.blocked.push_back(prefix);
}

extern "C" std::filesystem::path ResolvePath_nid_no_patch(const char* path) {
    if (!path) { APS5_INVALID_ARG_EX; }
    auto& state = Directories();
    std::lock_guard lock(state.mutex);
    unsigned links = 0;
    return Resolve(state, path, true, links);
}

extern "C" std::optional<std::string> ReadStoredGuestLink_nid_no_patch(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return std::nullopt;
    const auto name = path.wstring() + GuestLinkStream;
    const auto handle = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_NAME) return std::nullopt;
        throw std::filesystem::filesystem_error("Opening guest link metadata", path, std::error_code(error, std::system_category()));
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> owner(handle, &CloseHandle);
    LARGE_INTEGER size{};
    std::array<char, 2100> bytes{};
    DWORD count = 0;
    if (!GetFileSizeEx(handle, &size) || size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(bytes.size()) ||
        !ReadFile(handle, bytes.data(), static_cast<DWORD>(size.QuadPart), &count, nullptr) || count != size.QuadPart)
        throw std::filesystem::filesystem_error("Reading guest link metadata", path, std::make_error_code(std::errc::io_error));
    std::string_view contents(bytes.data(), count);
    if (!contents.starts_with(GuestLinkMagic) || contents.size() == GuestLinkMagic.size() || contents.find('\0') != std::string_view::npos)
        throw std::filesystem::filesystem_error("Malformed guest link metadata", path, std::make_error_code(std::errc::invalid_argument));
    return std::string(contents.substr(GuestLinkMagic.size()));
#else
    (void)path;
    return std::nullopt;
#endif
}

extern "C" std::filesystem::path ResolvePathNoFollow_nid_no_patch(const char* path) {
    if (!path) { APS5_INVALID_ARG_EX; }
    auto& state = Directories();
    std::lock_guard lock(state.mutex);
    unsigned links = 0;
    return Resolve(state, path, false, links);
}

extern "C" std::filesystem::path ResolveNativeLinks_nid_no_patch(const std::filesystem::path& path, bool followFinal) {
    auto& state = Directories();
    std::lock_guard lock(state.mutex);
    unsigned links = 0;
    return FollowLinks(state, path, followFinal, links);
}

extern "C" int SetWorkingDirectory_nid_no_patch(const char* resolved) {
    if (resolved == nullptr) return 14;
    auto& state = Directories();
    std::lock_guard lock(state.mutex);
    try {
        return SetWorkingDirectory(state, std::filesystem::u8path(resolved));
    } catch (const std::bad_alloc&) {
        return 12;
    } catch (const std::filesystem::filesystem_error& error) {
        return DirectoryFailure(error.code());
    }
}

extern "C" bool FinalPathComponentIsDot_nid_no_patch(const char* path) {
    std::string_view text(path);
    while (!text.empty() && (text.back() == '/' || text.back() == '\\')) text.remove_suffix(1);
    const auto separator = text.find_last_of("/\\");
    const auto component = text.substr(separator == std::string_view::npos ? 0 : separator + 1);
    return component == ".";
}

extern "C" int APS5_VABI chdir_nid_postfix(const char* path) {
    if (!path) { errno = 14; return -1; }
    if (!*path) { errno = 2; return -1; }
    try {
        auto& state = Directories();
        std::lock_guard lock(state.mutex);
        unsigned links = 0;
        const auto resolved = Resolve(state, path, true, links);
        const int error = SetWorkingDirectory(state, resolved);
        if (error) { errno = error; return -1; }
        return 0;
    } catch (const std::bad_alloc&) { errno = 12; return -1; }
      catch (const std::filesystem::filesystem_error& error) { errno = DirectoryFailure(error.code()); return -1; }
}

extern "C" char* APS5_VABI getcwd_nid_postfix(char* buffer, std::size_t size) {
    if (buffer && size == 0) { errno = 22; return nullptr; }
    try {
        auto& state = Directories();
        std::lock_guard lock(state.mutex);
        std::error_code error;
        if (!std::filesystem::is_directory(state.current, error)) {
            errno = error ? DirectoryFailure(error) : 2; return nullptr;
        }
        const auto relative = state.current.lexically_relative(state.root);
        const auto path = relative == "." ? std::string("/") : "/" + relative.generic_string();
        const auto required = path.size() + 1;
        if ((buffer || size) && size < required) { errno = 34; return nullptr; }
        if (!buffer) buffer = static_cast<char*>(ApplicationHeapAllocate_nid_no_patch(size ? size : required));
        if (!buffer) { errno = 12; return nullptr; }
        std::memcpy(buffer, path.c_str(), required);
        return buffer;
    } catch (const std::bad_alloc&) { errno = 12; return nullptr; }
      catch (const std::filesystem::filesystem_error& error) { errno = DirectoryFailure(error.code()); return nullptr; }
}

#ifdef _WIN32
extern "C" int WindowsOpenError_nid_no_patch(unsigned long error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_FILENAME_EXCED_RANGE:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT:
    case ERROR_NETWORK_ACCESS_DENIED:
    case ERROR_CANNOT_MAKE:
    case ERROR_DRIVE_LOCKED:
        return EACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return EEXIST;
    case ERROR_TOO_MANY_OPEN_FILES:
        return EMFILE;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    case ERROR_DISK_FULL:
        return ENOSPC;
    default:
        return EINVAL;
    }
}
#endif

extern "C" void NotImplemented_nid_no_patch(const char* funcName) {
    throw std::runtime_error(std::string(funcName) + " not implemented");
}
