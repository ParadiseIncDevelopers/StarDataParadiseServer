#include "types.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <utility>

namespace sdp::sql
{
namespace
{
constexpr size_t decimal_limit = 38;

bool digit(char c) { return c >= '0' && c <= '9'; }

bool digits(std::string_view text)
{
    return std::ranges::all_of(text, digit);
}

std::string_view magnitude(std::string_view text)
{
    return text.starts_with('-') ? text.substr(1) : text;
}

bool is_numeric(Kind kind)
{
    return kind == Kind::integer || kind == Kind::decimal;
}

bool is_null(const disk::Value& value)
{
    return std::holds_alternative<std::monostate>(value);
}

template<class T>
const T& scalar(const disk::Value& value, Source source)
{
    const auto* field = std::get_if<T>(&value);
    if (!field) throw Error(Code::type, source, "Value does not match its SQL type");
    return *field;
}

void check_utf8(std::string_view text, Source source)
{
    if (text.size() > disk::row_limit)
        throw Error(Code::limit, source, "Text exceeds the storage byte limit");
    while (!text.empty())
    {
        const auto width = utf8_width(text);
        if (!width) throw Error(Code::type, source, "Text is not valid UTF-8");
        text.remove_prefix(width);
    }
}

void strip_zeros(std::string& number)
{
    const auto first = number.find_first_not_of('0');
    if (first == std::string::npos) number = "0";
    else number.erase(0, first);
}

Type decimal_type(const disk::Decimal& value)
{
    const auto precision = (std::max)(magnitude(value.unscaled).size(), static_cast<size_t>(value.scale));
    return {Kind::decimal, static_cast<uint8_t>(precision), value.scale};
}

disk::Decimal decimal_text(std::string_view text, Source source)
{
    const bool negative = text.starts_with('-');
    if (negative || text.starts_with('+')) text.remove_prefix(1);
    if (text.size() > disk::row_limit)
        throw Error(Code::limit, source, "Decimal spelling exceeds the storage byte limit");

    const auto dot = text.find('.');
    const auto whole = text.substr(0, dot);
    const auto fraction = dot == text.npos ? std::string_view{} : text.substr(dot + 1);
    if ((whole.empty() && fraction.empty()) || !digits(whole) || !digits(fraction))
        throw Error(Code::type, source, "Invalid exact decimal literal");
    if (fraction.size() > decimal_limit)
        throw Error(Code::range, source, "Decimal scale exceeds 38");

    const auto first = whole.find_first_not_of('0');
    const auto significant_whole = first == whole.npos ? std::string_view{} : whole.substr(first);
    if (significant_whole.size() + fraction.size() > decimal_limit)
        throw Error(Code::range, source, "Decimal precision exceeds 38");

    std::string unscaled(significant_whole);
    unscaled.append(fraction);
    strip_zeros(unscaled);
    if (negative && unscaled != "0") unscaled.insert(unscaled.begin(), '-');
    return {std::move(unscaled), static_cast<uint8_t>(fraction.size())};
}

void validate_decimal(const disk::Decimal& value, Type type, Source source)
{
    const auto number = magnitude(value.unscaled);
    if (value.scale != type.scale || number.empty() || !digits(number)
        || (number.front() == '0' && value.unscaled != "0"))
        throw Error(Code::type, source, "Decimal does not match its declared scale or encoding");
    if (number.size() > type.precision)
        throw Error(Code::range, source, "Decimal exceeds its declared precision");
}

void increment(std::string& digits)
{
    for (auto i = digits.rbegin(); i != digits.rend(); ++i)
    {
        if (*i != '9')
        {
            ++*i;
            return;
        }
        *i = '0';
    }
    digits.insert(digits.begin(), '1');
}

disk::Decimal rescale(const disk::Decimal& value, Type target, Source source)
{
    std::string number(magnitude(value.unscaled));
    if (target.scale > value.scale && number != "0")
        number.append(target.scale - value.scale, '0');
    else if (target.scale < value.scale)
    {
        const auto discarded = static_cast<size_t>(value.scale - target.scale);
        const bool round_up = number.size() >= discarded && number[number.size() - discarded] >= '5';
        number.resize(number.size() > discarded ? number.size() - discarded : 0);
        if (number.empty()) number = "0";
        if (round_up) increment(number);
    }
    if (number.size() > target.precision)
        throw Error(Code::range, source, "Rounded decimal exceeds its declared precision");
    if (number != "0" && value.unscaled.starts_with('-')) number.insert(number.begin(), '-');
    return {std::move(number), target.scale};
}

int64_t integer_text(std::string_view text, Source source)
{
    if (text.starts_with('+'))
    {
        text.remove_prefix(1);
        if (text.starts_with('-')) throw Error(Code::type, source, "Invalid integer literal");
    }
    if (text.empty()) throw Error(Code::type, source, "Invalid integer literal");
    int64_t result{};
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, result);
    if (parsed.ptr != end) throw Error(Code::type, source, "Invalid integer literal");
    if (parsed.ec == std::errc::result_out_of_range)
        throw Error(Code::range, source, "Integer is outside the signed 64-bit range");
    if (parsed.ec != std::errc{}) throw Error(Code::type, source, "Invalid integer literal");
    return result;
}

int date_or_time_number(std::string_view text, Source source)
{
    if (text.empty() || !digits(text)) throw Error(Code::type, source, "Invalid temporal literal");
    int value{};
    for (const auto c : text) value = value * 10 + (c - '0');
    return value;
}

std::chrono::sys_days date_text(std::string_view text, Source source)
{
    using namespace std::chrono;
    if (text.size() != 10 || text[4] != '-' || text[7] != '-')
        throw Error(Code::type, source, "Expected a date in YYYY-MM-DD form");
    const auto y = date_or_time_number(text.substr(0, 4), source);
    const auto m = date_or_time_number(text.substr(5, 2), source);
    const auto d = date_or_time_number(text.substr(8, 2), source);
    const auto date = year{y} / static_cast<unsigned>(m) / static_cast<unsigned>(d);
    if (y == 0 || !date.ok()) throw Error(Code::range, source, "Calendar date does not exist");
    return sys_days{date};
}

std::pair<int, int> time_text(std::string_view text, Source source)
{
    if (text.size() < 8 || text[2] != ':' || text[5] != ':' || text.size() > 18)
        throw Error(Code::type, source, "Expected time in HH:MM:SS[.fraction] form");
    const auto hours = date_or_time_number(text.substr(0, 2), source);
    const auto minutes = date_or_time_number(text.substr(3, 2), source);
    const auto seconds = date_or_time_number(text.substr(6, 2), source);
    if (hours > 23 || minutes > 59 || seconds > 59)
        throw Error(Code::range, source, "Time is outside the civil clock range");
    int nanos{};
    if (text.size() > 8)
    {
        if (text[8] != '.' || text.size() == 9)
            throw Error(Code::type, source, "Invalid fractional time");
        const auto fractional = text.substr(9);
        nanos = date_or_time_number(fractional, source);
        for (auto i = fractional.size(); i < 9; ++i) nanos *= 10;
    }
    return {hours * 3600 + minutes * 60 + seconds, nanos};
}

std::string normalized_time(std::string_view text, int nanos)
{
    std::string result(text.substr(0, 8));
    result += '.';
    auto fraction = std::to_string(nanos);
    result.append(9 - fraction.size(), '0');
    return result + fraction;
}

int offset_minutes(std::string_view text, Source source)
{
    if (text == "Z") return 0;
    if (text.size() != 6 || (text[0] != '+' && text[0] != '-') || text[3] != ':')
        throw Error(Code::type, source, "Timestamp requires Z or a signed HH:MM offset");
    const auto hours = date_or_time_number(text.substr(1, 2), source);
    const auto minutes = date_or_time_number(text.substr(4, 2), source);
    if (hours > 14 || minutes > 59 || (hours == 14 && minutes != 0))
        throw Error(Code::range, source, "Timestamp offset is outside +/-14:00");
    const auto total = hours * 60 + minutes;
    return text[0] == '-' ? -total : total;
}

struct Timestamp
{
    std::pair<int64_t, int> utc;
    size_t timezone_at;
    int offset;
};

Timestamp timestamp_text(std::string_view text, Source source)
{
    using namespace std::chrono;
    if (text.size() < 20 || (text[10] != 'T' && text[10] != ' '))
        throw Error(Code::type, source, "Expected timestamp with date, time and explicit offset");
    const auto zone = text.find_first_of("Z+-", 19);
    if (zone == text.npos) throw Error(Code::type, source, "Timestamp requires an explicit offset");
    const auto day = date_text(text.substr(0, 10), source);
    const auto [time, nanos] = time_text(text.substr(11, zone - 11), source);
    const auto offset = offset_minutes(text.substr(zone), source);
    const auto utc_seconds = duration_cast<seconds>(day.time_since_epoch()).count() + time - offset * 60;
    return {{utc_seconds, nanos}, zone, offset};
}

int hex_digit(char c, Source source)
{
    if (digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw Error(Code::type, source, "Invalid hexadecimal binary literal");
}

disk::Bytes binary_text(std::string_view text, Source source)
{
    if (text.size() % 2) throw Error(Code::type, source, "Binary literal requires two digits per byte");
    if (text.size() / 2 > disk::row_limit) throw Error(Code::limit, source, "Binary exceeds the storage byte limit");
    disk::Bytes result;
    result.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2)
        result.push_back(static_cast<disk::Byte>(hex_digit(text[i], source) * 16 + hex_digit(text[i + 1], source)));
    return result;
}

disk::Decimal numeric(const disk::Value& value, Type type, Source source)
{
    if (type.kind == Kind::integer) return {std::to_string(scalar<int64_t>(value, source)), 0};
    const auto& decimal = scalar<disk::Decimal>(value, source);
    validate_decimal(decimal, type, source);
    return decimal;
}

int compare_decimal(const disk::Decimal& left, const disk::Decimal& right)
{
    const bool l_negative = left.unscaled.starts_with('-');
    const bool r_negative = right.unscaled.starts_with('-');
    if (l_negative != r_negative) return l_negative ? -1 : 1;
    std::string l(magnitude(left.unscaled));
    std::string r(magnitude(right.unscaled));
    const auto scale = (std::max)(left.scale, right.scale);
    if (l != "0") l.append(scale - left.scale, '0');
    if (r != "0") r.append(scale - right.scale, '0');
    const auto compared = l.size() == r.size() ? l.compare(r) : l.size() < r.size() ? -1 : 1;
    const int sign = (compared > 0) - (compared < 0);
    return l_negative ? -sign : sign;
}

template<class T>
int compare(const T& left, const T& right)
{
    return (left > right) - (left < right);
}
}

size_t utf8_width(std::string_view text) noexcept
{
    if (text.empty()) return 0;
    const auto lead = static_cast<unsigned char>(text.front());
    if (lead <= 0x7f) return 1;
    if (lead < 0xc2 || lead > 0xf4) return 0;
    const size_t width = lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    if (text.size() < width) return 0;
    for (size_t pos = 1; pos < width; ++pos)
        if ((static_cast<unsigned char>(text[pos]) & 0xc0) != 0x80) return 0;
    const auto second = static_cast<unsigned char>(text[1]);
    if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0)
        || (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90)) return 0;
    return width;
}

void validate_type(Type type, Source source)
{
    if (type.kind == Kind::decimal)
    {
        if (!type.precision || type.precision > decimal_limit || type.scale > type.precision)
            throw Error(Code::type, source, "DECIMAL requires 1 <= precision <= 38 and 0 <= scale <= precision");
    }
    else if (type.kind < Kind::null || type.kind > Kind::collection || type.precision || type.scale)
        throw Error(Code::type, source, "Invalid type or unexpected type dimensions");
}

std::string type_name(Type type)
{
    switch (type.kind)
    {
    case Kind::null: return "NULL";
    case Kind::boolean: return "BOOLEAN";
    case Kind::integer: return "BIGINT";
    case Kind::decimal: return "DECIMAL(" + std::to_string(type.precision) + "," + std::to_string(type.scale) + ")";
    case Kind::text: return "TEXT";
    case Kind::blob: return "BLOB";
    case Kind::date: return "DATE";
    case Kind::time: return "TIME";
    case Kind::timestamp_tz: return "TIMESTAMP WITH TIME ZONE";
    case Kind::record: return "RECORD";
    case Kind::collection: return "COLLECTION";
    }
    throw Error(Code::type, {}, "Invalid SQL type");
}

Type decimal_literal_type(std::string_view text, Source source)
{
    return decimal_type(decimal_text(text, source));
}

disk::Value parse_literal(Type type, std::string_view text, Source source)
{
    validate_type(type, source);
    switch (type.kind)
    {
    case Kind::null: return {};
    case Kind::boolean:
        if (text == "true" || text == "TRUE") return int64_t{1};
        if (text == "false" || text == "FALSE") return int64_t{0};
        throw Error(Code::type, source, "Expected a boolean literal");
    case Kind::integer: return integer_text(text, source);
    case Kind::decimal: return rescale(decimal_text(text, source), type, source);
    case Kind::text:
        check_utf8(text, source);
        return std::string(text);
    case Kind::blob: return binary_text(text, source);
    case Kind::date:
        date_text(text, source);
        return std::string(text);
    case Kind::time: return normalized_time(text, time_text(text, source).second);
    case Kind::timestamp_tz:
    {
        const auto parsed = timestamp_text(text, source);
        std::string result(text.substr(0, 10));
        result += 'T';
        result += normalized_time(text.substr(11, parsed.timezone_at - 11), parsed.utc.second);
        result += parsed.offset ? text.substr(parsed.timezone_at) : "Z";
        return result;
    }
    case Kind::record:
    case Kind::collection: return parse_compound(type, text, source);
    }
    throw Error(Code::type, source, "Invalid SQL type");
}

bool can_coerce(Type from, Type to) noexcept
{
    return from.kind == Kind::null || from.kind == to.kind ||
        (from.kind == Kind::integer && to.kind == Kind::decimal);
}

disk::Value coerce(const disk::Value& value, Type from, Type to, Source source)
{
    validate_type(from, source);
    validate_type(to, source);
    if (!can_coerce(from, to))
        throw Error(Code::type, source, "Cannot convert " + type_name(from) + " to " + type_name(to));
    if (is_null(value)) return {};
    if (from.kind == Kind::null) throw Error(Code::type, source, "A non-NULL value cannot have SQL NULL type");
    switch (to.kind)
    {
    case Kind::integer: return scalar<int64_t>(value, source);
    case Kind::decimal: return rescale(numeric(value, from, source), to, source);
    case Kind::boolean: return boolean_value(truth(value, source));
    case Kind::text:
    case Kind::date:
    case Kind::time:
    case Kind::timestamp_tz: return parse_literal(to, scalar<std::string>(value, source), source);
    case Kind::blob:
    {
        const auto& bytes = scalar<disk::Bytes>(value, source);
        if (bytes.size() > disk::row_limit)
            throw Error(Code::limit, source, "Binary exceeds the storage byte limit");
        return bytes;
    }
    case Kind::record:
    case Kind::collection:
    {
        const auto& bytes = scalar<disk::Bytes>(value, source);
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return parse_compound(to, text, source);
    }
    case Kind::null: break;
    }
    throw Error(Code::type, source, "A non-NULL value cannot have SQL NULL type");
}

disk::Value negate_numeric(const disk::Value& value, Type type, Source source)
{
    validate_type(type, source);
    if (is_null(value)) return {};
    if (type.kind == Kind::integer)
    {
        const auto number = scalar<int64_t>(value, source);
        if (number == (std::numeric_limits<int64_t>::min)())
            throw Error(Code::range, source, "Negation overflows a signed 64-bit integer");
        return -number;
    }
    if (type.kind == Kind::decimal)
    {
        auto number = numeric(value, type, source);
        if (number.unscaled.starts_with('-')) number.unscaled.erase(0, 1);
        else if (number.unscaled != "0") number.unscaled.insert(number.unscaled.begin(), '-');
        return number;
    }
    throw Error(Code::type, source, "Unary minus requires a numeric operand");
}

Truth truth(const disk::Value& value, Source source)
{
    if (is_null(value)) return Truth::unknown;
    const auto stored = scalar<int64_t>(value, source);
    if (stored == 0) return Truth::false_;
    if (stored == 1) return Truth::true_;
    throw Error(Code::type, source, "Boolean storage value must be 0 or 1");
}

Truth logical_not(Truth value) noexcept
{
    switch (value)
    {
    case Truth::true_: return Truth::false_;
    case Truth::false_: return Truth::true_;
    case Truth::unknown: return Truth::unknown;
    }
    return Truth::unknown;
}

Truth logical_and(Truth left, Truth right) noexcept
{
    if (left == Truth::false_ || right == Truth::false_) return Truth::false_;
    if (left == Truth::unknown || right == Truth::unknown) return Truth::unknown;
    return Truth::true_;
}

Truth logical_or(Truth left, Truth right) noexcept
{
    if (left == Truth::true_ || right == Truth::true_) return Truth::true_;
    if (left == Truth::unknown || right == Truth::unknown) return Truth::unknown;
    return Truth::false_;
}

disk::Value boolean_value(Truth value)
{
    if (value == Truth::unknown) return {};
    return static_cast<int64_t>(value == Truth::true_);
}

bool comparable(Type left, Type right) noexcept
{
    if (left.kind == Kind::record || left.kind == Kind::collection
        || right.kind == Kind::record || right.kind == Kind::collection) return false;
    return left.kind == Kind::null || right.kind == Kind::null || left.kind == right.kind
        || (is_numeric(left.kind) && is_numeric(right.kind));
}

int compare_non_null(const disk::Value& left, Type left_type, const disk::Value& right, Type right_type, Source source)
{
    validate_type(left_type, source);
    validate_type(right_type, source);
    if (is_null(left) || is_null(right) || left_type.kind == Kind::null || right_type.kind == Kind::null
        || !comparable(left_type, right_type))
        throw Error(Code::type, source, "Comparison requires compatible non-NULL operands");

    if (left_type.kind == Kind::integer && right_type.kind == Kind::integer)
        return compare(scalar<int64_t>(left, source), scalar<int64_t>(right, source));
    if (is_numeric(left_type.kind) && is_numeric(right_type.kind))
        return compare_decimal(numeric(left, left_type, source), numeric(right, right_type, source));

    switch (left_type.kind)
    {
    case Kind::boolean: return compare(truth(left, source), truth(right, source));
    case Kind::text: return compare(scalar<std::string>(left, source), scalar<std::string>(right, source));
    case Kind::blob: return compare(scalar<disk::Bytes>(left, source), scalar<disk::Bytes>(right, source));
    case Kind::date:
        return compare(date_text(scalar<std::string>(left, source), source),
            date_text(scalar<std::string>(right, source), source));
    case Kind::time:
        return compare(time_text(scalar<std::string>(left, source), source),
            time_text(scalar<std::string>(right, source), source));
    case Kind::timestamp_tz:
        return compare(timestamp_text(scalar<std::string>(left, source), source).utc,
            timestamp_text(scalar<std::string>(right, source), source).utc);
    default: throw Error(Code::type, source, "Invalid SQL type for comparison");
    }
}
}
