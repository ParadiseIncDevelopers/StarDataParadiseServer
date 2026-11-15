#include "schedule.hpp"

#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
using sdp::jobs::Misfire;
using sdp::jobs::schedule_after;
using sdp::jobs::validate_schedule;
using sdp::wire::Code;
using sdp::wire::Json;
using namespace std::chrono;

constexpr int64_t utc(int y, unsigned m, unsigned d, int h = 0, int minute = 0, int second = 0)
{
    return (sys_days{year{y} / m / d} + hours{h} + minutes{minute} + seconds{second}).time_since_epoch().count();
}

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
void expect(Code code, const Function& function)
{
    try { function(); }
    catch (const sdp::wire::Error& error)
    {
        require(error.code() == code, "wrong schedule error code");
        return;
    }
    throw std::runtime_error("expected a schedule error");
}

Json daily(const char* timezone, int hour, int minute)
{
    return {{"timezone", timezone}, {"hour", hour}, {"minute", minute}};
}

void test_interval()
{
    const Json cadence = {{"every_seconds", 30}};
    require(validate_schedule(cadence) == Misfire::skip && schedule_after(cadence, 0) == 30,
        "default interval or default missed policy changed");
    const Json catch_up = {{"every_seconds", 31'536'000}, {"misfire", "run_once"}};
    require(validate_schedule(catch_up) == Misfire::run_once &&
        schedule_after(catch_up, 100) == 31'536'100, "maximum interval or missed policy was lost");
    require(schedule_after({{"every_seconds", 1}}, 41) == 42, "minimum interval did not advance");
    expect(Code::resource_limit, [&] { (void)schedule_after(cadence, (std::numeric_limits<int64_t>::max)()); });
    expect(Code::invalid_argument, [&] { (void)schedule_after(cadence, -1); });
}

void test_daily()
{
    const auto utc_rule = daily("UTC", 2, 30);
    require(schedule_after(utc_rule, utc(2026, 12, 31, 23, 59)) == utc(2027, 1, 1, 2, 30),
        "UTC day or year rollover used host local time");
    const auto leap_rule = daily("UTC", 0, 0);
    require(schedule_after(leap_rule, utc(2028, 2, 28, 0, 0)) == utc(2028, 2, 29),
        "daily boundary was not exclusive or leap day was skipped");
    expect(Code::resource_limit, [&] { (void)schedule_after(leap_rule, utc(2100, 12, 31, 23, 59, 59)); });
    expect(Code::invalid_argument, [&] { (void)schedule_after(leap_rule, utc(2101, 1, 1)); });

    // These rules are read from the explicit zone's Windows DST data, independent of host zone.
    const auto gap = daily("Pacific Standard Time", 2, 30);
    require(schedule_after(gap, utc(2026, 3, 7, 10, 29, 59)) == utc(2026, 3, 7, 10, 30),
        "winter UTC offset was incorrect");
    require(schedule_after(gap, utc(2026, 3, 8, 8, 0)) == utc(2026, 3, 9, 9, 30),
        "nonexistent spring local time was incorrectly moved or accepted");
    const auto fold = daily("pacific standard time", 1, 30);
    const auto earlier = utc(2026, 11, 1, 8, 30);
    const auto tomorrow = utc(2026, 11, 2, 9, 30);
    require(schedule_after(fold, earlier - 1) == earlier, "ambiguous fall time did not use the earlier UTC instant");
    require(schedule_after(fold, earlier) == tomorrow && schedule_after(fold, utc(2026, 11, 1, 9, 29)) == tomorrow,
        "fall fold generated a second job on the same local date");

    const auto offset = daily("India Standard Time", 2, 30);
    require(schedule_after(offset, utc(2025, 12, 31, 20, 59, 59)) == utc(2025, 12, 31, 21, 0),
        "non-integer-hour offset or local year boundary was not preserved");
}

void test_validation()
{
    const Json rejected[] = {
        Json::array(), Json::object(),
        {{"every_seconds", 0}}, {{"every_seconds", -1}}, {{"every_seconds", 31'536'001}},
        {{"every_seconds", 1.0}}, {{"every_seconds", true}}, {{"every_seconds", (std::numeric_limits<uint64_t>::max)()}},
        {{"every_seconds", 30}, {"hour", 2}}, {{"every_seconds", 30}, {"misfire", "all"}},
        {{"every_seconds", 30}, {"misfire", nullptr}}, {{"hour", 2}, {"minute", 0}},
        daily("No Such Windows Time Zone", 1, 0), daily("UTC", 24, 0), daily("UTC", 0, 60),
        {{"timezone", "UTC"}, {"hour", 1}, {"minute", 0}, {"extra", false}},
        {{"timezone", std::string("UTC\0hidden", 10)}, {"hour", 0}, {"minute", 0}}
    };
    for (const auto& rule : rejected) expect(Code::invalid_argument, [&] { (void)validate_schedule(rule); });
}
}

int main()
{
    try
    {
        test_interval();
        test_daily();
        test_validation();
        std::cout << "Schedule interval and timezone boundary checks passed, including DST gap and single-run fold\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
