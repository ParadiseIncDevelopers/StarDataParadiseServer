#include "wire_values.hpp"

#include <algorithm>
#include <charconv>
#include <set>
#include <chrono>

namespace sdp::sql
{
namespace
{
using wire::Json;

constexpr size_t max_compound_bytes = 64 * 1024;
constexpr size_t max_compound_depth = 8;
constexpr size_t max_compound_items = 128;
constexpr size_t max_compound_nodes = 2048;

[[noreturn]] void invalid()
{
    throw wire::Error(wire::Code::invalid_argument);
}

std::string_view member_text(const Json& json, const char* key)
{
    auto it = json.find(key);
    if (it == json.end() || !it->is_string()) invalid();
    return it->get_ref<const std::string&>();
}

int64_t signed_decimal(const std::string_view s)
{
    int64_t out{};
    const auto end = s.data() + s.size();
    const auto parsed = std::from_chars(s.data(), end, out);
    if (parsed.ptr != end || parsed.ec != std::errc{} || std::to_string(out) != s) invalid();
    return out;
}

bool canonical_coefficient(std::string_view value)
{
    const bool negative = value.starts_with('-');
    if (negative) value.remove_prefix(1);
    return !value.empty() && value.size() <= 38 && value.find_first_not_of("0123456789") == value.npos &&
        (value[0] != '0' || (value.size() == 1 && !negative));
}

std::string decimal_sql(std::string_view coefficient, uint32_t scale)
{
    if (!canonical_coefficient(coefficient)) invalid();
    const bool negative = coefficient.starts_with('-');
    if (negative) coefficient.remove_prefix(1);
    std::string digits(coefficient);
    if (scale)
    {
        if (digits.size() <= scale) digits.insert(0, scale + 1 - digits.size(), '0');
        digits.insert(digits.size() - scale, 1, '.');
    }
    if (negative) digits.insert(0, 1, '-');
    return digits;
}

uint8_t nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return static_cast<uint8_t>(ch - '0');
    if (ch >= 'a' && ch <= 'f') return static_cast<uint8_t>(ch - 'a' + 10);
    invalid();
}

disk::Bytes binary(std::string_view hex)
{
    if (hex.size() % 2 || hex.size() > disk::row_limit * 2) invalid();
    disk::Bytes bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
        bytes.push_back(static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    return bytes;
}

using namespace std::chrono;

std::string padded(unsigned n, size_t width)
{
    auto digits = std::to_string(n);
    if (digits.size() < width) digits.insert(0, width - digits.size(), '0');
    return digits;
}

std::string format_timestamp(const Json& item)
{
    const auto total = signed_decimal(member_text(item, "seconds"));
    const auto found = item.find("offset_minutes");
    if (found == item.end() || !found->is_number_integer() || *found < -840 || *found > 840) invalid();
    const int offset = found->get<int>();
    const auto ns = unsigned_field(item, "nanos", 999999999);
    if (!item.contains("nanos") || total < -64000000000LL || total > 255000000000LL) invalid();
    if (item.contains("zone")) throw wire::Error(wire::Code::unsupported);
    const sys_seconds local{seconds{total} + minutes{offset}};
    const auto day = floor<days>(local);
    const year_month_day ymd{day};
    if (int(ymd.year()) < 1 || int(ymd.year()) > 9999) invalid();
    const hh_mm_ss clock{local - day};
    const auto absolute = static_cast<unsigned>(offset < 0 ? -offset : offset);
    return padded(static_cast<unsigned>(int(ymd.year())), 4) + '-' + padded(unsigned(ymd.month()), 2) +
        '-' + padded(unsigned(ymd.day()), 2) + 'T' + padded(static_cast<unsigned>(clock.hours().count()), 2) +
        ':' + padded(static_cast<unsigned>(clock.minutes().count()), 2) +
        ':' + padded(static_cast<unsigned>(clock.seconds().count()), 2) + '.' + padded(ns, 9) +
        (offset < 0 ? '-' : '+') + padded(absolute / 60, 2) + ':' + padded(absolute % 60, 2);
}

unsigned digits(std::string_view text, size_t offset, size_t width)
{
    unsigned value{};
    const auto end = text.data() + offset + width;
    const auto parsed = std::from_chars(text.data() + offset, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) invalid();
    return value;
}

Json timestamp_json(std::string_view canonical)
{
    const bool utc = canonical.size() == 30 && canonical[29] == 'Z';
    if (!utc && canonical.size() != 35) invalid();
    const year_month_day calendar{year{static_cast<int>(digits(canonical, 0, 4))},
        month{digits(canonical, 5, 2)}, day{digits(canonical, 8, 2)}};
    auto offset = utc ? 0 : static_cast<int>(digits(canonical, 30, 2) * 60 + digits(canonical, 33, 2));
    if (canonical[29] == '-') offset = -offset;
    const sys_seconds value = sys_days(calendar) + hours{digits(canonical, 11, 2)} +
        minutes{digits(canonical, 14, 2)} + seconds{digits(canonical, 17, 2)} - minutes{offset};
    return {{"t", "timestamp"}, {"seconds", std::to_string(value.time_since_epoch().count())},
        {"nanos", digits(canonical, 20, 9)}, {"offset_minutes", offset}};
}

Type declaration(const Json& source)
{
    if (!source.is_object()) invalid();
    auto text = member_text(source, "type");
    if (text == "BOOLEAN") return {Kind::boolean};
    if (text == "BIGINT") return {Kind::integer};
    if (text == "TEXT") return {Kind::text};
    if (text == "BLOB") return {Kind::blob};
    if (text == "DATE") return {Kind::date};
    if (text == "TIME") return {Kind::time};
    if (text == "RECORD") return {Kind::record};
    if (text == "COLLECTION") return {Kind::collection};
    if (text == "TIMESTAMP WITH TIME ZONE") return {Kind::timestamp_tz};
    if (text == "DECIMAL" && source.contains("precision") && source.contains("scale"))
    {
        const auto p = static_cast<uint8_t>(unsigned_field(source, "precision", 38));
        const auto s = static_cast<uint8_t>(unsigned_field(source, "scale", 38));
        const Type type{Kind::decimal, p, s};
        validate_type(type);
        return type;
    }
    invalid();
}

disk::Value pack_compound(const Json& json, Type type, Source source);

disk::Value read_value(const Json& json, Type type)
{
    if (!json.is_object()) invalid();
    const auto tag = member_text(json, "t");
    if (tag == "null" && json.size() == 1) return {};
    if ((tag == "record" && type.kind == Kind::record) || (tag == "collection" && type.kind == Kind::collection))
        return pack_compound(json, type, {});
    if (tag == "i64" && type.kind == Kind::integer)
        return signed_decimal(member_text(json, "v"));
    if (tag == "bool" && type.kind == Kind::boolean)
    {
        const auto value = json.find("v");
        if (value == json.end() || !value->is_boolean()) invalid();
        return static_cast<int64_t>(value->get<bool>());
    }
    if (tag == "decimal" && type.kind == Kind::decimal)
    {
        if (!json.contains("scale")) invalid();
        return parse_literal(type, decimal_sql(member_text(json, "unscaled"), unsigned_field(json, "scale", 38)));
    }
    if (tag == "binary" && type.kind == Kind::blob)
        return binary(member_text(json, "hex"));
    if (tag == "timestamp" && type.kind == Kind::timestamp_tz)
        return parse_literal(type, format_timestamp(json));
    if (tag == "text" && (type.kind == Kind::text || type.kind == Kind::date || type.kind == Kind::time))
        return parse_literal(type, member_text(json, "v"));
    invalid();
}

void require_json_fields(const Json& item, std::initializer_list<const char*> fields)
{
    if (item.size() != fields.size() || std::ranges::any_of(fields, [&](const auto key) { return !item.contains(key); }))
        invalid();
}

void validate_cell(const Json& item, size_t depth, size_t& nodes)
{
    if (++nodes > max_compound_nodes)
        throw wire::Error(wire::Code::resource_limit);
    const auto tag = member_text(item, "t");
    if (tag == "record" || tag == "collection")
    {
        if (depth >= max_compound_depth) throw wire::Error(wire::Code::resource_limit);
        const auto name = tag == "record" ? "fields" : "items";
        require_json_fields(item, {"t", name});
        const auto& children = item.at(name);
        if (!children.is_array()) invalid();
        if (children.size() > max_compound_items) throw wire::Error(wire::Code::resource_limit);
        if (tag == "collection")
        {
            for (const auto& child : children) validate_cell(child, depth + 1, nodes);
        }
        else
        {
            std::set<std::string_view> names;
            for (const auto& field : children)
            {
                const auto field_name = member_text(field, "name");
                if (field_name.empty() || field_name.size() > 128 ||
                    field_name.find('\0') != field_name.npos || !names.insert(field_name).second)
                    invalid();
                require_json_fields(field, {"name", "value"});
                validate_cell(field.at("value"), depth + 1, nodes);
            }
        }
        return;
    }

    if (tag == "null") require_json_fields(item, {"t"});
    else if (tag == "decimal")
    {
        require_json_fields(item, {"t", "unscaled", "scale"});
        const auto coefficient = member_text(item, "unscaled");
        if (!canonical_coefficient(coefficient)) invalid();
        const auto scale = unsigned_field(item, "scale", 38);
        const auto digits = coefficient.size() - static_cast<size_t>(coefficient.starts_with('-'));
        read_value(item, {Kind::decimal, static_cast<uint8_t>((std::max)(digits, size_t{scale})),
            static_cast<uint8_t>(scale)});
    }
    else if (tag == "timestamp")
    {
        require_json_fields(item, {"t", "seconds", "nanos", "offset_minutes"});
        read_value(item, {Kind::timestamp_tz});
    }
    else if (tag == "binary")
    {
        require_json_fields(item, {"t", "hex"});
        read_value(item, {Kind::blob});
    }
    else
    {
        require_json_fields(item, {"t", "v"});
        if (tag == "i64") read_value(item, {Kind::integer});
        else if (tag == "bool") read_value(item, {Kind::boolean});
        else if (tag == "text") read_value(item, {Kind::text});
        else if (tag == "date") parse_literal({Kind::date}, member_text(item, "v"));
        else if (tag == "time") parse_literal({Kind::time}, member_text(item, "v"));
        else invalid();
    }
}

void validate_compound(const Json& json, Type type)
{
    const auto expected = type.kind == Kind::record ? "record" : "collection";
    if ((type.kind != Kind::record && type.kind != Kind::collection) || member_text(json, "t") != expected)
        invalid();
    size_t nodes{};
    validate_cell(json, 0, nodes);
}

[[noreturn]] void bad_compound(const wire::Error& error, Source source)
{
    if (error.code() == wire::Code::resource_limit || error.code() == wire::Code::too_large)
        throw Error(Code::limit, source, "PL/SQL compound value exceeds its resource limit");
    if (error.code() == wire::Code::unsupported)
        throw Error(Code::unsupported, source, "Unsupported PL/SQL compound value");
    throw Error(Code::type, source, "Malformed PL/SQL compound value");
}

disk::Value pack_compound(const Json& json, Type type, Source source)
{
    try
    {
        validate_compound(json, type);
        const auto canonical = json.dump();
        if (canonical.size() > max_compound_bytes) throw wire::Error(wire::Code::resource_limit);
        return disk::Bytes(canonical.begin(), canonical.end());
    }
    catch (const wire::Error& error) { bad_compound(error, source); }
    catch (const Json::exception&) { throw Error(Code::type, source, "Malformed PL/SQL compound value"); }
}

Json unpack_compound(Type type, std::string_view text, Source source)
{
    if (text.size() > max_compound_bytes) throw Error(Code::limit, source, "PL/SQL compound value exceeds 64 KiB");
    try
    {
        auto json = wire::parse_payload(text);
        validate_compound(json, type);
        return json;
    }
    catch (const wire::Error& error) { bad_compound(error, source); }
    catch (const Json::exception&) { throw Error(Code::type, source, "Malformed PL/SQL compound value"); }
}
}

disk::Value parse_compound(Type type, std::string_view text, Source source)
{
    validate_type(type, source);
    const auto canonical = unpack_compound(type, text, source).dump();
    return disk::Bytes(canonical.begin(), canonical.end());
}

uint32_t unsigned_field(const wire::Json& object, const char* key, uint32_t maximum, uint32_t fallback)
{
    const auto it = object.find(key);
    if (it == object.end()) return fallback;
    if (!it->is_number_unsigned() || it->get<uint64_t>() > maximum) invalid();
    return it->get<uint32_t>();
}

ParameterTypes read_parameter_types(const wire::Json& object)
{
    if (!object.is_object() || object.size() > 64) invalid();
    ParameterTypes types;
    for (auto it = object.begin(); it != object.end(); ++it)
    {
        if (it.key().empty() || it.key().size() > 128 || it.key().find('\0') != std::string::npos) invalid();
        types.emplace(it.key(), declaration(it.value()));
    }
    return types;
}

ParameterValues read_parameters(const wire::Json& object, const ParameterTypes& types)
{
    if (!object.is_object() || object.size() != types.size()) invalid();
    ParameterValues values;
    for (const auto& [name, type] : types)
    {
        const auto it = object.find(name);
        if (it == object.end()) invalid();
        values.emplace(name, read_value(*it, type));
    }
    return values;
}

wire::Json write_value(const disk::Value& value, Type type)
{
    if (std::holds_alternative<std::monostate>(value)) return {{"t", "null"}};
    switch (type.kind)
    {
    case Kind::integer: return {{"t", "i64"}, {"v", std::to_string(std::get<int64_t>(value))}};
    case Kind::boolean: return {{"t", "bool"}, {"v", truth(value) == Truth::true_}};
    case Kind::decimal:
    {
        const auto& fixed = std::get<disk::Decimal>(value);
        return {{"t", "decimal"}, {"unscaled", fixed.unscaled}, {"scale", fixed.scale}};
    }
    case Kind::blob:
    {
        static constexpr std::string_view hex = "0123456789abcdef";
        const auto& bytes = std::get<disk::Bytes>(value);
        std::string text;
        text.reserve(bytes.size() * 2);
        for (const auto byte : bytes)
        {
            text.push_back(hex[byte >> 4]);
            text.push_back(hex[byte & 15]);
        }
        return {{"t", "binary"}, {"hex", std::move(text)}};
    }
    case Kind::timestamp_tz: return timestamp_json(std::get<std::string>(value));
    case Kind::date:
    case Kind::time:
    case Kind::text: return {{"t", "text"}, {"v", std::get<std::string>(value)}};
    case Kind::record:
    case Kind::collection:
    {
        const auto& bytes = std::get<disk::Bytes>(value);
        return unpack_compound(type, {reinterpret_cast<const char*>(bytes.data()), bytes.size()}, {});
    }
    default: invalid();
    }
}
}
