#include "advanced.hpp"
#include "collation.hpp"
#include "session.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>

namespace
{
using namespace sdp;
using namespace std::chrono_literals;
using wire::Json;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path fixture(const std::filesystem::path& folder)
{
    std::filesystem::create_directories(folder);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int i = 0; i < 100; ++i)
    {
        const auto unique = folder / (std::to_string(stamp) + "-" + std::to_string(i));
        if (std::filesystem::create_directory(unique)) return unique;
    }
    throw std::runtime_error("Could not reserve query fixture path");
}

std::string token()
{
    static unsigned counter{};
    char text[33]{};
    std::snprintf(text, sizeof(text), "%032x", ++counter);
    return text;
}

Json call(sql::Session& session, std::string_view op, Json body)
{
    auto response = session.dispatch(1, op, body, "query-test");
    if (!response.at("ok").get<bool>()) throw std::runtime_error(response.at("error").dump());
    return response.at("body");
}

Json execute(sql::Session& session, std::string_view query, bool write = false)
{
    const auto prepared = session.dispatch(1, "query.prepare", {{"sql", query}}, "query-test");
    if (!prepared.at("ok").get<bool>()) return prepared;
    const auto id = prepared.at("body").at("statement_id");
    Json args{{"statement_id", id}};
    if (write) args["request_token"] = token();
    auto response = session.dispatch(2, "execute", args, "query-test");
    call(session, "statement_close", {{"statement_id", id}});
    return response;
}

void write(sql::Session& session, std::string_view ddl)
{
    const auto packet = execute(session, ddl, true);
    if (!packet.at("ok").get<bool>())
        throw std::runtime_error(std::string(ddl.substr(0, 132)) + " -> " + packet.at("error").dump());
}

Json query(sql::Session& session, std::string_view source)
{
    auto response = execute(session, source);
    if (!response.at("ok").get<bool>())
        throw std::runtime_error(std::string(source) + " -> " + response.at("error").dump());

    const auto id = response.at("body").at("result_id");
    Json out = Json::array();
    for (;;)
    {
        const auto batch = call(session, "fetch", {{"result_id", id}, {"batch_rows", 31u}}).at("batch");
        for (const auto& row : batch.at("rows")) out.push_back(row);
        if (batch.at("end_of_results").get<bool>()) return out;
    }
}

void test_queries(disk::Engine& engine, sql::Session& session)
{
    write(session, "CREATE TABLE divisions (id BIGINT PRIMARY KEY, label TEXT)");
    write(session, "CREATE TABLE work (id BIGINT PRIMARY KEY, division BIGINT, points BIGINT)");
    write(session, "INSERT INTO divisions VALUES (1,'north'),(2,'south'),(3,'east')");
    write(session, "INSERT INTO work VALUES (1,1,10),(2,1,14),(3,2,3),(4,2,5),(5,2,8)");
    write(session, "ANALYZE work");
    auto grouped = query(session, "SELECT d.label, COUNT(*), SUM(w.points) FROM divisions AS d JOIN work AS w "
        "ON d.id = w.division GROUP BY d.label HAVING SUM(w.points) > 10 ORDER BY d.label");
    require(grouped.size() == 2 &&
        grouped[0][0]["v"] == "north" && grouped[0][1]["v"] == "2" && grouped[0][2]["unscaled"] == "24" && grouped[0][2]["scale"] == 0 &&
        grouped[1][0]["v"] == "south" && grouped[1][1]["v"] == "3" && grouped[1][2]["unscaled"] == "16" && grouped[1][2]["scale"] == 0,
        "Join, grouped aggregate or HAVING differs from expected fixture");
    const auto left_join = query(session, "SELECT d.label,w.points FROM divisions d LEFT JOIN work w "
        "ON d.id = w.division WHERE d.id = 3");
    require(left_join.size() == 1 && left_join[0][1]["t"] == "null", "Left join lost unmatched row");
    auto filtered = query(session, "SELECT id FROM work WHERE EXISTS (SELECT id FROM divisions WHERE id = 3) "
        "AND points = (SELECT MAX(points) FROM work)");
    require(filtered.size() == 1 && filtered[0][0]["v"] == "2", "EXISTS or scalar query returned wrong result");
    auto combined = query(session, "WITH candidates AS (SELECT division FROM work) "
        "SELECT DISTINCT division FROM candidates UNION SELECT division FROM work WHERE division = 2 ORDER BY division");
    require(combined.size() == 2 && combined[0][0]["v"] == "1" && combined[1][0]["v"] == "2",
        "CTE, DISTINCT or UNION returned wrong rows");

    const auto before = engine.current().lsn;
    const auto source = "SELECT d.label, w.points FROM divisions d JOIN work w ON d.id = w.division";
    auto plain = Json::parse(query(session, std::string("EXPLAIN ") + source)[0][0]["v"].get<std::string>());
    auto measured = Json::parse(query(session, std::string("EXPLAIN ANALYZE ") + source)[0][0]["v"].get<std::string>());
    require(plain.is_object() && !plain.empty() && !plain.contains("elapsed_us") &&
        measured["actual_rows"] == 5 && measured["elapsed_us"].is_number_integer(), "Plan or actual profile is wrong");
    require(engine.current().lsn == before, "EXPLAIN changed persistent data");
}

void test_collation(disk::Engine& engine, sql::Session& session)
{
    require(sql::compare_collated("I", "ı", "turkish") == 0 &&
        sql::compare_collated("İ", "i", "tr-TR") == 0 &&
        sql::compare_collated("ı", "i", "tr-TR") < 0, "Turkish dotted/dotless I mismatch");
    write(session, "CREATE TABLE words (label TEXT)");
    write(session, "INSERT INTO words VALUES ('i'),('İ'),('I'),('ı')");
    write(session, "CREATE INDEX words_tr ON words (label) COLLATE turkish");
    auto sorted = query(session, "SELECT label FROM words ORDER BY label COLLATE turkish");
    require(sorted.size() == 4 && sql::compare_collated(sorted[0][0]["v"].get<std::string>(), "I", "turkish") == 0 &&
        sql::compare_collated(sorted[3][0]["v"].get<std::string>(), "i", "turkish") == 0,
        "Locale-aware ORDER BY did not use Turkish ordering");

    sql::Object index;
    {
        auto tx = engine.begin(2, 0ms);
        sql::Catalog catalog(*tx, 2);
        index = catalog.resolve(sql::ObjectKind::index, {{"words_tr"}});
        auto stale_definition = Json::parse(index.definition);
        stale_definition["sort_version"] = "fixture-incompatible-version";
        catalog.update_definition(index, stale_definition.dump());
        tx->commit(2);
    }
    require(!execute(session, "INSERT INTO words VALUES ('extra')", true).at("ok").get<bool>(),
        "Mutating an index with a different sort version was permitted");
    write(session, "REINDEX words_tr COLLATE binary");
    auto tx = engine.begin(2, 0ms);
    sql::Catalog catalog(*tx, 2);
    const auto reindexed = catalog.resolve(sql::ObjectKind::index, {{"words_tr"}});
    require(reindexed.id == index.id && reindexed.version == index.version + 1,
        "REINDEX changed object id or failed to advance metadata revision");
    require(sql::catalog_index_collation(reindexed)->name == "binary", "REINDEX kept old collation");
}

void write_payloads(sql::Session& session, int blocks)
{
    for (int block = 0; block < blocks; ++block)
    {
        std::string insert = "INSERT INTO data_text VALUES ";
        for (int i = 0; i < 20; ++i)
        {
            if (i) insert += ',';
            insert += "('" + std::to_string(block) + "_" + std::to_string(i) + "_" + std::string(2096, 'a') + "')";
        }
        write(session, insert);
    }
}

void test_budget(disk::Engine& engine, sql::Session& session, const std::filesystem::path& temporary)
{
    write(session, "CREATE TABLE data_text (payload TEXT)");
    write_payloads(session, 2);
    require(query(session, "SELECT payload FROM data_text ORDER BY payload").size() == 40,
        "Temporary sorted result lost rows");
    require(std::filesystem::is_empty(temporary), "Completed query left a temporary file");
    {
        auto tx = engine.begin(2, 0ms);
        sql::Catalog catalog(*tx, 2);
        const auto plan = sql::plan_advanced(catalog, sql::parse("SELECT payload FROM data_text ORDER BY payload"), {});
        bool limited{};
        try { (void)sql::run_advanced(*plan, *tx, 2, {}, {}, 512, 10); }
        catch (const sql::Error& error) { limited = error.code() == sql::Code::limit; }
        require(limited, "Query memory cap did not reject the fixture");
    }
    write_payloads(session, 18);
    require(!execute(session, "SELECT payload FROM data_text ORDER BY payload").at("ok").get<bool>(),
        "Temporary spill quota did not reject the fixture");
    require(std::filesystem::is_empty(temporary), "Overflowing temporary result leaked a temporary file");
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) return 2;
        const auto dir = fixture(argv[1]);
        const auto temporary = dir / "temporary";
        std::filesystem::create_directory(temporary);
        const auto file = dir / "store.sdp";
        disk::Store::initialize(file);
        disk::Engine engine(file);
        sql::Session session(engine, 1, "query-test", 20s, {}, temporary);
        test_queries(engine, session);
        test_collation(engine, session);
        test_budget(engine, session, temporary);
        std::cout << "Advanced SQL, measured plans, Turkish collation and query budgets verified\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
