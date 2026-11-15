#pragma once

#include "dataset.hpp"

namespace sdp::sql { class HistoryJournal; }

namespace sdp::ml
{
struct Model
{
    sql::Object object;
    wire::Json metadata;
};

Model require_model(sql::Catalog& catalog, const sql::Name& name, const sql::Principal& actor);
void create_model(sql::Catalog& catalog, const sql::Name& name, std::string_view algorithm, std::string_view actor);
wire::Json model_status(const Model& model);
wire::Json model_version(const Model& model, uint32_t requested);
wire::Json load_model(disk::Transaction& tx, disk::Owner owner, const Model& model, const wire::Json& version);
void activate_model(sql::Catalog& catalog, Model& model, uint32_t version);
void deactivate_model(sql::Catalog& catalog, Model& model);
void delete_model_version(disk::Transaction& tx, disk::Owner owner, sql::Catalog& catalog,
    Model& model, uint32_t version, sql::HistoryJournal& history);
wire::Json publish_model(disk::Transaction& tx, disk::Owner owner, sql::ObjectId model,
    std::string_view dataset, std::string_view run, const wire::Json& output, const wire::Json& options);
bool model_references_dataset(sql::Catalog& catalog, std::string_view dataset);
}
