#pragma once

#include "recovery.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>

namespace sdp::disk
{
using Owner = uint64_t;
using Savepoint = uint32_t;
enum class CommitPhase
{
    body_written, wal_durable, page_written, data_flushed, data_published,
    checkpoint_written, checkpoint_published
};
struct Hooks
{
    IoHook wal;
    IoHook store;
    std::function<void(CommitPhase)> observe;
};
struct Revision
{
    uint64_t lsn;
    std::vector<Change> changes;
};
using RevisionVisitor = std::function<void(Revision&&)>;

class Transaction;
class Engine
{
public:
    explicit Engine(const std::filesystem::path& path, Hooks hooks = {});

    std::unique_ptr<Transaction> begin(Owner owner, std::chrono::milliseconds wait,
        std::stop_token stop = {}, std::chrono::milliseconds lifetime = std::chrono::seconds(20));
    void disconnect(Owner owner);
    Header current();
    std::vector<Revision> history();
    // Runs while the WAL read lock is held; callbacks must not reenter the engine.
    void visit_history(const RevisionVisitor& visitor, const std::function<void()>& check = {});
    Checkpoint checkpoint(const RetentionPins& pins = {});

private:
    using Clock = std::chrono::steady_clock;
    struct Lease
    {
        Owner owner;
        uint64_t token;
        Clock::time_point expires;
    };
    void expire(Clock::time_point now);
    void require_usable() const;
    void release(uint64_t token);
    uint64_t commit(Frame frame);
    friend class Transaction;

    std::filesystem::path path_;
    Hooks hooks_;
    Store store_;
    std::vector<Segment> segments_;
    std::unique_ptr<File> wal_;
    uint64_t archived_bytes_{};
    std::mutex mutex_;
    std::condition_variable_any available_;
    std::optional<Lease> active_;
    uint64_t next_{1};
    bool poisoned_{};
};

class Transaction
{
public:
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void ensure_active(Owner caller);
    void visit_changes(Owner caller, const std::function<void(std::span<const Change>)>& visitor);
    Row get(Owner caller, RowId id);
    // Visitors and boundary checks run inside the engine lock and must not reenter it.
    void visit(Owner caller, const RowVisitor& visitor, std::optional<uint32_t> schema = {});
    bool scan(Owner caller, RowId after, const ScanRow& visitor, std::optional<uint32_t> schema = {},
        const std::function<void()>& check = {});
    RowId insert(Owner caller, const Row& row, ChangeKind kind = ChangeKind::row);
    void update(Owner caller, RowId id, const Row& row, ChangeKind kind = ChangeKind::row);
    void erase(Owner caller, RowId id, ChangeKind kind = ChangeKind::row);
    void restore_row(Owner caller, RowId id, const Row& row, ChangeKind kind = ChangeKind::row);
    Savepoint savepoint(Owner caller);
    Savepoint statement_savepoint(Owner caller);
    void rollback_to(Owner caller, Savepoint id);
    void release_savepoint(Owner caller, Savepoint id);
    void rollback(Owner caller);
    uint64_t commit(Owner caller);

private:
    struct Saved
    {
        Savepoint id;
        Editor::Snapshot dirty;
        size_t count;
        size_t bytes;
    };
    friend class Engine;
    Transaction(Engine& engine, Owner owner, uint64_t token);
    std::unique_lock<std::mutex> lock(Owner caller);
    void ensure_change(const Change& change) const;
    void mark(Change change);
    Savepoint make_savepoint(size_t limit);
    size_t find(Savepoint id) const;
    void finish();

    Engine& engine_;
    Owner owner_;
    uint64_t token_;
    Editor editor_;
    std::vector<Change> changes_;
    std::vector<Saved> saves_;
    size_t bytes_{};
    Savepoint next_save_{1};
    bool finished_{};
};
}
