#pragma once

#include "diagnostic.hpp"
#include "../storage/storage.hpp"

#include <string_view>

namespace sdp::sql
{
enum class Kind { null, boolean, integer, decimal, text, blob, date, time, timestamp_tz, record, collection };

struct Type
{
    Kind kind{};
    uint8_t precision{};
    uint8_t scale{};
    bool operator==(const Type&) const = default;
};

enum class Truth { false_, true_, unknown };

size_t utf8_width(std::string_view text) noexcept;
void validate_type(Type type, Source source = {});
std::string type_name(Type type);
Type decimal_literal_type(std::string_view text, Source source = {});
disk::Value parse_literal(Type type, std::string_view text, Source source = {});
// PL/SQL RECORD and COLLECTION values are canonical tagged JSON stored as bounded bytes.
disk::Value parse_compound(Type type, std::string_view text, Source source = {});
bool can_coerce(Type from, Type to) noexcept;
disk::Value coerce(const disk::Value& value, Type from, Type to, Source source = {});
disk::Value negate_numeric(const disk::Value& value, Type type, Source source = {});
Truth truth(const disk::Value& value, Source source = {});
Truth logical_not(Truth value) noexcept;
Truth logical_and(Truth left, Truth right) noexcept;
Truth logical_or(Truth left, Truth right) noexcept;
disk::Value boolean_value(Truth value);
bool comparable(Type left, Type right) noexcept;
int compare_non_null(const disk::Value& left, Type left_type, const disk::Value& right, Type right_type,
    Source source = {});
}
