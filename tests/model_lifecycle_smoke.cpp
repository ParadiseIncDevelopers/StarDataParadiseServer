#include "model.hpp"
#include "history_journal.hpp"
#include "linear_models.hpp"

#include <chrono>
#include <iostream>

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

