#include "session.hpp"
#include "advanced.hpp"
#include "routines.hpp"
#include "pl_runtime.hpp"
#include "definition.hpp"
#include "debugger.hpp"
#include "history_restore.hpp"
#include "wire_values.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <openssl/crypto.h>

namespace sdp::sql
{
namespace
{
using wire::Json;
using W = wire::Code;
using namespace std::chrono_literals;

constexpr uint32_t outcome_schema = 0x53444c31;
constexpr auto outcome_ttl = std::chrono::hours(24);
constexpr size_t maximum_prepared = 16;
constexpr uint32_t maximum_results = 1'000'000;
constexpr size_t query_memory = 4 * 1024 * 1024;
constexpr size_t spill_threshold = 64 * 1024;
constexpr size_t spill_quota = 768 * 1024;
constexpr size_t batch_budget = 128 * 1024;

[[noreturn]] void reject(W code) { throw wire::Error(code); }

const Json& optional_object(const Json& body, const char* key)
{
    static const auto empty = Json::object();
    if (!body.contains(key)) return empty;
    return wire::object_field(body, key);
}

const std::string& token_field(const Json& body, const char* key = "request_token")
{
    const auto& token = wire::string_field(body, key);
    if (token.size() != 32 || token.find_first_not_of("0123456789abcdef") != token.npos)
        reject(W::invalid_argument);
    return token;
}

int64_t utc_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

Json columns_for(std::span<const Projection> columns)
{
    auto result = Json::array();
    for (const auto& column : columns)
        result.push_back({{"name", column.column}, {"type", type_name(column.expression.type)}});
    return result;
}

W sql_code(Code code)
{
    switch (code)
    {
    case Code::unsupported: return W::unsupported;
    case Code::limit: return W::resource_limit;
    default: return W::invalid_argument;
    }
}

W disk_code(disk::Fault fault)
{
    switch (fault)
    {
    case disk::Fault::cancelled: return W::cancelled;
    case disk::Fault::busy: return W::timeout;
    case disk::Fault::limit: return W::resource_limit;
    case disk::Fault::invalid: return W::invalid_argument;
    case disk::Fault::missing: return W::invalid_state;
    case disk::Fault::uncertain: return W::connection_lost;
    default: return W::internal;
    }
}

uint64_t history_number(const Json& body, const char* key, bool required = false)
{
    if (!body.contains(key) && !required) return 0;
    const auto& value = wire::string_field(body, key);
    uint64_t out{};
    const auto* end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, out);
    if (value.empty() || value.front() == '0' || parsed.ec != std::errc{} || parsed.ptr != end)
        reject(W::invalid_argument);
    return out;
}

std::optional<int64_t> history_date(const Json& body, const char* key)
{
    if (!body.contains(key)) return {};
    const auto& value = body.at(key);
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() && value.get<uint64_t>() > uint64_t{INT64_MAX}))
        reject(W::invalid_argument);
    return value.get<int64_t>();
}

HistoryCommand history_request(std::string_view op, const Json& body)
{
    using K = HistoryCommand::Kind;
    static const std::map<std::string_view, K> operations = {
        {"history_log", K::log}, {"history_diff", K::diff}, {"history_read", K::read},
        {"history_verify", K::verify}, {"history_status", K::status}, {"history_retain", K::retain},
        {"history_purge", K::purge}, {"history_pin", K::pin}, {"history_unpin", K::unpin},
        {"history_restore", K::restore}, {"history_revert", K::revert}};
    const auto found = operations.find(op);
    if (found == operations.end()) reject(W::unsupported);
    HistoryCommand command{};
    command.kind = found->second;
    const auto fields = [&](std::initializer_list<std::string_view> names) {
        for (const auto& item : body.items())
            if (std::ranges::find(names, item.key()) == names.end()) reject(W::invalid_argument);
    };
    switch (command.kind)
    {
    case K::log:
        fields({"target", "row_id", "from", "to", "since_utc", "until_utc", "offset", "limit"});
        if (body.contains("target")) command.target = parse_history_target(wire::string_field(body, "target"));
        command.from = history_number(body, "from");
        command.to = history_number(body, "to");
        command.since_utc = history_date(body, "since_utc");
        command.until_utc = history_date(body, "until_utc");
        break;
    case K::diff:
        fields({"target", "row_id", "from", "to", "offset", "limit"});
        command.target = parse_history_target(wire::string_field(body, "target"));
        command.from = history_number(body, "from", true);
        command.to = history_number(body, "to", true);
        break;
    case K::read:
        fields({"target", "revision", "row_id", "offset", "limit"});
        command.target = parse_history_target(wire::string_field(body, "target"));
        command.revision = history_number(body, "revision", true);
        break;
    case K::retain:
        fields({"revision", "quota_bytes"});
        command.revision = history_number(body, "revision", true);
        command.quota_bytes = history_number(body, "quota_bytes");
        return command;
    case K::purge:
        fields({"revision"});
        command.revision = history_number(body, "revision", true);
        return command;
    case K::pin:
    case K::unpin:
        if (command.kind == K::pin)
        {
            fields({"pin_kind", "pin_name", "revision"});
            command.revision = history_number(body, "revision", true);
        }
        else fields({"pin_kind", "pin_name"});
        command.pin_kind = wire::string_field(body, "pin_kind");
        command.pin_name = wire::string_field(body, "pin_name");
        return command;
    case K::restore:
    case K::revert:
        if (body.contains("plan_id"))
        {
            fields({"plan_id", "request_token"});
            command.plan_id = token_field(body, "plan_id");
            command.request_token = token_field(body);
        }
        else
        {
            fields({"target", "revision", "row_id", "field"});
            command.target = parse_history_target(wire::string_field(body, "target"));
            command.revision = history_number(body, "revision", true);
            command.row_id = history_number(body, "row_id");
            if (body.contains("field"))
            {
                auto field = parse_history_target(wire::string_field(body, "field"));
                if (field.parts.size() != 1) reject(W::invalid_argument);
                command.field = std::move(field.parts.front());
            }
        }
        return command;
    default:
        fields({});
        return command;
    }
    command.row_id = history_number(body, "row_id");
    command.offset = unsigned_field(body, "offset", UINT32_MAX);
    command.limit = unsigned_field(body, "limit", 256, 32);
    if (!command.limit) reject(W::invalid_argument);
    return command;
}

void ensure_rows_in_frame(const Json& result)
{
    // The fixed wire envelope and correlation identifiers also consume space.
    if (result.dump().size() > wire::max_payload - 4096) reject(W::resource_limit);
}
}

class Session::Spill
{
public:
    explicit Spill(const std::filesystem::path& path)
        : path_(path), file_(std::make_unique<disk::File>(path_, disk::File::Mode::create_new))
    {
    }

    ~Spill()
    {
        file_.reset();
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    void append(const RowValues& row)
    {
        const auto bytes = disk::encode({1, row});
        std::array<disk::Byte, 4> length{};
        if (bytes.size() + length.size() > spill_quota - size_) reject(W::resource_limit);
        disk::put<uint32_t>(length, 0, static_cast<uint32_t>(bytes.size()));
        file_->write(size_, length);
        file_->write(size_ + length.size(), bytes);
        size_ += length.size() + bytes.size();
    }

    bool end(uint64_t offset) const noexcept { return offset == size_; }

    std::pair<RowValues, size_t> row(uint64_t offset)
    {
        std::array<disk::Byte, 4> length{};
        if (offset > size_ || length.size() > size_ - offset) reject(W::internal);
        file_->read(offset, length);
        const auto count = disk::load_le<uint32_t>(length, 0);
        if (!count || count > disk::row_limit || count > size_ - offset - length.size()) reject(W::internal);
        disk::Bytes bytes(count);
        file_->read(offset + length.size(), bytes);
        auto decoded = disk::decode(bytes);
        if (decoded.schema != 1) reject(W::internal);
        return {std::move(decoded.values), length.size() + count};
    }

private:
    std::filesystem::path path_;
    std::unique_ptr<disk::File> file_;
    uint64_t size_{};
};

Session::Session(disk::Engine& engine, disk::Owner owner, std::string actor, std::chrono::milliseconds timeout,
    std::function<int64_t()> outcome_clock, std::filesystem::path temporary, Security* security, Principal principal,
    bool test_instance)
    : engine_(engine), owner_(owner), actor_(std::move(actor)), timeout_((std::min)(timeout, 20'000ms)),
      outcome_clock_(outcome_clock ? std::move(outcome_clock) : utc_seconds), temporary_(std::move(temporary)),
      security_(security), principal_(std::move(principal)), test_instance_(test_instance)
{
}

Session::~Session() = default;

void Session::attach_debugger(std::shared_ptr<Debugger> debugger)
{
    debugger_.store(std::move(debugger));
}

bool Session::debugging() const noexcept
{
    return bool(debugger_.load());
}

std::string Session::next_id()
{
    if (!next_) reject(W::resource_limit);
    return std::to_string(next_++);
}

void Session::check_deadline() const
{
    if (cancellation_.stop_requested()) reject(W::cancelled);
    if (Clock::now() >= deadline_) reject(W::timeout);
}

std::unique_ptr<disk::Transaction> Session::begin()
{
    if (!security_) return engine_.begin(owner_, 0ms, cancellation_, timeout_);
    lock_wait_started_.store(Clock::now().time_since_epoch().count());
    struct ClearWait
    {
        std::atomic<Clock::duration::rep>& value;
        ~ClearWait() { value.store(0); }
    } clear{lock_wait_started_};
    return engine_.begin(owner_, (std::min)(timeout_, 1500ms), cancellation_, timeout_);
}

std::vector<Requirement> Session::authorize(Catalog& catalog, const Statement& statement)
{
    if (!security_ || principal_.administrator) return {};
    if (security_ddl(statement)) reject(W::permission_denied);
    try
    {
        auto required = access_requirements(catalog, statement);
        enforce(*security_, principal_, required);
        return required;
    }
    catch (const Error& error)
    {
        if (error.code() == Code::name) reject(W::permission_denied);
        throw;
    }
}

disk::Transaction& Session::use(std::unique_ptr<disk::Transaction>& local)
{
    if (transaction_) return *transaction_;
    local = begin();
    return *local;
}

bool Session::transaction_active() const noexcept
{
    return Clock::now().time_since_epoch().count() < transaction_expires_.load();
}

uint64_t Session::lock_wait_ms() const noexcept
{
    const auto started = lock_wait_started_.load();
    if (!started) return 0;
    const auto elapsed = Clock::now() - Clock::time_point{Clock::duration{started}};
    return static_cast<uint64_t>((std::max)(int64_t{0}, std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
}

void Session::close_transaction()
{
    result_.reset();
    savepoints_.clear();
    transaction_.reset();
    transaction_expires_.store(0);
}

std::optional<Session::Outcome> Session::outcome(disk::Transaction& tx, std::string_view token)
{
    std::optional<Outcome> found;
    tx.visit(owner_, [&](disk::RowId, const disk::Row& row) {
        check_deadline();
        if (row.values.size() != 4) throw disk::Error(disk::Fault::corrupt, "Invalid SQL outcome record");
        const auto* actor = std::get_if<std::string>(&row.values[0]);
        const auto* key = std::get_if<std::string>(&row.values[1]);
        const auto* lsn = std::get_if<int64_t>(&row.values[2]);
        const auto* expires = std::get_if<int64_t>(&row.values[3]);
        if (!actor || !key || !lsn || !expires || *lsn <= 0 || *expires <= 0)
            throw disk::Error(disk::Fault::corrupt, "Invalid SQL outcome record");
        if (*actor == actor_ && *key == token)
        {
            if (found) throw disk::Error(disk::Fault::corrupt, "Duplicate SQL outcome record");
            found = Outcome{static_cast<uint64_t>(*lsn), *expires};
        }
    }, outcome_schema);
    return found;
}

uint64_t Session::commit(disk::Transaction& tx, std::string_view token)
{
    check_deadline();
    const auto lsn = engine_.current().lsn + 1;
    if (lsn > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) reject(W::resource_limit);
    const auto now = outcome_clock_();
    const auto expires = now + std::chrono::duration_cast<std::chrono::seconds>(outcome_ttl).count();
    tx.insert(owner_, {outcome_schema, {actor_, std::string(token), static_cast<int64_t>(lsn), expires}});
    if (security_) HistoryJournal(engine_, tx, owner_, [this] { check_deadline(); }).seal(actor_, now);
    return tx.commit(owner_);
}

Json Session::prepare(const Json& body)
{
    if (prepared_.size() == maximum_prepared) reject(W::resource_limit);
    auto statement = parse(wire::string_field(body, "sql"));
    auto types = read_parameter_types(optional_object(body, "parameter_types"));
    if (result_) reject(W::invalid_state);
    if (statement.history)
    {
        if (!types.empty()) reject(W::invalid_argument);
    }
    else
    {
        std::unique_ptr<disk::Transaction> local;
        Catalog catalog(use(local), owner_, [this] { check_deadline(); });
        (void)authorize(catalog, statement);
        if (!security_ || !security_ddl(statement)) (void)sdp::sql::bind(catalog, statement, types);
    }
    const auto id = next_id();
    prepared_.emplace(id, Prepared{std::move(statement), std::move(types)});
    return {{"statement_id", id}};
}

Json Session::execute(const Json& body)
{
    if (result_) reject(W::invalid_state);
    const auto it = prepared_.find(wire::string_field(body, "statement_id"));
    if (it == prepared_.end()) reject(W::invalid_state);
    const auto limit = unsigned_field(body, "row_limit", maximum_results, maximum_results);
    if (!limit) reject(W::invalid_argument);
    auto parameters = read_parameters(optional_object(body, "parameters"), it->second.parameter_types);
    if (it->second.statement.history)
    {
        if (!parameters.empty() || body.contains("request_token")) reject(W::invalid_argument);
        return {{"affected_rows", 0}, {"history", history(*it->second.statement.history)}};
    }
    std::unique_ptr<disk::Transaction> local;
    auto& tx = use(local);
    Catalog catalog(tx, owner_, [this] { check_deadline(); });
    auto required = authorize(catalog, it->second.statement);
    if (security_ && security_ddl(it->second.statement))
    {
        if (!local || !parameters.empty()) reject(W::invalid_state);
        const auto& token = token_field(body);
        if (outcome(tx, token)) reject(W::invalid_state);
        security_->apply(tx, owner_, principal_, it->second.statement);
        auto snapshot = security_->snapshot(tx, owner_);
        const auto lsn = security_->commit_and_publish(std::move(snapshot), [&] { return commit(tx, token); });
        return {{"affected_rows", 0}, {"lsn", std::to_string(lsn)}};
    }
    auto plan = sdp::sql::bind(catalog, it->second.statement, it->second.parameter_types);
    const WorkCheck check = [this] { check_deadline(); };

    if (auto* select = std::get_if<SelectPlan>(&plan))
    {
        if (body.contains("request_token")) reject(W::invalid_argument);
        auto columns = columns_for(select->projection);
        const auto id = next_id();
        result_ = Result{id, Cursor(std::move(*select), std::move(parameters), limit), std::move(local), 0, std::move(required)};
        return {{"affected_rows", 0}, {"result_id", id}, {"columns", std::move(columns)}};
    }

    if (const auto* advanced = std::get_if<AdvancedSelection>(&plan))
    {
        if (body.contains("request_token")) reject(W::invalid_argument);
        auto materialized = materialize(*advanced, tx, parameters, limit);
        auto columns = columns_for(materialized.columns);
        const auto id = next_id();
        result_ = Result{id, std::move(materialized), {}, 0, std::move(required)};
        return {{"affected_rows", 0}, {"result_id", id}, {"columns", std::move(columns)}};
    }

    const InvokerCheck invoker = [this, &required](std::span<const Requirement> requirements) {
        if (!security_) return;
        enforce(*security_, principal_, requirements);
        for (const auto& item : requirements)
        {
            const auto same = [&](const Requirement& stored) { return stored.verb == item.verb && stored.target == item.target; };
            if (std::ranges::none_of(required, same))
            {
                if (required.size() == 1024) reject(W::resource_limit);
                required.push_back(item);
            }
        }
    };

    if (const auto* routine = std::get_if<std::shared_ptr<const RoutineSelection>>(&plan))
    {
        const std::string* token{};
        if (routine_writes(**routine) && local)
        {
            token = &token_field(body);
            if (outcome(tx, *token)) reject(W::invalid_state);
        }
        else if (body.contains("request_token")) reject(W::invalid_argument);

        auto debug = debugger_.exchange({});
        struct Finish
        {
            std::shared_ptr<Debugger> debug;
            ~Finish() { if (debug) debug->finish(); }
        } finish{debug};
        last_profile_ = Json::array();
        pl::Instrumentation instrumentation;
        if (debug)
            instrumentation.trace = [&](std::span<const pl::TraceFrame> frames, bool exception) {
                debug->trace(frames, exception, check);
            };
        instrumentation.profile = [&](const pl::ProfileSample& sample) {
            if (last_profile_.size() >= 128) return;
            last_profile_.push_back({{"routine", sample.routine.parts}, {"revision", sample.revision},
                {"inclusive_us", sample.inclusive_us}, {"exclusive_us", sample.exclusive_us},
                {"sql_us", sample.sql_us}, {"instructions", sample.instructions}});
        };
        RoutineResults call;
        try { call = run_routine(**routine, tx, owner_, parameters, invoker, check, &instrumentation); }
        catch (...)
        {
            if (debug) close_transaction();
            throw;
        }
        Json response{{"affected_rows", call.affected}};
        if (token) response["lsn"] = std::to_string(commit(tx, *token));
        if (call.sets.empty()) return response;
        const auto id = next_id();
        response["result_id"] = id;
        response["columns"] = columns_for(call.sets.front().columns);
        RoutineStream stream;
        for (auto& set : call.sets)
        {
            Materialized result;
            result.columns = std::move(set.columns);
            result.rows = std::move(set.rows);
            stream.sets.emplace_back(std::move(set.name), std::move(result));
        }
        result_ = Result{id, std::move(stream), {}, 0, std::move(required)};
        return response;
    }

    const std::string* token{};
    if (local)
    {
        token = &token_field(body);
        if (outcome(tx, *token)) reject(W::invalid_state);
    }
    else if (body.contains("request_token")) reject(W::invalid_argument);
    const auto affected = execute_write(tx, owner_, plan, parameters, check, invoker);
    Json response{{"affected_rows", affected}};
    if (token) response["lsn"] = std::to_string(commit(tx, *token));
    return response;
}

Session::Materialized Session::materialize(const AdvancedSelection& selected, disk::Transaction& tx,
    const ParameterValues& parameters, uint32_t limit)
{
    Materialized result;
    const auto check = [this] { check_deadline(); };
    if (selected.mode == QueryMode::execute)
    {
        result.columns = advanced_columns(*selected.plan);
        result.rows = run_advanced(*selected.plan, tx, owner_, parameters, check, query_memory, limit);
    }
    else
    {
        auto description = explain_advanced(*selected.plan);
        if (selected.mode == QueryMode::explain_analyze)
        {
            const auto start = Clock::now();
            const auto rows = run_advanced(*selected.plan, tx, owner_, parameters, check, query_memory, limit);
            description["actual_rows"] = rows.size();
            description["elapsed_us"] = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
        }
        BoundExpression type;
        type.type = {Kind::text};
        result.columns.push_back({"plan", std::move(type)});
        result.rows.push_back({description.dump()});
    }

    if (temporary_.empty()) return result;
    size_t bytes{};
    for (const auto& row : result.rows)
    {
        check_deadline();
        bytes += disk::encode({1, row}).size() + sizeof(uint32_t);
        if (bytes > spill_threshold) break;
    }
    if (bytes <= spill_threshold) return result;

    const auto stamp = Clock::now().time_since_epoch().count();
    const auto name = "sdp-query-" + std::to_string(owner_) + "-" + std::to_string(stamp) + "-" + next_id() + ".tmp";
    result.spill = std::make_unique<Spill>(temporary_ / name);
    for (const auto& row : result.rows)
    {
        check_deadline();
        result.spill->append(row);
    }
    result.rows.clear();
    return result;
}

ResultBatch Session::fetch_materialized(Materialized& result, uint32_t count)
{
    ResultBatch batch;
    size_t available = batch_budget;
    while (batch.rows.size() < count)
    {
        check_deadline();
        if (result.spill)
        {
            if (result.spill->end(result.file_offset)) break;
            auto [row, bytes] = result.spill->row(result.file_offset);
            if (bytes > available)
            {
                if (batch.rows.empty()) reject(W::resource_limit);
                break;
            }
            available -= bytes;
            result.file_offset += bytes;
            batch.rows.push_back(std::move(row));
        }
        else
        {
            if (result.offset == result.rows.size()) break;
            auto& row = result.rows[result.offset];
            const auto bytes = disk::encode({1, row}).size() + sizeof(uint32_t);
            if (bytes > available)
            {
                if (batch.rows.empty()) reject(W::resource_limit);
                break;
            }
            available -= bytes;
            ++result.offset;
            batch.rows.push_back(std::move(row));
        }
    }
    batch.done = result.spill ? result.spill->end(result.file_offset) : result.offset == result.rows.size();
    return batch;
}

Json Session::fetch(const Json& body)
{
    const auto& id = wire::string_field(body, "result_id");
    if (!result_ || id != result_->id) reject(W::invalid_state);
    const auto count = unsigned_field(body, "batch_rows", 1024, 128);
    if (!count) reject(W::invalid_argument);
    try
    {
        auto& active = *result_;
        if (security_) enforce(*security_, principal_, active.requirements);
        ResultBatch data;
        const std::vector<Projection>* columns;
        const std::string* set_name{};
        size_t set_index{};
        size_t row_count{};
        bool last_set = true;
        auto* stream = std::get_if<RoutineStream>(&active.cursor);
        if (stream)
        {
            set_index = stream->active;
            auto& [name, set] = stream->sets.at(set_index);
            columns = &set.columns;
            set_name = &name;
            row_count = set.rows.size();
            last_set = set_index + 1 == stream->sets.size();
            data = fetch_materialized(set, count);
        }
        else if (auto* cursor = std::get_if<Cursor>(&active.cursor))
        {
            auto& tx = active.read ? *active.read : *transaction_;
            data = cursor->fetch(tx, owner_, count, batch_budget, [this] { check_deadline(); });
            columns = &cursor->plan().projection;
        }
        else
        {
            auto& materialized = std::get<Materialized>(active.cursor);
            data = fetch_materialized(materialized, count);
            columns = &materialized.columns;
        }
        auto rows = Json::array();
        for (const auto& row : data.rows)
        {
            auto values = Json::array();
            for (size_t i = 0; i < row.size(); ++i)
                values.push_back(write_value(row[i], (*columns)[i].expression.type));
            rows.push_back(std::move(values));
        }
        Json batch{{"result_set", set_index}, {"batch_index", active.batch}, {"rows", std::move(rows)},
            {"end_of_set", data.done}, {"end_of_results", data.done && last_set}};
        if (!active.batch)
        {
            batch["columns"] = columns_for(*columns);
            if (set_name)
            {
                batch["set_name"] = *set_name;
                batch["row_count"] = row_count;
            }
        }
        ++active.batch;
        if (data.done)
        {
            if (last_set) result_.reset();
            else
            {
                ++stream->active;
                active.batch = 0;
            }
        }
        Json response{{"batch", std::move(batch)}};
        ensure_rows_in_frame(response);
        return response;
    }
    catch (...)
    {
        result_.reset();
        throw;
    }
}

Json Session::metadata()
{
    if (result_) reject(W::invalid_state);
    std::unique_ptr<disk::Transaction> local;
    Catalog catalog(use(local), owner_, [this] { check_deadline(); });
    auto objects = Json::array();
    const auto snapshot = catalog.list();
    for (const auto& object : snapshot)
    {
        check_deadline();
        if (security_ && !principal_.administrator)
        {
            bool visible = false;
            for (const auto verb : {"usage", "select", "insert", "update", "delete", "ddl", "execute", "history", "backup", "debug"})
            {
                const auto target = object_access(snapshot, object, verb);
                visible |= security_->allowed(principal_, target.verb, target.target, target.schema, target.database);
            }
            if (!visible) continue;
        }
        auto columns = Json::array();
        for (const auto& column : object.columns)
            columns.push_back({{"ordinal", column.ordinal}, {"name", column.name},
                {"type", type_name(column.type)}, {"nullable", column.nullable}});
        auto dependencies = Json::array();
        for (const auto id : object_dependencies(object)) dependencies.push_back(std::to_string(id));
        objects.push_back({{"id", std::to_string(object.id)}, {"kind", static_cast<int>(object.kind)},
            {"qualified_name", Catalog::qualified(object, snapshot).parts},
            {"schema_version", object.version}, {"columns", std::move(columns)}, {"depends_on", std::move(dependencies)}});
        if (const auto collation = catalog_index_collation(object))
            objects.back()["collation"] = {{"name", collation->name}, {"version", collation->version}, {"stale", collation->stale}};
    }
    if (security_ && objects.empty()) reject(W::permission_denied);
    Json response{{"objects", std::move(objects)}};
    ensure_rows_in_frame(response);
    return response;
}

Json Session::restore_history(const HistoryCommand& command)
{
    const auto check = [this] { check_deadline(); };
    auto tx = begin();
    HistoryJournal journal(engine_, *tx, owner_, check);
    if (command.plan_id.empty())
    {
        HistoryView view(journal, *tx, owner_, check);
        auto plan = prepare_history_plan(view, command, *security_, principal_, check);
        ensure_rows_in_frame(plan.preview);
        auto response = std::move(plan.preview);
        history_plan_ = std::make_unique<HistoryPlan>(std::move(plan));
        return response;
    }

    const auto mode = command.kind == HistoryCommand::Kind::restore ? "restore" : "revert";
    if (const auto saved = outcome(*tx, command.request_token))
    {
        if (saved->expires <= outcome_clock_()) throw HistoryError(W::invalid_state, "request_conflict");
        bool found = false;
        tx->scan(owner_, 0, [&](disk::RowId, const disk::Row& row) {
            if (row.values.size() != 5) throw disk::Error(disk::Fault::corrupt, "Invalid History receipt width");
            const auto* actor = std::get_if<std::string>(&row.values[0]);
            const auto* token = std::get_if<std::string>(&row.values[1]);
            const auto* plan = std::get_if<std::string>(&row.values[2]);
            const auto* kind = std::get_if<std::string>(&row.values[3]);
            const auto* lsn = std::get_if<int64_t>(&row.values[4]);
            if (!actor || !token || !plan || !kind || !lsn || *lsn <= 0)
                throw disk::Error(disk::Fault::corrupt, "Invalid History receipt fields");
            if (*actor != actor_ || *token != command.request_token) return true;
            if (found || static_cast<uint64_t>(*lsn) != saved->lsn)
                throw disk::Error(disk::Fault::corrupt, "History receipt and outcome disagree");
            if (*plan != command.plan_id || *kind != mode) throw HistoryError(W::invalid_state, "request_conflict");
            found = true;
            return true;
        }, history_receipt_schema, check);
        if (!found) throw HistoryError(W::invalid_state, "request_conflict");
        return {{"status", "committed"}, {"lsn", std::to_string(saved->lsn)}, {"replayed", true}};
    }

    if (!history_plan_ || history_plan_->id != command.plan_id || history_plan_->mode != command.kind)
        throw HistoryError(W::invalid_state, "plan_unknown");
    const auto state = journal.verify();
    if (state.head != history_plan_->head) throw HistoryError(W::invalid_state, "stale_preview");
    if (state.head >= uint64_t{INT64_MAX}) reject(W::resource_limit);
    apply_history_plan(*tx, owner_, *history_plan_, *security_, principal_, check);
    tx->insert(owner_, {history_receipt_schema, {actor_, command.request_token, command.plan_id, std::string(mode),
        static_cast<int64_t>(state.head + 1)}}, disk::ChangeKind::catalog);
    const auto lsn = commit(*tx, command.request_token);
    history_plan_.reset();
    return {{"status", "committed"}, {"lsn", std::to_string(lsn)}, {"replayed", false}};
}

Json Session::history(const HistoryCommand& command)
{
    if (!security_) reject(W::unsupported);
    if (result_ || transaction_) reject(W::invalid_state);
    using K = HistoryCommand::Kind;
    const bool query = command.kind == K::log || command.kind == K::diff || command.kind == K::read;
    const bool status = command.kind == K::status || command.kind == K::verify;
    const bool restoring = command.kind == K::restore || command.kind == K::revert;
    if (status && !security_->allowed(principal_, "history")) reject(W::permission_denied);
    if (!query && !status && !restoring && !principal_.administrator) reject(W::permission_denied);
    if (command.kind == K::log && command.target.parts.empty() && !security_->allowed(principal_, "history"))
        reject(W::permission_denied);

    try
    {
        if (restoring) return restore_history(command);
        const auto check = [this] { check_deadline(); };
        auto tx = begin();
        HistoryJournal journal(engine_, *tx, owner_, check);
        if (query)
        {
            HistoryView view(journal, *tx, owner_, check);
            auto response = query_history(view, command, *security_, principal_, check);
            ensure_rows_in_frame(response);
            return response;
        }
        if (status)
        {
            auto response = history_status_json(command.kind == K::verify ? journal.verify() : journal.state(),
                principal_.administrator);
            if (command.kind == K::verify) response["verified"] = true;
            return response;
        }
        if (command.kind == K::retain || command.kind == K::purge) journal.purge_before(command.revision);
        if (command.quota_bytes) journal.quota(command.quota_bytes);
        if (command.kind == K::pin) journal.pin({command.pin_name, command.pin_kind, command.revision});
        if (command.kind == K::unpin) journal.unpin(command.pin_kind, command.pin_name);
        auto state = journal.state();
        journal.seal(actor_, outcome_clock_());
        state.head = tx->commit(owner_);
        return history_status_json(state, true);
    }
    catch (const HistoryUnavailable&) { throw HistoryError(W::invalid_state, "revision_not_recoverable"); }
    catch (const HistoryPinned&) { throw HistoryError(W::invalid_state, "retention_pinned"); }
    catch (const disk::Error& error)
    {
        if (error.fault() == disk::Fault::corrupt) throw HistoryError(W::internal, "history_integrity");
        throw;
    }
}

Json Session::transact(std::string_view op, const Json& body)
{
    if (op == "tx_begin")
    {
        if (result_ || transaction_) reject(W::invalid_state);
        transaction_ = begin();
        transaction_expires_.store((Clock::now() + timeout_).time_since_epoch().count());
        return Json::object();
    }
    if (op == "tx_outcome")
    {
        if (result_) reject(W::invalid_state);
        const auto& token = token_field(body);
        std::unique_ptr<disk::Transaction> local;
        const auto saved = outcome(use(local), token);
        if (!saved || saved->expires <= outcome_clock_()) return {{"status", "unknown"}};
        return {{"status", "committed"}, {"lsn", std::to_string(saved->lsn)}};
    }
    if (!transaction_) reject(W::invalid_state);
    if (op == "tx_rollback")
    {
        close_transaction();
        return Json::object();
    }
    if (result_) reject(W::invalid_state);
    if (op == "tx_commit")
    {
        const auto& token = token_field(body);
        if (outcome(*transaction_, token)) reject(W::invalid_state);
        try
        {
            const auto lsn = commit(*transaction_, token);
            close_transaction();
            return {{"status", "committed"}, {"lsn", std::to_string(lsn)}};
        }
        catch (...)
        {
            close_transaction();
            throw;
        }
    }

    if (op != "tx_savepoint" && op != "tx_rollback_to" && op != "tx_release") reject(W::unsupported);
    const auto& name = wire::string_field(body, "name");
    if (name.empty() || name.size() > 64 ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != name.npos)
        reject(W::invalid_argument);
    const auto found = std::ranges::find(savepoints_, name, &NamedSavepoint::name);
    if (op == "tx_savepoint")
    {
        if (found != savepoints_.end()) reject(W::invalid_state);
        if (savepoints_.size() == 8) reject(W::resource_limit);
        savepoints_.reserve(8);
        NamedSavepoint saved{name, 0};
        saved.id = transaction_->savepoint(owner_);
        savepoints_.push_back(std::move(saved));
    }
    else
    {
        if (found == savepoints_.end()) reject(W::invalid_state);
        if (op == "tx_rollback_to")
        {
            transaction_->rollback_to(owner_, found->id);
            savepoints_.erase(found + 1, savepoints_.end());
        }
        else
        {
            transaction_->release_savepoint(owner_, found->id);
            savepoints_.erase(found, savepoints_.end());
        }
    }
    return Json::object();
}

Json Session::password(Json& body)
{
    struct Wipe
    {
        Json& fields;
        ~Wipe() {
            for (const auto key : {"old_password", "new_password"})
            {
                const auto found = fields.find(key);
                if (found != fields.end() && found->is_string())
                {
                    auto& text = found->get_ref<std::string&>();
                    OPENSSL_cleanse(text.data(), text.size());
                }
            }
        }
    } wipe{body};

    if (!security_) reject(W::unsupported);
    if (transaction_ || result_) reject(W::invalid_state);
    const auto& user = wire::string_field(body, "username");
    const auto current = body.contains("old_password")
        ? std::string_view{wire::string_field(body, "old_password")} : std::string_view{};
    const auto& new_value = wire::string_field(body, "new_password");
    const auto& token = token_field(body);
    auto tx = begin();
    if (outcome(*tx, token)) reject(W::invalid_state);
    security_->set_password(*tx, owner_, principal_, user, current, new_value);
    auto snapshot = security_->snapshot(*tx, owner_);
    const auto lsn = security_->commit_and_publish(std::move(snapshot), [&] { return commit(*tx, token); });
    return {{"status", "committed"}, {"lsn", std::to_string(lsn)}};
}

Json Session::test_routine(const Json& body)
{
    if (!test_instance_ || !security_ || !security_->allowed(principal_, "debug")) reject(W::permission_denied);
    if (transaction_ || result_) reject(W::invalid_state);
    const auto& id = wire::string_field(body, "statement_id");
    const auto found = prepared_.find(id);
    if (found == prepared_.end()) reject(W::invalid_state);

    const auto params = read_parameters(optional_object(body, "parameters"), found->second.parameter_types);
    const auto& expected = wire::object_field(body, "expected");
    const auto expected_sets = expected.value("sets", Json::array());
    const auto expected_exception = expected.value("exception", Json(nullptr));
    if (!expected_sets.is_array() || !(expected_exception.is_null() || expected_exception.is_string()))
        reject(W::invalid_argument);
    auto tx = begin();
    Catalog catalog(*tx, owner_, [this] { check_deadline(); });
    (void)authorize(catalog, found->second.statement);
    auto plan = sdp::sql::bind(catalog, found->second.statement, found->second.parameter_types);
    const auto* routine = std::get_if<std::shared_ptr<const RoutineSelection>>(&plan);
    if (!routine) reject(W::invalid_argument);

    Json sets = Json::array();
    Json exception;
    try
    {
        const auto invoked = run_routine(**routine, *tx, owner_, params,
            [this](std::span<const Requirement> required) { enforce(*security_, principal_, required); },
            [this] { check_deadline(); });
        for (const auto& set : invoked.sets)
        {
            auto data = Json::array();
            for (const auto& row : set.rows)
            {
                auto values = Json::array();
                for (size_t column = 0; column < row.size(); ++column)
                    values.push_back(write_value(row[column], set.columns[column].expression.type));
                data.push_back(std::move(values));
            }
            sets.push_back({{"name", set.name}, {"rows", std::move(data)}});
        }
    }
    catch (const pl::RuntimeError& error) { exception = error.name(); }
    tx->rollback(owner_);
    const bool passed = (exception == expected_exception) && (sets == expected_sets);
    Json response{{"passed", passed}, {"rolled_back", true}, {"sets", std::move(sets)}, {"exception", exception}};
    ensure_rows_in_frame(response);
    return response;
}

Json Session::request(std::string_view op, Json& body)
{
    if (op == "session_reset")
    {
        if (auto attached = debugger_.exchange({})) attached->cancel();
        close_transaction();
        history_plan_.reset();
        prepared_.clear();
        return Json::object();
    }
    if (op == "password_set") return password(body);
    if (op == "profile_last") return {{"samples", last_profile_}};
    if (op == "routine_test") return test_routine(body);
    if (op.starts_with("history_")) return history(history_request(op, body));
    if (security_ && (op.starts_with("history") || op.starts_with("backup") || op.starts_with("debug")))
    {
        const auto permission = op.substr(0, op.find_first_of("._"));
        if (!security_->allowed(principal_, permission)) reject(W::permission_denied);
    }
    if (op == "query.prepare") return prepare(body);
    if (op == "execute") return execute(body);
    if (op == "fetch") return fetch(body);
    if (op == "metadata") return metadata();
    if (op == "statement_close")
    {
        if (!prepared_.erase(wire::string_field(body, "statement_id"))) reject(W::invalid_state);
        return Json::object();
    }
    if (op == "result_cancel")
    {
        if (!result_ || result_->id != wire::string_field(body, "result_id")) reject(W::invalid_state);
        result_.reset();
        return Json::object();
    }
    if (op.starts_with("tx_")) return transact(op, body);
    reject(W::unsupported);
}

Json Session::dispatch(uint32_t id, std::string_view op, const Json& body, std::string_view correlation)
{
    auto copy = body;
    return dispatch(id, op, copy, correlation, {});
}

Json Session::dispatch(uint32_t id, std::string_view op, Json& body, std::string_view correlation,
    std::stop_token cancellation)
{
    deadline_ = Clock::now() + timeout_;
    cancellation_ = cancellation;
    try { return wire::success(id, request(op, body)); }
    catch (const HistoryError& error)
    {
        auto response = wire::failure(id, error.code(), correlation);
        response["error"]["history_reason"] = error.reason();
        return response;
    }
    catch (const wire::Error& error) { return wire::failure(id, error.code(), correlation); }
    catch (const pl::RuntimeError& error)
    {
        auto response = wire::failure(id, sql_code(error.code()), correlation);
        response["error"]["line"] = error.source().line;
        response["error"]["column"] = error.source().column;
        response["error"]["pl_exception"] = error.name();
        return response;
    }
    catch (const Error& error)
    {
        auto response = wire::failure(id, sql_code(error.code()), correlation);
        response["error"]["line"] = error.source().line;
        response["error"]["column"] = error.source().column;
        return response;
    }
    catch (const disk::Error& error)
    {
        if (error.fault() == disk::Fault::cancelled || error.fault() == disk::Fault::uncertain)
            close_transaction();
        auto response = wire::failure(id, disk_code(error.fault()), correlation);
        bool history_apply = (op == "history_restore" || op == "history_revert") && body.contains("plan_id");
        if (op == "execute" && body.contains("statement_id") && body.at("statement_id").is_string())
        {
            const auto statement = prepared_.find(body.at("statement_id").get_ref<const std::string&>());
            if (statement != prepared_.end() && statement->second.statement.history)
                history_apply = !statement->second.statement.history->plan_id.empty();
        }
        response["error"]["unknown_outcome"] = error.fault() == disk::Fault::uncertain &&
            (op == "tx_commit" || op == "password_set" || op == "history_retain" || op == "history_purge" ||
             op == "history_pin" || op == "history_unpin" || history_apply ||
             (op == "execute" && body.contains("request_token")));
        return response;
    }
    catch (const std::bad_alloc&) { return wire::failure(id, W::out_of_memory, correlation); }
    catch (const Json::exception&) { return wire::failure(id, W::invalid_argument, correlation); }
}
}
