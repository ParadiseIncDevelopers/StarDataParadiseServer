#pragma once

#include "relational.hpp"
#include "access.hpp"

namespace sdp::sql
{
using TableRowVisitor = std::function<bool(disk::RowId, const RowValues&)>;

struct HistoryRecord
{
    ObjectId table;
    uint32_t version;
    std::span<const disk::Value> values;
};
std::optional<HistoryRecord> history_record(const disk::Row& row);
void visit_history_records(disk::Transaction& transaction, disk::Owner owner, ObjectId table,
    const disk::RowVisitor& visitor, const WorkCheck& check = {});

struct HistoryMutation
{
    disk::RowId id;
    std::optional<RowValues> before, after;
};
RowValues read_history_values(const Object& table, const disk::Row& row);
void apply_history_rows(disk::Transaction& transaction, disk::Owner owner, const Object& table,
    std::span<const HistoryMutation> mutations, const WorkCheck& check, const InvokerCheck& invoker);

RowValues read_table_row(disk::Transaction& transaction, disk::Owner owner, const Object& table, disk::RowId id);
RowValues read_table_row(disk::Store& store, const Object& table, disk::RowId id);
bool scan_table(disk::Transaction& transaction, disk::Owner owner, const Object& table,
    const TableRowVisitor& visitor, const WorkCheck& check = {});
bool scan_table(disk::Store& store, const Object& table, const TableRowVisitor& visitor);
void verify_table_rows(disk::Store& store, std::span<const Object> catalog);

size_t execute_write(disk::Transaction& transaction, disk::Owner owner, const Plan& plan,
    const ParameterValues& parameters = {}, const WorkCheck& check = {}, const InvokerCheck& invoker = {});

struct ResultBatch
{
    std::vector<std::vector<disk::Value>> rows;
    bool done{};
};

class Cursor
{
public:
    Cursor(SelectPlan plan, ParameterValues parameters, size_t row_limit);
    const SelectPlan& plan() const noexcept { return plan_; }
    bool using_index() const noexcept { return index_.has_value(); }
    // Fetch from the same transaction while its snapshot is held by the caller.
    ResultBatch fetch(disk::Transaction& transaction, disk::Owner owner, size_t row_count, size_t byte_limit,
        const WorkCheck& check = {});

private:
    SelectPlan plan_;
    ParameterValues parameters_;
    size_t limit_;
    size_t emitted_{};
    disk::RowId after_{};
    std::optional<IndexAccess> index_;
    std::optional<IndexEntry> index_after_;
    const disk::Transaction* transaction_{};
    disk::Owner owner_{};
    bool done_{};
};
}
