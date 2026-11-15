#pragma once

#include "../sql/executor.hpp"
#include <sdp/wire.hpp>

namespace sdp::ml
{
wire::Json fit_tree_model(std::string_view algorithm, std::span<const std::vector<double>> x,
    std::span<const double> y, const wire::Json& options, uint64_t seed, const sql::WorkCheck& check = {});
double predict_tree_model(std::string_view algorithm, const wire::Json& artifact, std::span<const double> row);
}
