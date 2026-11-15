#pragma once

#include "binder.hpp"
#include <sdp/wire.hpp>

namespace sdp::sql
{
uint32_t unsigned_field(const wire::Json& object, const char* key, uint32_t maximum, uint32_t fallback = 0);
ParameterTypes read_parameter_types(const wire::Json& object);
ParameterValues read_parameters(const wire::Json& object, const ParameterTypes& types);
wire::Json write_value(const disk::Value& value, Type type);
}
