#include "access.hpp"
#include "definition.hpp"

#include <algorithm>
#include <set>

namespace sdp::sql
{
namespace
{
using Names = std::set<std::string, std::less<>>;
void collect(Catalog&, const Statement&, Names, std::vector<Requirement>&);

void inspect(Catalog& catalog, const Expr& expr, const Names& names, std::vector<Requirement>& requirements)
{
    if (expr.query) collect(catalog, *expr.query, names, requirements);
    if (expr.op == Expr::Op::routine_call)
    {
        const auto routine = catalog.resolve(ObjectKind::routine, expr.name);
        requirements.push_back(object_access(catalog.list(), routine, "execute"));
    }
    for (const auto& operand : expr.args) inspect(catalog, operand, names, requirements);
}

void read_relation(Catalog& catalog, const Name& name, const Names& names, std::vector<Requirement>& requirements)
{
    if (name.parts.empty() || (name.parts.size() == 1 && names.contains(name.parts.front()))) return;
    const auto target = catalog.resolve_relation(name);
    const auto objects = catalog.list();
    requirements.push_back(object_access(objects, target, "select"));
    if (target.kind == ObjectKind::view)
        requirements.push_back(object_access(objects, read_view(catalog, target).table, "select"));
}

void collect(Catalog& catalog, const Statement& statement, Names names, std::vector<Requirement>& requirements)
{
    for (const auto& item : statement.ctes)
    {
        collect(catalog, *item.query, names, requirements);
        names.insert(item.name);
    }
    if (statement.query) collect(catalog, *statement.query, names, requirements);
    read_relation(catalog, statement.target, names, requirements);
    for (const auto& join : statement.joins)
    {
        read_relation(catalog, join.target, names, requirements);
        if (join.on) inspect(catalog, *join.on, names, requirements);
    }
    if (statement.filter) inspect(catalog, *statement.filter, names, requirements);
    if (statement.having) inspect(catalog, *statement.having, names, requirements);
    for (const auto& expr : statement.projection) inspect(catalog, expr, names, requirements);
    for (const auto& expr : statement.group_by) inspect(catalog, expr, names, requirements);
    for (const auto& term : statement.order_by) inspect(catalog, term.expression, names, requirements);
    for (const auto& arm : statement.unions) collect(catalog, *arm.query, names, requirements);
}
}

Requirement object_access(std::span<const Object> snapshot, const Object& object, std::string_view verb)
{
    if (object.kind == ObjectKind::database) return {verb, object.id, 0, object.id};
    if (object.kind == ObjectKind::schema) return {verb, object.id, object.id, object.parent};
    const auto parent = std::ranges::find(snapshot, object.parent, &Object::id);
    if (parent == snapshot.end() || parent->kind != ObjectKind::schema)
        throw disk::Error(disk::Fault::corrupt, "Permission target has no schema");
    return {verb, object.id, parent->id, parent->parent};
}

std::vector<Requirement> access_requirements(Catalog& catalog, const Statement& statement)
{
    std::vector<Requirement> requirements;
    switch (statement.action)
    {
    case Action::select:
    case Action::explain:
    case Action::explain_analyze:
        collect(catalog, statement, {}, requirements);
        break;
    case Action::call_routine:
    {
        const auto routine = catalog.resolve(ObjectKind::routine, statement.target);
        requirements.push_back(object_access(catalog.list(), routine, "execute"));
        for (const auto& argument : statement.projection) inspect(catalog, argument, {}, requirements);
        break;
    }
    case Action::pl_block:
        break;
    case Action::insert:
    case Action::update:
    case Action::delete_:
    {
        const auto table = catalog.resolve(ObjectKind::table, statement.target);
        const char* verb = statement.action == Action::insert ? "insert" :
            statement.action == Action::update ? "update" : "delete";
        requirements.push_back(object_access(catalog.list(), table, verb));
        break;
    }
    default:
        requirements.push_back({"ddl"});
        break;
    }
    return requirements;
}

void enforce(const Security& security, const Principal& actor, std::span<const Requirement> requirements)
{
    for (const auto& required : requirements)
        if (!security.allowed(actor, required.verb, required.target, required.schema, required.database))
            throw wire::Error(wire::Code::permission_denied);
}
}
