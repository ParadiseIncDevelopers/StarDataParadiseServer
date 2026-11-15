#include "routines.hpp"
#include "advanced.hpp"
#include "pl_ir.hpp"
#include "pl_runtime.hpp"

#include <algorithm>
#include <map>

namespace sdp::sql
{
using wire::Json;
using Op = Expr::Op;

struct RoutineSelection
{
    pl::Program program;
    std::vector<BoundExpression> arguments;
    std::vector<Projection> columns;
};

namespace
{
using Revisions = std::map<ObjectId, uint32_t>;

[[noreturn]] void damaged()
{
    throw disk::Error(disk::Fault::corrupt, "Invalid stored PL/SQL routine");
}

Json record(const Object& object)
{
    auto value = Json::parse(object.definition, nullptr, false);
    if (object.kind != ObjectKind::routine || !value.is_object() || value.value("format", 0) != 1 ||
        !value.contains("sql") || !value["sql"].is_string() || !value.contains("ir") || !value["ir"].is_object() ||
        !value.contains("dependencies") || !value["dependencies"].is_array() || value["dependencies"].size() > 128)
        damaged();
    return value;
}

Revisions revisions(const Json& value)
{
    Revisions out;
    for (const auto& item : value.at("dependencies"))
    {
        if (!item.is_array() || item.size() != 2 || !item[0].is_number_integer() ||
            !item[1].is_number_unsigned() || item[1].get<uint64_t>() > UINT32_MAX)
            damaged();
        const auto id = item[0].get<ObjectId>();
        const auto version = item[1].get<uint32_t>();
        if (id <= 0 || !version || !out.emplace(id, version).second) damaged();
    }
    return out;
}

std::shared_ptr<pl::Routine> source_routine(std::string_view source)
{
    if (source.size() > 8192) throw Error(Code::limit, {}, "Routine source exceeds 8 KiB");
    auto parsed = parse(source);
    if ((parsed.action != Action::create_routine && parsed.action != Action::create_trigger) || !parsed.routine) damaged();
    return std::move(parsed.routine);
}

ParameterTypes host_parameters(std::span<const pl::Binding> bindings)
{
    ParameterTypes types;
    for (const auto& binding : bindings) types.emplace(binding.name, binding.type);
    return types;
}

Type signature(const pl::Routine& routine, std::span<const Type> arguments, Source source)
{
    if (!routine.function) throw Error(Code::type, source, "Procedure is not a scalar function");
    size_t index{};
    for (const auto& argument : routine.variables)
    {
        if (argument.mode == pl::Mode::local) continue;
        if (argument.mode != pl::Mode::in || index == arguments.size() || !can_coerce(arguments[index++], argument.type))
            throw Error(Code::type, source, "Routine arguments do not match its signature");
    }
    if (index != arguments.size()) throw Error(Code::type, source, "Routine argument count does not match");
    return routine.returns;
}

const std::vector<Projection>& query_columns(const Plan& plan)
{
    if (const auto* p = std::get_if<SelectPlan>(&plan)) return p->projection;
    if (const auto* p = std::get_if<AdvancedSelection>(&plan)) return advanced_columns(*p->plan);
    if (const auto* p = std::get_if<std::shared_ptr<const RoutineSelection>>(&plan)) return (*p)->columns;
    throw Error(Code::unsupported, {}, "Routine SQL must be read-only");
}

pl::Program compile_program(Catalog& catalog, const pl::Routine& routine, uint32_t revision, Revisions& used)
{
    static thread_local size_t compiling{};
    if (compiling >= 12) throw Error(Code::limit, {}, "Routine compile depth exceeded");
    ++compiling;
    struct Leave { size_t& depth; ~Leave() { --depth; } } leave{compiling};

    const pl::HostType infer = [&](const Expr& expr, std::span<const Type> args, std::span<const pl::Binding> binds) -> Type {
        if (expr.op == Op::routine_call)
        {
            if (expr.name.parts == routine.name.parts) return signature(routine, args, expr.source);
            const auto target = catalog.resolve(ObjectKind::routine, expr.name);
            used[target.id] = target.version;
            return signature(*source_routine(record(target)["sql"].get<std::string>()), args, expr.source);
        }
        auto nested = expr.query ? *expr.query : parse(expr.text);
        if (nested.action != Action::select) throw Error(Code::unsupported, expr.source, "Routine SQL must be SELECT");
        const auto objects = catalog.list();
        for (const auto& access : access_requirements(catalog, nested))
        {
            const auto found = std::ranges::find(objects, access.target, &Object::id);
            if (found != objects.end()) used[found->id] = found->version;
        }

        const auto plan = sdp::sql::bind(catalog, nested, host_parameters(binds));
        const auto& columns = query_columns(plan);
        if (expr.op == Op::exists) return {Kind::boolean};
        if (columns.size() != 1) throw Error(Code::type, expr.source, "Scalar query requires one column");
        return columns.front().expression.type;
    };
    auto program = pl::compile(routine, revision, infer);
    const auto host_steps = [&](const auto& self, const std::vector<pl::Instruction>& steps) -> void {
        const auto objects = catalog.list();
        for (const auto& step : steps)
        {
            if (!step.host_sql.empty())
            {
                const auto hosted = parse(step.host_sql);
                for (const auto& access : access_requirements(catalog, hosted))
                {
                    const auto found = std::ranges::find(objects, access.target, &Object::id);
                    if (found != objects.end()) used[found->id] = found->version;
                }
                (void)sdp::sql::bind(catalog, hosted, host_parameters(step.bindings));
            }
            if (!step.body.empty()) self(self, step.body);
            if (!step.alternate.empty()) self(self, step.alternate);
            if (!step.handlers.empty()) self(self, step.handlers);
        }
    };
    host_steps(host_steps, program.body);
    host_steps(host_steps, program.handlers);
    if (used.size() > 128) throw Error(Code::limit, {}, "Routine dependency limit reached");
    return program;
}

pl::Routine trigger_body(const pl::Routine& source, const Object& table)
{
    auto routine = source;
    std::vector<pl::Variable> images;
    for (const auto label : {"old", "new"})
        for (const auto& column : table.columns)
            images.push_back({Name{{label, column.name}, source.name.source}, column.type, pl::Mode::in, {}});
    images.insert(images.end(), routine.variables.begin(), routine.variables.end());
    routine.variables = std::move(images);
    return routine;
}

pl::Program load_program(Catalog& catalog, const Object& object, const Object* trigger_table = nullptr)
{
    const auto stored = record(object);
    const auto expected = revisions(stored);
    const auto objects = catalog.list();
    for (const auto& [id, version] : expected)
    {
        const auto found = std::ranges::find(objects, id, &Object::id);
        if (found == objects.end() || found->version != version)
            throw Error(Code::name, {}, "Routine dependency changed, recompile the routine");
    }
    Revisions observed;
    auto source = source_routine(stored["sql"].get<std::string>());
    if (stored.contains("trigger") != (trigger_table != nullptr))
        throw Error(Code::unsupported, {}, "Trigger entry point is implicit");
    if (trigger_table)
    {
        observed[trigger_table->id] = trigger_table->version;
        *source = trigger_body(*source, *trigger_table);
    }
    auto program = compile_program(catalog, *source, object.version, observed);
    if (expected != observed || Json::parse(pl::serialize(program)) != stored["ir"]) damaged();
    return program;
}

struct CallOutcome
{
    pl::Result values;
    std::vector<NamedSet> sets;
    size_t affected{};
};

class Invocation
{
public:
    Invocation(disk::Transaction& tx, disk::Owner owner, const InvokerCheck& access, const WorkCheck& check,
        const pl::Instrumentation* instrumentation = nullptr)
        : tx_(tx), owner_(owner), access_(access), check_(check), catalog_(tx, owner, check),
          instrumentation_(instrumentation) {}

    CallOutcome run(const pl::Program& program, std::span<const disk::Value> values)
    {
        if (check_) check_();
        State state;
        CallOutcome out;
        pl::Limits limits;
        if (instrumentation_ && instrumentation_->trace) limits.time = std::chrono::seconds(10);
        out.values = pl::invoke(program, values, [&](const auto& expr, auto frame, auto args) {
            return host(expr, frame, args);
        }, check_, limits, [&](const auto& step, auto frame) {
            return host_action(program, state, step, frame);
        }, instrumentation_ ? *instrumentation_ : pl::Instrumentation{});
        for (const auto& name : program.out_cursors) emit(state, name, name, {});
        out.sets = std::move(state.emitted);
        out.affected = state.affected;
        return out;
    }

private:
    struct CursorState
    {
        NamedSet data;
        size_t offset{};
        size_t bytes{};
    };
    struct State
    {
        std::map<std::string, CursorState, std::less<>> cursors;
        std::vector<NamedSet> emitted;
        size_t memory{};
        size_t affected{};
    };

    ParameterValues values_for(std::span<const pl::Binding> bindings, std::span<const disk::Value> frame)
    {
        ParameterValues values;
        for (const auto& binding : bindings)
        {
            if (binding.slot >= frame.size()) damaged();
            values.emplace(binding.name, frame[binding.slot]);
        }
        return values;
    }

    void emit(State& state, std::string_view cursor, std::string_view label, Source source)
    {
        auto found = state.cursors.find(cursor);
        if (found == state.cursors.end()) throw pl::RuntimeError("invalid_cursor", source, "Cursor is not open");
        if (state.emitted.size() == 16) throw Error(Code::limit, source, "Routine result set limit reached");
        if (std::ranges::find(state.emitted, label, &NamedSet::name) != state.emitted.end() || label == "out")
            throw Error(Code::name, source, "Result set name is repeated or reserved");
        auto& cursor_data = found->second;
        for (size_t index = 0; index < cursor_data.offset; ++index)
            state.memory -= disk::encode({1, cursor_data.data.rows[index]}).size();
        cursor_data.data.rows.erase(cursor_data.data.rows.begin(), cursor_data.data.rows.begin() + cursor_data.offset);
        cursor_data.data.name = label;
        state.emitted.push_back(std::move(cursor_data.data));
        state.cursors.erase(found);
    }

    NamedSet read_set(Plan plan, ParameterValues values, size_t limit)
    {
        NamedSet data;
        data.columns = query_columns(plan);
        if (auto* simple = std::get_if<SelectPlan>(&plan))
        {
            Cursor cursor(std::move(*simple), std::move(values), limit);
            size_t bytes{};
            while (data.rows.size() < limit)
            {
                auto part = cursor.fetch(tx_, owner_, limit - data.rows.size(), 128 * 1024, check_);
                for (auto& row : part.rows)
                {
                    bytes += disk::encode({1, row}).size();
                    if (bytes > 512 * 1024) throw Error(Code::limit, {}, "Routine cursor byte budget reached");
                    data.rows.push_back(std::move(row));
                }
                if (part.done) break;
            }
        }
        else if (auto* advanced = std::get_if<AdvancedSelection>(&plan))
            data.rows = run_advanced(*advanced->plan, tx_, owner_, values, check_, 512 * 1024, limit);
        else if (auto* routine = std::get_if<std::shared_ptr<const RoutineSelection>>(&plan))
        {
            auto called = run_routine(**routine, tx_, owner_, values, access_, check_);
            if (called.sets.size() != 1) throw Error(Code::type, {}, "Hosted SELECT requires a single result set");
            data.rows = std::move(called.sets.front().rows);
        }
        return data;
    }

    RowValues into(const pl::Program& program, const pl::Instruction& step, const NamedSet& data, size_t row)
    {
        if (step.targets.size() != data.columns.size()) throw Error(Code::type, step.source, "INTO width mismatch");
        const auto& values = data.rows.at(row);
        RowValues converted;
        for (size_t index = 0; index < values.size(); ++index)
            converted.push_back(coerce(values[index], data.columns[index].expression.type,
                program.slots.at(step.targets.at(index)).type, step.source));
        return converted;
    }

    RowValues host_action(const pl::Program& program, State& state, const pl::Instruction& step,
        std::span<const disk::Value> frame)
    {
        using Kind = pl::Node::Kind;
        if (check_) check_();
        if (step.kind == Kind::emit_cursor)
        {
            emit(state, step.cursor, step.result_name, step.source);
            return {};
        }
        if (step.kind == Kind::fetch_cursor || step.kind == Kind::close_cursor)
        {
            auto found = state.cursors.find(step.cursor);
            if (found == state.cursors.end()) throw pl::RuntimeError("invalid_cursor", step.source, "Cursor is not open");
            if (step.kind == Kind::close_cursor)
            {
                state.memory -= found->second.bytes;
                state.cursors.erase(found);
                return {};
            }
            auto& cursor = found->second;
            if (cursor.offset == cursor.data.rows.size()) return {};
            auto values = into(program, step, cursor.data, cursor.offset);
            ++cursor.offset;
            return values;
        }

        const auto statement = parse(step.host_sql);
        if (access_) access_(access_requirements(catalog_, statement));
        auto values = values_for(step.bindings, frame);
        auto plan = sdp::sql::bind(catalog_, statement, host_parameters(step.bindings));
        if (step.kind == Kind::sql_mutation)
        {
            if (!std::holds_alternative<MutationPlan>(plan))
                throw Error(Code::unsupported, step.source, "Hosted write requires INSERT, UPDATE or DELETE");
            state.affected += execute_write(tx_, owner_, plan, values, check_, access_);
            return {};
        }
        auto data = read_set(std::move(plan), std::move(values), step.kind == Kind::select_into ? 2 : 257);
        if (step.kind == Kind::select_into)
        {
            if (data.rows.empty()) throw pl::RuntimeError("no_data_found", step.source, "SELECT INTO returned no data");
            if (data.rows.size() > 1) throw pl::RuntimeError("too_many_rows", step.source, "SELECT INTO returned several rows");
            return into(program, step, data, 0);
        }
        if (state.cursors.contains(step.cursor))
            throw pl::RuntimeError("invalid_cursor", step.source, "Cursor is already open");
        if (data.rows.size() > 256 || state.cursors.size() == 16)
            throw Error(Code::limit, step.source, "Routine cursor row or handle limit reached");
        size_t bytes{};
        for (const auto& row : data.rows) bytes += disk::encode({1, row}).size();
        if (bytes > 512 * 1024 - state.memory) throw Error(Code::limit, step.source, "Routine cursor byte budget reached");
        state.memory += bytes;
        state.cursors.emplace(step.cursor, CursorState{std::move(data), 0, bytes});
        return {};
    }
    disk::Value host(const pl::TypedExpr& expr, std::span<const disk::Value> frame, std::span<const disk::Value> args)
    {
        if (check_) check_();
        if (expr.op == Op::routine_call)
        {
            const auto object = catalog_.resolve(ObjectKind::routine, expr.name);
            const auto required = object_access(catalog_.list(), object, "execute");
            if (access_) access_(std::span{&required, 1});
            const auto program = load_program(catalog_, object);
            if (args.size() != program.parameters || expr.args.size() != args.size()) damaged();
            RowValues coerced;
            for (size_t slot = 0; slot < args.size(); ++slot)
                coerced.push_back(coerce(args[slot], expr.args[slot].type, program.slots[slot].type, expr.source));
            return run(program, coerced).values.returned.value_or(disk::Value{});
        }
        auto query = parse(expr.text);
        if (access_) access_(access_requirements(catalog_, query));
        const auto limit = expr.op == Op::exists ? 1u : 2u;
        auto plan = sdp::sql::bind(catalog_, query, host_parameters(expr.bindings));
        auto rows = read_set(std::move(plan), values_for(expr.bindings, frame), limit).rows;
        if (expr.op == Op::exists) return boolean_value(rows.empty() ? Truth::false_ : Truth::true_);
        if (rows.size() > 1) throw Error(Code::constraint, expr.source, "Scalar SQL returned multiple rows");
        if (rows.empty()) return {};
        if (rows[0].size() != 1) damaged();
        return std::move(rows[0][0]);
    }

    disk::Transaction& tx_;
    disk::Owner owner_;
    const InvokerCheck& access_;
    const WorkCheck& check_;
    Catalog catalog_;
    const pl::Instrumentation* instrumentation_;
};

std::string compile_definition(Catalog& catalog, const Statement& statement, pl::Routine definition, uint32_t version)
{
    Revisions used;
    Json descriptor;
    const bool trigger = statement.action == Action::create_trigger;
    if (trigger)
    {
        const auto table = catalog.resolve(ObjectKind::table, statement.related);
        used.emplace(table.id, table.version);
        definition = trigger_body(definition, table);
        descriptor = {{"table", table.id}, {"timing", statement.trigger_timing}, {"event", statement.trigger_event}};
    }
    const auto program = compile_program(catalog, definition, version, used);
    auto refs = Json::array();
    for (const auto& [id, revision] : used) refs.push_back(Json::array({id, revision}));
    Json object{{"format", 1}, {"sql", statement.routine->source}, {"ir", Json::parse(pl::serialize(program))},
        {"dependencies", std::move(refs)}};
    if (trigger) object["trigger"] = std::move(descriptor);
    auto bytes = object.dump();
    if (bytes.size() > 16 * 1024) throw Error(Code::limit, {}, "Routine exceeds catalog definition quota");
    return bytes;
}
}
bool routine_ddl(Catalog& catalog, const Statement& statement, bool apply)
{
    if (statement.action == Action::drop_trigger)
    {
        const auto trigger = catalog.resolve(ObjectKind::routine, statement.target);
        if (!record(trigger).contains("trigger")) throw Error(Code::name, statement.source, "Trigger does not exist");
        if (apply) catalog.remove(trigger);
        return true;
    }
    const bool trigger = statement.action == Action::create_trigger;
    if (!trigger && statement.action != Action::create_routine) return false;
    if (!statement.routine) damaged();
    const auto& source = statement.routine->source;
    auto definition = source_routine(source);
    const auto name = definition->name;
    std::optional<Object> earlier;
    try { earlier = catalog.resolve(ObjectKind::routine, name); }
    catch (const Error& error) { if (error.code() != Code::name) throw; }
    if (earlier && !definition->replace)
        throw Error(Code::name, name.source, "Routine already exists");
    if (!earlier) catalog.validate_auxiliary(ObjectKind::routine, name);
    if (earlier && earlier->version == UINT32_MAX) throw Error(Code::limit, {}, "Routine revision exhausted");

    auto bytes = compile_definition(catalog, statement, std::move(*definition), earlier ? earlier->version + 1u : 1u);
    if (apply)
    {
        if (earlier) catalog.update_definition(*earlier, std::move(bytes), true);
        else catalog.create_auxiliary(ObjectKind::routine, name, std::move(bytes));
    }
    return true;
}

void restore_history_routine(Catalog& catalog, const Object& historical)
{
    const auto source = record(historical).at("sql").get<std::string>();
    const auto statement = parse(source);
    if ((statement.action != Action::create_routine && statement.action != Action::create_trigger) || !statement.routine)
        damaged();

    const auto objects = catalog.list();
    const auto current = std::ranges::find(objects, historical.id, &Object::id);
    const auto version = current == objects.end() ? historical.version : current->version;
    if (version == UINT32_MAX) throw Error(Code::limit, {}, "Routine revision exhausted");
    auto bytes = compile_definition(catalog, statement, *statement.routine, version + 1);
    if (current != objects.end())
    {
        if (current->kind != ObjectKind::routine || current->parent != historical.parent || current->name != historical.name)
            throw Error(Code::constraint, {}, "Historical routine identity changed");
        catalog.update_definition(*current, std::move(bytes), true);
    }
    else
    {
        auto restored = historical;
        restored.version = version + 1;
        restored.definition = std::move(bytes);
        catalog.restore_object(restored);
    }
}

bool selects_routine(const Statement& statement)
{
    if (statement.action != Action::select) return false;
    const auto contains_call = [](const auto& self, const Expr& expression) -> bool {
        return expression.op == Op::routine_call ||
            std::ranges::any_of(expression.args, [&](const Expr& child) { return self(self, child); });
    };
    return std::ranges::any_of(statement.projection, [&](const Expr& expr) { return contains_call(contains_call, expr); });
}

std::shared_ptr<const RoutineSelection> plan_routine(Catalog& catalog, const Statement& statement,
    const ParameterTypes& parameters)
{
    auto output = std::make_shared<RoutineSelection>();
    if (statement.action == Action::pl_block)
    {
        if (!statement.routine) damaged();
        Revisions used;
        output->program = compile_program(catalog, *statement.routine, 1, used);
    }
    else
    {
        auto args = std::span<const Expr>{statement.projection};
        auto name = statement.target;
        if (statement.action == Action::select)
        {
            if (args.size() != 1 || args.front().op != Op::routine_call || !name.parts.empty() || statement.filter ||
                advanced_select(statement))
                throw Error(Code::unsupported, statement.source, "Use a standalone SELECT function call");
            name = args.front().name;
            args = args.front().args;
        }
        output->program = load_program(catalog, catalog.resolve(ObjectKind::routine, name));
        if ((statement.action == Action::select) != output->program.returns.has_value())
            throw Error(Code::type, statement.source, "Functions use SELECT and procedures use CALL");
        const auto& program = output->program;
        size_t inputs{};
        for (size_t slot = 0; slot < program.parameters; ++slot)
            if (program.slots[slot].mode != pl::Mode::out) ++inputs;
        if (args.size() != inputs && args.size() != program.parameters)
            throw Error(Code::type, statement.source, "Wrong number of routine arguments");
        const bool full = args.size() == program.parameters;
        size_t index{};
        for (size_t slot = 0; slot < program.parameters; ++slot)
        {
            const auto& variable = program.slots[slot];
            if (variable.mode == pl::Mode::out && !full) continue;
            const auto& arg = args[index++];
            if (variable.mode == pl::Mode::out && arg.op != Op::null)
                throw Error(Code::type, arg.source, "OUT argument uses a NULL placeholder");
            auto bound = bind_expression(catalog, arg, parameters);
            if (!can_coerce(bound.type, variable.type))
                throw Error(Code::type, arg.source, "Routine argument has wrong type");
            output->arguments.push_back(std::move(bound));
        }
    }

    const auto column = [&](std::string name, Type type) {
        BoundExpression expression;
        expression.type = type;
        output->columns.push_back({std::move(name), std::move(expression)});
    };
    if (output->program.returns) column("return_value", *output->program.returns);
    for (size_t slot = 0; slot < output->program.parameters; ++slot)
    {
        const auto& variable = output->program.slots[slot];
        if (variable.mode == pl::Mode::out || variable.mode == pl::Mode::in_out)
            column(variable.name, variable.type);
    }
    return output;
}

const std::vector<Projection>& routine_columns(const RoutineSelection& plan) { return plan.columns; }

bool routine_writes(const RoutineSelection& plan) { return plan.program.writes_sql; }

RoutineResults run_routine(const RoutineSelection& plan, disk::Transaction& transaction,
    disk::Owner owner, const ParameterValues& parameters, const InvokerCheck& rights, const WorkCheck& check,
    const pl::Instrumentation* instrumentation)
{
    RowValues values;
    size_t argument{};
    const bool full = plan.arguments.size() == plan.program.parameters;
    for (size_t slot = 0; slot < plan.program.parameters; ++slot)
    {
        const auto& target = plan.program.slots[slot];
        if (target.mode == pl::Mode::out && !full) continue;
        const auto& bound = plan.arguments[argument++];
        values.push_back(coerce(evaluate(bound, {}, parameters), bound.type, target.type, bound.source));
    }

    std::optional<disk::Savepoint> guard;
    if (routine_writes(plan)) guard = transaction.statement_savepoint(owner);
    try
    {
        auto result = Invocation(transaction, owner, rights, check, instrumentation).run(plan.program, values);
        RowValues row;
        if (result.values.returned) row.push_back(std::move(*result.values.returned));
        for (auto& value : result.values.out) row.push_back(std::move(value.value));
        if (row.size() != plan.columns.size()) damaged();
        if (!row.empty()) result.sets.insert(result.sets.begin(), {"out", plan.columns, {std::move(row)}});
        if (guard) transaction.release_savepoint(owner, *guard);
        return {std::move(result.sets), result.affected};
    }
    catch (...)
    {
        if (guard)
        {
            try
            {
                transaction.rollback_to(owner, *guard);
                transaction.release_savepoint(owner, *guard);
            }
            catch (const disk::Error&)
            {
                try { transaction.rollback(owner); }
                catch (const disk::Error&) {}
            }
        }
        throw;
    }
}

bool has_trigger_dependency(Catalog& catalog, ObjectId table)
{
    for (const auto& object : catalog.list())
    {
        if (object.kind != ObjectKind::routine) continue;
        const auto value = record(object);
        if (value.contains("trigger") && value["trigger"].at("table") == table) return true;
    }
    return false;
}

void fire_triggers(disk::Transaction& transaction, disk::Owner owner, const Object& table,
    std::string_view timing, std::string_view event, const RowValues* before, const RowValues* after,
    const WorkCheck& check, const InvokerCheck& invoker)
{
    Catalog catalog(transaction, owner, check);
    for (const auto& object : catalog.list())
    {
        if (object.kind != ObjectKind::routine) continue;
        const auto value = record(object);
        if (!value.contains("trigger")) continue;
        const auto& item = value.at("trigger");
        if (item.at("table") != table.id || item.at("timing").get_ref<const std::string&>() != timing || item.at("event").get_ref<const std::string&>() != event) continue;

        static thread_local size_t depth{};
        if (depth >= 8) throw Error(Code::limit, {}, "Trigger recursion limit reached");
        ++depth;
        struct Leave { size_t& depth; ~Leave() { --depth; } } leave{depth};

        auto program = load_program(catalog, object, &table);
        RowValues arguments;
        const auto append = [&](const RowValues* image) {
            if (image) arguments.insert(arguments.end(), image->begin(), image->end());
            else arguments.resize(arguments.size() + table.columns.size());
        };
        append(before);
        append(after);
        const auto result = Invocation(transaction, owner, invoker, check).run(program, arguments);
        if (!result.sets.empty()) throw Error(Code::unsupported, {}, "Triggers cannot return cursor results");
    }
}

std::vector<ObjectId> routine_dependencies(const Object& object)
{
    std::vector<ObjectId> ids;
    for (const auto& [id, version] : revisions(record(object))) ids.push_back(id);
    return ids;
}
}
