#pragma once

#include "pl_runtime.hpp"

namespace sdp::sql
{
Type decision_type(Catalog& catalog, const Object& table, const Expr& expression);
std::vector<ObjectId> decision_dependencies(Catalog& catalog, const Object& table, const Expr& expression);
disk::Value decision_value(disk::Transaction& tx, disk::Owner owner, const Object& table,
    const Expr& expression, const RowValues* before, const RowValues* after,
    std::string_view actor, const WorkCheck& check = {});

struct DecisionConditionResult
{
    disk::Value value;
    std::vector<pl::ExpressionSample> samples;
};

DecisionConditionResult trace_decision_value(disk::Transaction& tx, disk::Owner owner, const Object& table,
    const Expr& expression, const RowValues* before, const RowValues* after,
    std::string_view actor, const WorkCheck& check = {});
}
