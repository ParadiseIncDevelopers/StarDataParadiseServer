#include "model.hpp"
#include "history_journal.hpp"
#include "linear_models.hpp"
#include "../backup/archive.hpp"

#include <chrono>
#include <iostream>
#include <thread>

namespace
{
using namespace sdp;
using namespace std::chrono_literals;
using wire::Json;
constexpr disk::Owner owner = 19;
const sql::Principal actor{1, "tester", true};
const sql::Name name{{"model_fixture"}, {}};

void require(bool pass, const char* detail)
{
    if (!pass) throw std::runtime_error(detail);
}

template<typename Function>
uint64_t edit(disk::Engine& engine, const Function& function)
{
    auto tx = engine.begin(owner, 0ms);
    sql::Catalog catalog(*tx, owner);
    sql::HistoryJournal history(engine, *tx, owner);
    function(*tx, catalog, history);
    history.seal(actor.name, 1'699'999'999);
    return tx->commit(owner);
}

template<typename Function>
void require_invalid(const Function& function)
{
    try { function(); }
    catch (const wire::Error& error)
    {
        require(error.code() == wire::Code::invalid_state, "unexpected wire error");
        return;
    }
    throw std::runtime_error("expected invalid state");
}

Json artifact(uint64_t seed)
{
    const std::vector<std::vector<double>> xs{{0}, {1}, {2}, {3}};
    const std::vector<double> ys{1, 3, 5, 7};
    return {{"format", 1}, {"algorithm", "linear"}, {"width", 1u}, {"seed", seed},
         {"artifact", ml::fit_linear_model("linear", xs, ys, Json::object())},
         {"hardware", {{"backend", "cpu"}}},
         {"metrics", {{"train", Json::object()}, {"validation", Json::object()}, {"test", Json::object()}}}};
}

constexpr std::string_view backup_id = "28a0faf54f0e4a4ba91b49ad9122ca83";
constexpr std::string_view restore_name = "crash-fixture-restore";
constexpr uint64_t backup_limit = 16 * 1024 * 1024;

void await_kill()
{
    std::cout << "CRASH_READY" << std::endl;
    std::this_thread::sleep_for(30s);
    throw std::runtime_error("Test harness did not stop its child process");
}

void seed_crash_fixture(const std::filesystem::path& root)
{
    require(std::filesystem::is_empty(root), "Crash fixture must start in an empty directory");
    require(std::filesystem::create_directory(root / "archives") &&
        std::filesystem::create_directory(root / "temp"), "Crash fixture directory initialization failed");
    const auto store = root / "store.sdp";
    disk::Store::initialize(store);
    disk::Engine engine(store);
    sql::ObjectId id{};
    require(edit(engine, [&](auto&, auto& catalog, auto&) {
        const Json model{{"format", 1}, {"algorithm", "linear"}, {"owner", actor.name},
            {"active", nullptr}, {"versions", Json::array()}};
        id = catalog.create_auxiliary(sql::ObjectKind::model, name, model.dump()).id;
    }) == 1, "Model catalog seed did not commit");
    require(edit(engine, [&](auto& tx, auto&, auto&) {
        require(ml::publish_model(tx, owner, id, "dataset", "run1", artifact(17), Json::object()).at("version") == 1,
            "Model version seed did not publish");
    }) == 2, "Model version seed did not commit");
    require(edit(engine, [&](auto&, auto& catalog, auto&) {
        auto model = ml::require_model(catalog, name, actor);
        ml::activate_model(catalog, model, 1);
    }) == 3, "Model activation seed did not commit");
}

void interrupt_model_publication(const std::filesystem::path& store, disk::CommitPhase phase)
{
    disk::Hooks hooks;
    hooks.observe = [phase](disk::CommitPhase reached) { if (reached == phase) await_kill(); };
    disk::Engine engine(store, std::move(hooks));
    edit(engine, [&](auto& tx, auto& catalog, auto&) {
        const auto model = ml::require_model(catalog, name, actor);
        require(ml::publish_model(tx, owner, model.object.id, "dataset", "run2", artifact(22), Json::object())
            .at("version") == 2, "Second model version did not stage");
    });
    throw std::runtime_error("Model publication completed without an interruption");
}

void verify_model(const std::filesystem::path& store, uint64_t versions)
{
    disk::Engine engine(store);
    const auto header = engine.current();
    auto tx = engine.begin(owner, 0ms);
    sql::Catalog catalog(*tx, owner);
    sql::HistoryJournal history(engine, *tx, owner);
    const auto model = ml::require_model(catalog, name, actor);
    const auto revision = versions + 2;
    require(header.lsn == revision && history.verify().head == revision && history.read(revision).sha256.size() == 64,
        "Model recovery and sealed History disagree on the latest durable revision");
    require(ml::model_status(model).at("versions").size() == versions && ml::model_version(model, 0).at("version") == 1,
        "Model recovery changed the version count or the prior active version");
    require(ml::load_model(*tx, owner, model, ml::model_version(model, 1)) == artifact(17),
        "Original active model payload changed");
    if (versions == 2)
    {
        require(ml::load_model(*tx, owner, model, ml::model_version(model, 2)) == artifact(22) &&
            !history.read(revision).changes.empty(), "Durable model version or its History changes were lost");
    }
    std::cout << "MODEL versions=" << versions << " active=1 history=" << revision << '\n';
}

bool staged_file(const std::filesystem::path& root, std::string_view prefix, const char* name)
{
    if (!std::filesystem::is_directory(root)) return false;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        if (entry.path().filename().string().starts_with(prefix) && std::filesystem::is_regular_file(entry.path() / name))
            return true;
    return false;
}

void exercise_archive(const std::filesystem::path& root, std::string_view mode)
{
    disk::Engine engine(root / "store.sdp");
    const auto archives = root / "archives";
    const auto temp = root / "temp";
    backup::Archive archive(engine, archives, temp);
    const Json entry{{"backup_id", backup_id}};
    Json restore = entry;
    restore["target_name"] = restore_name;
    if (mode == "crash-backup")
    {
        archive.create(backup_id, backup_limit, {}, [&] {
            if (staged_file(archives, ".partial-", "manifest.json")) await_kill();
        });
        throw std::runtime_error("Backup publication completed without an interruption");
    }
    if (mode == "crash-restore")
    {
        archive.inspect(restore, "backup_restore", [&] {
            if (staged_file(temp / "restores", ".restore-", "store.sdp")) await_kill();
        });
        throw std::runtime_error("Backup restore completed without an interruption");
    }
    if (mode == "backup-unpublished")
    {
        require(archive.inspect(Json::object(), "backup_catalog", {}).at("backups").empty(),
            "Interrupted backup was advertised as a published archive");
        std::cout << "BACKUP published=0\n";
        return;
    }
    if (mode == "backup-denied")
    {
        bool denied = false;
        try { archive.create(backup_id, backup_limit, {}, {}); }
        catch (const std::filesystem::filesystem_error& error) {
            denied = error.code() == std::errc::permission_denied;
        }
        require(denied && archive.inspect(Json::object(), "backup_catalog", {}).at("backups").empty(),
            "Denied Windows archive directory did not fail closed");
        std::cout << "BACKUP permission_denied published=0\n";
        return;
    }
    if (mode == "crash-backup-after")
    {
        require(archive.create(backup_id, backup_limit, {}, {}).at("verified") == true, "Backup retry did not verify");
        await_kill();
    }
    const auto proof = archive.inspect(entry, "backup_verify", {});
    require(proof.at("verified") == true && proof.at("lsn") == 3, "Published backup lost its source revision");
    if (mode == "restore-denied")
    {
        require_invalid([&] { archive.inspect(restore, "backup_restore", {}); });
        require(!std::filesystem::exists(temp / "restores" / restore_name), "Denied restore left a visible destination");
        std::cout << "RESTORE permission_denied published=0\n";
        return;
    }
    if (mode == "crash-restore-after")
    {
        require(archive.inspect(restore, "backup_restore", {}).at("restored") == true, "Backup restore retry failed");
        await_kill();
    }
    std::cout << "BACKUP published=1 verified=1 history=3\n";
}

void exercise(const std::filesystem::path& db)
{
    disk::Store::initialize(db);
    const auto data = artifact(17);
    sql::ObjectId id{};
    disk::RowId deleted{};
    {
        disk::Engine engine(db);
        const auto before = edit(engine, [&](auto&, auto& catalog, auto&) {
            const auto legacy = Json{{"format", 1}, {"algorithm", "linear"}, {"owner", actor.name},
                {"active", nullptr}, {"versions", Json::array()}}.dump();
            id = catalog.create_auxiliary(sql::ObjectKind::model, name, legacy).id;
        });
        const auto v1 = edit(engine, [&](auto& tx, auto&, auto& history) {
            history.pin({"created", "snapshot", before});
            require(ml::publish_model(tx, owner, id, "dataset", "run1", data, Json::object()).at("version") == 1, "v1");
        });
        edit(engine, [&](auto& tx, auto&, auto& history) {
            history.pin({"publish", "snapshot", v1});
            require(ml::publish_model(tx, owner, id, "dataset", "run2", artifact(22), Json::object()).at("version") == 2, "v2");
        });
        edit(engine, [&](auto&, auto& catalog, auto&) {
            auto model = ml::require_model(catalog, name, actor);
            ml::activate_model(catalog, model, 2);
            require(ml::model_version(model, 0).at("version") == 2, "active v2");
        });
        edit(engine, [&](auto& tx, auto& catalog, auto& history) {
            auto model = ml::require_model(catalog, name, actor);
            require_invalid([&] { ml::delete_model_version(tx, owner, catalog, model, 2, history); });
            bool pinned = false;
            try { ml::delete_model_version(tx, owner, catalog, model, 1, history); }
            catch (const sql::HistoryPinned& pin) { pinned = pin.revision() == v1 && pin.name() == "publish"; }
            require(pinned, "pinned exact version deleted or attributed to an old unrelated revision");
            ml::deactivate_model(catalog, model);
            require(ml::model_status(model).at("active_version").is_null(), "deactivate lost null");
            require_invalid([&] { ml::model_version(model, 0); });
        });
        edit(engine, [&](auto& tx, auto& catalog, auto& history) {
            auto model = ml::require_model(catalog, name, actor);
            deleted = ml::model_version(model, 2).at("artifact_row").template get<disk::RowId>();
            ml::delete_model_version(tx, owner, catalog, model, 2, history);
            require_invalid([&] { ml::model_version(model, 2); });
            require(ml::load_model(tx, owner, model, ml::model_version(model, 1)) == data, "v1 payload changed");
            bool missing = false;
            try { static_cast<void>(tx.get(owner, deleted)); }
            catch (const disk::Error& error) { missing = error.fault() == disk::Fault::missing; }
            require(missing, "deleted artifact row still live");
            require(ml::publish_model(tx, owner, id, "dataset", "run3", artifact(23), Json::object()).at("version") == 3, "v2 was recycled");
        });
    }
    {
        disk::Engine engine(db);
        edit(engine, [&](auto& tx, auto& catalog, auto& history) {
            auto model = ml::require_model(catalog, name, actor);
            const auto status = ml::model_status(model);
            require(status.at("versions").size() == 2 && status.at("versions").at(1).at("version") == 3, "gap lost across restart");
            require_invalid([&] { ml::model_version(model, 2); });
            ml::activate_model(catalog, model, 3);
            ml::activate_model(catalog, model, 1);
            require(ml::model_version(model, 0).at("version") == 1, "older activation selected wrong artifact");
            ml::deactivate_model(catalog, model);
            history.unpin("snapshot", "publish");
            ml::delete_model_version(tx, owner, catalog, model, 1, history);
            ml::delete_model_version(tx, owner, catalog, model, 3, history);
            require(ml::model_status(model).at("versions").empty(), "versions left");
        });
        edit(engine, [&](auto& tx, auto&, auto&) {
            require(ml::publish_model(tx, owner, id, "dataset", "run4", data, Json::object()).at("version") == 4, "empty model reused deleted IDs");
        });
    }
}
}

int main(int count, char** argv)
{
    try
    {
        if (count == 3)
        {
            const std::string_view mode(argv[1]);
            const std::filesystem::path root(argv[2]);
            if (mode == "crash-seed") seed_crash_fixture(root);
            else if (mode == "crash-model-before") interrupt_model_publication(root / "store.sdp", disk::CommitPhase::body_written);
            else if (mode == "crash-model-after") interrupt_model_publication(root / "store.sdp", disk::CommitPhase::wal_durable);
            else if (mode == "model-expect-v1") verify_model(root / "store.sdp", 1);
            else if (mode == "model-expect-v2") verify_model(root / "store.sdp", 2);
            else if (mode == "crash-backup" || mode == "crash-restore" || mode == "backup-unpublished" ||
                mode == "backup-verify" || mode == "backup-denied" || mode == "restore-denied" ||
                mode == "crash-backup-after" || mode == "crash-restore-after") exercise_archive(root, mode);
            else return 2;
            return 0;
        }
        if (count != 2) return 2;
        const std::filesystem::path root(argv[1]);
        std::filesystem::create_directories(root);
        const auto ticket = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto scratch = root / ("lifecycle-" + std::to_string(ticket));
        if (!std::filesystem::create_directory(scratch)) return 2;
        exercise(scratch / "fixture.sdp");
        std::cout << "Legacy compatibility, stable version gaps after restart, snapshot pin visibility, deactivate, rollback, and artifact retirement passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

