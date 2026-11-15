#include "session.hpp"

#include "relational.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <latch>
#include <thread>

namespace
{
using namespace sdp;
using wire::Json;
using namespace std::chrono_literals;
std::atomic_uint32_t request{0};

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

std::string token()
{
    char text[33]{};
    std::snprintf(text, sizeof(text), "%032x", ++request);
    return text;
}

Json call(sql::Session& session, std::string_view op, Json body = Json::object())
{
    const auto packet = session.dispatch(1, op, body, "relations-test");
    if (!packet.at("ok").get<bool>()) throw std::runtime_error(packet.at("error").dump());
    return packet.at("body");
}

Json run(sql::Session& session, std::string_view query, bool write = true, bool autocommit = true)
{
    auto prepared = call(session, "query.prepare", {{"sql", query}});
    const auto id = prepared.at("statement_id");
    Json args{{"statement_id", id}, {"row_limit", 4096u}};
    if (write && autocommit) args["request_token"] = token();
    auto response = session.dispatch(2, "execute", args, "relations-test");
    call(session, "statement_close", {{"statement_id", id}});
    return response;
}

void write(sql::Session& session, std::string_view query, bool autocommit = true)
{
    const auto packet = run(session, query, true, autocommit);
    if (!packet.at("ok").get<bool>())
        throw std::runtime_error(std::string(query) + " -> " + packet.at("error").dump());
}

void rejects(sql::Session& session, std::string_view query, bool autocommit = true)
{
    const auto prepared = session.dispatch(1, "query.prepare", {{"sql", query}}, "relations-test");
    if (!prepared.at("ok").get<bool>()) return;
    const auto id = prepared.at("body").at("statement_id");
    Json args{{"statement_id", id}, {"row_limit", 4096u}};
    if (autocommit) args["request_token"] = token();
    const auto packet = session.dispatch(2, "execute", args, "relations-test");
    call(session, "statement_close", {{"statement_id", id}});
    require(!packet.at("ok").get<bool>(), "Invalid relational write succeeded");
}

Json rows(sql::Session& session, std::string_view query)
{
    const auto packet = run(session, query, false);
    if (!packet.at("ok").get<bool>())
        throw std::runtime_error(std::string(query) + " -> " + packet.at("error").dump());
    const auto id = packet.at("body").at("result_id");
    auto received = Json::array();
    while (true)
    {
        const auto batch = call(session, "fetch", {{"result_id", id}, {"batch_rows", 13u}}).at("batch");
        for (const auto& row : batch.at("rows")) received.push_back(row);
        if (batch.at("end_of_results").get<bool>()) return received;
    }
}

void expect_plan(disk::Engine& engine, std::string_view query, bool indexed)
{
    auto transaction = engine.begin(3, 0ms);
    sql::Catalog catalog(*transaction, 3);
    auto plan = std::get<sql::SelectPlan>(sql::check(catalog, query));
    sql::Cursor cursor(std::move(plan), {}, 100);
    const auto data = cursor.fetch(*transaction, 3, 100, 128 * 1024);
    require(!data.rows.empty() && cursor.using_index() == indexed, "Unexpected access path or empty result");
}

void prepare_fixture(const std::filesystem::path& path)
{
    disk::Engine engine(path);
    sql::Session session(engine, 1, "relations-test", 20s);
    write(session, "CREATE TABLE inventory (id BIGINT PRIMARY KEY, code TEXT UNIQUE, quantity BIGINT CHECK (quantity >= 0))");
    std::string insert = "INSERT INTO inventory VALUES ";
    for (int i = 1; i <= 120; ++i)
    {
        if (i > 1) insert += ',';
        insert += "(" + std::to_string(i) + ",'code" + std::to_string(i) + "'," + std::to_string(i % 3) + ")";
    }
    write(session, insert);
    constexpr auto equality = "SELECT id, code FROM inventory WHERE id = 117";
    expect_plan(engine, equality, false);
    const auto earlier = rows(session, equality);
    require(earlier.size() == 1 && earlier[0][1]["v"] == "code117", "Table scan returned wrong row");
    write(session, "CREATE INDEX inventory_id ON inventory (id)");
    expect_plan(engine, equality, true);
    require(rows(session, "SELECT id, code FROM inventory WHERE id = 117") == earlier, "Index returned different row");
    write(session, "ALTER INDEX inventory_id RENAME TO inventory_seek");
    write(session, "CREATE VIEW inventory_public AS SELECT id,code FROM inventory");
    const auto view = rows(session, "SELECT code FROM inventory_public WHERE id = 117");
    require(view.size() == 1 && view[0][0]["v"] == "code117", "Persistent view lost a projected column");
    write(session, "CREATE SEQUENCE tickets");
    write(session, "CREATE TABLE reservation (id BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, "
        "item BIGINT REFERENCES inventory (id), ticket BIGINT DEFAULT NEXT VALUE FOR tickets, note TEXT DEFAULT 'new')");
    write(session, "INSERT INTO reservation (item) VALUES (117),(118)");
    const auto defaults = rows(session, "SELECT id,ticket,note FROM reservation WHERE item = 118");
    require(defaults.size() == 1 && defaults[0][0]["v"] == "2" && defaults[0][1]["v"] == "2" &&
        defaults[0][2]["v"] == "new", "Identity, sequence or default not applied");
    rejects(session, "DROP TABLE inventory");
    rejects(session, "DROP SEQUENCE tickets");
    rejects(session, "INSERT INTO reservation (item) VALUES (1000)");
    rejects(session, "UPDATE inventory SET id = 444 WHERE id = 117");
    rejects(session, "INSERT INTO inventory VALUES (200,'code200',1),(201,'bad',-1)");
    require(rows(session, "SELECT id FROM inventory WHERE id = 200").empty(), "A failed CHECK left a partial insert");
    rejects(session, "CREATE TABLE deferred_ref (item BIGINT REFERENCES inventory (id) DEFERRABLE)");
}

void verify_restart(const std::filesystem::path& path)
{
    disk::Engine engine(path);
    sql::Session first(engine, 1, "relations-test", 20s);
    sql::Session second(engine, 2, "relations-test", 20s);
    constexpr auto range = "SELECT id,code FROM inventory WHERE id >= 118";
    expect_plan(engine, range, true);
    require(rows(first, range).size() == 3,
        "Range query failed after restart");
    auto pa = call(first, "query.prepare", {{"sql", "INSERT INTO inventory VALUES (400,'race',0)"}}).at("statement_id");
    auto pb = call(second, "query.prepare", {{"sql", "INSERT INTO inventory VALUES (401,'race',0)"}}).at("statement_id");
    std::latch start(2);
    std::atomic_uint32_t successes{};
    auto compete = [&](sql::Session& session, const Json& id) {
        start.arrive_and_wait();
        auto packet = session.dispatch(2, "execute", {{"statement_id", id}, {"request_token", token()}},
            "relations-race");
        if (packet.at("ok").get<bool>()) ++successes;
    };
    std::thread a(compete, std::ref(first), std::cref(pa));
    std::thread b(compete, std::ref(second), std::cref(pb));
    a.join();
    b.join();
    require(successes == 1 && rows(first, "SELECT id FROM inventory WHERE code = 'race'").size() == 1,
        "Concurrent UNIQUE enforcement did not serialize writes");
    write(first, "INSERT INTO inventory VALUES (500,NULL,0),(501,NULL,0)");
    write(first, "UPDATE inventory SET id = 560 WHERE id = 500");
    require(rows(first, "SELECT id FROM inventory WHERE id = 500").empty() &&
        rows(first, "SELECT id FROM inventory WHERE id = 560").size() == 1, "PK update was not indexed");

    call(first, "tx_begin");
    write(first, "CREATE INDEX discard_index ON inventory (code)", false);
    write(first, "CREATE TABLE discard_table (id BIGINT)", false);
    call(first, "tx_rollback");
    const auto metadata = call(first, "metadata").at("objects");
    for (const auto& object : metadata)
    {
        const auto last = object.at("qualified_name").back();
        require(last != "discard_index" && last != "discard_table", "DDL rollback left a catalog object");
    }

    write(first, "CREATE TABLE keep_history (id BIGINT PRIMARY KEY, note TEXT)");
    write(first, "INSERT INTO keep_history VALUES (1,'before truncate'),(2,'before drop')");
    write(first, "TRUNCATE TABLE keep_history");
    require(rows(first, "SELECT * FROM keep_history").empty(), "TRUNCATE left visible rows");
    write(first, "INSERT INTO keep_history VALUES (3,'before drop')");
    write(first, "DROP TABLE keep_history");
    bool retained_truncate{}, retained_drop{};
    for (const auto& revision : engine.history())
        for (const auto& edit : revision.changes)
        {
            if (edit.kind != disk::ChangeKind::row || edit.before.empty() || !edit.after.empty()) continue;
            for (const auto& value : disk::decode(edit.before).values)
            {
                if (const auto* str = std::get_if<std::string>(&value))
                {
                    retained_truncate |= *str == "before truncate";
                    retained_drop |= *str == "before drop";
                }
            }
        }
    require(retained_truncate && retained_drop, "DDL lost historical before-images");
    write(first, "DELETE FROM inventory WHERE id <= 100");
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) return 2;
        const auto file = isolated_store(argv[1]);
        prepare_fixture(file);
        verify_restart(file);
        sdp::disk::Store read_only(file, true);
        sdp::sql::verify_relational(read_only);
        std::cout << "Relational indexes, constraints and transactional DDL verified\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
