#pragma once

#include "definition.hpp"
#include "relational.hpp"

#include <array>
#include <optional>

namespace sdp::sql
{
namespace editor_metadata
{
inline std::string quote_sql_value(std::string_view text, char delimiter)
{
    std::string out(1, delimiter);
    for (const auto ch : text)
    {
        out.push_back(ch);
        if (ch == delimiter) out.push_back(ch);
    }
    out.push_back(delimiter);
    return out;
}

inline std::optional<std::string> qualified(const Name& name)
{
    if (name.parts.empty()) return std::nullopt;
    std::string out;
    for (const auto& part : name.parts)
    {
        if (!out.empty()) out += '.';
        out += quote_sql_value(part, '"');
    }
    return out;
}

inline std::optional<std::string> expression(const Expr& expr, size_t depth = 0)
{
    using Op = Expr::Op;
    if (depth > 24) return std::nullopt;

    switch (expr.op)
    {
    case Op::null: return "NULL";
    case Op::boolean:
        if (expr.text == "true" || expr.text == "TRUE") return "TRUE";
        if (expr.text == "false" || expr.text == "FALSE") return "FALSE";
        return std::nullopt;
    case Op::integer:
        (void)parse_literal({Kind::integer}, expr.text);
        return expr.text;
    case Op::decimal:
        (void)parse_literal(decimal_literal_type(expr.text), expr.text);
        return expr.text;
    case Op::text: return quote_sql_value(expr.text, '\'');
    case Op::date: return "DATE " + quote_sql_value(expr.text, '\'');
    case Op::time: return "TIME " + quote_sql_value(expr.text, '\'');
    case Op::timestamp_tz: return "TIMESTAMP WITH TIME ZONE " + quote_sql_value(expr.text, '\'');
    case Op::name: return qualified(expr.name);
    case Op::next_value:
        if (const auto id = qualified(expr.name)) return "NEXT VALUE FOR " + *id;
        return std::nullopt;
    default: break;
    }

    if (expr.args.size() == 1)
    {
        const auto arg = expression(expr.args.front(), depth + 1);
        if (!arg) return std::nullopt;
        switch (expr.op)
        {
        case Op::negate: return "(-" + *arg + ")";
        case Op::logical_not: return "(NOT " + *arg + ")";
        case Op::is_null: return "(" + *arg + " IS NULL)";
        case Op::is_not_null: return "(" + *arg + " IS NOT NULL)";
        default: return std::nullopt;
        }
    }
    if (expr.args.size() != 2) return std::nullopt;
    const auto left = expression(expr.args[0], depth + 1);
    const auto right = expression(expr.args[1], depth + 1);
    if (!left || !right) return std::nullopt;
    static constexpr std::array operators{
        std::pair{Op::and_, " AND "}, std::pair{Op::or_, " OR "},
        std::pair{Op::eq, " = "}, std::pair{Op::ne, " <> "}, std::pair{Op::lt, " < "},
        std::pair{Op::le, " <= "}, std::pair{Op::gt, " > "}, std::pair{Op::ge, " >= "},
        std::pair{Op::add, " + "}, std::pair{Op::subtract, " - "},
        std::pair{Op::multiply, " * "}, std::pair{Op::divide, " / "},
    };
    for (const auto& [op, token] : operators)
        if (op == expr.op) return "(" + *left + token + *right + ")";
    return std::nullopt;
}

inline void add_expression_sql(wire::Json& item, const char* input, const char* output)
{
    if (const auto found = item.find(input); found != item.end())
        if (const auto source = expression(read_expression(*found)); source && source->size() <= 16 * 1024)
            item[output] = *source;
}
}
}
