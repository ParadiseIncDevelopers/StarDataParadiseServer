#include "debugger.hpp"
#include "wire_values.hpp"

#include <algorithm>
#include <limits>

namespace sdp::sql
{
namespace
{
using wire::Json;
using W = wire::Code;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr size_t maximum_breakpoints = 64;
constexpr size_t maximum_stack_frames = 32;
constexpr size_t maximum_visible_locals = 128;
constexpr size_t locals_budget = 96 * 1024;

[[noreturn]] void reject(W code)
{
    throw wire::Error(code);
}

Name qualified_routine(Name name)
{
    if (name.parts.empty() || name.parts.size() > 3 ||
        std::ranges::any_of(name.parts, [](const auto& part) { return part.empty() || part.size() > 128; }))
        reject(W::invalid_argument);
    if (name.parts.size() == 1) name.parts.emplace(name.parts.begin(), default_schema);
    if (name.parts.size() == 2) name.parts.emplace(name.parts.begin(), default_database);
    return name;
}

size_t value_cost(const disk::Value& value)
{
    constexpr size_t overhead = 256;
    if (const auto* text = std::get_if<std::string>(&value))
        return text->size() > locals_budget / 6 ? locals_budget + 1 : overhead + text->size() * 6;
    if (const auto* bytes = std::get_if<disk::Bytes>(&value))
        return bytes->size() > locals_budget / 2 ? locals_budget + 1 : overhead + bytes->size() * 2;
    return overhead;
}

Json snapshot(std::span<const pl::TraceFrame> frames)
{
    if (frames.size() > maximum_stack_frames) reject(W::resource_limit);
    size_t remaining_bytes = locals_budget;
    size_t remaining_slots = maximum_visible_locals;
    auto stack = Json::array();
    for (size_t i = 0; i < frames.size(); ++i) stack.push_back(Json::object());

    // Reserve the most detail for the current frame while retaining the whole stack.
    for (size_t i = frames.size(); i > 0; --i)
    {
        const auto& active = frames[i - 1];
        if (!active.program || active.program->slots.size() != active.values.size()) reject(W::internal);
        const auto& program = *active.program;
        auto locals = Json::array();
        bool truncated = false;
        for (size_t index = 0; index < program.slots.size(); ++index)
        {
            const auto& slot = program.slots[index];
            const auto type = type_name(slot.type);
            const auto metadata_cost = slot.name.size() * 6 + type.size() + 96;
            if (!remaining_slots || metadata_cost > remaining_bytes)
            {
                truncated = true;
                break;
            }
            --remaining_slots;
            remaining_bytes -= metadata_cost;
            const auto cost = value_cost(active.values[index]);
            const auto available = cost <= remaining_bytes;
            Json local{{"name", slot.name}, {"type", type}, {"available", available}};
            if (available)
            {
                local["value"] = write_value(active.values[index], slot.type);
                remaining_bytes -= cost;
            }
            else
            {
                truncated = true;
            }
            locals.push_back(std::move(local));
        }
        stack[i - 1] = {{"routine", program.name.parts}, {"revision", program.revision}, {"depth", i},
            {"line", active.source.line}, {"column", active.source.column}, {"offset", active.source.offset},
            {"locals", std::move(locals)}, {"locals_truncated", truncated}};
    }
    return stack;
}

bool boolean_field(const Json& body, const char* key, bool fallback, bool required = false)
{
    const auto field = body.find(key);
    if (field == body.end() && !required) return fallback;
    if (field == body.end() || !field->is_boolean()) reject(W::invalid_argument);
    return field->get<bool>();
}
}

Debugger::Debugger(std::string id, Name routine, uint32_t revision, std::chrono::milliseconds pause_timeout)
    : id_(std::move(id)), routine_(qualified_routine(std::move(routine))), revision_(revision), pause_timeout_(pause_timeout)
{
    if (id_.empty() || id_.size() > 128 || !revision_ || pause_timeout_ <= 0ms || pause_timeout_ > 30s)
        reject(W::invalid_argument);
}

bool Debugger::target(const Name& routine) const noexcept
{
    const auto& parts = routine.parts;
    if (parts.empty() || parts.size() > 3 || parts.back() != routine_.parts[2]) return false;
    const auto database = parts.size() == 3 ? std::string_view(parts[0]) : default_database;
    const auto schema = parts.size() >= 2 ? std::string_view(parts[parts.size() - 2]) : default_schema;
    return database == routine_.parts[0] && schema == routine_.parts[1];
}

bool Debugger::matches_breakpoint(const pl::TraceFrame& leaf) const
{
    return target(leaf.program->name) && leaf.program->revision == revision_ &&
        leaf.source.line <= (std::numeric_limits<uint32_t>::max)() &&
        breakpoints_.contains(static_cast<uint32_t>(leaf.source.line));
}

bool Debugger::matches_step(size_t depth) const noexcept
{
    switch (step_)
    {
    case Step::into: return true;
    case Step::over: return depth <= step_depth_;
    case Step::out: return depth < step_depth_;
    default: return false;
    }
}

bool Debugger::terminal() const noexcept
{
    return state_ == State::finished || state_ == State::cancelled || state_ == State::timed_out;
}

void Debugger::stop(State state)
{
    if (terminal()) return;
    state_ = state;
    step_ = Step::none;
    reason_ = nullptr;
    stack_ = nullptr;
    changed_.notify_all();
}

void Debugger::cancel() noexcept
{
    const std::lock_guard lock(mutex_);
    stop(State::cancelled);
}

void Debugger::finish() noexcept
{
    const std::lock_guard lock(mutex_);
    stop(State::finished);
}

void Debugger::check_work(const WorkCheck& check)
{
    if (!check) return;
    try { check(); }
    catch (...)
    {
        cancel();
        throw;
    }
}

Json Debugger::status() const
{
    constexpr const char* states[]{"armed", "running", "paused", "finished", "cancelled", "timed_out"};
    Json result{{"debug_id", id_}, {"state", states[static_cast<size_t>(state_)]}, {"revision", revision_},
        {"routine", routine_.parts}, {"reason", reason_ ? Json(reason_) : Json(nullptr)},
        {"line", nullptr}, {"column", nullptr}, {"offset", nullptr}, {"depth", 0},
        {"stack", stack_.is_array() ? stack_ : Json::array()}};
    if (!stack_.empty())
    {
        const auto& frame = stack_.back();
        result["line"] = frame.at("line");
        result["column"] = frame.at("column");
        result["offset"] = frame.at("offset");
        result["depth"] = stack_.size();
    }
    return result;
}

Json Debugger::command(std::string_view op, const Json& body)
{
    const std::lock_guard lock(mutex_);
    if (op == "debug_status") return status();
    if (op == "debug_close")
    {
        stop(State::cancelled);
        return status();
    }
    if (terminal()) reject(W::invalid_state);
    if (op == "debug_breakpoint")
    {
        const auto revision = unsigned_field(body, "revision", (std::numeric_limits<uint32_t>::max)());
        if (revision != revision_) reject(W::invalid_state);
        const auto line = unsigned_field(body, "line", (std::numeric_limits<uint32_t>::max)());
        if (!line) reject(W::invalid_argument);
        if (boolean_field(body, "enabled", true))
        {
            if (breakpoints_.size() == maximum_breakpoints && !breakpoints_.contains(line))
                reject(W::resource_limit);
            breakpoints_.insert(line);
        }
        else
        {
            breakpoints_.erase(line);
        }
        return status();
    }
    if (op == "debug_options")
    {
        exception_stop_ = boolean_field(body, "exception_stop", false, true);
        return status();
    }
    if (op == "debug_watch")
    {
        if (state_ != State::paused) reject(W::invalid_state);
        const auto& name = wire::string_field(body, "name");
        if (name.empty() || name.size() > 256) reject(W::invalid_argument);
        const auto& frame = stack_.back();
        for (const auto& local : frame.at("locals"))
        {
            if (local.at("name").get_ref<const std::string&>() != name) continue;
            if (!local.at("available").get<bool>()) reject(W::resource_limit);
            return local;
        }
        if (frame.at("locals_truncated").get<bool>()) reject(W::resource_limit);
        reject(W::invalid_argument);
    }
    if (op != "debug_continue" && op != "debug_step") reject(W::unsupported);
    if (state_ != State::paused) reject(W::invalid_state);

    auto resume = Step::none;
    if (op == "debug_step")
    {
        const auto& mode = wire::string_field(body, "mode");
        if (mode == "into") resume = Step::into;
        else if (mode == "over") resume = Step::over;
        else if (mode == "out") resume = Step::out;
        else reject(W::invalid_argument);
    }
    step_depth_ = stack_.size();
    step_ = resume;
    state_ = State::running;
    reason_ = nullptr;
    stack_ = nullptr;
    changed_.notify_all();
    return status();
}

void Debugger::trace(std::span<const pl::TraceFrame> frames, bool exception, const WorkCheck& check)
{
    if (frames.empty() || !frames.front().program || !frames.back().program ||
        !target(frames.front().program->name)) return;
    check_work(check);
    std::unique_lock lock(mutex_);
    if (state_ == State::finished) return;
    if (state_ == State::cancelled) reject(W::cancelled);
    if (state_ == State::timed_out) reject(W::timeout);
    if (state_ == State::paused || frames.front().program->revision != revision_)
        reject(W::invalid_state);

    state_ = State::running;
    const auto pause_reason = [&]() -> const char* {
        if (exception) return exception_stop_ ? "exception" : nullptr;
        if (matches_breakpoint(frames.back())) return "breakpoint";
        return matches_step(frames.size()) ? "step" : nullptr;
    };
    if (!pause_reason()) return;

    lock.unlock();
    auto view = snapshot(frames);
    check_work(check);
    lock.lock();
    if (state_ == State::cancelled || state_ == State::finished) reject(W::cancelled);
    if (state_ == State::timed_out) reject(W::timeout);
    if (state_ == State::paused) reject(W::invalid_state);
    reason_ = pause_reason();
    if (!reason_) return;

    step_ = Step::none;
    stack_ = std::move(view);
    state_ = State::paused;
    const auto deadline = Clock::now() + pause_timeout_;
    while (state_ == State::paused)
    {
        const auto now = Clock::now();
        if (now >= deadline)
        {
            stop(State::timed_out);
            reject(W::timeout);
        }
        changed_.wait_until(lock, (std::min)(deadline, now + 25ms), [&] { return state_ != State::paused; });
        lock.unlock();
        check_work(check);
        lock.lock();
    }
    if (state_ == State::cancelled || state_ == State::finished) reject(W::cancelled);
    if (state_ == State::timed_out) reject(W::timeout);
}
}
