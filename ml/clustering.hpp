#pragma once

#include "../sql/executor.hpp"
#include <sdp/wire.hpp>

namespace sdp::ml
{
wire::Json fit_clustering(std::string_view algorithm, std::span<const std::vector<double>> x,
    const wire::Json& options, uint64_t seed, const sql::WorkCheck& check = {});
size_t assign_cluster(const wire::Json& artifact, std::span<const double> row);
wire::Json silhouette(std::span<const std::vector<double>> x, std::span<const int64_t> labels,
    const sql::WorkCheck& check = {});
}
