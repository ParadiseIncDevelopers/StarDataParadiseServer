#include "pl_runtime.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace sdp::pl
{
RuntimeError::RuntimeError(std::string name, sql::Source source, std::string message, sql::Code code)
    : sql::Error(code, source, std::move(message)), name_(std::move(name))
{
}

bool RuntimeError::mark_traced() const noexcept { return !std::exchange(traced_, true); }

namespace
{
using Op = sql::Expr::Op;
using Clock = std::chrono::steady_clock;

[[noreturn]] void limit(sql::Source source, const char* message)
{
    throw sql::Error(sql::Code::limit, source, message);
}

[[noreturn]] void broken(sql::Source source)
{
    throw sql::Error(sql::Code::unsupported, source, "Unsupported or invalid PL/SQL IR");
}

[[noreturn]] void overflow(sql::Source source)
{
    throw RuntimeError("value_error", source, "Signed BIGINT arithmetic overflow", sql::Code::range);
}

bool is_null(const disk::Value& value) { return std::holds_alternative<std::monostate>(value); }

size_t value_bytes(const disk::Value& value)
{
    if (const auto* bytes = std::get_if<disk::Bytes>(&value)) return bytes->capacity();
    if (const auto* text = std::get_if<std::string>(&value)) return text->capacity();
    if (const auto* number = std::get_if<disk::Decimal>(&value)) return number->unscaled.capacity();
    return 0;
}

class Meter
{
public:
    Meter(Limits limits, const Instrumentation& instrumentation)
        : limits_(limits), deadline_(Clock::now() + limits.time), instrumentation_(instrumentation)
    {
        if (instrumentation_.trace)
        {
            reserve(limits_.depth * sizeof(TraceFrame), {});
            frames_.reserve(limits_.depth);
        }
        if (instrumentation_.profile)
        {
            reserve(limits_.depth * sizeof(Timing), {});
            timing_.reserve(limits_.depth);
        }
    }

    void poll(sql::Source source, const sql::WorkCheck& check)
    {
        if (check) check();
        if (Clock::now() >= deadline_) limit(source, "PL/SQL time budget exceeded");
    }

    void step(sql::Source source, const sql::WorkCheck& check)
    {
        poll(source, check);
        if (steps_ == limits_.instructions) limit(source, "PL/SQL instruction budget exceeded");
        ++steps_;
    }

    void reserve(size_t bytes, sql::Source source)
    {
        if (bytes > limits_.memory - live_) limit(source, "PL/SQL memory budget exceeded");
        live_ += bytes;
    }

    void release(size_t bytes) noexcept { live_ -= bytes; }

    void enter(const Program& program)
    {
        if (depth_ == limits_.depth) limit(program.name.source, "PL/SQL call depth exceeded");
        if (instrumentation_.trace) frames_.push_back({&program, program.name.source, {}});
        if (instrumentation_.profile) timing_.push_back({Clock::now(), {}, {}, 0});
        ++depth_;
    }

    void leave(const Program& program) noexcept
    {
        --depth_;
        if (!frames_.empty()) frames_.pop_back();
        if (timing_.empty()) return;
        const auto frame = timing_.back();
        timing_.pop_back();
        const auto inclusive = Clock::now() - frame.start;
        if (!timing_.empty()) timing_.back().children += inclusive;
        const auto microseconds = [](Clock::duration value) -> uint64_t {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                (std::max)(value, Clock::duration::zero())).count());
        };
        try
        {
            instrumentation_.profile({program.name, program.revision, microseconds(inclusive),
                microseconds(inclusive - frame.children), microseconds(frame.sql), frame.instructions});
        }
        catch (...) { /* A profiling consumer must not replace the routine's outcome. */ }
    }

    void bind_frame(std::span<const disk::Value> values)
    {
        if (!frames_.empty()) frames_.back().values = values;
    }

    void unbind_frame()
    {
        if (!frames_.empty()) frames_.back().values = {};
    }

    void before(sql::Source source, const sql::WorkCheck& check)
    {
        if (!timing_.empty()) ++timing_.back().instructions;
        trace(source, false, check);
    }

    void exception(const RuntimeError& failure, const sql::WorkCheck& check)
    {
        if (instrumentation_.trace && !tracing_ && failure.mark_traced()) trace(failure.source(), true, check);
    }

    struct SqlTiming
    {
        size_t depth{};
        Clock::time_point start;
        Clock::duration children{};
    };

    SqlTiming started_sql(bool hosted_sql)
    {
        if (!hosted_sql || timing_.empty()) return {};
        return {depth_, Clock::now(), timing_.back().children};
    }

    void finished_sql(const SqlTiming& host) noexcept
    {
        if (!host.depth) return;
        auto& frame = timing_[host.depth - 1];
        const auto elapsed = Clock::now() - host.start - (frame.children - host.children);
        frame.sql += (std::max)(elapsed, Clock::duration::zero());
    }

    void lexical_depth(size_t depth, sql::Source source)
    {
        if (depth >= limits_.depth) limit(source, "PL/SQL lexical depth exceeded");
    }

private:
    struct Timing
    {
        Clock::time_point start;
        Clock::duration children;
        Clock::duration sql;
        uint64_t instructions;
    };

    void trace(sql::Source source, bool exception, const sql::WorkCheck& check)
    {
        if (frames_.empty() || tracing_) return;
        frames_.back().source = source;
        tracing_ = true;
        struct Boundary { bool& entered; ~Boundary() { entered = false; } } boundary{tracing_};
        instrumentation_.trace(frames_, exception);
        poll(source, check);
    }

    Limits limits_;
    Clock::time_point deadline_;
    const Instrumentation& instrumentation_;
    size_t live_{};
    size_t steps_{};
    size_t depth_{};
    std::vector<TraceFrame> frames_;
    std::vector<Timing> timing_;
    bool tracing_{};
};

class HostedSql
{
public:
    HostedSql(Meter& meter, bool hosted_sql) : meter_(meter), timing_(meter.started_sql(hosted_sql)) {}
    ~HostedSql() { meter_.finished_sql(timing_); }
private:
    Meter& meter_;
    Meter::SqlTiming timing_;
};

class Reservation
{
public:
    explicit Reservation(Meter& meter) : meter_(meter) {}
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    ~Reservation() { meter_.release(bytes_); }

    void add(size_t bytes, sql::Source source)
    {
        meter_.reserve(bytes, source);
        bytes_ += bytes;
    }

    void remove(size_t bytes) noexcept
    {
        bytes_ -= bytes;
        meter_.release(bytes);
    }

private:
    Meter& meter_;
    size_t bytes_{};
};

thread_local Meter* active_meter;

class Activation
{
public:
    Activation(Meter& meter, const Program& program) : meter_(meter), program_(program), previous_(active_meter)
    {
        meter_.enter(program_);
        active_meter = &meter;
    }
    ~Activation()
    {
        active_meter = previous_;
        meter_.leave(program_);
    }

private:
    Meter& meter_;
    const Program& program_;
    Meter* previous_;
};

int64_t integer(const disk::Value& value, sql::Source source)
{
    const auto* number = std::get_if<int64_t>(&value);
    if (!number) throw RuntimeError("value_error", source, "Expected a non-NULL BIGINT", sql::Code::type);
    return *number;
}

uint64_t magnitude(int64_t value)
{
    const auto bits = static_cast<uint64_t>(value);
    return value < 0 ? uint64_t{0} - bits : bits;
}

int64_t arithmetic(Op op, int64_t left, int64_t right, sql::Source source)
{
    constexpr auto minimum = (std::numeric_limits<int64_t>::min)();
    constexpr auto maximum = (std::numeric_limits<int64_t>::max)();
    switch (op)
    {
    case Op::add:
        if ((right > 0 && left > maximum - right) || (right < 0 && left < minimum - right))
            overflow(source);
        return left + right;
    case Op::subtract:
        if ((right > 0 && left < minimum + right) || (right < 0 && left > maximum + right))
            overflow(source);
        return left - right;
    case Op::multiply:
    {
        const auto a = magnitude(left);
        const auto b = magnitude(right);
        const bool negative = (left < 0) != (right < 0);
        const auto cap = uint64_t{maximum} + static_cast<uint64_t>(negative);
        if (b && a > cap / b) overflow(source);
        const auto product = a * b;
        if (product > uint64_t{maximum}) return minimum;
        const auto result = static_cast<int64_t>(product);
        return negative ? -result : result;
    }
    case Op::divide:
        if (!right) throw RuntimeError("zero_divide", source, "Division by zero", sql::Code::range);
        if (left == minimum && right == -1) overflow(source);
        return left / right;
    default: broken(source);
    }
}

bool condition(const disk::Value& result, sql::Source source)
{
    return sql::truth(result, source) == sql::Truth::true_;
}

enum class Flow { next, exit_loop, continue_loop, returned };

class Interpreter
{
public:
    Interpreter(const Program& program, Meter& meter, const SqlCallback& host, const sql::WorkCheck& check,
        const HostAction& action, const ExpressionObserver& observer)
        : program_(program), meter_(meter), host_(host), check_(check), action_(action), observer_(observer),
          frame_claim_(meter)
    {
        frame_claim_.add(program.slots.size() * sizeof(disk::Value), program.name.source);
        frame_.resize(program.slots.size());
        meter_.bind_frame(frame_);
    }

    ~Interpreter() { meter_.unbind_frame(); }

    Result run(std::span<const disk::Value> arguments)
    {
        const auto slots = std::span(program_.slots);
        if (program_.parameters > slots.size()) broken(program_.name.source);
        const auto parameters = slots.first(program_.parameters);
        const auto supplied = static_cast<size_t>(std::ranges::count_if(parameters,
            [](const Slot& slot) { return slot.mode != Mode::out; }));
        const bool full = arguments.size() == parameters.size();
        if (!full && arguments.size() != supplied)
            throw sql::Error(sql::Code::type, program_.name.source, "Routine argument count does not match declaration");

        size_t position{};
        for (size_t index = 0; index < parameters.size(); ++index)
        {
            const auto& parameter = parameters[index];
            if (parameter.mode == Mode::local) broken(parameter.source);
            if (parameter.mode == Mode::out)
            {
                if (full && !is_null(arguments[position++]))
                    throw sql::Error(sql::Code::type, parameter.source, "OUT argument position requires NULL");
                continue;
            }
            assign(index, arguments[position++], parameter.type, parameter.source);
        }

        const auto outcome = guarded(program_.body, program_.handlers, 0);
        if (outcome == Flow::exit_loop || outcome == Flow::continue_loop) broken(program_.name.source);
        if (program_.returns && !returned_)
            throw RuntimeError("program_error", program_.name.source, "Function reached the end without returning a value");

        Result result{std::move(returned_), {}};
        Reservation output(meter_);
        for (size_t index = 0; index < parameters.size(); ++index)
        {
            const auto& parameter = parameters[index];
            if (parameter.mode == Mode::in) continue;
            output.add(sizeof(OutValue) * 2 + parameter.name.size(), parameter.source);
            result.out.push_back({parameter.name, parameter.type, std::move(frame_[index])});
        }
        return result;
    }

private:
    const TypedExpr& required(const std::optional<TypedExpr>& expression, sql::Source source)
    {
        if (!expression) broken(source);
        return *expression;
    }

    void assign(size_t index, const disk::Value& value, sql::Type type, sql::Source source)
    {
        if (index >= frame_.size()) broken(source);
        auto next = sql::coerce(value, type, program_.slots[index].type, source);
        frame_claim_.add(value_bytes(next), source);
        frame_claim_.remove(value_bytes(frame_[index]));
        frame_[index] = std::move(next);
    }

    disk::Value expression(const TypedExpr& expr, size_t depth = 0)
    {
        if (!observer_) return evaluate(expr, depth);

        ExpressionSample sample{++expression_count_, expression_parent_, expr.op, expr.source,
            ExpressionState::evaluated, {}, 0};
        struct RestoreParent
        {
            uint32_t& current;
            uint32_t parent;
            ~RestoreParent() { current = parent; }
        } restore{expression_parent_, expression_parent_};
        expression_parent_ = sample.id;
        const auto started = Clock::now();
        const auto elapsed = [&] {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
        };
        disk::Value output;
        try
        {
            output = evaluate(expr, depth);
            const bool outcome = !sample.parent_id || !expr.args.empty() || expr.op == Op::exists ||
                expr.op == Op::subquery || expr.op == Op::routine_call;
            if (outcome && (expr.type.kind == sql::Kind::boolean || expr.type.kind == sql::Kind::null))
                sample.truth = sql::truth(output, expr.source);
        }
        catch (...)
        {
            sample.state = ExpressionState::failed;
            sample.elapsed_ns = elapsed();
            observer_(sample);
            throw;
        }
        sample.elapsed_ns = elapsed();
        observer_(sample);
        return output;
    }

    void skipped(const TypedExpr& expr, uint32_t parent)
    {
        if (!observer_) return;
        const ExpressionSample sample{++expression_count_, parent, expr.op, expr.source, ExpressionState::skipped, {}, 0};
        for (const auto& arg : expr.args) skipped(arg, sample.id);
        observer_(sample);
    }

    disk::Value evaluate(const TypedExpr& expr, size_t depth)
    {
        meter_.lexical_depth(depth, expr.source);
        meter_.step(expr.source, check_);
        const auto operand = [&](size_t index) {
            if (index >= expr.args.size()) broken(expr.source);
            return expression(expr.args[index], depth + 1);
        };
        switch (expr.op)
        {
        case Op::name:
            if (expr.slot >= frame_.size()) broken(expr.source);
            return frame_[expr.slot];
        case Op::null: case Op::boolean: case Op::integer: case Op::decimal:
        case Op::text: case Op::date: case Op::time: case Op::timestamp_tz:
            return expr.literal;
        case Op::routine_call: case Op::subquery: case Op::exists:
        {
            if (!host_) throw sql::Error(sql::Code::unsupported, expr.source, "Routine SQL host is unavailable");
            std::vector<disk::Value> arguments;
            Reservation temporary(meter_);
            temporary.add(expr.args.size() * sizeof(disk::Value), expr.source);
            arguments.reserve(expr.args.size());
            for (size_t index = 0; index < expr.args.size(); ++index)
            {
                auto value = operand(index);
                temporary.add(value_bytes(value), expr.source);
                arguments.push_back(std::move(value));
            }
            auto output = [&] {
                const HostedSql timed(meter_, expr.op != Op::routine_call);
                return host_(expr, frame_, arguments);
            }();
            meter_.step(expr.source, check_);
            temporary.add(value_bytes(output), expr.source);
            return sql::coerce(output, expr.type, expr.type, expr.source);
        }
        case Op::logical_not: return sql::boolean_value(sql::logical_not(sql::truth(operand(0), expr.source)));
        case Op::negate: return sql::negate_numeric(operand(0), expr.type, expr.source);
        case Op::is_null: return int64_t{is_null(operand(0))};
        case Op::is_not_null: return int64_t{!is_null(operand(0))};
        case Op::and_:
        {
            const auto left = sql::truth(operand(0), expr.source);
            if (left == sql::Truth::false_)
            {
                if (observer_) skipped(expr.args.at(1), expression_parent_);
                return sql::boolean_value(left);
            }
            return sql::boolean_value(sql::logical_and(left, sql::truth(operand(1), expr.source)));
        }
        case Op::or_:
        {
            const auto left = sql::truth(operand(0), expr.source);
            if (left == sql::Truth::true_)
            {
                if (observer_) skipped(expr.args.at(1), expression_parent_);
                return sql::boolean_value(left);
            }
            return sql::boolean_value(sql::logical_or(left, sql::truth(operand(1), expr.source)));
        }
        case Op::add: case Op::subtract: case Op::multiply: case Op::divide:
        {
            const auto left = operand(0);
            const auto right = operand(1);
            if (is_null(left) || is_null(right)) return {};
            return arithmetic(expr.op, integer(left, expr.source), integer(right, expr.source), expr.source);
        }
        case Op::eq: case Op::ne: case Op::lt: case Op::le: case Op::gt: case Op::ge:
        {
            Reservation temporary(meter_);
            const auto left = operand(0);
            temporary.add(value_bytes(left), expr.source);
            const auto right = operand(1);
            temporary.add(value_bytes(right), expr.source);
            if (is_null(left) || is_null(right)) return {};
            const auto order = sql::compare_non_null(left, expr.args[0].type, right, expr.args[1].type, expr.source);
            switch (expr.op)
            {
            case Op::eq: return int64_t{order == 0};
            case Op::ne: return int64_t{order != 0};
            case Op::lt: return int64_t{order < 0};
            case Op::le: return int64_t{order <= 0};
            case Op::gt: return int64_t{order > 0};
            default: return int64_t{order >= 0};
            }
        }
        default: broken(expr.source);
        }
    }

    bool predicate(const TypedExpr& expr) { return condition(expression(expr), expr.source); }

    void act(const Instruction& instruction)
    {
        using Kind = Node::Kind;
        if (!action_) throw sql::Error(sql::Code::unsupported, instruction.source, "Routine statement host is unavailable");
        auto values = [&] {
            using Kind = Node::Kind;
            const bool hosted_sql = instruction.kind == Kind::select_into || instruction.kind == Kind::open_cursor ||
                instruction.kind == Kind::sql_mutation;
            const HostedSql timed(meter_, hosted_sql);
            return action_(instruction, frame_);
        }();
        meter_.step(instruction.source, check_);
        if (values.empty() && instruction.kind == Kind::fetch_cursor) return;
        if (values.empty() && instruction.kind == Kind::select_into)
            throw RuntimeError("no_data_found", instruction.source, "SELECT INTO returned no rows");
        if (values.size() != instruction.targets.size())
            throw RuntimeError("value_error", instruction.source, "Output column count does not match INTO variables",
                sql::Code::type);

        Reservation temporary(meter_);
        temporary.add(values.capacity() * sizeof(disk::Value), instruction.source);
        for (const auto& value : values) temporary.add(value_bytes(value), instruction.source);
        size_t assigned_bytes{};
        for (size_t index = 0; index < values.size(); ++index)
        {
            const auto target = instruction.targets[index];
            if (target >= frame_.size()) broken(instruction.source);
            const auto type = program_.slots[target].type;
            auto converted = sql::coerce(values[index], type, type, instruction.source);
            temporary.add(value_bytes(converted), instruction.source);
            temporary.remove(value_bytes(values[index]));
            assigned_bytes += value_bytes(converted);
            values[index] = std::move(converted);
        }

        temporary.remove(assigned_bytes);
        frame_claim_.add(assigned_bytes, instruction.source);
        for (size_t index = 0; index < values.size(); ++index)
        {
            auto& target = frame_[instruction.targets[index]];
            frame_claim_.remove(value_bytes(target));
            target = std::move(values[index]);
        }
    }

    Flow instructions(std::span<const Instruction> body, size_t depth)
    {
        for (const auto& instruction : body)
        {
            meter_.lexical_depth(depth, instruction.source);
            meter_.step(instruction.source, check_);
            meter_.before(instruction.source, check_);
            const auto outcome = execute(instruction, depth);
            if (outcome != Flow::next) return outcome;
        }
        return Flow::next;
    }

    Flow guarded(std::span<const Instruction> body, std::span<const Instruction> handlers, size_t depth)
    {
        try { return instructions(body, depth); }
        catch (const RuntimeError& error)
        {
            meter_.exception(error, check_);
            if (handlers.empty()) throw;
            return handle(handlers, depth, std::current_exception());
        }
        catch (const sql::Error& error)
        {
            if (error.code() != sql::Code::type && error.code() != sql::Code::range &&
                error.code() != sql::Code::constraint)
                throw;
            const auto failure = RuntimeError("value_error", error.source(), error.what(), error.code());
            meter_.exception(failure, check_);
            if (handlers.empty()) throw failure;
            return handle(handlers, depth, std::make_exception_ptr(failure));
        }
    }

    Flow handle(std::span<const Instruction> handler, size_t depth, std::exception_ptr current)
    {
        struct Restore
        {
            std::exception_ptr& context;
            std::exception_ptr previous;
            ~Restore() { context = std::move(previous); }
        } restore{exception_, std::exchange(exception_, std::move(current))};
        return instructions(handler, depth);
    }

    Flow loop(const Instruction& instruction, size_t depth)
    {
        using Kind = Node::Kind;
        const auto body = [&] { return instructions(instruction.body, depth + 1); };
        if (instruction.kind == Kind::for_)
        {
            const auto lower = integer(expression(required(instruction.first, instruction.source)), instruction.source);
            const auto upper = integer(expression(required(instruction.second, instruction.source)), instruction.source);
            if (lower > upper) return Flow::next;
            for (auto item = lower;; ++item)
            {
                meter_.step(instruction.source, check_);
                assign(instruction.slot, item, {sql::Kind::integer}, instruction.source);
                const auto flow = body();
                if (flow == Flow::returned) return flow;
                if (flow == Flow::exit_loop || item == upper) break;
            }
            return Flow::next;
        }

        for (;;)
        {
            meter_.step(instruction.source, check_);
            if (instruction.kind == Kind::while_ && !predicate(required(instruction.first, instruction.source))) break;
            const auto flow = body();
            if (flow == Flow::returned) return flow;
            if (flow == Flow::exit_loop) break;
        }
        return Flow::next;
    }

    Flow execute(const Instruction& instruction, size_t depth)
    {
        using Kind = Node::Kind;
        switch (instruction.kind)
        {
        case Kind::assign:
        {
            const auto& value = required(instruction.first, instruction.source);
            assign(instruction.slot, expression(value), value.type, value.source);
            return Flow::next;
        }
        case Kind::if_:
            return instructions(predicate(required(instruction.first, instruction.source)) ? instruction.body :
                instruction.alternate, depth + 1);
        case Kind::block:
            return guarded(instruction.body, instruction.handlers, depth + 1);
        case Kind::for_: case Kind::while_: case Kind::loop_:
            return loop(instruction, depth);
        case Kind::exit_: case Kind::continue_:
            if (instruction.first && !predicate(*instruction.first)) return Flow::next;
            return instruction.kind == Kind::exit_ ? Flow::exit_loop : Flow::continue_loop;
        case Kind::return_:
            if (program_.returns)
            {
                const auto& expr = required(instruction.first, instruction.source);
                auto output = sql::coerce(expression(expr), expr.type, *program_.returns, expr.source);
                frame_claim_.add(value_bytes(output), expr.source);
                returned_ = std::move(output);
            }
            else if (instruction.first) broken(instruction.source);
            return Flow::returned;
        case Kind::raise:
            if (!instruction.exception.empty())
                throw RuntimeError(instruction.exception, instruction.source, "PL/SQL routine raised an exception");
            if (!exception_) broken(instruction.source);
            std::rethrow_exception(exception_);
        case Kind::select_into: case Kind::open_cursor: case Kind::fetch_cursor:
        case Kind::close_cursor: case Kind::emit_cursor: case Kind::sql_mutation:
            act(instruction);
            return Flow::next;
        default:
            broken(instruction.source);
        }
    }

    const Program& program_;
    Meter& meter_;
    const SqlCallback& host_;
    const sql::WorkCheck& check_;
    const HostAction& action_;
    const ExpressionObserver& observer_;
    Reservation frame_claim_;
    std::vector<disk::Value> frame_;
    std::optional<disk::Value> returned_;
    std::exception_ptr exception_;
    uint32_t expression_count_{};
    uint32_t expression_parent_{};
};
}

Result invoke(const Program& program, std::span<const disk::Value> arguments, const SqlCallback& host,
    const sql::WorkCheck& check, Limits limits, const HostAction& action, const Instrumentation& instrumentation)
{
    using namespace std::chrono_literals;
    if (!limits.instructions || limits.instructions > 10'000'000 || !limits.depth || limits.depth > 128 ||
        !limits.memory || limits.memory > 64 * 1024 * 1024 || limits.time <= 0ms || limits.time > 30s)
        limit(program.name.source, "Invalid PL/SQL execution limits");
    if (program.slots.size() > 1024) limit(program.name.source, "PL/SQL slot limit exceeded");
    std::optional<Meter> local;
    if (!active_meter) local.emplace(limits, instrumentation);
    auto& meter = active_meter ? *active_meter : *local;
    Activation activation(meter, program);
    meter.step(program.name.source, check);
    Interpreter runtime(program, meter, host, check, action, instrumentation.expression);
    try { return runtime.run(arguments); }
    catch (const RuntimeError& error)
    {
        meter.exception(error, check);
        throw;
    }
    catch (const sql::Error& error)
    {
        if (error.code() == sql::Code::type || error.code() == sql::Code::range || error.code() == sql::Code::constraint)
        {
            const auto failure = RuntimeError("value_error", error.source(), error.what(), error.code());
            meter.exception(failure, check);
            throw failure;
        }
        throw;
    }
}
}
