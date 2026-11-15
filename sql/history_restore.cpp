#include "history_restore.hpp"
#include "definition.hpp"
#include "routines.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <openssl/rand.h>

namespace sdp::sql
{
namespace
{
using K = HistoryCommand::Kind;
using E = HistoryPlan::CatalogEdit;
using W = wire::Code;
using wire::Json;

[[noreturn]] void conflict() { throw HistoryError(W::invalid_state, "restore_conflict"); }
[[noreturn]] void schema_conflict() { throw HistoryError(W::invalid_state, "schema_conflict"); }

const Object* by_id(std::span<const Object> objects, ObjectId id)
{
    const auto found = std::ranges::find(objects, id, &Object::id);
    return found == objects.end() ? nullptr : &*found;
}

bool same_columns(const Object& a, const Object& b)
{
    return std::ranges::equal(a.columns, b.columns, [](const Column& left, const Column& right) {
        return left.name == right.name && left.type == right.type && left.nullable == right.nullable;
    });
}

size_t column_at(const Object& object, std::string_view field)
{
    const auto column = std::ranges::find(object.columns, field, &Column::name);
    if (column == object.columns.end()) schema_conflict();
    return static_cast<size_t>(column - object.columns.begin());
}

bool permitted(const Security& security, const Principal& actor, const Requirement& required)
{
    return security.allowed(actor, required.verb, required.target, required.schema, required.database);
}

std::string plan_id()
{
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw wire::Error(W::internal);
    constexpr std::string_view hex = "0123456789abcdef";
    std::string id;
    id.reserve(32);
    for (auto byte : bytes)
    {
        id += hex[byte >> 4];
        id += hex[byte & 15];
    }
    return id;
}

std::optional<RowValues> row_values(const HistoricalRows& rows, const Object* schema, disk::RowId id)
{
    const auto found = rows.find(id);
    if (!schema || found == rows.end()) return {};
    return read_history_values(*schema, found->second);
}

std::optional<RowValues> reverted(const std::optional<RowValues>& prior, const std::optional<RowValues>& later,
    const std::optional<RowValues>& current, std::optional<size_t> selected)
{
    if (prior == later) return current;
    if (!prior || !later)
    {
        if (selected) conflict();
        if (current == prior) return current;
        if (current != later) conflict();
        return prior;
    }
    if (!current) conflict();
    auto result = *current;
    for (size_t index = 0; index < prior->size(); ++index)
    {
        if (selected && index != *selected) continue;
        if ((*prior)[index] == (*later)[index]) continue;
        if (result[index] == (*later)[index]) result[index] = (*prior)[index];
        else if (result[index] != (*prior)[index]) conflict();
    }
    return result;
}

std::optional<RowValues> restored(const std::optional<RowValues>& wanted, const std::optional<RowValues>& current,
    std::optional<size_t> field, std::optional<size_t> source_field)
{
    if (!field) return wanted;
    if (!wanted || !current) conflict();
    auto result = *current;
    result[*field] = (*wanted)[*source_field];
    return result;
}

void plan_rows(HistoryPlan& plan, HistoryView& view, const HistoryCommand& command,
    const Object* prior, const Object* later, const Object* current, const WorkCheck& check)
{
    const bool revert = command.kind == K::revert;
    const Object* wanted = revert ? prior : later;
    if (!wanted && !current) conflict();
    if (command.row_id && !wanted && plan.catalog_edit == E::drop_table) schema_conflict();

    std::optional<size_t> field, source_field;
    if (!command.field.empty())
    {
        if (!wanted || !current || (revert && !later)) schema_conflict();
        field = column_at(*current, command.field);
        source_field = column_at(*wanted, command.field);
        if (current->columns[*field].type != wanted->columns[*source_field].type) schema_conflict();
        if (revert && (!same_columns(*wanted, *later) || !same_columns(*later, *current))) schema_conflict();
    }
    else
    {
        if (wanted && current && !same_columns(*wanted, *current)) schema_conflict();
        if (prior && later && revert && !same_columns(*prior, *later)) schema_conflict();
    }

    const auto id = plan.object.id;
    const auto previous = prior ? view.rows_at(id, command.revision - 1, command.row_id) : HistoricalRows{};
    const auto selected = later ? view.rows_at(id, command.revision, command.row_id) : HistoricalRows{};
    const auto active = current ? view.rows_at(id, view.state().head, command.row_id) : HistoricalRows{};
    if (!revert && command.row_id && !selected.contains(command.row_id)) conflict();
    if (plan.catalog_edit == E::drop_table)
    {
        if (active != selected) conflict();
        if (active.size() > 512) throw wire::Error(W::resource_limit);
        for (const auto& [row, value] : active)
            plan.rows.push_back({row, read_history_values(*current, value), {}});
        return;
    }

    std::set<disk::RowId> ids;
    for (const auto& [row, value] : previous) ids.insert(row);
    for (const auto& [row, value] : selected) ids.insert(row);
    if (!revert) for (const auto& [row, value] : active) ids.insert(row);
    size_t bytes{};
    for (const auto row : ids)
    {
        check();
        auto old = row_values(active, current, row);
        const auto source = row_values(selected, later, row);
        const auto original = row_values(previous, prior, row);
        if (field && !command.row_id && (!old || !source || (revert && !original))) continue;
        auto change = revert ? reverted(original, source, old, field)
                             : restored(source, old, field, source_field);
        if (change == old) continue;
        if (old) bytes += disk::encode({1, *old}).size();
        if (change) bytes += disk::encode({1, *change}).size();
        if (plan.rows.size() == 512 || bytes > disk::change_budget) throw wire::Error(W::resource_limit);
        plan.rows.push_back({row, std::move(old), std::move(change)});
    }
}

std::optional<std::string> routine_source(const Object* object)
{
    if (!object) return {};
    const auto record = Json::parse(object->definition, nullptr, false);
    if (!record.is_object() || !record.contains("sql") || !record.at("sql").is_string())
        throw disk::Error(disk::Fault::corrupt, "Invalid historical routine source");
    return record.at("sql").get<std::string>();
}

bool dependency_visible(std::span<const Object> current, const Object& object, const Security& security,
    const Principal& actor)
{
    if (actor.administrator) return true;
    const auto* active = by_id(current, object.id);
    if (!active) return false;
    for (const auto verb : {"ddl", "select", "execute"})
        if (permitted(security, actor, object_access(current, *active, verb))) return true;
    return false;
}

Json dependencies_for(const Object& object, std::span<const Object> historic, std::span<const Object> current,
    const Security& security, const Principal& actor)
{
    auto result = Json::array();
    std::set<std::pair<ObjectId, std::string_view>> emitted;
    const auto add = [&](std::span<const Object> snapshot, const Object& related, std::string_view relationship) {
        if (related.id == object.id || !dependency_visible(current, related, security, actor) ||
            !emitted.emplace(related.id, relationship).second)
            return;
        result.push_back({{"object_id", std::to_string(related.id)}, {"name", Catalog::qualified(related, snapshot).parts},
            {"relationship", relationship}});
    };
    for (const auto snapshot : {historic, current})
    {
        const auto* candidate = by_id(snapshot, object.id);
        if (!candidate) continue;
        for (auto required : object_dependencies(*candidate))
            if (const auto* owner = by_id(snapshot, required)) add(snapshot, *owner, "requires");
        for (const auto& possible : snapshot)
        {
            const auto refs = object_dependencies(possible);
            if (std::ranges::find(refs, object.id) != refs.end()) add(snapshot, possible, "dependent");
        }
    }
    return result;
}

void append_requirement(HistoryPlan& plan, std::string_view verb, std::span<const Object> current)
{
    const auto requirement = object_access(current, plan.object, verb);
    if (std::ranges::none_of(plan.required, [&](const auto& item) { return item.verb == verb; }))
        plan.required.push_back(requirement);
}

Json effects_for(HistoryPlan& plan, std::span<const Object> current)
{
    size_t inserted{}, updated{}, deleted{};
    auto rows = Json::array();
    for (const auto& mutation : plan.rows)
    {
        const auto verb = !mutation.before ? "insert" : !mutation.after ? "delete" : "update";
        if (!mutation.before) ++inserted;
        else if (!mutation.after) ++deleted;
        else ++updated;
        append_requirement(plan, verb, current);
        auto fields = Json::array();
        if (mutation.before && mutation.after)
            for (size_t index = 0; index < mutation.before->size(); ++index)
                if ((*mutation.before)[index] != (*mutation.after)[index])
                    fields.push_back(plan.object.columns[index].name);
        rows.push_back({{"row_id", std::to_string(mutation.id)}, {"operation", verb}, {"fields", std::move(fields)}});
    }
    return {{"inserted", inserted}, {"updated", updated}, {"deleted", deleted},
        {"catalog", plan.catalog_edit == E::none ? 0 : 1}, {"rows", std::move(rows)}};
}
}

HistoryPlan prepare_history_plan(HistoryView& view, const HistoryCommand& command,
    const Security& security, const Principal& actor, const WorkCheck& check)
{
    const bool revert = command.kind == K::revert;
    view.require_revision(command.revision);
    if (revert) view.require_revision(command.revision - 1);
    const auto& current = view.current_catalog();
    const auto after = view.catalog_at(command.revision);
    const auto before = revert ? view.catalog_at(command.revision - 1) : std::vector<Object>{};
    const auto named = history_target(current, command.target);
    const auto earlier = history_target(before, command.target);
    const auto later = history_target(after, command.target);
    const auto* identity = later ? later : earlier;
    if (!identity) throw wire::Error(W::permission_denied);
    const auto* active = by_id(current, identity->id);
    if (!active && !actor.administrator) throw wire::Error(W::permission_denied);
    if (active)
    {
        const auto history = object_access(current, *active, "history");
        const auto read = object_access(current, *active, active->kind == ObjectKind::routine ? "execute" : "select");
        if (!permitted(security, actor, history) || !permitted(security, actor, read))
            throw wire::Error(W::permission_denied);
    }
    if ((named && named->id != identity->id) || (earlier && later && earlier->id != later->id))
        throw HistoryError(W::invalid_state, "object_identity_changed");
    if (identity->kind != ObjectKind::table && identity->kind != ObjectKind::routine)
        throw wire::Error(W::unsupported);
    if (!by_id(current, identity->parent)) schema_conflict();

    const auto* wanted = revert ? earlier : later;
    const auto* schema = active ? active : identity;
    HistoryPlan plan{};
    plan.id = plan_id();
    plan.mode = command.kind;
    plan.head = view.state().head;
    plan.revision = command.revision;
    plan.object = *schema;
    append_requirement(plan, "history", current);
    append_requirement(plan, identity->kind == ObjectKind::table ? "select" : "execute", current);

    if (identity->kind == ObjectKind::routine)
    {
        if (command.row_id || !command.field.empty()) throw wire::Error(W::invalid_argument);
        const auto old_source = routine_source(wanted);
        const auto new_source = routine_source(later);
        const auto live_source = routine_source(active);
        if (revert && old_source == new_source) conflict();
        if (revert && live_source != old_source && live_source != new_source) conflict();
        if (live_source != old_source)
        {
            plan.catalog_edit = old_source ? E::restore_routine : E::drop_routine;
            if (wanted) plan.object = *wanted;
        }
    }
    else
    {
        if (!active && wanted)
        {
            if (command.row_id || !command.field.empty()) schema_conflict();
            plan.catalog_edit = E::restore_table;
            const auto& source = revert ? before : after;
            for (const auto& candidate : source)
            {
                if (candidate.kind != ObjectKind::index) continue;
                const auto refs = object_dependencies(candidate);
                if (std::ranges::find(refs, wanted->id) != refs.end()) plan.indexes.push_back(candidate);
            }
        }
        if (revert && !wanted && active)
        {
            if (command.row_id || !command.field.empty() || !same_columns(*active, *later) ||
                active->definition != later->definition)
                schema_conflict();
            plan.catalog_edit = E::drop_table;
        }
        plan_rows(plan, view, command, earlier, later, active, check);
    }
    if (plan.catalog_edit != E::none) append_requirement(plan, "ddl", current);
    const auto name_in = by_id(current, schema->id) ? std::span<const Object>(current) :
        by_id(after, schema->id) ? std::span<const Object>(after) : std::span<const Object>(before);
    plan.preview = {{"plan_id", plan.id}, {"mode", revert ? "revert" : "restore"},
        {"current_revision", std::to_string(plan.head)}, {"revision", std::to_string(plan.revision)},
        {"object_id", std::to_string(schema->id)}, {"name", Catalog::qualified(*schema, name_in).parts},
        {"row_id", command.row_id ? Json(std::to_string(command.row_id)) : Json()},
        {"field", command.field.empty() ? Json() : Json(command.field)},
        {"dependencies", dependencies_for(*schema, wanted ? (revert ? before : after) : after, current, security, actor)},
        {"effects", effects_for(plan, current)}};
    enforce(security, actor, plan.required);
    if (plan.rows.empty() && plan.catalog_edit == E::none) conflict();
    return plan;
}

void apply_history_plan(disk::Transaction& tx, disk::Owner owner, const HistoryPlan& plan,
    const Security& security, const Principal& actor, const WorkCheck& check)
{
    enforce(security, actor, plan.required);
    Catalog catalog(tx, owner, check);
    const auto invoker = [&](std::span<const Requirement> required) { enforce(security, actor, required); };
    try
    {
        switch (plan.catalog_edit)
        {
        case E::restore_table:
            restore_history_table(tx, owner, plan.object, plan.indexes, check);
            break;
        case E::drop_table:
        {
            Statement drop{};
            drop.action = Action::drop_table;
            drop.target = catalog.qualified(plan.object);
            const auto bound = sdp::sql::bind(catalog, drop, {});
            (void)execute_write(tx, owner, bound, {}, check, invoker);
            return;
        }
        case E::restore_routine:
            restore_history_routine(catalog, plan.object);
            return;
        case E::drop_routine:
        {
            for (const auto& object : catalog.list())
            {
                const auto deps = object_dependencies(object);
                if (object.id != plan.object.id && std::ranges::find(deps, plan.object.id) != deps.end()) conflict();
            }
            catalog.remove(plan.object);
            return;
        }
        case E::none: break;
        }
        apply_history_rows(tx, owner, plan.object, plan.rows, check, invoker);
    }
    catch (const Error& error)
    {
        if (error.code() == Code::constraint) conflict();
        if (error.code() == Code::name || error.code() == Code::type || error.code() == Code::range)
            schema_conflict();
        throw;
    }
}
}
