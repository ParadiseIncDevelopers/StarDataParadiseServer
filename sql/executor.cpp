#include "executor.hpp"
#include "routines.hpp"
#include "definition.hpp"
#include "decision.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace sdp::sql
{
namespace
{
// SQL rows have an object/version header; the heap slot is the stable row identifier.
constexpr uint32_t sql_row_schema = 0x53445231;
constexpr size_t row_fields = 2;
using Values = RowValues;

void poll(const WorkCheck& check)
{
    if (check) check();
}

bool is_null(const disk::Value& value) { return std::holds_alternative<std::monostate>(value); }

bool matches(const std::optional<BoundExpression>& filter, std::span<const disk::Value> row,
    const ParameterValues& parameters)
{
    return !filter || truth(evaluate(*filter, row, parameters), filter->source) == Truth::true_;
}

void validate_parameters(const BoundExpression& expr, const ParameterValues& parameters,
    std::set<std::string_view>& seen)
{
    if (expr.op == Expr::Op::parameter && seen.insert(expr.parameter).second)
        evaluate(expr, {}, parameters);
    for (const auto& operand : expr.operands) validate_parameters(operand, parameters, seen);
}

void validate_parameters(const MutationPlan& plan, const ParameterValues& parameters)
{
    std::set<std::string_view> seen;
    if (plan.filter) validate_parameters(*plan.filter, parameters, seen);
    for (const auto& row : plan.rows)
        for (const auto& expr : row) validate_parameters(expr, parameters, seen);
    for (const auto& assignment : plan.assignments)
        validate_parameters(assignment.expression, parameters, seen);
}

void verify_schema(disk::Transaction& transaction, disk::Owner owner, const Object& table, const WorkCheck& check)
{
    const auto objects = Catalog(transaction, owner, check).list();
    poll(check);
    const auto current = std::ranges::find(objects, table.id, &Object::id);
    if (current == objects.end() || current->version != table.version)
        throw Error(Code::name, {}, "The bound table schema changed");
}

disk::Row encode_record(const Object& table, const Values& values)
{
    disk::Row row{sql_row_schema, {table.id, static_cast<int64_t>(table.version)}};
    row.values.insert(row.values.end(), values.begin(), values.end());
    return row;
}

[[noreturn]] void invalid_record()
{
    throw disk::Error(disk::Fault::corrupt, "Invalid SQL heap row");
}

ObjectId record_table(const disk::Row& stored)
{
    if (stored.schema != sql_row_schema || stored.values.size() <= row_fields) invalid_record();
    const auto* table = std::get_if<int64_t>(&stored.values[0]);
    if (!table || *table < (int64_t{1} << 32)) invalid_record();
    return *table;
}

Values decode_record(const Object& table, const disk::Row& stored)
{
    if (record_table(stored) != table.id) invalid_record();
    const auto* version = std::get_if<int64_t>(&stored.values[1]);
    const auto fields = stored.values.size() - row_fields;
    if (!version || *version <= 0 || *version > table.version ||
        !compatible_row_layout(table, static_cast<uint32_t>(*version), fields))
        invalid_record();

    Values result;
    result.reserve(table.columns.size());
    for (size_t index = 0; index < table.columns.size(); ++index)
    {
        const auto& column = table.columns[index];
        auto value = index < fields ? stored.values[index + row_fields] : disk::Value{};
        if (!column.nullable && is_null(value)) invalid_record();
        try
        {
            if (coerce(value, column.type, column.type) != value) invalid_record();
        }
        catch (const Error&)
        {
            invalid_record();
        }
        result.push_back(std::move(value));
    }
    return result;
}

disk::Value assign_value(const Column& column, const BoundExpression& expression, std::span<const disk::Value> row,
    const ParameterValues& parameters)
{
    auto result = coerce(evaluate(expression, row, parameters), expression.type, column.type, expression.source);
    if (!column.nullable && is_null(result))
        throw Error(Code::constraint, expression.source, "NULL is not allowed in the target column");
    return result;
}

void apply_row(disk::Transaction& transaction, disk::Owner owner, const Object& table, TableMutation& mutation,
    disk::RowId row, const Values* before, Values* after, const WorkCheck& check = {},
    const InvokerCheck& invoker = {}, bool triggers = false, const Values* expected_history_image = nullptr)
{
    const std::string_view event = !before ? "insert" : !after ? "delete" : "update";
    if (triggers) fire_triggers(transaction, owner, table, "before", event, before, after, check, invoker);
    apply_decision(transaction, owner, mutation.catalog_snapshot(), table, before, after, check);
    if (expected_history_image && *after != *expected_history_image)
        throw DecisionError(wire::Code::invalid_state, "history_image_changed");
    mutation.inspect(row, before, after);
    if (!after) transaction.erase(owner, row);
    else if (before) transaction.update(owner, row, encode_record(table, *after));
    else if (row) transaction.restore_row(owner, row, encode_record(table, *after));
    else row = transaction.insert(owner, encode_record(table, *after));
    mutation.index(row, before, after);
    if (triggers) fire_triggers(transaction, owner, table, "after", event, before, after, check, invoker);
}

std::vector<disk::RowId> match_rows(disk::Transaction& transaction, disk::Owner owner, const Object& table,
    const std::optional<BoundExpression>& filter, const ParameterValues& parameters, const WorkCheck& check)
{
    std::vector<disk::RowId> matches_ids;
    scan_table(transaction, owner, table, [&](disk::RowId id, const Values& values) {
        if (!matches(filter, values, parameters)) return true;
        if (matches_ids.size() == disk::change_count_limit)
            throw Error(Code::limit, {}, "Statement affects too many rows");
        matches_ids.push_back(id);
        return true;
    }, check);
    return matches_ids;
}

size_t execute_mutation(disk::Transaction& transaction, disk::Owner owner, const MutationPlan& plan,
    const ParameterValues& parameters, const WorkCheck& check, const InvokerCheck& invoker)
{
    verify_schema(transaction, owner, plan.table, check);
    validate_parameters(plan, parameters);
    TableMutation mutation(transaction, owner, plan.table, check);
    require_decision_module(mutation.catalog_snapshot(), plan.table.id, transaction.decision_enabled());
    if (plan.action == Action::insert)
    {
        for (const auto& values : plan.rows)
        {
            poll(check);
            Values row(plan.table.columns.size());
            for (size_t index = 0; index < values.size(); ++index)
            {
                const auto target = plan.insert_columns[index];
                row[target] = assign_value(plan.table.columns[target], values[index], {}, parameters);
            }
            mutation.fill_defaults(row, plan.insert_columns);
            apply_row(transaction, owner, plan.table, mutation, 0, nullptr, &row, check, invoker, true);
        }
        return plan.rows.size();
    }

    const auto ids = match_rows(transaction, owner, plan.table, plan.filter, parameters, check);
    for (const auto id : ids)
    {
        poll(check);
        const auto previous = read_table_row(transaction, owner, plan.table, id);
        if (plan.action == Action::delete_)
        {
            apply_row(transaction, owner, plan.table, mutation, id, &previous, nullptr, check, invoker, true);
            continue;
        }
        auto next = previous;
        for (const auto& assignment : plan.assignments)
        {
            const auto target = assignment.column;
            next[target] = assign_value(plan.table.columns[target], assignment.expression, previous, parameters);
        }
        apply_row(transaction, owner, plan.table, mutation, id, &previous, &next, check, invoker, true);
    }
    return ids.size();
}

void execute_ddl(disk::Transaction& transaction, disk::Owner owner, const Statement& statement, const WorkCheck& check)
{
    Catalog catalog(transaction, owner, check);
    if (statement.decision && !transaction.decision_enabled())
        throw DecisionError(wire::Code::module_unavailable, "module_unavailable");
    if (decision_ddl(catalog, statement, true)) return;
    if (statement.action == Action::analyze)
    {
        analyze_table(transaction, owner, catalog, statement.target, check);
        return;
    }
    if (routine_ddl(catalog, statement, true) || auxiliary_ddl(catalog, statement, true) ||
        execute_index_ddl(transaction, owner, catalog, statement, check) ||
        execute_table_alter(transaction, owner, catalog, statement, check))
        return;
    if (statement.action == Action::truncate_table)
    {
        const auto table = catalog.resolve(ObjectKind::table, statement.target);
        TableMutation mutation(transaction, owner, table, check);
        require_decision_module(mutation.catalog_snapshot(), table.id, transaction.decision_enabled());
        for (const auto id : match_rows(transaction, owner, table, {}, {}, check))
        {
            poll(check);
            const auto before = read_table_row(transaction, owner, table, id);
            apply_row(transaction, owner, table, mutation, id, &before, nullptr, check);
        }
        return;
    }
    const auto planned = catalog.validate(statement);
    if (statement.action == Action::drop_table)
    {
        check_decision_drop(catalog.list(), planned);
        relational_ddl(catalog, statement);
        if (has_trigger_dependency(catalog, planned.id))
            throw Error(Code::constraint, statement.source, "Table has a dependent trigger");
        TableMutation mutation(transaction, owner, planned, check);
        const auto ids = match_rows(transaction, owner, planned, {}, {}, check);
        for (const auto id : ids)
        {
            poll(check);
            const auto before = read_table_row(transaction, owner, planned, id);
            apply_row(transaction, owner, planned, mutation, id, &before, nullptr, check);
        }
        mutation.drop_indexes();
    }
    if (statement.action == Action::add_column) check_decision_drop(catalog.list(), planned);
    if (statement.action == Action::add_column && !planned.columns.back().nullable)
    {
        const auto full_scan = transaction.scan(owner, 0, [&](disk::RowId, const disk::Row& row) {
            return record_table(row) != planned.id;
        }, sql_row_schema, check);
        if (!full_scan) throw Error(Code::constraint, statement.source, "ADD NOT NULL requires an empty table");
    }
    poll(check);
    const auto object = catalog.apply(statement);
    if (statement.action == Action::create_table) install_table_rules(catalog, object, statement);
}

template<class Operation>
size_t statement_write(disk::Transaction& transaction, disk::Owner owner, const WorkCheck& check, Operation operation)
{
    const auto savepoint = transaction.statement_savepoint(owner);
    try
    {
        const auto result = operation();
        poll(check);
        transaction.release_savepoint(owner, savepoint);
        return result;
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
            // An incomplete rollback cannot leave staged SQL changes committable.
            try { transaction.rollback(owner); }
            catch (const disk::Error&) {}
        }
        throw;
    }
}

size_t value_bytes(const disk::Value& value)
{
    constexpr auto base = sizeof(disk::Value);
    if (const auto* text = std::get_if<std::string>(&value)) return base + text->size();
    if (const auto* blob = std::get_if<disk::Bytes>(&value)) return base + blob->size();
    if (const auto* number = std::get_if<disk::Decimal>(&value)) return base + number->unscaled.size();
    return base;
}

struct Projected
{
    Values values;
    size_t bytes;
};

Projected project(const SelectPlan& plan, std::span<const disk::Value> input, const ParameterValues& parameters,
    size_t byte_limit, const WorkCheck& check)
{
    Projected row{{}, sizeof(Values)};
    if (row.bytes > byte_limit)
        throw Error(Code::limit, {}, "Projected row exceeds the fetch byte budget");
    for (const auto& column : plan.projection)
    {
        poll(check);
        auto value = evaluate(column.expression, input, parameters);
        const auto bytes = value_bytes(value);
        if (bytes > byte_limit - row.bytes)
            throw Error(Code::limit, column.expression.source, "Projected row exceeds the fetch byte budget");
        row.values.push_back(std::move(value));
        row.bytes += bytes;
    }
    return row;
}
}

std::optional<HistoryRecord> history_record(const disk::Row& row)
{
    if (row.schema != sql_row_schema) return {};
    const auto table = record_table(row);
    const auto* version = std::get_if<int64_t>(&row.values[1]);
    if (!version || *version <= 0 || *version > UINT32_MAX) invalid_record();
    return HistoryRecord{table, static_cast<uint32_t>(*version), std::span(row.values).subspan(row_fields)};
}

void visit_history_records(disk::Transaction& transaction, disk::Owner owner, ObjectId table,
    const disk::RowVisitor& visitor, const WorkCheck& check)
{
    transaction.scan(owner, 0, [&](disk::RowId id, const disk::Row& row) {
        if (record_table(row) == table) visitor(id, row);
        return true;
    }, sql_row_schema, check);
}

RowValues read_history_values(const Object& table, const disk::Row& row)
{
    const auto record = history_record(row);
    if (!record || record->table != table.id) invalid_record();
    if (!compatible_row_layout(table, record->version, record->values.size()))
        throw Error(Code::unsupported, {}, "Historical row schema is incompatible with the current table");
    RowValues values(record->values.begin(), record->values.end());
    values.resize(table.columns.size());
    for (size_t index = 0; index < values.size(); ++index)
    {
        const auto type = table.columns[index].type;
        if (coerce(values[index], type, type) != values[index])
            throw Error(Code::unsupported, {}, "Historical value requires a schema conversion");
    }
    return values;
}

void apply_history_rows(disk::Transaction& transaction, disk::Owner owner, const Object& table,
    std::span<const HistoryMutation> mutations, const WorkCheck& check, const InvokerCheck& invoker)
{
    statement_write(transaction, owner, check, [&] {
        verify_schema(transaction, owner, table, check);
        TableMutation mutation(transaction, owner, table, check);
        require_decision_module(mutation.catalog_snapshot(), table.id, transaction.decision_enabled());
        std::set<disk::RowId> used;
        for (const auto& edit : mutations)
        {
            poll(check);
            if (!edit.id || (!edit.before && !edit.after) || !used.insert(edit.id).second)
                throw Error(Code::constraint, {}, "Invalid or repeated History row mutation");
            if (edit.before && edit.before->size() != table.columns.size())
                throw Error(Code::constraint, {}, "Preview values do not match the current schema");
            if (edit.after)
            {
                if (edit.after->size() != table.columns.size())
                    throw Error(Code::constraint, {}, "Restore values do not match the current schema");
                for (size_t index = 0; index < edit.after->size(); ++index)
                {
                    const auto type = table.columns[index].type;
                    if (coerce((*edit.after)[index], type, type) != (*edit.after)[index])
                        throw Error(Code::constraint, {}, "Restore value needs an unsupported type conversion");
                }
                mutation.verify_history_keys(edit.id, edit.before ? &*edit.before : nullptr, *edit.after);
            }
        }
        for (const auto& edit : mutations)
        {
            poll(check);
            std::optional<RowValues> current;
            try { current = read_table_row(transaction, owner, table, edit.id); }
            catch (const disk::Error& error)
            {
                if (error.fault() != disk::Fault::missing) throw;
            }
            if (current != edit.before)
                throw Error(Code::constraint, {}, "Current row differs from the History preview");
            if (edit.before != edit.after)
            {
                auto after = edit.after;
                apply_row(transaction, owner, table, mutation, edit.id, edit.before ? &*edit.before : nullptr,
                    after ? &*after : nullptr, check, invoker, true, edit.after ? &*edit.after : nullptr);
            }
        }
        return mutations.size();
    });
}

disk::Row encode_table_row(const Object& table, const RowValues& values)
{
    return encode_record(table, values);
}

RowValues read_table_row(disk::Transaction& transaction, disk::Owner owner, const Object& table, disk::RowId id)
{
    return decode_record(table, transaction.get(owner, id));
}

RowValues read_table_row(disk::Store& store, const Object& table, disk::RowId id)
{
    return decode_record(table, disk::Editor(store).get(id));
}

bool scan_table(disk::Transaction& transaction, disk::Owner owner, const Object& table,
    const TableRowVisitor& visitor, const WorkCheck& check)
{
    return transaction.scan(owner, 0, [&](disk::RowId id, const disk::Row& row) {
        return record_table(row) != table.id || visitor(id, decode_record(table, row));
    }, sql_row_schema, check);
}

bool scan_table(disk::Store& store, const Object& table, const TableRowVisitor& visitor)
{
    return disk::Editor(store).scan(0, [&](disk::RowId id, const disk::Row& row) {
        return record_table(row) != table.id || visitor(id, decode_record(table, row));
    }, sql_row_schema);
}

void verify_table_rows(disk::Store& store, std::span<const Object> catalog)
{
    disk::Editor(store).scan(0, [&](disk::RowId, const disk::Row& row) {
        const auto table = std::ranges::find(catalog, record_table(row), &Object::id);
        if (table == catalog.end() || table->kind != ObjectKind::table) invalid_record();
        decode_record(*table, row);
        return true;
    }, sql_row_schema);
}

size_t execute_write(disk::Transaction& transaction, disk::Owner owner, const Plan& plan,
    const ParameterValues& parameters, const WorkCheck& check, const InvokerCheck& invoker)
{
    poll(check);
    if (std::holds_alternative<SelectPlan>(plan) || std::holds_alternative<AdvancedSelection>(plan) ||
        std::holds_alternative<std::shared_ptr<const RoutineSelection>>(plan))
        throw Error(Code::type, {}, "SELECT requires a result cursor");

    return statement_write(transaction, owner, check, [&] {
        if (const auto* statement = std::get_if<Statement>(&plan))
        {
            execute_ddl(transaction, owner, *statement, check);
            return size_t{};
        }
        return execute_mutation(transaction, owner, std::get<MutationPlan>(plan), parameters, check, invoker);
    });
}

Cursor::Cursor(SelectPlan plan, ParameterValues parameters, size_t row_limit)
    : plan_(std::move(plan)), parameters_(std::move(parameters)), limit_(row_limit)
{
    if (!limit_ || limit_ > 1'000'000) throw Error(Code::limit, {}, "Invalid result row limit");
    std::set<std::string_view> seen;
    if (plan_.filter) validate_parameters(*plan_.filter, parameters_, seen);
    for (const auto& column : plan_.projection) validate_parameters(column.expression, parameters_, seen);
}

ResultBatch Cursor::fetch(disk::Transaction& transaction, disk::Owner owner, size_t row_count, size_t byte_limit,
    const WorkCheck& check)
{
    if (!row_count || row_count > 1024 || !byte_limit || byte_limit > 1024 * 1024)
        throw Error(Code::limit, {}, "Invalid fetch batch budget");
    if (transaction_ && (transaction_ != &transaction || owner_ != owner))
        throw Error(Code::name, {}, "Result cursor belongs to another transaction");
    transaction.ensure_active(owner);
    if (done_) return {{}, true};
    poll(check);
    if (!transaction_ && plan_.table)
    {
        verify_schema(transaction, owner, *plan_.table, check);
        Catalog catalog(transaction, owner, check);
        index_ = choose_index(catalog, plan_, parameters_);
    }

    ResultBatch batch;
    auto last = after_;
    auto last_index = index_after_;
    size_t used{};
    const auto remaining = limit_ - emitted_;
    const auto cap = (std::min)(row_count, remaining);
    if (plan_.table)
    {
        const auto& table = *plan_.table;
        const auto consume = [&](const Values& input) {
            if (!passes_filter(plan_, input, parameters_)) return true;
            if (batch.rows.size() == cap) return false;
            auto row = project(plan_, input, parameters_, byte_limit, check);
            if (row.bytes > byte_limit - used) return false;
            used += row.bytes;
            batch.rows.push_back(std::move(row.values));
            return true;
        };
        if (index_)
        {
            BTree tree(transaction, owner, index_->index.root, index_types(table, index_->index));
            batch.done = tree.visit(index_->lower, index_->upper, index_after_, [&](const IndexEntry& item) {
                poll(check);
                if (!consume(read_table_row(transaction, owner, table, item.row))) return false;
                last_index = item;
                return batch.rows.size() < remaining;
            });
        }
        else
        {
            batch.done = transaction.scan(owner, after_, [&](disk::RowId id, const disk::Row& stored) {
                if (record_table(stored) == table.id && !consume(decode_record(table, stored))) return false;
                last = id;
                return batch.rows.size() < remaining;
            }, sql_row_schema, check);
        }
    }
    else
    {
        if (passes_filter(plan_, {}, parameters_))
            batch.rows.push_back(project(plan_, {}, parameters_, byte_limit, check).values);
        batch.done = true;
    }
    poll(check);
    after_ = last;
    index_after_ = std::move(last_index);
    emitted_ += batch.rows.size();
    done_ = batch.done = batch.done || emitted_ == limit_;
    transaction_ = &transaction;
    owner_ = owner;
    return batch;
}
}
