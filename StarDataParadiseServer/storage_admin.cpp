#include "storage_admin.hpp"
#include "../storage/transactions.hpp"
#include "../sql/binder.hpp"
#include "../sql/advanced.hpp"
#include "../sql/definition.hpp"
#include "../sql/relational.hpp"
#include "../sql/routines.hpp"
#include "../sql/session.hpp"
#include "../ml/training.hpp"

#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <fstream>

#include <iostream>

namespace sdp::server
{
namespace
{
using wire::Json;

class InstallationScratch
{
public:
    InstallationScratch()
    {
        std::array<unsigned char, 16> random{};
        if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
            throw StartupError("Cannot create installation test workspace");
        std::string leaf = "StarDataParadise-install-test-";
        for (const auto byte : random)
        {
            constexpr auto hex = "0123456789abcdef";
            leaf += hex[byte >> 4];
            leaf += hex[byte & 15];
        }
        root = std::filesystem::canonical(std::filesystem::temp_directory_path());
        directory = root / leaf;
        if (!std::filesystem::create_directory(directory))
            throw StartupError("Installation test workspace already exists");
    }
    ~InstallationScratch()
    {
        std::error_code error;
        const auto resolved = std::filesystem::canonical(directory, error);
        if (!error && resolved == directory && resolved.parent_path() == root)
            std::filesystem::remove_all(resolved, error);
    }
    std::filesystem::path directory;

private:
    std::filesystem::path root;
};

Json setup_call(sql::Session& session, std::string_view operation, Json body = Json::object())
{
    const auto result = session.dispatch(1, operation, body, "installation-test");
    if (!result.at("ok").get<bool>())
        throw StartupError("Installation self-test " + std::string(operation) + " failed with code " +
            std::to_string(result.at("error").at("code").get<uint32_t>()));
    return result.at("body");
}

Json setup_sql(sql::Session& session, std::string_view statement, std::string_view token = {})
{
    const auto prepared = setup_call(session, "query.prepare", {{"sql", statement}}).at("statement_id");
    Json request{{"statement_id", prepared}};
    if (!token.empty()) request["request_token"] = token;
    auto result = setup_call(session, "execute", std::move(request));
    setup_call(session, "statement_close", {{"statement_id", prepared}});
    return result;
}

void check_empty(const Config& config)
{
    for (const auto& entry : std::filesystem::directory_iterator(config.data))
    {
        const auto name = entry.path().filename();
        if (name != ".sdp.lock" && name != "admin-v1.json")
            throw StartupError("Storage initialization requires an unused data directory");
    }
}
}

void initialize_storage(const Config& config)
{
    check_empty(config);
    const DataLock lock(config);
    check_empty(config);
    disk::Store::initialize(config.data / store_name);
    std::cout << "Storage initialized\n";
}

void inspect_storage(const Config& config)
{
    const DataLock lock(config);
    const auto path = config.data / store_name;
    const auto view = disk::verify_database(path).integrity;
    disk::Store store(path, true);
    sql::verify_relational(store);
    std::cout << "Storage OK: " << view.rows << " rows, " << view.heap_pages << " heap pages, "
        << view.overflow_pages << " overflow pages, " << view.free_pages << " free pages\n";
}

void recover_storage(const Config& config)
{
    const DataLock lock(config);
    const auto report = disk::verify_database(config.data / store_name, true);
    std::cout << "Recovery OK at LSN " << report.header.lsn << ", "
        << report.rewritten_pages << " pages rewritten, "
        << (report.discarded_tail ? "uncommitted tail discarded" : "complete WAL") << '\n';
}

void checkpoint_storage(const Config& config)
{
    const DataLock lock(config);
    disk::Engine storage(config.data / store_name);
    const auto result = storage.checkpoint();
    std::cout << "Checkpoint at LSN " << result.published << ", retain from "
        << result.retain_from << ", " << result.reclaimable << " reclaimable segments\n";
}

void inspect_catalog(const Config& config)
{
    const DataLock lock(config);
    const auto path = config.data / store_name;
    disk::verify_database(path);
    disk::Store store(path, true);
    sql::Catalog catalog(store);
    auto objects = wire::Json::array();
    static constexpr const char* kinds[]{"unknown", "database", "schema", "table", "index", "view", "routine", "sequence"};
    const auto snapshot = catalog.list();
    for (const auto& object : snapshot)
    {
        auto columns = wire::Json::array();
        for (const auto& column : object.columns)
            columns.push_back({{"ordinal", column.ordinal}, {"name", column.name},
                {"type", sql::type_name(column.type)}, {"nullable", column.nullable}});
        objects.push_back({{"id", object.id}, {"parent_id", object.parent}, {"name", object.name},
            {"qualified_name", sql::Catalog::qualified(object, snapshot).parts}, {"kind", kinds[static_cast<size_t>(object.kind)]},
            {"schema_version", object.version}, {"columns", std::move(columns)},
            {"depends_on", sql::object_dependencies(object)}, {"definition", object.definition}});
        if (const auto collation = sql::catalog_index_collation(object))
            objects.back()["collation"] = {{"name", collation->name}, {"version", collation->version}, {"stale", collation->stale}};
    }
    std::cout << wire::Json{{"objects", std::move(objects)}}.dump() << '\n';
}

void check_sql(const Config& config, const std::filesystem::path& source)
{
    std::ifstream file(source, std::ios::binary);
    std::string input(64 * 1024 + 1, '\0');
    file.read(input.data(), static_cast<std::streamsize>(input.size()));
    if (file.bad() || (!file.eof() && !file)) throw StartupError("Cannot read SQL source file");
    input.resize(static_cast<size_t>(file.gcount()));

    const DataLock lock(config);
    const auto path = config.data / store_name;
    disk::verify_database(path);
    disk::Store store(path, true);
    sql::Catalog catalog(store);
    const auto plan = sql::check(catalog, input);
    wire::Json result{{"compatible", true}, {"kind", std::holds_alternative<sql::MutationPlan>(plan) ? "mutation" : "ddl"}};
    if (const auto* select = std::get_if<sql::SelectPlan>(&plan))
    {
        result["kind"] = "select";
        result["columns"] = wire::Json::array();
        for (const auto& column : select->projection)
            result["columns"].push_back({{"name", column.column}, {"type", sql::type_name(column.expression.type)}});
    }
    if (const auto* advanced = std::get_if<sql::AdvancedSelection>(&plan))
    {
        result["kind"] = "select";
        result["columns"] = wire::Json::array();
        if (advanced->mode == sql::QueryMode::execute)
            for (const auto& column : sql::advanced_columns(*advanced->plan))
                result["columns"].push_back({{"name", column.column}, {"type", sql::type_name(column.expression.type)}});
        else result["columns"].push_back({{"name", "plan"}, {"type", "TEXT"}});
    }
    if (const auto* routine = std::get_if<std::shared_ptr<const sql::RoutineSelection>>(&plan))
    {
        result["kind"] = "routine";
        result["columns"] = wire::Json::array();
        for (const auto& column : sql::routine_columns(**routine))
            result["columns"].push_back({{"name", column.column}, {"type", sql::type_name(column.expression.type)}});
    }
    std::cout << result.dump() << '\n';
}

void check_storage_on_start(const Config& config)
{
    const auto path = config.data / store_name;
    if (std::filesystem::symlink_status(path).type() == std::filesystem::file_type::not_found)
    {
        auto checkpoint = path;
        checkpoint += ".checkpoint";
        if (!disk::segments(path).empty() ||
            std::filesystem::symlink_status(checkpoint).type() != std::filesystem::file_type::not_found)
            throw StartupError("Storage file is missing for retained WAL or checkpoint");
        return;
    }
    disk::verify_database(path, true);
}

void check_package_config(const Config& config)
{
    const DataLock lock(config);
    const auto path = config.data / store_name;
    disk::verify_database(path);
    disk::Store store(path, true);
    sql::verify_relational(store);
    const auto objects = sql::Catalog(store).list();
    const auto rules = std::ranges::count(objects, sql::ObjectKind::decision, &sql::Object::kind);
    if (!config.decision_enabled && rules)
        throw StartupError("Active Decision rules require the Decision module");
    std::cout << Json{{"valid", true}, {"active_decision_rules", rules},
        {"decision_enabled", config.decision_enabled}, {"ml_cpu_enabled", config.ml_cpu_enabled},
        {"mlp_cpu_enabled", config.ml_cpu_enabled && config.mlp_cpu_enabled && ml::mlp_compiled()}}.dump() << '\n';
}

void verify_installation()
{
    using namespace std::chrono_literals;
    const InstallationScratch scratch;
    const auto path = scratch.directory / store_name;
    disk::Store::initialize(path);
    disk::Engine storage(path);
    Admin bootstrap;
    bootstrap.username = "setup-self-test";
    sql::Security security(bootstrap);
    {
        auto tx = storage.begin(90, 0ms);
        security.publish(security.snapshot(*tx, 90), storage.current().lsn);
    }
    const auto principal = security.active_principal(bootstrap.username);
    if (!principal || !principal->administrator) throw StartupError("Installed security self-test failed");
    sql::Session session(storage, 91, principal->name, 5s, {}, scratch.directory, &security, *principal);
    setup_sql(session, "CREATE TABLE setup_health (id BIGINT PRIMARY KEY)", "00000000000000000000000000000001");
    setup_call(session, "tx_begin");
    setup_sql(session, "INSERT INTO setup_health VALUES (1)");
    setup_call(session, "tx_rollback");
    setup_call(session, "tx_begin");
    setup_sql(session, "INSERT INTO setup_health VALUES (7)");
    setup_call(session, "tx_commit", {{"request_token", "00000000000000000000000000000002"}});
    const auto result = setup_sql(session, "SELECT id FROM setup_health");
    const auto batch = setup_call(session, "fetch", {{"result_id", result.at("result_id")}, {"batch_rows", 2u}}).at("batch");
    const Json expected = Json::array({Json::array({Json{{"t", "i64"}, {"v", "7"}}})});
    if (batch.at("rows") != expected || batch.at("end_of_results") != true ||
        setup_call(session, "history_verify").at("verified") != true)
        throw StartupError("Installed data or History self-test failed");
    std::cout << Json{{"sql", true}, {"transaction", true}, {"history", true},
        {"mlp_compiled", ml::mlp_compiled()}, {"gpu_available", false}}.dump() << '\n';
}
}
