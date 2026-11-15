#include "schedule.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <chrono>
#include <limits>
#include <optional>
#include <variant>

namespace sdp::jobs
{
namespace
{
using namespace std::chrono;
using W = wire::Code;
using wire::Json;

constexpr int maximum_interval = 365 * 24 * 60 * 60;
constexpr int64_t last_daily_second =
    duration_cast<seconds>(sys_days{year{2101} / January / 1}.time_since_epoch()).count() - 1;
constexpr int daily_search_days = 8;

struct Interval { int seconds; };
struct Daily
{
    int hour;
    int minute;
    std::optional<DYNAMIC_TIME_ZONE_INFORMATION> zone;
};
struct Schedule
{
    Misfire misfire;
    std::variant<Interval, Daily> calendar;
};

[[noreturn]] void invalid() { throw wire::Error(W::invalid_argument); }
[[noreturn]] void time_zone_failure() { throw wire::Error(W::internal); }

int integer(const Json& input, const char* key, int minimum, int maximum)
{
    const auto found = input.find(key);
    if (found == input.end() || !found->is_number_integer()) invalid();
    if (found->is_number_unsigned())
    {
        const auto value = found->get<uint64_t>();
        if (value < static_cast<uint64_t>(minimum) || value > static_cast<uint64_t>(maximum)) invalid();
        return static_cast<int>(value);
    }
    const auto value = found->get<int64_t>();
    if (value < minimum || value > maximum) invalid();
    return static_cast<int>(value);
}

const std::string& string(const Json& input, const char* key)
{
    const auto found = input.find(key);
    if (found == input.end() || !found->is_string()) invalid();
    return found->get_ref<const std::string&>();
}

Misfire read_misfire(const Json& input)
{
    if (!input.contains("misfire")) return Misfire::skip;
    const auto& value = string(input, "misfire");
    if (value == "skip") return Misfire::skip;
    if (value == "run_once") return Misfire::run_once;
    invalid();
}

std::optional<DYNAMIC_TIME_ZONE_INFORMATION> resolve_zone(std::string_view name)
{
    if (name.empty() || name.size() > 127 || name.find('\0') != name.npos) invalid();
    if (name == "UTC") return {};
    const int length = static_cast<int>(name.size());
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), length, nullptr, 0);
    if (!count) invalid();
    std::wstring wide(static_cast<size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), length, wide.data(), count)) invalid();
    for (DWORD index = 0;; ++index)
    {
        DYNAMIC_TIME_ZONE_INFORMATION entry{};
        const auto status = EnumDynamicTimeZoneInformation(index, &entry);
        if (status == ERROR_NO_MORE_ITEMS) invalid();
        if (status != ERROR_SUCCESS) time_zone_failure();
        const auto match = CompareStringOrdinal(entry.TimeZoneKeyName, -1, wide.c_str(), -1, TRUE);
        if (!match) time_zone_failure();
        if (match == CSTR_EQUAL)
        {
            entry.DynamicDaylightTimeDisabled = FALSE;
            return entry;
        }
    }
}

Schedule parse(const Json& rule)
{
    if (!rule.is_object()) invalid();
    const auto misfire = read_misfire(rule);
    const bool interval = rule.contains("every_seconds");
    for (const auto& field : rule.items())
    {
        const auto& key = field.key();
        if (key == "misfire") continue;
        if (interval ? key != "every_seconds" : key != "timezone" && key != "hour" && key != "minute") invalid();
    }
    if (interval) return {misfire, Interval{integer(rule, "every_seconds", 1, maximum_interval)}};
    const auto hour = integer(rule, "hour", 0, 23);
    const auto minute = integer(rule, "minute", 0, 59);
    return {misfire, Daily{hour, minute, resolve_zone(string(rule, "timezone"))}};
}

SYSTEMTIME calendar_time(sys_seconds instant)
{
    const auto date = floor<days>(instant);
    const year_month_day fields{date};
    const hh_mm_ss time{instant - date};
    SYSTEMTIME output{};
    output.wYear = static_cast<WORD>(static_cast<int>(fields.year()));
    output.wMonth = static_cast<WORD>(static_cast<unsigned>(fields.month()));
    output.wDay = static_cast<WORD>(static_cast<unsigned>(fields.day()));
    output.wHour = static_cast<WORD>(time.hours().count());
    output.wMinute = static_cast<WORD>(time.minutes().count());
    output.wSecond = static_cast<WORD>(time.seconds().count());
    return output;
}

sys_seconds civil_time(const SYSTEMTIME& fields)
{
    return sys_days{year{fields.wYear} / fields.wMonth / fields.wDay}
        + hours{fields.wHour} + minutes{fields.wMinute} + seconds{fields.wSecond};
}

sys_seconds to_local(const DYNAMIC_TIME_ZONE_INFORMATION& zone, sys_seconds utc)
{
    const auto input = calendar_time(utc);
    SYSTEMTIME local{};
    if (!SystemTimeToTzSpecificLocalTimeEx(&zone, &input, &local)) time_zone_failure();
    return civil_time(local);
}

std::optional<sys_seconds> first_instant(DYNAMIC_TIME_ZONE_INFORMATION& zone, sys_seconds wall_time)
{
    TIME_ZONE_INFORMATION rules{};
    const auto local_year = calendar_time(wall_time).wYear;
    if (!GetTimeZoneInformationForYear(local_year, &zone, &rules)) time_zone_failure();
    std::optional<sys_seconds> first;
    const auto consider = [&](long relative_bias) {
        const auto utc = wall_time + minutes{int64_t{rules.Bias} + relative_bias};
        if (to_local(zone, utc) == wall_time && (!first || utc < *first)) first = utc;
    };
    consider(rules.StandardBias);
    if (rules.DaylightDate.wMonth && rules.StandardDate.wMonth) consider(rules.DaylightBias);
    return first;
}

int64_t daily_after(Daily daily, int64_t utc_exclusive)
{
    if (utc_exclusive > last_daily_second) invalid();
    const sys_seconds utc{seconds{utc_exclusive}};
    const auto local_now = daily.zone ? to_local(*daily.zone, utc) : utc;
    const auto today = floor<days>(local_now);
    for (int elapsed = 0; elapsed < daily_search_days; ++elapsed)
    {
        const auto wall_time = today + days{elapsed} + hours{daily.hour} + minutes{daily.minute};
        const auto next = daily.zone ? first_instant(*daily.zone, wall_time) : std::optional{wall_time};
        // A gap has no matching UTC instant. A fold uses its earlier instant once per local date.
        if (!next || *next <= utc) continue;
        const auto result = next->time_since_epoch().count();
        if (result <= last_daily_second) return result;
        break;
    }
    throw wire::Error(W::resource_limit);
}
}

Misfire validate_schedule(const wire::Json& rule) { return parse(rule).misfire; }

int64_t schedule_after(const wire::Json& rule, int64_t utc_exclusive)
{
    if (utc_exclusive < 0) invalid();
    const auto schedule = parse(rule);
    if (const auto* interval = std::get_if<Interval>(&schedule.calendar))
    {
        if (utc_exclusive > (std::numeric_limits<int64_t>::max)() - interval->seconds)
            throw wire::Error(W::resource_limit);
        return utc_exclusive + interval->seconds;
    }
    return daily_after(std::get<Daily>(schedule.calendar), utc_exclusive);
}
}
