#include "model.hpp"
#include "training.hpp"
#include "preprocess.hpp"
#include "../sql/history_journal.hpp"

#include <algorithm>
#include <limits>

namespace sdp::ml
{
namespace
{
using wire::Json;
using W = wire::Code;
constexpr auto artifact_schema = 0x53444d32u;
constexpr size_t maximum_versions = 16;

[[noreturn]] void damaged()
{
    throw disk::Error(disk::Fault::corrupt, "Invalid persisted model metadata");
}

uint32_t version_number(const Json& value)
{
    if (!value.is_number_unsigned() || value.get<uint64_t>() > (std::numeric_limits<uint32_t>::max)())
        damaged();
    return value.get<uint32_t>();
}

Json::const_iterator find_version(const Json& versions, uint32_t version)
{
    return std::ranges::find_if(versions, [version](const auto& entry) {
        return version_number(entry.at("version")) == version;
    });
}

Model decode(sql::Object object)
{
    try
    {
        auto info = Json::parse(object.definition, nullptr, false);
        if (object.kind != sql::ObjectKind::model || !info.is_object() || info.value("format", 0) != 1 ||
            !model_algorithm(info.at("algorithm").get_ref<const std::string&>()) ||
            !info.at("owner").is_string() || !info.at("versions").is_array() ||
            info.at("versions").size() > maximum_versions) damaged();

        const auto& versions = info.at("versions");
        const auto legacy = !info.contains("last_version");
        uint32_t previous = 0;
        for (const auto& entry : versions)
        {
            const auto version = version_number(entry.at("version"));
            if (version <= previous || (legacy && version != previous + 1) ||
                !entry.at("artifact_row").is_number_unsigned() || !entry.at("artifact_row").get<disk::RowId>() ||
                !entry.at("checksum").is_string() || !entry.at("dataset_id").is_string() ||
                !entry.at("run_id").is_string()) damaged();
            previous = version;
        }
        if (legacy) info["last_version"] = previous;
        else if (version_number(info.at("last_version")) < previous) damaged();

        const auto& active = info.at("active");
        if (!active.is_null() && find_version(versions, version_number(active)) == versions.end()) damaged();
        return {std::move(object), std::move(info)};
    }
    catch (const Json::exception&) { damaged(); }
}

void require_unpinned(sql::HistoryJournal& journal, disk::RowId artifact_row, const disk::Bytes& artifact)
{
    uint64_t published{};
    const auto state = journal.visit(0, 0, [&](const sql::HistoryRevision& revision) {
        for (const auto& change : revision.changes)
        {
            if (change.id != artifact_row || change.after != artifact) continue;
            if (!change.before.empty() || change.kind != disk::ChangeKind::catalog || published)
                damaged();
            published = revision.lsn;
        }
    });
    // If publication predates retention, every remaining pin can see the version.
    for (const auto& pin : state.pins)
        if (!published || pin.revision >= published) throw sql::HistoryPinned(pin);
}

}

Model require_model(sql::Catalog& catalog, const sql::Name& name, const sql::Principal& actor)
{
    try
    {
        auto model = decode(catalog.resolve(sql::ObjectKind::model, name));
        if (!actor.administrator && model.metadata.at("owner") != actor.name)
            throw wire::Error(W::permission_denied);
        return model;
    }
    catch (const sql::Error& error)
    {
        if (error.code() == sql::Code::name && !actor.administrator)
            throw wire::Error(W::permission_denied);
        throw;
    }
}

void create_model(sql::Catalog& catalog, const sql::Name& name, std::string_view algorithm, std::string_view actor)
{
    if (!model_algorithm(algorithm)) throw wire::Error(W::unsupported);
    const auto definition = Json{{"format", 1}, {"algorithm", algorithm}, {"owner", actor},
        {"active", nullptr}, {"last_version", uint32_t{}}, {"versions", Json::array()}}.dump();
    catalog.create_auxiliary(sql::ObjectKind::model, name, definition);
}

Json model_status(const Model& model)
{
    const auto& meta = model.metadata;
    auto versions = Json::array();
    for (const auto& entry : meta.at("versions"))
        versions.push_back({{"version", entry.at("version")}, {"checksum", entry.at("checksum")},
            {"dataset_id", entry.at("dataset_id")}, {"run_id", entry.at("run_id")}});
    return {{"name", model.object.name}, {"object_id", std::to_string(model.object.id)},
        {"algorithm", meta.at("algorithm")}, {"owner", meta.at("owner")},
        {"active_version", meta.at("active")}, {"versions", std::move(versions)}};
}

Json model_version(const Model& model, uint32_t requested)
{
    const auto& meta = model.metadata;
    if (!requested)
    {
        if (meta.at("active").is_null()) throw wire::Error(W::invalid_state);
        requested = version_number(meta.at("active"));
    }
    const auto& versions = meta.at("versions");
    const auto found = find_version(versions, requested);
    if (found == versions.end()) throw wire::Error(W::invalid_state);
    return *found;
}

Json load_model(disk::Transaction& tx, disk::Owner owner, const Model& model, const Json& version)
{
    const auto row_id = version.at("artifact_row").get<disk::RowId>();
    const auto row = tx.get(owner, row_id);
    if (row.schema != artifact_schema || row.values.size() != 3) damaged();
    const auto* id = std::get_if<int64_t>(&row.values[0]);
    const auto* number = std::get_if<int64_t>(&row.values[1]);
    const auto* raw = std::get_if<std::string>(&row.values[2]);
    if (!id || *id != model.object.id || !number || *number != version.at("version").get<int64_t>() || !raw) damaged();
    const auto output = Json::parse(*raw, nullptr, false);
    if (!valid_training_result(output, model.metadata.at("algorithm").get<std::string>()) ||
        dataset_fingerprint(output) != version.at("checksum").get<std::string>()) damaged();
    return output;
}

void activate_model(sql::Catalog& catalog, Model& model, uint32_t version)
{
    if (!version) throw wire::Error(W::invalid_state);
    model_version(model, version);
    auto next = model.metadata;
    next["active"] = version;
    catalog.update_definition(model.object, next.dump(), true);
    model.metadata = std::move(next);
}

void deactivate_model(sql::Catalog& catalog, Model& model)
{
    if (model.metadata.at("active").is_null()) return;
    auto next = model.metadata;
    next["active"] = nullptr;
    catalog.update_definition(model.object, next.dump(), true);
    model.metadata = std::move(next);
}

void delete_model_version(disk::Transaction& tx, disk::Owner owner, sql::Catalog& catalog,
    Model& model, uint32_t version, sql::HistoryJournal& history)
{
    if (!version) throw wire::Error(W::invalid_state);
    const auto& active = model.metadata.at("active");
    if (!active.is_null() && version_number(active) == version) throw wire::Error(W::invalid_state);
    const auto entry = model_version(model, version);
    load_model(tx, owner, model, entry);
    const auto row_id = entry.at("artifact_row").get<disk::RowId>();
    require_unpinned(history, row_id, disk::encode(tx.get(owner, row_id)));

    auto next = model.metadata;
    auto& versions = next.at("versions");
    versions.erase(find_version(versions, version));
    catalog.update_definition(model.object, next.dump(), true);
    tx.erase(owner, row_id, disk::ChangeKind::catalog);
    model.metadata = std::move(next);
}

Json publish_model(disk::Transaction& tx, disk::Owner owner, sql::ObjectId id,
    std::string_view dataset, std::string_view run, const Json& output, const Json& options)
{
    sql::Catalog catalog(tx, owner);
    const auto objects = catalog.list();
    const auto found = std::ranges::find(objects, id, &sql::Object::id);
    if (found == objects.end() || found->kind != sql::ObjectKind::model) damaged();
    auto model = decode(*found);
    const auto& algorithm = model.metadata.at("algorithm").get_ref<const std::string&>();
    if (!valid_training_result(output, algorithm)) damaged();
    const auto width = output.at("width").get<size_t>();
    if (!check_model_artifact(algorithm, output.at("artifact"), width)) damaged();
    auto& versions = model.metadata.at("versions");
    const auto last = version_number(model.metadata.at("last_version"));
    if (versions.size() >= maximum_versions || last == (std::numeric_limits<uint32_t>::max)())
        throw wire::Error(W::resource_limit);
    const auto version = last + 1;
    model.metadata["last_version"] = version;
    const auto raw = output.dump();
    if (raw.size() > 65'536) throw wire::Error(W::resource_limit);
    const auto checksum = dataset_fingerprint(output);
    const auto artifact = tx.insert(owner, {artifact_schema, {id, static_cast<int64_t>(version), raw}},
        disk::ChangeKind::catalog);
    versions.push_back({{"version", version}, {"artifact_row", artifact}, {"checksum", checksum}, {"dataset_id", dataset},
        {"run_id", run}, {"seed", output.at("seed")}, {"options", options},
        {"runtime", algorithm == "mlp" ? "sdp-mlp-cpu-v1" :
            supervised_algorithm(algorithm) ? "sdp-supervised-v1" : "sdp-unsupervised-v1"}});
    catalog.update_definition(model.object, model.metadata.dump(), true);
    return {{"version", version}, {"checksum", checksum}, {"algorithm", algorithm}, {"dataset_id", dataset}};
}

bool model_references_dataset(sql::Catalog& catalog, std::string_view dataset)
{
    for (const auto& object : catalog.list())
    {
        if (object.kind != sql::ObjectKind::model) continue;
        const auto model = decode(object);
        for (const auto& version : model.metadata.at("versions"))
            if (version.at("dataset_id").get_ref<const std::string&>() == dataset) return true;
    }
    return false;
}
}
