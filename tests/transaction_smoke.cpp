#include "transactions.hpp"

#include <windows.h>
#include <atomic>
#include <future>
#include <iostream>
#include <thread>

using namespace sdp::disk;
using namespace std::chrono_literals;

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Operation>
void rejects(Fault reason, Operation call)
{
    try { call(); }
    catch (const Error& error)
    {
        if (error.fault() == reason) return;
        throw;
    }
    throw std::runtime_error("Expected a transaction failure");
}

Row amount(int64_t value, uint32_t schema = 1) { return {schema, {value}}; }
int64_t value(const Row& row) { return std::get<int64_t>(row.values.at(0)); }

void atomicity(const std::filesystem::path& root)
{
    const auto path = root / "atomic.sdp";
    Store::initialize(path);
    RowId permanent{}, catalog{};
    {
        Engine database(path);
        auto tx = database.begin(10, 0ms);
        permanent = tx->insert(10, amount(3, 4));
        rejects(Fault::invalid, [&] { tx->get(11, permanent); });
        const auto outer = tx->savepoint(10);
        tx->update(10, permanent, amount(9, 5));
        const auto inner = tx->savepoint(10);
        tx->erase(10, permanent);
        tx->rollback_to(10, inner);
        check(tx->get(10, permanent) == amount(9, 5), "inner savepoint restored wrong row");
        tx->rollback_to(10, outer);
        check(tx->get(10, permanent) == amount(3, 4), "outer savepoint restored wrong schema or row");
        rejects(Fault::missing, [&] { tx->rollback_to(10, inner); });
        tx->release_savepoint(10, outer);
        catalog = tx->insert(10, amount(81, 12), ChangeKind::catalog);
        check(tx->commit(10) == 1, "initial commit LSN mismatch");

        auto discarded = database.begin(11, 0ms);
        discarded->update(11, permanent, amount(99));
        discarded->rollback(11);
        const auto revisions = database.history();
        const auto& changes = revisions.at(0).changes;
        check(revisions.size() == 1 && changes.size() == 2 && changes[0].id == permanent &&
            decode(changes[0].after) == amount(3, 4) && changes[0].before.empty() &&
            changes[1].kind == ChangeKind::catalog && changes[1].id == catalog &&
            decode(changes[1].after) == amount(81, 12), "history diverged from committed row and schema changes");
    }
    {
        Engine reopened(path);
        auto tx = reopened.begin(17, 0ms);
        check(tx->get(17, permanent) == amount(3, 4) && tx->get(17, catalog) == amount(81, 12),
            "reopen did not preserve committed rows");
        tx->update(17, permanent, amount(6, 5));
        check(tx->commit(17) == 2, "updated commit LSN mismatch");
        const auto history = reopened.history();
        check(history.size() == 2 && history[1].changes.size() == 1 &&
            decode(history[1].changes[0].before) == amount(3, 4) &&
            decode(history[1].changes[0].after) == amount(6, 5), "before/after schemas were not journaled");
    }
    std::cout << "Owner, commit, rollback, nested savepoints, stable IDs and atomic change-set passed\n";
}

void fault_cases(const std::filesystem::path& root)
{
    for (const auto mode : {1, 2, 3, 4})
    {
        const auto path = root / ("fault-" + std::to_string(mode) + ".sdp");
        Store::initialize(path);
        bool armed{};
        std::vector<CommitPhase> phases;
        Hooks hooks;
        hooks.wal = [&](Io io, size_t size) {
            if (!armed) return size;
            if (mode == 1 && io == Io::write) { armed = false; return size - 1; }
            if (mode == 2 && io == Io::write) { armed = false; throw Error(Fault::io, "Injected disk full"); }
            if (mode == 3 && io == Io::flush) { armed = false; throw Error(Fault::io, "Injected flush failure"); }
            return size;
        };
        hooks.store = [&](Io io, size_t size) {
            if (armed && mode == 4 && io == Io::write) return size - 1;
            return size;
        };
        hooks.observe = [&](CommitPhase phase) { phases.push_back(phase); };
        {
            Engine database(path, hooks);
            auto tx = database.begin(19, 0ms);
            tx->insert(19, amount(59));
            armed = true;
            rejects(mode < 3 ? Fault::io : Fault::uncertain, [&] { tx->commit(19); });
            if (mode < 3)
                check(database.current().lsn == 0 && database.history().empty() && phases.empty(),
                    "failed pre-marker write published a transaction");
            else
            {
                rejects(Fault::uncertain, [&] { database.current(); });
                rejects(Fault::uncertain, [&] { database.begin(20, 0ms); });
                check(phases.size() == (mode == 3 ? 1u : 2u) && phases[0] == CommitPhase::body_written,
                    "phase ordering violated during I/O failure");
            }
        }
        if (mode < 4)
        {
            Store untouched(path, true);
            check(untouched.header().lsn == 0 && untouched.inspect().rows == 0,
                "unconfirmed WAL write reached the data file");
        }
    }
    std::cout << "Injected short write, disk-full, WAL flush and data publish faults produced no success\n";
}

void check_parallel_transfers(const std::filesystem::path& root)
{
    const auto path = root / "balances.sdp";
    Store::initialize(path);
    Engine database(path);
    auto initial = database.begin(1, 0ms);
    const auto a = initial->insert(1, amount(5000));
    const auto b = initial->insert(1, amount(5000));
    initial->commit(1);

    auto blocker = database.begin(2, 0ms);
    auto timeout = std::async(std::launch::async, [&] {
        rejects(Fault::busy, [&] { database.begin(3, 35ms); });
    });
    timeout.get();

    std::stop_source cancellation;
    auto pending = std::async(std::launch::async, [&] {
        rejects(Fault::cancelled, [&] { database.begin(4, 2s, cancellation.get_token()); });
    });
    cancellation.request_stop();
    pending.get();

    blocker->update(2, a, amount(999));
    database.disconnect(2);
    rejects(Fault::cancelled, [&] { blocker->commit(2); });
    {
        auto expired = database.begin(6, 0ms, {}, 20ms);
        auto next = database.begin(7, 500ms);
        check(next->get(7, a) == amount(5000), "disconnected owner leaked a write");
        rejects(Fault::cancelled, [&] { expired->get(6, a); });
        next->rollback(7);
    }

    std::vector<std::future<void>> jobs;
    for (Owner owner = 10; owner != 14; ++owner)
        jobs.push_back(std::async(std::launch::async, [&, owner] {
            for (int iteration = 0; iteration < 8; ++iteration)
            {
                auto tx = database.begin(owner, 10s);
                auto balance_a = value(tx->get(owner, a));
                auto balance_b = value(tx->get(owner, b));
                check(balance_a + balance_b == 10000, "partial transfer observed");
                const auto delta = owner % 2 == 0 ? 11 : -11;
                tx->update(owner, a, amount(balance_a - delta));
                tx->update(owner, b, amount(balance_b + delta));
                tx->commit(owner);
            }
        }));
    for (auto& job : jobs) job.get();
    auto balance = database.begin(9, 0ms);
    check(value(balance->get(9, a)) == 5000 && value(balance->get(9, b)) == 5000,
        "concurrent transfers broke the balance invariant");
    balance->rollback(9);
    check(database.history().size() == 33, "transfer ledger lost a revision");
    std::cout << "Thirty-two concurrent transfers, lock expiry, timeout, cancel and disconnect passed\n";
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc != 2) return 2;
        auto root = std::filesystem::path(argv[1]) / ("transactions-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()));
        if (!std::filesystem::create_directories(root)) return 2;
        atomicity(root);
        fault_cases(root);
        check_parallel_transfers(root);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
