#pragma once

#include "../sql/relational.hpp"

#include <sdp/wire.hpp>

namespace sdp::ml
{
using MlpRows = std::span<const std::vector<double>>;
using MlpCheckpoint = std::function<void(const wire::Json&)>;

struct MlpGradient
{
    double loss;
    std::vector<double> gradient;
};

wire::Json fit_mlp(MlpRows train, std::span<const double> train_y,
    MlpRows validation, std::span<const double> validation_y, const wire::Json& options, uint64_t seed,
    const sql::WorkCheck& check = {}, const wire::Json& resume = {}, const MlpCheckpoint& checkpoint = {});
double predict_mlp(const wire::Json& artifact, std::span<const double> row);
MlpGradient mlp_loss_gradient(std::span<const uint32_t> layers, std::string_view activation,
    std::span<const double> parameters, MlpRows rows, std::span<const double> labels);
}
