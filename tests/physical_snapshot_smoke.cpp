
#include "transactions.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

namespace
{
using namespace sdp::disk;
using namespace std::chrono_literals;
constexpr Owner owner = 1;
constexpr Owner writer = 2;
const Row original{1, {std::string("before checkpoint"), std::string(120'000, 'a')}};
const Row consistent{1, {std::string("snapshot checkpoint"), std::string(132'000, 'b')}};
const Row later{1, {std::string("after backup")}};

void require(bool ok, const char* reason)
{
    if (!ok) throw std::runtime_error(reason);
}

template<typename Function>
void expect(Fault fault, const Function& function)
{
    try { function(); }
    catch (const Error& error)
    {
        require(error.fault() == fault, "wrong snapshot failure status");
        return;
    }
    throw std::runtime_error("snapshot should have failed");
}

void exercise(const std::filesystem::path& scratch)
{
    const auto source = scratch / "source.sdp";
    Store::initialize(source);
    Engine engine(source);
    RowId row{};
    {
        auto tx = engine.begin(owner, 0ms);
        row = tx->insert(owner, original);
        require(tx->commit(owner) == 1, "first commit absent");
    }
    require(engine.checkpoint().published == 1, "checkpoint did not publish first segment");
    {
        auto tx = engine.begin(owner, 0ms);
        tx->update(owner, row, consistent);
        require(tx->commit(owner) == 2, "second commit absent");
    }

    const auto backup = scratch / "snapshot";
    const auto quota = scratch / "quota";
    const auto collision = scratch / "collision";
    const auto cancelled = scratch / "cancelled";
    const auto dirty = scratch / "dirty";
    for (const auto& folder : {backup, quota, collision, cancelled, dirty})
        require(std::filesystem::create_directory(folder), "test staging folder already exists");
    const auto name = [](const auto& folder) { return folder / "restorable.sdp"; };
    auto checkpoint_collision = name(collision);
    checkpoint_collision += ".checkpoint";
    {
        std::ofstream marker(checkpoint_collision);
        marker << "preserve existing data";
    }

    constexpr uint64_t limit = 16 * 1024 * 1024;
    PhysicalSnapshot snapshot;
    {
        auto tx = engine.begin(owner, 0ms);
        expect(Fault::invalid, [&] { tx->copy_physical_snapshot(writer, name(quota), limit); });
        expect(Fault::limit, [&] { tx->copy_physical_snapshot(owner, name(quota), page_size); });
        require(std::filesystem::is_empty(quota), "byte quota failure created files");
        expect(Fault::invalid, [&] { tx->copy_physical_snapshot(owner, name(collision), limit); });
        require(!std::filesystem::exists(name(collision)), "checkpoint collision created base data");

        uint32_t calls{};
        expect(Fault::cancelled, [&] {
            tx->copy_physical_snapshot(owner, name(cancelled), limit, [&] {
                if (++calls == 5) throw Error(Fault::cancelled, "fixture cancelled during copy");
            });
        });
        require(calls == 5, "midstream cancellation checkpoint was not observed");

        std::promise<void> started;
        auto pending = started.get_future();
        std::exception_ptr failure;
        std::atomic_bool committed{};
        std::jthread publication([&] {
            try
            {
                started.set_value();
                auto write_tx = engine.begin(writer, 5s);
                write_tx->update(writer, row, later);
                require(write_tx->commit(writer) == 3, "overlap did not publish afterward");
                committed.store(true);
            }
            catch (...) { failure = std::current_exception(); }
        });
        pending.wait();
        // Avoid joining the waiting writer until the owner explicitly releases the read lease.
        try
        {
            snapshot = tx->copy_physical_snapshot(owner, name(backup), limit, [&] {
                require(!committed.load(), "writer published inside snapshot lock");
            });
            require(!committed.load(), "writer published before the read lease was released");
        }
        catch (...)
        {
            tx->rollback(owner);
            throw;
        }
        tx->rollback(owner);
        publication.join();
        if (failure) std::rethrow_exception(failure);
        require(committed.load(), "writer never finished after snapshot lease");
    }

    require(snapshot.header.lsn == 2 && snapshot.checkpoint.published == 2 &&
        snapshot.checkpoint.retain_from == 1 && snapshot.files.size() == 4,
        "physical snapshot provenance lost data, archived/active WAL or checkpoint");
    uint64_t bytes{};
    for (const auto& file : snapshot.files) bytes += std::filesystem::file_size(file);
    require(bytes == snapshot.bytes && snapshot.files.at(1) == segment_path(name(backup), 1) &&
        snapshot.files.at(2) == segment_path(name(backup), 2), "snapshot path or exact byte inventory differs");

    std::ifstream marker(checkpoint_collision);
    std::string contents;
    std::getline(marker, contents);
    require(contents == "preserve existing data", "checkpoint collision modified pre-existing data");

    const auto restored = verify_database(name(backup));
    require(restored.header.lsn == 2 && restored.header.pages == snapshot.header.pages &&
        restored.integrity.rows == 1 && restored.transactions == 2, "restored snapshot verification disagreed");
    {
        Engine reopened(name(backup));
        auto tx = reopened.begin(owner, 0ms);
        require(tx->get(owner, row) == consistent, "restore did not contain the quiesced snapshot row");
    }
    {
        auto tx = engine.begin(owner, 0ms);
        require(tx->get(owner, row) == later, "source commit after snapshot is absent");
        tx->update(owner, row, original);
        expect(Fault::invalid, [&] { tx->copy_physical_snapshot(owner, name(dirty), limit); });
        require(std::filesystem::is_empty(dirty), "dirty read snapshot created files");
    }
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) return 2;
        const std::filesystem::path scratch(argv[1]);
        const auto unique = scratch / ("snapshot-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(unique);
        exercise(unique);
        std::cout << "Exclusive quiesced physical snapshot, archived+active WAL, exact provenance, restore and failures passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

