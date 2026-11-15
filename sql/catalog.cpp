#include "catalog.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace sdp::sql
{
namespace
{
// Reserved row-schema tag for SQL catalog records in the disk v1 heap.
constexpr uint32_t catalog_schema = 0x53444331;
constexpr ObjectId default_database_id = 1;
constexpr ObjectId default_schema_id = 2;
constexpr size_t max_objects = 1024;
constexpr size_t max_columns = 60;
constexpr size_t max_name = 128;
constexpr size_t max_definition = 16 * 1024;
constexpr size_t header_fields = 5;

[[noreturn]] void corrupt()
{
    throw disk::Error(disk::Fault::corrupt, "Invalid SQL catalog");
}

template<typename T>
const T& field(const disk::Row& row, size_t index)
{
    const auto* value = std::get_if<T>(&row.values[index]);
    if (!value) corrupt();
    return *value;
}

bool valid_utf8(std::string_view text)
{
    if (text.find('\0') != text.npos) return false;
    while (!text.empty())
    {
        const auto width = utf8_width(text);
        if (!width) return false;
        text.remove_prefix(width);
    }
    return true;
}

bool valid_name(std::string_view text) { return !text.empty() && text.size() <= max_name && valid_utf8(text); }

void check_name(const Name& name)
{
    if (name.parts.empty() || !std::ranges::all_of(name.parts, valid_name))
        throw Error(Code::name, name.source, "Invalid catalog identifier");
}

bool auxiliary(ObjectKind kind)
{
    return kind == ObjectKind::index || kind == ObjectKind::view || kind == ObjectKind::routine || kind == ObjectKind::sequence;
}

Object decode_object(disk::RowId row_id, const disk::Row& row)
{
    const auto size = row.values.size();
    if (size < header_fields || size > header_fields + max_columns * 2 || (size - header_fields) % 2)
        corrupt();
    const auto kind = field<int64_t>(row, 0);
    const auto parent = field<int64_t>(row, 1);
    const auto& name = field<std::string>(row, 2);
    const auto version = field<int64_t>(row, 3);
    const auto& definition = field<std::string>(row, 4);
    if (kind < static_cast<int64_t>(ObjectKind::database) || kind > static_cast<int64_t>(ObjectKind::sequence) ||
        parent < 0 || !valid_name(name) || version < 1 || version > std::numeric_limits<uint32_t>::max() ||
        definition.size() > max_definition || !valid_utf8(definition))
        corrupt();

    Object object{static_cast<ObjectId>(row_id), parent, static_cast<ObjectKind>(kind), name,
        static_cast<uint32_t>(version), {}, definition};
    const bool has_columns = size != header_fields;
    const bool has_definition = !definition.empty();
    if ((object.kind == ObjectKind::table) != has_columns ||
        (object.kind != ObjectKind::table && auxiliary(object.kind) != has_definition))
        corrupt();
    std::set<std::string> names;
    for (size_t slot = header_fields; slot < size; slot += 2)
    {
        const auto& column_name = field<std::string>(row, slot);
        const auto& descriptor = field<disk::Bytes>(row, slot + 1);
        if (!valid_name(column_name) || !names.insert(column_name).second || descriptor.size() != 4 ||
            descriptor[0] <= static_cast<uint8_t>(Kind::null) ||
            descriptor[0] > static_cast<uint8_t>(Kind::timestamp_tz) || descriptor[3] > 1)
            corrupt();
        const Type type{static_cast<Kind>(descriptor[0]), descriptor[1], descriptor[2]};
        try { validate_type(type, {}); }
        catch (const Error&) { corrupt(); }
        object.columns.push_back({column_name, type, descriptor[3] != 0,
            static_cast<uint32_t>(object.columns.size() + 1)});
    }
    return object;
}

disk::Row encode_object(const Object& object)
{
    disk::Row row{catalog_schema, {static_cast<int64_t>(object.kind), object.parent, object.name,
        static_cast<int64_t>(object.version), object.definition}};
    for (const auto& column : object.columns)
    {
        row.values.emplace_back(column.name);
        row.values.emplace_back(disk::Bytes{static_cast<uint8_t>(column.type.kind), column.type.precision,
            column.type.scale, static_cast<uint8_t>(column.nullable)});
    }
    return row;
}

using Objects = std::vector<Object>;

const Object* by_id(std::span<const Object> objects, ObjectId id)
{
    const auto found = std::ranges::lower_bound(objects, id, {}, &Object::id);
    return found != objects.end() && found->id == id ? &*found : nullptr;
}

const Object* child(const Objects& objects, ObjectId parent, std::string_view name)
{
    const auto found = std::ranges::find_if(objects, [&](const Object& object) {
        return object.parent == parent && object.name == name;
    });
    return found == objects.end() ? nullptr : &*found;
}

const Object& required(const Objects& objects, ObjectId parent, std::string_view name, ObjectKind kind, Source source)
{
    const auto* object = child(objects, parent, name);
    if (!object || object->kind != kind)
        throw Error(Code::name, source, "Unknown catalog object: " + std::string(name));
    return *object;
}

struct Path
{
    ObjectId parent;
    std::string_view leaf;
};

Path qualify(const Objects& objects, ObjectKind kind, const Name& name)
{
    check_name(name);
    const auto& parts = name.parts;
    if (kind == ObjectKind::database && parts.size() == 1) return {0, parts.back()};
    if (kind == ObjectKind::schema && parts.size() <= 2)
    {
        const auto database = parts.size() == 2
            ? required(objects, 0, parts.front(), ObjectKind::database, name.source).id
            : default_database_id;
        return {database, parts.back()};
    }
    if ((kind == ObjectKind::table || auxiliary(kind)) && parts.size() <= 3)
    {
        const auto database = parts.size() == 3
            ? required(objects, 0, parts.front(), ObjectKind::database, name.source).id
            : default_database_id;
        const auto schema = parts.size() >= 2
            ? required(objects, database, parts[parts.size() - 2], ObjectKind::schema, name.source).id
            : default_schema_id;
        return {schema, parts.back()};
    }
    throw Error(Code::name, name.source, "Invalid catalog qualification");
}

Object prepare_create(const Objects& objects, ObjectKind kind, const Name& name)
{
    const auto [parent, leaf] = qualify(objects, kind, name);
    if (child(objects, parent, leaf)) throw Error(Code::name, name.source, "Catalog object already exists");
    if (objects.size() >= max_objects + 2) throw Error(Code::limit, name.source, "Catalog object limit reached");
    return {0, parent, kind, std::string(leaf), 1, {}, {}};
}

void append_column(Object& table, const ColumnDefinition& column)
{
    check_name(column.name);
    const auto& name = column.name.parts.front();
    if (column.name.parts.size() != 1) throw Error(Code::name, column.name.source, "Column name must be unqualified");
    if (table.columns.size() == max_columns) throw Error(Code::limit, column.name.source, "Table column limit reached");
    if (std::ranges::find(table.columns, name, &Column::name) != table.columns.end())
        throw Error(Code::name, column.name.source, "Duplicate column identifier");
    if (column.type.kind == Kind::null) throw Error(Code::type, column.name.source, "Column needs a concrete type");
    validate_type(column.type, column.name.source);
    table.columns.push_back({name, column.type, column.nullable, static_cast<uint32_t>(table.columns.size() + 1)});
}

enum class Edit { insert, erase, update };
struct Ddl
{
    Edit edit;
    Object object;
};

Ddl plan(const Objects& objects, const Statement& statement)
{
    auto kind = ObjectKind::table;
    switch (statement.action)
    {
    case Action::create_database:
    case Action::drop_database: kind = ObjectKind::database; break;
    case Action::create_schema:
    case Action::drop_schema: kind = ObjectKind::schema; break;
    case Action::create_table:
    case Action::drop_table:
    case Action::add_column: break;
    default: throw Error(Code::unsupported, statement.source, "Statement is not a supported catalog operation");
    }

    const bool creating = statement.action == Action::create_database || statement.action == Action::create_schema ||
        statement.action == Action::create_table;
    if (creating)
    {
        auto object = prepare_create(objects, kind, statement.target);
        if (kind == ObjectKind::table)
        {
            if (statement.columns.empty()) throw Error(Code::syntax, statement.source, "A table needs at least one column");
            for (const auto& column : statement.columns) append_column(object, column);
            for (const auto& constraint : statement.constraints)
                if (constraint.kind == ConstraintDefinition::Kind::primary_key)
                    for (const auto& name : constraint.columns)
                    {
                        check_name(name);
                        if (name.parts.size() != 1)
                            throw Error(Code::name, name.source, "Key columns must be unqualified");
                        const auto found = std::ranges::find(object.columns, name.parts.front(), &Column::name);
                        if (found == object.columns.end())
                            throw Error(Code::name, name.source, "Unknown primary key column");
                        found->nullable = false;
                    }
        }
        return {Edit::insert, std::move(object)};
    }

    const auto [parent, leaf] = qualify(objects, kind, statement.target);
    auto object = required(objects, parent, leaf, kind, statement.target.source);
    if (statement.action == Action::add_column)
    {
        if (statement.columns.size() != 1) throw Error(Code::syntax, statement.source, "Expected one column definition");
        if (object.version == std::numeric_limits<uint32_t>::max())
            throw Error(Code::limit, statement.source, "Table schema version exhausted");
        append_column(object, statement.columns.front());
        ++object.version;
        return {Edit::update, std::move(object)};
    }
    if (object.id == default_database_id || object.id == default_schema_id)
        throw Error(Code::constraint, statement.target.source, "The default namespace is protected");
    if (std::ranges::any_of(objects, [&](const Object& other) { return other.parent == object.id; }))
        throw Error(Code::constraint, statement.target.source, "Catalog object has dependent objects");
    return {Edit::erase, std::move(object)};
}

void validate_hierarchy(const Objects& objects)
{
    std::set<std::pair<ObjectId, std::string_view>> names;
    for (const auto& object : objects)
    {
        if (!names.emplace(object.parent, object.name).second) corrupt();
        if (object.kind == ObjectKind::database)
        {
            if (object.parent) corrupt();
            continue;
        }
        const auto* parent = by_id(objects, object.parent);
        if (!parent || parent->kind != (object.kind == ObjectKind::schema ? ObjectKind::database : ObjectKind::schema))
            corrupt();
    }
}
}

std::optional<Object> Catalog::history_object(disk::RowId id, const disk::Row& row)
{
    if (row.schema != catalog_schema) return {};
    if (id < (uint64_t{1} << 32) || id > uint64_t{INT64_MAX}) corrupt();
    return decode_object(id, row);
}

std::vector<Object> Catalog::list() const
{
    Objects objects{
        {default_database_id, 0, ObjectKind::database, std::string(default_database), 1, {}, {}},
        {default_schema_id, default_database_id, ObjectKind::schema, std::string(default_schema), 1, {}, {}}};
    const disk::RowVisitor visit = [&](disk::RowId row_id, const disk::Row& row) {
        if (objects.size() >= max_objects + 2) corrupt();
        objects.push_back(decode_object(row_id, row));
    };
    if (transaction_)
        transaction_->scan(owner_, 0, [&](disk::RowId id, const disk::Row& row) {
            visit(id, row);
            return true;
        }, catalog_schema, check_);
    else disk::Editor(*store_).visit(visit, catalog_schema);
    std::ranges::sort(objects, {}, &Object::id);
    validate_hierarchy(objects);
    return objects;
}

Object Catalog::resolve(ObjectKind kind, const Name& name) const
{
    const auto objects = list();
    const auto [parent, leaf] = qualify(objects, kind, name);
    return required(objects, parent, leaf, kind, name.source);
}

Object Catalog::resolve_relation(const Name& name) const
{
    const auto objects = list();
    const auto [parent, leaf] = qualify(objects, ObjectKind::table, name);
    const auto* found = child(objects, parent, leaf);
    if (!found || (found->kind != ObjectKind::table && found->kind != ObjectKind::view))
        throw Error(Code::name, name.source, "Unknown table or view");
    return *found;
}

Object Catalog::validate_auxiliary(ObjectKind kind, const Name& name) const
{
    if (!auxiliary(kind)) throw Error(Code::unsupported, name.source, "Expected auxiliary catalog object");
    return prepare_create(list(), kind, name);
}

void Catalog::update_definition(const Object& object, std::string definition, bool revise)
{
    if (!transaction_) throw Error(Code::unsupported, {}, "Metadata view is read-only");
    if (object.kind != ObjectKind::table && !auxiliary(object.kind))
        throw Error(Code::type, {}, "Object has no stored definition");
    if ((auxiliary(object.kind) && definition.empty()) || definition.size() > max_definition)
        throw Error(Code::limit, {}, "Missing or oversized catalog definition");
    if (!valid_utf8(definition)) throw Error(Code::type, {}, "Invalid UTF-8 catalog definition");

    auto current = decode_object(static_cast<disk::RowId>(object.id),
        transaction_->get(owner_, static_cast<disk::RowId>(object.id)));
    if (current.kind != object.kind) corrupt();
    if (revise)
    {
        if (current.version == std::numeric_limits<uint32_t>::max()) throw Error(Code::limit, {}, "Catalog version exhausted");
        ++current.version;
    }
    current.definition = std::move(definition);
    transaction_->update(owner_, static_cast<disk::RowId>(current.id), encode_object(current), disk::ChangeKind::catalog);
}

void Catalog::remove(const Object& object)
{
    if (!transaction_) throw Error(Code::unsupported, {}, "Metadata view is read-only");
    if (!auxiliary(object.kind)) throw Error(Code::unsupported, {}, "Expected auxiliary catalog object");
    const auto current = decode_object(static_cast<disk::RowId>(object.id),
        transaction_->get(owner_, static_cast<disk::RowId>(object.id)));
    if (current.kind != object.kind) corrupt();
    transaction_->erase(owner_, static_cast<disk::RowId>(object.id), disk::ChangeKind::catalog);
}

void Catalog::restore_object(const Object& object)
{
    if (!transaction_) throw Error(Code::unsupported, {}, "Metadata view is read-only");
    if (object.id < (int64_t{1} << 32) ||
        (object.kind != ObjectKind::table && object.kind != ObjectKind::index && object.kind != ObjectKind::routine))
        throw Error(Code::unsupported, {}, "Historical catalog object cannot be restored");
    const auto snapshot = list();
    const auto* parent = by_id(snapshot, object.parent);
    if (!parent || parent->kind != ObjectKind::schema)
        throw Error(Code::constraint, {}, "Historical object's current schema is missing");
    if (by_id(snapshot, object.id) || child(snapshot, object.parent, object.name))
        throw Error(Code::constraint, {}, "Historical object ID or name is already in use");
    if (snapshot.size() >= max_objects + 2) throw Error(Code::limit, {}, "Catalog object limit reached");

    const auto row = encode_object(object);
    decode_object(static_cast<disk::RowId>(object.id), row);
    transaction_->restore_row(owner_, static_cast<disk::RowId>(object.id), row, disk::ChangeKind::catalog);
}

void Catalog::rename(const Object& object, const Name& new_name)
{
    if (!transaction_) throw Error(Code::unsupported, new_name.source, "Metadata view is read-only");
    check_name(new_name);
    if (new_name.parts.size() != 1 || !auxiliary(object.kind))
        throw Error(Code::name, new_name.source, "Expected an unqualified auxiliary object name");
    const auto objects = list();
    const auto* current = by_id(objects, object.id);
    if (!current || current->kind != object.kind) corrupt();
    const auto& name = new_name.parts.front();
    if (name == current->name) return;
    if (child(objects, current->parent, name)) throw Error(Code::name, new_name.source, "Catalog object already exists");
    auto renamed = *current;
    renamed.name = name;
    transaction_->update(owner_, static_cast<disk::RowId>(renamed.id), encode_object(renamed), disk::ChangeKind::catalog);
}

Name Catalog::qualified(const Object& object) const
{
    const auto snapshot = list();
    return qualified(object, snapshot);
}

Name Catalog::qualified(const Object& object, std::span<const Object> snapshot)
{
    Name name{{object.name}, {}};
    for (auto parent = object.parent; parent;)
    {
        if (name.parts.size() == 3) corrupt();
        const auto* next = by_id(snapshot, parent);
        if (!next) throw Error(Code::name, {}, "Catalog parent no longer exists");
        name.parts.push_back(next->name);
        parent = next->parent;
    }
    std::ranges::reverse(name.parts);
    return name;
}

Object Catalog::validate(const Statement& statement) const
{
    return plan(list(), statement).object;
}

Object Catalog::apply(const Statement& statement)
{
    if (!transaction_) throw Error(Code::unsupported, statement.source, "Metadata view is read-only");
    auto [edit, object] = plan(list(), statement);
    if (edit == Edit::insert)
        object.id = static_cast<ObjectId>(transaction_->insert(owner_, encode_object(object), disk::ChangeKind::catalog));
    else if (edit == Edit::erase)
        transaction_->erase(owner_, static_cast<disk::RowId>(object.id), disk::ChangeKind::catalog);
    else
        transaction_->update(owner_, static_cast<disk::RowId>(object.id), encode_object(object), disk::ChangeKind::catalog);
    return object;
}

Object Catalog::create_auxiliary(ObjectKind kind, const Name& name, std::string definition)
{
    if (!transaction_) throw Error(Code::unsupported, name.source, "Metadata view is read-only");
    if (!auxiliary(kind)) throw Error(Code::unsupported, name.source, "Expected auxiliary catalog object");
    if (definition.empty() || definition.size() > max_definition)
        throw Error(Code::limit, name.source, "Missing or oversized catalog definition");
    if (!valid_utf8(definition)) throw Error(Code::type, name.source, "Invalid UTF-8 catalog definition");
    auto object = validate_auxiliary(kind, name);
    object.definition = std::move(definition);
    object.id = static_cast<ObjectId>(transaction_->insert(owner_, encode_object(object), disk::ChangeKind::catalog));
    return object;
}
}
