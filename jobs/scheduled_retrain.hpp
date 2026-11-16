#pragma once

#include "../sql/security.hpp"
#include <sdp/wire.hpp>

namespace sdp::jobs
{
// Pin only the recipe and durable identities. Training reads the already-materialized dataset.
wire::Json pin_retrain(disk::Transaction&, disk::Owner, const sql::Security&, const sql::Principal&,
    const wire::Json& request, bool mlp_enabled, const std::function<void()>& check);
wire::Json prepare_retrain(disk::Transaction&, disk::Owner, const sql::Security&, const sql::Principal&,
    const wire::Json& config, bool mlp_enabled, const std::function<void()>& check);
void authorize_retrain(disk::Transaction&, disk::Owner, const sql::Security&, const sql::Principal&,
    const wire::Json& config, bool mlp_enabled, const std::function<void()>& check);
}
