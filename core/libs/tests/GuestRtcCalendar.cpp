#include "SceTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "tests/ModuleChecks.hpp"
#include <chrono>
#include <cstdint>
#include <initializer_list>

extern "C" {
int APS5_VABI sceRtcGetTick(const RtcDateTime*, RtcTick*);
int APS5_VABI sceRtcSetTick(RtcDateTime*, const RtcTick*);
int APS5_VABI sceRtcIsLeapYear(int);
int APS5_VABI sceRtcGetDaysInMonth(int, int);
int APS5_VABI sceRtcGetDayOfWeek(int, int, int);
int APS5_VABI sceRtcTickAddDays(RtcTick*, const RtcTick*, std::int32_t);
}

using ModuleTest::Require;
using namespace std::chrono;

static void CheckDay(sys_days day) {
    const year_month_day calendar{day};
    const int year = static_cast<int>(calendar.year());
    const unsigned month = static_cast<unsigned>(calendar.month());
    const unsigned date = static_cast<unsigned>(calendar.day());
    const RtcDateTime input{static_cast<std::uint16_t>(year), static_cast<std::uint16_t>(month),
        static_cast<std::uint16_t>(date), 23, 59, 59, 999999};
    const auto expected = static_cast<std::uint64_t>(62135596800000000ll +
        duration_cast<microseconds>(day.time_since_epoch()).count() + 86399999999ll);
    RtcTick tick{};
    Require(sceRtcGetTick(&input, &tick) == 0 && tick.tick == expected);
    RtcDateTime output{};
    Require(sceRtcSetTick(&output, &tick) == 0);
    Require(output.year == input.year && output.month == input.month && output.day == input.day);
    Require(output.hour == 23 && output.minute == 59 && output.second == 59 && output.microsecond == 999999);
    Require(sceRtcIsLeapYear(year) == calendar.year().is_leap());
    Require(sceRtcGetDaysInMonth(year, month) == static_cast<int>(static_cast<unsigned>(
        year_month_day_last{calendar.year(), month_day_last{calendar.month()}}.day())));
    Require(sceRtcGetDayOfWeek(year, month, date) == static_cast<int>(weekday{day}.c_encoding()));
    if (year < 9999) {
        Require(sceRtcTickAddDays(&tick, &tick, 1) == 0 && tick.tick == expected + 86400000000ull);
        const year_month_day next{day + days{1}};
        Require(sceRtcSetTick(&output, &tick) == 0);
        Require(output.year == static_cast<int>(next.year()) && output.month == static_cast<unsigned>(next.month())
            && output.day == static_cast<unsigned>(next.day()) && output.microsecond == 999999);
    }
}

int main() {
    const sys_days begin{year{2000}/January/1};
    const sys_days end{year{2400}/January/1};
    int checked = 0;
    for (auto day = begin; day < end; day += days{1}) {
        CheckDay(day);
        ++checked;
    }
    Require(checked == 146097);
    for (int value : {1, 4, 100, 400, 1601, 1900, 1969, 1970, 3002, 9999}) {
        CheckDay(sys_days{year{value}/January/1});
        CheckDay(sys_days{year{value}/February/28});
        CheckDay(sys_days{year{value}/March/1});
        CheckDay(sys_days{year{value}/December/31});
    }
    std::printf("Validated %d Gregorian dates plus 40 boundary dates against std::chrono\n", checked);
}
