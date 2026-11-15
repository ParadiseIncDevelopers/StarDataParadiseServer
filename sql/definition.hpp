#pragma once

#include "binder.hpp"
#include <sdp/wire.hpp>

namespace sdp::sql
{
wire::Json write_expression(const Expr& expression);
Expr read_expression(const wire::Json& encoded);
struct ViewShape { Object table; std::vector<size_t> columns; };
ViewShape read_view(Catalog& catalog, const Object& view);
bool auxiliary_ddl(Catalog& catalog, const Statement& statement, bool apply);
void analyze_table(disk::Transaction& transaction, disk::Owner owner, Catalog& catalog, const Name& name,
    const std::function<void()>& check);
bool has_view_dependency(Catalog& catalog, ObjectId table);
std::vector<ObjectId> object_dependencies(const Object& object);
int64_t advance_sequence(Catalog& catalog, const Name& sequence);
int64_t advance_sequence(Catalog& catalog, ObjectId sequence);
}
