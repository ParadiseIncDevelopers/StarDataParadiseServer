#include "transactions.hpp"

#include <algorithm>
#include <utility>

namespace sdp::disk
{
namespace
{
using namespace std::chrono_literals;
constexpr size_t max_savepoints = 8;
constexpr size_t max_internal_savepoints = 10;
}

Engine::Engine(const std::filesystem::path& path, Hooks hooks)
    : path_(path), hooks_(std::move(hooks)), store_((verify_database(path, true), path), false, 16, hooks_.store),
      segments_(segments(path))
{
    if (segments_.empty())
    {
        if (store_.header().lsn != 0) throw Error(Fault::format, "Existing data requires its matching WAL");
        segments_.push_back({segment_path(path, 1), 1});
        wal_ = open_segment(segments_.back().path, 1, true, hooks_.wal);
    }
    else
    {
        for (size_t index = 0; index + 1 < segments_.size(); ++index)
            archived_bytes_ += std::filesystem::file_size(segments_[index].path);
        const auto& active = segments_.back();
        wal_ = open_segment(active.path, active.first, false, hooks_.wal);
    }
    const auto ledger = scan(*wal_);
    if (ledger.incomplete_tail || ledger.last != store_.header().lsn)
        throw Error(Fault::uncertain, "The WAL and data together require recovery");
}

void Engine::require_usable() const
{
    if (poisoned_) throw Error(Fault::uncertain, "Storage is paused pending recovery");
}

void Engine::expire(Clock::time_point now)
{
    if (active_ && now >= active_->expires) release(active_->token);
}

void Engine::release(uint64_t token)
{
    if (active_ && active_->token == token)
    {
        active_.reset();
        available_.notify_all();
    }
}

std::unique_ptr<Transaction> Engine::begin(Owner owner, std::chrono::milliseconds wait,
    std::stop_token stop, std::chrono::milliseconds lifetime)
{
    if (!owner || wait < 0ms || wait > 30s || lifetime <= 0ms || lifetime > 30s)
        throw Error(Fault::invalid, "Transaction ownership or time limit is invalid");
    std::unique_lock lock(mutex_);
    const auto deadline = Clock::now() + wait;
    for (;;)
    {
        require_usable();
        if (stop.stop_requested()) throw Error(Fault::cancelled, "Transaction wait was cancelled");
        const auto now = Clock::now();
        expire(now);
        if (!active_) break;
        if (now >= deadline) throw Error(Fault::busy, "Transaction lock wait timed out");
        const auto current = active_->token;
        available_.wait_until(lock, stop, (std::min)(deadline, active_->expires),
            [&] { return poisoned_ || !active_ || active_->token != current; });
    }
    if (!next_) throw Error(Fault::limit, "Transaction ticket limit reached");
    const auto token = next_++;
    auto transaction = std::unique_ptr<Transaction>(new Transaction(*this, owner, token));
    active_ = Lease{owner, token, Clock::now() + lifetime};
    return transaction;
}

void Engine::disconnect(Owner owner)
{
    const std::lock_guard lock(mutex_);
    if (active_ && active_->owner == owner) release(active_->token);
}

Header Engine::current()
{
    const std::lock_guard lock(mutex_);
    require_usable();
    return store_.header();
}

std::vector<Revision> Engine::history()
{
    std::vector<Revision> revisions;
    visit_history([&](Revision&& revision) { revisions.push_back(std::move(revision)); });
    return revisions;
}

void Engine::visit_history(const RevisionVisitor& visitor, const std::function<void()>& check)
{
    const std::lock_guard lock(mutex_);
    require_usable();
    uint64_t last{};
    const auto yield = [&](Frame&& frame) {
        if (check) check();
        if (frame.batch.header.lsn != last + 1)
            throw Error(Fault::corrupt, "Published history has a WAL revision gap");
        last = frame.batch.header.lsn;
        if (visitor) visitor({last, std::move(frame.changes)});
    };
    for (size_t index = 0; index < segments_.size(); ++index)
    {
        if (check) check();
        const auto& segment = segments_[index];
        if (segment.first != last + 1) throw Error(Fault::corrupt, "Published history is missing a WAL segment");
        const bool active = index + 1 == segments_.size();
        const auto result = [&] {
            if (active) return scan(*wal_, yield);
            File file(segment.path, File::Mode::read_only);
            return scan(file, yield);
        }();
        if (result.first != segment.first || result.incomplete_tail || (!active && !result.commits))
            throw Error(Fault::corrupt, "Published history contains an invalid WAL segment");
    }
    if (last != store_.header().lsn)
        throw Error(Fault::corrupt, "Published history and WAL disagree");
}

Checkpoint Engine::checkpoint(const RetentionPins& pins)
{
    const std::lock_guard lock(mutex_);
    require_usable();
    expire(Clock::now());
    const auto published = store_.header().lsn;
    const std::optional<uint64_t> active = active_ ? std::optional<uint64_t>((std::max)(published, uint64_t{1})) : std::nullopt;
    auto result = retention(published, segments_.front().first, active, pins);

    if (segments_.back().first <= published)
    {
        const auto bytes = wal_->size();
        if (bytes + archived_bytes_ > wal_quota - 64)
            throw Error(Fault::limit, "WAL quota leaves no room for a checkpoint segment");
        const auto next = published + 1;
        Segment descriptor{segment_path(path_, next), next};
        segments_.reserve(segments_.size() + 1);
        auto next_file = open_segment(descriptor.path, next, true, hooks_.wal);
        archived_bytes_ += bytes;
        segments_.push_back(std::move(descriptor));
        wal_ = std::move(next_file);
    }

    for (size_t index = 0; index + 1 < segments_.size(); ++index)
    {
        const auto last = segments_[index + 1].first - 1;
        if (last < result.retain_from && last <= published) ++result.reclaimable;
    }

    save_checkpoint(path_, result, [this] {
        if (hooks_.observe) hooks_.observe(CommitPhase::checkpoint_written);
    });
    if (hooks_.observe) hooks_.observe(CommitPhase::checkpoint_published);
    return result;
}

uint64_t Engine::commit(Frame frame)
{
    // Establish WAL ownership before the first publication using this recovery implementation.
    auto checkpoint_file = path_;
    checkpoint_file += ".checkpoint";
    if (std::filesystem::symlink_status(checkpoint_file).type() == std::filesystem::file_type::not_found)
        save_checkpoint(path_, retention(store_.header().lsn, segments_.front().first, {}, {}));

    const auto emit = [&](CommitPhase phase) {
        if (hooks_.observe) hooks_.observe(phase);
    };
    try
    {
        append_commit(*wal_, frame, [&] { emit(CommitPhase::body_written); }, wal_quota - archived_bytes_);
    }
    catch (const Error& error)
    {
        if (error.fault() == Fault::uncertain) poisoned_ = true;
        throw;
    }

    try
    {
        emit(CommitPhase::wal_durable);
        store_.apply(frame.batch, [&](Publication event) {
            emit(event == Publication::page_written ? CommitPhase::page_written : CommitPhase::data_flushed);
        });
        emit(CommitPhase::data_published);
    }
    catch (...)
    {
        poisoned_ = true;
        throw Error(Fault::uncertain, "Durable WAL exists but data publication did not finish");
    }
    return frame.batch.header.lsn;
}

Transaction::Transaction(Engine& engine, Owner owner, uint64_t token)
    : engine_(engine), owner_(owner), token_(token), editor_(engine.store_)
{
    changes_.reserve(change_count_limit);
    saves_.reserve(max_savepoints + max_internal_savepoints);
}

Transaction::~Transaction()
{
    if (finished_) return;
    const std::lock_guard lock(engine_.mutex_);
    engine_.release(token_);
}

std::unique_lock<std::mutex> Transaction::lock(Owner caller)
{
    std::unique_lock held(engine_.mutex_);
    if (caller != owner_) throw Error(Fault::invalid, "Transaction belongs to a different session");
    if (finished_) throw Error(Fault::invalid, "Transaction is already closed");
    engine_.require_usable();
    engine_.expire(Engine::Clock::now());
    if (!engine_.active_ || engine_.active_->token != token_)
        throw Error(Fault::cancelled, "Transaction expired or was disconnected");
    return held;
}

void Transaction::ensure_active(Owner caller)
{
    const auto held = lock(caller);
}

void Transaction::visit_changes(Owner caller, const std::function<void(std::span<const Change>)>& visitor)
{
    const auto held = lock(caller);
    visitor(changes_);
}

Row Transaction::get(Owner caller, RowId id)
{
    const auto held = lock(caller);
    return editor_.get(id);
}

void Transaction::visit(Owner caller, const RowVisitor& visitor, std::optional<uint32_t> schema)
{
    scan(caller, 0, [&](RowId id, const Row& row) { visitor(id, row); return true; }, schema);
}

bool Transaction::scan(Owner caller, RowId after, const ScanRow& visitor, std::optional<uint32_t> schema,
    const std::function<void()>& check)
{
    const auto held = lock(caller);
    const auto boundary = [&] {
        engine_.expire(Engine::Clock::now());
        if (!engine_.active_) throw Error(Fault::cancelled, "Transaction expired during scan");
        if (check) check();
    };
    return editor_.scan(after, visitor, schema, boundary);
}

void Transaction::ensure_change(const Change& change) const
{
    if (change.kind != ChangeKind::row && change.kind != ChangeKind::catalog)
        throw Error(Fault::invalid, "Invalid change kind");
    if (changes_.size() == change_count_limit || change.before.size() + change.after.size() > change_budget - bytes_)
        throw Error(Fault::limit, "Transaction change-set budget reached");
}

void Transaction::mark(Change change)
{
    bytes_ += change.before.size() + change.after.size();
    changes_.push_back(std::move(change));
}

RowId Transaction::insert(Owner caller, const Row& row, ChangeKind kind)
{
    const auto held = lock(caller);
    Change change{0, kind, {}, encode(row)};
    ensure_change(change);
    const auto id = change.id = editor_.insert(row);
    mark(std::move(change));
    return id;
}

void Transaction::update(Owner caller, RowId id, const Row& row, ChangeKind kind)
{
    const auto held = lock(caller);
    Change change{id, kind, encode(editor_.get(id)), encode(row)};
    ensure_change(change);
    editor_.update(id, row);
    mark(std::move(change));
}

void Transaction::erase(Owner caller, RowId id, ChangeKind kind)
{
    const auto held = lock(caller);
    Change change{id, kind, encode(editor_.get(id)), {}};
    ensure_change(change);
    editor_.erase(id);
    mark(std::move(change));
}

void Transaction::restore_row(Owner caller, RowId id, const Row& row, ChangeKind kind)
{
    const auto held = lock(caller);
    Change change{id, kind, {}, encode(row)};
    ensure_change(change);
    editor_.restore_row(id, row);
    mark(std::move(change));
}

Savepoint Transaction::savepoint(Owner caller)
{
    const auto held = lock(caller);
    return make_savepoint(max_savepoints);
}

Savepoint Transaction::statement_savepoint(Owner caller)
{
    const auto held = lock(caller);
    return make_savepoint(max_savepoints + max_internal_savepoints);
}

Savepoint Transaction::make_savepoint(size_t limit)
{
    if (saves_.size() >= limit || !next_save_) throw Error(Fault::limit, "Savepoint limit reached");
    const auto point = next_save_++;
    saves_.push_back({point, editor_.snapshot(), changes_.size(), bytes_});
    return point;
}

size_t Transaction::find(Savepoint id) const
{
    const auto item = std::ranges::find(saves_, id, &Saved::id);
    if (item == saves_.end()) throw Error(Fault::missing, "Savepoint does not exist");
    return static_cast<size_t>(item - saves_.begin());
}

void Transaction::rollback_to(Owner caller, Savepoint id)
{
    const auto held = lock(caller);
    const auto index = find(id);
    const auto& saved = saves_[index];
    editor_.restore(saved.dirty);
    changes_.resize(saved.count);
    bytes_ = saved.bytes;
    saves_.resize(index + 1);
}

void Transaction::release_savepoint(Owner caller, Savepoint id)
{
    const auto held = lock(caller);
    saves_.resize(find(id));
}

void Transaction::finish()
{
    finished_ = true;
    engine_.release(token_);
    editor_.restore({engine_.store_.header().pages, {}});
    changes_.clear();
    saves_.clear();
}

void Transaction::rollback(Owner caller)
{
    const auto held = lock(caller);
    finish();
}

uint64_t Transaction::commit(Owner caller)
{
    const auto held = lock(caller);
    const auto published = engine_.store_.header().lsn;
    if (changes_.empty()) { finish(); return published; }
    if (published >= UINT64_MAX - 2) throw Error(Fault::limit, "WAL LSN limit reached");
    auto batch = editor_.prepare(published + 1);
    try
    {
        const auto lsn = engine_.commit({std::move(batch), std::move(changes_)});
        finish();
        return lsn;
    }
    catch (...) { finish(); throw; }
}
}
