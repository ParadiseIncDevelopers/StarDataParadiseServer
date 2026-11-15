#include "relational.hpp"
#include "collation.hpp"

#include "definition.hpp"
#include "executor.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace sdp::sql
{
namespace
{
using wire::Json;
using KeyKind = ConstraintDefinition::Kind;

struct ColumnRule
{
    size_t column;
    std::optional<Expr> expression;
    std::optional<ObjectId> sequence;
    std::optional<int64_t> identity_next;
};

struct TableKey
{
    KeyKind kind;
    std::vector<size_t> columns;
    ObjectId referenced_table{};
    std::vector<size_t> referenced_columns;
    std::optional<Expr> predicate;
    std::string name;
};

struct TableDefinition
{
    std::vector<ColumnRule> defaults;
    std::vector<TableKey> constraints;
};

[[noreturn]] void corrupt()
{
    throw disk::Error(disk::Fault::corrupt, "Invalid relational metadata or index reference");
}

const Object& table_by_id(std::span<const Object> objects, ObjectId id)
{
    const auto table = std::ranges::find(objects, id, &Object::id);
    if (table == objects.end() || table->kind != ObjectKind::table) corrupt();
    return *table;
}

std::string table_definition(const TableDefinition& definition)
{
    if (definition.defaults.empty() && definition.constraints.empty()) return {};
    Json defaults = Json::array();
    for (const auto& rule : definition.defaults)
    {
        Json entry{{"column", rule.column}};
        if (rule.expression) entry["default"] = write_expression(*rule.expression);
        if (rule.sequence) entry["sequence"] = *rule.sequence;
        if (rule.identity_next) entry["identity_next"] = *rule.identity_next;
        defaults.push_back(std::move(entry));
    }
    Json checks = Json::array();
    for (const auto& rule : definition.constraints)
    {
        Json entry{{"kind", static_cast<int>(rule.kind)}, {"columns", rule.columns}, {"name", rule.name}};
        if (rule.kind == KeyKind::foreign_key)
        {
            entry["table"] = rule.referenced_table;
            entry["references"] = rule.referenced_columns;
        }
        if (rule.predicate) entry["predicate"] = write_expression(*rule.predicate);
        checks.push_back(std::move(entry));
    }
    return Json{{"relational_table", 1}, {"defaults", std::move(defaults)}, {"constraints", std::move(checks)}}.dump();
}

bool valid_key(std::span<const size_t> columns, size_t width)
{
    if (columns.empty() || columns.size() > 16) return false;
    std::set<size_t> seen;
    return std::ranges::all_of(columns, [&](size_t index) {
        return index < width && seen.insert(index).second;
    });
}

TableDefinition read_table(const Object& table)
{
    if (table.definition.empty()) return {};
    TableDefinition result;
    try
    {
        const auto root = Json::parse(table.definition);
        const auto& defaults = root.at("defaults");
        const auto& constraints = root.at("constraints");
        if (root.at("relational_table") != 1 || !defaults.is_array() || !constraints.is_array() ||
            defaults.size() > table.columns.size() || constraints.size() > 64)
            corrupt();
        std::set<size_t> default_columns;
        for (const auto& entry : defaults)
        {
            ColumnRule rule{entry.at("column").get<size_t>(), {}, {}, {}};
            if (rule.column >= table.columns.size() || !default_columns.insert(rule.column).second ||
                size_t(entry.contains("default")) + size_t(entry.contains("sequence")) +
                    size_t(entry.contains("identity_next")) != 1)
                corrupt();
            if (entry.contains("default")) rule.expression = read_expression(entry.at("default"));
            if (entry.contains("sequence")) rule.sequence = entry.at("sequence").get<ObjectId>();
            if (entry.contains("identity_next")) rule.identity_next = entry.at("identity_next").get<int64_t>();
            if ((rule.sequence && *rule.sequence <= 0) || (rule.identity_next && *rule.identity_next < 0) ||
                ((rule.sequence || rule.identity_next) && table.columns[rule.column].type.kind != Kind::integer))
                corrupt();
            result.defaults.push_back(std::move(rule));
        }
        for (const auto& entry : constraints)
        {
            const auto kind = entry.at("kind").get<int>();
            if (kind < static_cast<int>(KeyKind::primary_key) || kind > static_cast<int>(KeyKind::check)) corrupt();
            TableKey rule{static_cast<KeyKind>(kind), entry.at("columns").get<std::vector<size_t>>(), 0, {}, {},
                entry.at("name").get<std::string>()};
            if (rule.kind == KeyKind::check)
            {
                if (!rule.columns.empty()) corrupt();
                rule.predicate = read_expression(entry.at("predicate"));
            }
            else if (!valid_key(rule.columns, table.columns.size())) corrupt();
            if (rule.kind == KeyKind::foreign_key)
            {
                rule.referenced_table = entry.at("table").get<ObjectId>();
                rule.referenced_columns = entry.at("references").get<std::vector<size_t>>();
                if (rule.referenced_table <= 0 || rule.referenced_columns.size() != rule.columns.size()) corrupt();
            }
            result.constraints.push_back(std::move(rule));
        }
    }
    catch (const Json::exception&) { corrupt(); }
    return result;
}

IndexDefinition read_index(const Object& object)
{
    try
    {
        const auto root = Json::parse(object.definition);
        const auto table = root.at("table").get<ObjectId>();
        const auto page = root.at("root").get<disk::RowId>();
        auto columns = root.at("columns").get<std::vector<size_t>>();
        if (root.at("relational_index") != 1 || table <= 0 || !page || !valid_key(columns, 60) ||
            root.contains("collation") != root.contains("sort_version"))
            corrupt();
        auto collation = root.value("collation", std::string("binary"));
        auto sort_version = root.value("sort_version", collation_version("binary"));
        if (collation != canonical_collation(collation) || sort_version.empty() || sort_version.size() > 96)
            corrupt();
        return {object, table, page, std::move(columns), root.at("unique").get<bool>(),
            std::move(collation), std::move(sort_version)};
    }
    catch (const Json::exception&) { corrupt(); }
    catch (const Error&) { corrupt(); }
}

bool index_current(const IndexDefinition& index)
{
    return index.sort_version == collation_version(index.collation);
}

void require_current(const IndexDefinition& index)
{
    if (!index_current(index)) throw Error(Code::constraint, {}, "Index collation data changed. REINDEX is required");
}

std::string index_definition(const IndexDefinition& index)
{
    return Json{{"relational_index", 1}, {"table", index.table}, {"root", index.root},
        {"columns", index.columns}, {"unique", index.unique},
        {"collation", index.collation}, {"sort_version", index.sort_version}}.dump();
}

std::vector<IndexDefinition> indexes(std::span<const Object> snapshot, ObjectId table)
{
    std::vector<IndexDefinition> out;
    for (const auto& object : snapshot)
    {
        if (object.kind != ObjectKind::index) continue;
        auto index = read_index(object);
        if (index.table == table) out.push_back(std::move(index));
    }
    return out;
}

std::vector<size_t> column_numbers(const Object& table, std::span<const Name> names, Source source)
{
    std::vector<size_t> columns;
    for (const auto& name : names)
    {
        if (name.parts.size() != 1) throw Error(Code::name, name.source, "Key column must be unqualified");
        const auto column = std::ranges::find(table.columns, name.parts.front(), &Column::name);
        if (column == table.columns.end()) throw Error(Code::name, name.source, "Unknown key column");
        columns.push_back(static_cast<size_t>(column - table.columns.begin()));
    }
    if (!valid_key(columns, table.columns.size()))
        throw Error(Code::constraint, source, "Keys require 1–16 distinct columns");
    return columns;
}

bool has_unique(const TableDefinition& definition, const std::vector<size_t>& columns)
{
    return std::ranges::any_of(definition.constraints, [&](const auto& key) {
        return (key.kind == KeyKind::primary_key || key.kind == KeyKind::unique) && key.columns == columns;
    });
}

TableDefinition prepare_table(Catalog& catalog, const Object& table, const Statement& statement)
{
    TableDefinition result;
    for (size_t index = 0; index < statement.columns.size(); ++index)
    {
        const auto& column = statement.columns[index];
        if (!column.identity && !column.default_value) continue;
        ColumnRule rule{index, {}, {}, {}};
        if (column.identity)
        {
            if (column.type.kind != Kind::integer)
                throw Error(Code::type, column.name.source, "IDENTITY requires BIGINT");
            rule.identity_next = 1;
        }
        else if (column.default_value->op == Expr::Op::next_value)
        {
            if (column.type.kind != Kind::integer)
                throw Error(Code::type, column.name.source, "A sequence DEFAULT requires BIGINT");
            rule.sequence = catalog.resolve(ObjectKind::sequence, column.default_value->name).id;
        }
        else
        {
            const auto bound = bind_expression(catalog, *column.default_value);
            const auto value = coerce(evaluate(bound), bound.type, table.columns[index].type, bound.source);
            if (!table.columns[index].nullable && std::holds_alternative<std::monostate>(value))
                throw Error(Code::constraint, bound.source, "DEFAULT NULL conflicts with NOT NULL");
            rule.expression = *column.default_value;
        }
        result.defaults.push_back(std::move(rule));
    }
    if (statement.constraints.size() > 64)
        throw Error(Code::limit, statement.source, "Too many table constraints");
    std::set<std::string> constraint_names;
    bool primary{};
    for (const auto& constraint : statement.constraints)
    {
        TableKey rule{constraint.kind, {}, 0, {}, constraint.predicate, constraint.name.value_or("")};
        if (constraint.name && !constraint_names.insert(*constraint.name).second)
            throw Error(Code::name, constraint.source, "Duplicate constraint name");
        if (constraint.kind == KeyKind::check)
        {
            if (!rule.predicate) throw Error(Code::syntax, constraint.source, "CHECK requires a predicate");
            const auto bound = bind_expression(catalog, table, *rule.predicate);
            if (!can_coerce(bound.type, {Kind::boolean}))
                throw Error(Code::type, bound.source, "CHECK requires a boolean predicate");
        }
        else
        {
            rule.columns = column_numbers(table, constraint.columns, constraint.source);
            if (constraint.kind == KeyKind::primary_key)
            {
                if (primary) throw Error(Code::constraint, constraint.source, "A table can have one PRIMARY KEY");
                primary = true;
            }
            if (constraint.kind == KeyKind::foreign_key)
            {
                // A referenced table and its declared candidate key must already exist.
                const auto parent = catalog.resolve(ObjectKind::table, constraint.referenced_table);
                rule.referenced_table = parent.id;
                rule.referenced_columns = column_numbers(parent, constraint.referenced_columns, constraint.source);
                if (rule.columns.size() != rule.referenced_columns.size() || !has_unique(read_table(parent), rule.referenced_columns))
                    throw Error(Code::constraint, constraint.source, "FOREIGN KEY requires a referenced PRIMARY or UNIQUE key");
                for (size_t i = 0; i < rule.columns.size(); ++i)
                    if (table.columns[rule.columns[i]].type != parent.columns[rule.referenced_columns[i]].type)
                        throw Error(Code::type, constraint.source, "Foreign key column types differ from the referenced key");
            }
        }
        result.constraints.push_back(std::move(rule));
    }
    if (table_definition(result).size() > 16 * 1024)
        throw Error(Code::limit, statement.source, "Table constraint definitions exceed the catalog limit");
    return result;
}

Name qualified_index(Catalog& catalog, const Object& table, const Name& requested)
{
    const auto canonical = catalog.qualified(table).parts;
    const auto& given = requested.parts;
    const bool own_schema = given.size() == 1 ||
        (given.size() == 2 && canonical[0] == default_database && given[0] == canonical[1]) ||
        (given.size() == 3 && given[0] == canonical[0] && given[1] == canonical[1]);
    if (!own_schema) throw Error(Code::name, requested.source, "Index namespace must match its table");
    return {{canonical[0], canonical[1], given.back()}, requested.source};
}

void set_collation(IndexDefinition& index, const Object& table, std::string_view name, Source source)
{
    index.collation = canonical_collation(name);
    if (index.collation != "binary" && !std::ranges::any_of(index.columns,
        [&](size_t column) { return table.columns[column].type.kind == Kind::text; }))
        throw Error(Code::type, source, "A locale collation requires a TEXT index column");
    index.sort_version = collation_version(index.collation);
}

IndexDefinition prepare_index(Catalog& catalog, const Statement& statement)
{
    const auto table = catalog.resolve(ObjectKind::table, statement.related);
    auto object = catalog.validate_auxiliary(ObjectKind::index, qualified_index(catalog, table, statement.target));
    IndexDefinition result{std::move(object), table.id, 0,
        column_numbers(table, statement.key_columns, statement.source), statement.unique_index, {}, {}};
    set_collation(result, table, statement.collation, statement.source);
    return result;
}

IndexDefinition prepare_reindex(const Statement& statement, const IndexDefinition& old, const Object& table)
{
    if (table.parent != old.object.parent || !valid_key(old.columns, table.columns.size())) corrupt();
    auto replacement = old;
    set_collation(replacement, table, statement.collation.empty() ? old.collation : statement.collation,
        statement.source);
    return replacement;
}

bool null_key(const IndexKey& key)
{
    return std::ranges::any_of(key, [](const auto& value) { return std::holds_alternative<std::monostate>(value); });
}

IndexKey key_for(std::span<const disk::Value> row, std::span<const size_t> columns)
{
    IndexKey key;
    key.reserve(columns.size());
    for (const auto column : columns)
    {
        if (column >= row.size()) corrupt();
        key.push_back(row[column]);
    }
    return key;
}

bool same_key(const IndexKey& left, const IndexKey& right, std::span<const Type> types,
    std::string_view collation = "binary")
{
    if (left.size() != right.size() || left.size() != types.size()) corrupt();
    for (size_t index = 0; index < left.size(); ++index)
    {
        const auto ln = std::holds_alternative<std::monostate>(left[index]);
        const auto rn = std::holds_alternative<std::monostate>(right[index]);
        if (ln != rn) return false;
        if (ln) continue;
        const auto type = types[index];
        const auto order = type.kind == Kind::text && collation != "binary"
            ? compare_collated(std::get<std::string>(left[index]), std::get<std::string>(right[index]), collation)
            : compare_non_null(left[index], type, right[index], type);
        if (order) return false;
    }
    return true;
}

std::vector<Type> types_for(const Object& table, std::span<const size_t> columns)
{
    if (!valid_key(columns, table.columns.size())) corrupt();
    std::vector<Type> types;
    types.reserve(columns.size());
    for (const auto column : columns) types.push_back(table.columns[column].type);
    return types;
}

bool search(disk::Transaction& tx, disk::Owner owner, const Object& table, const IndexKey& key,
    const std::vector<size_t>& columns, disk::RowId ignored, const WorkCheck& check)
{
    const auto types = types_for(table, columns);
    return !scan_table(tx, owner, table, [&](disk::RowId id, const RowValues& row) {
        return id == ignored || !same_key(key, key_for(row, columns), types);
    }, check);
}

void validate_foreign(disk::Transaction& tx, disk::Owner owner, std::span<const Object> snapshot,
    const TableKey& rule, const IndexKey& key, const WorkCheck& check)
{
    if (null_key(key)) return;
    const auto& parent = table_by_id(snapshot, rule.referenced_table);
    if (!search(tx, owner, parent, key, rule.referenced_columns, 0, check))
        throw Error(Code::constraint, {}, "FOREIGN KEY target row does not exist");
}

void reject_incoming(Catalog& catalog, const Object& table)
{
    if (has_view_dependency(catalog, table.id))
        throw Error(Code::constraint, {}, "A view depends on this table");
    for (const auto& object : catalog.list())
    {
        if (object.kind != ObjectKind::table) continue;
        for (const auto& rule : read_table(object).constraints)
            if (rule.kind == KeyKind::foreign_key && rule.referenced_table == table.id)
                throw Error(Code::constraint, {}, "A FOREIGN KEY depends on this table");
    }
}

void unique_index(BTree& tree, const IndexKey& key, disk::RowId ignored)
{
    if (null_key(key)) return;
    const auto equal = tree.equal(key, 2);
    if (std::ranges::any_of(equal, [&](const IndexEntry& entry) { return entry.row != ignored; }))
        throw Error(Code::constraint, {}, "Duplicate key in UNIQUE index");
}

disk::RowId build_index(disk::Transaction& transaction, disk::Owner owner, const Object& table,
    const IndexDefinition& index, const WorkCheck& check)
{
    // The scan visitor does not reenter the transaction. Build the tree after collecting row IDs.
    std::vector<disk::RowId> rows;
    scan_table(transaction, owner, table, [&](disk::RowId id, const RowValues&) {
        if (rows.size() == disk::change_count_limit)
            throw Error(Code::limit, {}, "Index build row budget exceeded");
        rows.push_back(id);
        return true;
    }, check);
    const auto root = BTree::create(transaction, owner);
    BTree tree(transaction, owner, root, index_types(table, index), index.collation);
    for (const auto id : rows)
    {
        if (check) check();
        auto key = key_for(read_table_row(transaction, owner, table, id), index.columns);
        if (index.unique) unique_index(tree, key, 0);
        tree.insert({std::move(key), id});
    }
    return root;
}

void retire_index(disk::Transaction& transaction, disk::Owner owner, const Object& table,
    const IndexDefinition& index)
{
    BTree old(transaction, owner, index.root, index_types(table, index), index.collation);
    if (index_current(index)) old.clear();
    else old.clear_stale();
}
}

std::optional<IndexCollation> catalog_index_collation(const Object& object)
{
    if (object.kind != ObjectKind::index) return {};
    auto index = read_index(object);
    const bool stale = !index_current(index);
    return IndexCollation{std::move(index.collation), std::move(index.sort_version), stale};
}

std::vector<Type> index_types(const Object& table, const IndexDefinition& index)
{
    if (table.id != index.table) corrupt();
    return types_for(table, index.columns);
}

bool relational_ddl(Catalog& catalog, const Statement& statement)
{
    switch (statement.action)
    {
    case Action::create_table:
        prepare_table(catalog, catalog.validate(statement), statement);
        return true;
    case Action::drop_table:
        reject_incoming(catalog, catalog.validate(statement));
        return true;
    case Action::truncate_table:
        catalog.resolve(ObjectKind::table, statement.target);
        return true;
    case Action::create_index:
        prepare_index(catalog, statement);
        return true;
    case Action::drop_index:
        read_index(catalog.resolve(ObjectKind::index, statement.target));
        return true;
    case Action::reindex:
    {
        const auto index = read_index(catalog.resolve(ObjectKind::index, statement.target));
        const auto snapshot = catalog.list();
        prepare_reindex(statement, index, table_by_id(snapshot, index.table));
        return true;
    }
    case Action::rename_index:
    {
        const auto object = catalog.resolve(ObjectKind::index, statement.target);
        if (statement.related.parts.size() != 1)
            throw Error(Code::name, statement.related.source, "New index name must be unqualified");
        auto destination = catalog.qualified(object);
        destination.parts.back() = statement.related.parts.front();
        catalog.validate_auxiliary(ObjectKind::index, destination);
        return true;
    }
    default: return false;
    }
}

std::vector<ObjectId> relational_dependencies(const Object& object)
{
    std::vector<ObjectId> dependencies;
    if (object.kind == ObjectKind::index) dependencies.push_back(read_index(object).table);
    if (object.kind == ObjectKind::table)
    {
        const auto definition = read_table(object);
        for (const auto& key : definition.constraints)
            if (key.kind == KeyKind::foreign_key) dependencies.push_back(key.referenced_table);
        for (const auto& rule : definition.defaults)
            if (rule.sequence) dependencies.push_back(*rule.sequence);
    }
    std::ranges::sort(dependencies);
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    return dependencies;
}

void install_table_rules(Catalog& catalog, const Object& table, const Statement& statement)
{
    const auto definition = table_definition(prepare_table(catalog, table, statement));
    if (!definition.empty()) catalog.update_definition(table, definition);
}

void restore_history_table(disk::Transaction& transaction, disk::Owner owner, const Object& historical,
    std::span<const Object> historical_indexes, const WorkCheck& check)
{
    if (historical.kind != ObjectKind::table)
        throw Error(Code::type, {}, "Restore requires historical table metadata");
    Catalog catalog(transaction, owner, check);
    const auto snapshot = catalog.list();
    const auto requirements = read_table(historical);
    const auto current = [&](ObjectId id, ObjectKind kind) -> const Object& {
        const auto found = std::ranges::find(snapshot, id, &Object::id);
        if (found == snapshot.end() || found->kind != kind)
            throw Error(Code::constraint, {}, "A dependency of the historical table is missing");
        return *found;
    };
    for (const auto& rule : requirements.defaults)
        if (rule.sequence) (void)current(*rule.sequence, ObjectKind::sequence);
    for (const auto& rule : requirements.constraints)
    {
        if (check) check();
        if (rule.kind == KeyKind::foreign_key)
        {
            const auto& parent = current(rule.referenced_table, ObjectKind::table);
            if (!valid_key(rule.referenced_columns, parent.columns.size()) ||
                !has_unique(read_table(parent), rule.referenced_columns) ||
                types_for(historical, rule.columns) != types_for(parent, rule.referenced_columns))
                throw Error(Code::constraint, {}, "A referenced key changed since the historical revision");
        }
        if (rule.kind == KeyKind::check)
        {
            const auto expression = bind_expression(catalog, historical, *rule.predicate);
            if (!can_coerce(expression.type, {Kind::boolean}))
                throw Error(Code::constraint, {}, "A historical CHECK no longer binds as BOOLEAN");
        }
    }

    std::vector<IndexDefinition> indexes;
    for (const auto& object : historical_indexes)
    {
        if (check) check();
        if (object.kind != ObjectKind::index || object.parent != historical.parent)
            throw Error(Code::constraint, {}, "Historical index is outside the selected table scope");
        auto index = read_index(object);
        index_types(historical, index);
        if (index.object.version == UINT32_MAX) throw Error(Code::limit, {}, "Index schema version exhausted");
        index.sort_version = collation_version(index.collation);
        ++index.object.version;
        indexes.push_back(std::move(index));
    }

    const auto savepoint = transaction.statement_savepoint(owner);
    try
    {
        auto table = historical;
        table.definition = table_definition(requirements);
        catalog.restore_object(table);
        for (auto& index : indexes)
        {
            if (check) check();
            index.root = BTree::create(transaction, owner);
            index.object.definition = index_definition(index);
            catalog.restore_object(index.object);
        }
        transaction.release_savepoint(owner, savepoint);
    }
    catch (...)
    {
        try
        {
            transaction.rollback_to(owner, savepoint);
            transaction.release_savepoint(owner, savepoint);
        }
        catch (...)
        {
            try { transaction.rollback(owner); }
            catch (const disk::Error&) {}
        }
        throw;
    }
}

bool execute_index_ddl(disk::Transaction& transaction, disk::Owner owner, Catalog& catalog,
    const Statement& statement, const WorkCheck& check)
{
    if (statement.action == Action::create_index)
    {
        auto index = prepare_index(catalog, statement);
        const auto snapshot = catalog.list();
        const auto& table = table_by_id(snapshot, index.table);
        const auto qualified = qualified_index(catalog, table, statement.target);
        index.root = build_index(transaction, owner, table, index, check);
        catalog.create_auxiliary(ObjectKind::index, qualified, index_definition(index));
        return true;
    }
    if (statement.action == Action::reindex)
    {
        const auto previous = read_index(catalog.resolve(ObjectKind::index, statement.target));
        const auto snapshot = catalog.list();
        const auto& table = table_by_id(snapshot, previous.table);
        auto replacement = prepare_reindex(statement, previous, table);
        replacement.root = build_index(transaction, owner, table, replacement, check);
        retire_index(transaction, owner, table, previous);
        catalog.update_definition(previous.object, index_definition(replacement), true);
        return true;
    }
    if (statement.action == Action::drop_index || statement.action == Action::rename_index)
    {
        const auto object = catalog.resolve(ObjectKind::index, statement.target);
        if (statement.action == Action::rename_index)
        {
            catalog.rename(object, statement.related);
            return true;
        }
        const auto index = read_index(object);
        const auto snapshot = catalog.list();
        retire_index(transaction, owner, table_by_id(snapshot, index.table), index);
        catalog.remove(object);
        return true;
    }
    return false;
}

TableMutation::TableMutation(disk::Transaction& transaction, disk::Owner owner, const Object& table, WorkCheck check)
    : transaction_(transaction), owner_(owner), table_(table), check_(std::move(check)),
      catalog_(transaction, owner, check_), snapshot_(catalog_.list()), indexes_(indexes(snapshot_, table.id))
{
}

void TableMutation::fill_defaults(RowValues& values, std::span<const size_t> assigned)
{
    auto definition = read_table(table_);
    bool identity_changed{};
    for (auto& rule : definition.defaults)
    {
        if (std::ranges::find(assigned, rule.column) != assigned.end()) continue;
        const auto& column = table_.columns[rule.column];
        if (rule.identity_next)
        {
            const auto current = *rule.identity_next;
            if (!current) throw Error(Code::limit, {}, "IDENTITY counter exhausted");
            values[rule.column] = current;
            rule.identity_next = current == (std::numeric_limits<int64_t>::max)() ? 0 : current + 1;
            identity_changed = true;
        }
        else if (rule.sequence)
            values[rule.column] = advance_sequence(catalog_, *rule.sequence);
        else
        {
            const auto expression = bind_expression(catalog_, *rule.expression);
            values[rule.column] = coerce(evaluate(expression), expression.type, column.type, expression.source);
        }
    }
    if (identity_changed)
    {
        table_.definition = table_definition(definition);
        catalog_.update_definition(table_, table_.definition);
    }
}

void TableMutation::verify_history_keys(disk::RowId id, const RowValues* before, const RowValues& after)
{
    // A live row retains its key for preflight, even if the same plan would remove it.
    for (const auto& rule : read_table(table_).constraints)
    {
        if (rule.kind != KeyKind::primary_key && rule.kind != KeyKind::unique) continue;
        if (check_) check_();
        const auto key = key_for(after, rule.columns);
        if (null_key(key)) continue;
        if (before && same_key(key_for(*before, rule.columns), key, types_for(table_, rule.columns))) continue;
        if (search(transaction_, owner_, table_, key, rule.columns, id, check_))
            throw Error(Code::constraint, {}, "A current row already owns this historical key");
    }
    for (const auto& index : indexes_)
    {
        if (!index.unique) continue;
        if (check_) check_();
        const auto key = key_for(after, index.columns);
        if (null_key(key)) continue;
        const auto types = index_types(table_, index);
        if (before && same_key(key_for(*before, index.columns), key, types, index.collation)) continue;
        require_current(index);
        BTree tree(transaction_, owner_, index.root, types, index.collation);
        unique_index(tree, key, id);
    }
}

void TableMutation::inspect(disk::RowId id, const RowValues* before, const RowValues* after)
{
    if (after)
    {
        for (size_t column = 0; column < table_.columns.size(); ++column)
            if (!table_.columns[column].nullable && std::holds_alternative<std::monostate>((*after)[column]))
                throw Error(Code::constraint, {}, "NULL is not allowed in a required column");
        for (const auto& rule : read_table(table_).constraints)
        {
            if (check_) check_();
            if (rule.kind == KeyKind::check)
            {
                const auto bound = bind_expression(catalog_, table_, *rule.predicate);
                if (truth(evaluate(bound, *after), bound.source) == Truth::false_)
                    throw Error(Code::constraint, bound.source, "CHECK constraint rejected the row");
                continue;
            }
            auto key = key_for(*after, rule.columns);
            if (rule.kind == KeyKind::foreign_key)
                validate_foreign(transaction_, owner_, snapshot_, rule, key, check_);
            else if (null_key(key))
            {
                if (rule.kind == KeyKind::primary_key) throw Error(Code::constraint, {}, "PRIMARY KEY cannot contain NULL");
            }
            else if (search(transaction_, owner_, table_, key, rule.columns, id, check_))
                throw Error(Code::constraint, {}, "Duplicate PRIMARY or UNIQUE key");
        }
    }
    if (!before) return;
    for (const auto& child : snapshot_)
    {
        if (child.kind != ObjectKind::table || child.definition.empty()) continue;
        for (const auto& rule : read_table(child).constraints)
        {
            if (rule.kind != KeyKind::foreign_key || rule.referenced_table != table_.id) continue;
            const auto old = key_for(*before, rule.referenced_columns);
            if (null_key(old)) continue;
            const auto types = types_for(table_, rule.referenced_columns);
            if (after && same_key(old, key_for(*after, rule.referenced_columns), types)) continue;
            if (search(transaction_, owner_, child, old, rule.columns, 0, check_))
                throw Error(Code::constraint, {}, "FOREIGN KEY restricts changing this parent row");
        }
    }
}

void TableMutation::index(disk::RowId id, const RowValues* before, const RowValues* after)
{
    for (const auto& index : indexes_)
    {
        if (check_) check_();
        require_current(index);
        const auto types = index_types(table_, index);
        BTree tree(transaction_, owner_, index.root, types, index.collation);
        if (before && after && same_key(key_for(*before, index.columns), key_for(*after, index.columns), types)) continue;
        if (before) tree.erase({key_for(*before, index.columns), id});
        if (after)
        {
            auto key = key_for(*after, index.columns);
            if (index.unique) unique_index(tree, key, id);
            tree.insert({std::move(key), id});
        }
    }
}

void TableMutation::drop_indexes()
{
    for (const auto& index : indexes_)
    {
        if (check_) check_();
        retire_index(transaction_, owner_, table_, index);
        catalog_.remove(index.object);
    }
    indexes_.clear();
}

std::optional<IndexAccess> choose_index(Catalog& catalog, const SelectPlan& plan, const ParameterValues& parameters)
{
    if (!plan.table || !plan.filter) return {};
    using Op = Expr::Op;
    auto op = plan.filter->op;
    if (op != Op::eq && op != Op::lt && op != Op::le && op != Op::gt && op != Op::ge) return {};
    const auto* key = &plan.filter->operands.at(0);
    const auto* value = &plan.filter->operands.at(1);
    if (key->op != Op::name)
    {
        std::swap(key, value);
        switch (op)
        {
        case Op::lt: op = Op::gt; break;
        case Op::le: op = Op::ge; break;
        case Op::gt: op = Op::lt; break;
        case Op::ge: op = Op::le; break;
        default: break;
        }
    }
    const auto has_column = [](const auto& self, const BoundExpression& expression) -> bool {
        return expression.op == Op::name || std::ranges::any_of(expression.operands,
            [&](const BoundExpression& operand) { return self(self, operand); });
    };
    if (key->op != Op::name || has_column(has_column, *value) || !can_coerce(value->type, key->type)) return {};
    const auto evaluated = evaluate(*value, {}, parameters);
    if (std::holds_alternative<std::monostate>(evaluated)) return {};
    IndexKey bound;
    try { bound.push_back(coerce(evaluated, value->type, key->type)); }
    catch (const Error& error) { if (error.code() == Code::range) return {}; else throw; }
    try { if (disk::encode({1, bound}).size() > 2048) return {}; }
    catch (const disk::Error& error) { if (error.fault() == disk::Fault::limit) return {}; else throw; }

    for (auto& index : indexes(catalog.list(), plan.table->id))
    {
        if (index.collation != "binary" || index.columns != std::vector<size_t>{key->column} || !index_current(index))
            continue;
        IndexAccess access{std::move(index), {}, {}};
        if (op == Op::eq || op == Op::gt || op == Op::ge) access.lower = bound;
        if (op == Op::eq || op == Op::lt || op == Op::le) access.upper = bound;
        return access;
    }
    return {};
}

void verify_relational(disk::Store& store)
{
    Catalog catalog(store);
    const auto snapshot = catalog.list();
    verify_table_rows(store, snapshot);
    for (const auto& object : snapshot)
    {
        if (object.kind == ObjectKind::index)
        {
            const auto index = read_index(object);
            const auto& table = table_by_id(snapshot, index.table);
            if (table.parent != object.parent) corrupt();
            require_current(index);
            const auto types = index_types(table, index);
            BTree tree(store, index.root, types, index.collation);
            tree.verify();
            size_t entries{};
            std::optional<IndexKey> previous;
            tree.visit({}, {}, {}, [&](const IndexEntry& entry) {
                const auto row = read_table_row(store, table, entry.row);
                if (!same_key(key_for(row, index.columns), entry.key, types)) corrupt();
                if (index.unique && !null_key(entry.key) && previous &&
                    same_key(*previous, entry.key, types, index.collation))
                    corrupt();
                previous = entry.key;
                ++entries;
                return true;
            });
            size_t rows{};
            scan_table(store, table, [&](disk::RowId, const RowValues&) { ++rows; return true; });
            if (entries != rows) corrupt();
        }
        if (object.kind != ObjectKind::table) continue;
        for (const auto& key : read_table(object).constraints)
        {
            if (key.kind != KeyKind::foreign_key) continue;
            const auto& parent = table_by_id(snapshot, key.referenced_table);
            const auto types = types_for(parent, key.referenced_columns);
            if (types_for(object, key.columns) != types || !has_unique(read_table(parent), key.referenced_columns))
                corrupt();
            scan_table(store, object, [&](disk::RowId, const RowValues& child_row) {
                const auto child = key_for(child_row, key.columns);
                if (null_key(child)) return true;
                const auto missing = scan_table(store, parent, [&](disk::RowId, const RowValues& parent_row) {
                    return !same_key(child, key_for(parent_row, key.referenced_columns), types);
                });
                if (missing) corrupt();
                return true;
            });
        }
    }
}
}
