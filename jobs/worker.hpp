#pragma once

#include <filesystem>
#include <functional>
#include <sdp/wire.hpp>

namespace sdp::jobs
{
struct Budget
{
    uint32_t timeout_ms;
    uint32_t memory_mib;
    uint32_t output_bytes;
};

struct WorkerResult
{
    enum class Status { succeeded, failed, cancelled };
    Status status;
    std::string reason;
    wire::Json result;
    bool cpu_rate_limited{};
};

using Progress = std::function<bool(uint32_t pid, uint64_t done, uint64_t total)>;

void validate_dataset_routine(std::string_view source);
void validate_dataset_calls(std::string_view source, const wire::Json& calls);
bool valid_worker_request(const wire::Json& payload, Budget budget);
void cleanup_worker_workspaces(const std::filesystem::path& temp) noexcept;
WorkerResult run_worker(const std::filesystem::path& temp, const wire::Json& payload,
    Budget budget, const Progress& progress);
int worker_main(const std::filesystem::path& request, const std::filesystem::path& result,
    const std::filesystem::path& progress);
}
