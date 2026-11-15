#pragma once

#include "server.hpp"
#include "../storage/transactions.hpp"

#include <atomic>
#include <mutex>

namespace sdp::jobs { class Manager; }

namespace sdp::server
{
class Operations
{
public:
    Operations(const Config& config, disk::Engine& engine, jobs::Manager& jobs);
    wire::Json request(std::string_view operation, const wire::Json& body, std::stop_token stop);
    std::chrono::milliseconds timeout() const noexcept { return std::chrono::milliseconds(timeout_ms_.load()); }
    const std::string& instance_id() const noexcept { return instance_id_; }

private:
    wire::Json measure(std::string_view check, const std::function<void()>& poll);
    wire::Json configure(const wire::Json& body, bool apply);
    const Config& config_;
    disk::Engine& engine_;
    jobs::Manager& jobs_;
    std::string instance_id_;
    std::atomic<int64_t> timeout_ms_;
    std::mutex mutex_;
    wire::Json status_;
};

void log_diagnostic(const Config& config, std::string_view channel, std::string_view event,
    uint32_t code = 0, std::string_view correlation = {}, uint64_t duration_ms = 0) noexcept;
}
