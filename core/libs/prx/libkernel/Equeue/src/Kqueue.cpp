#include "prx/libkernel/Equeue/Equeue.hpp"
#include "prx/libkernel/Equeue/DescriptorEvents.hpp"
#include "prx/libc/include/General.hpp"
#include <limits>

namespace {
constexpr std::uint32_t NOTE_SECONDS = 0x0001;
constexpr std::uint32_t NOTE_MSECONDS = 0x0002;
constexpr std::uint32_t NOTE_USECONDS = 0x0004;
constexpr std::uint32_t NOTE_NSECONDS = 0x0008;
constexpr std::uint32_t NOTE_TIMER_PRECMASK = NOTE_SECONDS | NOTE_MSECONDS | NOTE_USECONDS | NOTE_NSECONDS;
}

std::optional<std::uint64_t> TimerPeriodNanos_nid_no_patch(std::int64_t data, std::uint32_t fflags) {
    std::uint64_t unit = 0;
    switch (fflags & NOTE_TIMER_PRECMASK) {
    case NOTE_SECONDS:
        unit = 1000000000ULL;
        break;
    case NOTE_MSECONDS:
    case 0:
        unit = 1000000ULL;
        break;
    case NOTE_USECONDS:
        unit = 1000ULL;
        break;
    case NOTE_NSECONDS:
        if (data >= 1000000000LL && data % 1000000000LL != 0) {
            NotImplemented_nid_no_patch("kevent: EVFILT_TIMER NOTE_NSECONDS period above one second with a fraction");
        }
        unit = 1;
        break;
    default:
        return std::nullopt;
    }
    const auto value = static_cast<std::uint64_t>(data);
    if (value > std::numeric_limits<std::uint64_t>::max() / unit) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return value * unit;
}

bool IsKqueueDescriptor(int descriptor) {
    return IsKqueueDescriptor_nid_no_patch(descriptor);
}
