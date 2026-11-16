#pragma once

#include "access.hpp"
#include "executor.hpp"

namespace sdp::pl { struct Instrumentation; }

namespace sdp::sql
{
struct RoutineSelection;
struct NamedSet
{
    std::string name;
    std::vector<Projection> columns;
    std::vector<RowValues> rows;
};
struct RoutineResults
{
    std::vector<NamedSet> sets;
    size_t affected{};
};

bool routine_ddl(Catalog& catalog, const Statement& statement, bool apply);
void restore_history_routine(Catalog& catalog, const Object& historical);
bool selects_routine(const Statement& statement);
std::shared_ptr<const RoutineSelection> plan_routine(Catalog& catalog, const Statement& statement,
    const ParameterTypes& parameters);
const std::vector<Projection>& routine_columns(const RoutineSelection& plan);
RoutineResults run_routine(const RoutineSelection& plan, disk::Transaction& transaction,
    disk::Owner owner, const ParameterValues& parameters, const InvokerCheck& invoker, const WorkCheck& check,
    const pl::Instrumentation* instrumentation = nullptr);
bool routine_writes(const RoutineSelection& plan);
wire::Json routine_export_snapshot(Catalog& catalog, const Name& name, bool standalone,
    const std::function<void(const Object&, std::string_view)>& check_access, const WorkCheck& check);
std::vector<ObjectId> routine_dependencies(const Object& object);
size_t verify_routines(Catalog& catalog);
bool has_trigger_dependency(Catalog& catalog, ObjectId table);
void fire_triggers(disk::Transaction& transaction, disk::Owner owner, const Object& table,
    std::string_view timing, std::string_view event, const RowValues* before, const RowValues* after,
    const WorkCheck& check, const InvokerCheck& invoker);
}
