#include "tests/ModuleChecks.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"
#include "SceTypes.hpp"
#include "prx/libc/include/GuestDirectory.hpp"
#include <cerrno>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

extern "C" {
void* APS5_VABI opendir_nid_postfix(const char*);
GuestDirectoryEntry* APS5_VABI readdir_nid_postfix(void*);
int APS5_VABI closedir_nid_postfix(void*);
struct FileStream;
FileStream* APS5_VABI fopen_nid_postfix(const char*, const char*);
int APS5_VABI symlink_nid_postfix(const char*, const char*);
std::int64_t APS5_VABI readlink_nid_postfix(const char*, char*, std::size_t);
std::int64_t APS5_VABI readlinkat_nid_postfix(int, const char*, char*, std::size_t);
int APS5_VABI open_nid_postfix(const char*, int, int);
int APS5_VABI openat_nid_postfix(int, const char*, int, ...);
int APS5_VABI close_nid_postfix(int);
std::int64_t APS5_VABI sceKernelRead(int, void*, std::size_t);
int APS5_VABI unlink_nid_postfix(const char*);
int APS5_VABI rename_nid_postfix(const char*, const char*);
int APS5_VABI remove_nid_postfix(const char*);
int APS5_VABI access_nid_postfix(const char*, int);
int APS5_VABI lstat_nid_postfix(const char*, FileStat*);
int APS5_VABI stat_nid_postfix(const char*, FileStat*);
int APS5_VABI getdents_nid_postfix(int, char*, int);
int APS5_VABI mkdirat_nid_postfix(int, const char*, std::uint16_t);
int* APS5_VABI __error_nid_postfix();
void AddPathAlias_nid_no_patch(const char*, const char*);
void RemovePathAlias_nid_no_patch(const char*);
void BlockPathAlias_nid_no_patch(const char*);
}

using ModuleTest::Require;
namespace fs = std::filesystem;

std::string Read(const std::string& path) {
    const int fd = open_nid_postfix(path.c_str(), SCE_KERNEL_O_RDONLY, 0);
    Require(fd >= 0);
    std::array<char, 64> bytes{};
    const auto size = sceKernelRead(fd, bytes.data(), bytes.size());
    Require(size >= 0 && close_nid_postfix(fd) == 0);
    return std::string(bytes.data(), static_cast<std::size_t>(size));
}

void Link(const std::string& target, const fs::path& path) {
    Require(symlink_nid_postfix(target.c_str(), path.generic_string().c_str()) == 0);
}

void CheckTarget(const fs::path& link, const std::string& expected) {
    std::array<char, 1100> bytes;
    const auto path = link.generic_string();
    for (std::size_t capacity : {std::size_t(0), std::size_t(1), expected.size() - 1, expected.size(), bytes.size()}) {
        bytes.fill('#');
        const auto count = std::min(capacity, expected.size());
        Require(readlink_nid_postfix(path.c_str(), bytes.data(), capacity) == static_cast<std::int64_t>(count));
        Require(std::memcmp(bytes.data(), expected.data(), count) == 0);
        for (auto i = count; i < bytes.size(); ++i) Require(bytes[i] == '#');
    }
    Require(readlink_nid_postfix(path.c_str(), nullptr, 0) == 0);
    Require(readlink_nid_postfix(path.c_str(), nullptr, 1) == -1 && *__error_nid_postfix() == 14);
}

void Roundtrip(const fs::path& root) {
    const auto target = "/" + (root / "file").generic_string();
    std::ofstream(root / "file") << "original";
    Link(target, root / "link");
    Require(Read((root / "link").generic_string()) == "original");
    FileStat info{};
    Require(lstat_nid_postfix((root / "link").generic_string().c_str(), &info) == 0);
    Require((info.st_mode & 0170000) == 0120000 && info.st_size == static_cast<std::int64_t>(target.size()));
    Require(stat_nid_postfix((root / "link").generic_string().c_str(), &info) == 0 && info.st_size == 8 && (info.st_mode & 0170000) == 0100000);
    CheckTarget(root / "link", target);
    Require(symlink_nid_postfix(target.c_str(), (root / "link").generic_string().c_str()) == -1 && *__error_nid_postfix() == 17);
    Require(rename_nid_postfix((root / "link").generic_string().c_str(), (root / "nested/moved").generic_string().c_str()) == 0);
    CheckTarget(root / "nested/moved", target);
    Require(Read((root / "nested/moved").generic_string()) == "original");
    Require(unlink_nid_postfix((root / "nested/moved").generic_string().c_str()) == 0);
    Require(fs::file_size(root / "file") == 8 && !fs::is_symlink(fs::symlink_status(root / "nested/moved")));
    const auto raw = "/" + root.generic_string() + "//nested/../file";
    Link(raw, root / "raw");
    CheckTarget(root / "raw", raw);
    Require(Read((root / "raw").generic_string()) == "original");
    Link(target, root / "removed");
    Require(remove_nid_postfix((root / "removed").generic_string().c_str()) == 0 && fs::file_size(root / "file") == 8);
    const auto unicode = "/" + root.generic_string() + "/a\xc3\xa7\xc3\xa3o";
    Link(unicode, root / "unicode");
    CheckTarget(root / "unicode", unicode);
    const auto unicodeFile = root / fs::path(u8"a\u00e7\u00e3o");
#ifdef _WIN32
    const auto handle = CreateFileW(unicodeFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    Require(handle != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    Require(WriteFile(handle, "unicode", 7, &written, nullptr) && written == 7);
    Require(CloseHandle(handle) != 0);
#else
    std::ofstream(unicodeFile) << "unicode";
#endif
    Require(Read((root / "unicode").generic_string()) == "unicode");
    Link("a\xc3\xa7\xc3\xa3o", root / "unicode-relative");
    Require(Read((root / "unicode-relative").generic_string()) == "unicode");
    Link("nested//../file", root / "relative-raw");
    CheckTarget(root / "relative-raw", "nested//../file");
    Link("/" + std::string(1022, 'x'), root / "long");
    CheckTarget(root / "long", "/" + std::string(1022, 'x'));
    Link(".anyps5-absolute-v1/not-hex", root / "reserved");
    CheckTarget(root / "reserved", ".anyps5-absolute-v1/not-hex");
}

void Resolution(const fs::path& root) {
    fs::create_directory(root / "a");
    fs::create_directory(root / "b");
    std::ofstream(root / "a/file") << "first";
    std::ofstream(root / "b/file") << "second";
    AddPathAlias_nid_no_patch("/symlink-mount", fs::absolute(root / "a").generic_string().c_str());
    Link("/symlink-mount/file", root / "mounted");
    Require(Read((root / "mounted").generic_string()) == "first");
    AddPathAlias_nid_no_patch("/symlink-mount", fs::absolute(root / "b").generic_string().c_str());
    Require(Read((root / "mounted").generic_string()) == "second");
    CheckTarget(root / "mounted", "/symlink-mount/file");
    BlockPathAlias_nid_no_patch("/symlink-mount");
    Require(open_nid_postfix((root / "mounted").generic_string().c_str(), 0, 0) == -1 && *__error_nid_postfix() == 2);
    AddPathAlias_nid_no_patch("/symlink-mount", fs::absolute(root / "a").generic_string().c_str());
    Link("/symlink-mount", root / "directory");
    Require(Read((root / "directory/file").generic_string()) == "first");
    Require(access_nid_postfix((root / "directory/file").generic_string().c_str(), 0) == 0);
    const int dir = open_nid_postfix(root.generic_string().c_str(), SCE_KERNEL_O_RDONLY | SCE_KERNEL_O_DIRECTORY, 0);
    Require(dir >= 0);
    std::array<char, 4096> entries{};
    const auto count = getdents_nid_postfix(dir, entries.data(), entries.size());
    Require(count > 0);
    bool foundLink = false;
    for (int offset = 0; offset < count;) {
        std::uint16_t size;
        std::memcpy(&size, entries.data() + offset + 4, sizeof(size));
        Require(size >= 8 && offset + size <= count);
        if (std::strcmp(entries.data() + offset + 8, "directory") == 0) {
            Require(entries[offset + 6] == 10);
            foundLink = true;
        }
        offset += size;
    }
    Require(foundLink);
    void* listing = opendir_nid_postfix(root.generic_string().c_str());
    Require(listing != nullptr);
    foundLink = false;
    while (auto* entry = readdir_nid_postfix(listing)) {
        if (std::strcmp(entry->name, "directory") == 0) { Require(entry->type == 10); foundLink = true; }
    }
    Require(foundLink && closedir_nid_postfix(listing) == 0);
    Require(open_nid_postfix((root / "directory").generic_string().c_str(), SCE_KERNEL_O_DIRECTORY | SCE_KERNEL_O_NOFOLLOW, 0) == -1 && *__error_nid_postfix() == 31);
    Require(mkdirat_nid_postfix(dir, "directory/created", 0700) == 0 && fs::is_directory(root / "a/created"));
    const int file = openat_nid_postfix(dir, "directory/file", SCE_KERNEL_O_RDONLY);
    Require(file >= 0);
    char bytes[5];
    Require(sceKernelRead(file, bytes, sizeof(bytes)) == 5 && std::memcmp(bytes, "first", 5) == 0);
    Require(close_nid_postfix(file) == 0);
    char text[64]{};
    Require(readlinkat_nid_postfix(dir, "mounted", text, sizeof(text)) == 19 && std::memcmp(text, "/symlink-mount/file", 19) == 0);
    Require(close_nid_postfix(dir) == 0);
    Link("mounted", root / "relative");
    Require(Read((root / "relative").generic_string()) == "first");
    Link("/" + (root / "relative").generic_string(), root / "chain");
    Require(Read((root / "chain").generic_string()) == "first");
    RemovePathAlias_nid_no_patch("/symlink-mount");
}

void Errors(const fs::path& root) {
    const auto missing = "/" + (root / "missing/file").generic_string();
    Link(missing, root / "dangling");
    CheckTarget(root / "dangling", missing);
    Require(open_nid_postfix((root / "dangling").generic_string().c_str(), 0, 0) == -1 && *__error_nid_postfix() == 2);
    Require(open_nid_postfix((root / "dangling").generic_string().c_str(), SCE_KERNEL_O_WRONLY | SCE_KERNEL_O_CREAT | SCE_KERNEL_O_EXCL, 0600) == -1 && *__error_nid_postfix() == 17);
    Require(open_nid_postfix((root / "dangling").generic_string().c_str(), SCE_KERNEL_O_NOFOLLOW, 0) == -1 && *__error_nid_postfix() == 31);
    Require(fopen_nid_postfix((root / "dangling").generic_string().c_str(), "wx") == nullptr && errno == EEXIST);
    fs::create_directory(root / "missing");
    std::ofstream(root / "missing/file") << "late";
    Require(Read((root / "dangling").generic_string()) == "late");
    Link("/" + (root / "self").generic_string(), root / "self");
    Require(open_nid_postfix((root / "self").generic_string().c_str(), 0, 0) == -1 && *__error_nid_postfix() == 62);
    CheckTarget(root / "self", "/" + (root / "self").generic_string());
    Link("/" + (root / "cycle-b").generic_string(), root / "cycle-a");
    Link("cycle-a", root / "cycle-b");
    Require(open_nid_postfix((root / "cycle-a").generic_string().c_str(), 0, 0) == -1 && *__error_nid_postfix() == 62);
    const auto invalid = (root / "invalid").generic_string();
    Require(symlink_nid_postfix(("/" + std::string(1023, 'x')).c_str(), invalid.c_str()) == -1 && *__error_nid_postfix() == 63);
    Require(!fs::exists(fs::symlink_status(invalid)));
    std::ofstream(root / "plain") << "kept";
    Require(symlink_nid_postfix("/target", (root / "plain/link").generic_string().c_str()) == -1 && *__error_nid_postfix() == 20);
    Require(fs::file_size(root / "plain") == 4);
    Require(symlink_nid_postfix("/target", (root / "absent/link").generic_string().c_str()) == -1 && *__error_nid_postfix() == 2);
#ifdef _WIN32
    std::ofstream(root / "corrupt") << "";
    std::ofstream(fs::path((root / "corrupt").wstring() + L":AnyPS5.GuestSymlink.v1")) << "invalid";
    char output[16];
    FileStat info{};
    Require(readlink_nid_postfix((root / "corrupt").generic_string().c_str(), output, sizeof(output)) == -1 && *__error_nid_postfix() == 22);
    Require(lstat_nid_postfix((root / "corrupt").generic_string().c_str(), &info) == -1 && *__error_nid_postfix() == 22);
    Require(open_nid_postfix((root / "corrupt").generic_string().c_str(), SCE_KERNEL_O_NOFOLLOW, 0) == -1 && *__error_nid_postfix() == 22);
#endif
    Require(rename_nid_postfix((root / "self").generic_string().c_str(), (root / "self-moved").generic_string().c_str()) == 0);
    Require(unlink_nid_postfix((root / "self-moved").generic_string().c_str()) == 0);
}

void Child(const fs::path& root) {
    Require(Read((root / "persisted").generic_string()) == "persistent");
    CheckTarget(root / "persisted", "/" + (root / "file").generic_string());
}

void Persistence(const fs::path& root, const char* executable) {
    std::ofstream(root / "file") << "persistent";
    Link("/" + (root / "file").generic_string(), root / "persisted");
#ifdef _WIN32
    std::array<wchar_t, 32768> image{};
    Require(GetModuleFileNameW(nullptr, image.data(), image.size()) != 0);
    auto command = L"\"" + std::wstring(image.data()) + L"\" child " + root.wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(image.data(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0);
    CloseHandle(process.hThread);
    Require(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
    DWORD result = 1;
    Require(GetExitCodeProcess(process.hProcess, &result) != 0 && result == 0);
    CloseHandle(process.hProcess);
#else
    const auto pid = fork();
    Require(pid >= 0);
    if (!pid) { execl(executable, executable, "child", root.c_str(), nullptr); _exit(127); }
    int result = 0;
    Require(waitpid(pid, &result, 0) == pid && WIFEXITED(result) && WEXITSTATUS(result) == 0);
#endif
}

int main(int argc, char** argv) {
    Require(argc >= 2);
    if (std::strcmp(argv[1], "child") == 0) { Require(argc == 3); Child(argv[2]); return 0; }
    Require(argc == 2);
    const fs::path root("anyps5-absolute-links-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto parent = fs::current_path();
    const auto cleanup = fs::absolute(root).lexically_normal();
    Require(fs::create_directory(root));
    Require(fs::create_directory(root / "nested"));
    if (std::strcmp(argv[1], "roundtrip") == 0) Roundtrip(root);
    else if (std::strcmp(argv[1], "resolution") == 0) Resolution(root);
    else if (std::strcmp(argv[1], "errors") == 0) Errors(root);
    else { Require(std::strcmp(argv[1], "persistence") == 0); Persistence(root, argv[0]); }
    Require(cleanup.parent_path() == parent && cleanup.filename().string().starts_with("anyps5-absolute-links-"));
    fs::remove_all(cleanup);
}
