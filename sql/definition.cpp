#include "definition.hpp"
#include "relational.hpp"
#include "executor.hpp"
#include "routines.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>

namespace sdp::sql
{
namespace
{
using wire::Json;
using Op = Expr::Op;

[[noreturn]] void damaged()
{
    throw disk::Error(disk::Fault::corrupt, "Invalid SQL object definition");
}

int64_t sequence_value(std::string_view definition)
{
    int64_t value{};
    const char* end = definition.data() + definition.size();
    const auto parsed = std::from_chars(definition.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value <= 0) damaged();
    return value;
}

int64_t increment(Catalog& catalog, const Object& object)
{
    const auto value = sequence_value(object.definition);
    if (value == (std::numeric_limits<int64_t>::max)())
        throw Error(Code::range, {}, "Sequence exhausted");
    catalog.update_definition(object, std::to_string(value + 1));
    return value;
}

Json view_definition(const Object& view)
{
    const auto shape = Json::parse(view.definition, nullptr, false);
    if (!shape.is_object() || !shape.contains("base") || !shape["base"].is_number_integer() ||
        !shape.contains("columns") || !shape["columns"].is_array() || shape["columns"].empty() ||
        shape["columns"].size() > 60)
        damaged();
    return shape;
}

ViewShape view_projection(Catalog& catalog, const Statement& statement)
{
    const auto query = parse(statement.view_sql);
    if (query.action != Action::select || query.target.parts.empty() || query.filter ||
        std::ranges::any_of(query.projection, [](const Expr& expr) { return expr.op != Op::name; }))
        throw Error(Code::unsupported, statement.source, "Views require a named projection of one base table");
    auto table = catalog.resolve(ObjectKind::table, query.target);
    auto plan = std::get<SelectPlan>(sdp::sql::bind(catalog, query));
    std::vector<size_t> columns;
    std::set<size_t> seen;
    for (const auto& column : plan.projection)
    {
        if (!seen.insert(column.expression.column).second)
            throw Error(Code::name, column.expression.source, "View column is repeated");
        columns.push_back(column.expression.column);
    }
    return {std::move(table), std::move(columns)};
}

Expr decode_expression(const Json& encoded, size_t depth)
{
    if (depth > 64 || !encoded.is_array() || encoded.size() != 7 ||
        !encoded[0].is_number_unsigned() || !encoded[1].is_string() || !encoded[2].is_array() ||
        !encoded[3].is_array() || !encoded[4].is_number_unsigned() || !encoded[5].is_number_unsigned() ||
        !encoded[6].is_number_unsigned())
        damaged();
    const auto op = encoded[0].get<uint64_t>();
    if (op > static_cast<uint64_t>(Op::next_value) || encoded[2].size() > 4 || encoded[3].size() > 2)
        damaged();
    Source location{encoded[4].get<size_t>(), encoded[5].get<size_t>(), encoded[6].get<size_t>()};
    if (!location.line || !location.column) damaged();
    Expr out{static_cast<Op>(op), location, encoded[1].get<std::string>(), Name{{}, location}, {}};
    for (const auto& name : encoded[2])
    {
        if (!name.is_string()) damaged();
        out.name.parts.push_back(name.get<std::string>());
    }
    for (const auto& nested : encoded[3]) out.args.push_back(decode_expression(nested, depth + 1));
    return out;
}
}

Json write_expression(const Expr& expr)
{
    auto arguments = Json::array();
    for (const auto& arg : expr.args) arguments.push_back(write_expression(arg));
    return Json::array({static_cast<unsigned>(expr.op), expr.text, expr.name.parts, std::move(arguments),
        expr.source.offset, expr.source.line, expr.source.column});
}

Expr read_expression(const Json& encoded)
{
    return decode_expression(encoded, 1);
}

ViewShape read_view(Catalog& catalog, const Object& view)
{
    if (view.kind != ObjectKind::view) throw Error(Code::type, {}, "Expected a view object");
    const auto shape = view_definition(view);
    const auto id = shape["base"].get<ObjectId>();
    const auto objects = catalog.list();
    const auto found = std::ranges::find(objects, id, &Object::id);
    if (found == objects.end() || found->kind != ObjectKind::table) damaged();
    std::vector<size_t> columns;
    std::set<size_t> seen;
    for (const auto& value : shape["columns"])
    {
        if (!value.is_number_unsigned()) damaged();
        const auto index = value.get<size_t>();
        if (index >= found->columns.size() || !seen.insert(index).second) damaged();
        columns.push_back(index);
    }
    return {*found, std::move(columns)};
}

std::vector<ObjectId> object_dependencies(const Object& object)
{
    if (object.kind == ObjectKind::view) return {view_definition(object)["base"].get<ObjectId>()};
    if (object.kind == ObjectKind::routine) return routine_dependencies(object);
    return relational_dependencies(object);
}

bool has_view_dependency(Catalog& catalog, ObjectId table)
{
    for (const auto& object : catalog.list())
    {
        if (object.kind == ObjectKind::view && view_definition(object)["base"].get<ObjectId>() == table)
            return true;
    }
    return false;
}

bool auxiliary_ddl(Catalog& catalog, const Statement& statement, bool apply)
{
    switch (statement.action)
    {
    case Action::analyze:
        catalog.resolve(ObjectKind::table, statement.target);
        return !apply;
    case Action::create_view:
    {
        catalog.validate_auxiliary(ObjectKind::view, statement.target);
        auto shape = view_projection(catalog, statement);
        if (apply)
        {
            auto value = Json{{"base", shape.table.id}, {"columns", shape.columns}, {"sql", statement.view_sql}};
            catalog.create_auxiliary(ObjectKind::view, statement.target, value.dump());
        }
        return true;
    }
    case Action::create_sequence:
        catalog.validate_auxiliary(ObjectKind::sequence, statement.target);
        if (apply) catalog.create_auxiliary(ObjectKind::sequence, statement.target, "1");
        return true;
    case Action::drop_view:
    case Action::drop_sequence:
    {
        const auto kind = statement.action == Action::drop_view ? ObjectKind::view : ObjectKind::sequence;
        const auto object = catalog.resolve(kind, statement.target);
        if (kind == ObjectKind::sequence)
        {
            for (const auto& item : catalog.list())
            {
                const auto referenced = relational_dependencies(item);
                if (std::ranges::find(referenced, object.id) != referenced.end())
                    throw Error(Code::constraint, statement.source, "Sequence is used by another object");
            }
        }
        if (apply) catalog.remove(object);
        return true;
    }
    default:
        return false;
    }
}

void analyze_table(disk::Transaction& transaction, disk::Owner owner, Catalog& catalog, const Name& name,
    const std::function<void()>& check)
{
    const auto table = catalog.resolve(ObjectKind::table, name);
    size_t rows = 0;
    scan_table(transaction, owner, table, [&](disk::RowId, const RowValues&) { ++rows; return true; }, check);
    auto definition = table.definition.empty()
        ? Json{{"relational_table", 1}, {"defaults", Json::array()}, {"constraints", Json::array()}}
        : Json::parse(table.definition, nullptr, false);
    if (!definition.is_object()) damaged();
    definition["statistics"] = {{"rows", rows}, {"schema_version", table.version}};
    catalog.update_definition(table, definition.dump());
}

int64_t advance_sequence(Catalog& catalog, const Name& sequence)
{
    return increment(catalog, catalog.resolve(ObjectKind::sequence, sequence));
}

int64_t advance_sequence(Catalog& catalog, ObjectId sequence)
{
    const auto snapshot = catalog.list();
    const auto found = std::ranges::find(snapshot, sequence, &Object::id);
    if (found == snapshot.end() || found->kind != ObjectKind::sequence) damaged();
    return increment(catalog, *found);
}
}
