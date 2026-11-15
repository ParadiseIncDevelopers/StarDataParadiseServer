#pragma once

#include "../sql/relational.hpp"
#include <sdp/wire.hpp>

namespace sdp::ml
{
inline constexpr auto mlp_runtime = "sdp-mlp-cpu-v1";
bool mlp_compiled() noexcept;
std::string_view training_kind(std::string_view algorithm);
uint64_t training_steps(const wire::Json& payload);
bool supervised_algorithm(std::string_view algorithm);
bool model_algorithm(std::string_view algorithm);
wire::Json train_model(const wire::Json& payload, const std::function<void(uint64_t)>& progress = {});
double predict_supervised(std::string_view algorithm, const wire::Json& artifact, std::span<const double> row);
bool valid_training_transfer(const wire::Json& output, const wire::Json& payload);
bool valid_training_result(const wire::Json& output, std::string_view expected);
bool check_model_artifact(std::string_view algorithm, const wire::Json& artifact, size_t width);
}
