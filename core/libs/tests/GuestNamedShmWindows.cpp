#include "prx/libc/include/general/VabiMacros.hpp"
#include "SceTypes.hpp"
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

extern "C" {
int APS5_VABI shm_open_nid_postfix(const char*, int, int);
int APS5_VABI shm_unlink_nid_postfix(const char*);
int APS5_VABI close_nid_postfix(int);
int APS5_VABI dup_nid_postfix(int);
int APS5_VABI dup2_nid_postfix(int, int);
int APS5_VABI fcntl_nid_postfix(int, int, ...);
int APS5_VABI ftruncate_nid_postfix(int, std::int64_t);
int APS5_VABI fchmod_nid_postfix(int, int);
std::int64_t APS5_VABI fstat_nid_disambig1_nid_postfix(int, FileStat*);
std::int64_t APS5_VABI pread_nid_postfix(int, void*, std::size_t, std::int64_t);
std::int64_t APS5_VABI pwrite_nid_disambig1_nid_postfix(int, const void*, std::size_t, std::int64_t);
std::int64_t APS5_VABI read_nid_postfix(int, void*, std::size_t);
std::int64_t APS5_VABI write_nid_postfix(int, const void*, std::size_t);
std::int64_t APS5_VABI lseek_nid_postfix(int, std::int64_t, int);
std::uint16_t APS5_VABI umask_nid_postfix(std::uint16_t);
int* APS5_VABI __error_nid_postfix();
}

static constexpr int readWrite = 2, create = 0x200, truncate = 0x400, exclusive = 0x800;

static void Require(bool value, int line) {
    if (!value) throw std::runtime_error("GuestNamedShmWindows line " + std::to_string(line) +
        ", guest errno=" + std::to_string(*__error_nid_postfix()));
}

#define REQUIRE(value) Require((value), __LINE__)

class Descriptor {
public:
    explicit Descriptor(int value) : value(value) { REQUIRE(value >= 0); }
    ~Descriptor() { if (value >= 0) close_nid_postfix(value); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int Get() const { return value; }
    void Close() { REQUIRE(close_nid_postfix(value) == 0); value = -1; }
private:
    int value;
};

class Object {
public:
    explicit Object(const char* suffix) : name("/anyps5-windows-shm-" + std::to_string(GetCurrentProcessId()) +
        "-" + std::to_string(GetTickCount64()) + suffix) {}
    ~Object() { try { shm_unlink_nid_postfix(name.c_str()); } catch (...) {} }
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    int Open(int flags, int mode = 0600) const { return shm_open_nid_postfix(name.c_str(), flags, mode); }
    std::string name;
};

class Child {
public:
    Child(const char* mode, const std::string& name) {
        std::wstring executable(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        REQUIRE(length > 0 && length < executable.size());
        executable.resize(length);
        const std::string arguments = std::string(mode) + " " + name;
        auto command = L"\"" + executable + L"\" " + std::wstring(arguments.begin(), arguments.end());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        REQUIRE(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process));
        CloseHandle(process.hThread);
    }
    ~Child() {
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process.hProcess, 91);
            WaitForSingleObject(process.hProcess, 5000);
        }
        CloseHandle(process.hProcess);
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    DWORD Wait() {
        REQUIRE(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0);
        DWORD result = 0;
        REQUIRE(GetExitCodeProcess(process.hProcess, &result));
        return result;
    }
private:
    PROCESS_INFORMATION process{};
};

static void Error(const char* name, int flags, int expected) {
    *__error_nid_postfix() = 0;
    REQUIRE(shm_open_nid_postfix(name, flags, 0600) == -1);
    REQUIRE(*__error_nid_postfix() == expected);
}

static FileStat Status(int descriptor) {
    FileStat result{};
    REQUIRE(fstat_nid_disambig1_nid_postfix(descriptor, &result) == 0);
    return result;
}

static void Lifecycle() {
    Object object("/lifecycle");
    Error(object.name.c_str(), readWrite, 2);
    *__error_nid_postfix() = 73;
    Descriptor first(object.Open(readWrite | create | exclusive));
    REQUIRE(*__error_nid_postfix() == 73);
    REQUIRE(fcntl_nid_postfix(first.Get(), 1) == 1);
    REQUIRE((fcntl_nid_postfix(first.Get(), 3) & 3) == readWrite);
    REQUIRE(Status(first.Get()).st_size == 0);
    REQUIRE(pwrite_nid_disambig1_nid_postfix(first.Get(), "x", 1, 0) == 0);
    REQUIRE(ftruncate_nid_postfix(first.Get(), 16) == 0);
    REQUIRE(pwrite_nid_disambig1_nid_postfix(first.Get(), "original", 9, 0) == 9);
    Error(object.name.c_str(), readWrite | create | exclusive, 17);
    Descriptor reader(object.Open(truncate));
    REQUIRE(Status(reader.Get()).st_size == 16);
    REQUIRE((fcntl_nid_postfix(reader.Get(), 3) & 3) == 0);
    char contents[16]{};
    REQUIRE(pread_nid_postfix(reader.Get(), contents, sizeof(contents), 0) == 16);
    REQUIRE(std::memcmp(contents, "original", 9) == 0);
    for (std::size_t i = 9; i < sizeof(contents); ++i) REQUIRE(contents[i] == 0);
    REQUIRE(pwrite_nid_disambig1_nid_postfix(reader.Get(), "x", 1, 0) == -1 && *__error_nid_postfix() == 9);
    REQUIRE(write_nid_postfix(reader.Get(), "x", 1) == -1 && *__error_nid_postfix() == 9);
    REQUIRE(lseek_nid_postfix(first.Get(), 17, 0) == -1 && *__error_nid_postfix() == 22);
    REQUIRE(lseek_nid_postfix(first.Get(), 15, 0) == 15);
    REQUIRE(write_nid_postfix(first.Get(), "end", 3) == 1);
    REQUIRE(Status(first.Get()).st_size == 16);
    REQUIRE(write_nid_postfix(first.Get(), "x", 1) == 0);
    Descriptor alias(dup_nid_postfix(first.Get()));
    REQUIRE(fcntl_nid_postfix(alias.Get(), 1) == 0);
    REQUIRE(lseek_nid_postfix(alias.Get(), 2, 0) == 2);
    REQUIRE(lseek_nid_postfix(first.Get(), 0, 1) == 2);
    REQUIRE(pwrite_nid_disambig1_nid_postfix(alias.Get(), "O", 1, 0) == 1);
    REQUIRE(pread_nid_postfix(alias.Get(), contents, 1, 0) == 1 && contents[0] == 'O');
    REQUIRE(lseek_nid_postfix(first.Get(), 0, 1) == 2);
    REQUIRE(shm_unlink_nid_postfix(object.name.c_str()) == 0);
    Error(object.name.c_str(), readWrite, 2);
    Descriptor replacement(object.Open(readWrite | create | exclusive));
    REQUIRE(Status(replacement.Get()).st_size == 0);
    REQUIRE(ftruncate_nid_postfix(replacement.Get(), 16) == 0);
    REQUIRE(pwrite_nid_disambig1_nid_postfix(replacement.Get(), "new", 4, 0) == 4);
    REQUIRE(pread_nid_postfix(alias.Get(), contents, 9, 0) == 9 && std::memcmp(contents, "Original", 9) == 0);
    REQUIRE(pwrite_nid_disambig1_nid_postfix(alias.Get(), "old", 4, 0) == 4);
    REQUIRE(pread_nid_postfix(replacement.Get(), contents, 4, 0) == 4 && std::memcmp(contents, "new", 4) == 0);
    REQUIRE(fchmod_nid_postfix(alias.Get(), 0400) == 0);
    REQUIRE((Status(first.Get()).st_mode & 0777) == 0400);
    REQUIRE((Status(replacement.Get()).st_mode & 0777) == 0600);
    first.Close();
    REQUIRE(pread_nid_postfix(alias.Get(), contents, 4, 0) == 4 && std::memcmp(contents, "old", 4) == 0);
    const int target = replacement.Get();
    REQUIRE(dup2_nid_postfix(alias.Get(), target) == target);
    REQUIRE(Status(target).st_size == 16 && (Status(target).st_mode & 0777) == 0400);
    REQUIRE(pread_nid_postfix(target, contents, 4, 0) == 4 && std::memcmp(contents, "old", 4) == 0);
}

static void Permissions() {
    Object object("/permissions");
    const auto previous = umask_nid_postfix(0027);
    const int descriptor = object.Open(readWrite | create | exclusive, 07777);
    umask_nid_postfix(previous);
    Descriptor first(descriptor);
    REQUIRE((Status(first.Get()).st_mode & 07777) == 0750);
    Descriptor reader(object.Open(create, 0000));
    REQUIRE((Status(reader.Get()).st_mode & 07777) == 0750);
    REQUIRE(fchmod_nid_postfix(reader.Get(), 0000) == 0);
    REQUIRE((Status(first.Get()).st_mode & 07777) == 0000);
    REQUIRE(ftruncate_nid_postfix(first.Get(), 1) == 0);
    REQUIRE(pwrite_nid_disambig1_nid_postfix(first.Get(), "p", 1, 0) == 1);
    Descriptor rootReopen(object.Open(readWrite));
    REQUIRE((Status(rootReopen.Get()).st_mode & 0777) == 0);
    first.Close();
    reader.Close();
    rootReopen.Close();
    Descriptor persisted(object.Open(readWrite));
    REQUIRE((Status(persisted.Get()).st_mode & 0777) == 0 && Status(persisted.Get()).st_size == 1);
    Descriptor truncated(object.Open(readWrite | truncate));
    REQUIRE(Status(persisted.Get()).st_size == 0);
}

static void StatAccounting() {
    Object object("/stat");
    Descriptor descriptor(object.Open(readWrite | create | exclusive));
    auto status = Status(descriptor.Get());
    REQUIRE(status.st_size == 0 && status.st_blocks == 0 && status.st_nlink == 0);
    REQUIRE(status.st_blksize == 0x4000 && (status.st_mode & 0170000) == 0100000);
    REQUIRE(ftruncate_nid_postfix(descriptor.Get(), 0x4001) == 0);
    status = Status(descriptor.Get());
    REQUIRE(status.st_size == 0x4001 && status.st_blocks == 2 && status.st_nlink == 0);
    const auto inode = status.st_ino;
    Descriptor alias(object.Open(0));
    REQUIRE(Status(alias.Get()).st_ino == inode && Status(alias.Get()).st_blocks == 2);
    REQUIRE(shm_unlink_nid_postfix(object.name.c_str()) == 0);
    status = Status(alias.Get());
    REQUIRE(status.st_ino == inode && status.st_nlink == 0 && status.st_blocks == 2);
    REQUIRE(ftruncate_nid_postfix(descriptor.Get(), 0) == 0);
    REQUIRE(Status(alias.Get()).st_size == 0 && Status(alias.Get()).st_blocks == 0);
}

static int ChildMain(const char* mode, const char* name) {
    if (std::strcmp(mode, "exclusive") == 0) {
        const int descriptor = shm_open_nid_postfix(name, readWrite | create | exclusive, 0600);
        if (descriptor < 0) { REQUIRE(*__error_nid_postfix() == 17); return 17; }
        Descriptor owner(descriptor);
        REQUIRE(ftruncate_nid_postfix(owner.Get(), 16) == 0);
        return 0;
    }
    if (std::strcmp(mode, "create") == 0) {
        Descriptor descriptor(shm_open_nid_postfix(name, readWrite | create | exclusive, 0600));
        REQUIRE(ftruncate_nid_postfix(descriptor.Get(), 16) == 0);
        REQUIRE(pwrite_nid_disambig1_nid_postfix(descriptor.Get(), "child", 6, 0) == 6);
        return 0;
    }
    REQUIRE(std::strcmp(mode, "update") == 0);
    Descriptor descriptor(shm_open_nid_postfix(name, readWrite, 0));
    char contents[6]{};
    REQUIRE(read_nid_postfix(descriptor.Get(), contents, sizeof(contents)) == 6);
    REQUIRE(std::memcmp(contents, "child", 6) == 0);
    REQUIRE(pwrite_nid_disambig1_nid_postfix(descriptor.Get(), "other", 6, 0) == 6);
    REQUIRE(lseek_nid_postfix(descriptor.Get(), 0, 1) == 6);
    return 0;
}

static void CrossProcess() {
    Object object("/cross-process");
    Child creator("create", object.name);
    REQUIRE(creator.Wait() == 0);
    Descriptor opened(object.Open(readWrite));
    char contents[6]{};
    REQUIRE(pread_nid_postfix(opened.Get(), contents, sizeof(contents), 0) == 6 && std::memcmp(contents, "child", 6) == 0);
    Child updater("update", object.name);
    REQUIRE(updater.Wait() == 0);
    REQUIRE(pread_nid_postfix(opened.Get(), contents, sizeof(contents), 0) == 6 && std::memcmp(contents, "other", 6) == 0);
    Object competing("/exclusive");
    Child first("exclusive", competing.name);
    Child second("exclusive", competing.name);
    const auto one = first.Wait(), two = second.Wait();
    REQUIRE((one == 0 && two == 17) || (one == 17 && two == 0));
    Descriptor winner(competing.Open(readWrite));
    REQUIRE(Status(winner.Get()).st_size == 16);
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        if (argc == 3) return ChildMain(argv[1], argv[2]);
        REQUIRE(argc == 1);
        Lifecycle();
        Permissions();
        StatAccounting();
        CrossProcess();
        Error(nullptr, readWrite, 14);
        Error("", readWrite, 22);
        Error("relative", readWrite, 22);
        Error("/invalid", 1 | create, 22);
        Error("/invalid", readWrite | 0x8, 22);
        Error(std::string(1024, '/').c_str(), readWrite, 63);
        std::puts("Windows named shared-memory lifecycle, metadata, I/O and cross-process tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
