#pragma once

#include "pl_runtime.hpp"
#include <sdp/wire.hpp>

#include <condition_variable>
#include <mutex>
#include <set>

namespace sdp::sql
{
class Debugger
{
public:
    Debugger(std::string id, Name routine, uint32_t revision,
        std::chrono::milliseconds pause_timeout = std::chrono::milliseconds(3000));

    const std::string& id() const noexcept { return id_; }
    wire::Json command(std::string_view op, const wire::Json& body);
    void trace(std::span<const pl::TraceFrame> frames, bool exception, const WorkCheck& check);
    void cancel() noexcept;
    void finish() noexcept;

private:
    enum class State { armed, running, paused, finished, cancelled, timed_out };
    enum class Step { none, into, over, out };

    bool target(const Name& routine) const noexcept;
    bool matches_breakpoint(const pl::TraceFrame& leaf) const;
    bool matches_step(size_t depth) const noexcept;
    bool terminal() const noexcept;
    void stop(State state);
    void check_work(const WorkCheck& check);
    wire::Json status() const;

    const std::string id_;
    const Name routine_;
    const uint32_t revision_;
    const std::chrono::milliseconds pause_timeout_;
    std::mutex mutex_;
    std::condition_variable changed_;
    State state_ = State::armed;
    Step step_ = Step::none;
    size_t step_depth_{};
    bool exception_stop_{};
    std::set<uint32_t> breakpoints_;
    wire::Json stack_;
    const char* reason_{};
};
}
