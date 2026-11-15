#pragma once

#include "executor.hpp"
#include "access.hpp"

#include <atomic>
#include <sdp/wire.hpp>

namespace sdp::jobs { class Manager; }

namespace sdp::sql
{
class Debugger;
struct HistoryPlan;

class Session
{
public:
    Session(disk::Engine& engine, disk::Owner owner, std::string actor, std::chrono::milliseconds timeout,
        std::function<int64_t()> outcome_clock = {}, std::filesystem::path temporary = {},
        Security* security = nullptr, Principal principal = {}, bool test_instance = false, jobs::Manager* jobs = nullptr);
    ~Session();
    wire::Json dispatch(uint32_t id, std::string_view op, wire::Json& body, std::string_view correlation,
        std::stop_token cancellation = {});
    wire::Json dispatch(uint32_t id, std::string_view op, const wire::Json& body, std::string_view correlation);
    bool transaction_active() const noexcept;
    uint64_t lock_wait_ms() const noexcept;
    void attach_debugger(std::shared_ptr<Debugger> debugger);
    bool debugging() const noexcept;

private:
    using Clock = std::chrono::steady_clock;

    struct Prepared { Statement statement; ParameterTypes parameter_types; };
    class Spill;
    struct Materialized
    {
        std::vector<Projection> columns;
        std::vector<RowValues> rows;
        size_t offset{};
        uint64_t file_offset{};
        std::unique_ptr<Spill> spill;
    };
    struct RoutineStream
    {
        std::vector<std::pair<std::string, Materialized>> sets;
        size_t active{};
    };
    struct Result
    {
        std::string id;
        std::variant<Cursor, Materialized, RoutineStream> cursor;
        std::unique_ptr<disk::Transaction> read;
        uint32_t batch{};
        std::vector<Requirement> requirements;
    };
    struct NamedSavepoint { std::string name; disk::Savepoint id; };
    struct Outcome { uint64_t lsn; int64_t expires; };

    wire::Json request(std::string_view op, wire::Json& body);
    wire::Json prepare(const wire::Json& body);
    wire::Json execute(const wire::Json& body);
    wire::Json fetch(const wire::Json& body);
    ResultBatch fetch_materialized(Materialized& result, uint32_t count);
    Materialized materialize(const AdvancedSelection& selected, disk::Transaction& tx,
        const ParameterValues& parameters, uint32_t limit);
    wire::Json transact(std::string_view op, const wire::Json& body);
    wire::Json metadata();
    wire::Json history(const HistoryCommand& command);
    wire::Json decision(std::string_view operation, const wire::Json& body);
    wire::Json job_request(std::string_view operation, const wire::Json& body);
    wire::Json backup_request(std::string_view operation, const wire::Json& body);
    wire::Json dataset_request(std::string_view operation, const wire::Json& body);
    wire::Json create_dataset(disk::Transaction& tx, const wire::Json& body);
    wire::Json model_request(std::string_view operation, const wire::Json& body);
    wire::Json enqueue_model(disk::Transaction& tx, int64_t model, std::string_view dataset,
        const std::string& token, wire::Json payload, const WorkCheck& check);
    wire::Json compare_models(disk::Transaction& tx, Catalog& catalog, const wire::Json& body);
    wire::Json restore_history(const HistoryCommand& command);
    std::unique_ptr<disk::Transaction> begin();
    disk::Transaction& use(std::unique_ptr<disk::Transaction>& local);
    std::vector<Requirement> authorize(Catalog& catalog, const Statement& statement);
    wire::Json password(wire::Json& body);
    wire::Json test_routine(const wire::Json& body);
    std::optional<Outcome> outcome(disk::Transaction& tx, std::string_view token);
    uint64_t commit(disk::Transaction& tx, std::string_view token);
    void check_deadline() const;
    void close_transaction();
    std::string next_id();

    disk::Engine& engine_;
    disk::Owner owner_;
    std::string actor_;
    std::chrono::milliseconds timeout_;
    Clock::time_point deadline_;
    std::stop_token cancellation_;
    std::atomic<Clock::duration::rep> transaction_expires_{0};
    std::atomic<Clock::duration::rep> lock_wait_started_{0};
    std::function<int64_t()> outcome_clock_;
    std::filesystem::path temporary_;
    Security* security_;
    Principal principal_;
    bool test_instance_;
    jobs::Manager* jobs_;
    std::atomic<std::shared_ptr<Debugger>> debugger_;
    std::unique_ptr<HistoryPlan> history_plan_;
    wire::Json last_profile_ = wire::Json::array();
    std::unique_ptr<disk::Transaction> transaction_;
    std::optional<Result> result_;
    std::vector<NamedSavepoint> savepoints_;
    std::map<std::string, Prepared, std::less<>> prepared_;
    uint64_t next_{1};
};
}
