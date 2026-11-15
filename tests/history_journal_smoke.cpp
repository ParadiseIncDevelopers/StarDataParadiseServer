#include "history_journal.hpp"

#include <windows.h>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace
{
using namespace sdp;
using namespace std::chrono_literals;
constexpr disk::Owner owner = 12;
constexpr int64_t fixed_time = 1'699'999'999;

void require(bool test, const char* message)
{
    if (!test) throw std::runtime_error(message);
}

void seal(sql::HistoryJournal& journal)
{
    journal.seal("test_actor", fixed_time);
}

void normal(const std::filesystem::path& path)
{
    disk::Store::initialize(path);
    disk::RowId id{};
    std::string digest;
    {
        disk::Engine engine(path);
        {
            auto transaction = engine.begin(owner, 0ms);
            id = transaction->insert(owner, {1, {int64_t{10}}});
            require(transaction->commit(owner) == 1, "Legacy revision not published");
        }
        {
            auto transaction = engine.begin(owner, 0ms);
            sql::HistoryJournal journal(engine, *transaction, owner);
            const auto legacy = journal.read(1);
            require(legacy.actor.empty() && legacy.sha256.empty() && !legacy.utc_seconds, "Legacy metadata fabricated");
            transaction->update(owner, id, {1, {int64_t{20}}});
            transaction->insert(owner, {0x53444131, {std::string("internal-security-fixture")}}, disk::ChangeKind::catalog);
            transaction->insert(owner, {0x53444c31, {std::string("test_actor"), std::string(32,'a'),
                int64_t{2}, int64_t{fixed_time + 24 * 60 * 60}}});
            seal(journal);
            require(transaction->commit(owner) == 2, "Sealed revision not published");
        }
        {
            auto transaction = engine.begin(owner, 0ms);
            sql::HistoryJournal journal(engine, *transaction, owner);
            const auto revision = journal.read(2);
            digest = revision.sha256;
            require(revision.actor == "test_actor" && revision.parent == 1 && revision.transaction == 2 &&
                revision.utc_seconds == fixed_time && digest.size() == 64 && revision.changes.size() == 1 &&
                revision.changes.front().id == id, "Stable ID lost or sensitive records exposed");
            journal.pin({"copy","snapshot",2});
            journal.pin({"copy","backup",2});
            try { journal.purge_before(3); require(false, "Pinned revision lost"); }
            catch (const sql::HistoryPinned& error) { require(error.revision() == 2, "Wrong pin diagnostic"); }
            journal.unpin("snapshot","copy");
            try { journal.purge_before(3); require(false, "Other typed pin lost"); }
            catch (const sql::HistoryPinned& error) { require(error.kind() == "backup", "Typed unpin affected wrong copy"); }
            journal.quota(1);
            seal(journal);
            transaction->commit(owner);
        }
        const auto before_failure = engine.current().lsn;
        {
            auto transaction = engine.begin(owner, 0ms);
            sql::HistoryJournal journal(engine, *transaction, owner);
            transaction->update(owner, id, {1, {int64_t{30}}});
            try { seal(journal); require(false, "Quota did not reject write"); }
            catch (const sql::Error& error) { require(error.code() == sql::Code::limit, "Wrong quota error"); }
            transaction->rollback(owner);
        }
        {
            auto transaction = engine.begin(owner, 0ms);
            sql::HistoryJournal journal(engine, *transaction, owner);
            require(engine.current().lsn == before_failure &&
                transaction->get(owner, id).values[0] == disk::Value{int64_t{20}}, "Quota rollback left a revision");
            journal.unpin("backup","copy");
            journal.purge_before(3);
            seal(journal);
            transaction->commit(owner);
        }
        engine.checkpoint();
        {
            auto transaction = engine.begin(owner, 0ms);
            sql::HistoryJournal journal(engine, *transaction, owner);
            transaction->insert(owner, {0x53444c31, {std::string("test_actor"), std::string(32,'b'),
                int64_t{5}, int64_t{fixed_time + 24 * 60 * 60}}});
            seal(journal);
            transaction->commit(owner);
        }
    }
    {
        disk::Engine engine(path);
        auto transaction = engine.begin(owner, 0ms);
        sql::HistoryJournal journal(engine,*transaction,owner);
        const auto state = journal.verify();
        require(state.head == 5 && state.floor == 3 && state.quota == 1 && state.pins.empty() &&
            state.retained_bytes == 0, "Policy or retained bytes lost across restart");
        try { journal.read(2); require(false, "Expired history visible after restart"); }
        catch (const sql::HistoryUnavailable& error) { require(error.floor() == 3, "Wrong recoverability floor"); }
        const auto empty = journal.read(5);
        require(empty.changes.empty() && empty.sha256.empty() && empty.actor == "test_actor" && empty.utc_seconds == fixed_time, "Outcome-only semantics");
    }
}

void tamper(const std::filesystem::path& path)
{
    disk::Store::initialize(path);
    {
        disk::Engine engine(path);
        auto transaction = engine.begin(owner, 0ms);
        transaction->insert(owner, {1,{int64_t{7}}});
        transaction->insert(owner, {sql::history_seal_schema,
            {int64_t{1},int64_t{1},int64_t{0},std::string("test_actor"),fixed_time,disk::Bytes(32),disk::Bytes(32)}},
            disk::ChangeKind::catalog);
        transaction->commit(owner);
    }
    // Legacy physical CRC/redo checks accept the record; the independent SHA digest does not.
    require(disk::verify_database(path).header.lsn == 1, "Fixture is not physically valid");
    disk::Engine engine(path);
    auto transaction = engine.begin(owner, 0ms);
    sql::HistoryJournal journal(engine,*transaction,owner);
    try { journal.verify(); require(false, "Forged seal accepted"); }
    catch (const disk::Error& error) { require(error.fault() == disk::Fault::corrupt, "Wrong integrity error"); }
}

void malformed_ledger(const std::filesystem::path& path)
{
    disk::Store::initialize(path);
    {
        disk::Engine engine(path);
        auto transaction = engine.begin(owner, 0ms);
        transaction->insert(owner, {0x53444c31, {std::string("test_actor"), std::string(32,'a'),
            int64_t{2}, fixed_time + 24 * 60 * 60}});
        transaction->commit(owner);
    }
    disk::Engine engine(path);
    auto transaction = engine.begin(owner,0ms);
    sql::HistoryJournal journal(engine,*transaction,owner);
    try { journal.verify(); require(false, "Mismatched legacy ledger LSN accepted"); }
    catch (const disk::Error& error) { require(error.fault() == disk::Fault::corrupt, "Wrong ledger error"); }
}

void crash_seed(const std::filesystem::path& path)
{
    disk::Store::initialize(path);
    disk::Engine engine(path);
    auto transaction = engine.begin(owner,0ms);
    transaction->insert(owner, {1,{int64_t{10}}});
    sql::HistoryJournal journal(engine,*transaction,owner);
    seal(journal);
    transaction->commit(owner);
}

void crash_at(const std::filesystem::path& path, disk::CommitPhase point)
{
    disk::Hooks hooks;
    hooks.observe = [point](disk::CommitPhase current) {
        if (current == point) TerminateProcess(GetCurrentProcess(), 73);
    };
    disk::Engine engine(path, std::move(hooks));
    auto transaction = engine.begin(owner,0ms);
    transaction->insert(owner, {1,{int64_t{20}}});
    sql::HistoryJournal journal(engine,*transaction,owner);
    seal(journal);
    transaction->commit(owner);
}

void crash_verify(const std::filesystem::path& path, uint64_t expected_head)
{
    disk::Engine engine(path);
    auto transaction = engine.begin(owner,0ms);
    sql::HistoryJournal journal(engine,*transaction,owner);
    const auto info = journal.verify();
    require(info.head == expected_head, "Incorrect head after process kill");
    const auto revision = journal.read(info.head);
    require(revision.sha256.size() == 64 && revision.changes.size() == 1 &&
        transaction->get(owner, revision.changes[0].id).values[0] ==
            disk::Value{static_cast<int64_t>(10 * info.head)}, "Data and sealed history recovered differently");
}

void delete_seed(const std::filesystem::path& path)
{
    crash_seed(path);
    disk::Engine engine(path);
    auto transaction = engine.begin(owner, 0ms);
    sql::HistoryJournal journal(engine, *transaction, owner);
    const auto original = journal.read(1);
    transaction->erase(owner, original.changes.front().id);
    seal(journal);
    require(transaction->commit(owner) == 2, "Delete revision missing");
}

void restore_at(const std::filesystem::path& path, disk::CommitPhase point)
{
    disk::Hooks hooks;
    hooks.observe = [point](disk::CommitPhase current) {
        if (current == point) TerminateProcess(GetCurrentProcess(), 73);
    };
    disk::Engine engine(path, std::move(hooks));
    auto transaction = engine.begin(owner, 0ms);
    sql::HistoryJournal journal(engine, *transaction, owner);
    const auto removed = journal.read(2);
    const auto& image = removed.changes.front();
    transaction->restore_row(owner, image.id, disk::decode(image.before));
    seal(journal);
    transaction->commit(owner);
}

void restore_verify(const std::filesystem::path& path, bool durable)
{
    disk::Engine engine(path);
    auto transaction = engine.begin(owner, 0ms);
    sql::HistoryJournal journal(engine, *transaction, owner);
    const auto status = journal.verify();
    require(status.head == (durable ? 3 : 2), "Wrong revision after interrupted restore");
    const auto original = journal.read(1);
    const auto deleted = journal.read(2);
    const auto id = original.changes.front().id;
    require(id == deleted.changes.front().id && deleted.changes.front().after.empty(), "Delete history changed");
    if (!durable)
    {
        try { transaction->get(owner, id); require(false, "Uncommitted restore is visible"); }
        catch (const disk::Error& error) { require(error.fault() == disk::Fault::missing, "Wrong tombstone error"); }
        return;
    }
    const auto restored = journal.read(3);
    require(restored.changes.size() == 1 && restored.changes.front().id == id &&
        restored.changes.front().before.empty() && restored.changes.front().after == original.changes.front().after &&
        transaction->get(owner, id) == disk::decode(original.changes.front().after),
        "Durable restore did not preserve identity and sealed history");
    try { transaction->restore_row(owner, id, {1, {int64_t{99}}}); require(false, "Restore overwrote a live row"); }
    catch (const disk::Error& error) { require(error.fault() == disk::Fault::missing, "Wrong live-row restore error"); }
    require(transaction->get(owner, id).values.front() == disk::Value{int64_t{10}}, "Failed restore altered a live row");
}
}

int main(int argc, char** argv)
{
    if (argc != 3) return 1;
    try
    {
        const std::string_view mode = argv[1];
        const auto path = std::filesystem::path(argv[2]);
        if (mode == "normal") normal(path);
        else if (mode == "tamper") tamper(path);
        else if (mode == "bad-ledger") malformed_ledger(path);
        else if (mode == "seed") crash_seed(path);
        else if (mode == "kill-before") crash_at(path, disk::CommitPhase::body_written);
        else if (mode == "kill-after") crash_at(path, disk::CommitPhase::wal_durable);
        else if (mode == "verify-before") crash_verify(path,1);
        else if (mode == "verify-after") crash_verify(path,2);
        else if (mode == "delete-seed") delete_seed(path);
        else if (mode == "kill-restore-before") restore_at(path, disk::CommitPhase::body_written);
        else if (mode == "kill-restore-after") restore_at(path, disk::CommitPhase::wal_durable);
        else if (mode == "verify-restore-before") restore_verify(path, false);
        else if (mode == "verify-restore-after") restore_verify(path, true);
        else return 1;
        std::cout << "History " << mode << " passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
