#pragma once

#include "executor.hpp"
#include "history_journal.hpp"

namespace sdp::sql
{
class HistoryError : public wire::Error
{
public:
    HistoryError(wire::Code code, const char* reason) : wire::Error(code), reason_(reason) {}
    const char* reason() const noexcept { return reason_; }
private:
    const char* reason_;
};

using HistoricalRows = std::map<disk::RowId, disk::Row>;

class HistoryView
{
public:
    HistoryView(HistoryJournal& journal, disk::Transaction& tx, disk::Owner owner, WorkCheck check);
    const HistoryState& state() const noexcept { return state_; }
    const std::vector<Object>& current_catalog() const noexcept { return current_; }
    std::vector<Object> catalog_at(uint64_t revision) const;
    HistoricalRows rows_at(ObjectId table, uint64_t revision, disk::RowId only = 0) const;
    const std::vector<HistoryRevision>& revisions() const noexcept { return revisions_; }
    const HistoryRevision& at(uint64_t revision) const;
    void require_revision(uint64_t revision) const;
private:
    disk::Transaction& tx_;
    disk::Owner owner_;
    WorkCheck check_;
    HistoryState state_;
    std::vector<Object> current_;
    std::vector<HistoryRevision> revisions_;
};

const Object* history_target(std::span<const Object> catalog, const Name& name);
wire::Json history_status_json(const HistoryState& state, bool show_pins);
wire::Json query_history(HistoryView& view, const HistoryCommand& command,
    const Security& security, const Principal& actor, const WorkCheck& check);
}
