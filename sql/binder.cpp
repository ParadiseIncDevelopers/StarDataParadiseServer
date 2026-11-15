#include "binder.hpp"
#include "advanced.hpp"
#include "routines.hpp"
#include "definition.hpp"
#include "relational.hpp"

#include <algorithm>
#include <numeric>
#include <set>

namespace sdp::sql
{
namespace
{
using Op = Expr::Op;
constexpr Type boolean_type{Kind::boolean};
constexpr Type null_type{Kind::null};

Type literal_type(const Expr& expr)
{
    switch (expr.op)
    {
    case Op::null: return null_type;
    case Op::boolean: return boolean_type;
    case Op::integer: return {Kind::integer};
    case Op::decimal: return decimal_literal_type(expr.text, expr.source);
    case Op::text: return {Kind::text};
    case Op::date: return {Kind::date};
    case Op::time: return {Kind::time};
    case Op::timestamp_tz: return {Kind::timestamp_tz};
    default: throw Error(Code::type, expr.source, "Invalid literal");
    }
}

void require_truth(Type type, Source source)
{
    if (!can_coerce(type, boolean_type))
        throw Error(Code::type, source, "A boolean expression is required");
}

class Binder
{
public:
    Binder(Catalog& catalog, const Statement& statement, const ParameterTypes& parameters, const Object* input = nullptr)
        : statement_(statement), parameters_(parameters)
    {
        if (input)
        {
            table_ = *input;
            qualifier_ = catalog.qualified(*input).parts;
        }
        else if (!statement.target.parts.empty())
        {
            auto relation = statement.action == Action::select ? catalog.resolve_relation(statement.target) :
                catalog.resolve(ObjectKind::table, statement.target);
            if (statement.alias) qualifier_ = {*statement.alias};
            else qualifier_ = catalog.qualified(relation).parts;
            if (relation.kind == ObjectKind::view)
            {
                auto view = read_view(catalog, relation);
                table_ = std::move(view.table);
                projection_ = std::move(view.columns);
            }
            else table_ = std::move(relation);
        }
    }

    BoundExpression scalar(const Expr& expr) const { return bind_expression(expr); }

    SelectPlan select() const
    {
        SelectPlan plan{table_, {}, bind_filter()};
        if (statement_.all_columns)
        {
            if (!table_) throw Error(Code::name, statement_.source, "SELECT * requires a table");
            const auto count = projection_.empty() ? table_->columns.size() : projection_.size();
            for (size_t visible = 0; visible < count; ++visible)
            {
                const auto index = projection_.empty() ? visible : projection_[visible];
                const auto& column = table_->columns[index];
                plan.projection.push_back({column.name, column_expression(index, statement_.source)});
            }
        }
        else
        {
            size_t index{};
            for (const auto& expression : statement_.projection)
            {
                ++index;
                auto bound = bind_expression(expression);
                const auto label = expression.op == Op::name ? expression.name.parts.back() :
                    "column" + std::to_string(index);
                plan.projection.push_back({label, std::move(bound)});
            }
        }
        return plan;
    }

    MutationPlan mutate() const
    {
        if (!table_) throw Error(Code::name, statement_.source, "Write requires a target table");
        MutationPlan plan{statement_.action, statement_.source, *table_, {}, {}, {}, bind_filter()};
        if (statement_.action == Action::insert)
        {
            if (statement_.insert_columns.empty())
            {
                plan.insert_columns.resize(table_->columns.size());
                std::iota(plan.insert_columns.begin(), plan.insert_columns.end(), size_t{});
            }
            else
            {
                std::set<size_t> columns;
                for (const auto& name : statement_.insert_columns)
                {
                    const auto index = assignment_index(name);
                    if (!columns.insert(index).second)
                        throw Error(Code::name, name.source, "Repeated column in INSERT");
                    plan.insert_columns.push_back(index);
                }

            }
            for (const auto& row : statement_.values)
            {
                if (row.size() != plan.insert_columns.size())
                    throw Error(Code::type, statement_.source, "INSERT value count does not match the target columns");
                auto& values = plan.rows.emplace_back();
                for (size_t index = 0; index < row.size(); ++index)
                    values.push_back(bind_assignment(plan.insert_columns[index], row[index]));
            }
        }
        else if (statement_.action == Action::update)
        {
            std::set<size_t> columns;
            for (const auto& assignment : statement_.assignments)
            {
                const auto index = assignment_index(assignment.column);
                if (!columns.insert(index).second)
                    throw Error(Code::name, assignment.column.source, "Repeated column in UPDATE");
                plan.assignments.push_back({index, bind_assignment(index, assignment.value)});
            }
        }
        return plan;
    }

private:
    std::optional<BoundExpression> bind_filter() const
    {
        if (!statement_.filter) return {};
        auto filter = bind_expression(*statement_.filter);
        require_truth(filter.type, filter.source);
        return filter;
    }

    size_t assignment_index(const Name& name) const
    {
        if (name.parts.size() != 1) throw Error(Code::name, name.source, "A target column must be unqualified");
        return lookup_column(name.parts.front(), name.source);
    }

    size_t lookup_column(std::string_view name, Source source) const
    {
        const auto& columns = table_->columns;
        const auto found = std::ranges::find(columns, name, &Column::name);
        if (found == columns.end()) throw Error(Code::name, source, "Unknown column");
        const auto index = static_cast<size_t>(found - columns.begin());
        if (!projection_.empty() && std::ranges::find(projection_, index) == projection_.end())
            throw Error(Code::name, source, "Column is outside the view projection");
        return index;
    }

    BoundExpression bind_assignment(size_t index, const Expr& expr) const
    {
        auto value = bind_expression(expr);
        const auto& column = table_->columns[index];
        if (!can_coerce(value.type, column.type))
            throw Error(Code::type, value.source, "Value type does not match the target column");
        return value;
    }

    BoundExpression column_expression(size_t index, Source source) const
    {
        BoundExpression expression;
        expression.op = Op::name;
        expression.type = table_->columns[index].type;
        expression.source = source;
        expression.column = index;
        return expression;
    }

    BoundExpression resolve_column(const Expr& expr) const
    {
        const auto& parts = expr.name.parts;
        if (!table_ || parts.empty() || statement_.action == Action::insert)
            throw Error(Code::name, expr.source, "Column requires a visible table");
        const size_t prefix = parts.size() - 1;
        if (prefix > qualifier_.size() ||
            !std::equal(parts.begin(), parts.begin() + prefix, qualifier_.end() - prefix))
            throw Error(Code::name, expr.source, "Unknown column qualifier");
        return column_expression(lookup_column(parts.back(), expr.source), expr.source);
    }

    BoundExpression bind_expression(const Expr& expr) const
    {
        if (expr.op == Op::name) return resolve_column(expr);
        BoundExpression out;
        out.op = expr.op;
        out.source = expr.source;
        if (expr.op == Op::parameter)
        {
            const auto type = parameters_.find(expr.text);
            if (type == parameters_.end()) throw Error(Code::name, expr.source, "Parameter type is missing");
            validate_type(type->second, expr.source);
            out.type = type->second;
            out.parameter = expr.text;
            return out;
        }

        switch (expr.op)
        {
        case Op::null:
        case Op::boolean:
        case Op::integer:
        case Op::decimal:
        case Op::text:
        case Op::date:
        case Op::time:
        case Op::timestamp_tz:
            out.type = literal_type(expr);
            out.literal = parse_literal(out.type, expr.text, expr.source);
            return out;
        default:
            break;
        }

        for (const auto& arg : expr.args) out.operands.push_back(bind_expression(arg));
        out.type = boolean_type;
        switch (expr.op)
        {
        case Op::logical_not:
        case Op::and_:
        case Op::or_:
            for (const auto& arg : out.operands) require_truth(arg.type, arg.source);
            break;
        case Op::is_null:
        case Op::is_not_null:
            break;
        case Op::negate:
            out.type = out.operands.at(0).type;
            if (out.type.kind != Kind::null && out.type.kind != Kind::integer && out.type.kind != Kind::decimal)
                throw Error(Code::type, expr.source, "Unary minus requires a number");
            break;
        case Op::eq:
        case Op::ne:
        case Op::lt:
        case Op::le:
        case Op::gt:
        case Op::ge:
            if (!comparable(out.operands.at(0).type, out.operands.at(1).type))
                throw Error(Code::type, expr.source, "Comparison operand types do not match");
            break;
        default:
            throw Error(Code::unsupported, expr.source, "Expression is unsupported");
        }
        return out;
    }

    const Statement& statement_;
    const ParameterTypes& parameters_;
    std::optional<Object> table_;
    std::vector<std::string> qualifier_;
    std::vector<size_t> projection_;
};
}

BoundExpression bind_expression(Catalog& catalog, const Object& table, const Expr& expression)
{
    const Statement select{Action::select, expression.source};
    return Binder(catalog, select, {}, &table).scalar(expression);
}

BoundExpression bind_expression(Catalog& catalog, const Expr& expression, const ParameterTypes& parameters)
{
    const Statement select{Action::select, expression.source};
    return Binder(catalog, select, parameters).scalar(expression);
}

Plan bind(Catalog& catalog, const Statement& statement, const ParameterTypes& parameters)
{
    if (statement.action == Action::call_routine || statement.action == Action::pl_block || selects_routine(statement))
        return plan_routine(catalog, statement, parameters);
    if (statement.action == Action::select)
    {
        if (advanced_select(statement))
            return AdvancedSelection{plan_advanced(catalog, statement, parameters), QueryMode::execute};
        return Binder(catalog, statement, parameters).select();
    }
    if (statement.action == Action::explain || statement.action == Action::explain_analyze)
    {
        if (!statement.query || statement.query->action != Action::select)
            throw Error(Code::unsupported, statement.source, "Only SELECT plans are supported");
        const auto mode = statement.action == Action::explain ? QueryMode::explain : QueryMode::explain_analyze;
        return AdvancedSelection{plan_advanced(catalog, *statement.query, parameters), mode};
    }
    if (statement.action == Action::insert || statement.action == Action::update || statement.action == Action::delete_)
        return Binder(catalog, statement, parameters).mutate();
    if (!routine_ddl(catalog, statement, false) && !auxiliary_ddl(catalog, statement, false) &&
        !relational_ddl(catalog, statement)) catalog.validate(statement);
    return statement;
}

Plan check(Catalog& catalog, std::string_view text, const ParameterTypes& parameters)
{
    return sdp::sql::bind(catalog, parse(text), parameters);
}

disk::Value evaluate(const BoundExpression& expr, std::span<const disk::Value> row,
    const ParameterValues& parameters)
{
    const auto eval = [&](size_t index) { return evaluate(expr.operands.at(index), row, parameters); };
    const auto is_null = [](const disk::Value& value) { return std::holds_alternative<std::monostate>(value); };
    const auto boolean = [](bool value) { return boolean_value(value ? Truth::true_ : Truth::false_); };
    switch (expr.op)
    {
    case Op::name:
        if (expr.column >= row.size()) throw Error(Code::name, expr.source, "Input column is missing");
        return row[expr.column];
    case Op::parameter:
    {
        const auto found = parameters.find(expr.parameter);
        if (found == parameters.end()) throw Error(Code::name, expr.source, "Parameter value is missing");
        return coerce(found->second, expr.type, expr.type, expr.source);
    }
    case Op::logical_not:
        return boolean_value(logical_not(truth(eval(0), expr.source)));
    case Op::and_:
    {
        const auto left = truth(eval(0), expr.source);
        if (left == Truth::false_) return boolean_value(left);
        return boolean_value(logical_and(left, truth(eval(1), expr.source)));
    }
    case Op::or_:
    {
        const auto left = truth(eval(0), expr.source);
        if (left == Truth::true_) return boolean_value(left);
        return boolean_value(logical_or(left, truth(eval(1), expr.source)));
    }
    case Op::negate:
        return negate_numeric(eval(0), expr.type, expr.source);
    case Op::is_null:
        return boolean(is_null(eval(0)));
    case Op::is_not_null:
        return boolean(!is_null(eval(0)));
    case Op::eq:
    case Op::ne:
    case Op::lt:
    case Op::le:
    case Op::gt:
    case Op::ge:
    {
        const auto left = eval(0);
        const auto right = eval(1);
        if (is_null(left) || is_null(right)) return {};
        const int order = compare_non_null(left, expr.operands[0].type, right, expr.operands[1].type, expr.source);
        switch (expr.op)
        {
        case Op::eq: return boolean(order == 0);
        case Op::ne: return boolean(order != 0);
        case Op::lt: return boolean(order < 0);
        case Op::le: return boolean(order <= 0);
        case Op::gt: return boolean(order > 0);
        default: return boolean(order >= 0);
        }
    }
    default:
        return expr.literal;
    }
}

bool passes_filter(const SelectPlan& plan, std::span<const disk::Value> row, const ParameterValues& parameters)
{
    return !plan.filter ||
        truth(evaluate(*plan.filter, row, parameters), plan.filter->source) == Truth::true_;
}
}
