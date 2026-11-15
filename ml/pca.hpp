#pragma once

#include "../sql/relational.hpp"

#include <sdp/wire.hpp>

namespace sdp::ml
{
wire::Json fit_pca(std::span<const std::vector<double>> train, const wire::Json& options,
    const sql::WorkCheck& check = {});
std::vector<double> transform_pca(const wire::Json& artifact, std::span<const double> row);
std::vector<double> reconstruct_pca(const wire::Json& artifact, std::span<const double> scores);
}
