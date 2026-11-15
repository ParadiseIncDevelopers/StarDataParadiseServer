#pragma once

#include "binder.hpp"
#include "btree.hpp"

namespace sdp::sql
{
using WorkCheck = std::function<void()>;
using RowValues = std::vector<disk::Value>;

struct IndexDefinition
{
    Object object;
    ObjectId table;
    disk::RowId root;
    std::vector<size_t> columns;
    bool unique;
    std::string collation;
    std::string sort_version;
};

struct IndexCollation
{
    std::string name;
    std::string version;
    bool stale;
};

std::optional<IndexCollation> catalog_index_collation(const Object& object);

struct IndexAccess
{
    IndexDefinition index;
    std::optional<IndexKey> lower;
    std::optional<IndexKey> upper;
};

bool relational_ddl(Catalog& catalog, const Statement& statement);
std::vector<ObjectId> relational_dependencies(const Object& object);
bool execute_index_ddl(disk::Transaction& transaction, disk::Owner owner, Catalog& catalog,
    const Statement& statement, const WorkCheck& check);
void install_table_rules(Catalog& catalog, const Object& table, const Statement& statement);
void restore_history_table(disk::Transaction& transaction, disk::Owner owner, const Object& historical,
    std::span<const Object> historical_indexes, const WorkCheck& check);
std::optional<IndexAccess> choose_index(Catalog& catalog, const SelectPlan& plan, const ParameterValues& parameters);
std::vector<Type> index_types(const Object& table, const IndexDefinition& index);
void verify_relational(disk::Store& store);

class TableMutation
{
public:
    TableMutation(disk::Transaction& transaction, disk::Owner owner, const Object& table, WorkCheck check);
    void fill_defaults(RowValues& values, std::span<const size_t> assigned);
    void verify_history_keys(disk::RowId id, const RowValues* before, const RowValues& after);
    void inspect(disk::RowId id, const RowValues* before, const RowValues* after);
    void index(disk::RowId id, const RowValues* before, const RowValues* after);
    void drop_indexes();
    const std::vector<Object>& catalog_snapshot() const noexcept { return snapshot_; }

private:
    disk::Transaction& transaction_;
    disk::Owner owner_;
    Object table_;
    WorkCheck check_;
    Catalog catalog_;
    std::vector<Object> snapshot_;
    std::vector<IndexDefinition> indexes_;
};
}
