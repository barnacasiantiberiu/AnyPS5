#include "prx/libc/include/General.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"
#include "prx/libkernel/File/include/SharedMemory.hpp"
#include "prx/libc/include/CreationMask.hpp"
#include "prx/libc/include/HostDescriptorFork.hpp"
#include "prx/libkernel/File/include/File.hpp"
#include "prx/libkernel/File/include/DescriptorControl.hpp"
#include "nid/Sha1.hpp"

#include <cerrno>
#include <algorithm>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <string>
#include <cstdint>
#include <limits>
#include <optional>
#include <atomic>
#include <array>
#include <cstring>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <filesystem>
#include <random>
#include <windows.h>
#include <winternl.h>
#include "prx/libkernel/File/include/WindowsFileTime.hpp"
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/uio.h>
#include <unistd.h>
#include <cstdio>
#endif

extern "C" int* APS5_VABI __error_nid_postfix();
#ifdef _WIN32
extern "C" _invalid_parameter_handler _set_thread_local_invalid_parameter_handler(_invalid_parameter_handler);
#endif

namespace {

#ifdef _WIN32

constexpr char sharedAttribute[] = "ANYPS5.SHM.V1";
constexpr std::uint32_t sharedMagic = 0x4d485341;

struct SharedMetadata {
    std::uint32_t magic = sharedMagic;
    std::uint32_t version = 1;
    std::uint32_t mode = 0;
    std::uint32_t owner = 0;
    std::uint32_t group = 0;
};

struct SharedAttribute {
    ULONG next = 0;
    UCHAR flags = 0;
    UCHAR nameLength = sizeof(sharedAttribute) - 1;
    USHORT valueLength = sizeof(SharedMetadata);
    char bytes[sizeof(sharedAttribute) + sizeof(SharedMetadata)]{};
};

struct AttributeRequest {
    ULONG next = 0;
    UCHAR nameLength = sizeof(sharedAttribute) - 1;
    char name[sizeof(sharedAttribute)]{};
};

class OwnedHandle {
public:
    explicit OwnedHandle(HANDLE value = INVALID_HANDLE_VALUE) : value(value) {}
    ~OwnedHandle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    HANDLE Get() const { return value; }
    HANDLE Release() { const auto result = value; value = INVALID_HANDLE_VALUE; return result; }
private:
    HANDLE value;
};

class OwnedView {
public:
    explicit OwnedView(void* value) : value(value) {}
    ~OwnedView() { if (value != nullptr) UnmapViewOfFile(value); }
    OwnedView(const OwnedView&) = delete;
    OwnedView& operator=(const OwnedView&) = delete;
    void* Get() const { return value; }
private:
    void* value;
};

void IgnoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) {}

HANDLE NativeHandle(int descriptor) {
    const auto previous = _set_thread_local_invalid_parameter_handler(IgnoreInvalidParameter);
    const auto result = ::_get_osfhandle(descriptor);
    _set_thread_local_invalid_parameter_handler(previous);
    return reinterpret_cast<HANDLE>(result);
}

int WindowsError(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: errno = EEXIST; break;
    case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: errno = EACCES; break;
    case ERROR_INVALID_HANDLE: errno = EBADF; break;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: errno = ENOSPC; break;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: errno = ENOMEM; break;
    case ERROR_TOO_MANY_OPEN_FILES: errno = EMFILE; break;
    case ERROR_FILENAME_EXCED_RANGE: errno = ENAMETOOLONG; break;
    case ERROR_INVALID_PARAMETER: errno = EINVAL; break;
    default: errno = EIO; break;
    }
    return -1;
}

int NativeError(NTSTATUS status) {
    using Translate = ULONG(NTAPI*)(NTSTATUS);
    static const auto translate = reinterpret_cast<Translate>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError")));
    return WindowsError(translate == nullptr ? ERROR_GEN_FAILURE : translate(status));
}

ACCESS_MASK NativeAccess(HANDLE handle);

bool ReadMetadata(HANDLE handle, SharedMetadata& metadata) {
    if (handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_DISK) return false;
    using Query = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, BOOLEAN, PVOID, ULONG, PULONG, BOOLEAN);
    static const auto query = reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryEaFile")));
    if (query == nullptr) NotImplemented_nid_no_patch("shared memory requires NtQueryEaFile");
    AttributeRequest request;
    std::memcpy(request.name, sharedAttribute, sizeof(sharedAttribute));
    SharedAttribute attribute;
    IO_STATUS_BLOCK status{};
    const auto result = query(handle, &status, &attribute, sizeof(attribute), TRUE, &request,
        offsetof(AttributeRequest, name) + sizeof(sharedAttribute), nullptr, TRUE);
    if (result < 0) {
        const auto code = static_cast<std::uint32_t>(result);
        if (code == 0xc0000051u || code == 0xc0000052u || code == 0x80000012u ||
            code == 0xc000004fu || code == 0xc0000010u) return false;
        if (code == 0xc0000022u && (NativeAccess(handle) & FILE_READ_EA) == 0) return false;
        throw std::runtime_error("shared memory: querying file metadata failed, status=" + std::to_string(result));
    }
    if (attribute.valueLength == 0) return false;
    if (status.Information < offsetof(SharedAttribute, bytes) + sizeof(sharedAttribute) + sizeof(metadata) ||
        attribute.nameLength != sizeof(sharedAttribute) - 1 || attribute.valueLength != sizeof(metadata) ||
        std::memcmp(attribute.bytes, sharedAttribute, sizeof(sharedAttribute)) != 0)
        throw std::runtime_error("shared memory: malformed file metadata");
    std::memcpy(&metadata, attribute.bytes + sizeof(sharedAttribute), sizeof(metadata));
    if (metadata.magic != sharedMagic || metadata.version != 1)
        throw std::runtime_error("shared memory: unsupported file metadata format");
    return true;
}

int WriteMetadata(HANDLE handle, const SharedMetadata& metadata) {
    using Set = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG);
    static const auto set = reinterpret_cast<Set>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetEaFile")));
    if (set == nullptr) NotImplemented_nid_no_patch("shared memory requires NtSetEaFile");
    SharedAttribute attribute;
    std::memcpy(attribute.bytes, sharedAttribute, sizeof(sharedAttribute));
    std::memcpy(attribute.bytes + sizeof(sharedAttribute), &metadata, sizeof(metadata));
    IO_STATUS_BLOCK status{};
    const auto result = set(handle, &status, &attribute, offsetof(SharedAttribute, bytes) + sizeof(attribute.bytes));
    if (static_cast<std::uint32_t>(result) == 0xc000004fu)
        NotImplemented_nid_no_patch("shared memory requires filesystem extended attributes");
    return result < 0 ? NativeError(result) : 0;
}

ACCESS_MASK NativeAccess(HANDLE handle) {
    using Query = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    static const auto query = reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile")));
    ACCESS_MASK access = 0;
    IO_STATUS_BLOCK status{};
    const auto result = query == nullptr ? static_cast<NTSTATUS>(0xc0000002u)
        : query(handle, &status, &access, sizeof(access), static_cast<FILE_INFORMATION_CLASS>(8));
    if (result < 0) throw std::runtime_error("shared memory: querying native descriptor access failed, status=" + std::to_string(result));
    return access;
}

std::filesystem::path NamespaceDirectory() {
    const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (size == 0) throw std::runtime_error("shared memory: LOCALAPPDATA is not available");
    std::wstring local(size, L'\0');
    const auto written = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), size);
    if (written == 0 || written >= size) throw std::runtime_error("shared memory: LOCALAPPDATA changed while reading it");
    local.resize(written);
    const auto directory = std::filesystem::path(local) / L"AnyPS5" / L"SharedMemory" / L"v1";
    if (!directory.is_absolute()) throw std::runtime_error("shared memory: LOCALAPPDATA must be an absolute path");
    std::filesystem::create_directories(directory);
    return directory;
}

#endif

bool IsHostSharedMemoryDescriptor(int descriptor) {
#ifdef __linux__
    char descriptorPath[64];
    std::snprintf(descriptorPath, sizeof(descriptorPath), "/proc/self/fd/%d", descriptor);
    char objectPath[4096];
    const auto length = ::readlink(descriptorPath, objectPath, sizeof(objectPath) - 1);
    if (length < 0) return false;
    objectPath[length] = '\0';
    return std::string_view(objectPath).starts_with("/dev/shm/anyps5-shm-") ||
        std::string_view(objectPath).starts_with("/memfd:anyps5 shm_open");
#elif defined(_WIN32)
    SharedMetadata metadata;
    return ReadMetadata(NativeHandle(descriptor), metadata);
#else
    (void)descriptor;
    return false;
#endif
}

class HostShmLock {
public:
    HostShmLock() {
#ifdef _WIN32
        const auto path = NamespaceDirectory() / L"namespace.lock";
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE || !LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlapped)) {
            WindowsError(GetLastError());
            error = errno;
        }
#elif defined(__linux__)
        descriptor = ::open("/dev/shm", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (descriptor < 0 || ::flock(descriptor, LOCK_EX) != 0) error = errno;
#endif
    }

    ~HostShmLock() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) {
            if (!error) UnlockFileEx(handle, 0, 1, 0, &overlapped);
            CloseHandle(handle);
        }
#elif defined(__linux__)
        const int savedError = errno;
        if (descriptor >= 0) {
            if (!error) ::flock(descriptor, LOCK_UN);
            ::close(descriptor);
        }
        errno = savedError;
#endif
    }

    int Error() const { return error; }

private:
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped{};
#else
    int descriptor = -1;
#endif
    int error = 0;
};

constexpr int GuestNoMemory = 12;
constexpr int GuestInvalid = 22;
constexpr int GuestTooManyFilesInSystem = 23;
constexpr int GuestTooManyFiles = 24;
constexpr int GuestCloseOnExec = 0x00100000;
constexpr int AcceptedFlags = SCE_KERNEL_O_ACCMODE | SCE_KERNEL_O_CREAT | SCE_KERNEL_O_EXCL | SCE_KERNEL_O_TRUNC | GuestCloseOnExec;

const char* const anonymousObject = reinterpret_cast<const char*>(1);

int Fail(int error) {
    *__error_nid_postfix() = error;
    return -1;
}

int GuestError(int error) {
    switch (error) {
    case EMFILE:
        return GuestTooManyFiles;
    case ENFILE:
        return GuestTooManyFilesInSystem;
    case ENOMEM:
        return GuestNoMemory;
    default:
        return 0;
    }
}

int NamedGuestError(int error) {
    switch (error) {
    case ENOENT:
        return 2;
    case EINTR:
        return 4;
    case EACCES:
        return 13;
    case EFAULT:
        return 14;
    case EEXIST:
        return 17;
    case EINVAL:
        return GuestInvalid;
    case ENOSPC:
        return 28;
    case EROFS:
        return 30;
    case EIO:
        return 5;
    case ENAMETOOLONG:
        return 63;
    default:
        return GuestError(error);
    }
}

std::string NamedHostPath(const char* path, int& error) {
    if (path == nullptr || path == anonymousObject) { error = EFAULT; return {}; }
    std::size_t length = 0;
    while (length < 1024 && path[length] != '\0') ++length;
    if (length == 1024) { error = ENAMETOOLONG; return {}; }
    if (length == 0 || path[0] != '/') { error = EINVAL; return {}; }
    const auto digest = Nid::Sha1(std::vector<std::uint8_t>(path, path + length));
    std::string hostName = "/anyps5-shm-";
    for (const auto byte : digest) {
        hostName += "0123456789abcdef"[byte >> 4];
        hostName += "0123456789abcdef"[byte & 15];
    }
    error = 0;
    return hostName;
}

int OpenNamedFile(const char* path, int flags, int mode) {
    int validationError = 0;
    const auto hostName = NamedHostPath(path, validationError);
    if (validationError != 0) return Fail(NamedGuestError(validationError) ? NamedGuestError(validationError) : GuestInvalid);
    HostShmLock namespaceLock;
    if (namespaceLock.Error() != 0) {
        errno = namespaceLock.Error();
        if (const int guest = NamedGuestError(errno)) return Fail(guest);
        throw std::runtime_error("shm_open: locking the host shared-memory namespace failed, errno=" + std::to_string(errno));
    }
#ifdef _WIN32
    const auto nativePath = NamespaceDirectory() / (hostName.substr(1) + ".data");
    const bool writable = (flags & SCE_KERNEL_O_ACCMODE) == SCE_KERNEL_O_RDWR;
    const DWORD access = GENERIC_READ | GENERIC_EXECUTE | FILE_WRITE_EA | (writable ? GENERIC_WRITE : 0);
    constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    bool created = false;
    HANDLE raw = INVALID_HANDLE_VALUE;
    if ((flags & SCE_KERNEL_O_CREAT) != 0) {
        raw = CreateFileW(nativePath.c_str(), access, share, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        created = raw != INVALID_HANDLE_VALUE;
        if (!created && GetLastError() == ERROR_FILE_EXISTS && !(flags & SCE_KERNEL_O_EXCL))
            raw = CreateFileW(nativePath.c_str(), access, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    } else {
        raw = CreateFileW(nativePath.c_str(), access, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (raw == INVALID_HANDLE_VALUE) {
        WindowsError(GetLastError());
        return Fail(NamedGuestError(errno) ? NamedGuestError(errno) : GuestInvalid);
    }
    OwnedHandle handle(raw);
    const auto discard = [&] {
        if (created) {
            if (handle.Get() != INVALID_HANDLE_VALUE) CloseHandle(handle.Release());
            DeleteFileW(nativePath.c_str());
            created = false;
        }
    };
    try {
        SharedMetadata metadata;
        if (created) {
            metadata.mode = (mode & 0777) & ~File::GetCreationMask_nid_no_patch();
            if (WriteMetadata(handle.Get(), metadata) != 0) {
                const int error = errno;
                discard();
                return Fail(NamedGuestError(error) ? NamedGuestError(error) : GuestInvalid);
            }
        } else if (!ReadMetadata(handle.Get(), metadata)) {
            throw std::runtime_error("shm_open: the namespace contains an object without shared-memory metadata");
        }
        if (writable && (flags & SCE_KERNEL_O_TRUNC)) {
            if (!SetFilePointerEx(handle.Get(), LARGE_INTEGER{}, nullptr, FILE_BEGIN) || !SetEndOfFile(handle.Get())) {
                WindowsError(GetLastError());
                const int error = errno;
                discard();
                return Fail(NamedGuestError(error) ? NamedGuestError(error) : GuestInvalid);
            }
        }
        const int descriptor = ::_open_osfhandle(reinterpret_cast<std::intptr_t>(handle.Get()),
            _O_BINARY | _O_NOINHERIT | (writable ? _O_RDWR : _O_RDONLY));
        if (descriptor < 0) {
            const int error = errno;
            discard();
            return Fail(NamedGuestError(error) ? NamedGuestError(error) : GuestInvalid);
        }
        handle.Release();
        try { File::RememberDescriptor(descriptor, flags | GuestCloseOnExec); }
        catch (...) { ::_close(descriptor); if (created) DeleteFileW(nativePath.c_str()); throw; }
        return descriptor;
    } catch (...) {
        discard();
        throw;
    }
#else
    int hostFlags = (flags & SCE_KERNEL_O_ACCMODE) == SCE_KERNEL_O_RDWR ? O_RDWR : O_RDONLY;
    if (flags & SCE_KERNEL_O_CREAT) hostFlags |= O_CREAT;
    if (flags & SCE_KERNEL_O_EXCL) hostFlags |= O_EXCL;
    if ((flags & SCE_KERNEL_O_TRUNC) && (flags & SCE_KERNEL_O_ACCMODE) == SCE_KERNEL_O_RDWR) hostFlags |= O_TRUNC;
    const auto permissions = (mode & 0777) & ~File::GetCreationMask_nid_no_patch();
    bool created = false;
    int descriptor;
    if (flags & SCE_KERNEL_O_CREAT) {
        descriptor = ::shm_open(hostName.c_str(), hostFlags | O_CLOEXEC | O_EXCL, permissions);
        created = descriptor >= 0;
        if (descriptor < 0 && errno == EEXIST && !(flags & SCE_KERNEL_O_EXCL)) {
            descriptor = ::shm_open(hostName.c_str(), (hostFlags & ~(O_CREAT | O_EXCL)) | O_CLOEXEC, 0);
        }
    } else {
        descriptor = ::shm_open(hostName.c_str(), hostFlags | O_CLOEXEC, 0);
    }
    if (descriptor >= 0) {
        if (!created || ::fchmod(descriptor, permissions) == 0) return descriptor;
        const int error = errno;
        ::close(descriptor);
        ::shm_unlink(hostName.c_str());
        if (const int guest = NamedGuestError(error)) return Fail(guest);
        throw std::runtime_error("shm_open: setting guest creation permissions failed, errno=" + std::to_string(error));
    }
    const int error = errno;
    if (const int guest = NamedGuestError(error)) return Fail(guest);
    throw std::runtime_error("shm_open: opening the host named object failed, errno=" + std::to_string(error));
#endif
}

int CreateAnonymousFile(int mode) {
#ifdef _WIN32
    static std::atomic<unsigned> sequence{0};
    const auto directory = NamespaceDirectory();
    const auto seed = std::to_string(std::random_device{}());
    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto path = directory / ("anyps5-shm-" + seed + "-" + std::to_string(sequence++));
        OwnedHandle handle(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
        if (handle.Get() != INVALID_HANDLE_VALUE) {
            SharedMetadata metadata;
            metadata.mode = (mode & 0777) & ~File::GetCreationMask_nid_no_patch();
            if (WriteMetadata(handle.Get(), metadata) != 0) return -1;
            const int descriptor = ::_open_osfhandle(reinterpret_cast<std::intptr_t>(handle.Get()), _O_RDWR | _O_BINARY | _O_NOINHERIT);
            if (descriptor >= 0) handle.Release();
            return descriptor;
        }
        WindowsError(GetLastError());
        const int error = errno;
        if (error != EEXIST) {
            errno = error;
            return -1;
        }
    }
    errno = EEXIST;
    return -1;
#else
    (void)mode;
    const int descriptor = memfd_create("anyps5 shm_open", MFD_CLOEXEC);
    return descriptor;
#endif
}

}

namespace File {

bool IsSharedMemoryDescriptor_nid_no_patch(int descriptor) {
    const HostDescriptorOperation operation;
    return IsHostSharedMemoryDescriptor(descriptor);
}

namespace {

class SharedMemoryLock {
public:
    explicit SharedMemoryLock(int value) {
#ifdef __linux__
        struct stat originalStatus{};
        if (::fstat(value, &originalStatus) != 0) { shared = true; error = errno; return; }
#endif
        if (!IsHostSharedMemoryDescriptor(value)) {
#ifdef __linux__
            struct stat currentStatus{};
            if (::fstat(value, &currentStatus) != 0) { shared = true; error = errno; return; }
            if (currentStatus.st_dev != originalStatus.st_dev || currentStatus.st_ino != originalStatus.st_ino) {
                shared = true;
                error = EBADF;
            }
#endif
            return;
        }
        shared = true;
        hostLock.emplace();
        if (hostLock->Error() != 0) { error = hostLock->Error(); return; }
        descriptor = Duplicate(value);
        if (descriptor < 0) { error = errno; return; }
        try {
            if (!IsHostSharedMemoryDescriptor(descriptor)) { error = EBADF; return; }
        } catch (...) {
            const int savedError = errno;
#ifdef _WIN32
            ::_close(descriptor);
#else
            ::close(descriptor);
#endif
            descriptor = -1;
            errno = savedError;
            throw;
        }
        if (!Refresh()) { error = errno; return; }
#ifdef __linux__
        if (status.st_dev != originalStatus.st_dev || status.st_ino != originalStatus.st_ino) error = EBADF;
#endif
    }

    ~SharedMemoryLock() {
        if (descriptor >= 0) {
            const int savedError = errno;
#ifdef _WIN32
            ::_close(descriptor);
#else
            ::close(descriptor);
#endif
            errno = savedError;
        }
    }

    bool IsShared() const { return shared; }
    int Error() const { return error; }
    int Descriptor() const { return descriptor; }
    bool Refresh() {
#ifdef _WIN32
        struct _stat64 nativeStatus{};
        if (::_fstat64(descriptor, &nativeStatus) != 0) return false;
        status.st_size = nativeStatus.st_size;
        return true;
#else
        return ::fstat(descriptor, &status) == 0;
#endif
    }
#ifdef _WIN32
    struct { std::int64_t st_size = 0; } status;
#else
    struct stat status{};
#endif

private:
    HostDescriptorOperation operation;
    static int Duplicate(int value) {
#ifdef _WIN32
        return ::_dup(value);
#else
        return ::fcntl(value, F_DUPFD_CLOEXEC, 0);
#endif
    }

    int descriptor = -1;
    std::optional<HostShmLock> hostLock;
    bool shared = false;
    int error = 0;
};
}

bool QuerySharedMemoryAccess_nid_no_patch(int descriptor, bool* readable, bool* writable) {
    const HostDescriptorOperation operation;
    if (!IsHostSharedMemoryDescriptor(descriptor)) return false;
#ifdef _WIN32
    const auto access = NativeAccess(NativeHandle(descriptor));
    *readable = (access & FILE_READ_DATA) != 0;
    *writable = (access & FILE_WRITE_DATA) != 0;
#else
    const auto flags = ::fcntl(descriptor, F_GETFL);
    if (flags < 0) throw std::runtime_error("shared memory: querying descriptor access failed");
    *readable = (flags & O_ACCMODE) != O_WRONLY;
    *writable = (flags & O_ACCMODE) != O_RDONLY;
#endif
    return true;
}

int SharedMemoryStatus_nid_no_patch(int descriptor, FileStat* result) {
#ifdef _WIN32
    SharedMemoryLock guard(descriptor);
    if (!guard.IsShared()) return -2;
    if (guard.Error() != 0) { errno = guard.Error(); return -1; }
    const auto handle = NativeHandle(guard.Descriptor());
    SharedMetadata metadata;
    if (!ReadMetadata(handle, metadata)) { errno = EIO; return -1; }
    BY_HANDLE_FILE_INFORMATION identity{};
    FILE_BASIC_INFO times{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandle(handle, &identity) ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &times, sizeof(times)) ||
        !GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard))) return WindowsError(GetLastError());
    *result = FileStat{};
    result->st_mode = static_cast<std::uint16_t>(0100000 | metadata.mode);
    result->st_uid = metadata.owner;
    result->st_gid = metadata.group;
    result->st_dev = identity.dwVolumeSerialNumber;
    const auto inode = identity.nFileIndexLow ^ (identity.nFileIndexHigh * 0x9e3779b1u);
    result->st_ino = inode == 0 ? 1 : inode;
    result->st_size = standard.EndOfFile.QuadPart;
    result->st_blksize = 0x4000;
    result->st_blocks = result->st_size / result->st_blksize + (result->st_size % result->st_blksize != 0);
    result->st_atim = WindowsFileTime::Decode(times.LastAccessTime.QuadPart);
    result->st_mtim = WindowsFileTime::Decode(times.LastWriteTime.QuadPart);
    result->st_ctim = WindowsFileTime::Decode(times.ChangeTime.QuadPart);
    result->st_birthtim = WindowsFileTime::Decode(times.CreationTime.QuadPart);
    return 0;
#else
    (void)descriptor;
    (void)result;
    return -2;
#endif
}

int ChmodSharedMemory_nid_no_patch(int descriptor, int mode) {
#ifdef _WIN32
    SharedMemoryLock guard(descriptor);
    if (!guard.IsShared()) return -2;
    if (guard.Error() != 0) { errno = guard.Error(); return -1; }
    const auto handle = NativeHandle(guard.Descriptor());
    SharedMetadata metadata;
    if (!ReadMetadata(handle, metadata)) { errno = EIO; return -1; }
    metadata.mode = mode & 0777;
    return WriteMetadata(handle, metadata);
#else
    (void)descriptor;
    (void)mode;
    return -2;
#endif
}

#ifdef _WIN32
static std::int64_t TransferSharedMemory(int descriptor, void* buffer, std::size_t size,
                                        std::int64_t offset, bool positioned, bool write) {
    SharedMemoryLock guard(descriptor);
    if (!guard.IsShared()) return -2;
    if (guard.Error() != 0) { errno = guard.Error(); return -1; }
    const auto handle = NativeHandle(guard.Descriptor());
    if ((NativeAccess(handle) & (write ? FILE_WRITE_DATA : FILE_READ_DATA)) == 0) { errno = EBADF; return -1; }
    if (positioned && offset < 0) { errno = EINVAL; return -1; }
    LARGE_INTEGER current{};
    if (!positioned && !SetFilePointerEx(handle, LARGE_INTEGER{}, &current, FILE_CURRENT)) return WindowsError(GetLastError());
    const auto position = positioned ? offset : current.QuadPart;
    if (position >= guard.status.st_size) return 0;
    const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(size,
        static_cast<std::uint64_t>(guard.status.st_size - position)));
    if (bytes == 0) return 0;
    const OwnedHandle mapping(CreateFileMappingW(handle, nullptr, write ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr));
    if (mapping.Get() == nullptr) return WindowsError(GetLastError());
    SYSTEM_INFO information{};
    GetSystemInfo(&information);
    const auto aligned = static_cast<std::uint64_t>(position) / information.dwAllocationGranularity * information.dwAllocationGranularity;
    const auto delta = static_cast<std::size_t>(position - aligned);
    const OwnedView view(MapViewOfFile(mapping.Get(), write ? FILE_MAP_WRITE : FILE_MAP_READ,
        static_cast<DWORD>(aligned >> 32), static_cast<DWORD>(aligned), delta + bytes));
    if (view.Get() == nullptr) return WindowsError(GetLastError());
    auto* data = static_cast<std::byte*>(view.Get()) + delta;
    if (write) std::memcpy(data, buffer, bytes);
    else std::memcpy(buffer, data, bytes);
    if (!positioned) {
        current.QuadPart = position + static_cast<std::int64_t>(bytes);
        if (!SetFilePointerEx(handle, current, nullptr, FILE_BEGIN)) return WindowsError(GetLastError());
    }
    return static_cast<std::int64_t>(bytes);
}
#endif

std::int64_t ReadSharedMemory_nid_no_patch(int descriptor, void* buffer, std::size_t size) {
#ifdef _WIN32
    return TransferSharedMemory(descriptor, buffer, size, 0, false, false);
#else
    (void)descriptor;
    (void)buffer;
    (void)size;
    return -2;
#endif
}

std::int64_t PreadSharedMemory_nid_no_patch(int descriptor, void* buffer, std::size_t size, std::int64_t offset) {
#ifdef _WIN32
    return TransferSharedMemory(descriptor, buffer, size, offset, true, false);
#else
    (void)descriptor;
    (void)buffer;
    (void)size;
    (void)offset;
    return -2;
#endif
}

std::int64_t WriteSharedMemory_nid_no_patch(int descriptor, const void* buffer, std::size_t size) {
#ifdef _WIN32
    return TransferSharedMemory(descriptor, const_cast<void*>(buffer), size, 0, false, true);
#else
    SharedMemoryLock guard(descriptor);
    if (!guard.IsShared()) return -2;
    if (guard.Error() != 0) { errno = guard.Error(); return -1; }
    if (!guard.Refresh()) return -1;
    const auto fd = guard.Descriptor();
    const int access = ::fcntl(fd, F_GETFL);
    if (access < 0) return -1;
    if ((access & O_ACCMODE) == O_RDONLY) { errno = EBADF; return -1; }
    const auto& status = guard.status;
    const auto position = ::lseek(fd, 0, SEEK_CUR);
    if (position < 0) return -1;
    if (position >= status.st_size) return 0;
    const auto count = std::min<std::uint64_t>(size, static_cast<std::uint64_t>(status.st_size - position));
    const auto result = ::write(fd, buffer, static_cast<std::size_t>(count));
    return static_cast<std::int64_t>(result);
#endif
}

std::int64_t PwriteSharedMemory_nid_no_patch(int descriptor, const void* buffer, std::size_t size, std::int64_t offset) {
#ifdef _WIN32
    return TransferSharedMemory(descriptor, const_cast<void*>(buffer), size, offset, true, true);
#else
    SharedMemoryLock guard(descriptor);
    if (!guard.IsShared()) return -2;
    if (guard.Error() != 0) { errno = guard.Error(); return -1; }
    if (!guard.Refresh()) return -1;
    const auto fd = guard.Descriptor();
    const int access = ::fcntl(fd, F_GETFL);
    if (access < 0) return -1;
    if ((access & O_ACCMODE) == O_RDONLY) { errno = EBADF; return -1; }
    const auto& status = guard.status;
    if (offset < 0) { errno = EINVAL; return -1; }
    if (offset >= status.st_size) return 0;
    const auto count = std::min<std::uint64_t>(size, static_cast<std::uint64_t>(status.st_size - offset));
    const auto result = ::pwrite(fd, buffer, static_cast<std::size_t>(count), static_cast<off_t>(offset));
    return static_cast<std::int64_t>(result);
#endif
}

std::int64_t SeekSharedMemory_nid_no_patch(int descriptor, std::int64_t offset, int whence) {
    SharedMemoryLock guard(descriptor);
    if (!guard.IsShared()) return -2;
    if (guard.Error() != 0) { errno = guard.Error(); return -1; }
    if (!guard.Refresh()) return -1;
    const auto fd = guard.Descriptor();
    const auto& status = guard.status;
    std::int64_t base = 0;
    if (whence == SEEK_CUR) {
#ifdef _WIN32
        base = ::_lseeki64(fd, 0, SEEK_CUR);
#else
        base = ::lseek(fd, 0, SEEK_CUR);
#endif
        if (base < 0) return -1;
    } else if (whence == SEEK_END) {
        base = static_cast<std::int64_t>(status.st_size);
    } else if (whence != SEEK_SET) {
        errno = EINVAL;
        return -1;
    }
    if ((offset > 0 && base > std::numeric_limits<std::int64_t>::max() - offset) ||
        (offset < 0 && base < std::numeric_limits<std::int64_t>::min() - offset)) {
        errno = EOVERFLOW;
        return -1;
    }
    const auto target = base + offset;
    if (target < 0 || target > status.st_size) {
        errno = EINVAL;
        return -1;
    }
#ifdef _WIN32
    return ::_lseeki64(fd, target, SEEK_SET);
#else
    return ::lseek(fd, static_cast<off_t>(target), SEEK_SET);
#endif
}

#ifndef _WIN32
static std::int64_t TransferSharedMemoryVectors(int descriptor, const struct iovec* vectors, int count,
                                                std::int64_t offset, bool positioned) {
    SharedMemoryLock guard(descriptor);
    if (!guard.IsShared()) return -2;
    if (guard.Error() != 0) { errno = guard.Error(); return -1; }
    const auto fd = guard.Descriptor();
    const int access = ::fcntl(fd, F_GETFL);
    if (access < 0) return -1;
    if ((access & O_ACCMODE) == O_RDONLY) { errno = EBADF; return -1; }
    if (positioned && offset < 0) { errno = EINVAL; return -1; }
    struct stat status{};
    if (::fstat(fd, &status) != 0) return -1;
    std::uint64_t total = 0;
    constexpr auto maxTransfer = static_cast<std::uint64_t>(std::numeric_limits<ssize_t>::max());
    for (int i = 0; i < count; ++i) {
        if (vectors[i].iov_len > maxTransfer - total) { errno = EINVAL; return -1; }
        total += vectors[i].iov_len;
    }
    std::uint64_t available = 0;
    if (positioned) {
        if (offset < status.st_size) available = static_cast<std::uint64_t>(status.st_size - offset);
        if (total > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - offset)) { errno = EINVAL; return -1; }
    } else {
        const auto position = ::lseek(fd, 0, SEEK_CUR);
        if (position < 0) return -1;
        if (position < status.st_size) available = static_cast<std::uint64_t>(status.st_size - position);
    }
    std::vector<struct iovec> bounded;
    bounded.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count && available > 0; ++i) {
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(vectors[i].iov_len, available));
        if (length != 0) bounded.push_back({vectors[i].iov_base, length});
        available -= length;
    }
    if (bounded.empty()) return 0;
    return positioned ? ::pwritev(fd, bounded.data(), static_cast<int>(bounded.size()), static_cast<off_t>(offset))
                      : ::writev(fd, bounded.data(), static_cast<int>(bounded.size()));
}

std::int64_t WritevSharedMemory_nid_no_patch(int descriptor, const struct iovec* vectors, int count) {
    return TransferSharedMemoryVectors(descriptor, vectors, count, 0, false);
}

std::int64_t PwritevSharedMemory_nid_no_patch(int descriptor, const struct iovec* vectors, int count, std::int64_t offset) {
    return TransferSharedMemoryVectors(descriptor, vectors, count, offset, true);
}
#endif

int TruncateSharedMemory_nid_no_patch(int descriptor, std::int64_t length) {
    SharedMemoryLock guard(descriptor);
    if (!guard.IsShared()) return -2;
    if (guard.Error() != 0) {
        errno = guard.Error();
#ifdef _WIN32
        return errno;
#else
        return -1;
#endif
    }
    const auto fd = guard.Descriptor();
#ifdef _WIN32
    const int result = static_cast<int>(::_chsize_s(fd, length));
#else
    const int result = ::ftruncate(fd, static_cast<off_t>(length));
#endif
    return result;
}

}

extern "C" int APS5_VABI shm_open_nid_postfix(const char* path, int flags, int mode) {
    const int savedError = *__error_nid_postfix();
    const HostDescriptorOperation operation;
    const int accessMode = flags & SCE_KERNEL_O_ACCMODE;
    if (accessMode != SCE_KERNEL_O_RDONLY && accessMode != SCE_KERNEL_O_RDWR) return Fail(GuestInvalid);
    if ((flags & ~AcceptedFlags) != 0) return Fail(GuestInvalid);
    if (path != anonymousObject) {
        const int descriptor = OpenNamedFile(path, flags, mode);
        if (descriptor >= 0) *__error_nid_postfix() = savedError;
        return descriptor;
    }
    if (accessMode == SCE_KERNEL_O_RDONLY) return Fail(GuestInvalid);
    const int descriptor = CreateAnonymousFile(mode);
    if (descriptor >= 0) {
#ifdef _WIN32
        try { File::RememberDescriptor(descriptor, flags | GuestCloseOnExec); }
        catch (...) { ::_close(descriptor); throw; }
#endif
        *__error_nid_postfix() = savedError;
        return descriptor;
    }
    const int error = errno;
    if (const int guest = GuestError(error)) return Fail(guest);
    throw std::runtime_error("shm_open: creating the host anonymous file failed, errno=" + std::to_string(error));
}

extern "C" int APS5_VABI shm_unlink_nid_postfix(const char* path) {
    const HostDescriptorOperation operation;
    int validationError = 0;
    const auto hostName = NamedHostPath(path, validationError);
    if (validationError != 0) return Fail(NamedGuestError(validationError) ? NamedGuestError(validationError) : GuestInvalid);
    HostShmLock namespaceLock;
    if (namespaceLock.Error() != 0) {
        errno = namespaceLock.Error();
        if (const int guest = NamedGuestError(errno)) return Fail(guest);
        throw std::runtime_error("shm_unlink: locking the host shared-memory namespace failed, errno=" + std::to_string(errno));
    }
#ifdef _WIN32
    const auto objectPath = NamespaceDirectory() / (hostName.substr(1) + ".data");
    const OwnedHandle handle(CreateFileW(objectPath.c_str(), DELETE | FILE_READ_EA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (handle.Get() == INVALID_HANDLE_VALUE) {
        WindowsError(GetLastError());
        return Fail(NamedGuestError(errno) ? NamedGuestError(errno) : GuestInvalid);
    }
    SharedMetadata metadata;
    if (!ReadMetadata(handle.Get(), metadata)) throw std::runtime_error("shm_unlink: the namespace contains an object without shared-memory metadata");
    struct { DWORD flags; } disposition{0x00000001u | 0x00000002u};
    if (SetFileInformationByHandle(handle.Get(), static_cast<FILE_INFO_BY_HANDLE_CLASS>(21), &disposition, sizeof(disposition))) return 0;
    const auto error = GetLastError();
    if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION)
        NotImplemented_nid_no_patch("shm_unlink requires filesystem POSIX delete semantics");
    WindowsError(error);
    return Fail(NamedGuestError(errno) ? NamedGuestError(errno) : GuestInvalid);
#else
    const auto objectPath = "/dev/shm" + hostName;
    if (::faccessat(AT_FDCWD, objectPath.c_str(), R_OK | W_OK, AT_EACCESS) != 0) {
        const int error = errno;
        if (const int guest = NamedGuestError(error)) return Fail(guest);
        throw std::runtime_error("shm_unlink: checking shared-memory permissions failed, errno=" + std::to_string(error));
    }
    if (::shm_unlink(hostName.c_str()) == 0) return 0;
    const int error = errno;
    if (const int guest = NamedGuestError(error)) return Fail(guest);
    throw std::runtime_error("shm_unlink: removing the host named object failed, errno=" + std::to_string(error));
#endif
}
