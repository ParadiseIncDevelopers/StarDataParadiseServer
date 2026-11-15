#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <sdp/wire.hpp>

namespace sdp::jobs
{
inline constexpr size_t dataset_worker_request_limit = 128 * 1024;

uint64_t dataset_routine_calls(const wire::Json& payload);
void validate_dataset_routine(std::string_view source);
void validate_dataset_calls(std::string_view source, const wire::Json& calls);
std::optional<wire::Json> run_dataset_routine(const wire::Json& payload, size_t output_limit,
    const std::function<void(uint64_t done)>& progress);
}
