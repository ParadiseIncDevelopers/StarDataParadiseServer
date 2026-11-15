#pragma once

#include "../sql/access.hpp"
#include <sdp/wire.hpp>

namespace sdp::ml
{
struct Dataset
{
    disk::RowId storage{};
    std::string id, actor, token;
    wire::Json content;
};

constexpr size_t dataset_rows = 1024;
std::optional<Dataset> dataset_by_request(disk::Transaction& tx, disk::Owner owner, std::string_view actor,
    std::string_view token);
Dataset require_dataset(disk::Transaction& tx, disk::Owner owner, std::string_view id, const sql::Principal& actor);
void insert_dataset(disk::Transaction& tx, disk::Owner owner, Dataset& dataset);
void remove_dataset(disk::Transaction& tx, disk::Owner owner, const Dataset& dataset);
void require_dataset_read(const Dataset& dataset, const sql::Catalog& catalog,
    const sql::Security& security, const sql::Principal& actor);
wire::Json dataset_summary(const Dataset& dataset, bool include_sample);
wire::Json dataset_authors(std::span<const sql::Requirement> requirements);
wire::Json model_input_schema(const Dataset& dataset);
std::string dataset_fingerprint(const wire::Json& input);
std::string dataset_id();
}
