#pragma once

#include "relational.hpp"
#include <sdp/wire.hpp>

namespace sdp::sql
{
class DecisionError : public wire::Error
{
public:
    DecisionError(wire::Code code, const char* reason) : wire::Error(code), reason_(reason) {}
    const char* reason() const noexcept { return reason_; }
private:
    const char* reason_;
};

bool decision_ddl(Catalog& catalog, const Statement& statement, bool apply);
std::vector<ObjectId> decision_dependencies(const Object& rule);
void check_decision_drop(std::span<const Object> objects, const Object& dropping);
void require_decision_module(std::span<const Object> objects, ObjectId table, bool enabled);
using DecisionOverride = std::optional<std::pair<ObjectId, uint32_t>>;
ObjectId decision_rule_id(std::span<const Object> objects, ObjectId table, std::string_view name);
std::vector<ObjectId> decision_access_inputs(ObjectId table, std::span<const Object> objects,
    const DecisionOverride& version = {});
std::vector<ObjectId> decision_inputs(Catalog& catalog, const Object& table,
    std::span<const Object> objects, const DecisionOverride& version = {});
void apply_decision(disk::Transaction& tx, disk::Owner owner, std::span<const Object> objects,
    const Object& table, const RowValues* before, RowValues* after, const WorkCheck& check,
    wire::Json* trace = nullptr, const DecisionOverride& version = {});
}
