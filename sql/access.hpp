#pragma once
#include "security.hpp"

namespace sdp::sql
{
struct Requirement
{
    std::string_view verb;
    ObjectId target{}, schema{}, database{};
};
Requirement object_access(std::span<const Object> snapshot, const Object& object, std::string_view verb);
std::vector<Requirement> access_requirements(Catalog& catalog, const Statement& statement);
using InvokerCheck = std::function<void(std::span<const Requirement>)>;
void enforce(const Security& security, const Principal& actor, std::span<const Requirement> requirements);
}
