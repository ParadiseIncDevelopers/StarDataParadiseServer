#include "transactions.hpp"

#include <windows.h>
#include <array>
#include <chrono>
#include <iostream>

using namespace sdp::disk;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

constexpr RowId primary = (uint64_t{1} << 32) | 1;
constexpr RowId secondary = (uint64_t{1} << 32) | 2;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

Row first() { return {1, {int64_t{10}}}; }
Row grown() { return {2, {int64_t{11}, Bytes(8200, 0xa7)}}; }

void initialize(const std::filesystem::path& data)
{
    Store::initialize(data);
    Engine engine(data);
    auto edit = engine.begin(1, 100ms);
    require(edit->insert(1, first()) == primary && edit->commit(1) == 1, "fixture initialization failed");
}

void write(const std::filesystem::path& data, Hooks hooks)
{
    Engine engine(data, std::move(hooks));
    auto transaction = engine.begin(1, 100ms);
    transaction->update(1, primary, grown());
    require(transaction->insert(1, Row{3, {Bytes(2048, 0xf0)}}, ChangeKind::catalog) == secondary,
        "fixture second row ID changed");
    require(transaction->commit(1) == 2, "fixture commit did not publish");
}

void resume(const std::filesystem::path& data)
{
    Engine engine(data);
    auto transaction = engine.begin(1, 100ms);
    transaction->update(1, primary, {4, {int64_t{12}}});
    require(transaction->commit(1) == 3, "fixture commit after checkpoint failed");
}

void verify(const std::filesystem::path& data, unsigned revision)
{
    const auto start = std::chrono::steady_clock::now();
    const auto result = verify_database(data, true, std::array{primary});
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
    require(result.header.lsn == revision && result.transactions == revision,
        "recovery found the wrong durable transaction");
    Engine engine(data);
    const auto history = engine.history();
    require(history.size() == revision && history.front().changes.size() == 1 &&
        decode(history.front().changes.front().after) == first(), "history missing the original transaction");
    auto tx = engine.begin(22, 100ms);
    switch (revision)
    {
    case 1:
        require(result.integrity.rows == 1 && tx->get(22, primary) == first(),
            "uncommitted changes survived recovery");
        break;
    case 2:
        require(result.integrity.rows == 2 && tx->get(22, primary) == grown() &&
            tx->get(22, secondary) == Row{3, {Bytes(2048, 0xf0)}}, "committed rows were not repaired");
        break;
    case 3:
        require(result.integrity.rows == 2 && tx->get(22, primary) == Row{4, {int64_t{12}}},
            "post-checkpoint commit did not survive");
        break;
    default: throw std::runtime_error("unexpected test revision");
    }
    if (revision >= 2)
    {
        const auto& changes = history.at(1).changes;
        require(changes.size() == 2 && decode(changes[0].before) == first()
            && decode(changes[0].after) == grown() && changes[1].kind == ChangeKind::catalog,
            "recovery lost before/after or catalog history");
    }
    tx->rollback(22);
    std::cout << "LSN " << revision << " pages " << result.rewritten_pages
        << " tail " << result.discarded_tail << " recovery_ms " << elapsed.count() << '\n';
}

CommitPhase phase_for(std::wstring_view value)
{
    if (value == L"body"sv) return CommitPhase::body_written;
    if (value == L"durable"sv) return CommitPhase::wal_durable;
    if (value == L"page"sv) return CommitPhase::page_written;
    if (value == L"flush"sv) return CommitPhase::data_flushed;
    if (value == L"publish"sv) return CommitPhase::data_published;
    if (value == L"checkpoint-write"sv) return CommitPhase::checkpoint_written;
    if (value == L"checkpoint-publish"sv) return CommitPhase::checkpoint_published;
    throw std::runtime_error("invalid crash phase");
}

Hooks injected(std::wstring_view label)
{
    Hooks result;
    if (!label.empty())
    {
        const auto phase = phase_for(label);
        result.observe = [phase](CommitPhase reached) {
            if (phase == reached)
            {
                std::cout << "CRASH_READY" << std::endl;
                Sleep(INFINITE);
            }
        };
    }
    return result;
}

void checkpoint(const std::filesystem::path& data, Hooks hooks)
{
    Engine engine(data, std::move(hooks));
    auto live = engine.begin(24, 0ms);
    live->update(24, primary, {5, {int64_t{100}}});
    const auto saved = engine.checkpoint({2,2});
    require(saved.published == 2 && saved.retain_from == 1 && saved.reclaimable == 0
        && segments(data).size() == 2, "history or active view was not retained");
    live->rollback(24);
    require(retention(80, 75, 42, {12, 31}).retain_from == 12, "retention ignored a consumer pin");
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc < 3 || argc > 4) return 2;
        const auto data = std::filesystem::path(argv[2]);
        const auto command = std::wstring_view(argv[1]);
        const auto arg = argc == 4 ? std::wstring_view(argv[3]) : std::wstring_view{};
        if (command == L"init"sv) initialize(data);
        else if (command == L"commit"sv) write(data, injected(arg));
        else if (command == L"verify"sv) verify(data, static_cast<unsigned>(std::stoi(std::wstring(arg))));
        else if (command == L"checkpoint"sv) checkpoint(data, injected(arg));
        else if (command == L"resume"sv) resume(data);
        else if (command == L"legacy"sv) require(verify_database(data).integrity.rows == 2, "older fixture unreadable");
        else if (command == L"index"sv)
        {
            try { verify_database(data, false, std::array<RowId, 1>{12345}); }
            catch (const Error& error)
            {
                if (error.fault() == Fault::corrupt) { std::cout << "Dangling index candidate rejected\n"; return 0; }
                throw;
            }
            throw std::runtime_error("Invalid index candidate accepted");
        }
        else if (command == L"read-only"sv) verify_database(data);
        else return 2;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
