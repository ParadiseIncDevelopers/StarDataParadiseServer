#pragma once

#include "decision.hpp"
#include "security.hpp"

namespace sdp::sql
{
wire::Json inspect_decision(disk::Engine& engine, disk::Transaction& tx, disk::Owner owner,
    const Security& security, const Principal& actor, std::string_view operation,
    const wire::Json& input, const WorkCheck& check);
}
