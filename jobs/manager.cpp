#include "manager.hpp"
#include "schedule.hpp"
#include "worker.hpp"
#include "../sql/history_journal.hpp"
#include "../sql/wire_values.hpp"
#include "../ml/model.hpp"
#include "../ml/dataset.hpp"
#include "../ml/training.hpp"
#include "../backup/archive.hpp"
#include "../sql/definition.hpp"

#include <algorithm>
#include <array>
#include <openssl/rand.h>

namespace sdp::jobs
{
namespace
{
using wire::Json;
using W = wire::Code;
using namespace std::chrono_literals;

constexpr uint32_t job_schema = 0x53444a31;
constexpr uint32_t schedule_schema = 0x53444a32;
constexpr disk::Owner runner_owner = UINT64_MAX - 1;
constexpr size_t job_limit = 128;
constexpr size_t schedule_limit = 32;
constexpr size_t event_limit = 32;

[[noreturn]] void invalid() { throw wire::Error(W::invalid_argument); }
[[noreturn]] void denied() { throw wire::Error(W::permission_denied); }

int64_t utc()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string id()
{
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw wire::Error(W::internal);
    constexpr std::string_view hex = "0123456789abcdef";
    std::string value;
    value.reserve(32);
    for (const auto byte : bytes)
    {
        value += hex[byte >> 4];
        value += hex[byte & 15];
    }
    return value;
}

bool valid_id(std::string_view value)
{
    return value.size() == 32 && value.find_first_not_of("0123456789abcdef") == value.npos;
}

const std::string& identity(const Json& body, const char* field)
{
    const auto& value = wire::string_field(body, field);
    if (!valid_id(value)) invalid();
    return value;
}

void fields(const Json& body, std::initializer_list<std::string_view> allowed)
{
    if (!body.is_object()) invalid();
    for (const auto& field : body.items())
        if (std::ranges::find(allowed, field.key()) == allowed.end()) invalid();
}

Json task(const Json& body, bool admin, bool backup_enabled)
{
    const auto& kind = wire::string_field(body, "kind");
    if (kind == "maintenance")
    {
        if (!admin) denied();
        const auto& action = wire::string_field(body, "action");
        if (action != "integrity" && action != "statistics" && action != "index") invalid();
        if (body.contains("count") || body.contains("memory_mib") || body.contains("output_bytes") ||
            body.contains("timeout_ms") || body.contains("max_bytes")) invalid();
        return {{"payload", {{"kind", "maintenance"}, {"action", action}}},
            {"timeout_ms", 30'000}, {"memory_mib", 128}, {"output_bytes", 4096}};
    }
    if (body.contains("action")) invalid();
    if (kind == "backup")
    {
        if (!admin) denied();
        if (!backup_enabled) throw wire::Error(W::module_unavailable);
        constexpr auto maximum = 512u * 1024 * 1024;
        const auto budget = sql::unsigned_field(body, "max_bytes", maximum, maximum);
        if (!budget || body.contains("count") || body.contains("memory_mib") || body.contains("output_bytes") ||
            body.contains("timeout_ms")) invalid();
        return {{"payload", {{"kind", "backup"}, {"max_bytes", budget}}},
            {"timeout_ms", 60'000}, {"memory_mib", 128}, {"output_bytes", 4096}};
    }
    if (kind != "compute") throw wire::Error(W::module_unavailable);
    if (body.contains("max_bytes")) invalid();
    const auto count = sql::unsigned_field(body, "count", 1'000'000'000);
    const auto timeout = sql::unsigned_field(body, "timeout_ms", 60'000, 20'000);
    const auto memory = sql::unsigned_field(body, "memory_mib", 512, 128);
    const auto output = sql::unsigned_field(body, "output_bytes", 65'536, 4096);
    Json payload{{"kind", "compute"}, {"count", count}};
    if (!valid_worker_request(payload, {timeout, memory, output})) invalid();
    return {{"payload", std::move(payload)},
        {"timeout_ms", timeout}, {"memory_mib", memory}, {"output_bytes", output}};
}

struct Entity
{
    disk::RowId storage{};
    uint32_t schema{};
    std::string identity, actor, request;
    Json data;
};

Entity decode(disk::RowId id, const disk::Row& row)
{
    if (row.values.size() != 4 || (row.schema != job_schema && row.schema != schedule_schema))
        throw disk::Error(disk::Fault::corrupt, "Invalid job record schema");
    const auto* identity = std::get_if<std::string>(&row.values[0]);
    const auto* actor = std::get_if<std::string>(&row.values[1]);
    const auto* request = std::get_if<std::string>(&row.values[2]);
    const auto* body = std::get_if<std::string>(&row.values[3]);
    if (!identity || !valid_id(*identity) || !actor || !request || !body || body->size() > disk::row_limit / 2)
        throw disk::Error(disk::Fault::corrupt, "Invalid job record fields");
    auto data = wire::parse_payload(*body);
    if (!data.is_object() || data.value("format", 0) != 1)
        throw disk::Error(disk::Fault::corrupt, "Unsupported job record format");
    return {id, row.schema, *identity, *actor, *request, std::move(data)};
}

std::vector<Entity> list(disk::Transaction& tx, disk::Owner owner, uint32_t schema,
    const std::function<void()>& check = {})
{
    std::vector<Entity> result;
    tx.scan(owner, 0, [&](disk::RowId id, const disk::Row& row) {
        if (result.size() >= (schema == job_schema ? job_limit : schedule_limit))
            throw disk::Error(disk::Fault::corrupt, "Job catalog exceeds its limit");
        result.push_back(decode(id, row));
        return true;
    }, schema, check);
    return result;
}

void store(disk::Transaction& tx, disk::Owner owner, Entity& entity)
{
    disk::Row row{entity.schema, {entity.identity, entity.actor, entity.request, entity.data.dump()}};
    if (entity.storage) tx.update(owner, entity.storage, row, disk::ChangeKind::catalog);
    else entity.storage = tx.insert(owner, row, disk::ChangeKind::catalog);
}

void transition(Entity& job, const char* state)
{
    job.data["state"] = state;
    auto& events = job.data.at("events");
    if (events.size() >= event_limit) events.erase(events.begin());
    events.push_back({{"state", state}, {"at", utc()}});
}

bool terminal(const Entity& job)
{
    const auto& state = job.data.at("state");
    return state == "succeeded" || state == "failed" || state == "cancelled" || state == "paused";
}

Entity new_job(std::string actor, std::string request, const Json& config, std::string schedule = {},
    uint32_t retries = 0, uint32_t backoff = 1)
{
    Entity entity{0, job_schema, id(), std::move(actor), std::move(request),
        {{"format", 1}, {"task", config}, {"schedule", std::move(schedule)}, {"attempt", 0},
         {"cancel_requested", false}, {"not_before", utc()}, {"ended", 0}, {"events", Json::array()},
         {"result", nullptr}, {"reason", nullptr}, {"cpu_rate_limited", nullptr}, {"retries", retries},
         {"backoff_seconds", backoff}}};
    transition(entity, "queued");
    return entity;
}

Json external(const Entity& entity)
{
    if (entity.schema == schedule_schema)
    {
        const auto& data = entity.data;
        return {{"schedule_id", entity.identity}, {"enabled", data.at("enabled")}, {"next_utc", data.at("next_utc")},
            {"last_job_id", data.at("last_job_id")}, {"rule", data.at("rule")}, {"retries", data.at("retries")},
            {"backoff_seconds", data.at("backoff_seconds")}};
    }
    const auto& data = entity.data;
    return {{"job_id", entity.identity}, {"state", data.at("state")}, {"attempt", data.at("attempt")},
        {"cancel_requested", data.at("cancel_requested")}, {"result", data.at("result")}, {"reason", data.at("reason")},
        {"worker_pid", nullptr}, {"progress", nullptr}, {"events", data.at("events")},
        {"cpu_rate_limited", data.at("cpu_rate_limited")}};
}

void durable(disk::Engine& engine, disk::Transaction& tx)
{
    sql::HistoryJournal(engine, tx, runner_owner).seal("job-system", utc());
    tx.commit(runner_owner);
}

std::unique_ptr<disk::Transaction> acquire(disk::Engine& engine, std::stop_token stop)
{
    while (!stop.stop_requested())
    {
        try { return engine.begin(runner_owner, 75ms, stop, 5s, "job-system"); }
        catch (const disk::Error& error)
        {
            if (error.fault() != disk::Fault::busy) throw;
        }
    }
    throw disk::Error(disk::Fault::cancelled, "Job scheduler is stopping");
}

Entity& require_job(std::vector<Entity>& jobs, std::string_view id)
{
    const auto found = std::ranges::find(jobs, id, &Entity::identity);
    if (found == jobs.end()) throw disk::Error(disk::Fault::corrupt, "Running job row disappeared");
    return *found;
}

Json perform_maintenance(disk::Engine& engine, std::string_view action, std::stop_token stop)
{
    using sql::ObjectKind;
    const auto deadline = std::chrono::steady_clock::now() + 25s;
    const auto check = [&] {
        if (stop.stop_requested()) throw wire::Error(W::cancelled);
        if (std::chrono::steady_clock::now() >= deadline) throw wire::Error(W::timeout);
    };
    auto tx = engine.begin(runner_owner, 5s, stop, 30s, "maintenance");
    if (action == "integrity")
    {
        const auto stats = tx->inspect_committed(runner_owner);
        const auto history = sql::HistoryJournal(engine, *tx, runner_owner, check).verify();
        return {{"action", action}, {"rows", stats.rows}, {"history_head", history.head}, {"verified", true}};
    }
    sql::Catalog catalog(*tx, runner_owner, check);
    const auto objects = catalog.list();
    uint64_t count{}, rows{};
    for (const auto& object : objects)
    {
        check();
        if (action == "index" && object.kind == ObjectKind::index)
        {
            sql::Statement statement{};
            statement.action = sql::Action::reindex;
            statement.target = sql::Catalog::qualified(object, objects);
            if (!sql::execute_index_ddl(*tx, runner_owner, catalog, statement, check))
                throw wire::Error(W::invalid_state);
            ++count;
        }
        if (action == "statistics" && object.kind == ObjectKind::table)
        {
            const auto name = sql::Catalog::qualified(object, objects);
            sql::analyze_table(*tx, runner_owner, catalog, name, check);
            const auto table = catalog.resolve(ObjectKind::table, name);
            rows += Json::parse(table.definition).at("statistics").at("rows").get<uint64_t>();
            ++count;
        }
    }
    check();
    if (count) durable(engine, *tx);
    return {{"action", action}, {"objects", count}, {"rows", rows}, {"lsn", engine.current().lsn}};
}

int64_t advance(const Json& rule, int64_t due, int64_t now)
{
    if (rule.contains("every_seconds"))
    {
        const auto interval = rule.at("every_seconds").get<int64_t>();
        return schedule_after(rule, due + ((now - due) / interval) * interval);
    }
    return schedule_after(rule, now);
}
}

Manager::Manager(disk::Engine& engine, std::filesystem::path temporary, bool enable_mlp, backup::Archive* archives)
    : engine_(engine), temporary_(std::move(temporary)), mlp_enabled_(enable_mlp && ml::mlp_compiled()),
      archives_(archives), runner_([this](std::stop_token stop) { run(stop); })
{
}

Json Manager::inspect_backup(std::string_view operation, const Json& body, const std::function<void()>& check)
{
    if (!archives_) throw wire::Error(W::module_unavailable);
    return archives_->inspect(body, operation, check);
}

Json Manager::capabilities() const
{
    auto kinds = Json::array({"compute", "dataset_plsql", "supervised", "unsupervised", "maintenance"});
    if (mlp_enabled_) kinds.push_back("mlp_training");
    auto unavailable = Json::array({"sql_script", "recheck", "retrain"});
    if (archives_) kinds.push_back("backup");
    else unavailable.push_back("backup");
    return {{"kinds", std::move(kinds)},
        {"maximum_workers", 1}, {"queued_capacity", job_limit}, {"unavailable_kinds", std::move(unavailable)},
        {"schedule_capacity", schedule_limit}, {"scheduler_paused", paused_.load()}, {"process_isolation", "windows_job_object"},
        {"hard_memory_limit", true}, {"wall_time_limit", true}, {"output_limit", "bounded_transfer"},
        {"checkpoint_resume", mlp_enabled_}, {"schedule_types", Json::array({"interval", "daily"})}};
}

Json Manager::statistics(disk::Transaction& tx, disk::Owner owner) const
{
    Json states{{"queued", 0u}, {"starting", 0u}, {"running", 0u}, {"succeeded", 0u},
        {"failed", 0u}, {"cancelled", 0u}, {"paused", 0u}, {"checkpointing", 0u}};
    for (const auto& job : list(tx, owner, job_schema))
    {
        const auto& key = job.data.at("state").get_ref<const std::string&>();
        if (!states.contains(key)) throw disk::Error(disk::Fault::corrupt, "Unknown job state");
        states[key] = states[key].get<unsigned>() + 1;
    }
    return {{"states", std::move(states)}, {"schedules", list(tx, owner, schedule_schema).size()},
        {"scheduler_paused", paused_.load()}, {"job_capacity", job_limit}, {"schedule_capacity", schedule_limit}};
}

Reply Manager::submit_dataset_job(disk::Transaction& tx, disk::Owner owner, std::string_view actor,
    std::string_view token, std::string_view dataset, Json payload, const std::function<void()>& check, int64_t model)
{
    if (paused_.load()) throw wire::Error(W::module_unavailable);
    const Budget budget{20'000, 128, 65'536};
    if (payload.value("kind", std::string{}) == "mlp_training" && !mlp_enabled_) throw wire::Error(W::unsupported);
    if (!valid_id(token) || !valid_id(dataset) || !valid_worker_request(payload, budget)) invalid();
    Json config{{"payload", std::move(payload)}, {"timeout_ms", budget.timeout_ms},
        {"memory_mib", budget.memory_mib}, {"output_bytes", budget.output_bytes}, {"dataset", dataset}};
    if (model) config["model"] = model;
    const auto jobs = list(tx, owner, job_schema, check);
    for (const auto& item : jobs)
        if (item.actor == actor && item.request == token)
        {
            if (item.data.at("task") != config) throw wire::Error(W::invalid_state);
            return {external(item), false};
        }
    if (jobs.size() >= job_limit) throw wire::Error(W::resource_limit);
    auto entity = new_job(std::string(actor), std::string(token), config);
    store(tx, owner, entity);
    return {external(entity), true};
}

Manager::CheckpointData Manager::checkpoint(disk::Transaction& tx, disk::Owner owner, std::string_view actor,
    bool admin, int64_t model, std::string_view requested, const std::function<void()>& check) const
{
    if (!valid_id(requested)) invalid();
    const auto jobs = list(tx, owner, job_schema, check);
    const auto found = std::ranges::find(jobs, requested, &Entity::identity);
    if (found == jobs.end() || (!admin && found->actor != actor)) denied();
    const auto& job = found->data;
    const auto& config = job.at("task");
    if (job.at("state") != "paused" || config.value("model", int64_t{}) != model)
        throw wire::Error(W::invalid_state);
    const auto& stored = job.at("checkpoint");
    const auto& record = stored.at("record");
    const auto& checksum = stored.at("checksum");
    const auto& metadata = job.at("result");
    if (record.at("format") != 1 || record.at("runtime") != ml::mlp_runtime ||
        record.at("dataset_id") != config.at("dataset") || record.at("source_job").get_ref<const std::string&>() != requested ||
        record.at("model") != model || record.at("request_digest") != ml::dataset_fingerprint(config.at("payload")) ||
        checksum != ml::dataset_fingerprint(record) || metadata.at("checksum") != checksum)
        throw wire::Error(W::invalid_state);
    auto payload = config.at("payload");
    payload.erase("pause_after_epoch");
    payload["resume"] = record.at("state");
    return {metadata, std::move(payload)};
}

bool Manager::references_dataset(disk::Transaction& tx, disk::Owner owner, std::string_view dataset,
    const std::function<void()>& check) const
{
    for (const auto& item : list(tx, owner, job_schema, check))
        if (item.data.at("task").value("dataset", std::string{}) == dataset) return true;
    return false;
}

Reply Manager::request(disk::Transaction& tx, disk::Owner owner, std::string_view actor, bool admin,
    std::string_view operation, const Json& body, const std::function<void()>& check)
{
    const bool create_job = operation == "job_submit";
    const bool create_schedule = operation == "job_schedule_create";
    const bool schedule = create_schedule || operation == "job_schedule_status" || operation == "job_schedule_cancel";
    const bool watching = operation == "job_watch";
    const bool backup_op = operation == "backup_job_status" || operation == "backup_job_cancel";
    const bool cancel = operation == "job_cancel" || operation == "job_schedule_cancel" || operation == "backup_job_cancel";
    const bool submit = create_job || create_schedule;
    if (!submit && !cancel && !watching && !backup_op && operation != "job_status" && operation != "job_schedule_status")
        throw wire::Error(W::unsupported);
    const auto schema = schedule ? schedule_schema : job_schema;
    auto objects = list(tx, owner, schema, check);
    if (submit)
    {
        if (paused_.load()) throw wire::Error(W::module_unavailable);
        if (create_job) fields(body, {"request_token", "kind", "count", "timeout_ms", "memory_mib", "output_bytes", "max_bytes", "action"});
        else fields(body, {"request_token", "kind", "count", "timeout_ms", "memory_mib", "output_bytes", "max_bytes", "action",
            "rule", "misfire", "retries", "backoff_seconds"});
        const auto& token = identity(body, "request_token");
        const auto config = task(body, admin, archives_ != nullptr);
        auto existing = std::ranges::find_if(objects, [&](const Entity& item) {
            return item.actor == actor && item.request == token;
        });
        auto rule = Json::object();
        uint32_t retries{}, backoff{1};
        if (create_schedule)
        {
            rule = wire::object_field(body, "rule");
            if (body.contains("misfire"))
            {
                if (rule.contains("misfire")) invalid();
                rule["misfire"] = wire::string_field(body, "misfire");
            }
            (void)validate_schedule(rule);
            retries = sql::unsigned_field(body, "retries", 3);
            backoff = sql::unsigned_field(body, "backoff_seconds", 3600, 1);
            if (!backoff) invalid();
        }
        if (existing != objects.end())
        {
            if (existing->data.at("task") != config || (create_schedule &&
                (existing->data.at("rule") != rule || existing->data.at("retries") != retries ||
                 existing->data.at("backoff_seconds") != backoff))) throw wire::Error(W::invalid_state);
            return {external(*existing), false};
        }
        if (objects.size() >= (create_schedule ? schedule_limit : job_limit))
            throw wire::Error(W::resource_limit);
        auto entity = create_job ? new_job(std::string(actor), token, config) :
            Entity{0, schedule_schema, id(), std::string(actor), token,
                {{"format", 1}, {"task", config}, {"enabled", true}, {"rule", rule},
                 {"next_utc", schedule_after(rule, utc())}, {"last_job_id", nullptr}, {"retries", retries},
                 {"backoff_seconds", backoff}}};
        store(tx, owner, entity);
        return {external(entity), true};
    }

    const auto key = schedule ? "schedule_id" : "job_id";
    fields(body, {key});
    const auto& requested = identity(body, key);
    const auto found = std::ranges::find(objects, requested, &Entity::identity);
    if (found == objects.end() || (!admin && found->actor != actor)) denied();
    if (backup_op && (!admin || found->data.at("task").at("payload").at("kind") != "backup")) denied();
    bool changed = false;
    if (cancel && schedule && found->data.at("enabled") == true)
    {
        found->data["enabled"] = false;
        changed = true;
    }
    else if (cancel && !schedule && !terminal(*found) && found->data.at("cancel_requested") == false)
    {
        found->data["cancel_requested"] = true;
        if (found->data.at("state") == "queued")
        {
            transition(*found, "cancelled");
            found->data["reason"] = "cancelled";
            found->data["ended"] = utc();
        }
        changed = true;
    }
    if (changed) store(tx, owner, *found);
    auto reply = external(*found);
    if (!schedule && !terminal(*found))
    {
        const std::lock_guard lock(progress_mutex_);
        if (progress_.id == requested && progress_.pid)
        {
            reply["worker_pid"] = progress_.pid;
            if (progress_.total) reply["progress"] = {{"done", progress_.done}, {"total", progress_.total}};
        }
    }
    return {std::move(reply), changed, schedule ? std::string{} :
        found->data.at("task").value("dataset", std::string{})};
}

void Manager::recover(std::stop_token stop)
{
    auto tx = acquire(engine_, stop);
    bool changed = false;
    for (auto& job : list(*tx, runner_owner, job_schema))
    {
        const auto& state = job.data.at("state");
        if (state != "running" && state != "starting" && state != "checkpointing") continue;
        const auto cancel = job.data.at("cancel_requested").get<bool>();
        transition(job, cancel ? "cancelled" : "failed");
        job.data["reason"] = cancel ? "cancelled" : "interrupted";
        job.data["ended"] = utc();
        store(*tx, runner_owner, job);
        changed = true;
    }
    if (changed) durable(engine_, *tx);
}

void Manager::step(std::stop_token stop)
{
    Entity claimed;
    {
        auto tx = engine_.begin(runner_owner, 0ms, stop, 5s, "job-system");
        auto jobs = list(*tx, runner_owner, job_schema);
        const auto now = utc();
        bool changed = false;
        for (auto& schedule : list(*tx, runner_owner, schedule_schema))
        {
            const auto due = schedule.data.at("next_utc").get<int64_t>();
            if (!schedule.data.at("enabled").get<bool>() || due > now) continue;
            const auto& rule = schedule.data.at("rule");
            const auto missed = now - due > 2;
            const auto& last_id = schedule.data.at("last_job_id");
            bool overlaps = false;
            if (!last_id.is_null())
            {
                const auto last = std::ranges::find(jobs, last_id.get<std::string>(), &Entity::identity);
                overlaps = last != jobs.end() && (!terminal(*last) || last->data.at("ended").get<int64_t>() >= due);
            }
            if (!overlaps && jobs.size() < job_limit && (!missed || validate_schedule(rule) == Misfire::run_once))
            {
                auto job = new_job(schedule.actor, schedule.identity + "." + std::to_string(due),
                    schedule.data.at("task"), schedule.identity, schedule.data.at("retries").get<uint32_t>(),
                    schedule.data.at("backoff_seconds").get<uint32_t>());
                schedule.data["last_job_id"] = job.identity;
                store(*tx, runner_owner, job);
                jobs.push_back(std::move(job));
            }
            schedule.data["next_utc"] = advance(rule, due, now);
            store(*tx, runner_owner, schedule);
            changed = true;
        }
        const auto next = std::ranges::find_if(jobs, [&](const Entity& job) {
            return job.data.at("state") == "queued" && !job.data.at("cancel_requested").get<bool>() &&
                job.data.at("not_before").get<int64_t>() <= now;
        });
        if (next != jobs.end())
        {
            claimed = *next;
            claimed.data["attempt"] = claimed.data.at("attempt").get<uint32_t>() + 1;
            transition(claimed, "starting");
            store(*tx, runner_owner, claimed);
            changed = true;
        }
        if (changed) durable(engine_, *tx);
    }
    if (!claimed.storage) return;

    {
        auto tx = acquire(engine_, stop);
        auto jobs = list(*tx, runner_owner, job_schema);
        auto& running = require_job(jobs, claimed.identity);
        const auto cancel = running.data.at("cancel_requested").get<bool>();
        transition(running, cancel ? "cancelled" : "running");
        if (cancel)
        {
            running.data["reason"] = "cancelled";
            running.data["ended"] = utc();
        }
        store(*tx, runner_owner, running);
        durable(engine_, *tx);
        if (cancel) return;
    }

    const auto& config = claimed.data.at("task");
    const Budget budget{config.at("timeout_ms").get<uint32_t>(), config.at("memory_mib").get<uint32_t>(),
        config.at("output_bytes").get<uint32_t>()};
    const auto progress = [&](uint32_t pid, uint64_t done, uint64_t total) {
        {
            const std::lock_guard lock(progress_mutex_);
            progress_ = {claimed.identity, pid, done, total};
        }
        if (stop.stop_requested()) return false;
        try
        {
            auto tx = engine_.begin(runner_owner, 0ms, stop, 5s, "job-system");
            auto jobs = list(*tx, runner_owner, job_schema);
            return !require_job(jobs, claimed.identity).data.at("cancel_requested").get<bool>();
        }
        catch (const disk::Error& error)
        {
            if (error.fault() == disk::Fault::busy) return true;
            return false;
        }
    };
    const auto& payload = config.at("payload");
    const auto& kind = payload.at("kind").get_ref<const std::string&>();
    const auto backup = kind == "backup";
    const auto maintenance = kind == "maintenance";
    const auto result = [&]() -> WorkerResult {
        using Status = WorkerResult::Status;
        if (payload.value("kind", std::string{}) == "mlp_training" && !mlp_enabled_)
            return {Status::failed, "mlp_module_unavailable", nullptr};
        if (maintenance)
        {
            try { return {Status::succeeded, {}, perform_maintenance(engine_, payload.at("action").get<std::string>(), stop)}; }
            catch (const wire::Error& error)
            {
                if (error.code() == W::cancelled) return {Status::cancelled, "cancelled", nullptr};
                if (error.code() == W::timeout) return {Status::failed, "timeout", nullptr};
                return {Status::failed, "maintenance_failed", nullptr};
            }
            catch (const std::exception&) { return {Status::failed, "maintenance_failed", nullptr}; }
        }
        if (!backup) return run_worker(temporary_, payload, budget, progress);
        if (!archives_) return {Status::failed, "backup_unavailable", nullptr};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget.timeout_ms);
        const auto local_check = [&] {
            if (stop.stop_requested()) throw wire::Error(W::cancelled);
            if (std::chrono::steady_clock::now() >= deadline) throw wire::Error(W::timeout);
        };
        const auto post_capture = [&] {
            local_check();
            if (!progress(0, 1, 2)) throw wire::Error(W::cancelled);
        };
        const auto failed = [](W code) -> WorkerResult {
            if (code == W::cancelled) return {Status::cancelled, "cancelled", nullptr};
            if (code == W::resource_limit) return {Status::failed, "backup_space_insufficient", nullptr};
            if (code == W::timeout) return {Status::failed, "timeout", nullptr};
            return {Status::failed, "backup_verification_failed", nullptr};
        };
        try
        {
            return {Status::succeeded, {}, archives_->create(claimed.identity,
                payload.at("max_bytes").get<uint64_t>(), local_check, post_capture)};
        }
        catch (const wire::Error& error) { return failed(error.code()); }
        catch (const disk::Error& error)
        {
            return failed(error.fault() == disk::Fault::limit ? W::resource_limit : W::invalid_state);
        }
        catch (const std::exception&) { return failed(W::invalid_state); }
    }();
    {
        const std::lock_guard lock(progress_mutex_);
        progress_ = {};
    }
    if (stop.stop_requested()) return;
    auto tx = acquire(engine_, stop);
    auto jobs = list(*tx, runner_owner, job_schema);
    auto& job = require_job(jobs, claimed.identity);
    using Status = WorkerResult::Status;
    const bool cancel = result.status == Status::cancelled ||
        (job.data.at("cancel_requested").get<bool>() && !((backup || maintenance) && result.status == Status::succeeded));
    bool success = !cancel && result.status == Status::succeeded;
    auto reason = result.reason;
    auto output = success ? result.result : Json(nullptr);
    bool paused = false;
    if (success && config.contains("model") && result.result.contains("checkpoint"))
    {
        const auto& state = result.result.at("checkpoint");
        const auto& epoch = state.at("iterator").at("epoch");
        if (result.result.at("runtime") != ml::mlp_runtime || !payload.contains("pause_after_epoch") ||
            epoch != payload.at("pause_after_epoch"))
        {
            success = false;
            reason = "invalid_checkpoint";
            output = nullptr;
        }
        else
        {
            Json record{{"format", 1}, {"runtime", ml::mlp_runtime}, {"dataset_id", config.at("dataset")},
                {"request_digest", ml::dataset_fingerprint(payload)}, {"source_job", job.identity},
                {"model", config.at("model")}, {"state", state}};
            const auto checksum = ml::dataset_fingerprint(record);
            job.data["checkpoint"] = {{"record", std::move(record)}, {"checksum", checksum}};
            output = {{"job_id", job.identity}, {"dataset_id", config.at("dataset")}, {"checksum", checksum},
                {"epoch", epoch}, {"runtime", ml::mlp_runtime}};
            paused = true;
        }
    }
    if (success && !paused && config.contains("model"))
    {
        const auto save = tx->statement_savepoint(runner_owner);
        try
        {
            output = ml::publish_model(*tx, runner_owner, config.at("model").get<int64_t>(),
                config.at("dataset").get<std::string>(), job.identity, result.result, config.at("payload").at("options"));
            tx->release_savepoint(runner_owner, save);
        }
        catch (...)
        {
            tx->rollback_to(runner_owner, save);
            tx->release_savepoint(runner_owner, save);
            success = false;
            reason = "model_publish_failed";
            output = nullptr;
        }
    }
    const bool retry = !cancel && !success && job.data.at("attempt").get<uint32_t>() <= job.data.at("retries").get<uint32_t>();
    transition(job, retry ? "queued" : cancel ? "cancelled" : paused ? "paused" : success ? "succeeded" : "failed");
    job.data["reason"] = cancel ? Json("cancelled") : success ? Json(nullptr) : Json(reason);
    job.data["result"] = std::move(output);
    job.data["ended"] = retry ? 0 : utc();
    job.data["cpu_rate_limited"] = result.cpu_rate_limited;
    if (retry) job.data["not_before"] = utc() + job.data.at("backoff_seconds").get<uint32_t>();
    store(*tx, runner_owner, job);
    durable(engine_, *tx);
}

void Manager::run(std::stop_token stop)
{
    try
    {
        cleanup_worker_workspaces(temporary_);
        recover(stop);
        while (!stop.stop_requested())
        {
            try { step(stop); }
            catch (const disk::Error& error)
            {
                if (error.fault() != disk::Fault::busy) throw;
            }
            std::this_thread::sleep_for(50ms);
        }
    }
    catch (...)
    {
        if (!stop.stop_requested()) paused_.store(true);
    }
}
}
