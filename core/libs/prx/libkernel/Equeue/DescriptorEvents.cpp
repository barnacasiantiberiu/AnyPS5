#include "prx/libkernel/Equeue/DescriptorEvents.hpp"
#include "prx/libkernel/File/include/File.hpp"
#include "prx/libkernel/File/include/DirectoryDescriptor.hpp"
#include "prx/libkernel/File/include/RandomDevice.hpp"
#include "prx/libkernel/File/include/DescriptorControl.hpp"
#include "prx/libkernel/Socket/include/SocketRuntime.hpp"
#include "prx/libc/include/HostDescriptorFork.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <winternl.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

struct DescriptorWatch {
    bool file = false;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::shared_ptr<void> positionState;
    ~DescriptorWatch() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
    int descriptor = -1;
    ~DescriptorWatch() { if (descriptor >= 0) ::close(descriptor); }
#endif
};

bool NativeReady(KernelEqueueEvent* event) {
    const auto& watch = *static_cast<DescriptorWatch*>(event->filter.data);
    const bool write = event->event.filter == EVFILT_WRITE;
    std::int64_t available = 0;
    bool eof = false;
#ifdef _WIN32
    if (watch.file) {
        const File::PositionGuard position(watch.positionState);
        LARGE_INTEGER size{}, offset{}, zero{};
        if (!GetFileSizeEx(watch.handle, &size) || !SetFilePointerEx(watch.handle, zero, &offset, FILE_CURRENT))
            throw std::runtime_error("equeue: cannot query watched file size or offset");
        available = size.QuadPart - offset.QuadPart;
    } else {
        struct PipeLocalInformation {
            ULONG type, configuration, maximumInstances, currentInstances, inboundQuota, readable, outboundQuota, writable, state, end;
        };
        using QueryFile = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
        static const auto query = reinterpret_cast<QueryFile>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile")));
        PipeLocalInformation information{};
        IO_STATUS_BLOCK status{};
        const NTSTATUS result = query ? query(watch.handle, &status, &information, sizeof(information), static_cast<FILE_INFORMATION_CLASS>(24)) : -1;
        if (result < 0) throw std::runtime_error("equeue: cannot query watched pipe state");
        available = write ? information.writable : information.readable;
        eof = information.state == 1 || information.state == 4;
    }
#else
    if (watch.file) {
        struct stat status{};
        const auto offset = ::lseek(watch.descriptor, 0, SEEK_CUR);
        if (offset < 0 || ::fstat(watch.descriptor, &status) != 0)
            throw std::runtime_error("equeue: cannot query watched file size or offset");
        available = status.st_size - offset;
    } else {
        pollfd state{watch.descriptor, static_cast<short>(write ? POLLOUT : POLLIN), 0};
        if (::poll(&state, 1, 0) < 0) throw std::runtime_error("equeue: cannot poll watched pipe");
        eof = (state.revents & (POLLHUP | POLLERR)) != 0;
        int readable = 0;
        if (::ioctl(watch.descriptor, FIONREAD, &readable) != 0)
            throw std::runtime_error("equeue: cannot query watched pipe data");
        available = readable;
        if (write) {
#ifdef F_GETPIPE_SZ
            const int capacity = ::fcntl(watch.descriptor, F_GETPIPE_SZ);
            if (capacity < 0) throw std::runtime_error("equeue: cannot query watched pipe capacity");
            available = capacity - readable;
#else
            NotImplemented_nid_no_patch("equeue: pipe write quota on this host");
#endif
            if ((state.revents & POLLOUT) == 0 && !eof) return false;
        }
    }
#endif
    if (write && eof) available = 0;
    event->event.data = static_cast<std::intptr_t>(available);
    event->event.flags = static_cast<std::uint16_t>(eof ? event->event.flags | EV_EOF : event->event.flags & ~EV_EOF);
    return eof || (watch.file ? available != 0 : available >= (write ? 512 : 1));
}

}

int PrepareDescriptorEvent_nid_no_patch(KernelEqueueEvent& event, std::size_t lowWatermark) {
    if (event.event.ident > static_cast<std::uintptr_t>(std::numeric_limits<int>::max())) return SCE_KERNEL_ERROR_EBADF;
    const int descriptor = static_cast<int>(event.event.ident);
    if (descriptor >= GuestSockets::FirstDescriptor) {
        if (!GuestSockets::IsOpen(descriptor)) return SCE_KERNEL_ERROR_EBADF;
        if (event.event.filter == EVFILT_WRITE && lowWatermark > 1)
            NotImplemented_nid_no_patch("equeue: socket write low watermark");
        if (event.event.filter == EVFILT_READ && lowWatermark > 1 && !GuestSockets::ReadWatermarkSupported_nid_no_patch(descriptor))
            NotImplemented_nid_no_patch("equeue: read low watermark outside connected streams");
        auto threshold = std::make_shared<std::size_t>(lowWatermark);
        event.filter.owner = threshold;
        event.filter.data = threshold.get();
        event.filter.timedPolling = lowWatermark > 1;
        event.filter.pollFunc = [](KernelEqueueEvent* current) {
            std::int64_t available = 0;
            bool eof = false;
            if (!GuestSockets::Ready(static_cast<int>(current->event.ident), current->event.filter == EVFILT_WRITE, &available, &eof)) return false;
            const auto threshold = *static_cast<const std::size_t*>(current->filter.data);
            current->event.data = static_cast<std::intptr_t>(available);
            current->event.flags = static_cast<std::uint16_t>(eof ? current->event.flags | EV_EOF : current->event.flags & ~EV_EOF);
            return eof || threshold <= 1 || (available >= 0 && static_cast<std::uint64_t>(available) >= threshold);
        };
        return 0;
    }
    const HostDescriptorOperation operation;
    if (!DescriptorIsOpen_nid_no_patch(descriptor)) return SCE_KERNEL_ERROR_EBADF;
    if (IsKqueueDescriptor_nid_no_patch(descriptor)) NotImplemented_nid_no_patch("equeue: watching another kqueue");
    if (File::IsRandomDevice(descriptor)) NotImplemented_nid_no_patch("equeue: random device filter");
    auto watch = std::make_shared<DescriptorWatch>();
#ifdef _WIN32
    if (File::DirectoryDescriptorPath(descriptor)) NotImplemented_nid_no_patch("equeue: directory read filter");
    const HANDLE handle = reinterpret_cast<HANDLE>(::_get_osfhandle(descriptor));
    const DWORD kind = GetFileType(handle);
    watch->file = kind == FILE_TYPE_DISK;
    if (watch->file) watch->positionState = File::PositionGuard::Capture(descriptor);
    if (!watch->file && kind != FILE_TYPE_PIPE) NotImplemented_nid_no_patch("equeue: character device filter");
    if (watch->file && event.event.filter == EVFILT_WRITE) return SCE_KERNEL_ERROR_EINVAL;
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &watch->handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return SCE_KERNEL_ERROR_ENOMEM;
#else
    struct stat status{};
    if (::fstat(descriptor, &status) != 0) return SCE_KERNEL_ERROR_EBADF;
    watch->file = S_ISREG(status.st_mode);
    if (!watch->file && !S_ISFIFO(status.st_mode)) NotImplemented_nid_no_patch("equeue: non-file/non-pipe descriptor filter");
    if (watch->file && event.event.filter == EVFILT_WRITE) return SCE_KERNEL_ERROR_EINVAL;
    watch->descriptor = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
    if (watch->descriptor < 0) return SceKernelError(errno == EMFILE ? 24 : errno == ENFILE ? 23 : errno == ENOMEM ? 12 : 5);
#endif
    event.filter.data = watch.get();
    event.filter.owner = std::move(watch);
    event.filter.pollFunc = NativeReady;
    event.filter.timedPolling = true;
    return 0;
}
