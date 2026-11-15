#pragma once

#include "executor.hpp"
#include <sdp/wire.hpp>

namespace sdp::sql
{
struct AdvancedPlan;

bool advanced_select(const Statement& statement);
std::shared_ptr<const AdvancedPlan> plan_advanced(Catalog& catalog, const Statement& statement,
    const ParameterTypes& parameters = {});
const std::vector<Projection>& advanced_columns(const AdvancedPlan& plan);
std::vector<RowValues> run_advanced(const AdvancedPlan& plan, disk::Transaction& transaction, disk::Owner owner,
    const ParameterValues& parameters, const WorkCheck& check, size_t memory_limit, size_t row_limit);
wire::Json explain_advanced(const AdvancedPlan& plan);
}
