#pragma once

#include "history_query.hpp"

namespace sdp::sql
{
struct HistoryPlan
{
    enum class CatalogEdit { none, restore_table, drop_table, restore_routine, drop_routine };

    std::string id;
    HistoryCommand::Kind mode;
    uint64_t head, revision;
    Object object;
    CatalogEdit catalog_edit{};
    std::vector<Object> indexes;
    std::vector<HistoryMutation> rows;
    std::vector<Requirement> required;
    wire::Json preview;
};

HistoryPlan prepare_history_plan(HistoryView& view, const HistoryCommand& command,
    const Security& security, const Principal& actor, const WorkCheck& check);
void apply_history_plan(disk::Transaction& tx, disk::Owner owner, const HistoryPlan& plan,
    const Security& security, const Principal& actor, const WorkCheck& check);
}
