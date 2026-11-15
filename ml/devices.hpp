#pragma once

#include <sdp/wire.hpp>

namespace sdp::ml
{
wire::Json model_devices(const wire::Json& body, bool cpu_available);
wire::Json choose_device(const wire::Json& body);
}
