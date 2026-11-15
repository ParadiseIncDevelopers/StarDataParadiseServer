#pragma once

#include "pl_ir.hpp"
#include "executor.hpp"

namespace sdp::pl
{
struct Limits
{
    size_t instructions{100'000};
    size_t depth{32};
    size_t memory{1024 * 1024};
    std::chrono::milliseconds time{2000};
};

struct OutValue
{
    std::string name;
    sql::Type type;
    disk::Value value;
};

struct Result
{
    std::optional<disk::Value> returned;
    std::vector<OutValue> out;
};

class RuntimeError : public sql::Error
{
public:
    RuntimeError(std::string name, sql::Source source, std::string message,
        sql::Code code = sql::Code::constraint);
    std::string_view name() const noexcept { return name_; }
    bool mark_traced() const noexcept;

private:
    std::string name_;
    mutable bool traced_{};
};

using SqlCallback = std::function<disk::Value(const TypedExpr&, std::span<const disk::Value> frame,
    std::span<const disk::Value> evaluated_args)>;
using HostAction = std::function<std::vector<disk::Value>(const Instruction&, std::span<const disk::Value> frame)>;

struct TraceFrame
{
    const Program* program;
    sql::Source source;
    std::span<const disk::Value> values;
};

struct ProfileSample
{
    sql::Name routine;
    uint64_t revision;
    uint64_t inclusive_us;
    uint64_t exclusive_us;
    uint64_t sql_us;
    uint64_t instructions;
};

struct Instrumentation
{
    // A callback sees the live frames from outermost to innermost until it returns.
    std::function<void(std::span<const TraceFrame>, bool exception)> trace;
    std::function<void(const ProfileSample&)> profile;
};

Result invoke(const Program& program, std::span<const disk::Value> arguments, const SqlCallback& sql = {},
    const sql::WorkCheck& check = {}, Limits limits = {}, const HostAction& action = {},
    const Instrumentation& instrumentation = {});
}
