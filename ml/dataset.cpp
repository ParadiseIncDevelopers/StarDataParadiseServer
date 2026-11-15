#include "dataset.hpp"
#include "../sql/history_journal.hpp"

#include <algorithm>
#include <openssl/rand.h>

namespace sdp::ml
{
namespace
{
using wire::Json;
using W = wire::Code;
constexpr uint32_t schema = 0x53444d31;
constexpr size_t dataset_limit = 12;
constexpr size_t payload_limit = 384 * 1024;

bool valid_id(std::string_view value)
{
    return value.size() == 32 && value.find_first_not_of("0123456789abcdef") == value.npos;
}

[[noreturn]] void denied() { throw wire::Error(W::permission_denied); }

Dataset decode(disk::RowId id, const disk::Row& row)
{
    if (row.values.size() != 4)
        throw disk::Error(disk::Fault::corrupt, "Invalid dataset row");
    const auto* key = std::get_if<std::string>(&row.values[0]);
    const auto* actor = std::get_if<std::string>(&row.values[1]);
    const auto* token = std::get_if<std::string>(&row.values[2]);
    const auto* data = std::get_if<std::string>(&row.values[3]);
    if (!key || !valid_id(*key) || !actor || !token || !valid_id(*token) || !data || data->size() > payload_limit)
        throw disk::Error(disk::Fault::corrupt, "Invalid dataset fields");
    auto content = wire::parse_payload(*data);
    if (content.value("format", 0) != 1)
        throw disk::Error(disk::Fault::corrupt, "Unsupported dataset format");
    return {id, *key, *actor, *token, std::move(content)};
}
}

std::optional<Dataset> dataset_by_request(disk::Transaction& tx, disk::Owner owner, std::string_view actor,
    std::string_view token)
{
    std::optional<Dataset> found;
    tx.visit(owner, [&](disk::RowId id, const disk::Row& row) {
        auto item = decode(id, row);
        if (item.actor != actor || item.token != token) return;
        if (found) throw disk::Error(disk::Fault::corrupt, "Duplicate dataset request");
        found = std::move(item);
    }, schema);
    return found;
}

Dataset require_dataset(disk::Transaction& tx, disk::Owner owner, std::string_view id, const sql::Principal& actor)
{
    if (!valid_id(id)) throw wire::Error(W::invalid_argument);
    std::optional<Dataset> found;
    tx.visit(owner, [&](disk::RowId storage, const disk::Row& row) {
        auto item = decode(storage, row);
        if (item.id != id || (!actor.administrator && item.actor != actor.name)) return;
        if (found) throw disk::Error(disk::Fault::corrupt, "Duplicate dataset identifier");
        found = std::move(item);
    }, schema);
    if (!found) denied();
    return std::move(*found);
}

void insert_dataset(disk::Transaction& tx, disk::Owner owner, Dataset& dataset)
{
    size_t count{};
    tx.visit(owner, [&](disk::RowId, const disk::Row&) { ++count; }, schema);
    if (count >= dataset_limit) throw wire::Error(W::resource_limit);
    auto content = dataset.content.dump();
    if (content.size() > payload_limit) throw wire::Error(W::resource_limit);
    dataset.storage = tx.insert(owner, {schema,
        {dataset.id, dataset.actor, dataset.token, std::move(content)}}, disk::ChangeKind::catalog);
}

void remove_dataset(disk::Transaction& tx, disk::Owner owner, const Dataset& dataset)
{
    tx.erase(owner, dataset.storage, disk::ChangeKind::catalog);
}

Json dataset_authors(std::span<const sql::Requirement> requirements)
{
    auto result = Json::array();
    for (const auto& required : requirements)
        result.push_back({{"verb", required.verb}, {"object", required.target},
            {"schema", required.schema}, {"database", required.database}});
    return result;
}

void require_dataset_read(const Dataset& dataset, const sql::Catalog& catalog,
    const sql::Security& security, const sql::Principal& actor)
{
    const auto objects = catalog.list();
    for (const auto& item : dataset.content.at("requirements"))
    {
        const auto object = item.at("object").get<sql::ObjectId>();
        if (object && std::ranges::find(objects, object, &sql::Object::id) == objects.end()) denied();
        const auto& verb = item.at("verb").get_ref<const std::string&>();
        if (!security.allowed(actor, verb, object, item.at("schema").get<sql::ObjectId>(),
            item.at("database").get<sql::ObjectId>())) denied();
    }
}

Json dataset_summary(const Dataset& dataset, bool include_sample)
{
    const auto& info = dataset.content;
    Json body{{"dataset_id", dataset.id}, {"owner", dataset.actor}, {"revision", info.at("revision")}, {"row_count", info.at("row_count")},
        {"byte_count", info.at("byte_count")}};
    if (include_sample)
    {
        body["source"] = info.at("source");
        body["prepared"] = info.at("prepared");
        body["column_types"] = info.at("column_types");
        body["query"] = info.at("query");
        body["options"] = info.at("options");
    }
    return body;
}

Json model_input_schema(const Dataset& dataset)
{
    const auto& content = dataset.content;
    const auto& names = content.at("source").at("columns");
    const auto& types = content.at("column_types");
    auto schema = Json::object();
    for (const auto& feature : content.at("options").at("features"))
    {
        const auto& name = feature.at("column");
        const auto found = std::ranges::find(names, name);
        if (found == names.end()) throw disk::Error(disk::Fault::corrupt, "Model input schema is inconsistent");
        schema[name.get<std::string>()] = types.at(static_cast<size_t>(found - names.begin()));
    }
    return {{"features", std::move(schema)}, {"target_classes", content.at("prepared").at("target_classes")}};
}

std::string dataset_fingerprint(const Json& input)
{
    auto body = input.dump();
    return sql::history_sha256({reinterpret_cast<const disk::Byte*>(body.data()), body.size()});
}

std::string dataset_id()
{
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw wire::Error(W::internal);
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (auto byte : bytes)
    {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
}
