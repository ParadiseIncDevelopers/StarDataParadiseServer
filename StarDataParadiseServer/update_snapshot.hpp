#pragma once

#include "server.hpp"

namespace sdp::server
{
wire::Json update_snapshot(const Config& config, const std::filesystem::path& destination = {}, bool readiness = false);
}
