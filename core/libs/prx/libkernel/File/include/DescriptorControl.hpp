#ifndef CORE_LIBS_PRX_LIBKERNEL_FILE_DESCRIPTORCONTROL_HPP
#define CORE_LIBS_PRX_LIBKERNEL_FILE_DESCRIPTORCONTROL_HPP

#include <cstdint>
#ifdef _WIN32
#include <memory>
#endif

namespace File {

#ifdef _WIN32
class PositionGuard {
public:
    explicit PositionGuard(int descriptor);
    explicit PositionGuard(std::shared_ptr<void> state);
    static std::shared_ptr<void> Capture(int descriptor);
    ~PositionGuard();
    PositionGuard(const PositionGuard&) = delete;
    PositionGuard& operator=(const PositionGuard&) = delete;

private:
    std::shared_ptr<void> owner;
    void* mutex = nullptr;
};
#endif

enum class FcntlArgument { None, Int, Pointer };

FcntlArgument FcntlArgumentOf(int command);
int Fcntl(int fd, int command, std::intptr_t argument);
int Ioctl(int fd, std::uint64_t request, void* argument);
int DuplicateFrom(int fd, int start);
void RememberDescriptor(int fd, int openFlags);
void ShareDescriptor(int from, int to);
void ForgetDescriptor(int fd);

}

#endif
