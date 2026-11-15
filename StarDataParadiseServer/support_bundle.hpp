#pragma once

#include "server.hpp"

namespace sdp::server
{
wire::Json support_bundle(const Config& config, std::string_view operation, const wire::Json& body);
}
