#include "decision_condition.hpp"
#include "advanced.hpp"
#include "pl_runtime.hpp"

#include <algorithm>
#include <set>

namespace sdp::sql
{
namespace
{
using Op = Expr::Op;
using FunctionName = std::vector<std::string>;

[[noreturn]] void unsafe(Source source)
{
    throw Error(Code::unsupported, source, "Decision expressions require deterministic read-only operations");
}

std::vector<pl::Variable> decision_inputs(const Object& table, Source source)
{
    std::vector<pl::Variable> inputs;
    for (const auto image : {"old", "new"})
        for (const auto& column : table.columns)
            inputs.push_back({{{image, column.name}, source}, column.type, pl::Mode::in, {}});
    inputs.push_back({{{"actor"}, source}, {Kind::text}, pl::Mode::in, {}});
    return inputs;
}

void check_query(const Statement& query, size_t depth);

void check_read_expression(const Expr& expression, size_t depth)
{
    if (depth > 32) throw Error(Code::limit, expression.source, "Decision query nesting limit exceeded");
    if (expression.op == Op::routine_call || expression.op == Op::next_value)
        unsafe(expression.source);
    if (expression.query) check_query(*expression.query, depth + 1);
    for (const auto& argument : expression.args) check_read_expression(argument, depth + 1);
}

void check_query(const Statement& query, size_t depth)
{
    if (depth > 32) throw Error(Code::limit, query.source, "Decision query nesting limit exceeded");
    if (query.action != Action::select) unsafe(query.source);
    if (query.query) check_query(*query.query, depth + 1);
    if (query.filter) check_read_expression(*query.filter, depth + 1);
    if (query.having) check_read_expression(*query.having, depth + 1);
    for (const auto& term : query.joins)
        if (term.on) check_read_expression(*term.on, depth + 1);
    for (const auto& expression : query.projection) check_read_expression(expression, depth + 1);
    for (const auto& expression : query.group_by) check_read_expression(expression, depth + 1);
    for (const auto& term : query.order_by) check_read_expression(term.expression, depth + 1);
    for (const auto& item : query.ctes) check_query(*item.query, depth + 1);
    for (const auto& item : query.unions) check_query(*item.query, depth + 1);
}

Expr decision_expression(const Expr& expression)
{
    auto normalized = expression;
    if (normalized.op == Op::name && normalized.name.parts.size() == 1 && normalized.name.parts[0] != "actor")
        normalized.name.parts.insert(normalized.name.parts.begin(), "new");
    for (auto& argument : normalized.args) argument = decision_expression(argument);
    if (normalized.op == Op::exists)
    {
        auto query = parse(normalized.text);
        check_query(query, 0);
        normalized.query = std::make_shared<Statement>(std::move(query));
    }
    else if (normalized.op == Op::subquery || normalized.op == Op::next_value)
        unsafe(normalized.source);
    return normalized;
}

pl::Program load_pure_function(const Object& object, Source source)
{
    const auto record = wire::Json::parse(object.definition, nullptr, false);
    if (!record.is_object() || record.value("format", 0) != 1 || !record.contains("sql") ||
        !record.at("sql").is_string() || !record.contains("ir") || !record.at("ir").is_object())
        throw disk::Error(disk::Fault::corrupt, "Invalid Decision function definition");
    if (record.contains("trigger")) unsafe(source);
    const auto parsed = parse(record.at("sql").get_ref<const std::string&>());
    if (parsed.action != Action::create_routine || !parsed.routine || !parsed.routine->function)
        throw Error(Code::type, source, "Decision requires a scalar function");
    const auto program = pl::compile(*parsed.routine, object.version);
    if (!program.deterministic || program.uses_sql || program.writes_sql || !program.out_cursors.empty() ||
        program.returns->kind >= Kind::record ||
        std::ranges::any_of(std::span(program.slots).first(program.parameters), [](const pl::Slot& slot) {
            return slot.mode != pl::Mode::in || slot.type.kind >= Kind::record;
        }))
        unsafe(source);
    if (wire::Json::parse(pl::serialize(program)) != record.at("ir"))
        throw disk::Error(disk::Fault::corrupt, "Decision function IR differs from stored source");
    return program;
}

class Compilation
{
public:
    Compilation(Catalog& catalog, const Object& table, const Expr& expression)
    {
        const auto normalized = decision_expression(expression);
        program = pl::compile_expression(decision_inputs(table, expression.source), normalized,
            [&](const Expr& host, std::span<const Type> args, std::span<const pl::Binding> bindings) {
                if (host.op == Op::routine_call)
                {
                    auto found = functions.find(host.name.parts);
                    if (found == functions.end())
                    {
                        const auto object = catalog.resolve(ObjectKind::routine, host.name);
                        dependencies.insert(object.id);
                        found = functions.emplace(host.name.parts, load_pure_function(object, host.source)).first;
                    }
                    const auto& function = found->second;
                    if (function.parameters != args.size())
                        throw Error(Code::type, host.source, "Decision scalar function argument count differs");
                    for (size_t index = 0; index < args.size(); ++index)
                        if (!can_coerce(args[index], function.slots[index].type))
                            throw Error(Code::type, host.source, "Decision scalar function argument type differs");
                    return *function.returns;
                }
                if (host.op != Op::exists || !host.query) unsafe(host.source);
                if (!queries.contains(host.text))
                {
                    ParameterTypes parameters;
                    for (const auto& binding : bindings) parameters.emplace(binding.name, binding.type);
                    for (const auto& required : access_requirements(catalog, *host.query))
                        dependencies.insert(required.target);
                    auto selected = sdp::sql::bind(catalog, *host.query, parameters);
                    if (!std::holds_alternative<SelectPlan>(selected) &&
                        !std::holds_alternative<AdvancedSelection>(selected))
                        unsafe(host.source);
                    queries.emplace(host.text, std::move(selected));
                }
                return Type{Kind::boolean};
            });
    }

    pl::Program program;
    std::set<ObjectId> dependencies;
    std::map<FunctionName, pl::Program> functions;
    std::map<std::string, Plan, std::less<>> queries;
};

constexpr pl::Limits limits{4096, 12, 1024 * 1024, std::chrono::milliseconds{200}};

void append_image(RowValues& arguments, const RowValues* image, size_t columns)
{
    if (!image)
    {
        arguments.resize(arguments.size() + columns);
        return;
    }
    if (image->size() != columns) throw disk::Error(disk::Fault::corrupt, "Decision row image has incorrect width");
    arguments.insert(arguments.end(), image->begin(), image->end());
}

disk::Value invoke_decision(const Compilation& compiled, disk::Transaction& tx, disk::Owner owner,
    const RowValues& arguments, const WorkCheck& check, const pl::ExpressionObserver& observer)
{
    const pl::SqlCallback host = [&](const pl::TypedExpr& expression, std::span<const disk::Value> frame,
                                    std::span<const disk::Value> args) -> disk::Value {
        if (check) check();
        if (expression.op == Op::routine_call)
        {
            const auto& function = compiled.functions.at(expression.name.parts);
            RowValues values;
            for (size_t index = 0; index < args.size(); ++index)
                values.push_back(coerce(args[index], expression.args[index].type, function.slots[index].type, expression.source));
            return *pl::invoke(function, values, {}, check, limits).returned;
        }
        ParameterValues values;
        for (const auto& binding : expression.bindings)
            values.emplace(binding.name, frame[binding.slot]);
        const auto& plan = compiled.queries.at(expression.text);
        if (const auto* select = std::get_if<SelectPlan>(&plan))
        {
            Cursor cursor(*select, std::move(values), 1);
            return int64_t{!cursor.fetch(tx, owner, 1, 128 * 1024, check).rows.empty()};
        }
        const auto& advanced = std::get<AdvancedSelection>(plan);
        return int64_t{!run_advanced(*advanced.plan, tx, owner, values, check, 512 * 1024, 1).empty()};
    };
    pl::Instrumentation instrumentation;
    instrumentation.expression = observer;
    try { return *pl::invoke(compiled.program, arguments, host, check, limits, {}, instrumentation).returned; }
    catch (const pl::RuntimeError& error)
    {
        throw Error(error.code(), {}, "Decision evaluation failed");
    }
}

disk::Value evaluate_decision(disk::Transaction& tx, disk::Owner owner, const Object& table,
    const Expr& expression, const RowValues* before, const RowValues* after, std::string_view actor,
    const WorkCheck& check, const pl::ExpressionObserver& observer = {})
{
    Catalog catalog(tx, owner, check);
    const Compilation compiled(catalog, table, expression);
    RowValues arguments;
    arguments.reserve(table.columns.size() * 2 + 1);
    append_image(arguments, before, table.columns.size());
    append_image(arguments, after, table.columns.size());
    arguments.emplace_back(std::string(actor));
    return invoke_decision(compiled, tx, owner, arguments, check, observer);
}
}

Type decision_type(Catalog& catalog, const Object& table, const Expr& expression)
{
    return *Compilation(catalog, table, expression).program.returns;
}

std::vector<ObjectId> decision_dependencies(Catalog& catalog, const Object& table, const Expr& expression)
{
    const Compilation compiled(catalog, table, expression);
    return {compiled.dependencies.begin(), compiled.dependencies.end()};
}

disk::Value decision_value(disk::Transaction& tx, disk::Owner owner, const Object& table,
    const Expr& expression, const RowValues* before, const RowValues* after,
    std::string_view actor, const WorkCheck& check)
{
    return evaluate_decision(tx, owner, table, expression, before, after, actor, check);
}

DecisionConditionResult trace_decision_value(disk::Transaction& tx, disk::Owner owner, const Object& table,
    const Expr& expression, const RowValues* before, const RowValues* after,
    std::string_view actor, const WorkCheck& check)
{
    DecisionConditionResult result;
    result.value = evaluate_decision(tx, owner, table, expression, before, after, actor, check,
        [&](const pl::ExpressionSample& sample) { result.samples.push_back(sample); });
    return result;
}
}
