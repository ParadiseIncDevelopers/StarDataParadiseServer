#pragma once

#include "../sql/relational.hpp"

#include <sdp/wire.hpp>

namespace sdp::ml
{
wire::Json fit_linear_model(std::string_view algorithm, std::span<const std::vector<double>> x,
    std::span<const double> y, const wire::Json& options, const sql::WorkCheck& check = {});
double predict_linear_model(std::string_view algorithm, const wire::Json& artifact, std::span<const double> row);
}
