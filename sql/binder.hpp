#pragma once

#include "catalog.hpp"

#include <map>
#include <memory>
#include <span>
#include <string_view>

namespace sdp::sql
{
using ParameterTypes = std::map<std::string, Type, std::less<>>;
using ParameterValues = std::map<std::string, disk::Value, std::less<>>;

struct BoundExpression
{
    Expr::Op op{};
    Source source;
    Type type;
    disk::Value literal;
    size_t column{};
    std::string parameter;
    std::vector<BoundExpression> operands;
};

struct Projection
{
    std::string column;
    BoundExpression expression;
};

struct SelectPlan
{
    std::optional<Object> table;
    std::vector<Projection> projection;
    std::optional<BoundExpression> filter;
};

struct BoundAssignment
{
    size_t column;
    BoundExpression expression;
};

struct MutationPlan
{
    Action action;
    Source source;
    Object table;
    std::vector<size_t> insert_columns;
    std::vector<std::vector<BoundExpression>> rows;
    std::vector<BoundAssignment> assignments;
    std::optional<BoundExpression> filter;
};

struct AdvancedPlan;
enum class QueryMode { execute, explain, explain_analyze };
struct AdvancedSelection
{
    std::shared_ptr<const AdvancedPlan> plan;
    QueryMode mode = QueryMode::execute;
};
struct RoutineSelection;
using Plan = std::variant<Statement, SelectPlan, MutationPlan, AdvancedSelection, std::shared_ptr<const RoutineSelection>>;

BoundExpression bind_expression(Catalog& catalog, const Object& table, const Expr& expression);
BoundExpression bind_expression(Catalog& catalog, const Expr& expression, const ParameterTypes& parameters = {});
Plan bind(Catalog& catalog, const Statement& statement, const ParameterTypes& parameters = {});
Plan check(Catalog& catalog, std::string_view text, const ParameterTypes& parameters = {});

disk::Value evaluate(const BoundExpression& expression, std::span<const disk::Value> row = {},
    const ParameterValues& parameters = {});
bool passes_filter(const SelectPlan& plan, std::span<const disk::Value> row = {},
    const ParameterValues& parameters = {});
}
