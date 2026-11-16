#include "session.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace
{
using namespace sdp;
using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path isolated_store(const std::filesystem::path& folder)
{
    std::filesystem::create_directories(folder);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned i = 0; i < 100; ++i)
    {
        const auto directory = folder / (std::to_string(stamp) + "-" + std::to_string(i));
        if (std::filesystem::create_directory(directory))
        {
            const auto store = directory / "store.sdp";
            disk::Store::initialize(store);
            return store;
        }
    }
    throw std::runtime_error("Could not reserve isolated store");
}

wire::Json call(sql::Session& session, std::string_view op, wire::Json body)
{
    auto response = session.dispatch(1, op, body, "sql-test");
    if (!response.at("ok").get<bool>()) throw std::runtime_error(response.at("error").dump());
    return response.at("body");
}

void seed_sdk_expiry(const std::filesystem::path& path)
{
    require(path.filename() == "store.sdp" && !std::filesystem::exists(path),
        "Expiry fixture requires a fresh store.sdp");
    disk::Store::initialize(path);
    const auto recorded = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch() - 48h).count();
    const auto clock = [&] { return recorded; };
    const wire::Json::string_t token = "2ad57e0bc4aac915a1ed08953f7b6024";
    const wire::Json outcome{{"request_token", token}};
    {
        disk::Engine engine(path);
        sql::Session writer(engine, 1, "local-admin", 5s, clock);
        call(writer, "tx_begin", wire::Json::object());
        require(call(writer, "tx_commit", outcome).at("lsn") == "1", "Fixture ledger commit failed");
    }
    {
        disk::Engine engine(path);
        sql::Session owner(engine, 1, "local-admin", 5s, clock);
        require(call(owner, "tx_outcome", outcome).at("status") == "committed",
            "Expected a persisted outcome before the fixture expiry");
    }
    std::cout << token << '\n';
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 3 && std::string_view(argv[1]) == "--seed-sdk-expiry")
        {
            seed_sdk_expiry(argv[2]);
            return 0;
        }
        if (argc != 2) return 2;
        const auto path = isolated_store(argv[1]);
        int64_t now = 2'000'000'000;
        const auto clock = [&] { return now; };
        const wire::Json::string_t token = "2ad57e0bc4aac915a1ed08953f7b6024";
        const wire::Json outcome{{"request_token", token}};
        {
            disk::Engine engine(path);
            sql::Session writer(engine, 1, "fixture", 5s, clock);
            const auto statement = call(writer, "query.prepare", {{"sql", "CREATE TABLE durable (id BIGINT)"}});
            call(writer, "tx_begin", wire::Json::object());
            call(writer, "execute", {{"statement_id", statement.at("statement_id")}});
            const auto committed = call(writer, "tx_commit", outcome);
            require(committed.at("lsn") == "1", "Commit outcome LSN mismatch");
        }
        {
            disk::Engine engine(path);
            sql::Session owner(engine, 1, "fixture", 5s, clock);
            sql::Session other(engine, 2, "unrelated", 5s, clock);
            require(call(owner, "tx_outcome", outcome).at("status") == "committed",
                "Outcome missing after restart");
            require(call(other, "tx_outcome", outcome).at("status") == "unknown",
                "Outcome disclosed to a different actor");
            const auto history = engine.history();
            require(history.size() == 1 && history.front().changes.size() == 2,
                "Catalog mutation and ledger must share one committed frame");
            require(history.front().changes.front().kind == disk::ChangeKind::catalog,
                "Durable catalog mutation missing from journal");
            now += 24 * 60 * 60 + 1;
            require(call(owner, "tx_outcome", outcome).at("status") == "unknown",
                "Expired ledger outcome was returned");
            require(engine.current().lsn == 1, "Read and expiry changed committed storage");
        }
        std::cout << "Durable atomic ledger, actor visibility and expiry verified\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
