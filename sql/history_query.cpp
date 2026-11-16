#include "history_query.hpp"
#include "wire_values.hpp"

#include <algorithm>
#include <memory>
#include <set>

namespace sdp::sql
{
namespace
{
using wire::Json;
using W = wire::Code;
using Objects = std::map<ObjectId, Object>;

std::unique_ptr<Object> catalog_image(disk::RowId id, const disk::Bytes& bytes)
{
    if (bytes.empty()) return {};
    auto object = Catalog::history_object(id, disk::decode(bytes));
    if (!object) return {};
    return std::make_unique<Object>(std::move(*object));
}

void change_catalog(Objects& objects, const disk::Change& change, bool forward)
{
    const auto& image = forward ? change.after : change.before;
    const auto& other = forward ? change.before : change.after;
    if (auto object = catalog_image(change.id, image))
    {
        objects.insert_or_assign(object->id, std::move(*object));
        return;
    }
    if (auto previous = catalog_image(change.id, other)) objects.erase(previous->id);
}

std::vector<Object> list_objects(const Objects& objects)
{
    std::vector<Object> result;
    result.reserve(objects.size());
    for (const auto& [id, object] : objects) result.push_back(object);
    return result;
}

Objects index_objects(std::span<const Object> values)
{
    Objects objects;
    for (const auto& object : values) objects.emplace(object.id, object);
    return objects;
}

Name qualified_object(const Objects& objects, const Object& object)
{
    Name name{{object.name}, {}};
    auto parent = object.parent;
    while (parent)
    {
        const auto found = objects.find(parent);
        if (found == objects.end() || name.parts.size() == 3)
            throw disk::Error(disk::Fault::corrupt, "Invalid historical catalog hierarchy");
        name.parts.insert(name.parts.begin(), found->second.name);
        parent = found->second.parent;
    }
    return name;
}

std::optional<disk::Row> row_image(const disk::Bytes& bytes, ObjectId table)
{
    if (bytes.empty()) return {};
    auto row = disk::decode(bytes);
    if (const auto record = history_record(row); record && record->table == table) return row;
    return {};
}

std::string_view kind_name(ObjectKind kind)
{
    switch (kind)
    {
    case ObjectKind::database: return "database";
    case ObjectKind::schema: return "schema";
    case ObjectKind::table: return "table";
    case ObjectKind::index: return "index";
    case ObjectKind::view: return "view";
    case ObjectKind::routine: return "routine";
    case ObjectKind::sequence: return "sequence";
    case ObjectKind::decision: return "decision";
    case ObjectKind::model: return "model";
    }
    throw disk::Error(disk::Fault::corrupt, "Unknown history object kind");
}

Json object_image(const Object& object, bool definition)
{
    auto columns = Json::array();
    for (const auto& column : object.columns)
        columns.push_back({{"name", column.name}, {"type", type_name(column.type)}, {"nullable", column.nullable}});
    auto image = Json{{"name", object.name}, {"kind", kind_name(object.kind)}, {"schema_version", object.version},
        {"parent_id", std::to_string(object.parent)}, {"columns", std::move(columns)}};
    if (!definition || object.definition.empty()) return image;
    if (object.kind == ObjectKind::routine)
    {
        const auto routine = Json::parse(object.definition, nullptr, false);
        if (!routine.is_object() || !routine.contains("sql") || !routine.at("sql").is_string())
            throw disk::Error(disk::Fault::corrupt, "Invalid historical routine source");
        image["source"] = routine.at("sql");
    }
    else image["definition"] = object.definition;
    return image;
}

Json values_image(const Object& table, const disk::Row& row, bool project_all = false)
{
    const auto record = history_record(row);
    if (!record || record->table != table.id ||
        !compatible_row_layout(table, record->version, record->values.size()))
        throw disk::Error(disk::Fault::corrupt, "Historical row has no matching schema");
    auto values = Json::object();
    const auto count = project_all ? table.columns.size() : record->values.size();
    for (size_t i = 0; i < count; ++i)
    {
        const auto& column = table.columns[i];
        const auto value = i < record->values.size() ? record->values[i] : disk::Value{};
        if (const auto* blob = std::get_if<disk::Bytes>(&value); blob && column.type.kind == Kind::blob)
            values[column.name] = {{"type", "BLOB"}, {"size", blob->size()}, {"sha256", history_sha256(*blob)}, {"scope", "value"}};
        else values[column.name] = write_value(value, column.type);
    }
    return {{"schema_version", record->version}, {"values", std::move(values)}};
}

class Page
{
public:
    explicit Page(const HistoryCommand& command) : offset_(command.offset), limit_(command.limit ? command.limit : 32) {}

    bool add(Json value)
    {
        if (seen_++ < offset_) return true;
        const auto length = value.dump().size() + 1;
        if (rows.size() >= limit_ || bytes_ + length > 768 * 1024)
        {
            if (rows.empty()) throw wire::Error(W::resource_limit);
            next = seen_ - 1;
            return false;
        }
        bytes_ += length;
        rows.push_back(std::move(value));
        return true;
    }

    Json rows = Json::array();
    Json next;
private:
    uint64_t seen_{};
    uint32_t offset_;
    uint32_t limit_;
    size_t bytes_{};
};

const Object* object_by_id(std::span<const Object> catalog, ObjectId id)
{
    const auto found = std::ranges::find(catalog, id, &Object::id);
    return found == catalog.end() ? nullptr : &*found;
}

class Visibility
{
public:
    Visibility(const HistoryView& view, const Security& security, const Principal& actor)
        : current_(view.current_catalog()), security_(security), actor_(actor) {}
    bool global() const { return security_.allowed(actor_, "history"); }
    bool visible(ObjectId id) const
    {
        if (actor_.administrator) return true;
        const auto* object = object_by_id(current_, id);
        if (!object || !allowed(*object, "history")) return false;
        const auto verb = object->kind == ObjectKind::routine ? "execute" :
            object->kind == ObjectKind::table || object->kind == ObjectKind::view ? "select" : "ddl";
        return allowed(*object, verb);
    }
    bool definition(ObjectId id) const
    {
        if (actor_.administrator) return true;
        const auto* object = object_by_id(current_, id);
        return object && allowed(*object, "ddl");
    }
    void require(const Object* object) const
    {
        if (!object || !visible(object->id)) throw wire::Error(W::permission_denied);
    }
private:
    bool allowed(const Object& object, std::string_view verb) const
    {
        const auto access = object_access(current_, object, verb);
        return security_.allowed(actor_, access.verb, access.target, access.schema, access.database);
    }
    std::span<const Object> current_;
    const Security& security_;
    const Principal& actor_;
};

Json entry(const HistoryRevision& revision, const Object& object, const Name& name, disk::RowId row, uint32_t version,
    Json before, Json after)
{
    return {{"revision", std::to_string(revision.lsn)}, {"parent", std::to_string(revision.parent)},
        {"transaction", std::to_string(revision.transaction)}, {"actor", revision.actor.empty() ? Json() : Json(revision.actor)},
        {"commit_utc", revision.utc_seconds ? Json(*revision.utc_seconds) : Json()}, {"name", name.parts},
        {"kind", row ? "row" : kind_name(object.kind)}, {"object_id", std::to_string(object.id)},
        {"row_id", row ? Json(std::to_string(row)) : Json()}, {"schema_version", version}, {"before", std::move(before)},
        {"after", std::move(after)}};
}

Json query_log(HistoryView& view, const HistoryCommand& command, const Visibility& visibility, const WorkCheck& check)
{
    const auto& state = view.state();
    auto from = command.from ? command.from : state.floor;
    auto to = command.to ? command.to : state.head;
    if (command.from) view.require_revision(from);
    if (command.to) view.require_revision(to);
    if (from > to && state.head) throw wire::Error(W::invalid_argument);
    if (command.since_utc && command.until_utc && *command.since_utc > *command.until_utc)
        throw wire::Error(W::invalid_argument);

    ObjectId target{};
    if (command.target.parts.empty())
    {
        if (!visibility.global()) throw wire::Error(W::permission_denied);
    }
    else
    {
        const auto scope = view.catalog_at(to);
        if (const auto* object = history_target(scope, command.target))
        {
            visibility.require(object);
            target = object->id;
        }
        else
        {
            if (visibility.global())
            {
                auto prior = index_objects(scope);
                for (auto revision = view.revisions().rbegin(); revision != view.revisions().rend() && !target; ++revision)
                {
                    check();
                    if (revision->lsn > to) continue;
                    bool edited = false;
                    for (auto change = revision->changes.rbegin(); change != revision->changes.rend(); ++change)
                        if (change->kind == disk::ChangeKind::catalog)
                        {
                            change_catalog(prior, *change, false);
                            edited = true;
                        }
                    if (!edited) continue;
                    const auto snapshot = list_objects(prior);
                    if (const auto* object = history_target(snapshot, command.target); object && visibility.visible(object->id))
                        target = object->id;
                }
            }
            if (const auto* current = history_target(view.current_catalog(), command.target); !target && current)
            {
                visibility.require(current);
                target = current->id;
            }
            if (!target) throw wire::Error(W::permission_denied);
        }
    }

    auto objects = index_objects(view.catalog_at(from ? from - 1 : 0));
    Page page(command);
    bool space = true;
    for (const auto& revision : view.revisions())
    {
        if (revision.lsn < from) continue;
        if (!space || revision.lsn > to) break;
        check();
        // A rewritten row's before image may use any schema superseded in this revision.
        std::map<ObjectId, std::vector<Object>> prior_layouts;
        const bool date = (!command.since_utc || (revision.utc_seconds && *revision.utc_seconds >= *command.since_utc)) &&
            (!command.until_utc || (revision.utc_seconds && *revision.utc_seconds <= *command.until_utc));
        for (const auto& change : revision.changes)
        {
            check();
            auto before_object = catalog_image(change.id, change.before);
            auto after_object = catalog_image(change.id, change.after);
            if (before_object || after_object)
            {
                const auto& object = after_object ? *after_object : *before_object;
                if (before_object && after_object && before_object->kind == ObjectKind::table &&
                    before_object->version != after_object->version)
                    prior_layouts[object.id].push_back(*before_object);
                if (after_object) objects.insert_or_assign(object.id, object);
                if (date && !command.row_id && (!target || target == object.id) && visibility.visible(object.id))
                {
                    const auto qualified = qualified_object(objects, object);
                    const bool show_source = visibility.definition(object.id);
                    const auto version = object.version;
                    auto previous_image = before_object ? object_image(*before_object, show_source) : Json();
                    auto current_image = after_object ? object_image(*after_object, show_source) : Json();
                    space = page.add(entry(revision, object, qualified, 0, version,
                        std::move(previous_image), std::move(current_image)));
                }
                if (!after_object) objects.erase(object.id);
                if (!space) break;
                continue;
            }
            if (!date || (command.row_id && command.row_id != change.id)) continue;
            const auto before = change.before.empty() ? disk::Row{} : disk::decode(change.before);
            const auto after = change.after.empty() ? disk::Row{} : disk::decode(change.after);
            const auto previous = history_record(before);
            const auto next = history_record(after);
            if (!previous && !next) continue;
            const auto record = next ? *next : *previous;
            if ((target && target != record.table) || !visibility.visible(record.table)) continue;
            const auto found = objects.find(record.table);
            if (found == objects.end()) throw disk::Error(disk::Fault::corrupt, "Historical row has no table");
            const auto& table = found->second;
            const Object* before_table = &table;
            if (previous && !compatible_row_layout(table, previous->version, previous->values.size()))
            {
                if (const auto older = prior_layouts.find(record.table); older != prior_layouts.end())
                    for (auto layout = older->second.rbegin(); layout != older->second.rend(); ++layout)
                        if (compatible_row_layout(*layout, previous->version, previous->values.size()))
                        {
                            before_table = &*layout;
                            break;
                        }
            }
            space = page.add(entry(revision, table, qualified_object(objects, table), change.id, record.version,
                previous ? values_image(*before_table, before) : Json(), next ? values_image(table, after) : Json()));
            if (!space) break;
        }
    }
    return {{"current_revision", std::to_string(state.head)}, {"earliest_revision", std::to_string(state.floor)},
        {"entries", std::move(page.rows)}, {"next_offset", page.next}};
}

Json query_read(HistoryView& view, const HistoryCommand& command, const Visibility& visibility, const WorkCheck& check)
{
    view.require_revision(command.revision);
    const auto objects = view.catalog_at(command.revision);
    const auto* object = history_target(objects, command.target);
    visibility.require(object);
    const auto name = Catalog::qualified(*object, objects).parts;
    Page page(command);
    if (object->kind == ObjectKind::table)
    {
        for (const auto& [id, row] : view.rows_at(object->id, command.revision, command.row_id))
        {
            check();
            auto image = values_image(*object, row, true);
            image["row_id"] = std::to_string(id);
            if (!page.add(std::move(image))) break;
        }
    }
    const bool show_source = visibility.definition(object->id);
    return {{"revision", std::to_string(command.revision)}, {"object_id", std::to_string(object->id)}, {"name", name},
        {"object", object_image(*object, show_source)}, {"rows", std::move(page.rows)}, {"next_offset", page.next}};
}

Json query_diff(HistoryView& view, const HistoryCommand& command, const Visibility& visibility, const WorkCheck& check)
{
    view.require_revision(command.from);
    view.require_revision(command.to);
    if (command.from >= command.to) throw wire::Error(W::invalid_argument);
    const auto old_objects = view.catalog_at(command.from);
    const auto new_objects = view.catalog_at(command.to);
    const auto* before = history_target(old_objects, command.target);
    const auto* after = history_target(new_objects, command.target);
    if (!before && !after) throw wire::Error(W::permission_denied);
    const auto& object = after ? *after : *before;
    visibility.require(&object);
    if (before && after && before->id != after->id)
        throw HistoryError(W::invalid_state, "object_identity_changed");
    if (!before) before = object_by_id(old_objects, object.id);
    if (!after) after = object_by_id(new_objects, object.id);
    const auto name = Catalog::qualified(object, object_by_id(new_objects, object.id) ? new_objects : old_objects);
    const auto& revision = view.at(command.to);
    const bool show_source = visibility.definition(object.id);
    Page page(command);
    const auto old_definition = before ? object_image(*before, show_source) : Json();
    const auto new_definition = after ? object_image(*after, show_source) : Json();
    bool space = true;
    if (!command.row_id && old_definition != new_definition)
        space = page.add(entry(revision, object, name, 0, object.version, old_definition, new_definition));
    if (space && object.kind == ObjectKind::table)
    {
        auto old_rows = before ? view.rows_at(object.id, command.from, command.row_id) : HistoricalRows{};
        auto new_rows = after ? view.rows_at(object.id, command.to, command.row_id) : HistoricalRows{};
        std::set<disk::RowId> ids;
        for (const auto& [id, row] : old_rows) ids.insert(id);
        for (const auto& [id, row] : new_rows) ids.insert(id);
        for (const auto id : ids)
        {
            check();
            const auto a = old_rows.find(id), b = new_rows.find(id);
            if (a != old_rows.end() && b != new_rows.end() && a->second == b->second) continue;
            const auto image = b != new_rows.end() ? history_record(b->second) : history_record(a->second);
            if (!page.add(entry(revision, object, name, id, image->version,
                a == old_rows.end() ? Json() : values_image(*before, a->second),
                b == new_rows.end() ? Json() : values_image(*after, b->second)))) break;
        }
    }
    return {{"from_revision", std::to_string(command.from)}, {"to_revision", std::to_string(command.to)},
        {"earliest_revision", std::to_string(view.state().floor)}, {"entries", std::move(page.rows)}, {"next_offset", page.next}};
}
}

HistoryView::HistoryView(HistoryJournal& journal, disk::Transaction& tx, disk::Owner owner, WorkCheck check)
    : tx_(tx), owner_(owner), check_(std::move(check)), current_(Catalog(tx, owner, check_).list())
{
    state_ = journal.visit(0, 0,
        [&](HistoryRevision&& item) { revisions_.push_back(std::move(item)); });
}

std::vector<Object> HistoryView::catalog_at(uint64_t revision) const
{
    auto objects = index_objects(current_);
    for (auto frame = revisions_.rbegin(); frame != revisions_.rend() && frame->lsn > revision; ++frame)
    {
        check_();
        for (auto change = frame->changes.rbegin(); change != frame->changes.rend(); ++change)
            change_catalog(objects, *change, false);
    }
    return list_objects(objects);
}

HistoricalRows HistoryView::rows_at(ObjectId table, uint64_t revision, disk::RowId only) const
{
    HistoricalRows rows;
    size_t bytes{};
    const auto erase = [&](disk::RowId id) {
        const auto found = rows.find(id);
        if (found == rows.end()) return;
        bytes -= disk::encode(found->second).size();
        rows.erase(found);
    };
    const auto add = [&](disk::RowId id, disk::Row row) {
        erase(id);
        bytes += disk::encode(row).size();
        if (bytes > disk::wal_quota) throw wire::Error(W::resource_limit);
        rows.emplace(id, std::move(row));
    };
    visit_history_records(tx_, owner_, table, [&](disk::RowId id, const disk::Row& row) {
        if (!only || only == id) add(id, row);
    }, check_);
    for (auto frame = revisions_.rbegin(); frame != revisions_.rend() && frame->lsn > revision; ++frame)
    {
        check_();
        for (auto change = frame->changes.rbegin(); change != frame->changes.rend(); ++change)
        {
            if (only && change->id != only) continue;
            if (auto before = row_image(change->before, table)) add(change->id, std::move(*before));
            else if (row_image(change->after, table)) erase(change->id);
        }
    }
    return rows;
}

const HistoryRevision& HistoryView::at(uint64_t revision) const
{
    require_revision(revision);
    const auto found = std::ranges::lower_bound(revisions_, revision, {}, &HistoryRevision::lsn);
    if (found == revisions_.end() || found->lsn != revision)
        throw disk::Error(disk::Fault::corrupt, "History revision is missing");
    return *found;
}

void HistoryView::require_revision(uint64_t revision) const
{
    if (!revision || revision < state_.floor || revision > state_.head)
        throw HistoryError(W::invalid_state, "revision_not_recoverable");
}

const Object* history_target(std::span<const Object> catalog, const Name& name)
{
    if (name.parts.empty() || name.parts.size() > 3) return nullptr;
    auto candidates = name.parts;
    if (candidates.size() <= 2) candidates.insert(candidates.begin(), std::string(default_database));
    if (candidates.size() == 2) candidates.insert(candidates.begin() + 1, std::string(default_schema));
    const Object* scope{};
    for (const auto& object : catalog)
    {
        if (object.name != name.parts.back()) continue;
        const auto qualified = Catalog::qualified(object, catalog).parts;
        if (qualified == candidates) return &object;
        if (qualified == name.parts) scope = &object;
    }
    return scope;
}

Json history_status_json(const HistoryState& state, bool show_pins)
{
    auto pins = Json::array();
    auto backup_copies = Json::array();
    if (show_pins)
        for (const auto& pin : state.pins)
        {
            const auto info = Json{{"kind", pin.kind}, {"name", pin.name}, {"revision", std::to_string(pin.revision)}};
            pins.push_back(info);
            if (pin.kind == "backup" || pin.kind == "dataset") backup_copies.push_back(info);
        }
    return {{"current_revision", std::to_string(state.head)}, {"earliest_revision", std::to_string(state.floor)},
        {"quota_bytes", std::to_string(state.quota)}, {"retained_bytes", std::to_string(state.retained_bytes)},
        {"pins", std::move(pins)}, {"backup_copies_outside_purge", std::move(backup_copies)}};
}

Json query_history(HistoryView& view, const HistoryCommand& command,
    const Security& security, const Principal& actor, const WorkCheck& check)
{
    const Visibility visibility(view, security, actor);
    using Kind = HistoryCommand::Kind;
    switch (command.kind)
    {
    case Kind::log: return query_log(view, command, visibility, check);
    case Kind::diff: return query_diff(view, command, visibility, check);
    case Kind::read: return query_read(view, command, visibility, check);
    default: throw wire::Error(W::invalid_argument);
    }
}
}
